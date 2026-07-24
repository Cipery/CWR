# macOS Port — Plan & Research

**Goal:** Port the CWR engine (Arma: Cold War Assault Remastered, codename *Poseidon*) to macOS on Apple Silicon (arm64), ultimately with a native Metal renderer.

- Fork: `Cipery/CWR` (upstream: `BohemiaInteractive/CWR`, locked repo — no upstream PRs)
- Target: **arm64 (Apple Silicon) only**. No x86_64/universal binary unless a reason appears.
- Renderer strategy: **native macOS OpenGL first** (see the GL contract caveat below), **Metal backend as the final phase**. No ANGLE/Zink/MoltenVK intermediates.

> Reviewed 2026-07-24 in a two-cycle review loop: cycle 1 — Claude Opus 4.8 (xhigh) + gpt-5.6-terra (max), 26 source-verified corrections; cycle 2 — Opus + terra + gpt-5.6-luna (max) + gpt-5.6-sol (high), 15 further source-verified corrections (1 finding rejected as false positive). All 41 incorporated in this version.

---

## Research findings (2026-07-24, review-corrected)

### Why this port is tractable

The engine was modernized by Bohemia to C++17/20 (mixed — core `Poseidon` and `PoseidonGL33` targets pin `CXX_STANDARD 17`, others use 20) / CMake / Clang and already supports **two platforms: Windows x64 and Linux x64**. The middleware stack is macOS-native:

| Subsystem | Tech | macOS status |
|---|---|---|
| Renderer | Single backend `PoseidonGL33`; requests GL 3.3 Core Forward-Compatible (`engine/PoseidonGL33/EngineGL33.cpp:277`) **but see GL contract caveat below** | Works under macOS GL 4.1 ceiling, with capability-gating work required |
| Windowing/input | SDL3 (~87+ files), no Win32 message loop in hot path | Native Cocoa support |
| Audio | OpenAL (openal-soft) + opus/ogg/vorbis | CoreAudio backend exists; **runtime loader is Linux-only** (`engine/PoseidonOpenAL/OpenALRuntime.hpp:147-153` tries only `libopenal.so*` names — needs `.dylib`/`@rpath` handling) |
| Entry point | POSIX `int main` already exists alongside `WinMain` (`apps/cwr/Game/WinMain.cpp:24`) | Ready |
| Rust (Trident test harness, papa-bear mserver) | tokio/clap, one `#[cfg(unix)]` | Portable; note `mserver/` has **no workspace manifest** — four independent crates (`Archive`, `CLI`, `Client`, `MasterService`), build each separately |
| vcpkg deps | sdl3, openal-soft, curl[ssl], imgui[freetype], mimalloc, zstd, enkits, spdlog, … | All available for macOS; none Windows-only |

Renderer abstraction seam: base class `Engine` (`engine/Poseidon/Graphics/Core/Engine.hpp`) + **`GraphicsEngineFactory`** (`engine/Poseidon/Graphics/GraphicsEngineFactory.hpp:29-34` — `GraphicsBackend` enum `Dummy`/`GL33`/`Auto`, consumed by `GameApplication.cpp`). A Metal backend must extend this enum/registry/CLI selection surface, not just add a `CreateEngineMetal()` factory. The abstraction leaks GL concepts (`GLBlendState.hpp` etc. live in `Graphics/Core/`), so the Metal phase includes cleaning up that seam.

No DirectX/Vulkan/Metal code exists — all "D3D" grep hits are comments documenting how the GL backend reproduces legacy D3D9/D3D11 behavior. No XAudio2/DirectSound.

### ⚠️ The real GL contract (review finding, source-verified)

The backend *requests* 3.3 Core but **relies on unguarded post-3.3 APIs**:

- **Program binary shader cache** — `glProgramBinary` / `glGetProgramBinary` / `GL_PROGRAM_BINARY_RETRIEVABLE_HINT` (`engine/PoseidonGL33/EngineGL33_Shaders.cpp:1423,1445,1459`; cache load/save at 1554-1580). GL 4.1 / `ARB_get_program_binary` tier; **no `GL_NUM_PROGRAM_BINARY_FORMATS` or extension check anywhere**. On macOS, binary formats count is typically 0 → the cache path must be gated/disabled gracefully.
- **Immutable texture storage** — `glTexStorage2D` (`engine/PoseidonGL33/TextureGL33_Init.cpp:463`). GL 4.2 / `ARB_texture_storage`; called unconditionally (comment acknowledges the extension, no runtime check). macOS exposes `ARB_texture_storage` on 4.1 — verify at bring-up, add a gate.
- **Clip control** — the one *gated* site (`EngineGL33_State.cpp:491-499`, `if (GLAD_GL_ARB_clip_control)`). macOS never implemented `ARB_clip_control`, so the else-branch always runs there. `ShadowMath.cpp:169-196` unconditionally builds zero-to-one depth matrices, so without clip control the clip volume stays [-1,1] → **reduced depth precision** (the code's own warning says exactly this). Verify shadow/depth artifacts on macOS; consider a proper fallback.
- **Debug logging** — `glDebugMessageCallback` (GL 4.3/KHR_debug) is null-guarded (`EngineGL33.cpp:421`) but macOS never provides it → bring-up loses per-error GL logging; plan a `glGetError`-polling fallback for development.
- Shaders are **inline GLSL `#version 330 core` strings compiled at runtime** via `glShaderSource`/`glCompileShader` (`EngineGL33_Shaders.cpp:~730-758`). glslang from vcpkg is used **only by a unit test** (`tests/unit/engine/Poseidon/Graphics/test_shader_named_outputs.cpp`), *not* by the renderer; SPIRV-Cross is not a dependency at all. 330-core shaders are macOS-4.1-compatible, but Apple's GLSL compiler is stricter — verify compilation of every shader at Phase 2.
- Repo inconsistency to reconcile while in there: `CWR_HAS_OPENGL` option is labeled "OpenGL 4.5 graphics backend" (`CMakeLists.txt:145`) — stale label vs. the actual 3.3-request contract.

### The actual obstacles

1. **x86 SIMD intrinsics — Phase 0 hard blocker.** Complete inventory of unconditionally-reachable x86-intrinsic includes (source-verified sweep): `engine/Poseidon/Foundation/Math/Math3DK.hpp:1` (`<immintrin.h>`, builds `Vector3K`/`FloatQuad` on `__m128`, included by 13+ files), `engine/Poseidon/Foundation/Math/V3QuadsP3.cpp:13` (`<x86intrin.h>`, non-MSVC else-branch), `engine/Poseidon/Graphics/Rendering/ColorsK.hpp:5` (`<xmmintrin.h>`, no guard), `engine/Poseidon/Foundation/Math/Quatrix.hpp:6` (`<xmmintrin.h>`, non-MSVC else-branch), `engine/Poseidon/World/Terrain/Occlusion.cpp:388,417-429` (MMX/SSE includes under hardcoded `#define OPTIMIZE_FOR_MMX 1`). (Other intrinsic sites — `DebugTrap.cpp`, `Shape*.cpp`, `FltOpts.hpp`, `Math3DPK.cpp` — are already properly arch-guarded.) Foundation/Math links into **every** target incl. headless `PoseidonServer`/`PoseidonTools` → nothing compiles on arm64 without this. Options: **sse2neon shim** (fast, low-risk) or scalarization; audit `__m128`-in-struct size/alignment impact on any binary file/network layouts.
2. **"POSIX" branches are really Linux branches.** `#ifdef _WIN32 … #else` where `else` assumes Linux:
   - `engine/Poseidon/Foundation/Platform/CrashHandler.cpp` — **hard compile error**, not a stub-later item: `#include <link.h>` (:118) + `dl_iterate_phdr` + `ElfW()` ELF build-id parsing (:264-293) + `/proc/self/maps` (:248). Needs a real `__APPLE__` branch: `_dyld_image_count`/`_dyld_get_image_header`, `LC_UUID`, `mach_vm_region`; a minimal stub first is acceptable but the *whole* non-Windows branch must be replaced, not just the /proc read.
   - `engine/Poseidon/Foundation/Memory/MemGrow.cpp` — Linux-only includes at :6 (`<linux/sysinfo.h>`) and :11 (`<sys/sysinfo.h>`) must be guarded. Note: `sysinfo()` at :145 feeds **only a swap-stats error message** (`totalswap`/`freeswap`); the actual `mmap`/`mprotect` reservation path already works on macOS. Correct replacement is `sysctlbyname("vm.swapusage")` (xsw_usage), *not* `sysctl(HW_MEMSIZE)` (that's physical RAM — wrong quantity).
   - `engine/Poseidon/Foundation/Common/Platform.cpp:227` — `/proc/<pid>/statm` → `task_info(MACH_TASK_BASIC_INFO)`.
   - `engine/Poseidon/Network/MultiplayerAuth.cpp:75-89` — machine identity reads only `/etc/machine-id` / `/var/lib/dbus/machine-id` → on macOS the client sends identity `"0"` (via `NetworkClientOnMessage.cpp:744-750`). Needs an IOKit `IOPlatformUUID` path + a decision on identity policy (ban-list contract expects stable decimal ids).
   - **Threads (compiles-but-broken class — build gate won't catch these):** `engine/Poseidon/Foundation/Threads/PoCritical.cpp:9` uses glibc-only `PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP` **and** copy-assigns the initializer into mutexes (`:25,46,61`) instead of `pthread_mutex_init` → replace with `pthread_mutexattr_settype(PTHREAD_MUTEX_RECURSIVE)` + init/destroy. `engine/Poseidon/Foundation/Threads/PoSemaphore.cpp:16,29,64,81` + `MultiSync.hpp:189,200` use **unnamed POSIX semaphores** (`sem_init` & co.) — ENOSYS stubs on macOS: compiles, fails at runtime; reaches headless targets via `MemHeap.hpp`/`netpch.hpp`/`ThreadSync.cpp` → Darwin backend via `dispatch_semaphore_t` (or named `sem_open`, or mutex+condvar; `std::counting_semaphore` unavailable — targets pin C++17).
   - `engine/Poseidon/IO/PackFiles.cpp:5` — unconditional `#include <malloc.h>` (absent from macOS SDK; the include is dead — nothing from it is used) → remove it.
   - `tests/unit/engine/Poseidon/test_fixtures.hpp:37` — fixtures resolve the executable via `readlink("/proc/self/exe")`, and the crash-handler test expects `/proc/self/maps` output → `_NSGetExecutablePath` port + platform-specific crash assertions, prerequisite for any macOS CTest gate.
3. **Build plumbing gaps beyond the top level.** No macOS preset/toolchain/triplet (`cmake/presets/`, `cmake/toolchains/` are win/linux, linux hardcodes `-m64`), and a top-level `if(WIN32)…else()`/`APPLE` fix is **not sufficient**: per-target CMake injects GNU-ld-only `-Wl,--start-group/--end-group` flags in non-Windows branches (`apps/cwr/Server/CMakeLists.txt:51-59`, `apps/tools/Tools/CMakeLists.txt:61-70`) which Apple's ld rejects → audit every target-level non-Windows branch. (The `libmount` overlay port is a **non-issue**: it's referenced nowhere in `vcpkg.json` or CMake — verify reachability, then drop it from the checklist.)
4. **Breadth audit:** roughly ~198 files carry `_WIN32` forks (~35 include `windows.h`) — treat all counts in this doc as floor estimates, not exact figures. Most have working POSIX `#else` branches (BSD sockets, pthreads, `dlopen`, `_posix.cpp` files). Risky clusters: memory/diagnostics, and every hardcoded `.so` name (`OpenALRuntime.hpp`, `RenderDocCapture.cpp:47-59` — `librenderdoc.so`; RenderDoc has **no macOS capture** at all, so macOS graphics debugging needs a different tool story).
5. **macOS user-data paths.** All non-Windows builds select the XDG path provider (`engine/Poseidon/CMakeLists.txt:100-107` → `PlatformPaths_posix.cpp:41-58`; also `GamePaths.cpp:33,84`): profiles/saves/cache/mods land in `~/.config`, `~/.cache`, `~/.local/share`. Non-idiomatic for a `.app` and entangled with sandbox/codesigning. Needs an Apple provider (`~/Library/Application Support`, `~/Library/Caches`) + migration decision.
6. **Game-data launch contract.** The game resolves data from CWD unless `-C/--work-dir` is given (`apps/cwr/GameBase/GameBase.cpp:88-150`); CTest hardcodes `packages/Remaster`; `tests/README.md` recommends `packages/Demo`. Our staging layout (below) must be wired into one explicit convention for CLI, Finder-launch, and tests.

### Build targets overview

- `PoseidonGame` / `PoseidonGameDemo` — GUI clients (where the hard obstacles concentrate)
- `PoseidonServer` — dedicated server, console-only, no GPU
- `PoseidonTools`, `PoseidonEvaluator`, `PoseidonStudio` (ImGui editor), `pbo`, `poseidon` modules
- `PoseidonFormats` — C API shared lib (P3D/PAA/PBO/RTM), used by Blender addon
- Rust: `engine/Trident` (test orchestrator); `mserver/` = 4 separate crates (no workspace) — papa-bear master server + CLI

---

## Game data

Compiled binaries need game data (not in repo, APL-SA licensed). We use the user's Steam install of *ARMA Cold War Assault* (incl. `Remastered/` data), copied from the Parallels Windows 11 VM:

- Original location (VM): `C:\Steam\steamapps\common\ARMA Cold War Assault`
- **Local copy (Mac): `packages/ARMA Cold War Assault/`** — `packages/` is the repo's designated git-ignored game-data staging area
- The `Remastered/` subfolder holds the remastered data the new engine targets
- **TODO (Phase 0):** reconcile with the repo's expected layout — CTest expects `packages/Remaster`, docs mention `packages/Demo`. Likely action: stage/symlink `packages/Remaster` → our `Remastered/` data and standardize launch via `-C/--work-dir`.

---

## Plan — escalation ladder

### Phase 0 — Build system + headless targets
Goal: `PoseidonServer`, `PoseidonTools`, `PoseidonEvaluator` + Rust crates build & run on macOS arm64.
- [ ] `cmake/presets/macos.json` (`macos-arm64-clang-{debug,rwdi,release}`), wired into `CMakePresets.json`
- [ ] `cmake/toolchains/macos-arm64-clang.cmake` (Apple Clang vs Homebrew LLVM — decide; repo pins Clang). Include **`-fsigned-char`** — arm64 defaults `char` to unsigned, x86 targets assume signed
- [ ] vcpkg `arm64-osx` triplet config in `cmake/vcpkg-triplets/` — incl. per-port `VCPKG_LIBRARY_LINKAGE=dynamic` override for **openal-soft** (mirror `x64-windows-clang.cmake:4-6`; LGPL + the engine `dlopen`s it) — and **verify the pinned deps resolve for arm64-osx**: `sdl3 >= 3.4.10#1`, openal-soft, imgui[sdl3-*] under the pinned `builtin-baseline`; bump baseline if the arm64-osx port lags
- [ ] `APPLE` branch in top-level `CMakeLists.txt` **plus per-target audit**: replace GNU `--start-group/--end-group` link flags with Darwin equivalents in `apps/cwr/Server/`, `apps/tools/Tools/` (and any sibling target CMake)
- [ ] **SIMD port (blocker):** sse2neon shim (or scalar fallback) for the complete inventory — `Math3DK.hpp`, `V3QuadsP3.cpp`, `ColorsK.hpp`, `Quatrix.hpp`, `Occlusion.cpp` (`OPTIMIZE_FOR_MMX` hardcode); verify struct sizes/alignment unchanged
- [ ] **Threads (runtime-broken on macOS despite compiling):** `PoCritical.cpp` recursive-mutex init idiom (`_NP` initializer + struct copy) → `pthread_mutex_init` path; `PoSemaphore.cpp`/`MultiSync.hpp` unnamed semaphores → `dispatch_semaphore_t` backend; smoke-run a threaded tool binary, don't trust the build gate
- [ ] **CrashHandler `__APPLE__` branch:** minimal Mach/dyld implementation or clean stub of the entire non-Windows path (`<link.h>`/ELF code cannot compile on Darwin)
- [ ] `PackFiles.cpp`: drop dead `<malloc.h>` include
- [ ] MemGrow: guard Linux includes; swap-stats via `sysctlbyname("vm.swapusage")`
- [ ] Platform.cpp process-memory via `task_info(MACH_TASK_BASIC_INFO)`
- [ ] Test fixtures: `_NSGetExecutablePath` instead of `/proc/self/exe` (`test_fixtures.hpp:37`); platform-specific crash-handler test expectations
- [ ] Verify whether vcpkg resolves `libmount` on macOS at all; drop from checklist if unreachable
- [ ] Rust: `cargo build` per crate — `engine/Trident` + each of `mserver/{Archive,CLI,Client,MasterService}`
- [ ] Game-data convention: stage `packages/Remaster` (symlink to our data), document `-C` launch usage
- Verify: server starts, tools run against PBO files from game data, per-crate Rust builds green

### Phase 1 — Client compiles & links
Goal: `PoseidonGame` builds on macOS arm64.
- [ ] Sweep remaining compile errors (more Linux-isms and arm64 issues expected)
- [ ] Audit `_WIN32` forks the client pulls in (Network winsock mirror, IO, UI subsystems)
- [ ] **Multiplayer identity:** macOS machine-id via IOKit `IOPlatformUUID` in `MultiplayerAuth.cpp`; keep decimal ban-list contract; add identity test
- [ ] Arch reporting: `OptionsUIApp.cpp:119-124` labels every non-x86-64 build "x86" — recognize `__aarch64__`/`__arm64__`
- Verify: binary links, `--help`/version runs **and reports arm64**

### Phase 2 — Window + renderer bring-up
Goal: main menu renders; with game data, gameplay smoke test.
- [ ] SDL3 window on Cocoa, GL context creation; assert what macOS actually returns for a 3.3-core request
- [ ] **Capability-gate the unguarded GL usage:** program-binary shader cache (`GL_NUM_PROGRAM_BINARY_FORMATS` == 0 on macOS → disable cache gracefully), `glTexStorage2D` (verify `ARB_texture_storage` presence), clip-control fallback (verify shadow/depth precision artifacts)
- [ ] Verify every inline GLSL 330 shader compiles under Apple's stricter Core-profile GLSL compiler
- [ ] `glGetError`-polling debug fallback (no KHR_debug on macOS)
- [ ] Retina/high-DPI framebuffer scaling (`SDLEventWindow.hpp`)
- [ ] Graphics debugging story: RenderDoc has no macOS capture (`RenderDocCapture.cpp` hardcodes `librenderdoc.so`) — pick apitrace / gl-frame-capture alternative + lldb/dSYM workflow
- Verify: menu on screen → load a mission with demo/full data → play

### Phase 3 — Polish & packaging
- [ ] **OpenAL runtime + packaging chain:** add `.dylib`/`@rpath` names to `OpenALRuntime.hpp` (currently `libopenal.so*` only → sound system silently null on macOS); requires the dynamic openal-soft from the Phase 0 triplet override; extend `DistCopy.cmake` runtime-copy logic beyond its `if(WIN32 …)` gate; fix install name/rpath; bundle + sign the dylib (incl. LGPL notice); smoke-test playback from the packaged app
- [ ] **Mic capture permissions:** `VoNCaptureOpenAL.cpp:19` opens an input device → `NSMicrophoneUsageDescription` in Info.plist + graceful TCC authorization handling; test first-run/denied/revoked states
- [ ] **Finder-launch data discovery:** CLI `-C/--work-dir` (`GameBase.cpp:88`) is the only data-root mechanism; a Finder-launched `.app` has no CWD and can't bundle the (excluded) game data → add external data discovery/selection with a persisted path; test Finder launch against separately installed data
- [ ] **macOS paths provider:** `~/Library/Application Support`/`Caches` instead of XDG dirs; migration/override behavior; test saves/mods/cache from a bundled launch
- [ ] `mimalloc` behavior on macOS (keep or disable if interposing misbehaves)
- [ ] Crash handler: full Mach implementation (LC_UUID build-id, `mach_vm_region` maps) replacing the Phase 0 stub
- [ ] `.app` bundle: `MACOSX_BUNDLE` CMake rules, Info.plist, **.icns generation** (resources/ has only Windows `.ico`/`.rc`), rpath/dependency closure, nested codesigning verification
- [ ] **Distribution gate:** for any public release add hardened runtime + Developer ID signing, notarization + stapling, `codesign`/`spctl` verification, quarantined clean-machine launch test; otherwise explicitly scope the milestone to local/ad-hoc builds
- [ ] Blender addon: `p3d_lib.py:12` + `package.ps1:18` assume `PoseidonFormats.so` on non-Windows but macOS builds `.dylib` → add Darwin mapping, or explicitly defer the addon out of the port scope
- [ ] **CI lane:** GitHub Actions macOS arm64 job — configure/build/CTest + per-crate Rust (needs the Phase 0 fixture port); optional data-backed renderer smoke job
- [ ] `PoseidonStudio` (ImGui editor) as a bonus target
- Verify: signed `.app` runs from Finder with sound, saves in the right place

### Phase 4 — Native Metal backend (endgame)
- [ ] Firm up the `Engine` abstraction (de-GL-ify `Graphics/Core` leaked types)
- [ ] **Extend the real selection surface:** `GraphicsBackend` enum + registration in `GraphicsEngineFactory` + `GameApplication.cpp` wiring + config/CLI selection — not just a `CreateEngineMetal()` factory
- [ ] **Native build layer:** top-level `project()` enables only `CXX C` (`CMakeLists.txt:3`) — add `OBJCXX` (or go metal-cpp), link Metal/QuartzCore/Foundation frameworks, define SDL↔`CAMetalLayer` ownership
- [ ] Shader pipeline decision: current reality is runtime-compiled inline GLSL 330. For Metal: wire the **already-present** vcpkg glslang into the renderer/build path and add **spirv-cross** (the sole new dependency) for a GLSL→SPIR-V→MSL offline pipeline (needs shader source adjustments for Vulkan-style semantics), or hand-port the shader set to MSL. Include reflection/bindings, cache behavior, CI compilation of all shaders
- [ ] Keep GL33 backend working as reference for A/B validation

---

## Working notes

- Upstream is locked (no PRs accepted); all work happens on the fork. Community continuation exists — check whether someone already started a macOS port before heavy lifting.
- **Licensing / release gate (tightened after review):**
  - Code: GPL-3.0-or-later **with additional terms under GPL §7** — `LICENSE:682-695` explicitly requires modified versions to be *marked as modified* and not identified as the original; a public release must satisfy source-availability + these marking/notice obligations.
  - Trademarks: no "ARMA"/"Operation Flashpoint" branding in any public release (not granted by the license).
  - Binary redistribution must ship **version-exact third-party notices** — `THIRD_PARTY_NOTICES.md:561-568` says to regenerate from `vcpkg_installed/` at build time; add this to the packaging release gate, plus Rust crate notices where mserver/Trident binaries ship.
  - Game data: APL-SA, separate from GPL code — keep `packages/` out of git (already ignored), never bundle data in releases.
- Review provenance: cycle-1 panel findings and verification table live in `.claude/prompts/run-20260724-084334-97419-29538/` (session-local, not committed).
