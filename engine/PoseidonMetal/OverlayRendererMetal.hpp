#pragma once

#include <Poseidon/Dev/Debug/OverlayRenderer.hpp>
#include <PoseidonMetal/MetalFwd.hpp>
#include <PoseidonMetal/OverlayBufferRing.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_set>

struct ImDrawCmd;
struct ImDrawData;
struct ImDrawList;
struct ImTextureData;

namespace Poseidon
{
class EngineMetal;

namespace Metal
{
/// Byte-compatible mirror of ImDrawVert. OverlayRendererMetal.cpp pins this
/// layout to the vendored ImGui definition with compile-time assertions.
struct OverlayVertex
{
    float pos[2];
    float uv[2];
    unsigned char col[4];
};

class OverlayRendererMetal final : public Dev::IOverlayRenderer
{
  public:
    explicit OverlayRendererMetal(EngineMetal& engine);
    ~OverlayRendererMetal() override;

    bool Init(SDL_Window* window) override;
    void Shutdown() override;
    bool PrepareShutdown() override;
    void NewFrame() override {}
    void CorrectDisplayMetrics() override;
    void RenderDrawData(ImDrawData* drawData) override;
    const char* Name() const override { return "metal"; }

  private:
    // C3 texture-manager entry points. Their implementation belongs in
    // OverlayRendererMetal_Textures.cpp.
    bool UpdateTextures(ImDrawData* drawData);
    bool UpdateTexture(ImTextureData* texture);
    bool DestroyTexture(ImTextureData* texture);
    bool UploadTextureRegion(ImTextureData* textureData, MTL::Texture* texture, unsigned x, unsigned y, unsigned width,
                             unsigned height, bool waitForCompletion);
    void ShutdownTextures();
    MTL::Texture* ResolveTexture(std::uint64_t textureId) const;
    void RecordDrawDiagnosticOnce(std::uint64_t bit, const char* message);
    void RecordTextureDiagnosticOnce(ImTextureData* texture, const char* message);

    static void ResetRenderStateSentinel(const ImDrawList* parentList, const ImDrawCmd* command);

    EngineMetal& _engine;
    MTL::RenderPipelineState* _pipeline = nullptr; // Borrowed from EngineMetal's PSO cache.
    OverlayBufferRing _bufferRing;
    std::size_t _textureCreates = 0;
    std::size_t _textureDestroys = 0;
    std::size_t _liveTextureHandles = 0;
    std::unordered_set<const ImTextureData*> _textureFailures;
    std::uint64_t _drawDiagnosticMask = 0;
    mutable bool _loggedInvalidTextureResolve = false;
    bool _platformInitialized = false;
    bool _rendererStateInstalled = false;
    bool _executingUserCallback = false;
};
} // namespace Metal
} // namespace Poseidon
