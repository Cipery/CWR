#pragma once

#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Core/RenderState.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/Graphics/Rendering/RenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Shared/SDLEventWindow.hpp>
#include <PoseidonMetal/EncoderBroker.hpp>
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
namespace Metal
{
class OverlayRendererMetal;
}

class TextBankMetal;
class TextureMetal;
class VertexBufferMetal;

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
    // Instanced-run accumulation capacity — the WorldInstances upload clamp
    // in EngineMetal_Constants.cpp static_asserts against this value.
    static constexpr int kInstArrayCapacity = 256;

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
    void FogColorChanged(ColorVal) override;
    void EnableNightEye(float night) override;
    void SetGrassParams(float a1, float a2, float a3 = 0, float a4 = 0) override;
    bool CanGrass() const override { return true; }
    void SetAlphaToCoverage(bool enable) override;
    bool GetAlphaToCoverage() const override { return _alphaToCoverageCfg && _msaaSamples > 1; }
    void SetDebugFlatColor(bool enable) override;
    bool GetDebugFlatColor() const override { return _debugFlatColor; }

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
    void PrepareTriangleTL(const MipInfo&, const render::LegacySpec&) override;
    void DrawPolygon(const VertexIndex*, int) override;
    void DrawSection(const FaceArray&, Offset, Offset) override;
    void DrawDecal(Vector3Par, float, float, float, PackedColor, const MipInfo&, int) override;
    void Draw2D(const Draw2DPars&, const Rect2DAbs&, const Rect2DAbs&) override;
    void DrawPoly(const MipInfo&, const Vertex2DAbs*, int, const Rect2DAbs&, int) override;
    void DrawPoly(const MipInfo&, const Vertex2DPixel*, int, const Rect2DPixel&, int) override;
    void DrawLine(const Line2DAbs&, PackedColor, PackedColor, const Rect2DAbs&) override;
    void DrawLine(int, int) override;
    void DrawPoints(int beg, int end) override;
    void PrepareMesh(const render::LegacySpec&) override;
    void BeginMesh(TLVertexTable&, const render::LegacySpec&) override;
    void EndMesh(TLVertexTable&) override;
    void EnableReorderQueues(bool enable) override;
    void FlushQueues() override;
    void BeginShadowPass() override;
    void EndShadowPass() override;
    void EmitDraw(const render::frame::Draw& d) override;
    VertexBuffer* CreateVertexBuffer(const Shape& src, VBType type) override;
    int CompareBuffers(const Shape&, const Shape&) override { return 0; }
    bool GetTL() const override { return true; }
    bool GetTLOnSurface() const override { return true; }
    void SetMaterial(const TLMaterial&, const LightList&, const render::LegacySpec&) override;
    void EnableSunLight(bool enable) override;
    void UpdateProjection() override;
    void InstancedRunReset() override { _instPending = 0; }
    bool InstancedRunAdd(const Matrix4& modelToWorld) override;
    void BeginInstancedRunUpload() override;
    bool EndInstancedRun() override;
    void PrepareMeshTL(const LightList&, const Matrix4&, const render::LegacySpec&) override;
    void BeginMeshTL(const Shape&, int, bool dynamic = false) override;
    void EndMeshTL(const Shape&) override;
    void DrawSectionTL(const Shape&, int, int) override;
    AbstractTextBank* TextBank() override;
    void TextureDestroyed(Texture*) override;
    void Screenshot(RString filename) override;
    void FlushPendingScreenshot() override;
    int SampleBackBufferNonBlack() override;
    bool SamplePixel(int x, int y, uint8_t* outRGB) override;
    void DrawTestPattern(const char* name) override;
    size_t GetDrawItemCount() const override { return _drawItems.size(); }
    const std::vector<DrawItem>* GetRecordedDraws() const override { return &_drawItems; }
    bool GetGLViewport(int outRect[4]) const override;
    int MinGuardX() const override { return _minGuardX; }
    int MaxGuardX() const override { return _maxGuardX; }
    int MinGuardY() const override { return _minGuardY; }
    int MaxGuardY() const override { return _maxGuardY; }
    int MinSatX() const override { return _minGuardX; }
    int MaxSatX() const override { return _maxGuardX; }
    int MinSatY() const override { return _minGuardY; }
    int MaxSatY() const override { return _maxGuardY; }
    void SetRenderScale(float scale) override;
    float GetRenderScale() const override { return _renderScale; }
    void SetMsaaSamples(int samples) override;
    int GetMsaaSamples() const override { return _msaaSamples; }
    bool ShadowDepthProbe(const float* lightVP16, const float* triXYZ, int vertCount, int res,
                          float* outDepth) override;
    void SetShadowMapsEnabled(bool enabled) override { _shadowTuning.enabled = enabled; }
    bool ShadowMapsEnabled() const override { return _shadowTuning.enabled; }
    ShadowMapTuning GetShadowMapTuning() const override { return _shadowTuning; }
    void SetShadowMapTuning(const ShadowMapTuning& tuning) override { _shadowTuning = tuning; }
    void SetShadowMapSunFactor(float factor) override;
    void RenderShadowDepthScene(const float* lightVPs, const float* splitViewDist, const float* camFwd3,
                                int numCascades, int omniCount, int res,
                                const ShadowCasterSet& casters) override;
    bool DumpShadowMap(const char* path) override;
    bool ShadowMapCacheSelfTest() override;

    float ZShadowEpsilon() const override { return 0.01f; }
    float ZRoadEpsilon() const override { return 0.005f; }
    float ObjMipmapCoef() const override { return 1.5f; }
    void GetZCoefs(float& zAdd, float& zMult) override;
    int GetBias() override { return _bias; }
    void SetBias(int value) override;
    bool CanZBias() const override { return false; }
    bool ZBiasExclusion() const override { return false; }
    void ResetForRemount() override;

    bool IsTextureHandleLive(std::uint32_t handle) const;

  private:
    friend class TextureMetal;
    friend class TextBankMetal;
    friend class VertexBufferMetal;
    friend class Metal::OverlayRendererMetal;

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
        MTL::Buffer* vertices = nullptr;
        MTL::Buffer* indices = nullptr;
        std::size_t vertexOffset = 0;
        std::size_t indexOffset = 0;
        std::size_t vertexBytes = 0;
        std::size_t indexBytes = 0;
        bool owned = false;
    };
    struct ViewportState
    {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
    };
    struct ScissorState
    {
        unsigned x = 0;
        unsigned y = 0;
        unsigned width = 0;
        unsigned height = 0;
    };

    bool InitializeWindow(int width, int height, bool requestedWindowed);
    void DestroyWindow();
    bool LoadShaderLibrary();
    bool InitializeM1Resources();
    void DestroyM1Resources();
    bool RebuildFrameTargets();
    void RefreshSupportedSampleCounts();
    int ClampMsaaSampleCount(int samples);
    unsigned FrameSampleCount() const;
    std::uint8_t FrameSampleCountLog2() const;
    MTL::Texture* ResolvedFrameColor() const;
    bool InitializePipelines();
    bool InitializeDepthStates();
    bool InitializeSamplers();
    bool InitializeFallbackTextures();
    MTL::RenderPipelineState* ResolvePipeline(Metal::PipelineKey key, bool logMiss);
    MTL::RenderPipelineState* BuildPipeline(Metal::PipelineKey key);
    MTL::RenderCommandEncoder* EnsureFrameEncoder();
    MTL::RenderCommandEncoder* EnsureCascadeEncoder(unsigned layer);
    void EndFrameEncoder(bool terminal = false, bool resolveForReadback = false);
    static void ReplayFrameStickyStateThunk(void* context, MTL::RenderCommandEncoder* encoder);
    void ReplayFrameStickyState(MTL::RenderCommandEncoder* encoder);
    void ApplyDepthBias(MTL::RenderCommandEncoder* encoder, const render::RenderPassDescriptor& descriptor);
    bool ApplyScreenState(const render::RenderPassDescriptor& descriptor, Metal::FragmentStage fragment);
    bool ApplyWorldState(const render::RenderPassDescriptor& descriptor);
    Metal::FragmentStage FragmentStageFor(const render::RenderPassDescriptor& descriptor) const;
    void ApplyWorldViewport();
    void EndWorldViewport();
    void FillFrameRectBlack(unsigned x, unsigned y, unsigned width, unsigned height);
    void SetViewport(const ViewportState& viewport);
    void SetFullScissor();
    void DrawClear(bool clearDepthStencil, bool clearColor, const MTL::ClearColor& color);
    void MarkConstantsDirty();
    bool SnapshotConstants();
    bool BindWorldSlot(const GfxMatrix& world);
    bool UploadWorldInstances(const GfxMatrix* matrices, int count);
    void BeginInstancedRun(int count);
    int InstancedRunPending() const { return _instPending; }
    void UploadFrameConstants(const FrameState& frame);
    void UploadProjection(const GfxMatrix& projection);
    void UploadMaterialConstants(const TLMaterial& material);
    void UploadLightConstants(const LightList& lights, const TLMaterial& material, float nightEffect);
    void UploadTexGenConstants(render::TexGenMode mode);
    void UploadPSConstant(int slot, const float data[4]);
    void UpdateShadowMapLitState();
    bool EnsureShadowDepthArray(int resolution, int layers);
    bool ReadShadowDepthLayer(MTL::Texture* texture, int layer, std::vector<float>& depth);
    FrameState BuildFrameState();
    bool UploadStaticMesh(MeshResource& resource, const void* vertices, std::size_t vertexBytes,
                          const void* indices, std::size_t indexBytes);
    void ReleaseMesh(std::uint32_t handle, MeshResource& resource);
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
    MTL::Texture* EncodeCaptureResolve(MTL::CommandBuffer* commandBuffer);
    MTL::Texture* EncodeOverlayCaptureResolve(MTL::CommandBuffer* commandBuffer);
    void DiscardOverlayCaptureResolve();
    void MarkOverlayCaptureCommitted();
    bool SubmitSynchronousReadback(MTL::CommandBuffer* commandBuffer);
    void CaptureScreenshotIfPending();
    void ApplyPendingResize();
    void RecordDiagnostic(const std::string& message);
    void RecordOverlayDiagnosticOnce(unsigned bit, const char* message);
    void AttachDiagnostics(MTL::CommandBuffer* commandBuffer);

    MetalContext _metal;
    Metal::EncoderBroker _encoderBroker;
    FrameRing _frameRing;
    std::shared_ptr<MetalDiagnostics> _diagnostics = std::make_shared<MetalDiagnostics>();

    TextBankMetal* _textBank = nullptr;
    SDL_Window* _sdlWindow = nullptr;
    SDLEventWindow _eventWindow;
    MTL::CommandBuffer* _frameCommandBuffer = nullptr;
    MTL::RenderCommandEncoder* _frameEncoder = nullptr;
    NS::AutoreleasePool* _framePool = nullptr;

    MTL::Texture* _frameColor = nullptr;
    MTL::Texture* _frameResolveColor = nullptr;
    MTL::Texture* _frameDepthStencil = nullptr;
    MTL::Texture* _captureColor = nullptr;
    MTL::Texture* _overlayCaptureColor = nullptr;
    MTL::Texture* _overlayCaptureTarget = nullptr;          // Pending overlay command buffer.
    MTL::Texture* _committedOverlayCaptureTarget = nullptr; // Authoritative after commit.
    std::array<MTL::Texture*, 2> _fallbackWhite = {};
    MTL::Texture* _fallbackShadowDepth = nullptr;
    MTL::Texture* _shadowDepthArray = nullptr;
    MTL::Texture* _shadowProbeDepth = nullptr;
    HandleRegistry<MTL::Texture> _textureRegistry;
    HandleRegistry<MeshResource> _meshRegistry;
    std::unordered_map<std::uint32_t, MTL::RenderPipelineState*> _pipelineCache;
    std::array<MTL::DepthStencilState*, static_cast<std::size_t>(Metal::DepthMode::Count)> _depthStates = {};
    MTL::DepthStencilState* _shadowDepthState = nullptr;
    std::array<MTL::SamplerState*, 8> _samplers = {};
    MTL::SamplerState* _shadowCompareSampler = nullptr;
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
    bool _captureResolvedThisFrame = false;
    bool _worldViewportActive = false;
    bool _in3DPass = false;
    bool _skipCurrentWorldDraw = false;
    bool _constantsVSDirty = true;
    bool _constantsPSDirty = true;
    bool _sunEnabled = true;
    MTL::Buffer* _boundVSBuffer = nullptr;
    MTL::Buffer* _boundPSBuffer = nullptr;
    MTL::Buffer* _boundWorldBuffer = nullptr;
    std::size_t _boundVSOffset = 0;
    std::size_t _boundPSOffset = 0;
    std::size_t _boundWorldOffset = 0;
    MTL::Buffer* _encoderVSBuffer = nullptr;
    MTL::Buffer* _encoderPSBuffer = nullptr;
    MTL::Buffer* _encoderWorldBuffer = nullptr;
    std::size_t _encoderVSOffset = 0;
    std::size_t _encoderPSOffset = 0;
    std::size_t _encoderWorldOffset = 0;
    MTL::Buffer* _stickyVSBuffer = nullptr;
    MTL::Buffer* _stickyPSBuffer = nullptr;
    MTL::Buffer* _stickyWorldBuffer = nullptr;
    std::size_t _stickyVSOffset = 0;
    std::size_t _stickyPSOffset = 0;
    std::size_t _stickyWorldOffset = 0;
    MTL::RenderPipelineState* _currentPipeline = nullptr;
    bool _currentPipelineWorld = false;
    MTL::DepthStencilState* _currentDepthState = nullptr;
    std::array<MTL::Texture*, 3> _stickyFragmentTextures = {};
    std::array<MTL::SamplerState*, 3> _stickyFragmentSamplers = {};
    bool _currentDepthClamp = false;
    float _currentDepthBias = 0.0f;
    float _currentDepthSlope = 0.0f;
    render::CullMode _currentCull = render::CullMode::None;
    render::FrontFaceMode _currentWinding = render::FrontFaceMode::CW;
    ViewportState _currentViewport = {};
    ScissorState _currentScissor = {};
    FrameState _frameState = {};
    DrawItem _currentDrawItem = {};
    render::RenderPassDescriptor _currentDescriptor = {};
    render::TexGenMode _texGenMode = render::TexGenMode::None;
    std::array<float, 70 * 4> _vsConstants = {};
    std::array<float, 27 * 4> _psConstants = {};
    TLMaterial _materialSet = {};
    std::uint64_t _materialLightsSignature = 0;
    int _materialSetSpec = -1;
    int _activePassId = static_cast<int>(PassId::ScreenSpace);
    int _instCount = 0;
    bool _instImpure = false;
    int _instPending = 0;
    std::array<GfxMatrix, kInstArrayCapacity> _instArray = {};
    MTL::Buffer* _runWorldBuffer = nullptr;
    std::size_t _runWorldOffset = 0;
    RString _pendingScreenshotPath;
    ShadowMapTuning _shadowTuning;
    float _shadowSunFactor = 1.0f;
    bool _shadowMapActive = false;
    int _shadowMapRes = 0;
    int _shadowMapLayers = 0;
    int _shadowCascades = 0;
    int _shadowOmniCount = 0;
    std::array<float, 4 * 16> _shadowMapVP = {};
    std::array<float, 4> _shadowSplits = {};
    std::array<float, 3> _shadowCamFwd = {};

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
    float _nightEye = 0.0f;
    std::array<float, 4> _grassParams = {};
    float _renderScale = 1.0f;
    float _pendingRenderScale = 1.0f;
    int _msaaSamples = 0;
    int _pendingMsaaSamples = 0;
    std::array<bool, 4> _supportedSampleCounts = {true, false, false, false};
    unsigned _msaaClampLoggedMask = 0;
    int _minGuardX = -4096;
    int _maxGuardX = _w + 4096;
    int _minGuardY = -4096;
    int _maxGuardY = _h + 4096;
    bool _windowed = true;
    bool _frameOpen = false;
    bool _loggedMidFrameReadback = false;
    unsigned _overlayDiagnosticMask = 0;
    bool _alphaToCoverageCfg = true;
    bool _debugFlatColor = false;
    bool _initialized = false;
    WindowMode _windowMode = WindowMode::Windowed;
    std::unique_ptr<MTL::ClearColor> _clearColor;
};

Engine* CreateEngineMetal(int width, int height, bool windowed, int bpp);

} // namespace Poseidon
