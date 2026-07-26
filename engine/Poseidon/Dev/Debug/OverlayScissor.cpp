#include <Poseidon/Dev/Debug/OverlayScissor.hpp>

#include <algorithm>

namespace Poseidon::Dev
{
bool ComputeOverlayScissor(const OverlayClipRect& clipRect, const OverlayDisplayMetrics& metrics,
                           OverlayScissor& scissor)
{
    scissor = {};

    const float framebufferWidth = static_cast<float>(metrics.framebufferWidth);
    const float framebufferHeight = static_cast<float>(metrics.framebufferHeight);
    const float clipMinX = std::max(0.0f, (clipRect.x - metrics.displayPosX) * metrics.framebufferScaleX);
    const float clipMinY = std::max(0.0f, (clipRect.y - metrics.displayPosY) * metrics.framebufferScaleY);
    const float clipMaxX = std::min(framebufferWidth, (clipRect.z - metrics.displayPosX) * metrics.framebufferScaleX);
    const float clipMaxY = std::min(framebufferHeight, (clipRect.w - metrics.displayPosY) * metrics.framebufferScaleY);
    if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
        return false;

    const std::size_t scissorX = static_cast<std::size_t>(clipMinX);
    const std::size_t scissorY = static_cast<std::size_t>(clipMinY);
    // Match imgui_impl_metal: truncate the origin and extent independently.
    const std::size_t scissorWidth = static_cast<std::size_t>(clipMaxX - clipMinX);
    const std::size_t scissorHeight = static_cast<std::size_t>(clipMaxY - clipMinY);
    if (scissorWidth == 0 || scissorHeight == 0)
        return false;

    scissor = {scissorX, scissorY, scissorWidth, scissorHeight};
    return true;
}
} // namespace Poseidon::Dev
