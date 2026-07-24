#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/FrameRing.hpp>

FrameRing::FrameRing() : _available(dispatch_semaphore_create(kMaxFramesInFlight)) {}

FrameRing::~FrameRing()
{
    Shutdown();
#if !OS_OBJECT_USE_OBJC
    if (_available)
        dispatch_release(_available);
#endif
    _available = nullptr;
}

void FrameRing::Shutdown()
{
    if (_active)
        AbortSpan();
    WaitIdle();
    ReleaseArenas();
}

bool FrameRing::Initialize(MTL::Device* device)
{
    if (!device || !_available)
        return false;

    ReleaseArenas();
    _device = device;
    for (Slot& slot : _slots)
    {
        Block block;
        block.buffer = device->newBuffer(kInitialArenaSize, MTL::ResourceStorageModeShared);
        block.capacity = kInitialArenaSize;
        if (!block.buffer)
        {
            ReleaseArenas();
            return false;
        }
        slot.blocks.push_back(block);
    }
    return true;
}

void FrameRing::ReleaseArenas()
{
    for (Slot& slot : _slots)
    {
        for (Block& block : slot.blocks)
        {
            if (block.buffer)
                block.buffer->release();
        }
        slot.blocks.clear();
    }
    _device = nullptr;
}

MTL::CommandBuffer* FrameRing::BeginSpan(MTL::CommandQueue* queue)
{
    if (!queue || !_available || _active)
        return nullptr;

    dispatch_semaphore_wait(_available, DISPATCH_TIME_FOREVER);
    _slotIndex = (_slotIndex + 1) % kMaxFramesInFlight;
    for (Block& block : _slots[_slotIndex].blocks)
        block.cursor = 0;

    // Every slot rotation invalidates streamed {buffer,offset} bindings.
    ++_streamingGeneration;
    _active = true;

    MTL::CommandBuffer* commandBuffer = queue->commandBuffer();
    if (!commandBuffer)
        AbortSpan();
    return commandBuffer;
}

MTL::CommandBuffer* FrameRing::CreateContinuation(MTL::CommandQueue* queue)
{
    // A continuation shares the active slot and its existing allocation
    // cursor. It deliberately does not rotate/reset the slot or move the
    // semaphore signal away from EndSpan's final command buffer.
    if (!queue || !_active)
        return nullptr;
    return queue->commandBuffer();
}

FrameRing::Allocation FrameRing::Allocate(std::size_t size, std::size_t alignment)
{
    if (!_active || !_device || size == 0)
        return {};
    if (alignment == 0)
        alignment = 1;

    Slot& slot = _slots[_slotIndex];
    for (Block& block : slot.blocks)
    {
        const std::size_t offset = (block.cursor + alignment - 1) & ~(alignment - 1);
        if (offset + size > block.capacity)
            continue;
        block.cursor = offset + size;
        return {block.buffer, offset, static_cast<char*>(block.buffer->contents()) + offset};
    }

    std::size_t capacity = kInitialArenaSize;
    if (!slot.blocks.empty())
        capacity = slot.blocks.back().capacity * 2;
    while (capacity < size)
        capacity *= 2;

    Block block;
    block.buffer = _device->newBuffer(capacity, MTL::ResourceStorageModeShared);
    if (!block.buffer)
        return {};
    block.capacity = capacity;
    block.cursor = size;
    slot.blocks.push_back(block);
    return {block.buffer, 0, block.buffer->contents()};
}

void FrameRing::EndSpan(MTL::CommandBuffer* commandBuffer)
{
    if (!_active)
        return;

    _active = false;
    if (!commandBuffer)
    {
        dispatch_semaphore_signal(_available);
        return;
    }

    dispatch_semaphore_t available = _available;
    commandBuffer->addCompletedHandler([available](MTL::CommandBuffer*) { dispatch_semaphore_signal(available); });
    commandBuffer->commit();
}

void FrameRing::AbortSpan()
{
    if (!_active)
        return;
    _active = false;
    dispatch_semaphore_signal(_available);
}

void FrameRing::WaitIdle()
{
    if (!_available || _active)
        return;

    for (std::size_t i = 0; i < kMaxFramesInFlight; ++i)
        dispatch_semaphore_wait(_available, DISPATCH_TIME_FOREVER);
    for (std::size_t i = 0; i < kMaxFramesInFlight; ++i)
        dispatch_semaphore_signal(_available);
}
