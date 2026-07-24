# Native Metal Backend (`PoseidonMetal`) — Architecture & Implementation Plan

**Scope:** native Metal renderer backend for the CWR engine on macOS arm64 (Apple Silicon),
replacing Apple's GL→Metal translation layer as the primary renderer while keeping
`PoseidonGL33` as the A/B reference backend.

**Branch:** `metal-backend` (off `macos-port`). `macos-port` stays the always-playable baseline.

**Primary goal (perf):** the GL backend is draw-call-bound on Apple's GL→Metal layer
(~40 µs/call CPU; town scenes ~1.7 k calls → ~70 ms). The Metal backend treats per-draw
CPU cost as a first-class constraint: **target < 5 µs/draw-call CPU**, achieved by
prebuilt PSOs, zero per-draw heap allocations, and offset-only constant streaming from a
triple-buffered ring.

**Secondary goal (correctness):** Metal's native zero-to-one depth matches what
`ShadowMath.cpp:169-196` and the engine's projection math already produce — the Metal
backend removes the GL clip-control fallback precision loss for free.

> Plan authored 2026-07-24, revised same day after a 3-reviewer panel with independent
> source verification (23 confirmed findings applied — notably the projected-shadow
> algorithm, the presentation lifecycle, and the P8 pipeline), and revised again after a
> cycle-2 review (19 verified findings — notably CMake dependency ordering, TBDR
> load/store actions, the CPU-clip decision, font-atlas backend neutrality, alpha-class
> routing, and DrawItem recording). All library/OS facts were
> verified against primary sources (see §1); all engine-behavior claims were re-verified
> against the GL33 **code paths** (not doc comments — see the stale-comment inventory in
> §6.3). Facts that could not be pinned are flagged `⚠️ VERIFY AT M0`.

---

## 0. Boundaries for implementers

Work packages derived from this plan MUST honor:

- **ALWAYS**
  - Keep `CWR_HAS_OPENGL` / GL33 building and selectable after every milestone. Every
    milestone leaves the branch buildable with `--renderBackend gl33` working.
  - Keep all new engine-side code C++17 (`PoseidonMetal` pins `CXX_STANDARD 17`,
    matching its sibling `PoseidonGL33`; note the top-level default is C++20 —
    `CMakeLists.txt:9` — only the backends pin 17).
  - Mirror `PoseidonGL33` file naming/layout (see §3.1) — reviewers navigate by analogy.
  - Preserve the WorldInstances instanced-run contract (see `docs/brain/GOTCHAS.md`
    "WorldInstances range must stay bound throughout an instanced run") — the same bug
    class exists in the Metal design (§6.6).
  - Increment `Poseidon::gPerfDrawCalls` at every draw-emission seam (frame profiler
    contract).
  - Treat GL33 **source** as the parity oracle. Where a doc comment and the code path
    disagree (e.g. `Engine.hpp:457-466` vs `EngineGL33_Draw.cpp:94-110`), parity gates on
    the code path; flag the stale comment in review, do not implement it.
  - Append to `docs/brain/DECISIONS.md` / `GOTCHAS.md` when a load-bearing deviation from
    this plan is made or a trap is hit (project CLAUDE.md brain protocol).
- **ASK FIRST** (halt, surface in §11 Questions)
  - Any new vcpkg dependency (none is planned; SPIRV-Cross explicitly rejected in §5).
  - Any change to shared engine math (`ShadowMath.cpp`, `MatrixConversion.*`) beyond what
    §8 prescribes — it is shared with GL33 and the CPU oracle tests.
  - Changing the `Engine` virtual surface (`engine/Poseidon/Graphics/Core/Engine.hpp`).
    The Metal backend implements it; it does not reshape it (the de-GL-ification in §3.4
    is additive moves only).
  - Adopting reversed-Z (explicitly deferred, §8.3).
- **NEVER**
  - Do not modify `PoseidonGL33` behavior. Permitted GL33-adjacent changes are exactly:
    (1) the mechanical include-path change from the `SDLEventWindow.hpp` hoist (§3.4);
    (2) the **additive, optional** pixel-size callback on the hoisted `SDLEventWindow`
    (§4.5) — GL33 does not register it; (3) the behavior-identical
    `TextureGL33::IsGpuResident()` override (returns `GetHandle() != 0`) required by the
    font-atlas backend neutralization (§3.4) — it replaces the exact expression
    `FontDrawFreeType.cpp` already computes via `static_cast`. All three leave GL33
    bit-identical. It is the A/B reference; a drifting reference invalidates parity
    testing.
  - Do not commit game data or screenshots containing licensed assets to the repo.
  - Do not use any `MTL4*` / Metal 4 API — they require macOS 26 (§1.1); minimum OS is
    macOS 15.
  - Do not introduce per-draw dynamic allocation, per-draw `MTLBuffer` creation, or
    per-draw PSO creation on the frame path.
  - Do not reset `FrameRing` cursors, reuse a ring slot, or mutate ring memory outside
    semaphore ownership — slot rotation happens **only** in `InitDraw` after
    `dispatch_semaphore_wait` (§6.1). `ResetForRemount` never touches the ring (§7.3).

---

## 1. Documentation findings (verified 2026-07-24)

### 1.1 Metal version to target: **Metal 3, MSL 3.2** (not Metal 4)

- `MTLLanguageVersion.version3_2` — **introduced macOS 15.0** (also iOS 18). Verified via
  Apple's documentation data API:
  `developer.apple.com/tutorials/data/documentation/metal/mtllanguageversion/version3_2.json`
  → `platforms: macOS 15.0`.
- `MTLLanguageVersion.version4_0` — **introduced macOS 26.0** (Tahoe); `version4_1` is
  macOS 27 beta. Same source, `.../version4_0.json`, `.../version4_1.json`.
- Metal 4 (the new `MTL4*` command-buffer/argument-table API family) ships with
  macOS 26 and requires Apple silicon (M1+/A14+): [Apple Support — Metal on Apple
  devices](https://support.apple.com/en-ug/102894), [Metal 4 overview
  (lowendmac)](https://lowendmac.com/2025/metal-4-an-overview/).

**Decision:** with macOS 15 Sequoia as the minimum supported OS, Metal 4 is unavailable
→ target the **Metal 3 API surface with MSL 3.2 as the language cap**. Nothing in this
renderer (forward-lit, ~13 shaders, no bindless/ML) benefits from Metal 4's tensor/ML or
new binding model anyway. Revisit only if the min OS moves to 26+.

### 1.2 SDL3 Metal integration (verified)

- [`SDL_Metal_CreateView`](https://wiki.libsdl.org/SDL3/SDL_Metal_CreateView) — creates a
  `CAMetalLayer`-backed `NSView` and attaches it to an `SDL_Window`; available since
  SDL 3.2.0 (repo pins `sdl3 >= 3.4.10#1` in `vcpkg.json` — fine). Remark verified: *"On
  macOS, this does not associate a MTLDevice with the CAMetalLayer on its own. It is up
  to user code to do that."*
- [`SDL_Metal_GetLayer`](https://wiki.libsdl.org/SDL3/SDL_Metal_GetLayer) — returns the
  backing `CAMetalLayer*` for the view (main thread only).
- `SDL_WINDOW_METAL` window flag exists in SDL3
  ([SDL_video.h](https://github.com/libsdl-org/SDL/blob/main/include/SDL3/SDL_video.h),
  `0x0000000020000000`, "window usable for Metal view").
- `SDL_GetRenderMetalLayer` belongs to the **SDL_Render** API — not used; we drive
  `CAMetalLayer` directly.
- **View lifetime (M0 implementation note):** retain the `SDL_MetalView` handle for the
  window's whole life; the `CAMetalLayer*` from `SDL_Metal_GetLayer` is **borrowed**
  (owned by the view — do not release it). Teardown order:
  `SDL_Metal_DestroyView(view)` on the **main thread**, *then* `SDL_DestroyWindow`.

### 1.3 Texture formats on Apple Silicon Macs (verified)

- **BC1/BC2/BC3 are natively supported.** `MTLPixelFormat.bc1_rgba/bc2_rgba/bc3_rgba`
  are macOS 10.11+ ([pixel format docs](https://developer.apple.com/documentation/metal/mtlpixelformat/bc1_rgba));
  on Apple GPUs BC support is queryable via
  [`MTLDevice.supportsBCTextureCompression`](https://developer.apple.com/documentation/metal/mtldevice/supportsbctexturecompression)
  (macOS 11+) and is **true on all Apple silicon Macs** (M1+ supports BC in hardware;
  see also [Aras Pranckevičius — Texture Compression on Apple M1](https://aras-p.info/blog/2021/01/18/Texture-Compression-on-Apple-M1/)).
  → PAA DXT1/3/5 payloads upload **as-is**, no transcode.
- 16-bit packed formats (`b5g6r5Unorm`, `bgr5A1Unorm`, `abgr4Unorm`) are macOS 11+
  **Apple-GPU-only** formats (verified via docs JSON — all "macOS 11.0"). Supported on
  our target, but component ordering differs from GL's `GL_RGB5_A1`/`GL_RGBA4` packing —
  §7.1 chooses CPU expansion to RGBA8 for these formats instead.

### 1.4 Submission / streaming best practice (verified)

- Triple-buffering dynamic data with a `dispatch_semaphore` and command-buffer completion
  handlers is Apple's documented pattern:
  [Synchronizing CPU and GPU Work](https://developer.apple.com/documentation/metal/synchronizing-cpu-and-gpu-work)
  ("Avoid stalls between CPU and GPU work by using multiple instances of a resource").
- Per-draw constant offsets:
  [`setVertexBufferOffset(_:index:)`](https://developer.apple.com/documentation/metal/mtlrendercommandencoder/setvertexbufferoffset(_:index:))
  (macOS 10.11+) updates only the offset of an already-bound buffer — this is the
  <1 µs per-draw rebind primitive the constant ring relies on.
- PSO caching: [`MTLBinaryArchive`](https://developer.apple.com/documentation/metal/mtlbinaryarchive)
  (macOS 11+) provides explicit pipeline-binary caching. With our small PSO set
  (§6.3) startup compilation is expected to be cheap; the archive is an *optional* M6
  polish, not architecture.
- Argument buffers: not needed. Classic binding (`setVertexBuffer`/`setFragmentTexture`)
  with ≤3 buffers + ≤3 textures per draw is well within Metal's fast path, and
  non-argument-buffer bindings are automatically resident (no `useResource`
  bookkeeping). Argument buffers/bindless would add complexity with zero benefit at 6
  fragment shaders and 2 texture slots (YAGNI).

### 1.5 Presentation / depth / toolchain / interop (verified)

- [`CAMetalLayer.displaySyncEnabled`](https://developer.apple.com/documentation/quartzcore/cametallayer/displaysyncenabled)
  (macOS 10.13+) is the vsync switch for Metal presentation — maps to the engine's
  `SetSwapInterval` (§4.4).
- `MTLPixelFormat.depth32Float` macOS 10.11+ (verified). Apple GPUs do **not** support
  `depth24Unorm_stencil8` (Intel/AMD-only; gated by
  `MTLDevice.isDepth24Stencil8PixelFormatSupported`) → use **`depth32Float_stencil8`**
  for the frame depth-stencil target (§8.1).
- Metal NDC depth is **natively [0,1]** (Metal Shading Language Specification,
  [PDF](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf) —
  clip-space w-divide maps z to [0,1]; framebuffer origin top-left).
- [metal-cpp](https://developer.apple.com/metal/cpp/): Apple's official header-only C++
  interface for Metal — direct 1:1 mapping of the ObjC API in the `MTL::`/`NS::`/`CA::`
  namespaces, C++17, zero measured overhead, one TU defines
  `NS_PRIVATE_IMPLEMENTATION`/`CA_PRIVATE_IMPLEMENTATION`/`MTL_PRIVATE_IMPLEMENTATION`;
  links Foundation + QuartzCore + Metal frameworks. Memory management is manual
  retain/release per Cocoa rules (no ARC).
- Offline shader compilation:
  [Building a shader library by precompiling source files](https://developer.apple.com/documentation/metal/building-a-shader-library-by-precompiling-source-files)
  — `xcrun -sdk macosx metal -c X.metal -o X.air` then
  `xcrun -sdk macosx metallib X.air -o X.metallib`.
  ⚠️ VERIFY AT M0: exact language-standard flag spelling (expected `-std=metal3.2`) via
  `xcrun -sdk macosx metal --help`.
- **The Metal compiler is a separately-downloaded component.** On current Xcode the
  `metal` tool is NOT installed by default — it ships as the downloadable **"Metal
  Toolchain"** component. **Verified failing on the dev machine 2026-07-24:**
  `xcrun -sdk macosx metal` errors until `xcodebuild -downloadComponent MetalToolchain`
  has been run. This is a hard build prerequisite with a CMake discovery + runtime
  fallback design in §3.3 and a CI provisioning step.

### 1.6 Library facts table

| Fact | Value | Source (2026-07-24) |
|---|---|---|
| MSL cap on macOS 15 | 3.2 (`MTLLanguageVersion.version3_2`) | Apple docs data API (§1.1) |
| Metal 4 availability | macOS 26+, Apple silicon only — **out of scope** | Apple Support 102894, docs data API |
| SDL3 Metal glue | `SDL_Metal_CreateView` / `SDL_Metal_GetLayer` / `SDL_WINDOW_METAL`, since 3.2.0; view retained, layer borrowed, destroy view before window (§1.2) | wiki.libsdl.org, SDL_video.h |
| BC1/2/3 on Apple silicon | native; `supportsBCTextureCompression == true` | Apple docs (§1.3) |
| 16-bit packed formats | macOS 11+ Apple-GPU only | Apple docs JSON (§1.3) |
| Depth-stencil format | `depth32Float_stencil8` (no depth24 on Apple GPUs) | Apple docs (§1.5) |
| Constant offset rebind | `set{Vertex,Fragment}BufferOffset`, macOS 10.11+ | Apple docs (§1.4) |
| PSO binary cache | `MTLBinaryArchive`, macOS 11+ (optional M6) | Apple docs (§1.4) |
| Vsync | `CAMetalLayer.displaySyncEnabled`, macOS 10.13+ | Apple docs (§1.5) |
| metal-cpp | official, header-only, C++17 | developer.apple.com/metal/cpp |
| Offline compile | `xcrun metal` → `.air` → `xcrun metallib`; **requires the separately-downloaded Metal Toolchain** (`xcodebuild -downloadComponent MetalToolchain`) | Apple docs (§1.5) + verified failing locally 2026-07-24 |
| MSAA sample counts | must query `MTLDevice.supportsTextureSampleCount(_:)` per count (2/4/8 not guaranteed) | Apple docs; clamped per §6.5 |
| Deferred store actions | `setColorStoreAction(_:index:)` / `setDepthStoreAction(_:)` / `setStencilStoreAction(_:)`, macOS 10.12+; requires the pass descriptor attachment `storeAction = Unknown`, must be called before `endEncoding()`, and cannot itself pass `Unknown` | Apple docs data API, `.../mtlrendercommandencoder/setcolorstoreaction(_:index:).json` + `setdepthstoreaction(_:).json`, verified 2026-07-24 (§6.4) |
| Buffer-offset alignment for constant data | use **256 B** (safe on all Mac GPU families) | ⚠️ VERIFY AT M0 whether Apple-GPU family relaxes this to 4/16 B; 256 B is the conservative documented Mac constraint and costs nothing at our sizes |

---

## 2. Target & constraints

- **OS:** macOS 15 (Sequoia) minimum. **Arch:** arm64 only (per `DECISIONS.md`). No
  Intel/universal support — Apple-GPU family assumptions (unified memory, TBDR, BC
  support) may be relied on after a single startup capability assert.
- **API:** Metal 3, MSL 3.2 cap. No `MTL4*` symbols.
- **Language: metal-cpp (pure C++17), not Objective-C++.**
  *Why:* the top-level `project()` enables only `LANGUAGES CXX C`
  (`CMakeLists.txt:5`) — **no `OBJCXX` language is enabled anywhere in the build**, and
  repo tooling (`.clang-format`, `.clang-tidy`) is C++-oriented. The tree is C++
  throughout (top-level `CMAKE_CXX_STANDARD 20` at `CMakeLists.txt:9`; `PoseidonGL33`
  pins 17, and `PoseidonMetal` pins 17 to match its sibling backend). metal-cpp is
  Apple-official, header-only, maps 1:1 to the ObjC API (Apple docs read directly
  against it), and interops with SDL3's `void*` returns
  (`SDL_Metal_GetLayer()` → `static_cast<CA::MetalLayer*>`). Manual retain/release
  matches the codebase's existing manual `Ref<>` discipline.
  *Rejected:* Objective-C++ (.mm) backend — forces enabling `OBJCXX` in the top-level
  build, ARC-vs-MRC decisions per file, and a style split in review. If an AppKit edge
  case ever genuinely needs ObjC (none is anticipated — SDL owns the NSWindow/NSView), a
  **single** isolated `.mm` file may be added, gated behind an ASK-FIRST question.
- **metal-cpp vendoring:** commit the **macOS 15 / iOS 18 release** of metal-cpp to
  `thirdparty/metal-cpp/` (mirrors `thirdparty/glad/`). Pinning the Sequoia release makes
  accidental use of macOS 26-only API a compile error, not a runtime availability bug.
- **GL33 stays intact** as reference. The only GL33-adjacent edits allowed are the
  include-path change from the `SDLEventWindow.hpp` hoist and the optional pixel-size
  callback GL33 does not register (§3.4, §4.5).
- **Existing contracts that must not change:** the `Engine` virtual surface
  (`Engine.hpp`, `IGraphicsEngine.hpp`), `FrameState`/`PassState`/`DrawItem`
  (`RenderState.hpp`), the `render::frame::Draw` emission seam **including its 32-bit
  resource handles** (`Frame.hpp` — `TextureHandle{uint32_t}`,
  `MeshHandle{uint32_t vao; BufferHandle vbo, ibo}`; §6.6), the texture bank contract
  (`AbstractTextBank`), the instanced-run protocol
  (`InstancedRunReset/Add/BeginInstancedRunUpload/EndInstancedRun`), the shadow API
  (`BeginShadowPass/EndShadowPass`, `RenderShadowDepthScene`, `ShadowCasterSet`), and the
  **frame lifecycle split**: `FinishDraw` closes the frame *without presenting*;
  `NextFrame` presents (§6.1) — trident tests read pixels between the two.

---

## 3. Architecture

### 3.1 New library `engine/PoseidonMetal/` (mirrors `PoseidonGL33`)

| File | Purpose (GL33 analogue) |
|---|---|
| `CMakeLists.txt` | static lib `PoseidonMetal`; frameworks; metallib rule + toolchain discovery (§3.3) |
| `GraphicsBackendMetal.cpp` | `RegisterMetalGraphicsBackend()` descriptor (GL: `GraphicsBackendGL33.cpp`) |
| `MetalCppImpl.cpp` | the one TU with `NS/CA/MTL_PRIVATE_IMPLEMENTATION` defines |
| `MetalContext.hpp` | owns `MTL::Device`, `MTL::CommandQueue`, `SDL_MetalView` + borrowed `CA::MetalLayer*`, drawable acquisition, autorelease-pool scope helper |
| `HandleRegistry.hpp` | generation-checked `uint32_t` → MTL-object registries for mesh & texture handles (§6.6) — new; GL stores GL names directly in the 32-bit handle ids |
| `EngineMetal.hpp` | `class EngineMetal : public Engine` (GL: `EngineGL33.hpp`) |
| `EngineMetal.cpp` | ctor: SDL window (+`SDL_WINDOW_METAL`), device, view/layer wiring, config (GL: `EngineGL33.cpp`) |
| `EngineMetal_Lifecycle.cpp` | `InitDraw`/`FinishDraw` (commit, **no present**), `NextFrame` (compose→capture→present, §6.1/§6.5), resize, fullscreen, window-mode/display queries incl. `GetCurrentWindowMode` (§4.5), `DrawTestPattern` (§6.7), `ResetForRemount`, screenshots (GL: `EngineGL33_Lifecycle.cpp`) |
| `CopyLayout.hpp` | format-aware copy-layout helper: texel/block sizes, `bytesPerRow`/`bytesPerImage` (incl. BC block-row math), mip dimension rounding, aligned mip offsets — single source of truth for uploads, screenshots, probes, dumps (§6.5/§7.3) — new |
| `FrameRing.hpp` / `FrameRing.cpp` | triple-buffered per-frame arena + semaphore + command-buffer span management + overflow-buffer retention (§6.1–6.2) — new; replaces GL's UBO orphaning |
| `EncoderBroker.hpp` / `EncoderBroker.cpp` | lazy render-encoder open/close over target switches, sticky-state replay, `{buffer,offset}`-pair binding cache, mid-frame clear handling (§6.4) — new |
| `EngineMetal_State.cpp` | PSO cache, `MTLDepthStencilState` set, sampler states, `ApplyPipeline`, `ApplyWorldViewport`/`EndWorldViewport` (GL: `EngineGL33_State.cpp`) |
| `EngineMetal_Constants.cpp` | CPU shadow copies + dirty tracking + ring snapshots for VS/PS/WorldInstances (GL: constants half of `EngineGL33_Shaders.cpp`) |
| `EngineMetal_Draw.cpp`, `EngineMetal_DrawShared.cpp` | decals, lines, points, `UpdateProjection`, `Begin/EndShadowPass` flush brackets (GL analogues) |
| `EngineMetal_2D.cpp`, `EngineMetal_2DRendering.cpp` | 2D/HUD drawing (GL analogues) |
| `EngineMetal_Queue.cpp` | TLVertex soup queue → ring vertices (GL: `EngineGL33_Queue.cpp`) |
| `EngineMetal_VertexBuffer.cpp` | static mesh `MTLBuffer`s, `DrawSectionTL`, `EmitDraw`, instanced runs, readbacks (GL: `EngineGL33_VertexBuffer.cpp`) |
| `EngineMetal_Material.cpp`, `EngineMetal_Mesh.cpp` | material/light upload, mesh begin/end (GL analogues) |
| `EngineMetal_ShadowDepth.cpp` | cascade depth passes + `ShadowDepthProbe` + `DumpShadowMap` + fallback white textures (GL: `EngineGL33_ShadowDepth.cpp`) |
| `TextureMetal.hpp`, `TextureMetal_Init.cpp`, `TextureMetal_Loading.cpp` | texture object + PAA/DXT upload, `DstFormat` resolution, alpha-class scan (`GetAlphaClass`/`ScanTopMipAlphaClass`, §7.4), interpolated-format resolution (§7.1) (GL: `TextureGL33*`) |
| `TextureBankMetal_Core.cpp`, `TextureBankMetal_Cache.cpp` | texture bank + mip streaming + upload command-buffer ordering + `CreateDynamic`/`UpdateDynamic` (font atlas, §3.4/§6.7) (GL analogues) |
| `Shaders/PoseidonShaderTypes.h` | C++/MSL-shared constant struct layouts + `static_assert`s (§5.3) |
| `Shaders/PoseidonShaders.metal` | all MSL shaders (§5) |

### 3.2 Selection surface (enum + factory + app wiring)

Files to modify:

| File | Change |
|---|---|
| `engine/Poseidon/Graphics/GraphicsEngineFactory.hpp` | add `Metal = 40` to `enum class GraphicsBackend`; declare `void RegisterMetalGraphicsBackend();` |
| `engine/Poseidon/Graphics/GraphicsEngineFactory.cpp` | `CodeForBackend`: `Metal → "metal"`; `GetBackendName` fallback: `"Metal 3 (SDL3)"` |
| `apps/cwr/Game/GameApplication.cpp` (`RegisterGraphicsBackends()`, ~line 1479) | `#if defined(__APPLE__) && defined(CWR_HAS_METAL)` → `RegisterMetalGraphicsBackend();` |

The string surface already works end-to-end: `AppConfig::GetRenderBackend()` →
`GraphicsEngineFactory::Create(string)` (`GameApplication.cpp:1554-1560`), so
`--renderBackend metal` (or the config equivalent) needs **no new parsing**.

Descriptor: `{"metal", "Metal 3 (SDL3)", priority, CreateMetalBackend, IsMetalAvailable}`.

- `IsMetalAvailable()` = `MTLCreateSystemDefaultDevice() != nullptr` (create, check,
  release; cache the result).
- **Priority: 50 until M6, then 200.** Below GL33's 100 during bring-up so `Auto` (the
  default) keeps picking GL33 and the branch stays playable; the flip to 200 is the
  explicit M6 "Metal becomes default on macOS" switch.

### 3.3 CMake

- Top-level `CMakeLists.txt`: `option(CWR_HAS_METAL "Metal 3 graphics backend (macOS)" ${APPLE})`,
  `add_subdirectory(engine/PoseidonMetal)` under `if(APPLE AND CWR_HAS_METAL)` (with the
  other engine subdirs, `CMakeLists.txt:194-197`).
  **App-target audit (verified):** three executables compile
  `apps/cwr/Game/GameApplication.cpp` (which contains `RegisterGraphicsBackends()`) and
  link `PoseidonGL33`: `PoseidonGame` (`apps/cwr/Game`), `PoseidonGameDemo`
  (`apps/cwr/GameDemo` — reuses `GameApplication.cpp` via `GAMEDEMO_SOURCES`), and
  `PoseidonTetris` (`apps/tetris/Tetris` — same reuse). **All three** must, under
  `if(APPLE AND CWR_HAS_METAL)` in their own `CMakeLists.txt`: add the `CWR_HAS_METAL`
  compile definition (otherwise the registration `#if` silently differs per binary),
  `target_link_libraries(<target> PRIVATE PoseidonMetal)` (without this the registration
  symbol never reaches the executable), and `add_dependencies(<target>
  PoseidonMetalShaders)` (shader staging, below).
  While in there: fix the stale `CWR_HAS_OPENGL` label at `CMakeLists.txt:145`
  ("OpenGL 4.5" → "OpenGL 3.3") — documented cleanup from `MACOS_PORT.md`.
- **Metal Toolchain discovery (configure time, fail loud):** the shader compiler is a
  separate download on current Xcode (§1.5 — verified failing on the dev machine).
  `engine/PoseidonMetal/CMakeLists.txt` runs
  `execute_process(xcrun -sdk macosx metal --version)` at configure time:
  - Toolchain present → define the offline metallib rule (below);
    `CWR_METAL_OFFLINE_SHADERS=ON`.
  - Toolchain missing → `message(FATAL_ERROR ...)` with the exact remedy
    (`xcodebuild -downloadComponent MetalToolchain`) **unless**
    `-DCWR_METAL_RUNTIME_SHADERS=ON` is passed, which skips the metallib rule; the
    `PoseidonMetalShaders` target (same name in both modes, below) then stages a
    **pre-expanded** `.metal` source into the runtime dir instead; the engine compiles
    it at init via `device->newLibrary(source, options, &err)` — so toolchain-less
    machines can still build & run, loudly (`LOG_WARN` at startup).
  - **Runtime-compiled MSL has no `#include` resolution** — `newLibrary(source:)` gets
    no include search path, so the staged source must be **self-contained**. The staging
    step pre-expands `Shaders/PoseidonShaderTypes.h` into the staged
    `PoseidonShaders.metal` (build-time concatenation via a `cmake -P` script:
    `file(READ)` both files, substitute the `#include "PoseidonShaderTypes.h"` line,
    write the staged file). The offline `xcrun metal` compile resolves the `#include`
    normally and does not need this. Exercised at M0 (§9) — the fallback path must
    actually compile the concatenated source.
  - Runtime library load order in the engine: `PoseidonShaders.metallib` if present next
    to the binary, else the staged (pre-expanded) `.metal` source, else **fatal,
    logged** init error.
  - **CI provisioning:** the CI/setup docs gain a step running
    `xcodebuild -downloadComponent MetalToolchain` (idempotent) before the first Metal
    build.
- `engine/PoseidonMetal/CMakeLists.txt`:
  - static library, `CXX_STANDARD 17`, includes `thirdparty/metal-cpp/`;
  - `target_link_libraries(PoseidonMetal PUBLIC Poseidon "-framework Metal" "-framework QuartzCore" "-framework Foundation")`;
  - **metallib rule:** `add_custom_command` chain
    `xcrun -sdk macosx metal -std=metal3.2 -o PoseidonShaders.air -c Shaders/PoseidonShaders.metal`
    → `xcrun -sdk macosx metallib PoseidonShaders.air -o PoseidonShaders.metallib`
    (flag spelling ⚠️ VERIFY AT M0), plus a copy step into the runtime output dir next to
    the binaries (mirror how `DistCopy.cmake` stages runtime files). Debug configs add
    `-frecord-sources -gline-tables-only` so Xcode's Metal debugger shows source.
    **The custom command must have a consuming target or it never runs:** add
    `add_custom_target(PoseidonMetalShaders ALL DEPENDS <staged outputs>)` — in offline
    mode it depends on the metallib chain, in `CWR_METAL_RUNTIME_SHADERS` mode on the
    pre-expanded staged source (one target name covers both modes, so app wiring is
    mode-independent). **`add_dependencies(<app> PoseidonMetalShaders)` must NOT live in
    `engine/PoseidonMetal/CMakeLists.txt`:** the engine subdirs are configured at
    top-level lines 194-197, the app subdirs only at 210/211/218 — at engine configure
    time `PoseidonGame` does not exist yet and `add_dependencies` on it is a CMake
    **fatal error**. The dependency is attached from each app target's own
    `CMakeLists.txt` (all three executables — see the app-target audit above).
  - No `OBJCXX` anywhere — the target stays pure C++ (metal-cpp decision, §2).

### 3.4 Minimal de-GL-ification (enables reuse without touching behavior)

- Move `engine/PoseidonGL33/SDLEventWindow.hpp` → `engine/Poseidon/Graphics/Shared/SDLEventWindow.hpp`.
  It is pure SDL (verified — no GL symbols); both backends include it. Update the one GL33
  include. Done in M0. The hoisted file additionally gains the **optional** pixel-size
  event callback (§4.5) that GL33 leaves unregistered — no GL33 behavior change.
- **Font-atlas backend neutralization (M1, with fonts).**
  `engine/Poseidon/Graphics/Rendering/Draw/FontDrawFreeType.cpp` — **shared engine
  code** — includes `PoseidonGL33/TextureGL33.hpp` (`:2`) and `static_cast`s
  `Texture* → TextureGL33*` (`:49`, `IsTextureGpuValid`) to detect a texture whose GPU
  surface was freed under a live `Ref<Texture>` (F5 hot-reload). Under Metal that cast
  is UB on a `TextureMetal`. Fix, behavior-identical on GL33:
  - Add `virtual bool IsGpuResident() const { return true; }` to the `Texture` base
    (`Poseidon/Graphics/Textures/TextureBank.hpp`, next to `GetAlphaClass` at `:64`).
  - `TextureGL33` overrides it as `return GetHandle() != 0;` (the exact expression the
    cast computed — permitted GL33-adjacent change (3), §0).
  - `TextureMetal` overrides it as "registry handle resolves to a live `MTL::Texture`"
    (§6.6).
  - `FontDrawFreeType.cpp` drops the GL33 include + cast and calls the virtual — this
    also removes a shared-layer → backend include-direction violation.
  The atlas upload path itself is already backend-neutral (`AbstractTextBank`
  `CreateDynamic`/`UpdateDynamic` virtuals, `TextureBank.hpp:168/:173`); the Metal bank
  implements both for dynamic RGBA atlases (§6.7).
- Do **not** attempt the broader `Graphics/Core` GL-type cleanup (`GLBlendState.hpp` etc.)
  in this project. `EngineMetal` consumes the backend-agnostic types it actually needs
  (`RenderState.hpp`, `RenderPassDescriptor`, `TLVertex.hpp`, `MipmapLayout.hpp`,
  `ZBiasMath.hpp`, `FanDecompose.hpp`) and ignores the `GL*.hpp` headers. Renaming those
  is cosmetic churn across the shared layer with regression risk in the reference
  backend — explicitly out of scope (YAGNI; revisit after M6).

---

## 4. Window & surface

### 4.1 Window creation (`EngineMetal.cpp`, mirroring `EngineGL33.cpp:266-455`)

1. `SDL_Init(SDL_INIT_VIDEO)`.
2. Window flags: `SDL_WINDOW_METAL` (replaces `SDL_WINDOW_OPENGL`);
   `SDL_WINDOW_HIGH_PIXEL_DENSITY` **iff `engineCfg.nativePixelDensity`** — exactly the
   GL33 rule (`EngineGL33.cpp:342-343`; Retina default-off per the Phase 2 finding).
   Same `ResolveWindowPlacement` flow, same borderless/fullscreen handling (the
   non-`_WIN32` branch).
3. `SDL_Metal_CreateView(window)` → **store the `SDL_MetalView` handle** →
   `SDL_Metal_GetLayer(view)` → `CA::MetalLayer* layer` (**borrowed**, never released by
   us; §1.2). Teardown: `SDL_Metal_DestroyView` on the main thread **before**
   `SDL_DestroyWindow`.
4. Configure layer: `setDevice(device)` (mandatory on macOS, §1.2),
   `setPixelFormat(MTL::PixelFormatBGRA8Unorm)`, `setFramebufferOnly(true)` (the
   drawable is only ever a render-pass color attachment; captures never read it — §6.5),
   `setMaximumDrawableCount(3)`, `setDisplaySyncEnabled(vsync)`.
5. `layer->setDrawableSize({pxW, pxH})` from `SDL_GetWindowSizeInPixels` — and again on
   every resize/pixel-size event (§4.5; `_w/_h` remain pixel sizes exactly as in GL33).
6. `_eventWindow.Attach(...)` — the shared `SDLEventWindow` (§3.4), unchanged behavior
   (`HandleEvents`, focus, Alt-Enter, resize events) plus the Metal-only pixel-size
   callback registration (§4.5).

### 4.2 Device objects created at init

`MTL::Device` (system default), one `MTL::CommandQueue` (single-threaded submission — the
engine renders from one thread today; no reason for more queues; **all** command buffers,
including texture uploads and readbacks, go through this one queue so commit order ==
execution order, §7.3/§6.5), `MTL::Library` from the metallib (or source fallback, §3.3),
PSO cache prebuild (§6.3), depth-stencil state set (§6.3), sampler-state set (§6.7),
fallback white textures (§6.6), `FrameRing` (§6.1), offscreen frame target (§6.5).

### 4.3 Resize / fullscreen

Resize handling is **coalesced at the frame boundary**: event handlers record the latest
pending pixel size; `NextFrame`'s frame-boundary block applies it once — update `_w/_h`,
`layer->setDrawableSize`, drop & lazily recreate FrameTarget/CompositeTarget + depth
target at the new size, fire `FireResizePostHook`. (`setDrawableSize` itself is cheap and
may also be called immediately; target rebuilds must not happen mid-frame.)
`OnFullscreenChanged` mirrors GL33's single-source-of-truth `_windowed` update.

### 4.4 VSync / present

- `SetSwapInterval(interval)`: `1` → `displaySyncEnabled = true`; `0` → `false`;
  `-1` (adaptive) → **treated as `1`** and logged once — CAMetalLayer has no adaptive
  mode (documented mapping; the Graphics UI row keeps working).
- **Present happens in `NextFrame`, not `FinishDraw`** (§6.1 — repo lifecycle contract):
  `NextFrame` acquires the drawable **late**, encodes the composition pass into it,
  captures any pending screenshot **pre-present**, then
  `commandBuffer->presentDrawable(drawable)` + `commit()`.
- `nextDrawable()` may return nil (occluded window, size 0) → skip composition + present
  cleanly, still run the frame-boundary block; never crash. `FinishDraw` never touches
  the drawable, so mid-frame reset paths (Alt-Enter) present nothing — same as GL33,
  where the swap lives in `BackToFront`.

### 4.5 Window/display method parity table (vs GL33's SDL lifecycle)

Every window/display virtual the game calls, with its GL33 anchor and the Metal port
rule. All of these are SDL-only logic unless noted — port near-verbatim.

| Method | GL33 anchor | Metal port |
|---|---|---|
| `SetWindowMode(WindowMode)` | `EngineGL33_Lifecycle.cpp:653` | identical SDL calls; no GL context juggling; drawable-size refresh after mode change |
| `SwitchRes` / `ListResolutions` / `ListMonitors` | `EngineGL33_Lifecycle.cpp` | verbatim SDL display-mode queries |
| `ListRefreshRates(FindArray<int>&)` | `EngineGL33_Lifecycle.cpp:952` | verbatim SDL display-mode queries |
| `SwitchRefreshRate` | GL33 lifecycle | verbatim; refresh-rate application via SDL fullscreen display mode |
| `GetSwapInterval()` | `EngineGL33_Lifecycle.cpp:646` (`SDL_GL_GetSwapInterval`) | no GL query exists → return the backend-cached interval (`0/1`; `-1` requests are stored as `1` per §4.4) |
| `SetSwapInterval` | GL33 | `displaySyncEnabled` mapping (§4.4) |
| `OnWindowResized` | GL33 + `SDLEventWindow.hpp:115` (`SDL_EVENT_WINDOW_RESIZED`) | same logical-size path, plus pixel-size events below |
| pixel-size tracking | not needed under GL (SDL manages the GL framebuffer) | `SDLEventWindow` gains an **optional** callback forwarding `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` **and** `SDL_EVENT_WINDOW_METAL_VIEW_RESIZED`; only `EngineMetal` registers it (GL33 passes nullptr → zero behavior change). Handler records pending pixel size; applied once at frame boundary (§4.3) — coalescing bursts of RESIZED + PIXEL_SIZE_CHANGED + METAL_VIEW_RESIZED into one target rebuild |
| `SetGamma` | `EngineGL33_Draw.cpp:30-63` — `DoSetGamma` is **`#ifdef _WIN32` only**; on macOS GL33 stores `_gamma` and does nothing | **documented no-op** (store `_gamma`, return) — exact A/B parity with GL33-on-macOS. See §10 risk 3 |
| `GetCurrentWindowMode()` | `EngineGL33_Lifecycle.cpp:777` (base default: `IGraphicsEngine`, `Engine.cpp:55`) — **live consumers:** `DisplayPage.cpp:470` (options page reads the applied mode) and `GameStateExtTestAudio.cpp:491` (harness verb) | verbatim SDL window-flag port (M0); verified via the options display page at M1 |
| `GetGLViewport(int[4])` | `EngineGL33_State.cpp:381` — returns live `glGetIntegerv(GL_VIEWPORT)`; **base returns `false`** (`Engine.hpp:536`), which makes `WorldFrameObserver`'s `DetectViewportMismatch` (`WorldFrameObserver.cpp:99`, `RuntimeChecks.hpp`) **silently no-op** — an unimplemented override quietly disables a frame validator | implement (M2): return the broker's current FrameTarget viewport in render-scale pixels. Convention: the observer compares against `SceneInputs.camera.viewport` scaled by `GetRenderScale()` (`WorldFrameObserver.cpp:100-108`), which is the engine's **top-left** window-space rect — Metal's viewport is natively top-left, so return it **unflipped** (no GL bottom-left conversion; GL33's raw GL rect only matched because world-viewport crops are vertically centered — flag in review, don't imitate) |
| `MinGuardX`/`MaxGuardX`/`MinGuardY`/`MaxGuardY`; `MinSatX`/`MaxSatX`/`MinSatY`/`MaxSatY` | GL33 updates these with the world-viewport state and extends the guard band (`_minGuardX = -maxBand`, `_maxGuardX = _w + maxBand`, `EngineGL33_State.cpp:473-476`); the `Engine.hpp` defaults are the tight `0..Width()` / `0..Height()` bounds. **Live consumer:** `TransLight.cpp:296` clips light saturation against `MinSatX`/`MaxSatX`, so base defaults diverge at screen edges | port and update all guard-band/saturation values alongside the viewport state (§6.4); do not inherit the base defaults |
| `GetDebugErrorCount()` / `GetLastDebugMessage()` | GL33 overrides both (`EngineGL33.hpp:538-539`); trident's `triResetGLErrorBaseline` / `triGetGLErrorCount` consume them (`GameStateExtTest.cpp:2269,2281`; `SceneExtractor.cpp:179-180`). The base defaults return `0` / empty, which would make the harness's “zero new GPU errors” gate silently pass on Metal | override both (M1); back them with a GPU-error counter and last-message store populated by `MTLCommandBufferError` and validation-layer errors (§6.6) |
| `ResetForRemount` / `ResetHard` | `EngineGL33_Lifecycle.cpp:508` / `:525` | §7.3 |

---

## 5. Shader strategy

### 5.1 The actual GL33 shader inventory (counted from source)

`EngineGL33_Shaders.cpp` — 9 inline GLSL 330 sources:

| # | Name | Kind | Notes |
|---|---|---|---|
| 1 | `vsScreen` (`:22`) | VS | pre-transformed TLVertex, rhw, vpScale |
| 2 | `vsTransform` (`:74`) | VS | full lit path: proj/view + `WorldInstances[gl_InstanceID]`, sun + 8 local lights (point/spot cones), specular, fog, texgen |
| 3 | `vsShadow` (`:594`) | VS | unlit transform for stencil-shadow draws |
| 4 | `psNormal` (`:200`) | PS | diffuse×tex + spec + cascade shadow sampling (3×3 PCF, omni+frustum tiers) + night-eye + fog + alpha-test/A2C |
| 5 | `psDetail` (`:319`) | PS | + second texture detail modulate |
| 6 | `psGrass` (`:436`) | PS | grass coef blend |
| 7 | `psWater` (`:552`) | PS | bump water + spec |
| 8 | `psShadow` (`:648`) | PS | cutout discard, black+alpha, forced late-Z |
| 9 | `psFlat` (`:689`) | PS | vColor passthrough (debug) |

`EngineGL33_ShadowDepth.cpp` — 4 more tiny sources: `kVS`/`kFS` (`:53,:59` — solid caster
depth-only) and `kAlphaVS`/`kAlphaFS` (`:203,:211` — alpha-cutout caster depth).

**Total: 13 small shader stages.** The GL program matrix
(`_shaderProgram[v][s][m][i]`, `InitPixelShaders` at `:1620-1666`) links the *same*
objects across the specular/mode dimensions — there are only **3 VS × 6 PS = at most 18
distinct stage pairs actually used** (screen VS pairs only with normal/flat; shadow VS
only with psShadow). The "40 programs" comment is a legacy dimension, not real source
variation.

### 5.2 Port method: **hand-port GLSL → MSL** (no SPIRV-Cross)

*Why:* 13 small, well-commented stages; a hand port to one `PoseidonShaders.metal` file
is 1–2 days of careful work and produces idiomatic MSL (`[[instance_id]]` replaces
`gl_InstanceID`, device-pointer `worldArr` replaces the padded 16 KB std140 block —
§6.6, `discard_fragment()`, `sample` with a `sampler` argument). A
glslang→SPIR-V→SPIRV-Cross toolchain would (a) add SPIRV-Cross as a **new** dependency
(repo has glslang only for a unit test; SPIRV-Cross is absent — verified in
`vcpkg.json`), (b) require rewriting the GLSL for Vulkan-style semantics
(explicit binding decorations, no `gl_InstanceID` w/o extension shims) — comparable
effort to porting, and (c) generate less-reviewable MSL that still needs the constant
layout re-mapped by hand. The GLSL stays in the repo as the behavioral reference and A/B
oracle.
*Rejected alternative:* runtime GLSL→MSL via SPIRV-Cross in-process — worst of both
(new deps + runtime complexity + slower startup).

MSL entry points (one library) — **6 VS, 9 PS** (these counts size the PSO-key stage-ID
fields, §6.3):

- VS (6): `vsScreen`, `vsTransform`, `vsShadow`, `vsShadowDepthSolid`,
  `vsShadowDepthAlpha`, `vsBlitScale`
- PS (9): `psNormal`, `psDetail`, `psGrass`, `psWater`, `psShadow`, `psFlat`,
  `psShadowDepthSolid` (empty), `psShadowDepthAlpha`, `psBlitScale`

`vsBlitScale`/`psBlitScale` is the **one new pair**: a fullscreen textured triangle used
for the composition/downsample passes (§6.5 — Metal blit encoders cannot scale) and,
with color writes masked off, for the mid-frame depth/stencil-clear draw (§6.4).

Port-time simplifications (verify at the milestone that owns them):
- `psShadow` drops the `gl_FragDepth = gl_FragCoord.z` late-Z forcing hack — Metal
  guarantees `discard_fragment()` suppresses the fragment's depth **and stencil** writes;
  in the per-poly shadow path (§6.3) a discarded cutout texel must not INCR the stencil,
  which is exactly what the GL hack protected (`:670-679`). Note: that GL source comment
  itself narrates the **dead** REPLACE-0xFF/fullscreen-darken scheme (stale-comment
  inventory, §6.3); this plan's restatement of its rationale in terms of the **live**
  INCR-clamp/EQUAL-0 states is a deliberate reinterpretation — the underlying invariant
  (discard must suppress the stencil write) is real and carries over unchanged. Verify
  at M5 that foliage leaf gaps do not phantom-darken (the exact symptom the hack fixed).
- `vsScreen`'s Y-flip (`1.0 - aPos.y * vpScale.y`) is re-derived for Metal's top-left
  framebuffer origin — expect the flip to *disappear* rather than double (verify with
  the M1 HUD, not by reasoning alone).

### 5.3 Constant-block mapping (std140 → Metal argument table)

Buffer index assignments (identical for VS and FS stages where present):

| Metal buffer index | Content | GL analogue |
|---|---|---|
| `buffer(0)` | `VSConstants` — 70 float4 slots, **byte-identical layout** to `s_vsShadow` (`VSConst` slot enums in `EngineGL33.hpp:108-160`) | UBO binding 0 |
| `buffer(1)` | `PSConstants` — 27 float4 slots, byte-identical to `s_psShadow` | UBO binding 1 |
| `buffer(2)` | `WorldInstances` — `const device float4x4*` (runtime-sized; §6.6) | UBO binding 2 (16 KB fixed) |
| `buffer(30)` | vertex data for the soup/screen path (TLVertex stream) | VAO `_vaoScreen` |

Vertex attributes for the mesh path use a `MTLVertexDescriptor` mirroring
`GLVertexAttribLayouts.hpp` (SVertex pos/normal/uv, and the TLVertex layout for the
screen path). Mesh vertex buffers bind at a reserved index (`buffer(29)`) via the vertex
descriptor's buffer layout. **TLVertex `color` and `specular` attributes are declared
with `GL_BGRA` component ordering in GL (`GLVertexAttribLayouts.hpp:48,51`) — the Metal
vertex descriptor MUST use `MTLVertexFormatUChar4Normalized_BGRA` for both**, or every
HUD/soup vertex color arrives red/blue-swapped. (This format exists exactly for D3D/GL
BGRA vertex colors; macOS 10.13+.)

`Shaders/PoseidonShaderTypes.h` defines the two constant structs once (float4 arrays
with named accessor constants matching `VSConst::Slot*` / `PSConstants::Slot`), included
from both C++ and MSL, with `static_assert(sizeof(VSConstantsPod) == 70*16)` /
`27*16` on the C++ side. The CPU shadow-copy code keeps writing through the existing
slot enums — **no offsets change**, which is what makes GL/Metal A/B diffs meaningful.

Texture/sampler table: `texture(0)` = tex0, `texture(1)` = tex1 (detail/grass/spec),
`texture(2)` = cascade shadow `texture2d_array<float>`; `sampler(0)`/`sampler(1)` =
draw samplers, `sampler(2)` = shadow-map sampler (nearest, clamp).

### 5.4 Offline compile + dev hot-reload

- Offline: the CMake metallib rule + consuming custom target (§3.3). CI compiles every
  shader on every build (after the Metal Toolchain provisioning step, §3.3) — the
  drift-vs-GLSL risk of hand-porting is caught at build time, not at runtime.
- Dev hot-reload (parity with GL33's `SetShaderOverrideDir`, `EngineGL33_Shaders.cpp:702-729`):
  when the override dir is set, `EngineMetal` loads `<dir>/PoseidonShaders.metal` at init
  via `device->newLibrary(sourceString, compileOptions, &err)`
  (`MTLCompileOptions.languageVersion = version3_2`) and rebuilds the PSO cache. Runtime
  compilation exists on this dev path and on the toolchain-less fallback path (§3.3).
- **Runtime compilation needs self-contained source** (`newLibrary(source:)` has no
  `#include` resolution — §3.3): the toolchain-less fallback consumes the build-staged
  **pre-expanded** source; the hot-reload loader concatenates at load time instead — it
  reads `<dir>/PoseidonShaderTypes.h` and substitutes it for the `#include` line in
  `<dir>/PoseidonShaders.metal` before compiling, so devs edit the same two files the
  offline build uses.

---

## 6. Frame loop & submission

### 6.1 Lifecycle & the triple-buffered frame ring (`FrameRing`)

**Repo lifecycle contract (verified):** `FinishDraw` closes the frame **without
presenting** (`EngineGL33_Lifecycle.cpp:186-218` — world-viewport safety restore, close
queues, frame-open bookkeeping; no swap). `NextFrame` → `BackToFront()` →
SSAA-resolve → overlay → **pre-swap screenshot capture** → `SDL_GL_SwapWindow`
(`EngineGL33_Lifecycle.cpp:220-224`, `EngineGL33_VertexBuffer.cpp:565-596`). Trident
tests call `SamplePixel` **between** `FinishDraw` and `NextFrame`. The Metal backend
preserves this split exactly:

- `kMaxFramesInFlight = 3`. One `dispatch_semaphore_t(3)` (via `dispatch/dispatch.h` —
  plain C API, fine from C++).
- Per in-flight slot: one **shared-storage** `MTL::Buffer` arena (initial 8 MB;
  unified memory makes shared the right choice for CPU-written, GPU-read-once data) +
  a bump-allocator cursor. Everything dynamic goes through it:
  VSConstants snapshots (1120 B), PSConstants snapshots (432 B), WorldInstances runs
  (64 B × instances), 2D/queue vertex+index soup, shadow-caster vertex uploads,
  per-frame misc.
- **`InitDraw`:** `dispatch_semaphore_wait`; rotate to the next slot; reset its cursor;
  create the span's `MTL::CommandBuffer`. **Slot rotation invalidates all streaming
  state:** every cached `{buffer, offset}` binding in the `EncoderBroker` is cleared and
  **every constant block (VS, PS, WorldInstances) is marked dirty** — the new slot's
  arena is a *different* `MTLBuffer`, so any offset cached from the previous frame
  points into the wrong buffer. Re-snapshot on first use.
- **`FinishDraw`:** `EndWorldViewport()` safety restore (§6.4), close the pass debug
  group, close all queues, end open encoders (**terminal close** — the broker applies
  the terminal store actions, incl. the MSAA resolve, per §6.4), **commit the frame
  command buffer — no drawable, no present**. The completed frame lives in the FrameTarget (§6.5). The
  semaphore-signal `addCompletedHandler` and the overflow/staging release list attach to
  **this** command buffer (the span's last; same-queue serial execution guarantees all
  earlier span CBs completed before it).
- **`NextFrame`** (the `BackToFront` analogue): resolve-to-window-size if needed,
  overlay hook (no-op v1, §10 risk 4), **capture pending screenshots pre-present**
  (§6.5), acquire drawable late, encode the composition pass into it,
  `presentDrawable` + commit on a small present command buffer, then the frame-boundary
  block: apply pending resize/render-scale/MSAA changes (§4.3, §6.5).
  **The present/readback command buffers must not reference ring memory** — the
  composition pass reads only the FrameTarget/CompositeTarget textures and passes its
  few parameters via `setVertexBytes` (inline constants), so ring-slot lifetime remains
  exactly the `InitDraw…FinishDraw` span.
- **Mid-frame `FinishDraw`/`InitDraw` pairs occur** (Alt-Enter/reset paths,
  `EngineGL33_Lifecycle.cpp:485-542` analogue) — each `InitDraw…FinishDraw` span is one
  slot acquisition + one (or more, see readback slow path §6.5) command buffer; nothing
  assumes 1 span == 1 present, and since the drawable only appears in `NextFrame`, these
  pairs present nothing — same as GL33.
- Arena overflow: allocate an overflow shared buffer, log `LOG_WARN` once per frame with
  the high-water mark, and record the size so the next frame's arena grows (fail loud,
  never stall or corrupt). **Overflow buffers are retained on the span's release list
  and released only in the frame command buffer's completion handler** — the GPU may
  read them until then. Bindings into an overflow buffer are a *different* `MTLBuffer`
  than the slot arena, which is why the binding cache must track pairs (§6.2).
  Alignment: all suballocations 256-B aligned (§1.6).
- An `NS::AutoreleasePool` wraps each `InitDraw…FinishDraw` span and each `NextFrame`
  (drawable and command-buffer objects are autoreleased in metal-cpp).

### 6.2 Constant streaming (replaces GL's mutate-in-place UBO)

GL33 mutates one UBO between draws (`FlushVSConstants` memcmp guard,
`EngineGL33_Shaders.cpp:782-835`). Metal must never mutate memory the GPU may still
read → **snapshot-on-dirty**:

- Keep the exact CPU shadow arrays (`s_vsShadow[280]`, `s_psShadow[108]` equivalents) and
  all existing `Upload*` entry points (`UploadVSMaterialConstants`, `UploadVSLights`,
  `UploadVSTexGenConstants`, `UpdateShadowMapLitState`, …) writing into them; each write
  sets a dirty flag instead of calling glBufferSubData.
- At draw emission: if dirty, bump-allocate, `memcpy` the block, remember the
  `{buffer, offset}` pair, clear dirty. Then bind: if the buffer matches the cached
  binding for that slot, `set*BufferOffset(offset, idx)`; if the buffer differs (first
  bind of the span, or the allocation landed in an overflow buffer),
  `set*Buffer(buffer, offset, idx)`. Clean draws reuse the previous pair — zero copies,
  at most one offset call, or nothing at all if unchanged.
- **The encoder-side binding cache keys on `{MTL::Buffer*, offset}` pairs per slot
  index** — never on offset alone. Overflow allocations (§6.1) and slot rotation both
  change the buffer identity; an offset-only cache would silently bind into the wrong
  buffer. All pairs are invalidated at slot rotation (§6.1) and at encoder open (§6.4).
- The GL world-matrix-only fast path (`UploadVSWorldMatrix` 64-byte sub-upload,
  `EngineGL33_Shaders.cpp:1172-1196`) becomes unnecessary: the world matrix lives in the
  WorldInstances stream (§6.6), and `SlotWorld` remains dead padding exactly as in GL.
- **Mid-pass projection changes** (`UpdateProjection`, called from TransportCore —
  GL33: `EngineGL33_Draw.cpp:80-91` flushes queues, then converts + uploads the new
  projection): the Metal port keeps the exact ordering — flush the pending queue draws
  first (they encode against the *old* VSConstants snapshot), then write the new
  projection into the CPU shadow copy (dirtying VSConstants) so the next draw snapshots
  it. Because constants are immutable per-draw snapshots, no encoder split is needed;
  correctness is purely the flush-before-write ordering. Verified by the M2
  cockpit/interior depth-transition check (§9).

### 6.3 PSO cache & depth-stencil states

- **PSO key** (packed `uint32_t`) — every field that is baked into a
  `MTLRenderPipelineDescriptor` must be in the key; a two-PSO situation that hashes to
  one key is a correctness bug, not a perf bug. Fields and widths (with
  `static_assert(kCount <= (1u << kBits))` for each enumerated field):

  | Field | Bits | Values |
  |---|---|---|
  | `vsID` | 3 | 6 VS entry points (§5.2) |
  | `psID` | 4 | 9 PS entry points (§5.2) |
  | `blendMode` | 2 | Opaque / AlphaBlend / Additive / **Shadow** (mirrors `render::blend`, `GLBlendState.hpp`) |
  | `colorWriteMask` | 4 | RGBA bits (used by the depth/stencil-clear draw §6.4; default all-on) |
  | `vertexLayout` | 3 | TLVertex / SVertex / ShadowDepthSolid (XYZ) / ShadowDepthAlpha (XYZUV) / None (blit fullscreen-triangle, generated in-shader) |
  | `attachmentConfig` | 2 | **FramePass** = color BGRA8 + `depth32Float_stencil8`, sampleCount N; **PresentPass** = color BGRA8, **no depth-stencil**, 1×; **CascadePass** = no color, `depth32Float`, 1× — these must never alias: a PSO built for FramePass is invalid in the drawable/composite pass and vice versa |
  | `sampleCount` | 2 | log2(1/2/4/8) — includes the configurable **2×** mode; actual availability clamped per §6.5 |
  | `alphaToCoverage` | 1 | on/off |

  Total 21 bits — fits `uint32_t` with headroom. Depth/stencil state, viewport,
  scissor, cull, stencil-reference are dynamic encoder state, **not** PSO state — they
  stay out of the key.
- Prebuild the valid set eagerly at init (the ~18 stage pairs × the blend modes each is
  actually drawn with × sample-count variants for the current MSAA setting + the blit
  and clear-draw variants — well under 100 PSOs; Apple-silicon compile of this set is
  expected sub-second). Runtime lookup is a flat
  `std::unordered_map<uint32_t, retained PSO>` hit; a miss builds synchronously
  **and logs LOG_WARN** (a miss means the prebuild enumeration has a gap — fail loud).
  `MTLBinaryArchive` is optional M6 polish if cold-start ever measures slow.
- **Depth-stencil states** (immutable, built at init) mirror `render::depthstencil`
  (`GLDepthStencilState.hpp:24-88`) — **the code path, not the stale `Engine.hpp`
  comment** (see below):

  | State | Depth | Stencil (ref always 0) |
  |---|---|---|
  | `Normal` | LEQUAL, write **on** | ALWAYS, pass=REPLACE (writes 0 — clears shadow stencil for any rendered pixel) |
  | `ReadOnly` | LEQUAL, write off | ALWAYS, pass=REPLACE (0) |
  | `Disabled` | ALWAYS, write off | ALWAYS, pass=REPLACE (0) |
  | `Shadow` | LEQUAL, write **off** | **EQUAL (ref 0), pass=IncrementClamp**, fail/zfail=KEEP — a pixel darkens at most once per caster set even under overlapping shadow polys |
  | `ClearDepthStencil` | ALWAYS, write **on** | ALWAYS, pass=REPLACE (0), writeMask 0xFF — used only by the mid-frame clear draw (§6.4) |

  Since every state uses stencil reference 0, `setStencilReferenceValue(0)` is set once
  at encoder open and never touched per-draw.
- **Projected-shadow algorithm (ground truth, drives M5):** the live GL33 path is a
  **per-poly multiplicative darken**, not a fullscreen quad. Casters between
  `BeginShadowPass`/`EndShadowPass` draw with `depthstencil::Shadow`
  (`GLDepthStencilState.hpp:75-88`: depth LEQUAL, depth-write off, stencil EQUAL ref 0,
  KEEP/KEEP/INCR-saturating) **plus** `blend::Shadow` (`GLBlendState.hpp:38-42`:
  `(ZERO, ONE_MINUS_SRC_ALPHA)` on color, `(ONE, ZERO)` on alpha) **with color writes
  ON** — each shadow poly directly multiplies the framebuffer by `1 − src.a`. The live
  producer is `Scene::DrawObjectsAndShadowsPass2` (`SceneDraw.cpp:1749-1760`), which
  brackets **each caster** with flush-only `BeginShadowPass`/`EndShadowPass`.
  `BeginShadowPass`/`EndShadowPass` **only flush queues**
  (`EngineGL33_Draw.cpp:94-110`); they set no state. There is **no fullscreen darken
  quad and no REPLACE-0xFF/EQUAL-0xFF pass** — verified at every layer:
  `FramePassKind::ShadowDarken` is **never emitted** by `BuildFrame.cpp` (only
  `ShadowAccum::tryEmit` exists; `ShadowDarken` survives only in tests and the
  `ValidateFrame` rank table, `ValidateFrame.cpp:98`), and the
  `ShaderFamily::Flat`/`VSScreen` "darken quad" combination has **zero live
  producers**. The `Engine.hpp:457-466` doc comment describing that scheme is stale (it
  documents a design the code no longer uses; flag it, don't port it — full inventory
  below). Metal mapping: one `Shadow` depth-stencil state (above) + the
  `Shadow` blend mode baked into the caster PSO variants
  (`vsShadow`+`psShadow`, blend=Shadow); the brackets flush the queue path and nothing
  else. M5 parity gates on the GL33 **code path** (§9).
- **Stale-comment inventory — the dead fullscreen-darken / REPLACE-0xFF scheme.** The
  following comments all still narrate that dead design. **None of it is live.** During
  the port: flag each in review (do not port the described behavior); the implementer
  may fix the comments in a separate mechanical commit, but must not change any code
  they sit on:
  | Location | Stale claim |
  |---|---|
  | `Poseidon/Graphics/Core/GLBlendState.hpp:36-37` | `blend::Shadow` "used by the stencil-shadow fullscreen darken quad" — the blend func itself (`:38-42`) is live, per-poly |
  | `PoseidonGL33/EngineGL33_State.cpp:289` | `ShaderFamily::Flat` "used by the fullscreen darken quad" — zero live producers of that combination |
  | `PoseidonGL33/EngineGL33_Shaders.cpp:670-679` | `psShadow` source comment reasons from "Phase 3's REPLACE 0xFF stencil" and "EndShadowPass's fullscreen darken" — the invariant it protects (discard suppresses stencil write) is real; the scheme it cites is dead. §5.2's INCR-based restatement is a **deliberate** reinterpretation |
  | `PoseidonGL33/EngineGL33_VertexBuffer.cpp:256-257` | `EmitDraw` inline-emission rationale mentions "the shadow darken quad" ordering |
  | `Poseidon/Graphics/Rendering/RenderPassDescriptor.hpp:173` | `ShaderFamily::Flat` "(fullscreen darken quad, debug overlays)" |
  | `Poseidon/Graphics/Rendering/Frame/Frame.hpp:120` (+ `BuildFrame.hpp:20` order comment, `ValidateFrame.cpp:98` rank table) | `FramePassKind::ShadowDarken` "fullscreen darken quad (consumes shadow stencil)" — the enum value is never emitted by `BuildFrame.cpp`; it exists only in tests + the validator rank table |
- `ApplyPipeline(const RenderPassDescriptor&)` keeps GL33's dedup shape
  (`EngineGL33_State.cpp:155-171`): same `_lastApplied` early-out, then resolves
  {PSO, DS state, cull/winding, sampler, fog/alpha constants} and sets only what
  changed (encoder-local state cache in `EncoderBroker`).

### 6.4 Encoders per frame phase (`EncoderBroker`)

Metal render encoders are bound to one render target set; the engine's immediate-mode
call pattern (shadow-map passes arrive mid-frame from the scene) needs lazy encoder
management:

- `EncoderBroker::Ensure(TargetId)` — if the current encoder targets `TargetId`, return
  it; else `endEncoding()` and open a new `MTLRenderCommandEncoder` with
  `loadAction=Load` (except first open per target per frame: `Clear` per `Clear()`
  semantics), then **replay sticky state** (viewport, scissor — broker-internal only,
  full-target by default (§6.7), DS state, stencil ref 0, PSO,
  `{buffer,offset}` bindings at indices 0/1/2 (§6.2), samplers, cull) from the broker's
  cache. Encoder open always invalidates the encoder-local caches first.
  **`TargetId` includes the array slice:** `TargetId = {texture, layer}` — cascade
  shadow layers are distinct targets. The first open of each `{cascade array, layer}`
  per frame gets `depthLoadAction=Clear`; without the layer in the id, a later cascade
  layer would count as "already opened this frame" and `Load` the previous layer's (or
  last frame's) stale depth.
- **Load/store actions (explicit, per attachment).** Metal's default
  `storeAction=DontCare` **discards** the attachment at encoder close on TBDR — every
  target that a later encoder resumes via `Load`, or that is ever sampled/blit-read,
  MUST close with `Store`. The broker owns this table; no encoder is opened with
  defaults:
  | Attachment | Open (first per frame / resume) | Close (intermediate) | Close (terminal) |
  |---|---|---|---|
  | FramePass color, 1× | `Clear` / `Load` | `Store` (resumed later + sampled by composition/readback) | `Store` |
  | FramePass color, MSAA (+ resolve tex attached) | `Clear` / `Load` | `Store` (MSAA contents must survive mid-frame encoder splits — no resolve yet) | `MultisampleResolve` (resolve to 1× **only** at the frame's terminal close in `FinishDraw`; MSAA contents then discardable) |
  | FramePass depth-stencil (sampleCount = color's, §6.5) | `Clear` / `Load` | `Store` (resumed by later encoders) | `DontCare` (nothing reads frame depth after the frame; depth is **never** resolved) |
  | Cascade depth `{array, layer}` | `Clear` (always fresh per frame, per-layer) / `Load` (same-layer resume) | `Store` | `Store` (sampled by the lit pass; blit-read by probes/dumps) |
  | CompositeTarget color | `DontCare` (fullscreen downsample draw covers it) | — (single encoder) | `Store` (sampled by present pass + capture) |
  | Drawable (PresentPass) | `DontCare` (fullscreen blit covers it) | — | `Store` (presented) |
  Terminal-vs-intermediate close is decided **at close time**, not open time (a
  mid-frame target switch cannot know whether the frame is over): FramePass attachments
  open with `storeAction=Unknown` and the broker calls
  `setColorStoreAction(_:index:)` / `setDepthStoreAction(_:)` /
  `setStencilStoreAction(_:)` before `endEncoding()` (macOS 10.12+, verified — §1.6;
  `Unknown` requires exactly this pattern). Documented fallback if the Unknown path
  misbehaves: close every MSAA encoder with `StoreAndMultisampleResolve` — correct but
  pays a full-screen resolve per mid-frame target switch (cascade interleaving makes
  that k+ resolves/frame); record in GOTCHAS if taken.
- **Mid-frame depth/stencil-only clear** — `Clear(bool clearZ, bool clear, PackedColor)`
  (`EngineGL33_VertexBuffer.cpp:351`) is a **live mid-frame API**: `Clear(true, false, …)`
  clears depth+stencil while preserving color. Metal render passes can only clear via
  `loadAction` at encoder open, so the broker needs an attachment-aware path.
  **Decision:** implement it as a **dedicated fullscreen clear draw** on the open
  encoder: `vsBlitScale` triangle at z = 1.0 (the clear depth), PSO variant with
  `colorWriteMask = 0` + `attachmentConfig = FramePass`, DS state `ClearDepthStencil`
  (§6.3 — depth ALWAYS+write, stencil ALWAYS REPLACE 0). **The clear draw must clear
  the whole target regardless of ambient state:** bracket it with full-target
  viewport + full-target scissor + `cull=None` (a cropped world viewport, a narrower
  scissor, or a culling mode may be active when `Clear()` arrives — any of them would
  silently clip the clear), then restore the broker's sticky state by replaying the
  cached viewport/scissor/cull/PSO/DS values. *Why:* on TBDR, splitting the
  encoder forces a full color store+load memory round-trip per clear; the draw keeps the
  pass on-chip. *Rejected (kept as documented fallback):* end the encoder and reopen
  with `depth/stencilLoadAction=Clear`, `colorLoadAction=Load` — correct but pays the
  bandwidth; switch to it only if the clear-draw shows artifacts (record in GOTCHAS).
  A mid-frame **color** clear request (not currently emitted mid-frame) uses the same
  mechanism with color writes on and depth/stencil masked off.
- **Cropped world viewport** (`ApplyWorldViewport`/`EndWorldViewport`,
  `EngineGL33_State.cpp:565-640`, called from the queue seam `EngineGL33_Queue.cpp:356/368`
  and the `FinishDraw` safety restore `EngineGL33_Lifecycle.cpp:192`) — port the full
  contract: `ApplyWorldViewport` computes the AspectSettings world sub-rect in
  **FrameTarget-relative** dimensions (`RenderTargetSize`, i.e. render-scale-sized, not
  window-sized) and sets the encoder viewport; Metal's `setViewport` origin is already
  **top-left**, so the GL bottom-left flip (`vpH - y1`) is *removed* — use `y0`
  directly. Update the `MinGuardX`/`MaxGuardX`/`MinGuardY`/`MaxGuardY` and
  `MinSatX`/`MaxSatX`/`MinSatY`/`MaxSatY` accessors with the ported viewport state:
  preserve GL33's extended guard band (for example `_minGuardX = -maxBand`,
  `_maxGuardX = _w + maxBand`, `EngineGL33_State.cpp:473-476`) rather than the base
  class's tight `0..Width()` / `0..Height()` defaults. This is observable at screen
  edges because `TransLight.cpp:296` uses `MinSatX`/`MaxSatX` for light-saturation
  clipping. `EndWorldViewport` (first 2D draw after a cropped 3D pass, and the
  `FinishDraw` safety restore) restores the full viewport and paints the periphery
  black: Metal has no scissored clear, so emit up to four scissored fullscreen-triangle
  clear draws (blit PSO, solid black) or equivalent solid-fill 2D quads — same four
  edge rects as GL. Verified by pillarbox A/B coverage at M2 (§9).
- Typical frame produces this encoder sequence:
  1. *k* × cascade shadow-depth encoders (`RenderShadowDepthScene` → depth-only target,
     one encoder per cascade layer via `MTLRenderPassDescriptor` depth slice);
  2. main frame-target encoder: opaque + cutout (Pass 1), per-poly projected-shadow
     draws (`BeginShadowPass` … casters with Shadow DS/blend … `EndShadowPass` — flush
     brackets only, §6.3), transparent/light/water, cockpit, sky — all one encoder
     (same target, state switches only; mid-frame `Clear(true,false)` handled by the
     clear draw above without splitting);
  3. screen-space/HUD draws continue on the same frame-target encoder (`VSScreen` PSOs);
  4. *(in `NextFrame`, separate command buffer)* composition pass into the
     `CAMetalDrawable` texture (§6.5).
- `BeginDebugGroup`/`EndDebugGroup` map to `pushDebugGroup`/`popDebugGroup` on the
  current encoder (shows pass structure in Xcode GPU captures — replaces the RenderDoc
  story, which has no macOS capture).

### 6.5 Offscreen frame target, composition & readbacks

Always render the 3D+HUD frame into an offscreen **FrameTarget**
(`BGRA8Unorm`, private storage; when MSAA is on: MSAA color texture + single-sample
resolve texture, resolved via the terminal-close `MultisampleResolve` store action —
§6.4 — free on TBDR; sized × renderScale for SSAA), mirroring GL33's
offscreen-frame-target design (`EngineGL33.cpp:293-299`).

**Created-target inventory (explicit `MTLTextureUsage` + sample counts).** Metal's
default usage is `ShaderRead` only — a target created without `RenderTarget` usage
fails at encoder open, and one without `ShaderRead` fails at first sample. Every
backend-created target declares its usage explicitly:

| Target | Format | Samples | Usage | Notes |
|---|---|---|---|---|
| FrameTarget color, 1× (no MSAA) | `BGRA8Unorm`, private | 1 | `RenderTarget \| ShaderRead` | sampled by composition; blit-read by captures |
| FrameTarget MSAA color | `BGRA8Unorm`, private | N | `RenderTarget` only | never sampled — resolved at terminal close (§6.4) |
| FrameTarget resolve texture | `BGRA8Unorm`, private | 1 | `RenderTarget \| ShaderRead` | resolve destination; the sampled/captured surface under MSAA |
| FrameTarget depth-stencil | `depth32Float_stencil8`, private | **N — always the SAME sample count as the FramePass color** | `RenderTarget` only | a 1× depth attached to an N× color pass is an API validation error; **rebuilt together with the color targets on every MSAA change** (frame boundary, §6.1); only color resolves to 1× — depth is never resolved (§6.4) |
| CompositeTarget | `BGRA8Unorm`, private | 1 | `RenderTarget \| ShaderRead` | sampled by present pass; blit-read by captures |
| Cascade depth array | `depth32Float`, private | 1 | `RenderTarget \| ShaderRead` | sampled by the lit pass; blit-read by `ShadowDepthProbe`/`DumpShadowMap` |
| Bank/dynamic textures (incl. font atlas) | per §7.1 | 1 | `ShaderRead` (default) | blit upload needs no usage bit |
| Drawable | `BGRA8Unorm` | 1 | — (`framebufferOnly=true`) | never read (§4.1) |

**Composition & capture (mirrors `BackToFront`, `EngineGL33_VertexBuffer.cpp:565-596`):**

- When `renderScale == 1`: `NextFrame` encodes one `vsBlitScale` pass sampling the
  FrameTarget (resolved) straight into the drawable (`PresentPass` attachment config).
- When SSAA is active: a persistent window-sized **CompositeTarget** (`BGRA8Unorm`,
  private, 1×) plays the role of GL33's default framebuffer: the downsample pass
  FrameTarget→CompositeTarget is the `ResolveSSAAToDefault` analogue (idempotent per
  frame — a flag resets at `InitDraw`), and a second 1:1 pass CompositeTarget→drawable
  presents it. The downsample runs lazily: `NextFrame` runs it always; a readback in the
  FinishDraw→NextFrame gap forces it early (GL33 does exactly this — `SamplePixel`
  calls `ResolveSSAAToDefault` itself, `EngineGL33_VertexBuffer.cpp:606,639`).
- **CaptureSource** := CompositeTarget when SSAA active, else the FrameTarget's resolved
  texture. It is always window-sized and post-resolve. The drawable itself is **never**
  read (`framebufferOnly=true` stands).

**Readback semantics (must match GL33 exactly):**

| Op | GL33 anchor | When | Source | Semantics |
|---|---|---|---|---|
| `SamplePixel(x,y)` | `EngineGL33_VertexBuffer.cpp:633-657` | between `FinishDraw` and `NextFrame` (trident) | CaptureSource | window-size, post-resolve, **pre-overlay**, top-left coords (GL flips `y` for `glReadPixels`; Metal readback rows are already top-down → the flip is **removed** at the conversion point) |
| `SampleBackBufferNonBlack` | `:598-631` | same gap | CaptureSource | 16×16 grid sample, same conversion |
| `Screenshot` (triScreenshot) | `CaptureScreenshotIfPending`, `:528-563`, called from `BackToFront:587` | inside `NextFrame`, **pre-present, post-overlay** | CaptureSource | final user-visible pixels; row order top-down (GL flips; Metal doesn't) |
| `ShadowDepthProbe` / `DumpShadowMap` | `EngineGL33_ShadowDepth.cpp` | after cascade pass | cascade depth array | keep the documented row-0-=-bottom contract by flipping at the CPU conversion point |

- **Conversion codecs — one per source format, both on `CopyLayout`.** The four ops
  read two different formats, so there are **two** codecs, not one:
  - **Color codec** (`BGRA8` → harness bytes) — shared by `SamplePixel`,
    `SampleBackBufferNonBlack`, and `Screenshot`: capture sources are BGRA8 while
    `glReadPixels(GL_RGBA)` returned RGBA → swizzle BGRA→RGB(A); rows are already
    top-down (no GL flip). One function, three call sites — the M1 A/B screenshot
    compare validates the codec, not per-call-site reasoning.
  - **Depth codec** (`Depth32Float` → probe/dump output) — shared by
    `ShadowDepthProbe` and `DumpShadowMap`: 4-byte float texels (float stride and
    row handling, no channel swizzle), CPU-side row flip to keep the documented
    row-0-=-bottom contract, plus `DumpShadowMap`'s float→visualization mapping.
  - Both codecs compute strides/offsets through the **central `CopyLayout` helper**
    (§3.1): one format-aware source of truth for texel/block sizes, `bytesPerRow` /
    `bytesPerImage` (including BC block-row math: `ceil(w/4) × blockBytes` per row,
    `ceil(h/4)` block rows), mip dimension rounding, and aligned mip offsets in staging
    buffers. The same helper is used by texture uploads (§7.3), screenshots, probes,
    and dumps — a stride bug can only exist in one place.
- **Mechanics & ordering vs open command buffers:** readback = transient command buffer
  on the same queue: (optional forced downsample pass) + `MTLBlitCommandEncoder`
  copy texture→shared `MTLBuffer` + `commit` + `waitUntilCompleted`. In the
  FinishDraw→NextFrame gap the frame CB is already committed, and same-queue ordering
  guarantees the copy sees the finished frame. If a readback is ever requested
  **mid-frame** (frame open): slow path — end encoders, commit the current span CB,
  open a continuation CB in the same slot (the span's semaphore signal stays attached
  to the *last* CB, i.e. moves to the continuation — same-queue serial execution keeps
  the guarantee), log once. Never sample an open command buffer's target without this.

**MSAA capability handling:** at init and on every setting change, query
`device->supportsTextureSampleCount(n)` for the requested count (the configurable modes
include **2×**, which is not guaranteed on all devices). Clamp deterministically to the
nearest supported lower count (8→4→2→1), log the clamp once. Applying an MSAA or
render-scale change rebuilds only the affected targets and PSO variants, and **only at
the frame boundary** (the `NextFrame` frame-boundary block, §6.1) — never mid-frame.

*Why not render straight into the drawable:* (a) `nextDrawable()` can block — acquiring
it only for the final cheap pass in `NextFrame` minimizes stall exposure; (b) MSAA/SSAA
need an offscreen target anyway; (c) the FinishDraw/NextFrame lifecycle contract
requires a completed, readable frame *before* any drawable exists; (d) it matches GL33's
architecture, keeping A/B behavior aligned.

Residency: no heaps, no argument buffers → all bound resources are automatically
resident; no `useResource` bookkeeping anywhere.

### 6.6 The per-draw contract: `EmitDraw`, handle registries, instanced runs

**Handle registries (the GL-name impedance mismatch):** `render::frame::Draw` references
resources through **32-bit ids** — `TextureHandle{uint32_t id}` and
`MeshHandle{uint32_t vao; BufferHandle vbo, ibo}` (`Frame.hpp:38-56`); GL stores GL
object names in them directly. Metal objects are pointers, so the backend introduces
**generation-checked registries** (`HandleRegistry.hpp`, §3.1) — never cast ids to
pointers, never assume pointer-size handles:

```cpp
// HandleRegistry<T>: uint32_t handle = {generation:8, slotIndex:24}.
// Allocate → returns handle; Release → bumps generation, recycles slot;
// Resolve(handle) → T* or nullptr on stale generation (assert + fallback).
// Handle value 0 is RESERVED and never allocated (skip slot 0 or start
// generations at 1): 0 is the null/invalid handle, and the frame validator
// counts any TL DrawItem with mesh.vao == 0 as a structural bug
// (DetectMissingMeshHandles, RuntimeChecks.hpp) — a registry that can
// legitimately hand out 0 would trip it on valid draws.
```

- Mesh registry: id → `{MTL::Buffer* vertices, MTL::Buffer* indices, sizes}` — the
  `MeshHandle.vao` field carries the registry handle; `vbo/ibo` ids are subsumed (one
  registry entry owns both buffers, mirroring the GL VAO+VBO+IBO travel-together rule).
- Texture registry: id → `MTL::Texture*` (+ bank bookkeeping). `TextureMetal` stores the
  registry handle where `TextureGL33` stores the GL name.
- Stale-handle resolve returns nullptr → assert in debug, fallback object in release
  (fallback white texture below / skip draw for meshes) — fail loud, never crash.

**Fallback textures:** two persistent **1×1 opaque-white `MTLTexture`s** created at init
and bound whenever texture slot 0/1 has a null or unresolved texture — mirrors
`_fallbackWhiteTex` (`EngineGL33_ShadowDepth.cpp:430-432`, also used on the lit path).
They live outside the registries and survive `ResetForRemount` (§7.3).

Metal mirror of `EngineGL33::EmitDraw` (`EngineGL33_VertexBuffer.cpp:304-349`):

```cpp
void EngineMetal::EmitDraw(const render::frame::Draw& d) {
    // encoder guaranteed by ApplyPipeline path; bail on d.mesh/indexCount == 0
    BindMeshBuffers(d);                  // registry resolve + vertex buffer + set*Texture (cached)
    SnapshotConstantsIfDirty();          // §6.2 — {buffer,offset} pairs
    BindWorldSlot(d.world);              // see below
    enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, d.indexCount,
                               MTL::IndexTypeUInt16, meshIndexBuffer,
                               d.indexBegin * sizeof(VertexIndex),
                               _instCount > 1 ? _instCount : 1);
    RecordDrawItem(d);                   // DrawItem recording — see below
    ++Poseidon::gPerfDrawCalls;
}
```

**DrawItem recording (`GetRecordedDraws` parity — M2 gates on it).** GL33 records
every draw into a per-frame `std::vector<DrawItem> _drawItems` at **two seams**: the
mesh/TL emission path (`EngineGL33_VertexBuffer.cpp:276`, inside `EmitDraw`) and the
queue-flush path (`EngineGL33_Queue.cpp:143`); cleared per frame at `InitDraw`
(`EngineGL33_Lifecycle.cpp:173`) and on queue reset (`EngineGL33_Queue.cpp:345`);
exposed via the `GetDrawItemCount()` / `GetRecordedDraws()` overrides
(`EngineGL33.hpp:536-537`). The base `Engine::GetRecordedDraws` returns `nullptr`
(`Engine.hpp:510`) — without the override, `SceneExtractor` (`SceneExtractor.cpp:185`)
and the `WorldFrameObserver` runtime checks silently see no draws and the frame
validator goes blind. `EngineMetal` mirrors all of it 1:1: record at both seams (its
`EmitDraw` and its queue flush), clear at `InitDraw`, override both getters. TL
DrawItems carry the mesh **registry handle** in `mesh.vao` — never 0 for a live mesh
(registry rule above), which is exactly what `DetectMissingMeshHandles` asserts.

**GPU-error getters (harness parity — M1).** Override `GetDebugErrorCount()` and
`GetLastDebugMessage()` (`EngineGL33.hpp:538-539`) instead of inheriting the base
`0` / empty defaults. Maintain a counter and last-message store from command-buffer
completion failures (`MTLCommandBufferError`) and Metal validation-layer errors; the
`MTL_DEBUG_LAYER=1` verification path feeds the same store. This preserves the
`triResetGLErrorBaseline` / `triGetGLErrorCount` contract
(`GameStateExtTest.cpp:2269,2281`; `SceneExtractor.cpp:179-180`) so the harness's
“zero new GPU errors” gate cannot silently pass when Metal reports an error.

- **WorldInstances becomes a device-pointer stream** (`const device float4x4*` at
  `buffer(2)`), not a fixed 16 KB std140 block — Metal has no UBO-style minimum-range
  requirement, so a scalar draw allocates **64 B** from the ring and an instanced run
  allocates `count × 64 B` (≤ 256 kept as the engine-side cap for parity). Binding is
  the `{buffer, offset}` pair of the run's allocation (§6.2).
- **Instanced-run protocol** (`InstancedRunReset/Add/BeginInstancedRunUpload/EndInstancedRun`
  + `_instCount`): identical semantics to GL33. `UploadWorldInstances` allocates the run's
  block once and binds it; **`BindWorldSlot` must keep that binding for every
  section while `_instCount > 1`** — rebinding a scalar 64 B slot mid-run would make
  `instance_id > 0` read out-of-run memory. This is the Metal twin of the GOTCHA
  ("WorldInstances range must stay bound throughout an instanced run"); encode it as a
  debug assert (`PoseidonAssert(_instCount <= 1 || offset == _runOffset)`).
- Per-draw cost inventory (all cached/offset-only): ≤1 PSO set (usually 0), ≤2 texture
  sets, ≤2 buffer-offset sets, 1 draw call → comfortably under the 5 µs budget;
  materials/lights that dirty VSConstants add one 1120 B memcpy.

### 6.7 Queue path, 2D clipping, samplers & test patterns

- The vertex-soup queue (`EngineGL33_Queue.cpp` — per-texture `TriQueue`s over a TLVertex
  stream with buffer orphaning at `:430-433`) ports to ring suballocations: vertices and
  16-bit indices are bump-allocated per flush; no orphaning dance exists in Metal —
  the ring *is* the orphaning. `FlushQueues` draws with `VSScreen` PSOs.
- **2D clipping — Decision: retain GL33's CPU clipping; no scissor on the 2D path.**
  GL33 does **true geometric CPU clipping**, not scissoring: `EngineGL33::DrawPoly`
  (`EngineGL33_2D.cpp:169-239`) rejects fully-outside polys and clips partially-outside
  ones vertex-by-vertex against the `Rect2DPixel` clip via the **shared**
  `Clip2D.hpp` (`engine/Poseidon/Graphics/Rendering/Primitives/`) *before* anything is
  enqueued — so per-texture `TriQueue` batches never carry clip state, and draws with
  different clip rects batch together freely. `EngineMetal_2D.cpp` ports this verbatim
  (the clip helpers are already backend-neutral).
  *A/B implication:* CPU clipping produces byte-identical clipped geometry to GL33 —
  2D screenshot parity is exact, and clip rects + vertices live in the same coordinate
  space, so no window→render-target conversion exists to get wrong.
  *Rejected:* mapping `Rect2DAbs` clips to `setScissorRect` — it would break
  per-texture batching (batches would need keying/splitting by clip rect), add scissor
  to the broker's sticky-state replay, and rasterize clip edges differently than GL33
  (A/B diffs at every clipped border). If a scissor path is ever introduced (the
  documented fallback), the window-pixel → render-target-pixel conversion is:
  scale by `RenderTargetSize / window size` per axis (= renderScale), **floor** the
  origin, **ceil** the far edge (conservative cover), clamp to `[0, targetW/H]` —
  Metal raises a validation error on a scissor rect that exceeds the render target, so
  clamping is mandatory, not cosmetic.
  Scissor remains in use only for broker-internal full-target/edge-rect draws (§6.4
  mid-frame clear, `EndWorldViewport` periphery), whose rects are computed directly in
  FrameTarget pixel space.
- **Dynamic textures (font atlas):** `TextureBankMetal` implements the backend-neutral
  `CreateDynamic`/`UpdateDynamic` virtuals (`TextureBank.hpp:168/:173`; consumer:
  `FontDrawFreeType.cpp` — §3.4): `CreateDynamic` builds an `RGBA8Unorm` private
  texture (the atlas pages are raw RGBA bytes; usage `ShaderRead`, mipmap off per the
  call site), `UpdateDynamic` re-uploads the full page via the standard staging-buffer
  blit on the shared queue (§7.3). Mid-frame updates are safe without fences: the blit
  CB commits before the consuming frame CB (§7.3), and same-queue serial execution
  orders it after any in-flight frame still sampling the old contents.
- **`DrawTestPattern(const char*)`** (base no-op, `Engine.hpp:375`; GL33:
  `EngineGL33_Lifecycle.cpp:226`) — harness-only named patterns drawn via the screen
  path: `"colorbar"` (5 vertical bars: red, green, blue, yellow, magenta;
  depth Disabled) and `"gradient3d"` (fullscreen corner-gradient quad at z=0.5; depth
  Normal). The trident render tests call it and assert per-bar RGB / gradient corners
  (`GameStateExtTest.cpp:1951-2072`) — with the base no-op, those tests read back an
  empty frame under Metal. Port: `VSScreen`+`PSFlat` PSO, opaque blend, cull None,
  TLVertex quads from the ring — lands in M1 with its harness assertions.
- Samplers: 8 immutable `MTLSamplerState`s mirroring `CreateSamplerStates`
  (`EngineGL33_State.cpp:15-46`): {linear-trilinear+aniso16 | nearest} ×
  {wrap/clamp U} × {wrap/clamp V}; `maxAnisotropy` from device (cap 16). Slot-0 per-draw
  sampler selection mirrors `ApplySamplerState`; slot 1 keeps default linear-wrap
  (same asymmetry as GL — documented there as intentional).

---

## 7. Textures

### 7.1 Format mapping (`TextureMetal_Init.cpp`, from `InitGLPixelFormat` + `DstFormat`, `TextureGL33_Init.cpp`)

**The `DstFormat()` resolution step is part of the pipeline and must be ported**
(`TextureGL33_Init.cpp:50-74`, applied in `DoLoadHeaders`): every source format is
resolved to a destination format *before* any upload site runs, and **all upload call
sites see the resolved DstFormat only**. In particular **P8 is a live format** — palette
textures resolve `PacP8 → PacARGB1555` at header-load time and the palette expansion
happens in the shared decode path; the GL `Fail("Palette textures obsolete")` branch is
**dead code** (unreachable after DstFormat resolution — do not port it, do not add a
Metal equivalent). `PacAI88` resolves conditionally through the backend caps
(`Can88`/`Can8888`); the Metal backend answers `Can88() == true` (RG8 exists) so AI88
stays AI88.

| PacFormat (resolved Dst) | GL33 | Metal | Upload |
|---|---|---|---|
| DXT1 | `COMPRESSED_RGBA_S3TC_DXT1` | `MTL::PixelFormatBC1_RGBA` | raw blocks, staging-buffer blit (§7.3) |
| DXT2/3 | `..._DXT3` | `BC2_RGBA` | raw blocks |
| DXT4/5 | `..._DXT5` | `BC3_RGBA` | raw blocks |
| ARGB1555 (incl. **all P8 sources**, resolved at header load) | `GL_RGB5_A1` | **CPU-expand → `RGBA8Unorm`** | avoids packed-component-order bugs (§1.3) |
| RGB565 | `GL_RGB565` | **CPU-expand → `RGBA8Unorm`** | rare |
| ARGB4444 | `GL_RGBA4` | **CPU-expand → `RGBA8Unorm`** | rare |
| AI88 | `GL_RG8` | `RG8Unorm` + shader swizzle as in GL | check GL sampling swizzle parity at M1 |
| ARGB8888 | `GL_RGBA8` (+ GL-side source-format handling) | **`BGRA8Unorm` — direct** | PAA ARGB8888 payloads are read as **raw bytes** (`Pactext.cpp:1200-1233`), i.e. little-endian packed ARGB = **B,G,R,A byte order in memory**. Uploading as `BGRA8Unorm` consumes them as-is; do **not** label this RGBA8 and do not swizzle twice |
| ~~P8 → R8Unorm~~ | — | **removed** — P8 never reaches an upload site as P8 (DstFormat resolves it to ARGB1555 above) | — |

*Decision — expand 16-bit packed on CPU:* the native `b5g6r5`/`bgr5A1`/`abgr4` formats
exist on Apple GPUs (§1.3) but their component order differs from the GL formats the PAA
decoder currently targets; a one-time 2× memory expansion at load beats a class of
channel-swap bugs. Revisit only if texture memory ever measures as a problem.
Startup assert: `PoseidonAssert(device->supportsBCTextureCompression())` — documents the
Apple-silicon-only assumption at the single place it matters.

**Interpolated textures — raw BC upload is for non-interpolated textures ONLY.** A
second resolution step sits after `DstFormat`: `UploadFormatForTextureGL33(format,
interpolate)` (`TextureGL33_Loading.cpp:36-41`, applied in `InitDesc` `:47`) forces
`PacARGB1555` whenever `_interpolate && IsCompressedInterpolationFormat(format)`
(DXT1–5) — interpolated (CPU-blended) textures must be uncompressed because **BC
blocks cannot be blended in place on the CPU**; the shared decode path
(`DecodePAABuffer`) decodes them and the blend operates on uncompressed texels. The
Metal port mirrors this exactly (`UploadFormatForTextureMetal`): same predicate, same
`PacARGB1555` resolution — which then takes the §7.1 CPU-expand→`RGBA8Unorm` upload
row. A port that only implements the table above would raw-upload BC blocks for
interpolated textures and corrupt every CPU-blended frame. Lands in M1 with the bank
port.

### 7.2 Mixed-format PAA mips

The GL path decodes any mip whose format differs from the surface format via
`DecodePAABuffer` (mixed-format PAAs are "rare but legal" —
`TextureGL33_Loading.cpp:92-95`). Keep that exact logic: the shared PAA decoder is
backend-agnostic; only the terminal upload call changes.

### 7.3 Upload path, ordering, mip streaming & remount

- Textures are **private storage** `MTLTexture`s. Uploads go: transient **shared
  staging buffer** → `MTLBlitCommandEncoder::copyFromBuffer(toTexture:)` per mip level,
  on a dedicated upload command buffer created on demand by the texture bank. All
  `bytesPerRow`/`bytesPerImage`/mip-offset math goes through the central `CopyLayout`
  helper (§3.1/§6.5) — uploads and readbacks share one stride implementation.
- **Upload ordering (demand-loaded textures):** upload command buffers are created on
  **the same `MTL::CommandQueue`** as render work (§4.2) and the bank **commits its
  pending upload CB before the frame command buffer that consumes those textures is
  committed** — `FinishDraw` flushes the bank first, then commits the frame CB.
  Same-queue commit order is GPU execution order, so no fences/events are needed. A
  texture first referenced mid-frame is therefore always fully uploaded before the
  frame's draws execute.
- **Staging lifetime:** every staging buffer is put on the upload CB's retain list and
  released only in that CB's **completion handler** — never freed at encode time.
- Mip streaming: mirror `TextureBankGL33_Cache` logic 1:1 (level-range decisions,
  eviction, budget). Where GL re-creates the texture object for a new resident mip range
  (`glTexStorage2D` per range), Metal does the same — create the new `MTLTexture`,
  blit-upload the resident levels, swap the registry entry (§6.6), release the old one
  (the old object stays alive until in-flight command buffers complete — Metal retains
  resources referenced by encoded work, so no fence bookkeeping is needed for
  destruction).
  *Rejected for v1:* allocate-all-mips-once + `lodMinClamp` residency windows — better
  steady-state behavior but diverges from the bank's tested logic; note as a post-M6
  optimization.
- **`ResetForRemount` (mirrors `EngineGL33_Lifecycle.cpp:508-523`):** a mod re-mount
  changes *content only*. GL33: invalidate bind caches, flush queues, release bank
  textures — shaders/VBs/samplers stay. Metal mirror: invalidate `EncoderBroker` caches,
  flush queues, then release content **through its owners**: the texture bank's
  `ReleaseAllTextures` walks `TextureMetal` objects → registry `Release` (generation
  bump) → `MTL::Texture` release (in-flight safety via Metal retain semantics, above).
  **The `FrameRing` is untouched:** no cursor reset, no slot reuse — slot rotation
  remains exclusively `InitDraw`-under-semaphore (§0 NEVER). Device, layer, window,
  PSOs, samplers, shader library, fallback white textures, mesh registry all stay
  intact — exactly the GL33 "engine-level infra survives" rule (its comment documents
  the crash class this prevents).

### 7.4 Alpha classification (`GetAlphaClass`) — transparency routing depends on it

The shared draw path routes per-section transparency on a three-way alpha class:
`ShapeDraw.cpp:66` and `:132` ask `tex->GetAlphaClass()` to decide whether a section
goes to the **blend** pass (vs opaque/cutout). The base `Texture::GetAlphaClass()`
defaults to `Opaque` (`TextureBank.hpp:64-65`) — a Metal backend that skips this port
would classify **every** alpha texture Opaque and mis-route all blend geometry (glass,
foliage sorting, decals) while looking superficially fine.

Port the GL33 implementation **verbatim** — it is already backend-agnostic logic living
in backend files (`TextureGL33_Init.cpp:303-363`):

- `GetAlphaClass()` (`:339`): cached in `signed char _alphaClass` (−1 = not computed);
  combines `_src->IsAlpha()`, **chroma** transparency (`_src->IsTransparent()`), and
  the **DXT1 = one-bit punch-through** special case (`GetFormat() == PacDXT1` → cutout
  routing without any decode) via the shared `ClassifyTextureAlpha`.
- `ScanTopMipAlphaClass()` (`:310`): for multi-bit-alpha formats only, decode the
  **top** mip once — bytes read through the VFS, decoded via the shared
  `DecodePAABuffer` (handles DXT1/3/5, ARGB8888/4444/1555, AI88, P8 — the full PAA
  matrix), classified by `ClassifyAlpha`. Top mip on purpose: smaller mips blur crisp
  0/255 cutout holes into false partial-alpha and would mis-route poles/fences to the
  blend pass. The decode result is cached with the class (one scan per texture).

`TextureMetal` mirrors both methods and the `_alphaClass` cache field. Implementation
lands with the M1 texture work; **M4 gates on it** (opaque/cutout/blend routing parity
is only observable once the blend passes exist — §9 M4).

---

## 8. Depth

### 8.1 Formats & targets

- Frame depth-stencil: **`depth32Float_stencil8`** (stencil is required by the
  per-poly projected-shadow accumulator §6.3; depth24 doesn't exist on Apple GPUs —
  §1.5). Allocated with **the same sample count as the FramePass color target** and
  rebuilt together with it on every MSAA change (frame boundary); never resolved —
  usage/sample details in the §6.5 target inventory.
- Cascade shadow maps: `depth32Float` `texture2d_array`, `res × res × ≤4` layers,
  usage `RenderTarget | ShaderRead` (lit-pass sampling + probe/dump blit reads —
  §6.5 inventory; per-layer encoder open/clear semantics in §6.4)
  (mirrors `EnsureArrayTarget`, `EngineGL33_ShadowDepth.cpp:148-187`; GL used
  DEPTH_COMPONENT24 — 32F is the Apple-native replacement, strictly more precision).

### 8.2 Zero-to-one: the free win

`ShadowMath.cpp:169-196` (`Ortho`, `Perspective`) and the engine's projection conversion
already emit **zero-to-one depth matrices** — the GL backend needs `glClipControl`
(never available on macOS → `EngineGL33_State.cpp:491-499` warns and eats the precision
loss). Metal's clip space **is** [0,1]: the Metal backend consumes these matrices
unchanged and the documented depth-precision caveat disappears. Implementation note: the
backend must use the projection exactly as `ConvertProjectionMatrix` delivers it — any
GL-only [-1,1] adaptation that may exist on the GL side stays on the GL side (verify at
M2 which side owns the remap; do not touch shared math — Boundaries §0).
ZBias: reuse `ZBiasMath.hpp` / the projection-bias path (`_canZBias` equivalent) —
Metal's `setDepthBias(depthBias:slopeScale:clamp:)` is available as the native
alternative; decide per parity results at M2 (start with the projection-bias path for
exact GL parity, note `setDepthBias` as the cleaner endgame).

### 8.3 Reversed-Z: evaluated, **deferred**

*Decision:* do **not** adopt reversed-Z in this project.
*Why:* the observed problem (clip-control fallback precision loss) is already fixed by
native [0,1]; with `depth32Float` and CWA-era view distances (≤ ~5 km, near plane ~0.1 m)
standard-Z float precision is sufficient; reversed-Z would require touching shared
projection math + comparison funcs + clear values + bias signs, diverging Metal from the
GL33 A/B reference and from the CPU shadow oracle (`ShadowMath` tests). Document as a
post-parity optimization behind a config flag if far-terrain z-fighting is ever observed
on Metal (it is not observed on GL today).

---

## 9. Milestone ladder

Every milestone: branch builds with `CWR_HAS_METAL=ON` **and** `OFF`; `--renderBackend
gl33` still plays; scoped verification — run the named checks only.

- **M0 — Scaffolding & clear color.**
  Vendored metal-cpp; `PoseidonMetal` target + metallib rule **with consuming custom
  target (`PoseidonMetalShaders`, one name for both shader modes) + app-side wiring in
  all three executables (`PoseidonGame`/`PoseidonGameDemo`/`PoseidonTetris`: link,
  `CWR_HAS_METAL` define, `add_dependencies` — attached from the app CMakeLists, NOT
  from engine/PoseidonMetal, §3.3) + Metal Toolchain configure-time discovery and the
  runtime-compile fallback path proven** (§3.3; dev machines without the toolchain must
  fail at configure with the download command, and build+run with
  `-DCWR_METAL_RUNTIME_SHADERS=ON` — which exercises the **pre-expanded self-contained
  source**: the staged `.metal` must compile via `newLibrary(source:)`, proving the
  `#include` bundling works); factory enum/registration (priority 50); SDL window
  + `SDL_MetalView` (retained handle, borrowed layer, correct destroy order — §1.2/§4.1)
  + `CAMetalLayer`; `FrameRing` + command-buffer span **with slot-rotation invalidation
  of bindings + dirty-all**; `Clear()` renders the requested color; `NextFrame` present
  + resize/pixel-size coalescing + vsync toggle; window-mode methods incl.
  `GetCurrentWindowMode` (§4.5); all other `Engine` virtuals stubbed at
  Dummy level. Fix `CWR_HAS_OPENGL` label; hoist `SDLEventWindow.hpp` (+ optional
  pixel-size callback, §4.5).
  **Verify:** `--renderBackend metal` shows a stable clear-color window at correct
  pixel size (Retina on/off via `nativePixelDensity`), resize/fullscreen work, no
  validation-layer errors (`MTL_DEBUG_LAYER=1`, routed into `GetDebugErrorCount()` /
  `GetLastDebugMessage()` via the §6.6 store), clean shutdown (view destroyed before
  window, no layer over-release), GL33 unaffected; runtime-shader configuration
  builds and clears (self-contained source compiles); ring stress test (risk 11) passes
  incl. forced overflow + growth + retained-overflow release.
- **M1 — 2D/HUD queue path.**
  TextureMetal + bank (BGRA8/RGBA8 + BC formats **with the `DstFormat` resolution step
  AND the interpolated-texture `UploadFormatForTextureMetal` branch** §7.1; alpha-class
  scan port §7.4 — implementation here, routing gate at M4), handle registries +
  fallback white textures (§6.6), samplers, `vsScreen`+`psNormal/psFlat` PSOs (TLVertex
  layout with `UChar4Normalized_BGRA` color/specular — §5.3), soup queue,
  `Draw2D`/`DrawPoly`/`DrawLine` **with CPU clipping via the shared `Clip2D` port —
  no 2D scissor (§6.7 decision)**, fonts incl. the backend-neutral atlas residency
  virtual + `CreateDynamic`/`UpdateDynamic` (§3.4/§6.7), `DrawTestPattern` (§6.7),
  color readback codec + `CopyLayout` (§6.5), `GetDebugErrorCount()` /
  `GetLastDebugMessage()` overrides (§6.6).
  **Verify:** main menu + UI fully navigable under Metal; side-by-side screenshots vs
  GL33 via the harness (`Screenshot`/`SamplePixel` between FinishDraw and NextFrame —
  exercises the capture-source path and the BGRA→RGB/top-down color codec); trident
  `DrawTestPattern` assertions pass (colorbar per-bar RGB + gradient corners,
  `GameStateExtTest.cpp:1951-2072`); 2D clip parity A/B (clipped UI elements
  byte-identical — CPU clip, §6.7); text crisp at both densities and still visible
  after an F5 hot-reload (atlas residency virtual); HUD vertex colors not
  channel-swapped; Y-orientation correct (§5.2).
- **M2 — Static world geometry.**
  Mesh vertex/index `MTLBuffer`s (`CreateVertexBuffer` → mesh registry),
  `vsTransform`+`psNormal`, frame/pass/object constant flow
  (`UploadFrameConstants`/`UploadPassConstants`/`EmitDraw`), depth target, mid-frame
  `Clear(true,false)` clear-draw incl. its full-target viewport/scissor/cull bracket
  (§6.4), `UpdateProjection` flush ordering (§6.2), world-viewport crop (§6.4),
  **DrawItem recording at both seams + `GetDrawItemCount`/`GetRecordedDraws`
  overrides (§6.6)**, **`GetGLViewport` override in the validator's convention
  (§4.5)**, sky, terrain.
  **Verify:** mission loads; terrain + static objects + sky render with correct depth
  (no clip-control warning path); **cockpit/interior depth transition correct** (the
  `UpdateProjection` mid-pass path — enter/exit a vehicle, no interior z-artifacts);
  **pillarbox A/B**: non-native aspect (cropped world rect) matches GL33 incl. black
  periphery, and `DetectViewportMismatch` reports no violation with the cropped rect
  active (proves `GetGLViewport` convention); `GetRecordedDraws` DrawItem counts match
  GL33 for the same scene within queue-flush tolerance and `DetectMissingMeshHandles`
  stays at zero (registry never hands out handle 0 — §6.6).
- **M3 — Full opaque + instancing + material system.**
  Materials/local lights/texgen (`psDetail`, `psGrass`, `psWater` day paths), instanced
  runs (`_instCount` contract §6.6), cull/winding parity, ZBias.
  **Verify:** forests/fences render instanced (draw-call count drops match GL33's);
  light-edge A/B matches GL33 with light-saturation clipping at screen boundaries;
  town-scene capture: **CPU submission < 5 µs/draw avg** (Instruments or the engine's
  perf traces — same 26-site instrumentation), Pass1 CPU time ≤ 25 % of GL33's.
- **M4 — Alpha, particles, cockpit, AA.**
  Transparent/light/water passes, decals, `DrawDecal`/`DrawPoints`/3D lines, A2C on
  cutout (PSO variant), MSAA target + resolve **with `supportsTextureSampleCount`
  clamping incl. the 2× mode** (§6.5), render-scale SSAA + CompositeTarget path + both
  composition passes, night vision / `psFlat` debug toggle.
  **Verify:** glass/foliage/flares/HUD blend correctly A/B; **alpha-class routing gate
  (§7.4): opaque/cutout/blend section routing matches GL33** (glass classifies Blend,
  fences/poles classify cutout — no mis-routed sections; the exact failure mode of a
  missing `GetAlphaClass` port); light-edge A/B matches GL33 with transparent/light
  passes at screen boundaries; MSAA 2x/4x/8x + renderScale
  1.5 switchable at runtime (changes apply at frame boundary only; unsupported counts
  clamp deterministically with one log line; **depth-stencil target rebuilt at the
  color's new sample count — §6.5 inventory**); intermediate MSAA encoder closes
  Store, terminal close resolves (§6.4 deferred store actions — no black frame after a
  mid-frame cascade switch); screenshots correct with SSAA on
  (CompositeTarget capture path); no PSO-miss warnings in a full mission.
- **M5 — Shadows.**
  **Per-poly projected-shadow path** (`BeginShadowPass`/`EndShadowPass` flush brackets +
  `vsShadow`/`psShadow` caster PSOs with Shadow blend + Shadow depth-stencil state —
  §6.3 ground truth; **no darken quad, no REPLACE-0xFF**), cascade shadow maps
  (`RenderShadowDepthScene`, per-layer `{array, layer}` `TargetId` encoders with
  Clear-on-first-open + Store for lit-pass sampling — §6.4, lit-pass
  `texture2d_array` sampling), `ShadowDepthProbe` (CPU-oracle harness; depth readback
  codec §6.5), `DumpShadowMap`, `ShadowMapCacheSelfTest`.
  **Verify — parity gates on the GL33 code path** (`GLDepthStencilState.hpp:75-88`,
  `GLBlendState.hpp:38-42`, `EngineGL33_Draw.cpp:94-110`), not the stale
  `Engine.hpp:457-466` comment: overlapping casters darken once (stencil EQUAL-0/INCR
  working); `ShadowDepthProbe` matches the CPU oracle within GL33's tolerance; foliage
  cutout shadows show no leaf-gap phantom darkening (§5.2 psShadow note — discard must
  suppress stencil INCR). Tuning verification drives `SetShadowMapTuning` via **trident
  CLI verbs, not the F8 overlay** — `DebugOverlay` is hard-wired to GL
  (`ImGui_ImplSDL3_InitForOpenGL` / `ImGui_ImplOpenGL3_Init`, `DebugOverlay.cpp:1700,1705`)
  and no-ops under Metal in v1 (§10 risk 4): sweep tuning values via CLI, compare
  `ShadowDepthProbe`/`DumpShadowMap`/screenshot output A/B.
- **M6 — Parity, perf gate, default flip.**
  A/B screenshot suite across menu/day/night/fog/water/shadow scenes; gamma —
  **documented no-op** identical to GL33-on-macOS (`DoSetGamma` is `#ifdef _WIN32`,
  `EngineGL33_Draw.cpp:30-63`; no work item beyond storing `_gamma`); `ResetForRemount`
  mod cycle (§7.3 — ring untouched, registries released via owners); soak (1 h gameplay,
  no leak growth — `MTL_DEBUG_LAYER=1` feeding `GetDebugErrorCount()` /
  `GetLastDebugMessage()` (§6.6) + Instruments leaks; incl. minimize/occlude/resize
  churn for `nextDrawable` nil paths); flip descriptor priority to 200 (Auto → Metal
  on macOS); update `docs/MACOS_PORT.md` Phase 4 + `DECISIONS.md`. Optional if measured
  useful: `MTLBinaryArchive` warm start, texture-residency polish. The imgui **Metal**
  overlay backend stays a post-M6 ASK-FIRST item — **no M6 gate depends on the
  overlay** (all gates run via harness verbs/screenshots).
  **Verify:** town-scene frame CPU ≤ 30 % of GL33's; zero validation errors; A/B suite
  signed off; GPU error count stays 0 across the A/B suite (`triGetGLErrorCount`);
  GL33 still selectable for regression triage.

---

## 10. Risks & open questions

| # | Risk / question | Proposed resolution path |
|---|---|---|
| 1 | **Metal Toolchain is a separate download** — `xcrun metal` fails out-of-box (verified on the dev machine: `xcodebuild -downloadComponent MetalToolchain` required); exact `-std` flag spelling also unpinned (⚠️ §1.5) | §3.3: configure-time discovery with actionable FATAL_ERROR, `CWR_METAL_RUNTIME_SHADERS=ON` runtime-compile fallback, CI provisioning step. M0 pins the flag spelling via `metal --help`. |
| 2 | **Buffer-offset alignment on Apple GPUs** (⚠️ §1.6) | Use 256 B; M0 spike may relax after checking Metal feature tables for the device family — only if ring pressure ever matters (it won't at 8 MB). |
| 3 | **`SetGamma`** — *resolved:* GL33's `DoSetGamma` is Win32-only (`EngineGL33_Draw.cpp:30-63`); on macOS GL33 stores `_gamma` and does nothing | Metal `SetGamma` = the same stored **no-op**, documented — exact A/B parity. No shader/display work. |
| 4 | **ImGui `DebugOverlay` (F8)** is hard-wired to GL (`ImGui_ImplSDL3_InitForOpenGL` + `ImGui_ImplOpenGL3_Init`, `DebugOverlay.cpp:1700,1705`) | v1 decision (consistent with M5/M6 gates): overlay **no-ops under Metal** (guard in `DebugOverlay`); all shadow-tuning verification runs through trident CLI `SetShadowMapTuning` verbs (§9 M5). Post-M6: vcpkg `imgui[metal-binding]` + SDL3 backend + the single permitted ObjC++ TU — ASK FIRST (new dep feature). |
| 5 | **`psShadow` discard vs stencil INCR on TBDR** (we drop the late-Z hack, §5.2; per-poly path makes stencil correctness user-visible as double-darkening) | M5 verify with the exact historical symptom (foliage gap shadows / phantom darkening); if Metal's discard-suppresses-stencil guarantee shows an edge case, fall back to explicit depth-write shader variant and document in GOTCHAS. |
| 6 | **`nextDrawable` nil / blocking under occlusion & fast resize** | Late acquisition in `NextFrame` only (§4.4/§6.5) + nil-skip; frame work is unaffected (already committed in FinishDraw); soak-test minimize/occlude/resize at M0 and M6. |
| 7 | **Screen-path Y orientation & viewport/scissor origin** (Metal top-left vs GL bottom-left; 2D clip is CPU-side per §6.7 — scissor exists only in broker-internal full-target/edge draws) | M1 visual verify; viewport and broker scissor rects use the top-left convention directly (GL-era flips *removed* — §6.4 world viewport, §6.5 readbacks; `GetGLViewport` returns top-left unflipped, §4.5); assert with HUD screenshots and the color readback codec. |
| 8 | **Mixed-format PAA mips & AI88 swizzle parity** | M1: port `DecodePAABuffer` call sites 1:1 (post-`DstFormat` incl. the interpolated-texture branch, §7.1); A/B a texture-viewer scene; add an RG8 swizzle note to the shader if GL relied on texture swizzle state. |
| 9 | **Trident harness readbacks** (`SamplePixel` gap-timing, top-left coords, BGRA→RGB conversion, `ShadowDepthProbe` row 0 = bottom contract) | §6.5 defines the capture source + the color/depth readback codecs on the shared `CopyLayout`; covered by existing harness tests at M1/M5. Mid-frame readback slow path (commit-and-continue) logged and exercised once in M1 tests. |
| 10 | **Perf target miss (< 5 µs/draw)** | Instruments "Metal System Trace" at M3; levers in order: coalesce texture sets via two-slot cache, reduce VSConstants dirty rate (split material vs frame blocks — layout change allowed under ASK FIRST), then instancing breadth. Do not reach for argument buffers until these are exhausted. |
| 11 | **Validation of long-session ring behavior** (mid-frame InitDraw pairs, overflow growth + retention, slot-rotation invalidation) | M0 unit-style stress: synthetic 10 k-span loop with tiny arena forcing overflow + growth; assert allocator invariants, pair-cache invalidation at rotation, overflow release only after completion handler. |
| 12 | **metal-cpp release drift** (vendored macOS 15 headers vs newer SDKs) | Pin in `thirdparty/metal-cpp/VERSION.md` with download URL + date; upgrade only deliberately (DECISIONS entry). |
| 13 | **Mid-frame depth/stencil clear-draw artifacts** (§6.4 clear draw vs encoder split; incl. its full-target viewport/scissor/cull bracket under a cropped world viewport) | M2 verify at the live call sites (incl. a `Clear` while the world crop is active); documented fallback = encoder split with selective load actions (pays a TBDR color store/load); record the outcome in GOTCHAS. |
| 14 | **Deferred store actions (`storeAction=Unknown` + `set*StoreAction` at close, §6.4)** — the terminal-vs-intermediate close decision is the one place the plan leans on a less-common API pattern | API verified (§1.6); M4 verify covers mid-frame MSAA encoder splits (cascade interleave) + terminal resolve. Documented fallback: `StoreAndMultisampleResolve` on every MSAA close — correct, pays k+ fullscreen resolves/frame; record in GOTCHAS if taken. |

---

## 11. Effort estimate (S/M/L per milestone)

| Milestone | Size | Dominant cost |
|---|---|---|
| M0 scaffolding/clear | **S–M** | CMake/toolchain discovery + FrameRing correctness |
| M1 2D/HUD + textures | **M** | texture bank + registries port + 2D/readback parity fiddliness + font-atlas neutralization + alpha-class/test-pattern ports |
| M2 static world | **M** | constant-flow port + depth/viewport verification |
| M3 opaque/instancing/materials | **L** | biggest surface: materials, lights, texgen, instancing, perf work |
| M4 alpha/AA/particles | **M** | pass matrix + MSAA/SSAA targets + composition paths |
| M5 shadows (both systems) | **L** | per-poly projected shadows + cascade maps + oracle parity |
| M6 parity/perf/flip | **M** | A/B discipline + soak + polish |

## Questions for Architect (filled by implementer)

*(empty — append here and halt if a plan ambiguity blocks execution)*
