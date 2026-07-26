#include <PoseidonGL33/OverlayRendererGL33.hpp>

// The PCH pulls in Logging.hpp which #defines DebugLog() as a logging macro.
// That collides with the method ImGui::DebugLog(). Undef before including
// ImGui headers -- none of our code in this TU uses the DebugLog macro.
#ifdef DebugLog
#undef DebugLog
#endif

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include <glad/gl.h>

namespace Poseidon::Dev
{
bool OverlayRendererGL33::Init(SDL_Window* window)
{
    _platformInitialized = ImGui_ImplSDL3_InitForOpenGL(window, _glContext);
    if (!_platformInitialized)
        return false;

    _rendererInitialized = ImGui_ImplOpenGL3_Init("#version 330");
    return _rendererInitialized;
}

void OverlayRendererGL33::Shutdown()
{
    if (_rendererInitialized)
    {
        ImGui_ImplOpenGL3_Shutdown();
        _rendererInitialized = false;
    }
    if (_platformInitialized)
    {
        ImGui_ImplSDL3_Shutdown();
        _platformInitialized = false;
    }
}

void OverlayRendererGL33::NewFrame()
{
    ImGui_ImplOpenGL3_NewFrame();
}

void OverlayRendererGL33::RenderDrawData(ImDrawData* drawData)
{
    // Make sure we draw to the default framebuffer in case the engine left
    // an FBO bound — happens with post-FX in GL33.  Other state (blend,
    // scissor, vao, depth) is saved/restored inside RenderDrawData.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
}

const char* OverlayRendererGL33::Name() const
{
    return "gl33";
}
} // namespace Poseidon::Dev
