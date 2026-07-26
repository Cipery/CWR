#pragma once

#include <Poseidon/Dev/Debug/OverlayRenderer.hpp>

namespace Poseidon::Dev
{
class OverlayRendererGL33 final : public IOverlayRenderer
{
  public:
    explicit OverlayRendererGL33(void* glContext) : _glContext(glContext) {}

    bool Init(SDL_Window* window) override;
    void Shutdown() override;
    void NewFrame() override;
    void RenderDrawData(ImDrawData* drawData) override;
    const char* Name() const override;

  private:
    void* _glContext;
    bool _platformInitialized = false;
    bool _rendererInitialized = false;
};
} // namespace Poseidon::Dev
