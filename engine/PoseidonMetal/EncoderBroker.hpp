#pragma once

#include <PoseidonMetal/MetalFwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Poseidon::Metal
{
enum class TargetKind : std::uint8_t
{
    Frame,
    Cascade,
};

struct TargetId
{
    MTL::Texture* texture = nullptr;
    std::uint32_t layer = 0;
    TargetKind kind = TargetKind::Frame;

    bool operator==(const TargetId& other) const
    {
        return texture == other.texture && layer == other.layer && kind == other.kind;
    }
};

class EncoderBroker
{
  public:
    using ReplayCallback = void (*)(void*, MTL::RenderCommandEncoder*);

    struct FrameTarget
    {
        MTL::Texture* color = nullptr;
        MTL::Texture* resolve = nullptr;
        MTL::Texture* depthStencil = nullptr;
        bool clearColor = false;
        bool clearDepthStencil = false;
        double clearRed = 0.0;
        double clearGreen = 0.0;
        double clearBlue = 0.0;
        double clearAlpha = 1.0;
    };

    void BeginFrame(MTL::CommandBuffer* commandBuffer);
    void SetCommandBuffer(MTL::CommandBuffer* commandBuffer);
    MTL::RenderCommandEncoder* EnsureFrame(const FrameTarget& target, ReplayCallback replay, void* replayContext);
    MTL::RenderCommandEncoder* EnsureCascade(MTL::Texture* depthArray, std::uint32_t layer);
    void EndCurrent(bool terminalFrame = false, bool resolveFrameForReadback = false);
    void Reset();

    bool HasOpenedFrameTarget(MTL::Texture* color) const;
    MTL::RenderCommandEncoder* Current() const { return _encoder; }
    const TargetId& CurrentTarget() const { return _current; }
    bool HasCurrent() const { return _encoder != nullptr; }

  private:
    bool WasOpened(const TargetId& target) const;
    void MarkOpened(const TargetId& target);

    MTL::CommandBuffer* _commandBuffer = nullptr;
    MTL::RenderCommandEncoder* _encoder = nullptr;
    TargetId _current = {};
    MTL::Texture* _currentResolve = nullptr;
    std::array<TargetId, 8> _opened = {};
    std::size_t _openedCount = 0;
};
} // namespace Poseidon::Metal
