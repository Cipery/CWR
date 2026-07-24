#pragma once

#include <PoseidonMetal/MetalFwd.hpp>

#include <dispatch/dispatch.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

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
    MTL::CommandBuffer* CreateContinuation(MTL::CommandQueue* queue);
    struct Allocation
    {
        MTL::Buffer* buffer = nullptr;
        std::size_t offset = 0;
        void* contents = nullptr;

        explicit operator bool() const { return buffer != nullptr && contents != nullptr; }
    };
    Allocation Allocate(std::size_t size, std::size_t alignment = 256);
    void EndSpan(MTL::CommandBuffer* commandBuffer);
    void AbortSpan();
    void WaitIdle();

    std::uint64_t StreamingGeneration() const { return _streamingGeneration; }

  private:
    struct Block
    {
        MTL::Buffer* buffer = nullptr; // Owned.
        std::size_t capacity = 0;
        std::size_t cursor = 0;
    };
    struct Slot
    {
        std::vector<Block> blocks;
    };

    void ReleaseArenas();

    std::array<Slot, kMaxFramesInFlight> _slots;
    MTL::Device* _device = nullptr; // Borrowed.
    dispatch_semaphore_t _available = nullptr;
    std::size_t _slotIndex = kMaxFramesInFlight - 1;
    std::uint64_t _streamingGeneration = 0;
    bool _active = false;
};
