#pragma once

#include <PoseidonMetal/MetalFwd.hpp>

#include <dispatch/dispatch.h>

#include <array>
#include <cstddef>

namespace Poseidon::Metal
{
/// Triple-buffered shared-storage vertex/index arena for the dev-overlay pass.
/// This is separate from FrameRing because the overlay encodes in NextFrame,
/// outside FrameRing's InitDraw..FinishDraw ownership span.
class OverlayBufferRing
{
  public:
    static constexpr std::size_t kSlots = 3;

    struct Slot
    {
        MTL::Buffer* vertices = nullptr;
        MTL::Buffer* indices = nullptr;
    };

    OverlayBufferRing();
    ~OverlayBufferRing();

    OverlayBufferRing(const OverlayBufferRing&) = delete;
    OverlayBufferRing& operator=(const OverlayBufferRing&) = delete;

    bool Initialize(MTL::Device* device);
    void Shutdown();

    class Lease
    {
      public:
        Lease() = default;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease() { Cancel(); }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        explicit operator bool() const { return _owner != nullptr; }
        const Slot& Get() const;

        /// Transfers responsibility for returning the permit to the command
        /// buffer's completion handler.
        void ReleaseOnCompletion(MTL::CommandBuffer* commandBuffer);

        /// Cancels an uncommitted acquisition, returning its permit and
        /// restoring the slot rotation.
        void Cancel();

      private:
        friend class OverlayBufferRing;
        Lease(OverlayBufferRing* owner, std::size_t slotIndex, std::size_t previousSlotIndex);

        OverlayBufferRing* _owner = nullptr;
        std::size_t _slotIndex = 0;
        std::size_t _previousSlotIndex = 0;
    };

    /// Waits for a free slot and grows its buffers as needed. Unavailable or
    /// re-entrant acquisition returns empty before consuming a permit;
    /// allocation failure restores the consumed permit and slot rotation.
    Lease Acquire(std::size_t vertexBytes, std::size_t indexBytes);

    /// Waits until every command buffer using a slot has completed.
    void WaitIdle();

  private:
    struct ArenaSlot
    {
        Slot buffers;
        std::size_t vertexCapacity = 0;
        std::size_t indexCapacity = 0;
    };

    static constexpr std::size_t kInitialVertexBytes = 64 * 1024;
    static constexpr std::size_t kInitialIndexBytes = 16 * 1024;

    bool GrowBuffer(MTL::Buffer*& buffer, std::size_t& capacity, std::size_t required,
                    std::size_t initialCapacity);
    void ReleaseBuffers();

    std::array<ArenaSlot, kSlots> _slots;
    MTL::Device* _device = nullptr; // Borrowed.
    dispatch_semaphore_t _available = nullptr;
    std::size_t _slotIndex = kSlots - 1;
    bool _leaseActive = false;
};
} // namespace Poseidon::Metal
