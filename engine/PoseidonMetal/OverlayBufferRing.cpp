#include <PoseidonMetal/Private/MetalCppFirst.hpp>
#include <imgui.h>

#ifdef DebugLog
#undef DebugLog
#endif

#include <PoseidonMetal/OverlayBufferRing.hpp>

#include <cassert>
#include <limits>
#include <utility>

namespace Poseidon::Metal
{
OverlayBufferRing::OverlayBufferRing() : _available(dispatch_semaphore_create(kSlots)) {}

OverlayBufferRing::~OverlayBufferRing()
{
    Shutdown();
#if !OS_OBJECT_USE_OBJC
    if (_available)
        dispatch_release(_available);
#endif
    _available = nullptr;
}

OverlayBufferRing::Lease::Lease(OverlayBufferRing* owner, std::size_t slotIndex,
                                std::size_t previousSlotIndex)
    : _owner(owner), _slotIndex(slotIndex), _previousSlotIndex(previousSlotIndex)
{
}

OverlayBufferRing::Lease::Lease(Lease&& other) noexcept
    : _owner(std::exchange(other._owner, nullptr)), _slotIndex(other._slotIndex),
      _previousSlotIndex(other._previousSlotIndex)
{
}

OverlayBufferRing::Lease& OverlayBufferRing::Lease::operator=(Lease&& other) noexcept
{
    if (this == &other)
        return *this;
    Cancel();
    _owner = std::exchange(other._owner, nullptr);
    _slotIndex = other._slotIndex;
    _previousSlotIndex = other._previousSlotIndex;
    return *this;
}

const OverlayBufferRing::Slot& OverlayBufferRing::Lease::Get() const
{
    assert(_owner && "cannot access an empty or moved-from overlay buffer lease");
    return _owner->_slots[_slotIndex].buffers;
}

void OverlayBufferRing::Lease::ReleaseOnCompletion(MTL::CommandBuffer* commandBuffer)
{
    if (!_owner)
        return;
    if (!commandBuffer)
    {
        Cancel();
        return;
    }

    dispatch_semaphore_t available = _owner->_available;
    commandBuffer->addCompletedHandler(
        [available](MTL::CommandBuffer*) { dispatch_semaphore_signal(available); });
    _owner->_leaseActive = false;
    _owner = nullptr;
}

void OverlayBufferRing::Lease::Cancel()
{
    if (!_owner)
        return;

    // Acquire is synchronous and only one not-yet-submitted lease may exist.
    // Restoring the exact predecessor makes a cancelled acquisition consume
    // neither a permit nor a slot rotation.
    _owner->_slotIndex = _previousSlotIndex;
    _owner->_leaseActive = false;
    dispatch_semaphore_signal(_owner->_available);
    _owner = nullptr;
}

bool OverlayBufferRing::GrowBuffer(MTL::Buffer*& buffer, std::size_t& capacity,
                                   std::size_t required, std::size_t initialCapacity)
{
    if (required <= capacity && buffer && buffer->contents())
        return true;

    std::size_t newCapacity = capacity ? capacity : initialCapacity;
    while (newCapacity < required)
    {
        if (newCapacity > std::numeric_limits<std::size_t>::max() / 2)
            return false;
        newCapacity *= 2;
    }

    MTL::Buffer* replacement = _device->newBuffer(newCapacity, MTL::ResourceStorageModeShared);
    if (!replacement)
        return false;
    if (!replacement->contents())
    {
        replacement->release();
        return false;
    }

    if (buffer)
        buffer->release();
    buffer = replacement;
    capacity = newCapacity;
    return true;
}

bool OverlayBufferRing::Initialize(MTL::Device* device)
{
    if (!device || !_available)
        return false;

    Shutdown();
    _device = device;
    for (ArenaSlot& slot : _slots)
    {
        if (!GrowBuffer(slot.buffers.vertices, slot.vertexCapacity, kInitialVertexBytes,
                        kInitialVertexBytes) ||
            !GrowBuffer(slot.buffers.indices, slot.indexCapacity, kInitialIndexBytes,
                        kInitialIndexBytes))
        {
            ReleaseBuffers();
            return false;
        }
    }
    _slotIndex = kSlots - 1;
    return true;
}

OverlayBufferRing::Lease OverlayBufferRing::Acquire(std::size_t vertexBytes,
                                                    std::size_t indexBytes)
{
    if (!_device || !_available)
        return {};
    if (_leaseActive)
        return {};

    dispatch_semaphore_wait(_available, DISPATCH_TIME_FOREVER);
    const std::size_t previousSlotIndex = _slotIndex;
    _slotIndex = (_slotIndex + 1) % kSlots;
    _leaseActive = true;
    Lease lease(this, _slotIndex, previousSlotIndex);

    ArenaSlot& slot = _slots[_slotIndex];
    if (!GrowBuffer(slot.buffers.vertices, slot.vertexCapacity, vertexBytes,
                    kInitialVertexBytes) ||
        !GrowBuffer(slot.buffers.indices, slot.indexCapacity, indexBytes,
                    kInitialIndexBytes))
    {
        lease.Cancel();
        return {};
    }
    return lease;
}

void OverlayBufferRing::WaitIdle()
{
    if (!_available)
        return;

    for (std::size_t i = 0; i < kSlots; ++i)
        dispatch_semaphore_wait(_available, DISPATCH_TIME_FOREVER);
    for (std::size_t i = 0; i < kSlots; ++i)
        dispatch_semaphore_signal(_available);
}

void OverlayBufferRing::ReleaseBuffers()
{
    for (ArenaSlot& slot : _slots)
    {
        if (slot.buffers.vertices)
            slot.buffers.vertices->release();
        if (slot.buffers.indices)
            slot.buffers.indices->release();
        slot = {};
    }
    _device = nullptr;
}

void OverlayBufferRing::Shutdown()
{
    WaitIdle();
    ReleaseBuffers();
    _slotIndex = kSlots - 1;
    _leaseActive = false;
}
} // namespace Poseidon::Metal
