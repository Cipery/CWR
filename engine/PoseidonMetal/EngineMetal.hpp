#pragma once

#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Shared/SDLEventWindow.hpp>
#include <PoseidonMetal/FrameRing.hpp>
#include <PoseidonMetal/MetalFwd.hpp>
#include <PoseidonMetal/MetalContext.hpp>

#include <memory>
#include <mutex>
#include <string>

namespace Poseidon
{
class TextBankDummy;

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

    void PrepareTriangle(const MipInfo&, int) override {}
    void DrawPolygon(const VertexIndex*, int) override {}
    void DrawSection(const FaceArray&, Offset, Offset) override {}
    void DrawDecal(Vector3Par, float, float, float, PackedColor, const MipInfo&, int) override {}
    void Draw2D(const Draw2DPars&, const Rect2DAbs&, const Rect2DAbs&) override {}
    void DrawPoly(const MipInfo&, const Vertex2DAbs*, int, const Rect2DAbs&, int) override {}
    void DrawPoly(const MipInfo&, const Vertex2DPixel*, int, const Rect2DPixel&, int) override {}
    void DrawLine(const Line2DAbs&, PackedColor, PackedColor, const Rect2DAbs&) override {}
    void DrawLine(int, int) override {}
    void PrepareMesh(const render::LegacySpec&) override {}
    void BeginMesh(TLVertexTable&, const render::LegacySpec&) override {}
    void EndMesh(TLVertexTable&) override {}
    AbstractTextBank* TextBank() override;
    void TextureDestroyed(Texture*) override {}

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

  private:
    bool InitializeWindow(int width, int height, bool requestedWindowed);
    void DestroyWindow();
    bool LoadShaderLibrary();
    void ApplyPendingResize();
    void AttachDiagnostics(MTL::CommandBuffer* commandBuffer);

    MetalContext _metal;
    FrameRing _frameRing;
    std::shared_ptr<MetalDiagnostics> _diagnostics = std::make_shared<MetalDiagnostics>();

    TextBankDummy* _textBank = nullptr;
    SDL_Window* _sdlWindow = nullptr;
    SDLEventWindow _eventWindow;
    MTL::CommandBuffer* _frameCommandBuffer = nullptr;
    NS::AutoreleasePool* _framePool = nullptr;

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
    bool _initialized = false;
    WindowMode _windowMode = WindowMode::Windowed;
    std::unique_ptr<MTL::ClearColor> _clearColor;
};

Engine* CreateEngineMetal(int width, int height, bool windowed, int bpp);

} // namespace Poseidon
