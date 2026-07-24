#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Graphics/Shared/WindowPlacement.hpp>

#include <SDL3/SDL.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <climits>
#include <cstdlib>

namespace Poseidon
{
namespace
{
bool ReadDisplayMode(const SDL_DisplayMode* mode, int& w, int& h, int& refresh)
{
    if (!mode)
        return false;
    w = mode->w;
    h = mode->h;
    refresh = static_cast<int>(mode->refresh_rate + 0.5f);
    return true;
}

bool FindExclusiveDisplayMode(SDL_DisplayID display, int requestedW, int requestedH, int requestedRefresh,
                              SDL_DisplayMode& out)
{
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
    if (!modes)
        return false;

    const SDL_DisplayMode* best = nullptr;
    int bestRefreshDelta = INT_MAX;
    for (int i = 0; i < count; ++i)
    {
        const SDL_DisplayMode* mode = modes[i];
        if (!mode || mode->w != requestedW || mode->h != requestedH)
            continue;

        const int modeRefresh = static_cast<int>(mode->refresh_rate + 0.5f);
        const int refreshDelta = requestedRefresh > 0 ? std::abs(modeRefresh - requestedRefresh) : 0;
        if (!best || refreshDelta < bestRefreshDelta)
        {
            best = mode;
            bestRefreshDelta = refreshDelta;
        }
    }

    const bool found = best != nullptr;
    if (found)
        out = *best;
    SDL_free(modes);
    return found;
}

bool ApplyExclusiveDisplayMode(SDL_Window* window, SDL_DisplayID display, int requestedW, int requestedH,
                               int requestedRefresh)
{
    SDL_DisplayMode target{};
    if (FindExclusiveDisplayMode(display, requestedW, requestedH, requestedRefresh, target))
        return SDL_SetWindowFullscreenMode(window, &target);

    if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display))
        target = *desktop;
    target.w = requestedW;
    target.h = requestedH;
    if (requestedRefresh > 0)
        target.refresh_rate = static_cast<float>(requestedRefresh);
    return SDL_SetWindowFullscreenMode(window, &target);
}

void SyncCachedWindowMode(SDL_Window* window, WindowMode& mode, bool& windowed)
{
    const bool fullscreen = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    windowed = !fullscreen;
    if (windowed)
        mode = WindowMode::Windowed;
    else
        mode = SDL_GetWindowFullscreenMode(window) ? WindowMode::Fullscreen : WindowMode::Borderless;
}

// The macOS 15.2 metal-cpp release omits wrappers for these two CAMetalLayer
// properties. Keep the compatibility shim local; NS::Object::sendMessage is
// protected, so call objc_msgSend directly (cast per the Objective-C ABI) —
// the backend remains pure C++17.
void SetMaximumDrawableCount(CA::MetalLayer* layer, NS::UInteger count)
{
    const SEL selector = sel_registerName("setMaximumDrawableCount:");
    reinterpret_cast<void (*)(void*, SEL, NS::UInteger)>(objc_msgSend)(layer, selector, count);
}

void SetDisplaySyncEnabled(CA::MetalLayer* layer, bool enabled)
{
    const SEL selector = sel_registerName("setDisplaySyncEnabled:");
    reinterpret_cast<void (*)(void*, SEL, BOOL)>(objc_msgSend)(layer, selector, enabled ? YES : NO);
}
} // namespace

bool EngineMetal::InitializeWindow(int width, int height, bool requestedWindowed)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        LOG_ERROR(Graphics, "Metal: SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    EngineConfig& engineCfg = GApp->GetConfig().GetEngineConfig();
    DisplayPlacementInput displayCfg;
    displayCfg.displayMode = engineCfg.displayMode;
    if (requestedWindowed && displayCfg.displayMode != "windowed")
        displayCfg.displayMode = "windowed";
    if (!requestedWindowed && displayCfg.displayMode == "windowed")
        displayCfg.displayMode = "borderless";
    displayCfg.width = width;
    displayCfg.height = height;
    displayCfg.refresh = _refreshRate;

    int desktopW = 0;
    int desktopH = 0;
    int desktopRefresh = 0;
    ReadDisplayMode(SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()), desktopW, desktopH, desktopRefresh);
    const WindowPlacement placement = ResolveWindowPlacement(displayCfg, desktopW, desktopH, desktopRefresh);
    _windowMode = placement.mode;
    _windowed = placement.mode == WindowMode::Windowed;

    SDL_WindowFlags flags = SDL_WINDOW_METAL;
    if (engineCfg.nativePixelDensity)
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (placement.mode == WindowMode::Windowed)
        flags |= SDL_WINDOW_RESIZABLE;
    else
        flags |= SDL_WINDOW_BORDERLESS;

    _sdlWindow = SDL_CreateWindow("Poseidon [Metal]", placement.width, placement.height, flags);
    if (!_sdlWindow)
    {
        LOG_ERROR(Graphics, "Metal: SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    if (placement.mode == WindowMode::Borderless)
    {
        const bool clearedMode = SDL_SetWindowFullscreenMode(_sdlWindow, nullptr);
        const bool enteredFullscreen = SDL_SetWindowFullscreen(_sdlWindow, true);
        if (!clearedMode || !enteredFullscreen)
        {
            LOG_WARN(Graphics, "Metal: borderless fullscreen startup failed: {}", SDL_GetError());
            SyncCachedWindowMode(_sdlWindow, _windowMode, _windowed);
        }
    }
    else if (placement.mode == WindowMode::Fullscreen)
    {
        SDL_DisplayID display = SDL_GetDisplayForWindow(_sdlWindow);
        if (!display)
            display = SDL_GetPrimaryDisplay();
        if (!ApplyExclusiveDisplayMode(_sdlWindow, display, placement.width, placement.height, placement.refreshHz) ||
            !SDL_SetWindowFullscreen(_sdlWindow, true))
        {
            LOG_WARN(Graphics, "Metal: exclusive fullscreen startup failed: {}", SDL_GetError());
            SyncCachedWindowMode(_sdlWindow, _windowMode, _windowed);
        }
    }
    else if (placement.posX != WindowPlacement::kCentered)
    {
        SDL_SetWindowPosition(_sdlWindow, placement.posX, placement.posY);
    }

    _metal.view = SDL_Metal_CreateView(_sdlWindow);
    if (!_metal.view)
    {
        RecordDiagnostic(std::string("SDL_Metal_CreateView failed: ") + SDL_GetError());
        return false;
    }

    _metal.layer = static_cast<CA::MetalLayer*>(SDL_Metal_GetLayer(_metal.view));
    if (!_metal.layer)
    {
        RecordDiagnostic("SDL_Metal_GetLayer returned null");
        return false;
    }

    _metal.layer->setDevice(_metal.device);
    _metal.layer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    _metal.layer->setFramebufferOnly(true);
    SetMaximumDrawableCount(_metal.layer, FrameRing::kMaxFramesInFlight);
    SetDisplaySyncEnabled(_metal.layer, true);

    SDL_GetWindowSizeInPixels(_sdlWindow, &_w, &_h);
    _metal.layer->setDrawableSize(CGSizeMake(_w, _h));
    _windowedRestoreW = placement.width;
    _windowedRestoreH = placement.height;
    if (placement.refreshHz > 0)
        _refreshRate = placement.refreshHz;

    _eventWindow.Attach(
        _sdlWindow, _w, _h, [](void* context, int pixelWidth, int pixelHeight)
        { static_cast<EngineMetal*>(context)->OnWindowResized(pixelWidth, pixelHeight); }, this);
    return true;
}

void EngineMetal::DestroyWindow()
{
    _eventWindow.Detach();
    if (_metal.view)
    {
        SDL_Metal_DestroyView(_metal.view);
        _metal.view = nullptr;
        _metal.layer = nullptr;
    }
    if (_sdlWindow)
    {
        SDL_StopTextInput(_sdlWindow);
        SDL_DestroyWindow(_sdlWindow);
        _sdlWindow = nullptr;
    }
}

bool EngineMetal::IsResizable() const
{
    return _sdlWindow && (SDL_GetWindowFlags(_sdlWindow) & SDL_WINDOW_RESIZABLE) != 0;
}

void EngineMetal::OnWindowResized(int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    _pendingPixelW = w;
    _pendingPixelH = h;
}

void EngineMetal::ApplyPendingResize()
{
    if (_pendingPixelW <= 0 || _pendingPixelH <= 0)
        return;

    _w = _pendingPixelW;
    _h = _pendingPixelH;
    _pendingPixelW = 0;
    _pendingPixelH = 0;
    _metal.layer->setDrawableSize(CGSizeMake(_w, _h));
    RebuildFrameTargets();
    FireResizePostHook(_w, _h);
    LOG_DEBUG(Graphics, "Metal: drawable resized to {}x{}", _w, _h);
}

void EngineMetal::OnFullscreenChanged(bool windowed)
{
    _windowed = windowed;
}

bool EngineMetal::SwitchRes(int w, int h, int bpp)
{
    if (!_sdlWindow || w <= 0 || h <= 0)
        return false;

    _pixelSize = bpp;
    if (_windowed)
    {
        SDL_SetWindowSize(_sdlWindow, w, h);
        SDL_GetWindowSize(_sdlWindow, &_windowedRestoreW, &_windowedRestoreH);
    }
    else if (_windowMode == WindowMode::Fullscreen)
    {
        SDL_DisplayID display = SDL_GetDisplayForWindow(_sdlWindow);
        if (!display)
            display = SDL_GetPrimaryDisplay();
        if (!ApplyExclusiveDisplayMode(_sdlWindow, display, w, h, _refreshRate))
            return false;
        SDL_SetWindowFullscreen(_sdlWindow, true);
    }

    int pixelW = 0;
    int pixelH = 0;
    SDL_GetWindowSizeInPixels(_sdlWindow, &pixelW, &pixelH);
    OnWindowResized(pixelW, pixelH);
    return true;
}

bool EngineMetal::SwitchRefreshRate(int refresh)
{
    if (refresh <= 0)
        return false;
    _refreshRate = refresh;
    if (_windowMode == WindowMode::Fullscreen)
        return SwitchRes(_w, _h, _pixelSize);
    return true;
}

bool EngineMetal::SetWindowMode(WindowMode mode)
{
    if (!_sdlWindow)
        return false;

    if (_windowed && mode != WindowMode::Windowed)
        SDL_GetWindowSize(_sdlWindow, &_windowedRestoreW, &_windowedRestoreH);

    SDL_DisplayID display = SDL_GetDisplayForWindow(_sdlWindow);
    if (!display)
        display = SDL_GetPrimaryDisplay();

    int desktopW = 0;
    int desktopH = 0;
    int desktopRefresh = 0;
    ReadDisplayMode(SDL_GetDesktopDisplayMode(display), desktopW, desktopH, desktopRefresh);

    DisplayPlacementInput input;
    input.displayMode = mode == WindowMode::Fullscreen   ? "exclusive"
                        : mode == WindowMode::Borderless ? "borderless"
                                                         : "windowed";
    input.width = mode == WindowMode::Windowed && _windowedRestoreW > 0 ? _windowedRestoreW : _w;
    input.height = mode == WindowMode::Windowed && _windowedRestoreH > 0 ? _windowedRestoreH : _h;
    input.refresh = _refreshRate;
    const WindowPlacement placement = ResolveWindowPlacement(input, desktopW, desktopH, desktopRefresh);

    bool ok = true;
    if (mode == WindowMode::Windowed)
    {
        const bool clearedMode = SDL_SetWindowFullscreenMode(_sdlWindow, nullptr);
        const bool leftFullscreen = SDL_SetWindowFullscreen(_sdlWindow, false);
        ok = clearedMode && leftFullscreen;
        SDL_SetWindowBordered(_sdlWindow, true);
        SDL_SetWindowResizable(_sdlWindow, true);
        SDL_SetWindowSize(_sdlWindow, placement.width, placement.height);
        SDL_SetWindowPosition(_sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    else if (mode == WindowMode::Borderless)
    {
        const bool clearedMode = SDL_SetWindowFullscreenMode(_sdlWindow, nullptr);
        const bool enteredFullscreen = SDL_SetWindowFullscreen(_sdlWindow, true);
        ok = clearedMode && enteredFullscreen;
    }
    else
    {
        ok = ApplyExclusiveDisplayMode(_sdlWindow, display, placement.width, placement.height, placement.refreshHz);
        if (ok)
            ok = SDL_SetWindowFullscreen(_sdlWindow, true);
    }

    if (!ok)
    {
        SyncCachedWindowMode(_sdlWindow, _windowMode, _windowed);
        LOG_WARN(Graphics, "Metal: SetWindowMode failed: {}", SDL_GetError());
        return false;
    }

    _windowMode = mode;
    _windowed = mode == WindowMode::Windowed;
    int pixelW = 0;
    int pixelH = 0;
    SDL_GetWindowSizeInPixels(_sdlWindow, &pixelW, &pixelH);
    OnWindowResized(pixelW, pixelH);
    return true;
}

WindowMode EngineMetal::GetCurrentWindowMode() const
{
    return _sdlWindow ? _windowMode : WindowMode::Windowed;
}

bool EngineMetal::SetSwapInterval(int interval)
{
    if (!_metal.layer || (interval != -1 && interval != 0 && interval != 1))
        return false;

    static bool adaptiveLogged = false;
    if (interval == -1)
    {
        interval = 1;
        if (!adaptiveLogged)
        {
            LOG_INFO(Graphics, "Metal: adaptive vsync requested; CAMetalLayer maps it to vsync on");
            adaptiveLogged = true;
        }
    }

    _swapInterval = interval;
    SetDisplaySyncEnabled(_metal.layer, interval != 0);
    return true;
}

void EngineMetal::ListResolutions(FindArray<ResolutionInfo>& ret)
{
    ret.Clear();
    if (_windowed)
        return;

    SDL_DisplayID display = _sdlWindow ? SDL_GetDisplayForWindow(_sdlWindow) : SDL_GetPrimaryDisplay();
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
    if (!modes)
        return;

    for (int i = 0; i < count; ++i)
    {
        ResolutionInfo info{modes[i]->w, modes[i]->h, SDL_BITSPERPIXEL(modes[i]->format)};
        ret.AddUnique(info);
    }
    SDL_free(modes);
}

void EngineMetal::ListRefreshRates(FindArray<int>& ret)
{
    ret.Clear();
    if (_windowed)
    {
        ret.Add(0);
        return;
    }

    SDL_DisplayID display = _sdlWindow ? SDL_GetDisplayForWindow(_sdlWindow) : SDL_GetPrimaryDisplay();
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
    if (!modes)
        return;
    for (int i = 0; i < count; ++i)
    {
        if (modes[i]->w == _w && modes[i]->h == _h)
            ret.AddUnique(static_cast<int>(modes[i]->refresh_rate + 0.5f));
    }
    SDL_free(modes);
}

void EngineMetal::ListMonitors(FindArray<MonitorInfo>& ret)
{
    ret.Clear();
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays)
        return;
    for (int i = 0; i < count; ++i)
    {
        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(displays[i]);
        MonitorInfo info;
        info.index = i;
        info.name = SDL_GetDisplayName(displays[i]) ? SDL_GetDisplayName(displays[i]) : "Unknown";
        info.w = mode ? mode->w : 0;
        info.h = mode ? mode->h : 0;
        info.refresh = mode ? static_cast<int>(mode->refresh_rate + 0.5f) : 0;
        ret.Add(info);
    }
    SDL_free(displays);
}

int EngineMetal::GetCurrentMonitor() const
{
    if (!_sdlWindow)
        return 0;
    const SDL_DisplayID current = SDL_GetDisplayForWindow(_sdlWindow);
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    int result = 0;
    for (int i = 0; displays && i < count; ++i)
    {
        if (displays[i] == current)
        {
            result = i;
            break;
        }
    }
    SDL_free(displays);
    return result;
}

bool EngineMetal::SwitchMonitor(int idx)
{
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!_sdlWindow || !displays || idx < 0 || idx >= count)
    {
        SDL_free(displays);
        return false;
    }
    SDL_Rect bounds{};
    const bool found = SDL_GetDisplayBounds(displays[idx], &bounds);
    SDL_free(displays);
    if (!found)
        return false;

    int logicalW = 0;
    int logicalH = 0;
    SDL_GetWindowSize(_sdlWindow, &logicalW, &logicalH);
    return SDL_SetWindowPosition(_sdlWindow, bounds.x + (bounds.w - logicalW) / 2,
                                 bounds.y + (bounds.h - logicalH) / 2);
}

bool EngineMetal::GetDesktopDisplayMode(int& w, int& h, int& refresh) const
{
    SDL_DisplayID display = _sdlWindow ? SDL_GetDisplayForWindow(_sdlWindow) : SDL_GetPrimaryDisplay();
    return ReadDisplayMode(SDL_GetDesktopDisplayMode(display), w, h, refresh);
}

bool EngineMetal::GetCurrentDisplayMode(int& w, int& h, int& refresh) const
{
    SDL_DisplayID display = _sdlWindow ? SDL_GetDisplayForWindow(_sdlWindow) : SDL_GetPrimaryDisplay();
    return ReadDisplayMode(SDL_GetCurrentDisplayMode(display), w, h, refresh);
}

bool EngineMetal::GetRequestedFullscreenMode(int& w, int& h, int& refresh) const
{
    return _sdlWindow && ReadDisplayMode(SDL_GetWindowFullscreenMode(_sdlWindow), w, h, refresh);
}

} // namespace Poseidon
