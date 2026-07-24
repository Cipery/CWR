#pragma once

#include <PoseidonMetal/MetalFwd.hpp>

#include <dispatch/dispatch.h>

#include <array>
#include <cstddef>
#include <cstdint>

class FrameRing
{
  public:
    static constexpr std::size_t kMaxFramesInFlight = 3;
    static constexpr std::size_t kInitialArenaSize = 8 * 1024 * 1024;

    FrameRing();
    ~FrameRing();

    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    bool Initialize(MTL::Device* device);
    void Shutdown();
    MTL::CommandBuffer* BeginSpan(MTL::CommandQueue* queue);
    void EndSpan(MTL::CommandBuffer* commandBuffer);
    void AbortSpan();
    void WaitIdle();

    std::uint64_t StreamingGeneration() const { return _streamingGeneration; }

  private:
    struct Slot
    {
        MTL::Buffer* arena = nullptr; // Owned.
        std::size_t cursor = 0;
    };

    void ReleaseArenas();

    std::array<Slot, kMaxFramesInFlight> _slots;
    dispatch_semaphore_t _available = nullptr;
    std::size_t _slotIndex = kMaxFramesInFlight - 1;
    std::uint64_t _streamingGeneration = 0;
    bool _active = false;
};
