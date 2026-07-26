#pragma once

#include <cstddef>

namespace Poseidon::Dev
{
struct OverlayClipRect
{
    float x;
    float y;
    float z;
    float w;
};

struct OverlayDisplayMetrics
{
    float displayPosX;
    float displayPosY;
    float framebufferScaleX;
    float framebufferScaleY;
    std::size_t framebufferWidth;
    std::size_t framebufferHeight;
};

struct OverlayScissor
{
    std::size_t x;
    std::size_t y;
    std::size_t width;
    std::size_t height;
};

/// Converts an ImGui-space clip rectangle to framebuffer pixels. Coordinates
/// are clamped to the framebuffer and truncated to integer pixel boundaries.
/// Returns false when the resulting integer rectangle is empty.
[[nodiscard]] bool ComputeOverlayScissor(const OverlayClipRect& clipRect, const OverlayDisplayMetrics& metrics,
                                         OverlayScissor& scissor);
} // namespace Poseidon::Dev
