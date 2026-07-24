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
    for (Slot& slot : _slots)
    {
        slot.arena = device->newBuffer(kInitialArenaSize, MTL::ResourceStorageModeShared);
        if (!slot.arena)
        {
            ReleaseArenas();
            return false;
        }
    }
    return true;
}

void FrameRing::ReleaseArenas()
{
    for (Slot& slot : _slots)
    {
        if (slot.arena)
        {
            slot.arena->release();
            slot.arena = nullptr;
        }
        slot.cursor = 0;
    }
}

MTL::CommandBuffer* FrameRing::BeginSpan(MTL::CommandQueue* queue)
{
    if (!queue || !_available || _active)
        return nullptr;

    dispatch_semaphore_wait(_available, DISPATCH_TIME_FOREVER);
    _slotIndex = (_slotIndex + 1) % kMaxFramesInFlight;
    _slots[_slotIndex].cursor = 0;

    // M0 has no streamed bindings yet. The generation is the explicit
    // invalidation contract M1's binding cache and dirty-all state consume.
    ++_streamingGeneration;
    _active = true;

    MTL::CommandBuffer* commandBuffer = queue->commandBuffer();
    if (!commandBuffer)
        AbortSpan();
    return commandBuffer;
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
