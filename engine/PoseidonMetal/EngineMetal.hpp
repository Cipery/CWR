#pragma once

#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/Graphics/Shared/SDLEventWindow.hpp>
#include <PoseidonMetal/FrameRing.hpp>
#include <PoseidonMetal/HandleRegistry.hpp>
#include <PoseidonMetal/MetalFwd.hpp>
#include <PoseidonMetal/MetalContext.hpp>
#include <PoseidonMetal/PipelineKey.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
class TextBankMetal;
class TextureMetal;

struct MetalDiagnostics
{
    mutable std::mutex mutex;
    unsigned int errorCount = 0;
    std::string lastMessage;
};

class EngineMetal final : public Engine
{
    using base = Engine;

  public:
    EngineMetal(int width, int height, bool windowed, int bpp);
    ~EngineMetal() override;

    bool IsAbleToDraw() override;
    bool InitDrawDone() override { return _frameOpen; }
    void InitDraw(bool clear = false, PackedColor color = PackedColor(0)) override;
    void FinishDraw() override;
    void NextFrame() override;
    void Clear(bool clearZ = true, bool clear = true, PackedColor color = PackedColor(0)) override;

    void Pause() override {}
    void Restore() override {}
    void FogColorChanged(ColorVal) override {}

    bool SwitchRes(int w, int h, int bpp) override;
    bool SwitchRefreshRate(int refresh) override;
    bool SetWindowMode(WindowMode mode) override;
    WindowMode GetCurrentWindowMode() const override;
    void OnWindowResized(int w, int h) override;
    void OnFullscreenChanged(bool windowed) override;

    void HandleEvents() override { _eventWindow.HandleEvents(); }
    bool IsOpen() const override { return _eventWindow.IsOpen(); }
    void SetMouseGrab(bool grab) override { _eventWindow.SetMouseGrab(grab); }
    bool IsMouseGrabbed() const override { return _eventWindow.IsMouseGrabbed(); }

    void ListResolutions(FindArray<ResolutionInfo>& ret) override;
    void ListRefreshRates(FindArray<int>& ret) override;
    void ListMonitors(FindArray<MonitorInfo>& ret) override;
    int GetCurrentMonitor() const override;
    bool SwitchMonitor(int idx) override;
    bool GetDesktopDisplayMode(int& w, int& h, int& refresh) const override;
    bool GetCurrentDisplayMode(int& w, int& h, int& refresh) const override;
    bool GetRequestedFullscreenMode(int& w, int& h, int& refresh) const override;
    bool SetSwapInterval(int interval) override;
    int GetSwapInterval() const override { return _swapInterval; }

    RString GetDebugName() const override;
    RString GetRendererName() const override { return "Metal 3"; }
    unsigned int GetDebugErrorCount() const override;
    std::string GetLastDebugMessage() const override;

    void SetGamma(float gamma) override { _gamma = gamma; }
    float GetGamma() const override { return _gamma; }

    int Width() const override { return _w; }
    int Height() const override { return _h; }
    int PixelSize() const override { return _pixelSize; }
    int RefreshRate() const override { return _refreshRate; }
    bool CanBeWindowed() const override { return true; }
    bool IsWindowed() const override { return _windowed; }
    bool IsResizable() const override;
    int AFrameTime() const override { return 0; }

    void PrepareTriangle(const MipInfo&, int) override;
    void DrawPolygon(const VertexIndex*, int) override;
    void DrawSection(const FaceArray&, Offset, Offset) override;
    void DrawDecal(Vector3Par, float, float, float, PackedColor, const MipInfo&, int) override {}
    void Draw2D(const Draw2DPars&, const Rect2DAbs&, const Rect2DAbs&) override;
    void DrawPoly(const MipInfo&, const Vertex2DAbs*, int, const Rect2DAbs&, int) override;
    void DrawPoly(const MipInfo&, const Vertex2DPixel*, int, const Rect2DPixel&, int) override;
    void DrawLine(const Line2DAbs&, PackedColor, PackedColor, const Rect2DAbs&) override;
    void DrawLine(int, int) override;
    void PrepareMesh(const render::LegacySpec&) override;
    void BeginMesh(TLVertexTable&, const render::LegacySpec&) override;
    void EndMesh(TLVertexTable&) override;
    void EnableReorderQueues(bool enable) override;
    void FlushQueues() override;
    void EmitDraw(const render::frame::Draw& d) override;
    AbstractTextBank* TextBank() override;
    void TextureDestroyed(Texture*) override;
    void Screenshot(RString filename) override;
    void FlushPendingScreenshot() override;
    int SampleBackBufferNonBlack() override;
    bool SamplePixel(int x, int y, uint8_t* outRGB) override;
    void DrawTestPattern(const char* name) override;
    size_t GetDrawItemCount() const override { return _drawItems.size(); }
    const std::vector<DrawItem>* GetRecordedDraws() const override { return &_drawItems; }

    float ZShadowEpsilon() const override { return 0.0f; }
    float ZRoadEpsilon() const override { return 0.0f; }
    float ObjMipmapCoef() const override { return 1.0f; }
    void GetZCoefs(float& zAdd, float& zMult) override
    {
        zAdd = 0.0f;
        zMult = 1.0f;
    }
    int GetBias() override { return _bias; }
    void SetBias(int value) override { _bias = value; }
    bool CanZBias() const override { return false; }
    bool ZBiasExclusion() const override { return false; }
    void ResetForRemount() override;

    bool IsTextureHandleLive(std::uint32_t handle) const;

  private:
    friend class TextureMetal;
    friend class TextBankMetal;

    struct TriQueue
    {
        TextureMetal* texture = nullptr;
        int level = 0;
        int special = 0;
        PassId passId = PassId::Opaque;
        std::vector<std::uint16_t> indices;
    };
    struct MeshResource
    {
        MTL::Buffer* vertices = nullptr; // Frame-ring owned.
        MTL::Buffer* indices = nullptr;  // Frame-ring owned.
        std::size_t vertexOffset = 0;
        std::size_t indexOffset = 0;
    };

    bool InitializeWindow(int width, int height, bool requestedWindowed);
    void DestroyWindow();
    bool LoadShaderLibrary();
    bool InitializeM1Resources();
    void DestroyM1Resources();
    bool RebuildFrameTargets();
    bool InitializePipelines();
    bool InitializeDepthStates();
    bool InitializeSamplers();
    bool InitializeFallbackTextures();
    MTL::RenderPipelineState* ResolvePipeline(Metal::PipelineKey key, bool logMiss);
    MTL::RenderPipelineState* BuildPipeline(Metal::PipelineKey key);
    MTL::RenderCommandEncoder* EnsureFrameEncoder();
    void EndFrameEncoder();
    bool ApplyScreenState(const render::RenderPassDescriptor& descriptor, Metal::FragmentStage fragment);
    void QueueVertices(const TLVertex* vertices, int count);
    void QueueFan(const VertexIndex* indices, int count);
    void Queue2DPoly(int count);
    TriQueue& QueueFor(TextureMetal* texture, int level, int spec);
    void FlushQueueBatch();
    bool UploadTexture(TextureMetal& texture, int levelMin);
    bool UploadDynamicTexture(TextureMetal& texture, int width, int height, const void* rgba, std::uint32_t size,
                              bool mipmap);
    void ReleaseTexture(TextureMetal& texture);
    bool ReadCapture(std::vector<std::uint8_t>& bgra, int& width, int& height);
    bool ReadCapturePixel(int x, int y, std::uint8_t rgba[4]);
    bool SubmitSynchronousReadback(MTL::CommandBuffer* commandBuffer);
    void CaptureScreenshotIfPending();
    void ApplyPendingResize();
    void RecordDiagnostic(const std::string& message);
    void AttachDiagnostics(MTL::CommandBuffer* commandBuffer);

    MetalContext _metal;
    FrameRing _frameRing;
    std::shared_ptr<MetalDiagnostics> _diagnostics = std::make_shared<MetalDiagnostics>();

    TextBankMetal* _textBank = nullptr;
    SDL_Window* _sdlWindow = nullptr;
    SDLEventWindow _eventWindow;
    MTL::CommandBuffer* _frameCommandBuffer = nullptr;
    MTL::RenderCommandEncoder* _frameEncoder = nullptr;
    NS::AutoreleasePool* _framePool = nullptr;

    MTL::Texture* _frameColor = nullptr;
    MTL::Texture* _frameDepthStencil = nullptr;
    std::array<MTL::Texture*, 2> _fallbackWhite = {};
    HandleRegistry<MTL::Texture> _textureRegistry;
    HandleRegistry<MeshResource> _meshRegistry;
    std::unordered_map<std::uint32_t, MTL::RenderPipelineState*> _pipelineCache;
    std::array<MTL::DepthStencilState*, static_cast<std::size_t>(Metal::DepthMode::Count)> _depthStates = {};
    std::array<MTL::SamplerState*, 8> _samplers = {};
    std::vector<TLVertex> _queuedVertices;
    std::vector<TriQueue> _triQueues;
    std::vector<DrawItem> _drawItems;
    std::vector<std::unique_ptr<MeshResource>> _frameMeshes;
    std::vector<std::uint32_t> _frameMeshHandles;
    TLVertexTable* _mesh = nullptr;
    int _meshBase = 0;
    int _currentPrimitiveBase = 0;
    int _activeQueue = -1;
    bool _enableReorder = true;
    bool _frameNeedsClear = true;
    bool _clearDepth = true;
    RString _pendingScreenshotPath;

    int _w = 0;
    int _h = 0;
    int _pixelSize = 32;
    int _refreshRate = 60;
    int _bias = 0;
    int _swapInterval = 1;
    int _windowedRestoreW = 0;
    int _windowedRestoreH = 0;
    int _pendingPixelW = 0;
    int _pendingPixelH = 0;
    float _gamma = 1.0f;
    bool _windowed = true;
    bool _frameOpen = false;
    bool _loggedMidFrameReadback = false;
    bool _initialized = false;
    WindowMode _windowMode = WindowMode::Windowed;
    std::unique_ptr<MTL::ClearColor> _clearColor;
};

Engine* CreateEngineMetal(int width, int height, bool windowed, int bpp);

} // namespace Poseidon
