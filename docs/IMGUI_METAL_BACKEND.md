# Dear ImGui dev-panel overlay on the Metal backend — Architecture & Implementation Plan

**Scope:** make the `--dev` dev panel (`engine/Poseidon/Dev/Debug/DebugOverlay.cpp`) work under
`PoseidonMetal`, by decoupling the overlay from OpenGL and writing a **custom Dear ImGui renderer
backend against metal-cpp**, routed through `EngineMetal`'s own pipeline/state machinery.

**Branch:** `metal-backend`. **Prerequisite reading:** `docs/METAL_BACKEND.md` (§0 boundaries, §6.1
lifecycle, §6.3 PSO key, §6.4 EncoderBroker, §6.5 capture source), `docs/brain/GOTCHAS.md`
(metal-cpp BOOL firewall; GL33-is-the-oracle; screenshot capture is unreliable), `CLAUDE.md`.

**Status of the superseded decision.** `docs/METAL_BACKEND.md:1395` (risk 4) proposed, for the
post-M6 timeframe, "vcpkg `imgui[metal-binding]` + SDL3 backend + the single permitted ObjC++ TU".
**This plan supersedes that proposal.** See §1 for why. `docs/brain/DECISIONS.md` must gain an entry
recording the change (see §0 ALWAYS).

> Authored 2026-07-26. Every library fact in §2 was verified against the **exact vendored sources on
> this machine** (imgui 1.92.8 at `/Users/cipery/vcpkg/buildtrees/imgui/src/v1.92.8-8e9c05eb59.clean/`,
> SDL3 headers under `build/macos-arm64-clang-rwdi/vcpkg_installed/arm64-osx-clang/include/SDL3/`)
> plus a context7 lookup of `/ocornut/imgui` `docs/BACKENDS.md`. Every engine claim cites `file:line`
> from the working tree. Items that could not be settled from source are flagged
> `⚠️ UNVERIFIED` or listed in §11.

---

## 0. Boundaries for implementers

- **ALWAYS**
  - Keep `--render gl33` byte-identical in behaviour. The dev panel under GL33 must look and behave
    exactly as it does today; the only permitted GL33 change is listed under NEVER below.
  - Keep the whole tree building with `CWR_HAS_METAL=ON` **and** `OFF`, and with
    `-DCWR_METAL_RUNTIME_SHADERS=ON` (the toolchain-less shader path — `engine/PoseidonMetal/CMakeLists.txt:82-107`).
  - `#include <PoseidonMetal/Private/MetalCppFirst.hpp>` must be **line 1** of every new
    `PoseidonMetal/*.cpp` (`engine/PoseidonMetal/Private/MetalCppFirst.hpp:3-5`), and `<imgui.h>`
    must be the **second** include, before any Poseidon header — see §8.3 (the `DebugLog` macro trap).
  - Every new `MTL::CommandBuffer` the overlay creates must go through `EngineMetal::AttachDiagnostics`
    before `commit()`, so `GetDebugErrorCount()` / `GetLastDebugMessage()` keep covering all GPU work
    (harness contract, `docs/METAL_BACKEND.md:1014-1021`).
  - New MSL entry points go into the existing `engine/PoseidonMetal/Shaders/PoseidonShaders.metal`
    (never a second library) and must be registered in **both** name tables at
    `engine/PoseidonMetal/EngineMetal_State.cpp:20-21` and `:27-29`.
  - Append to `docs/brain/DECISIONS.md` the decision recorded in §1 (custom metal-cpp imgui backend
    supersedes `METAL_BACKEND.md` risk 4), and to `docs/brain/GOTCHAS.md` any trap hit during the work.
  - After the final chunk, update `docs/METAL_BACKEND.md:1395` (risk 4) and
    `docs/brain/DECISIONS.md:41-46` ("dev-panel imgui overlay is still GL-only") so no stale claim survives.
- **ASK FIRST** (halt, append to §12 Questions)
  - Any change to `vcpkg.json`. **None is planned** — see §8.1. If you believe one is needed, stop.
  - Any change to the `Engine` virtual surface (`engine/Poseidon/Graphics/Core/Engine.hpp`).
    **None is planned** — see §4.2. If you believe one is needed, stop.
  - Adding `ImGui::Image()` / user textures to any tab (would extend the `ImTextureID` contract, §6.6).
- **NEVER**
  - Do not modify `PoseidonGL33` behaviour. Exactly **one** GL33-adjacent change is sanctioned by this
    plan: `engine/PoseidonGL33/EngineGL33.cpp:406` changes from
    `DebugOverlay::Init(_sdlWindow, _glContext);` to
    `DebugOverlay::Init(_sdlWindow, new Poseidon::Dev::OverlayRendererGL33(_glContext));`
    plus one `#include`, plus the two new files `OverlayRendererGL33.{hpp,cpp}` that carry the
    **verbatim** GL calls moved out of `DebugOverlay.cpp`. Nothing else in `PoseidonGL33` may change.
  - Do not allocate overlay vertex/index data from `FrameRing`. The ring is only live between
    `InitDraw`'s `BeginSpan` and `FinishDraw`'s `EndSpan` (`FrameRing.cpp:93` returns `{}` when
    `!_active`); the overlay draws in `NextFrame`, outside that span. See §6.4.
  - Do not render the overlay into the `CAMetalDrawable`. The drawable is `framebufferOnly = true`
    (`EngineMetal_Window.cpp:191`) and is acquired *after* the screenshot capture — drawing there
    would silently drop the overlay out of every capture, breaking GL33 parity (§7.1).
  - Do not touch `EncoderBroker`, `_current*`, `_sticky*` or any frame-encoder state. The overlay
    owns a private encoder on a private command buffer (§6.5). This is the whole point of the design.
  - Do not remove or reword the literal tokens `DevMode()` and `SetVisible(false)` from
    `DebugOverlay::NewFrame` / `SetVisible` / `ToggleVisible` — `tests/unit/engine/Poseidon/Dev/Debug/test_dev_mode_gates.cpp:91-102`
    greps the **source text** of those function bodies and will fail.
  - Do not commit, and do not run the full test suite as an iteration loop (§10).

---

## 1. Problem statement and the chosen approach

The dev panel is available only under GL33. `--render` now defaults to `auto`, which selects Metal on
macOS (`docs/brain/DECISIONS.md:41-46`), so in practice the panel is unreachable unless the user opts
into the slow reference backend. `EngineMetal::NextFrame` (`engine/PoseidonMetal/EngineMetal.cpp:287-364`)
has no overlay call at all, and `DebugOverlay` is hard-wired to OpenGL at
`DebugOverlay.cpp:1700` (`ImGui_ImplSDL3_InitForOpenGL`), `:1705` (`ImGui_ImplOpenGL3_Init`) and
`:1777` (`ImGui_ImplOpenGL3_RenderDrawData`).

**Decision (already taken; restated, not re-litigated):** write a **custom imgui renderer backend
against metal-cpp**, driven through `EngineMetal`'s own PSO cache, sampler set, texture registry and
capture-source plumbing.

Rationale:

1. **The firewall stays intact.** Locked decision #2 of the port is metal-cpp / pure C++17 / no
   Objective-C++, because Poseidon's `typedef int BOOL` (Memtype.h) collides with `objc/objc.h`
   (`docs/brain/GOTCHAS.md:42-46`). imgui's official backend is `imgui_impl_metal.mm` — Objective-C++
   only; the `IMGUI_IMPL_METAL_CPP` variant is still compiled as `.mm` and merely adds
   `__bridge` wrappers (`backends/imgui_impl_metal.mm:107-137`). Option 1 would have required enabling
   `OBJCXX` in the top-level `project()` and adding `imgui[metal-binding]` to `vcpkg.json`
   (a full vcpkg rebuild). **This plan needs neither** (§8.1).
2. **A foreign component must not mutate a shared encoder.** `ImGui_ImplMetal_SetupRenderState`
   unconditionally sets `setCullMode`, `setDepthStencilState`, `setViewport`, `setVertexBytes`,
   `setRenderPipelineState`, `setFragmentSamplerState`, `setVertexBuffer` and per-command
   `setScissorRect` / `setFragmentTexture` / `setVertexBufferOffset` on whatever encoder you hand it
   (`backends/imgui_impl_metal.mm:151-193, 268-297`), and restores nothing. `EngineMetal` owns its
   encoder through `EncoderBroker` with a sticky-state replay contract
   (`EngineMetal_State.cpp:745-782`). Leaked per-encoder state is exactly the defect class that
   produced two bugs on 2026-07-25 — `0052d08` (`ApplyScreenState` not resetting cull/frontFace/
   depth-clip) and `d3614d8` (leaked `setDepthBias`), see `docs/brain/GOTCHAS.md:72-86`. Option 1
   would have needed the same save/restore work anyway, so it saves less than it appears to.
3. **A backend written against the engine's own machinery inherits its parity fixes** — the PSO cache
   with its fail-loud miss warning, the eight canonical samplers, `HandleRegistry` generation checks,
   the capture-source resolution, and `AttachDiagnostics` GPU-error accounting.

**Bonus that falls out of the design:** because the overlay renders in `NextFrame` — after
`FinishDraw` has committed and closed the frame command buffer (`EngineMetal.cpp:269-281`) — it never
touches the frame encoder at all. The state-leak hazard is eliminated by *placement*, not merely by
ownership. What remains is genuine: we own ~450 lines of renderer code and must keep it working across
imgui version bumps (§11 R6).

**Known cost, accepted:** ~450 lines of new code we maintain, plus one MSL vertex/fragment pair.

---

## 2. Library facts (verified inline)

| Fact | Value | Source |
|---|---|---|
| imgui version in use | **1.92.8** (`IMGUI_VERSION_NUM 19280`) | `vcpkg_installed/arm64-osx-clang/include/imgui.h:32-33`; vcpkg listfile `imgui_1.92.8_arm64-osx-clang.list` |
| vcpkg imgui features | `sdl3-binding`, `sdl3-renderer-binding`, `opengl3-binding`, `freetype` — **no `metal-binding`** | `vcpkg.json:18-21` |
| imgui linkage | `imgui::imgui` linked **PUBLIC** to `Poseidon` → available transitively in `PoseidonMetal` | `engine/Poseidon/CMakeLists.txt:332-333`; `engine/PoseidonMetal/CMakeLists.txt:38-43` |
| Project `imconfig.h` / `IMGUI_USER_CONFIG` | **none** — stock defaults everywhere | repo-wide grep for `imconfig`, `IMGUI_USER_CONFIG`, `ImDrawIdx`, `IMGUI_OVERRIDE_DRAWVERT`: zero hits |
| `ImDrawIdx` | `unsigned short` (**16-bit**) — default, not overridden | `imgui.h:3157-3159` |
| `ImDrawVert` | `{ ImVec2 pos; ImVec2 uv; ImU32 col; }` → **stride 20**, offsets 0 / 8 / 16 | `imgui.h:3198-3203` |
| `ImDrawVert::col` byte order | `IM_COL32` = R,G,B,A in memory (`IMGUI_USE_BGRA_PACKED_COLOR` **not** defined) | `imgui.h:2944-2958`; repo grep: no define |
| `ImTextureID` | `typedef ImU64`; `ImTextureID_Invalid == 0` | `imgui.h:339-347` |
| Texture management API | 1.92 `ImGuiBackendFlags_RendererHasTextures` is **mandatory for dynamic font scaling**; backend implements `UpdateTexture(ImTextureData*)` over `WantCreate` / `WantUpdates` / `WantDestroy`, and must destroy all `ImGui::GetPlatformIO().Textures` at shutdown | `imgui.h:3483-3556`; `imgui.cpp:333-335`; context7 `/ocornut/imgui` `docs/BACKENDS.md` (2026-07-26); reference impl `backends/imgui_impl_metal.mm:332-383` |
| `ImTextureData` pixel format | always `ImTextureFormat_RGBA32` in practice; `GetPitch() == Width * BytesPerPixel`; `GetPixelsAt(x,y)` | `imgui.h:3486-3489, 3543-3547` |
| `ImDrawData` clip conversion | `clip_min = (ClipRect.xy - DisplayPos) * FramebufferScale`, `clip_max = (ClipRect.zw - DisplayPos) * FramebufferScale`; clamp to framebuffer; skip if degenerate or `ElemCount == 0` | context7 `/ocornut/imgui` `docs/BACKENDS.md`; `backends/imgui_impl_metal.mm:262-276` |
| `ImDrawData::FramebufferScale` | copied verbatim from `io.DisplayFramebufferScale` at `ImGui::Render()` | `imgui.cpp:5891` |
| SDL3 platform binding for a non-GL renderer | `ImGui_ImplSDL3_InitForMetal(SDL_Window*)` — **identical body** to `InitForOther`/`InitForVulkan`: `ImGui_ImplSDL3_Init(window, nullptr, nullptr)`. No `MTLDevice`, no Metal/ObjC headers. | `backends/imgui_impl_sdl3.h:34`; `backends/imgui_impl_sdl3.cpp:594-597, 609-612` |
| `imgui_impl_sdl3.cpp` includes | `imgui.h`, `imgui_impl_sdl3.h`, `<SDL3/SDL.h>`, `<stdio.h>`, `<TargetConditionals.h>` — **no ObjC, no Metal** | `backends/imgui_impl_sdl3.cpp:68-89` |
| `ImGui_ImplSDL3_NewFrame` HiDPI on Apple | sets `io.DisplayFramebufferScale = SDL_GetWindowDisplayScale(window)` (both axes) — the **content display scale**, *not* the pixel/logical ratio | `backends/imgui_impl_sdl3.cpp`, `ImGui_ImplSDL3_GetWindowSizeAndFramebufferScale` (`#if defined(__APPLE__)` branch) |
| `SDL_GetWindowDisplayScale` semantics | "content display scale relative to a window's pixel size … a combination of the window pixel density and the display content scale … corresponds to the scale display setting" — **not** `pixelsWide/logicalWide` | `SDL3/SDL_video.h:977-998` |
| `SDL_GetWindowPixelDensity` semantics | "ratio of pixel size to window size" — this **is** `pixelsWide/logicalWide` | `SDL3/SDL_video.h:958-975` |
| imgui reference alpha blend | RGB `SrcAlpha`/`OneMinusSrcAlpha`, alpha `One`/`OneMinusSrcAlpha`, op Add | `backends/imgui_impl_metal.mm:664-670`; GL3 equivalent `backends/imgui_impl_opengl3.cpp:339-340` |
| imgui reference render state | cull off, depth test off, scissor on, bilinear sampling | `backends/imgui_impl_opengl3.cpp:341-344`; `backends/imgui_impl_metal.mm:157-158`; context7 BACKENDS.md |
| Metal / MSL target | Metal 3, `-std=metal3.2`, macOS 15 minimum, arm64 | `engine/PoseidonMetal/CMakeLists.txt:119`; `docs/METAL_BACKEND.md:83-99` |
| Metal Toolchain on this machine | installed & verified (17C7003j) | `docs/HANDOFF.md:86` |

---

## 3. Current state, precisely

### 3.1 `DebugOverlay` is a free-function module, not a class

`engine/Poseidon/Dev/Debug/DebugOverlay.hpp` (57 lines) declares no class — it is
`namespace Poseidon::Dev::DebugOverlay` with 13 free functions and **zero `#include`s** (two forward
declarations only: `struct SDL_Window;` `:18`, `union SDL_Event;` `:19`). All state is file-static in
the `.cpp` (`DebugOverlay.cpp:76-113`). There is no owner, no accessor, no member anywhere in the
repo (grep for `_debugOverlay` / `GetDebugOverlay`: zero hits).

Public surface:

| Line | Signature | Backend-coupled? |
|---|---|---|
| 24 | `void Init(SDL_Window* window, void* glContext);` | **yes** — the one API-level GL leak |
| 25 | `void ProcessEvent(const SDL_Event& event);` | no |
| 32 | `void RequestDeferredReload(const char* modPath);` | no |
| 33 | `void NewFrame();` | **yes** (2 lines inside) |
| 34 | `void Render();` | **yes** (2 lines inside) |
| 35 | `void Shutdown();` | **yes** (1 line inside) |
| 37-39 | `bool IsVisible(); void SetVisible(bool); void ToggleVisible();` | no |
| 44 / 47 | `void SelectShadowsTab(); void SelectMemoryTab();` | no |
| 54-55 | `bool WantsKeyboard(); bool WantsMouse();` | no |

### 3.2 The GL coupling is 13 lines out of 1840

`DebugOverlay.cpp` is 1840 lines. GL-specific:

- Includes `:22-26` — `imgui.h`, `imgui_impl_sdl3.h`, **`imgui_impl_opengl3.h`**, `SDL3/SDL.h`, **`glad/gl.h`**.
- `:1700` `ImGui_ImplSDL3_InitForOpenGL(window, glContext)`
- `:1705` `ImGui_ImplOpenGL3_Init("#version 330")`
- `:1719` `ImGui_ImplOpenGL3_Shutdown()`
- `:1761` `ImGui_ImplOpenGL3_NewFrame()`
- `:1776` `glBindFramebuffer(GL_FRAMEBUFFER, 0)` — **the only `gl*` call in the whole file**
- `:1777` `ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData())`

Backend-**neutral** and reusable as-is: `ImGui_ImplSDL3_ProcessEvent` (`:1729`),
`ImGui_ImplSDL3_NewFrame` (`:1762`), `ImGui::CreateContext` (`:1694`),
`ImGui::NewFrame` / `Render` / `DestroyContext`, all thirteen tab-drawing functions
(~1150 lines, `:190-1684`) and every helper. The tabs reach the renderer only through
`GEngine`'s existing virtuals (`GetMsaaSamples`, `SetRenderScale`, `GetShadowMapTuning`, …), all of
which `EngineMetal` already overrides. There are **no** `static_cast<EngineGL33*>` casts in the tabs.

Notable: the panel loads **no fonts** — no `AddFontFromFileTTF`, no `io.Fonts->…`, no explicit atlas
build anywhere in the file. It runs on imgui's built-in ProggyClean atlas, created by the renderer
backend. The "Font" tab (`:190-252`) tunes the **game engine's** bitmap-font mapping, not imgui's.
The overlay also never sets `io.DisplaySize` / `io.DisplayFramebufferScale` / `ScaleAllSizes` — grep
returns zero hits; those come entirely from `ImGui_ImplSDL3_NewFrame()`.

Only `ImGuiConfigFlags_NavEnableKeyboard` is set (`:1696`); `io.IniFilename = nullptr` (`:1697`);
`ImGui::StyleColorsDark()` (`:1698`). No docking, no viewports.

### 3.3 Who drives it

| Site | Call |
|---|---|
| `engine/PoseidonGL33/EngineGL33.cpp:406` (ctor, after `SDL_GL_CreateContext`/`gladLoadGL`) | `DebugOverlay::Init(_sdlWindow, _glContext);` |
| `engine/PoseidonGL33/EngineGL33.cpp:476` (dtor, before `ShutdownGL()`) | `DebugOverlay::Shutdown();` |
| `engine/PoseidonGL33/EngineGL33_VertexBuffer.cpp:576-577` (inside `BackToFront()`, after `ResolveSSAAToDefault()` at `:571`, before `CaptureScreenshotIfPending()` at `:587` and `SDL_GL_SwapWindow` at `:590`) | `DebugOverlay::NewFrame(); DebugOverlay::Render();` |
| `engine/Poseidon/Graphics/Shared/SDLEventWindow.hpp:89` (shared, **already running under Metal**) | `DebugOverlay::ProcessEvent(event);` |
| `SDLEventWindow.hpp:103-104` | input swallowing via `WantsKeyboard()` / `WantsMouse()` |
| `GameStateExtTestGetters.cpp:273`, `GameStateExtTestShadow.cpp:216, :224` | trident verbs `IsVisible` / `SelectShadowsTab` / `SelectMemoryTab` |
| `apps/cwr/Game/GameApplication.cpp:861` | `RequestDeferredReload` (remount self-test only) |

The comment at `EngineGL33_VertexBuffer.cpp:573-575` states the intent that this plan must preserve:
*"ImGui composites on top of game + HUD. Render BEFORE the screenshot capture so trident captures
include the overlay… Both calls are safe no-ops when disabled."*

Under Metal, `ProcessEvent` is **already** called every frame via the shared `SDLEventWindow`
(`EngineMetal.hpp:74` → `_eventWindow.HandleEvents()`); it no-ops solely because `s_initialized` is
false (`DebugOverlay.cpp:1727-1728`). Note that `docs/METAL_BACKEND.md:1395` claims a "guard in
`DebugOverlay`" exists for Metal — **it does not**; the no-op is emergent.

### 3.4 There is no `Engine` seam

`Engine.hpp` (885 lines) has **zero** overlay/imgui-related virtuals (grep: one unrelated comment at
`:760`). GL33 hangs the overlay off its own private, non-virtual `BackToFront()`
(`EngineGL33.hpp:568`), called from its `NextFrame()` override (`EngineGL33_Lifecycle.cpp:220-224`).
`EngineMetal::NextFrame()` (`EngineMetal.cpp:287-364`) has no equivalent hook.

### 3.5 Structural wart worth fixing on the way past

`Poseidon.a` — nominally the backend-agnostic library — contains `DebugOverlay.o`, which references
`glBindFramebuffer` (resolved from glad, compiled only into `PoseidonGL33`,
`engine/PoseidonGL33/CMakeLists.txt:14`) and `ImGui_ImplOpenGL3_*`. A Metal-only executable that
dropped `PoseidonGL33` would fail to link. `DebugOverlay.cpp:26` is the **only** TU in `Poseidon`
that calls a GL function (the other `glad/gl.h` includes under `Graphics/Core/GL*.hpp` are
header-only constant tables). Moving the GL renderer into `PoseidonGL33` (§4.3) removes this.

---

## 4. The abstraction seam

### 4.1 Interface

New header `engine/Poseidon/Dev/Debug/OverlayRenderer.hpp`, namespace `Poseidon::Dev`. It forward-
declares `ImDrawData` and pulls in **no** imgui, GL or Metal header, so it is safe to include from
`Engine`-adjacent code and from a metal-cpp TU.

```cpp
#pragma once

struct ImDrawData;
struct SDL_Window;

namespace Poseidon::Dev
{
/// Renderer half of the ImGui dev overlay. One implementation per graphics
/// backend. The overlay owns the instance handed to DebugOverlay::Init and
/// deletes it in DebugOverlay::Shutdown.
///
/// Lifecycle, mirroring the ImGui backend contract:
///   Init(window)           — once, after the graphics device exists
///   NewFrame()             — once per frame, before ImGui_ImplSDL3_NewFrame()
///   CorrectDisplayMetrics()— once per frame, after ImGui_ImplSDL3_NewFrame()
///                            and before ImGui::NewFrame()
///   RenderDrawData(dd)     — once per frame, after ImGui::Render()
///   PrepareShutdown()      — owner-side guard before destruction
///   Shutdown()             — safe after successful or failed Init(); releases
///                            GPU resources, shuts down the SDL platform binding,
///                            and clears renderer-owned ImGui state
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

    /// False leaves the renderer and ImGui context alive until shutdown is safe.
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
```

`CorrectDisplayMetrics()` exists as a **separate** hook rather than folding the correction into
`NewFrame()` precisely so the GL33 call order stays byte-identical: today the order is
`ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();`
(`DebugOverlay.cpp:1761-1763`), and `io.DisplayFramebufferScale` is written by the SDL3 call. A single
hook would have forced a reorder. The default body is empty, so GL33 pays nothing.

### 4.2 Selection: constructed by the backend, not dispatched by the overlay

`DebugOverlay::Init` changes signature:

```cpp
// DebugOverlay.hpp
/// Takes ownership of `renderer`; deletes it in Shutdown(). Passing nullptr
/// leaves the overlay uninitialised (every entry point stays a no-op) — that
/// is the correct behaviour for backends with no overlay support.
void Init(SDL_Window* window, IOverlayRenderer* renderer);
```

Each backend constructs its own implementation at its existing `Init` call site. `DebugOverlay` passes
the same `SDL_Window*` through to `IOverlayRenderer::Init(window)`; the renderer stores it when it needs
it and owns the corresponding SDL platform-binding lifecycle. **No `Engine`
virtual is added** — `docs/METAL_BACKEND.md:56-58` makes reshaping the `Engine` surface an ASK-FIRST
item, and it is unnecessary here: both backends already call `DebugOverlay::Init` from concrete code
that knows its own type.

*Rejected alternatives:*
- `virtual IOverlayRenderer* Engine::CreateOverlayRenderer()` — an ASK-FIRST surface change, plus a
  virtual call from inside a constructor, for zero benefit.
- String dispatch on the backend code (`"gl33"` / `"metal"`) inside `DebugOverlay` — reintroduces the
  coupling we are removing, in a form the compiler cannot check.
- `dynamic_cast<EngineMetal*>` in `DebugOverlay` — the exact layering violation that
  `docs/METAL_BACKEND.md:368-384` removed from `FontDrawFreeType.cpp`.

### 4.3 Where the two implementations live

| Implementation | Location | Namespace | Why |
|---|---|---|---|
| `IOverlayRenderer` | `engine/Poseidon/Dev/Debug/OverlayRenderer.hpp` | `Poseidon::Dev` | Lives with its only consumer; `Graphics/Shared/` holds SDL/window/screenshot utilities, not Dev collaborators. Backends already include `Dev/Debug/DebugOverlay.hpp` (`EngineGL33.cpp:8`), so this direction is established. |
| `OverlayRendererGL33` | `engine/PoseidonGL33/OverlayRendererGL33.{hpp,cpp}` | `Poseidon::Dev` | Same namespace as the interface it implements — this class is Dev functionality that happens to need GL symbols. Physically in `PoseidonGL33` so `imgui_impl_opengl3.h` + `glad/gl.h` + the `glBindFramebuffer` call leave `Poseidon`, fixing §3.5. `PoseidonGL33/CMakeLists.txt:1-3` uses `GLOB_RECURSE *.cpp` — **no CMake edit needed.** |
| `OverlayRendererMetal`, `OverlayBufferRing` | `engine/PoseidonMetal/OverlayRendererMetal.{hpp,cpp}`, `OverlayRendererMetal_Textures.cpp`, `OverlayBufferRing.{hpp,cpp}` | `Poseidon::Metal` | Matches Metal-internal types such as `PipelineKey` (`PipelineKey.hpp:5`) and `EncoderBroker` (`EncoderBroker.hpp:5`). Other target types (`EngineMetal`, `TextBankMetal`) deliberately remain in `Poseidon`; this is not a target-wide namespace rule. Firewall: metal-cpp headers are `PRIVATE` to this target (`engine/PoseidonMetal/CMakeLists.txt:31-36`). Same `GLOB_RECURSE` — no CMake edit. The `_Textures` split mirrors the `EngineMetal_*.cpp` convention. |

The namespace asymmetry is deliberate and must not be "fixed": the interface and its GL33 sibling are
Dev-layer types; the Metal implementation is a Metal-internal type that happens to implement a
Dev-layer interface. It follows the namespace of its direct Metal collaborators, not a fictitious
target-wide convention.

`OverlayRendererGL33::RenderDrawData` must be a **verbatim** move:

```cpp
void OverlayRendererGL33::RenderDrawData(ImDrawData* drawData)
{
    // Make sure we draw to the default framebuffer in case the engine left
    // an FBO bound — happens with post-FX in GL33.  Other state (blend,
    // scissor, vao, depth) is saved/restored inside RenderDrawData.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
}
```

and `Init(SDL_Window* window)` is exactly
`ImGui_ImplSDL3_InitForOpenGL(window, _glContext) && ImGui_ImplOpenGL3_Init("#version 330")`
— note the **platform** init moves in with it, because `InitForOpenGL` is the GL-specific variant
(§8.2 explains why Metal uses `InitForMetal`). `DebugOverlay::Init` therefore calls
`renderer->Init(window)` where it used to call both `ImGui_Impl*_Init` functions, and keeps the
`s_window` assignment and the `ImGui::CreateContext()` / io-flag / style block unchanged.

### 4.4 Resulting `DebugOverlay` lifecycle (shape only — not the diff)

```
Init(window, renderer)      take `renderer` immediately into
                            `std::unique_ptr<IOverlayRenderer> candidate(renderer)`;
                            if (s_initialized || s_renderer) { LOG_ERROR; return; }
                            if (!candidate) { LOG_ERROR; return; }
                            IMGUI_CHECKVERSION(); ImGui::CreateContext();
                            io.ConfigFlags |= NavEnableKeyboard; io.IniFilename = nullptr;
                            StyleColorsDark();
                            if (!candidate->Init(window)) {
                                LOG_ERROR; candidate->Shutdown(); ImGui::DestroyContext();
                                s_window = nullptr; return;
                            }
                            s_window = window; s_renderer = candidate.release();
                            s_initialized = true; LOG_INFO("... renderer={}", s_renderer->Name())

ProcessEvent(e)             unchanged (ImGui_ImplSDL3_ProcessEvent + dev-gated hotkey)

NewFrame()                  unchanged guards, unchanged DevMode()/SetVisible(false) tokens,
                            unchanged io.MouseDrawCursor;
                            s_renderer->NewFrame();          // was ImGui_ImplOpenGL3_NewFrame()
                            ImGui_ImplSDL3_NewFrame();
                            s_renderer->CorrectDisplayMetrics();   // NEW, no-op on GL33
                            ImGui::NewFrame();
                            if (s_visible) DrawMainWindow();

Render()                    ImGui::Render();
                            s_renderer->RenderDrawData(ImGui::GetDrawData());
                            <deferred-action drain, unchanged>

Shutdown()                  if (s_renderer && !s_renderer->PrepareShutdown()) return;
                            if (s_renderer) { s_renderer->Shutdown(); delete s_renderer; }
                            s_renderer = nullptr;
                            if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
                            s_window = nullptr; s_initialized = false;
```

This makes initialization transactional. An already-initialized call logs and deletes the newly passed
owned renderer; a null renderer also logs. A failed renderer initialization always calls its idempotent
`Shutdown()` before the context is destroyed, so partial SDL binding, renderer flags/name, and registered
callbacks cannot escape. The renderer's `Shutdown()` is responsible for `ImGui_ImplSDL3_Shutdown()`;
`DebugOverlay` continues to own the context itself.

`DebugOverlay.cpp` loses `#include <imgui_impl_opengl3.h>` and `#include <glad/gl.h>` and gains
`#include <Poseidon/Dev/Debug/OverlayRenderer.hpp>`. It keeps `imgui.h`, `imgui_impl_sdl3.h`,
`SDL3/SDL.h` and the `#undef DebugLog` block at `:15-20`.

---

## 5. What an imgui 1.92.8 renderer backend must actually do

Canonical checklist (context7 `/ocornut/imgui` `docs/BACKENDS.md`, cross-checked against
`backends/imgui_impl_metal.mm`):

1. **At init:** set `io.BackendRendererName`, and
   - `io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset` — we can honour `ImDrawCmd::VtxOffset`,
     which is what lets 16-bit indices address meshes past 64 K vertices.
   - `io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures` — **required in 1.92**; without it
     imgui falls back to the legacy locked-atlas path and asserts on dynamic font scaling
     (`imgui_draw.cpp:2786, 5450`).
   `io.BackendRendererUserData` is unused here — our backend is an object, not a global.
2. **Texture management** (`ImDrawData::Textures`, which points at `ImGui::GetPlatformIO().Textures`):
   for each `ImTextureData*` with `Status != ImTextureStatus_OK`:
   - `WantCreate` → create a `Width × Height` RGBA8 texture from `tex->Pixels`
     (`GetPitch() == Width * BytesPerPixel`), then `SetTexID(id)` + `SetStatus(ImTextureStatus_OK)`.
   - `WantUpdates` → upload the dirty region. imgui offers both `tex->Updates[]` (a list) and
     `tex->UpdateRect` (their bounding box) and explicitly permits either (`imgui.h:3502-3504`).
     imgui guarantees these regions "have never been used before" (`imgui.h:3497`). Then `SetStatus(OK)`.
   - `WantDestroy && tex->UnusedFrames > 0` → destroy, `SetTexID(ImTextureID_Invalid)`,
     `BackendUserData = nullptr`, `SetStatus(ImTextureStatus_Destroyed)`.
   - **Never** write `TexID` / `Status` directly — always via `SetTexID` / `SetStatus`
     (`imgui.h:3549-3555`).
   - At shutdown, walk `ImGui::GetPlatformIO().Textures` and destroy every entry with `RefCount == 1`
     (`backends/imgui_impl_metal.mm:410-413`).
3. **Per-frame draw:**
   - `fb_width  = DisplaySize.x * FramebufferScale.x`, `fb_height = DisplaySize.y * FramebufferScale.y`.
     Bail if either `<= 0` or `CmdLists.Size == 0`.
   - Upload all `ImDrawList::VtxBuffer` / `IdxBuffer` contiguously
     (`TotalVtxCount * sizeof(ImDrawVert)`, `TotalIdxCount * sizeof(ImDrawIdx)`).
   - Set up: alpha blending on, **no** backface culling, **no** depth test/write, scissor on,
     **bilinear** sampling, viewport covering `(0,0)…(fb_width,fb_height)`, and an orthographic
     projection mapping `DisplayPos … DisplayPos+DisplaySize` to clip space:
     ```
     L = DisplayPos.x            R = DisplayPos.x + DisplaySize.x
     T = DisplayPos.y            B = DisplayPos.y + DisplaySize.y
     [ 2/(R-L)      0          0        0 ]
     [ 0            2/(T-B)    0        0 ]
     [ 0            0          1/(F-N)  0 ]
     [ (R+L)/(L-R)  (T+B)/(B-T) N/(F-N) 1 ]      with N=0, F=1
     ```
     (`backends/imgui_impl_metal.mm:174-187`. `2/(T-B)` is negative — that is the Y flip, and it is
     already correct for Metal's top-left framebuffer origin.)
   - For each command list, for each `ImDrawCmd`:
   - register a private no-op `ResetRenderStateSentinel` in
     `platform_io.DrawCallback_ResetRenderState` at init, clear it at shutdown (only if it is still
     ours), and set no sampler callbacks. If `UserCallback` equals that non-null sentinel, re-run
     setup; otherwise invoke it. (`ImDrawCallback` is stock — `imgui.h:3167-3170`.)
     - else: convert `ClipRect` (see §2 table), clamp to `[0, fb_width] × [0, fb_height]`, skip when
       degenerate or `ElemCount == 0`, apply as scissor; bind `pcmd->GetTexID()`; draw `ElemCount`
       indices starting at `IdxOffset`, with base vertex `VtxOffset`.
4. **Optional, deliberately skipped in v1:** `platform_io.DrawCallback_SetSamplerLinear` /
   `DrawCallback_SetSamplerNearest` (`imgui.h:4035-4036`). imgui only emits these when the backend
   registers them, and the only producer in the tree would be the demo window
   (`imgui_demo.cpp:897-901`), which the dev panel does not show. Leave them `NULL`. YAGNI.

Facts specific to this repo that drop out of the above:
- `ImDrawIdx` is **16-bit** → `MTL::IndexTypeUInt16`, index byte offset `IdxOffset * 2`.
- `DisplayPos` is always `(0,0)` here — no multi-viewport (`DebugOverlay.cpp` sets no
  `ImGuiConfigFlags_ViewportsEnable`). The subtraction is still written out, per the imgui guidance,
  because it costs nothing and documents the contract.
- The atlas is imgui's built-in ProggyClean (§3.2), so it is a single small RGBA32 texture; but
  because `RendererHasTextures` is set, imgui *may* create/destroy/grow it at any time, so the
  full `UpdateTexture` state machine is mandatory, not optional.

---

## 6. Mapping onto EngineMetal

### 6.1 Vertex layout — a new one is required

`ImDrawVert` is 20 bytes (`pos` float2 @0, `uv` float2 @8, `col` ImU32 @16). `TLVertex` is **40**
bytes (`pos` float3 @0, `rhw` float @12, `color` @16, `specular` @20, `t0` @24, `t1` @32 —
`EngineMetal_State.cpp:40-47`, `Graphics/Core/TLVertex.hpp:17-29`). They cannot share a descriptor,
and CPU-expanding every imgui vertex into a `TLVertex` would double the bandwidth and still not fit
(`vsScreen` expects pre-transformed positions with `rhw`, and `psNormal` carries a 9-tap cascade
shadow lookup and fog/night-eye math the overlay must not pay for —
`PoseidonShaders.metal:303-315`).

→ **Add `Metal::VertexLayout::ImGui`** (`PipelineKey.hpp:52-60`) with a builder alongside the three
existing ones (`EngineMetal_State.cpp:33-99`), bound at **`buffer(30)`** to match the screen-space
stream convention:

| Attribute | Format | Offset | Buffer |
|---|---|---|---|
| 0 `position` | `MTL::VertexFormatFloat2` | 0 | 30 |
| 1 `uv` | `MTL::VertexFormatFloat2` | 8 | 30 |
| 2 `color` | `MTL::VertexFormatUChar4Normalized` | 16 | 30 |

stride 20, `VertexStepFunctionPerVertex`, step rate 1.

> **Trap.** `ConfigureTLVertexDescriptor` uses `MTL::VertexFormatUChar4Normalized_**BGRA**`
> (`EngineMetal_State.cpp:43-44`) because Poseidon's `PackedColor` is 0xAARRGGBB. imgui's `ImU32` is
> `IM_COL32` = R,G,B,A in memory. Copying the `_BGRA` format here swaps red and blue on every
> widget. **Use plain `UChar4Normalized`.**

To keep `EngineMetal_State.cpp` free of imgui headers, declare a private POD in
`engine/PoseidonMetal/OverlayRendererMetal.hpp` (no imgui include there either — just the layout):

```cpp
namespace Poseidon::Metal
{
/// Byte-compatible mirror of ImDrawVert. The static_asserts that pin it to the
/// real thing live in OverlayRendererMetal.cpp, which does include <imgui.h>.
struct OverlayVertex
{
    float pos[2];
    float uv[2];
    unsigned char col[4];
};
} // namespace Poseidon::Metal
```

and in `OverlayRendererMetal.cpp`:

```cpp
static_assert(sizeof(ImDrawVert) == sizeof(Poseidon::Metal::OverlayVertex), "ImDrawVert layout drift");
static_assert(offsetof(ImDrawVert, pos) == 0,  "ImDrawVert::pos moved");
static_assert(offsetof(ImDrawVert, uv)  == 8,  "ImDrawVert::uv moved");
static_assert(offsetof(ImDrawVert, col) == 16, "ImDrawVert::col moved");
static_assert(sizeof(ImDrawIdx) == 2, "backend assumes 16-bit ImDrawIdx");
```

This is the fail-loud guard against an imgui bump silently changing the layout.

### 6.2 PSO key — new enum values only, **no width changes**

`PipelineKey::Make` packs 21 bits (`PipelineKey.hpp:74-85`) with these budgets
(`static_assert`s at `:88-92`):

| Field | Bits | Used today | After |
|---|---|---|---|
| `VertexStage` | 3 (max 8) | 6 | **7** (add `ImGui`) |
| `FragmentStage` | 4 (max 16) | 9 | **10** (add `ImGui`) |
| `VertexLayout` | 3 (max 8) | 5 | **6** (add `ImGui`) |
| `AttachmentConfig` | 2 (max 4) | 3 | 3 — reuse `PresentPass` |
| `PipelineBlend` | 2 (max 4) | 4 | 4 — reuse `AlphaBlend` |

Every `static_assert` still holds. Add the new enumerators **at the end** of each enum, before
`Count`, so no existing key value shifts.

The overlay's single key:

```cpp
const Metal::PipelineKey kOverlayKey = Metal::PipelineKey::Make(
    Metal::VertexStage::ImGui, Metal::FragmentStage::ImGui,
    Metal::PipelineBlend::AlphaBlend, /*colorWriteMask*/ 0xf,
    Metal::VertexLayout::ImGui, Metal::AttachmentConfig::PresentPass,
    /*sampleCountLog2*/ 0, /*alphaToCoverage*/ false);
```

- `AttachmentConfig::PresentPass` gives BGRA8 colour and **no depth/stencil attachment formats**
  (`EngineMetal_State.cpp:667-675`) — exactly what a post-resolve, window-sized overlay pass needs.
- `PipelineBlend::AlphaBlend` is `SrcAlpha / OneMinusSrcAlpha` for RGB and `One / **Zero**` for alpha
  (`EngineMetal_State.cpp:644-650`). imgui's reference uses `One / OneMinusSrcAlpha` for alpha. The
  RGB result is identical; only the destination alpha channel differs, and it is never observed:
  `CaptureScreenshotIfPending` converts BGRA→RGB and calls `ScreenshotWriter::WriteRGB`
  (`EngineMetal_Readback.cpp:284-292`), while SDL creates an opaque Metal layer because the window
  does not request `SDL_WINDOW_TRANSPARENT`. **Reuse `AlphaBlend`; do not add a blend mode.**
- `sampleCountLog2 = 0`: the capture source is always 1× (post-resolve). This also means the overlay
  PSO survives MSAA changes — `EngineMetal_Window.cpp:275-298` purges only `FramePass` PSOs.

**Prebuild it at overlay init** with `ResolvePipeline(kOverlayKey, /*logMiss*/ false)`
(`EngineMetal_State.cpp:560-571`), matching how `InitializePipelines` prebuilds
(`:497-557`). Building it lazily with `logMiss = true` would emit a spurious
`"PSO prebuild miss"` warning and violate the fail-loud contract.

### 6.3 Shaders

Add one pair to `engine/PoseidonMetal/Shaders/PoseidonShaders.metal` (no new file, no CMake change —
both the offline metallib rule and the `ExpandRuntimeShaders.cmake` inliner consume that single
source, `engine/PoseidonMetal/CMakeLists.txt:91-137`). The pair needs nothing from
`PoseidonShaderTypes.h`, so the expansion path is unaffected.

```metal
struct ImGuiVertexIn
{
    float2 position [[attribute(0)]];
    float2 uv       [[attribute(1)]];
    float4 color    [[attribute(2)]];
};

struct ImGuiVertexOut
{
    float4 position [[position]];
    float2 uv;
    float4 color;
};

vertex ImGuiVertexOut vsImGui(ImGuiVertexIn input [[stage_in]],
                              constant float4x4& projection [[buffer(0)]]);

fragment float4 psImGui(ImGuiVertexOut input [[stage_in]],
                        texture2d<float> atlas [[texture(0)]],
                        sampler atlasSampler   [[sampler(0)]]);
```

Bodies are the two-liners from `backends/imgui_impl_metal.mm:613-627`
(`projection * float4(in.position, 0, 1)`; `in.color * atlas.sample(sampler, in.uv)`).
`UChar4Normalized` performs the normalise+widen before the shader, so the MSL input is already a
`float4`; there is no `/ 255.0` conversion in shader code. This matches `ScreenVertexIn`'s normalised
colour input (`PoseidonShaders.metal:6-14`).

Register the names in the two tables:
`EngineMetal_State.cpp:20-21` → append `"vsImGui"`; `:27-29` → append `"psImGui"`.

Buffer/texture indices deliberately follow the engine's convention (constants at `buffer(0)`, vertex
stream at `buffer(30)`, `texture(0)` + `sampler(0)`), not imgui's reference layout.

### 6.4 Vertex/index buffers — **not** `FrameRing`

`FrameRing::Allocate` returns `{}` whenever `_active == false` (`FrameRing.cpp:93`), and `_active` is
set in `BeginSpan` (`:73`, inside `InitDraw`) and cleared in `EndSpan` (inside `FinishDraw`,
`EngineMetal.cpp:278`). The overlay draws in `NextFrame`, after that. Beyond the mechanical
impossibility, `docs/METAL_BACKEND.md:631-634` states the invariant explicitly: *"The present/readback
command buffers must not reference ring memory."*

→ New `engine/PoseidonMetal/OverlayBufferRing.{hpp,cpp}`, deliberately mirroring `FrameRing`'s proven
shape at a smaller scale:

```cpp
#pragma once
#include <PoseidonMetal/MetalFwd.hpp>
#include <dispatch/dispatch.h>
#include <array>
#include <cstddef>

namespace Poseidon::Metal
{
/// Triple-buffered shared-storage vertex/index arena for the dev-overlay pass.
/// Separate from FrameRing on purpose: the overlay encodes in NextFrame, outside
/// the InitDraw..FinishDraw span that owns FrameRing's slots.
class OverlayBufferRing
{
  public:
    static constexpr std::size_t kSlots = 3;

    struct Slot
    {
        MTL::Buffer* vertices = nullptr;
        MTL::Buffer* indices = nullptr;
    };

    bool Initialize(MTL::Device* device);
    void Shutdown();

    class Lease
    {
      public:
        Lease() = default;
        Lease(Lease&&) noexcept;
        Lease& operator=(Lease&&) noexcept;
        ~Lease() { Cancel(); }

        explicit operator bool() const;
        const Slot& Get() const;
        /// Transfers the permit return to this committed command buffer.
        void ReleaseOnCompletion(MTL::CommandBuffer* commandBuffer);
        /// Discards this uncommitted use: signals the permit and rewinds rotation.
        void Cancel();
    };

    /// Blocks until a slot is free, grows it to fit, and returns an owned lease.
    /// An empty lease means allocation failed and has already restored the ring.
    Lease Acquire(std::size_t vertexBytes, std::size_t indexBytes);

    /// Drains all permits; call before releasing the device.
    void WaitIdle();
};
} // namespace Poseidon::Metal
```

Semantics to implement:
- `dispatch_semaphore_create(kSlots)`; `Acquire` waits, advances the slot index exactly once, then
  grows either buffer by doubling until it fits (`MTL::ResourceStorageModeShared`, mirroring
  `FrameRing.cpp:108-121`). It does that only *after* the wait, when the selected slot's previous
  command buffer is complete, so its old `MTL::Buffer` may be released immediately.
- A valid lease owns the consumed permit and its rotation. Its terminal path is exactly one of:
  `ReleaseOnCompletion(cb)`, which attaches one completion handler that signals the permit, or
  `Cancel()`, which signals the permit **and rewinds the slot index**. `Acquire` calls `Cancel()`
  itself if growth/allocation fails. Thus the invariant is: one consumed permit advances exactly one
  slot; that rotation is either completed by one committed command buffer or cancelled and undone—no
  permit leak and no revisit of a still-in-flight slot. This is the ownership missing from a raw
  `Slot` return; `FrameRing::AbortSpan()` demonstrates the required permit return on cancellation
  (`FrameRing.cpp:141-147`).
- The lease destructor calls `Cancel()`, so every newly added render failure exit is mechanically
  covered until `ReleaseOnCompletion` has transferred ownership to the command-buffer handler.
- Initial sizes: 64 KB vertices / 16 KB indices is ample for this panel (a full 560×480 window with
  all tabs is a few thousand vertices) and grows on demand. Do not preallocate megabytes.

*Rejected:* a plain 3-deep rotation with no semaphore ("three frames is surely enough"). That is an
assumption, not a guarantee; the semaphore costs ten lines and makes it a proof.

### 6.5 Encoder acquisition and the state contract

**The overlay never touches the engine's encoder.** Sequence inside
`OverlayRendererMetal::RenderDrawData`:

1. `NS::AutoreleasePool` for the whole call.
2. If `drawData == nullptr`, `fb_width <= 0`, `fb_height <= 0`, or `drawData->CmdLists.Size == 0`,
   log the skip (`LOG_DEBUG` for the expected zero-list hidden-panel path) and return **before**
   servicing textures or allocating a command buffer. A never-opened panel therefore performs no
   texture GPU work.
3. Service texture requests (§6.6). Any texture failure records a diagnostic and skips this draw;
   it may commit only a successful upload command buffer.
4. Acquire the buffer-ring lease. An empty lease has already logged and restored the ring; return.
   Bind `const Slot& slot = lease.Get()` only after that success check.
5. `MTL::CommandBuffer* cb = _engine._metal.commandQueue->commandBuffer();` If null, record a
   diagnostic and return; the lease destructor cancels it.
6. `MTL::Texture* target = _engine.EncodeOverlayCaptureResolve(cb);` — see §7.2. `nullptr` records a
   diagnostic and returns. The uncommitted `cb` is discarded when the autorelease pool drains;
   it receives neither diagnostics nor `commit()`, even if a failed resolve encoded and ended work.
7. Build a fresh `MTL::RenderPassDescriptor`: colour attachment 0 = `target`,
   `LoadActionLoad` (the frame is already there), `StoreActionStore`. **No depth, no stencil.**
   A descriptor or encoder failure likewise records a diagnostic, ends any encoder if one exists,
   discards the uncommitted `cb`, and returns through the lease destructor.
8. Setup, once per pass and again when `UserCallback` equals the registered reset sentinel:
   - `enc->setRenderPipelineState(pso)` — the prebuilt `kOverlayKey` PSO.
   - `enc->setCullMode(MTL::CullModeNone)`.
   - **No `setDepthStencilState` at all.** The pass has no depth/stencil attachment, so any state is
     at best meaningless; `DepthMode::Disabled` would actively be wrong because its stencil pass op is
     `Replace` with write mask `0xff` (`EngineMetal_State.cpp:401-408`). The existing present pass
     sets no depth state either (`EngineMetal.cpp:325-340`) — follow it.
   - `enc->setViewport({0, 0, (double)target->width(), (double)target->height(), 0, 1})`.
   - `enc->setFragmentSamplerState(_engine._samplers[3], 0)` — index 3 is
     linear / ClampToEdge / ClampToEdge (`EngineMetal_State.cpp:442-456`: bit2 = point, bit1 = clampU,
     bit0 = clampV), which is imgui's required bilinear + clamp.
   - `enc->setVertexBytes(&ortho, sizeof(ortho), 0)` — 64 bytes, far under Metal's `setVertexBytes`
     limit; matches how the present/capture passes pass their parameters
     (`EngineMetal.cpp:337`, `EngineMetal_Readback.cpp:152`).
   - `enc->setVertexBuffer(slot.vertices, 0, 30)`.
9. Per `ImDrawCmd`: scissor (clamped against `target->width()/height()` — Metal raises a validation
   error on an out-of-bounds rect), `setFragmentTexture(resolved, 0)`,
   `setVertexBufferOffset(vtxBase + pcmd->VtxOffset * sizeof(ImDrawVert), 30)`,
   `drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, pcmd->ElemCount, MTL::IndexTypeUInt16,
   slot.indices, idxBase + pcmd->IdxOffset * sizeof(ImDrawIdx))`.
10. `enc->endEncoding();` → `lease.ReleaseOnCompletion(cb);` → `_engine.AttachDiagnostics(cb);` →
    `cb->commit();`. If an error occurs before this exact hand-off, it must be logged and return via
    the active lease; no failure may silently no-op.
11. `_engine.MarkOverlayCaptureCommitted();` immediately after `commit()` — **mandatory**, see §7.3.
    It makes the committed overlay target, including the scale-1 private copy, authoritative for the
    later present/readback; it must not run on a failed/uncommitted exit.

State the overlay must set: pipeline, cull, viewport, scissor, fragment sampler, fragment texture,
vertex buffer + offset, inline vertex bytes.
State the overlay must restore: **none.** The encoder is created and destroyed inside this function
and no engine state mirrors it. Do **not** write to `_currentPipeline`, `_currentScissor`,
`_stickyFragment*` or anything else — those describe the *frame* encoder, which is long gone.

Do **not** increment `Poseidon::gPerfDrawCalls` for overlay draws: GL33's imgui path does not either
(it goes through `ImGui_ImplOpenGL3_RenderDrawData`), and inflating the counter would corrupt A/B
perf traces.

### 6.6 Font atlas → `MTL::Texture`, and what `ImTextureID` carries

**`ImTextureID` carries a `HandleRegistry<MTL::Texture>::Handle` (`std::uint32_t`).**

- `HandleRegistry.hpp` encodes `{generation:8 << 24} | (slotIndex + 1)`, and **handle 0 is
  permanently invalid** — which lines up exactly with `ImTextureID_Invalid == 0` (`imgui.h:347`).
- It honours the port's standing rule from `docs/METAL_BACKEND.md:952-957`: *"never cast ids to
  pointers, never assume pointer-size handles."* A stale id resolves to `nullptr` instead of a
  dangling `MTL::Texture*`.
- Resolution per draw command is a vector index (`HandleRegistry::Resolve`) — negligible.
- It makes a future `ImGui::Image(tex->GetHandle())` of an engine texture free (still ASK FIRST, §0).
- Draw-time fallback mirrors the engine's own pattern (`EngineMetal_Queue.cpp:181-183`): resolve;
  on `nullptr`, bind `_engine._fallbackWhite[0]` and record a diagnostic once.

Store the same handle in `ImTextureData::BackendUserData` so `DestroyTexture` can assert
`(ImTextureID)(uintptr_t)BackendUserData == tex->TexID`, mirroring
`backends/imgui_impl_metal.mm:320-322`.

**Upload path: private texture + shared staging buffer + blit**, mirroring
`EngineMetal::UploadDynamicTexture` (`TextureMetal_Loading.cpp:362-418`) rather than inventing a new
idiom:

- `MTL::TextureDescriptor` 2D, `MTL::PixelFormatRGBA8Unorm` (imgui's `ImTextureFormat_RGBA32` is
  R,G,B,A byte order — **not** BGRA), `StorageModePrivate`, `TextureUsageShaderRead`, no mips.
- Staging: `newBuffer(layout.bytesPerImage, MTL::ResourceStorageModeShared)` where
  `layout = Metal::CopyLayout::Compute(Metal::CopyFormat::RGBA8, w, h)` — the central 256-byte row
  aligner (`CopyLayout.hpp`), same as every other upload in the backend. Copy row by row from
  `tex->GetPixelsAt(x, y)` with source pitch `tex->GetPitch()`.
- `blit->copyFromBuffer(staging, 0, layout.bytesPerRow, layout.bytesPerImage,
  MTL::Size::Make(w, h, 1), texture, 0, 0, MTL::Origin::Make(x, y, 0))`.
- `cb->addCompletedHandler([staging](MTL::CommandBuffer*) { staging->release(); });`
  `_engine.AttachDiagnostics(cb); cb->commit();`
- **Ordering:** the upload command buffer is created on `_engine._metal.commandQueue` — the same
  queue as the overlay draw command buffer — and committed **first**. Same-queue commit order is
  execution order, so no fences or events are needed. This is the identical argument
  `docs/METAL_BACKEND.md:1160-1166` makes for texture uploads.
- Use **`tex->UpdateRect`** (a single bounding box) rather than iterating `tex->Updates[]`: one
  staging buffer and one blit per texture per frame instead of N. Both are explicitly sanctioned
  (`imgui.h:3502-3504`).
- Registry: `handle = _engine._textureRegistry.Allocate(texture)` on create;
  `_engine._textureRegistry.Release(handle)` then `texture->release()` on destroy.

**Texture creation is completion-transactional; partial updates are submission-transactional.**
For `WantCreate`, do not set a `TexID`, `BackendUserData`, or `ImTextureStatus_OK` until texture
allocation, staging allocation, command-buffer/blit encoding, commit, and completion have all
succeeded. Wait for that small upload command buffer, check its status, then allocate the registry
handle; if `Allocate()` returns zero, release the texture and leave the request pending. For
`WantUpdates`, publish `OK` after successful submission without a CPU wait: the already-published
texture remains ordered before the overlay draw on the shared command queue, the completion handler
releases staging storage, and `AttachDiagnostics` still reports asynchronous GPU failures. On any
pre-submission allocation, encoder, or command-buffer failure, release every newly allocated staging
buffer/texture, record a diagnostic, leave the ImGui request in its retryable state, and continue
drawing other textures. `WantDestroy`
only releases a nonzero live handle, clears `TexID`/`BackendUserData`, and reports `Destroyed` after
that release succeeds. Track a renderer-local live-handle count (increment only after successful
registry allocation, decrement only after successful release) and require it to reach zero in the
shutdown sweep; `HandleRegistry` deliberately recycles slots and exposes no shrinking slot-count
contract (`HandleRegistry.hpp:60-93`).

*Rejected — `MTL::ResourceStorageModeShared` + `replaceRegion`* (what `imgui_impl_metal.mm:355` does):
fewer lines, but `replaceRegion` appears **nowhere** in `engine/PoseidonMetal/` (verified), so it
would introduce a second, unreviewed upload idiom and a CPU-writes-while-GPU-may-read pattern that
only imgui's "never-used-before regions" promise makes safe. Zero-tech-debt policy favours the
existing idiom.

*Rejected — `TextBankMetal::CreateDynamic` / `UpdateDynamic`* (`TextureBankMetal_Core.cpp:207-225`):
it allocates an engine `Texture` in the bank's slot table, participates in the LRU/VRAM budget
(`ReserveMemory` can *fail* under pressure, `TextureMetal_Loading.cpp:354`), and `UpdateDynamic` can
only re-upload a whole page — it cannot express imgui's partial-rect updates. Wrong tool.

### 6.7 Blend / depth / cull / scissor summary

| Requirement | How |
|---|---|
| Alpha blending | `PipelineBlend::AlphaBlend` baked into the PSO (§6.2) |
| No depth test, no depth write | `AttachmentConfig::PresentPass` has no depth attachment; no depth-stencil state is set |
| No stencil writes | same — and explicitly *not* `DepthMode::Disabled`, whose stencil op is `Replace`/`0xff` |
| No culling | `enc->setCullMode(MTL::CullModeNone)` |
| Scissor | `enc->setScissorRect(...)` per command, clamped to the target texture's dimensions |
| Bilinear + clamp sampling | `_engine._samplers[3]` |

---

## 7. Frame-lifecycle placement

### 7.1 Where GL33 puts it, and why that is the spec

`EngineGL33::BackToFront()` (`EngineGL33_VertexBuffer.cpp:565-596`):

```
571:    ResolveSSAAToDefault();          // SSAA → window-sized default framebuffer
576:    DebugOverlay::NewFrame();
577:    DebugOverlay::Render();          // overlay composites into the default FB
580:    GL33Bind::Invalidate();
581:    InvalidatePipelineCache();       // both GL caches must be dropped after ImGui
587:    CaptureScreenshotIfPending();    // screenshots are POST-overlay
590:    SDL_GL_SwapWindow(_sdlWindow);
```

`SamplePixel` / `SampleBackBufferNonBlack` run in the `FinishDraw`→`NextFrame` gap and are therefore
**pre-overlay**; `Screenshot` runs inside `NextFrame` and is **post-overlay**
(`docs/METAL_BACKEND.md:898-905`). The Metal port must preserve both.

### 7.2 Where it goes on Metal

The Metal capture source is defined by `EngineMetal::EncodeCaptureResolve`
(`EngineMetal_Readback.cpp:102-156`):

- if `ResolvedFrameColor()` (= `_frameResolveColor ? _frameResolveColor : _frameColor`,
  `EngineMetal_State.cpp:369-372`) is already window-sized → **that** texture;
- otherwise (render scale ≠ 1) → `_captureColor`, produced by a `BlitScale` downsample pass.

In both cases it is BGRA8, 1×, window-sized, `RenderTarget | ShaderRead`
(`EngineMetal_State.cpp:228-273`) — the exact analogue of GL33's post-`ResolveSSAAToDefault` default
framebuffer, and the *only* correct overlay target. Drawing into `_frameColor` under SSAA instead
would let the downsample scale the panel; drawing into the drawable would put it after the capture
**and** after `framebufferOnly = true` made the surface unreadable.

**Scale-1 invariant.** At `renderScale == 1`, `_captureColor` is not allocated
(`EngineMetal_State.cpp:261-272`), so `EncodeCaptureResolve()` returns the live resolved scene texture
directly when it is window-sized (`EngineMetal_Readback.cpp:102-108`). The overlay therefore writes
into that live scene target, not a copy. This is safe only when the frame is cleared before scene draw:
the overlay then replaces pixels from the current frame. It is **not** safe to rely on that when a
caller requests a non-clearing frame—`Engine::InitDraw` defaults `clear` to false
(`Engine.hpp:373`) and `EncoderBroker` loads instead of clearing when the frame does not need one
(`EncoderBroker.cpp:56-60`)—because old overlay pixels could persist. C4 must therefore add a
private, window-sized overlay-compositing capture texture for the scale-1 overlay path: blit/copy the
current resolved scene into it, render ImGui there, and make the later present/readback resolve select
that copy for this frame. It must be rebuilt with frame targets, cleared from selection at `InitDraw`,
and released with them. Normal scale-1 frames with no overlay still take the existing direct path.
This is the only way to keep a later non-clearing frame from inheriting overlay pixels that destroyed
scene data; clearing the live target after the fact would itself corrupt the scene. The pre-overlay
`SamplePixel` negative check is valid only for frames whose capture source has been freshly cleared;
it must not be used as a scale-1 non-clearing-frame oracle.

**Insertion point — `EngineMetal::NextFrame()` (`EngineMetal.cpp:287-364`), immediately before
`CaptureScreenshotIfPending()` at `:299`:**

```cpp
    NS::AutoreleasePool* pool = ...;                     // :292, unchanged

    // Mirrors EngineGL33::BackToFront (EngineGL33_VertexBuffer.cpp:576-577):
    // the overlay composites on top of game + HUD, before the capture, so
    // trident screenshots include it.
    Poseidon::Dev::DebugOverlay::NewFrame();
    Poseidon::Dev::DebugOverlay::Render();

    CaptureScreenshotIfPending();                        // :299, unchanged
    ...                                                   // :301-357, unchanged
```

and the two matching lifecycle hooks:

| Site | Addition | Mirrors |
|---|---|---|
| `EngineMetal.cpp:113-114` (end of ctor, after `_initialized = true; LoadConfig();`, before the final `LOG_INFO`/`pool->drain()`) | `DebugOverlay::Init(_sdlWindow, new Metal::OverlayRendererMetal(*this));` | `EngineGL33.cpp:406` |
| `EngineMetal.cpp` `~EngineMetal`: on the line **immediately after** `if (_initialized) SaveConfig();` and **immediately before** `ClearFontCache();` | `DebugOverlay::Shutdown();` | `EngineGL33.cpp:474-478` (`SaveConfig()` → `DebugOverlay::Shutdown()` → `ShutdownGL()`) |

Destructor ordering is load-bearing and this position satisfies all four constraints at once:
it is **after** `_frameRing.WaitIdle()` and the `commandQueue` `waitUntilCompleted` fence (so no
in-flight command buffer still references the overlay's textures or buffers), **after** `SaveConfig()`
(matching GL33's order exactly), **before** `DestroyM1Resources()` (which releases the samplers the
overlay borrows) and **before** `_metal.device->release()` (so every `MTL::Texture`/`MTL::Buffer`
release happens on a live device).

`ProcessEvent` needs **no** new call site — `SDLEventWindow.hpp:89` already runs under Metal (§3.3).

### 7.3 The `_captureResolvedThisFrame` trap (read this twice)

`EncodeCaptureResolve` short-circuits on `if (_captureResolvedThisFrame && !_frameOpen) return _captureColor;`
(`EngineMetal_Readback.cpp:115-116`). The flag is cleared in `InitDraw` (`EngineMetal.cpp:234`) and on
capture-target rebuild (`EngineMetal_State.cpp:303`), and set after a *committed* resolve by
`ReadCapture` (`:203-204`) and by the present path (`EngineMetal.cpp:344-345`).

If the overlay draws into `_captureColor` (render scale ≠ 1) and **fails** to set the flag, the very
next `EncodeCaptureResolve` — inside `CaptureScreenshotIfPending` → `ReadCapture`
(`EngineMetal_Readback.cpp:181`) — re-runs the downsample with `LoadActionDontCare`
(`:128`) and **erases the overlay**, and the same happens again for the present pass. The screenshot
would silently lose the panel and the panel would flicker or vanish on screen.

→ **Mandatory:** `EncodeOverlayCaptureResolve()` must select the normal `_captureColor` at non-unit
scale or the new scale-1 private copy, while `MarkOverlayCaptureCommitted()` marks that selected
texture authoritative only after the overlay CB is committed. `EncodeCaptureResolve()` must consult
that committed-overlay selection before its current same-size direct-return fast path. This preserves
the existing `_captureResolvedThisFrame` bookkeeping for the non-unit target and prevents a later
capture/present from either re-running the downsample or bypassing the scale-1 composited copy.
The scale-1 copy is allocated lazily when the visible overlay first needs it; it is not part of
`RebuildFrameTargets()` success. Allocation failure is diagnosed once, keeps the selection clear,
discards the uncommitted CB, and lets the ordinary present/readback resolve the scene instead.

Add a `GOTCHAS.md` entry for this once implemented.

---

## 8. Platform binding, build system, and the firewall

### 8.1 Build system: no production-target CMake edits, no vcpkg edits

| Concern | Status |
|---|---|
| `vcpkg.json` | **unchanged.** `metal-binding` is not needed — we do not use `imgui_impl_metal`. This is the single biggest build-side saving vs. option 1. |
| `engine/Poseidon/CMakeLists.txt` | **unchanged.** imgui is already `PUBLIC` on `Poseidon` (`:332-333`), so `PoseidonMetal` and `PoseidonGL33` both see it transitively. `Dev/Debug/*.cpp` is globbed (`:21`); the new `OverlayRenderer.hpp` is a header. |
| `engine/PoseidonGL33/CMakeLists.txt` | **unchanged** — `GLOB_RECURSE *.cpp` at `:1-3` picks up `OverlayRendererGL33.cpp`. (Optionally add the header to `POSEIDON_GL33_HEADERS` at `:5-9` for IDE grouping; not required.) |
| `engine/PoseidonMetal/CMakeLists.txt` | **unchanged for sources** — `GLOB_RECURSE *.cpp` at `:1-3`. **One optional edit:** add `OverlayRendererMetal.hpp` and `OverlayBufferRing.hpp` to the explicit `POSEIDON_METAL_HEADERS` list at `:5-16` to match the existing convention. |
| Shaders | **unchanged.** New MSL entry points go into the existing `PoseidonShaders.metal`; both the offline metallib rule (`:115-137`) and the runtime-source inliner (`:91-107`) pick them up automatically. |
| App targets | **unchanged.** All three executables already link `PoseidonMetal` and depend on `PoseidonMetalShaders` (`apps/cwr/Game/CMakeLists.txt:21-25`, `apps/cwr/GameDemo/CMakeLists.txt:23-27`, `apps/tetris/Tetris/CMakeLists.txt:25-29`). |

The one deliberate test-build edit is listed in §10: add the backend-neutral scissor test to the
explicit `PoseidonTests` source list. It does not alter a production target or the Metal linkage graph.

A `CONFIGURE_DEPENDS` glob still requires a re-run of CMake to notice new files; a plain
`cmake --build` on an existing tree will do it, but if a new `.cpp` appears not to compile, re-run
`cmake --preset macos-arm64-clang-rwdi` before investigating anything else.

### 8.2 SDL3 platform binding

Use `ImGui_ImplSDL3_InitForMetal(window)` in `OverlayRendererMetal::Init(SDL_Window* window)`.

- It is declared in `imgui_impl_sdl3.h:34` and its body is
  `return ImGui_ImplSDL3_Init(window, nullptr, nullptr);` (`imgui_impl_sdl3.cpp:594-597`) —
  byte-identical to `InitForOther` and `InitForVulkan`. It takes **no** `MTLDevice` and drags in **no**
  Metal or Objective-C header (`imgui_impl_sdl3.cpp:68-89`). Firewall-safe.
- Prefer it over `InitForOther` purely for intent; there is no functional difference.
- `ImGui_ImplSDL3_ProcessEvent` and `ImGui_ImplSDL3_NewFrame` stay in `DebugOverlay.cpp` unchanged.
  `ImGui_ImplSDL3_Shutdown` moves into each renderer's idempotent `Shutdown()` so a failed renderer
  initialization can unwind its own partial platform binding before `DebugOverlay` destroys the
  ImGui context.

**Metrics synchronization — the part that needs care.** There is **no pre-existing GL33 HiDPI bug**
to fix or investigate. The Cocoa video backend installs no `GetWindowContentScale` callback, so SDL
keeps `content_scale == 1.0f`; its display scale is therefore equal to pixel density on this platform.
GL33 rightly leaves the SDL3 backend's metrics alone.

Metal still needs a renderer-local override for a different reason: `EngineMetal::ApplyPendingResize()`
runs at the **end** of `NextFrame` (`EngineMetal.cpp:359-361`). On the resize frame,
`SDL_GetWindowSize()` already reports the new logical size while `_w`, `_h`, and the capture target
still describe the previous target. `ImGui_ImplSDL3_NewFrame()` alone would make clipping and the
viewport describe different surfaces. `CorrectDisplayMetrics()` therefore matches ImGui to the target
that this invocation will actually render into:

```cpp
void OverlayRendererMetal::CorrectDisplayMetrics()
{
    if (SDL_GetWindowFlags(_engine._sdlWindow) & SDL_WINDOW_MINIMIZED)
        return; // preserve imgui_impl_sdl3's minimized-window zero metrics

    int logicalW = 0, logicalH = 0;
    SDL_GetWindowSize(_engine._sdlWindow, &logicalW, &logicalH);
    if (logicalW <= 0 || logicalH <= 0)
    {
        _engine.RecordDiagnostic("overlay received non-positive non-minimized window metrics");
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)logicalW, (float)logicalH);
    io.DisplayFramebufferScale =
        ImVec2(logicalW > 0 ? (float)_engine._w / (float)logicalW : 1.0f,
               logicalH > 0 ? (float)_engine._h / (float)logicalH : 1.0f);
}
```

`_w` / `_h` are the engine's **pixel** sizes (`docs/METAL_BACKEND.md:412-413`), and the capture source
is `_w × _h` (`EngineMetal_Readback.cpp:107-110`), so by construction
`DisplaySize × FramebufferScale == target dimensions`. This is exactly what the non-Apple branch of
`ImGui_ImplSDL3_GetWindowSizeAndFramebufferScale` computes. Keeping `DisplaySize` in **logical** units
(rather than setting it to pixels with scale 1) preserves the widget size GL33 renders today.

The minimised-window guard is deliberately keyed on `SDL_WINDOW_MINIMIZED`, not a zero logical size:
`imgui_impl_sdl3` intentionally zeroes minimized metrics, and this override must not restore them.
This override is Metal-only; the GL33 renderer's default `CorrectDisplayMetrics()` remains empty
because GL33 is already correct.

### 8.3 Firewall rules for the new files

- `OverlayRendererMetal.cpp` / `OverlayRendererMetal_Textures.cpp` / `OverlayBufferRing.cpp`:
  line 1 **must** be `#include <PoseidonMetal/Private/MetalCppFirst.hpp>`
  (`Private/MetalCppFirst.hpp:3-5`; every existing `.cpp` in the directory obeys this).
- **Line 2 must be `#include <imgui.h>`**, before any Poseidon header. Reason: the engine's
  `Logging.hpp` `#define`s `DebugLog(...)`, which collides with the method `ImGui::DebugLog()` — the
  exact trap `DebugOverlay.cpp:15-20` already works around with `#ifdef DebugLog / #undef DebugLog`.
  `PoseidonMetal` has no PCH (verified: no `target_precompile_headers` in
  `engine/PoseidonMetal/CMakeLists.txt`), so include ordering is sufficient; add the same
  `#ifdef/#undef` block defensively anyway.
- `OverlayRendererGL33.cpp` is compiled with `PoseidonPCH.hpp` through the `PoseidonGL33` target
  (`CMakeLists.txt:236-249`), so it must retain the existing `DebugOverlay.cpp:15-20` workaround:
  `#undef DebugLog` before including `<imgui.h>`. Moving the GL calls must not reintroduce that
  macro collision.
- `OverlayBufferRing.hpp` may include `PoseidonMetal/MetalFwd.hpp`, `<dispatch/dispatch.h>` (as
  `FrameRing.hpp` already does), and standard headers—never `Metal.hpp`/`Foundation.hpp`/
  `QuartzCore.hpp` or `imgui.h`.
- `OverlayRendererMetal.hpp` must include the complete
  `Poseidon/Dev/Debug/OverlayRenderer.hpp` to derive from `IOverlayRenderer`, and
  `PoseidonMetal/OverlayBufferRing.hpp` to store the ring by value. Those project interface headers,
  `MetalFwd.hpp` via the ring, and standard headers are firewall-safe; it must not include a real
  metal-cpp or imgui header.
- If a header ever needs a metal-cpp type that is not yet forward-declared, add it to
  `MetalFwd.hpp` — do not include the real header. (Nothing in this plan requires a new one.)

### 8.4 Access from the overlay renderer into `EngineMetal`

`OverlayRendererMetal` needs `_metal.device`, `_metal.commandQueue`, `_samplers[3]`,
`_textureRegistry`, `_fallbackWhite[0]`, `_w`, `_h`, `_sdlWindow`, `_captureColor`,
`_captureResolvedThisFrame`, `ResolvePipeline`, `EncodeCaptureResolve`, `AttachDiagnostics`,
`RecordDiagnostic` — all private.

→ Before `namespace Poseidon`, forward-declare
`namespace Poseidon::Metal { class OverlayRendererMetal; }`, then add
`friend class Metal::OverlayRendererMetal;` next to the existing friends (`EngineMetal.hpp:24-29,
188-191`). A qualified friend declaration cannot introduce that class. This keeps the public `Engine`
surface untouched.

*Rejected:* a public `OverlayFrameContext` struct that `EngineMetal` fills and pushes into the
renderer. Cleaner on paper (DIP), but it forces `EngineMetal` to resolve the PSO and run
`EncodeCaptureResolve` on the overlay's behalf, spreading overlay logic across two files for no
testability gain (nothing here is unit-testable without a GPU).

---

## 9. File-by-file work breakdown

### 9.1 Files to create

| File | Responsibility |
|---|---|
| `engine/Poseidon/Dev/Debug/OverlayRenderer.hpp` | The `IOverlayRenderer` interface (§4.1). No imgui/GL/Metal includes. |
| `engine/PoseidonGL33/OverlayRendererGL33.hpp` | Declaration; holds the `void* _glContext`; `Init(SDL_Window*)` receives the platform window. |
| `engine/PoseidonGL33/OverlayRendererGL33.cpp` | Verbatim move of the four `ImGui_Impl*` GL calls + the one `glBindFramebuffer`. |
| `engine/PoseidonMetal/OverlayBufferRing.hpp` / `.cpp` | Triple-buffered shared vertex/index arena for the overlay pass (§6.4). |
| `engine/PoseidonMetal/OverlayRendererMetal.hpp` | Declaration + the `OverlayVertex` POD; includes the renderer interface and `OverlayBufferRing.hpp`, but no concrete metal-cpp/imgui header. |
| `engine/PoseidonMetal/OverlayRendererMetal.cpp` | Init/Shutdown, `CorrectDisplayMetrics`, the draw pass (§6.5), the `static_assert` block (§6.1). |
| `engine/PoseidonMetal/OverlayRendererMetal_Textures.cpp` | The `ImTextureData` state machine + staging-blit upload + registry bookkeeping (§6.6). |
| `engine/Poseidon/Dev/Debug/OverlayScissor.{hpp,cpp}` | Backend-neutral clip-rect PODs and `ComputeOverlayScissor`; no Metal or imgui dependency (§10). |

### 9.2 Files to modify

| File | Change |
|---|---|
| `engine/Poseidon/Dev/Debug/DebugOverlay.hpp` | `Init(SDL_Window*, IOverlayRenderer*)`; update the lifecycle doc comment at `:7-16` (it currently says "called from the GL33 engine" and "before `SDL_GL_SwapWindow`"). |
| `engine/Poseidon/Dev/Debug/DebugOverlay.cpp` | Drop `imgui_impl_opengl3.h` (`:24`) and `glad/gl.h` (`:26`); add `OverlayRenderer.hpp`, `<memory>`, and `s_renderer`; rewrite `Init` `:1687-1713`, `Shutdown` `:1715-1723`, `NewFrame` `:1750-1766`, `Render` `:1768-1790` as the transactional lifecycle in §4.4. **Preserve the `DevMode()` / `SetVisible(false)` literals.** |
| `engine/PoseidonGL33/EngineGL33.cpp` | `:406` construct `OverlayRendererGL33`; one `#include`. Nothing else. |
| `engine/PoseidonMetal/PipelineKey.hpp` | Append `ImGui` to `VertexStage`, `FragmentStage`, `VertexLayout` (before `Count`). |
| `engine/PoseidonMetal/EngineMetal_State.cpp` | `:20-21` + `:27-29` name tables; new `ConfigureImGuiVertexDescriptor`; extend the layout dispatch at `:613-632`; release/invalidate the lazy scale-1 private overlay capture texture with frame targets. |
| `engine/PoseidonMetal/Shaders/PoseidonShaders.metal` | Add `ImGuiVertexIn` / `ImGuiVertexOut` / `vsImGui` / `psImGui`. |
| `engine/PoseidonMetal/EngineMetal.hpp` | Forward-declare `Poseidon::Metal::OverlayRendererMetal` before `namespace Poseidon`, then add `friend class Metal::OverlayRendererMetal;` next to `:188-191`; declare the private overlay-resolve/commit helpers and per-frame selection state. |
| `engine/PoseidonMetal/EngineMetal_Readback.cpp` | Implement `EncodeOverlayCaptureResolve` / `MarkOverlayCaptureCommitted` and make `EncodeCaptureResolve` prefer the committed overlay target before its direct same-size return. |
| `engine/PoseidonMetal/EngineMetal.cpp` | ctor after `:113-114` → `DebugOverlay::Init(...)`; dtor after `:144-145` → `DebugOverlay::Shutdown()`; `NextFrame` before `:299` → `DebugOverlay::NewFrame(); DebugOverlay::Render();`. |
| `engine/PoseidonMetal/CMakeLists.txt` | *(optional, convention only)* add the two new headers to `POSEIDON_METAL_HEADERS` `:5-16`. |
| `docs/METAL_BACKEND.md` | Rewrite risk-table row 4 (`:1395`) and the M5 note (`:1366-1369`) to point at this document. |
| `docs/brain/DECISIONS.md` | New entry: custom metal-cpp imgui backend chosen over `imgui[metal-binding]` + ObjC++ TU. |
| `docs/brain/NORTH_STAR.md` | Update the status once the panel works on Metal. |

### 9.3 Chunks

**C0 — Foundation: the seam (blocking; nothing else may start until it lands).**
Creates `OverlayRenderer.hpp`, `OverlayRendererGL33.{hpp,cpp}`; refactors `DebugOverlay.{hpp,cpp}`;
one line in `EngineGL33.cpp`.
*Completion criterion:* build green; `PoseidonTests` at the known baseline (2352/2353);
`--render gl33 --dev` opens the panel with Ctrl+` and every tab behaves as before; `nm` on
`libPoseidon.a` shows no undefined `glBindFramebuffer` / `ImGui_ImplOpenGL3_*`.

**C1 — Metal lifecycle diagnostic (after C0; throwaway, never committed).**
Locally wire an `OverlayRendererMetal` only far enough to validate ownership and call ordering, then
discard it. It must not set `ImGuiBackendFlags_RendererHasTextures`, advertise a renderer capability,
or land in a shared branch: a command-counting renderer that claims texture support would violate the
1.92 texture contract. Record the diagnostic result in the implementation notes, then remove the
temporary wiring before C2/C3 start.

**C2 — Shader + PSO + vertex layout** (parallel with C3; no independently wired backend).
`PipelineKey.hpp`, the two name tables, `ConfigureImGuiVertexDescriptor`, the MSL pair, and the
`ResolvePipeline(kOverlayKey, false)` prebuild in `OverlayRendererMetal::Init`.
*Completion criterion (completed at C4 integration):* metallib compiles
(`cmake --build … --target PoseidonMetalShaders`); an `-DCWR_METAL_RUNTIME_SHADERS=ON` configure also
builds and the game starts with the `"runtime-compiled pre-expanded shader source"` warning; `Init`
logs that the overlay PSO was built and **no** `"PSO prebuild miss"` warning appears anywhere in a
full mission run.

**C3 — Texture manager** (parallel with C2; no independently wired backend).
`OverlayRendererMetal_Textures.cpp`: the `WantCreate`/`WantUpdates`/`WantDestroy` state machine,
staging-blit upload, registry allocate/release, shutdown sweep of
`ImGui::GetPlatformIO().Textures`.
*Completion criterion:* creation, replacement/rebake, and destruction each log their outcome; every
live ImGui `TexID` resolves before draw; every successful create has one matching release; the
renderer-local live-handle count reaches zero after shutdown. `MTL_DEBUG_LAYER=1` is clean and
Instruments shows no growth over 200 toggles. Do not require exactly one create (font rebakes may
create replacements), and do not use `HandleRegistry` slot-vector size as a leak oracle.

**C4 — Draw path** (needs C2 + C3).
`OverlayBufferRing`, the ortho matrix, the encoder pass, the per-command loop,
`_captureResolvedThisFrame` bookkeeping, scale-1 private capture copy and its selection state,
reset-state sentinel, and the real Metal lifecycle wiring.
This is the first landing chunk that registers `RendererHasTextures`; it lands only with texture
servicing and the draw path.
*Completion criterion:* the panel is visible and correct on Metal at render scale 1 and 1.5, MSAA 1×
and 4×; scissor clipping is correct when a tab's content scrolls; `MTL_DEBUG_LAYER=1` clean; and
at scale 1, a non-clearing frame cannot retain pixels from a previously visible overlay. The
`SamplePixel` negative check applies only to freshly-cleared capture-source frames (§7.2), where it
must remain unchanged in the `FinishDraw`→`NextFrame` gap.

**C5 — HiDPI + parity + docs.**
`CorrectDisplayMetrics`, the screenshot-ordering check, `docs/METAL_BACKEND.md` /
`docs/brain/*` updates.
*Completion criterion:* the panel occupies the same fraction of the window under Metal as under GL33
on the same display, with `nativePixelDensity` both off and on; a screenshot taken with the panel
open contains the panel; the three docs no longer claim the overlay is GL-only.

Order: **C0 → C1 (discard) → {C2 ∥ C3} → C4 → C5.**

---

## 10. Verification plan

**Risk zone.** Per `~/.claude/CLAUDE.md`'s trapezoid strategy: this is renderer-integration and UI
glue — the "skip unit tests" zone for the drawing code (it is untestable without a GPU and would only
encode the author's assumptions), and the **integration / manual-visual** zone for everything that
matters. The only genuinely pure logic worth pinning is the clip-rect conversion arithmetic.

**Required tests.**
- The existing `tests/unit/engine/Poseidon/Dev/Debug/test_dev_mode_gates.cpp` must keep passing
  **unmodified**. It greps the source text of `DebugOverlay::SetVisible` / `ToggleVisible` /
  `NewFrame` (`:91-102`); it is the regression guard on the dev-mode gating and it will catch a
  careless refactor of exactly the functions this plan touches.
- **One new unit test** (C4), `tests/unit/engine/Poseidon/Dev/Debug/test_overlay_scissor.cpp`, over a
  backend-neutral `Poseidon::Dev::ComputeOverlayScissor`. Put that helper and its POD input/output
  types in `OverlayScissor.{hpp,cpp}` under `engine/Poseidon/Dev/Debug/`; the Metal renderer converts
  `ImVec4`/`ImVec2` to those PODs at its boundary and converts the result to `MTL::ScissorRect` there.
  The test therefore links the existing `Poseidon` target, not `PoseidonMetal`, and tests:
  fully-inside rect passes through; negative origin clamps to 0; far edge clamps to target; degenerate
  (`max <= min`) returns false; fractional extents match `imgui_impl_metal` truncation; sub-pixel
  extents collapse; and nonzero `DisplayPos` composes with `FramebufferScale == 2`. Add the test source explicitly
  to `POSEIDON_TEST_SOURCES` in `tests/unit/engine/Poseidon/CMakeLists.txt:124-145`; that list is not
  globbed. *Intent:* this is the one piece of arithmetic that (a) is pure, (b) has a non-obvious clamp
  contract, and (c) crashes Metal validation when wrong. Nothing else in this work qualifies.
- No other new unit tests.

**Forbidden patterns.** No tests asserting that a mock encoder received `setScissorRect`
(mock-presence assertions on implementation details). No retroactive coverage filler over
`OverlayRendererMetal`. No test that instantiates `EngineMetal`. No "the panel renders the string
'Cheats'" widget assertions.

**Run scope during iteration.** Build the single target you touched
(`cmake --build build/macos-arm64-clang-rwdi --target PoseidonMetal -- -k 0`), and run only
`./build/macos-arm64-clang-rwdi/tests/unit/engine/Poseidon/PoseidonTests "[dev]"` plus the new
scissor test's tag. The **full** `PoseidonTests` run (baseline 2352/2353 — the one failure needs the
absent `packages/Demo` data, `docs/HANDOFF.md:8`) is a hand-off gate, not an iteration loop.

**Build.** `VCPKG_ROOT=~/vcpkg cmake --preset macos-arm64-clang-rwdi` then
`cmake --build build/macos-arm64-clang-rwdi -- -k 0`. Note the trap from `docs/brain/GOTCHAS.md:88-94`:
`-k 0` is a **ninja** flag and must come after `--`; judge the build by the process exit code (zsh:
lowercase `pipestatus`), not by grepping a truncated log. Also configure once with
`-DCWR_METAL_RUNTIME_SHADERS=ON` in a throwaway build dir to prove the self-contained-source path
still compiles (C2).

**Metal-disabled compatibility.** In a separate throwaway build directory, configure with
`-DCWR_HAS_METAL=OFF`, build the GL33 game target, and perform the final `--render gl33 --dev` panel
smoke. `CWR_HAS_METAL` gates the `PoseidonMetal` subdirectory (`CMakeLists.txt:147,197-199`); this
check proves the new common overlay seam has not made the non-Metal configuration depend on it.

**Running the game (this machine).**
```
cd /Users/cipery/source/jednohubky/CWR/packages/Remaster
../../dist/macos-arm64-clang-rwdi/PoseidonGame --dev --render metal   ... > /tmp/run.log 2>&1
```
`dist/macos-arm64-clang-rwdi/PoseidonGame` — **not** the `build/` copy; the metallib is staged next to
the dist binary and CWD must be `packages/Remaster` for fonts/textures
(`docs/brain/GOTCHAS.md:96-101`). **Always confirm `Metal: loaded .../PoseidonShaders.metallib` in the
log** — the game silently falls back to GL33 when the metallib is missing, and you would be testing
the wrong backend. Always `pkill PoseidonGame` afterwards.

**Validation layer.** `MTL_DEBUG_LAYER=1` on every chunk's run; capture both stdout and stderr to a
log and fail the pass on any Metal validation diagnostic, not only a nonzero
`GetDebugErrorCount()`. `AttachDiagnostics` records completed command buffers only when their status
is `Error` (`EngineMetal.cpp:458-475`); it is useful but cannot see synchronous or warning-only
validation output. Additionally assert `triGetGLErrorCount() == 0` via the harness. A validation
error here is the expected failure mode for: an out-of-bounds scissor, a depth-stencil state on a
pass with no depth attachment, and a PSO whose attachment config does not match the pass.

**Visual checks — and the screenshot problem.** `docs/brain/GOTCHAS.md:103-115` records that
`--auto-screenshot`, `--test-type screenshot` and the harness `screenshot` verb all fail
intermittently and **silently**, and that a real gameplay PNG is ~800 KB–1.5 MB while ~34 KB means a
black or shutdown frame. This plan therefore does **not** gate on automated capture. Instead:

1. **Primary: human visual verification.** C4 and C5 each end with a short scripted manual
   pass the user performs (or the implementer performs, on the user's machine): launch with `--dev`,
   Ctrl+`, walk the seven tabs, resize the window, toggle MSAA and render scale from the Render tab,
   toggle the panel 20×. This is how the two 2026-07-25 defects were actually confirmed
   (`docs/brain/NORTH_STAR.md:33-37`) and it is the honest primary gate.
2. **Secondary: a capture check that fails loudly.** For the "screenshots contain the overlay" claim
   (C5), take three captures in one session and accept the result **only** if at least two produced a
   PNG larger than 500 KB *and* both contain the panel. Record the PNG byte size in the verification
   note. Never treat a single successful capture as evidence, and never treat a missing file as a
   failure of *this* work without first re-running.
3. **Tertiary: a cheap in-engine assertion that needs no PNG.** On a **freshly-cleared** capture-source
   frame at a non-unit render scale, with the panel open at a known position, call the existing
   `SampleBackBufferNonBlack()` / `SamplePixel(x, y)` harness verbs *after* `NextFrame` has run at
   least once. They sample in the `FinishDraw`→`NextFrame` gap, i.e. pre-overlay by contract (§7.1),
   so they must be unchanged whether the panel is open or closed. Do not use this as the scale-1,
   non-clearing-frame oracle: that case instead verifies the dedicated compositing-copy path required
   by §7.2.
4. If the overlay ever needs a positive automated pixel check, the correct hook is a new debug-only
   trident verb that calls `FlushPendingScreenshot()` (`EngineMetal_Readback.cpp:268-271`) *after* the
   overlay pass and reports the byte size — but that is out of scope here and the flaky-capture
   investigation should be closed on its own first (`docs/brain/NORTH_STAR.md:40-42`).

**Perf.** Confirm with `--perf-trace` that frame time with the panel **closed** is within noise of
the pre-change baseline (the hidden-panel path must early-out before any GPU work — §6.5 step 3), and
that `gPerfDrawCalls` is unchanged with the panel open (§6.5).

---

## 11. Risks and open questions, ranked

| # | Risk / question | Mitigation / the investigation that closes it |
|---|---|---|
| **R2** | **`_captureResolvedThisFrame` erasure** (§7.3) — forgetting the flag silently drops the overlay from screenshots and makes it flicker at render scale ≠ 1. | Explicit step 11 in §6.5; verify at C4 by running at `--renderScale 1.5` with the panel open for 60 s and watching for flicker; add a `GOTCHAS.md` entry. |
| **R3** | **Vertex colour channel swap** — copying `UChar4Normalized_BGRA` from `ConfigureTLVertexDescriptor` (`EngineMetal_State.cpp:43-44`) into the imgui descriptor. Symptom: the whole panel is blue-tinted where it should be red-ish and vice versa; easy to mistake for a style issue. | Called out in §6.1; verify visually against a GL33 side-by-side of the same tab at C4. The dark theme's accent colours make it obvious once you look for it. |
| **R4** | **Overlay command buffer ordering vs. capture.** The overlay CB must be committed *before* `CaptureScreenshotIfPending()` submits its readback CB, or the capture reads pre-overlay pixels. Both are on `_metal.commandQueue`, so commit order is execution order — but only if the overlay actually commits rather than deferring. | §6.5 step 10 commits inside `RenderDrawData`, and §7.2 places the whole `DebugOverlay::Render()` call before `:299`. Verify at C5 with the capture check (§10 item 2). |
| **R5** | **Buffer-ring lifetime.** Reusing an overlay vertex buffer while its previous command buffer is still executing corrupts geometry in a way that looks like random garbage triangles — and is timing-dependent, so it may not reproduce for hours. | The `dispatch_semaphore(3)` in `OverlayBufferRing` (§6.4) makes it a proof rather than an assumption. Stress at C4 by forcing a 1-slot ring in a scratch build and confirming it still renders correctly (just slower). |
| **R6** | **imgui version bumps.** We own this backend; a 1.93 change to `ImDrawVert`, `ImDrawIdx`, or the texture state machine breaks it. | The `static_assert` block (§6.1) turns a layout change into a compile error. The `ImTextureStatus` switch must have no `default:` case, so a new enumerator is a `-Wswitch` warning. Record in `GOTCHAS.md` that `vcpkg.json`'s imgui version is load-bearing for `PoseidonMetal`. |
| **R7** | **Init-order in the `EngineMetal` constructor.** `DebugOverlay::Init` prebuilds a PSO, so it must run after `LoadShaderLibrary()` (`EngineMetal.cpp:101`) and `InitializeM1Resources()` (`:107`), which create `_metal.shaderLibrary` and `_samplers`. Every early-return path in the ctor (`:72, 79, 87, 93, 99, 104, 110`) must **not** leave a half-initialised overlay. | Place the call after `_initialized = true; LoadConfig();` (`:113-114`) — i.e. only on the fully-successful path, exactly where GL33 puts it relative to its own init. Verify by forcing `LoadShaderLibrary` to fail in a scratch build and confirming a clean fatal exit. |
| **R8** | **Destructor ordering.** `DebugOverlay::Shutdown()` releases `MTL::Texture`s and `MTL::Buffer`s; running it after `_metal.device->release()` is a use-after-free, running it before the queue fence risks releasing resources still referenced by in-flight work. | §7.2: place it after the existing `waitUntilCompleted` fence (`EngineMetal.cpp:130-142`) and before `DestroyM1Resources()`. Verify with `MTL_DEBUG_LAYER=1` + a clean quit; Instruments Leaks over an open/close cycle. |
| **R9** | **`ImGui::Render()` cost when the panel is hidden.** The overlay context is created unconditionally (GL33 parity, `EngineGL33.cpp:406` is not dev-gated), so `ImGui_ImplSDL3_NewFrame` + `ImGui::NewFrame` + `ImGui::Render` run every frame even in non-dev runs. | This is exactly what GL33 pays today, so it is parity, not a regression — but confirm with `--perf-trace` at C5 that the Metal frame time is within noise of the pre-change baseline. If it is not, gate `DebugOverlay::Init` on `AppConfig::Instance().DevMode()` — which would be a **deviation from GL33** and needs a `DECISIONS.md` entry. |
| **R10** | **Reset-render-state callbacks need a stable identity.** Comparing a callback to a null platform field creates dead code. | Register the private no-op sentinel at init, compare against it in the draw loop, and clear it on shutdown only if it still identifies this renderer (§5 item 3). |
| **R11** | ⚠️ **UNVERIFIED:** whether `MTL::RenderCommandEncoder` on a pass with **no** depth/stencil attachment tolerates having no `setDepthStencilState` call at all across a whole pass. | The existing present pass (`EngineMetal.cpp:325-340`) and capture-resolve pass (`EngineMetal_Readback.cpp:131-154`) already do exactly this and run clean under `MTL_DEBUG_LAYER=1` today, which is strong evidence — but confirm explicitly at C4 with the debug layer on. |
| **R12** | **Studio's independent imgui integration.** `apps/tools/Studio/main.cpp:22-23` uses `imgui_impl_sdlrenderer3` with its own context. Nothing in this plan touches it, but a careless change to `vcpkg.json` features would. | §8.1 keeps `vcpkg.json` unchanged. If a future change is proposed there, check `apps/tools/Studio` first. |

---

## 12. Questions for Architect (filled by implementer)

*(empty — append here and halt if a plan ambiguity blocks execution)*
