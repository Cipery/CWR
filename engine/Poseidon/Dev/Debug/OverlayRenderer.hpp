#pragma once

struct ImDrawData;
struct SDL_Window;

namespace Poseidon::Dev
{
/// Renderer half of the ImGui dev overlay. One implementation per graphics
/// backend. The overlay owns the instance handed to DebugOverlay::Init and
/// destroys it in DebugOverlay::Shutdown.
///
/// Lifecycle, mirroring the ImGui backend contract:
///   Init(window)            - once, after the graphics device exists
///   NewFrame()              - once per frame, before ImGui_ImplSDL3_NewFrame()
///   CorrectDisplayMetrics() - once per frame, after ImGui_ImplSDL3_NewFrame()
///                             and before ImGui::NewFrame()
///   RenderDrawData(dd)      - once per frame, after ImGui::Render()
///   PrepareShutdown()       - owner-side guard before destruction
///   Shutdown()              - safe after successful or failed Init(); releases
///                             GPU resources, shuts down the SDL platform binding,
///                             and clears renderer-owned ImGui state
class IOverlayRenderer
{
  public:
    virtual ~IOverlayRenderer() = default;

    /// Creates renderer-side GPU objects and sets io.BackendRendererName /
    /// io.BackendFlags. `window` is required by the SDL platform binding.
    /// Returns false on failure; Shutdown() must undo every partial platform /
    /// backend initialization and clear all renderer-owned ImGui state.
    /// Called after the ImGui context exists.
    virtual bool Init(SDL_Window* window) = 0;

    /// Releases successful or partial initialization. Must be idempotent.
    /// Clears only this renderer's flags/name/callbacks before its ImGui context dies.
    virtual void Shutdown() = 0;

    /// Gives a backend with externally owned in-flight state a chance to reject
    /// shutdown. A false result leaves the renderer and ImGui context alive.
    virtual bool PrepareShutdown() { return true; }

    /// Per-frame renderer hook. Occupies the slot where the GL path calls
    /// ImGui_ImplOpenGL3_NewFrame().
    virtual void NewFrame() = 0;

    /// Called immediately after ImGui_ImplSDL3_NewFrame() and before
    /// ImGui::NewFrame(). Lets a backend correct io.DisplaySize and
    /// io.DisplayFramebufferScale to match the surface it will actually
    /// render into. Default: leave the platform backend's values untouched.
    virtual void CorrectDisplayMetrics() {}

    /// Uploads pending texture requests and draws `drawData`. Must be a safe
    /// no-op when drawData has no command lists.
    virtual void RenderDrawData(ImDrawData* drawData) = 0;

    /// Stable identifier for logs (e.g. "gl33", "metal").
    virtual const char* Name() const = 0;
};
} // namespace Poseidon::Dev
