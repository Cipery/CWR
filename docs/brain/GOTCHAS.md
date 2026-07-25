# Gotchas

Traps that already cost us time. Read the relevant entry **before** touching the area.
Append when something bites you; include the symptom, the cause, and the fix so the
next reader doesn't re-debug it.

---

### Codebase — the `#else` of `#ifdef _WIN32` means *Linux*, not POSIX
**Symptom:** code that "already supports non-Windows" fails to compile/run on macOS.
**Cause:** non-Windows branches assume Linux (`<linux/sysinfo.h>`, `/proc/...`); same
for CMake's `if(WIN32) … else()`. Known sites are listed in `../MACOS_PORT.md`
(MemGrow.cpp, Platform.cpp, CrashHandler.cpp). **Fix:** add a real `__APPLE__` /
`APPLE` third branch (sysctl/Mach equivalents); never assume the else-branch is portable.

### Renderer — GL33 backend's "3.3 Core" contract is a lie; it uses unguarded 4.1/4.2 APIs
**Symptom:** assuming "engine needs only GL 3.3" (as its context request suggests) makes the
macOS GL story look free; on macOS the shader cache and textures would break at runtime.
**Cause:** `EngineGL33_Shaders.cpp:1423,1445,1459` call program-binary APIs (GL 4.1 tier) and
`TextureGL33_Init.cpp:463` calls `glTexStorage2D` (GL 4.2 tier) with **no capability checks**;
only clip-control is gated — and its fallback (always taken on macOS) reduces depth precision
(`ShadowMath.cpp:169-196` hardcodes zero-to-one depth). **Fix:** capability-gate these paths in
Phase 2 (see `../MACOS_PORT.md` "The real GL contract"). Found by gpt-5.6-terra review 2026-07-24;
an Opus review of the same code missed it — don't trust a single reviewer on GL API tiering.

### Renderer — WorldInstances range must stay bound throughout an instanced run
**Symptom:** changing the WorldInstances UBO from one fixed store to per-draw ranges can make
instances after `gl_InstanceID == 0` use uninitialized transforms. **Cause:** `EmitDraw` calls
`UploadVSWorldMatrix` for every section even during an instanced run; rebinding a scalar ring
slot there replaces the complete range bound by `UploadWorldInstances`. **Fix:** scalar draws
allocate and bind a ring slot, but `_instCount > 1` preserves the range uploaded at run start.

### Renderer — metal-cpp macOS 15 omits two live CAMetalLayer properties
**Symptom:** the M0 design calls for `setMaximumDrawableCount(3)` and a live vsync toggle, but
the vendored macOS 15 / iOS 18 `CA::MetalLayer` wrapper has neither
`maximumDrawableCount` nor `displaySyncEnabled`. **Cause:** those SDK properties were not
included in metal-cpp's QuartzCore surface even though they exist in `CAMetalLayer.h`.
**Fix:** keep a local pure-C++ shim in `EngineMetal_Window.cpp` that invokes only those two
setters through metal-cpp's `NS::Object::sendMessage` and `sel_registerName`; do not add an
Objective-C++ TU or release the borrowed layer.

**Firewall rule:** **Symptom:** a TU that sees Poseidon's `typedef int BOOL` before metal-cpp
fails when metal-cpp pulls in `objc/objc.h` and its incompatible `BOOL`. **Rule:** never include
Foundation, Metal, or QuartzCore metal-cpp headers from `PoseidonMetal` interface headers; use
`MetalFwd.hpp` there. The sole implementation-only umbrella is the private `MetalCppFirst.hpp`,
which must be included first in every metal-cpp implementation TU.

### Threads — Foundation/Threads compiles on macOS but is runtime-broken (build gate lies)
**Symptom:** arm64 build succeeds, threaded code deadlocks/fails at runtime on macOS.
**Cause:** `PoSemaphore.cpp`/`MultiSync.hpp` use unnamed POSIX semaphores (`sem_init` = ENOSYS
stub on Darwin) and `PoCritical.cpp` uses glibc-only `PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP`
with a copy-assign init idiom. Both compile cleanly. **Fix:** Darwin semaphore counters use
`pthread_mutex_t` + `pthread_cond_t` (rather than dispatch semaphores, because callers need
an exact `getValue()` snapshot); recursive locks use `pthread_mutex_init` + recursive attrs.
See `../MACOS_PORT.md` Phase 0. Lesson: for this codebase, "it compiles on macOS" proves
nothing about threads — smoke-run a threaded binary.

### Renderer — TriQueue invariant: vertices FIRST, then PrepareTriangle
**Symptom:** animated smoke rendered as torn / missing quads on Metal only; static A/B
screenshots looked perfect, so the M6 parity suite passed anyway. **Cause:**
`EngineMetal::DrawDecal` called `PrepareTriangle` before `QueueVertices`. `PrepareTriangle`
selects/flushes the per-texture queue, and on a texture change it peels the trailing
*unindexed* vertices into the new batch — with the order reversed, the "trailing unindexed"
vertices it grabbed were the *previous* decal's already-indexed ones. Smoke cycles textures
(`basic.06`..`basic.16`) every few frames, so only smoke corrupted. **Fix:** always
`QueueVertices` → `PrepareTriangle` → `QueueFan/QueueTriangles`, matching the GL33 oracle
`EngineGL33_DrawShared.cpp:114` (`AddVertices` → `QueuePrepareTriangle`). Fixed in `0052d08`.
**Two lessons:** (1) a static-screenshot A/B suite cannot catch animation-dependent bugs —
add a moving/particle-heavy scene or use the tri harness; (2) when auditing a new backend,
check *call ordering* against the oracle, not just the set of calls made.

### Renderer — depth bias has THREE modes in GL33, not two
**Symptom:** ground-projected shadows z-fight or drop out at steep / 3rd-person camera
angles on Metal, while looking fine head-on (and fine in static A/B screenshots).
**Cause:** GL33's single `ApplyPipeline` (`EngineGL33_State.cpp:311-318`) branches on
`ShaderFamily::Shadow` FIRST (`glPolygonOffset(-1,-64)`), only then on
`SurfaceMode::OnSurface` (`-1,-1`), else off. Metal's `ApplyWorldState` looked at
`surface` alone, so shadows got a 64x-too-weak constant term — and the slope term
collapses toward 0 near top-down, which is exactly when the shadow then loses the
LessEqual test (`GLPipelineState.hpp:45-51` documents this). **Fix:** one
`ApplyDepthBias()` owns the three-way rule, both `Apply*State` paths call it, and the
sticky replay replays the resolved pair instead of re-deriving it (`d3614d8`).
**Watch for:** Metal splits GL33's one `ApplyPipeline` into `ApplyWorldState` +
`ApplyScreenState`; any per-descriptor state GL33 resets centrally must be applied in
BOTH, or it leaks across the encoder (this bit `setDepthBias`, and earlier `cull`,
`frontFace`, `DepthClipMode` and the PS constants in `0052d08`).

### Tooling — running the game and trusting the build on macOS
**Symptom (build):** "build clean" reported when nothing compiled. **Cause:** `-k 0` is a
ninja flag; `cmake --build <dir> --target X -k 0` errors with `Unknown argument -k` and
builds nothing — and a `grep -E 'error|warning'` over the output does not match that
wording. **Fix:** `cmake --build <dir> -- -k 0`, check the process exit code (not
`PIPESTATUS` — zsh uses lowercase `pipestatus`), and sanity-check the binary's mtime.
Never judge a build from a filtered log you truncated with `tail`.

**Symptom (run):** missing fonts/textures, or Metal silently not being tested.
**Cause + fix:** launch `dist/<preset>/PoseidonGame` (the metallib is staged next to it;
the `build/` copy makes Metal init fail) **with CWD = `packages/Remaster`** (fonts and
several textures resolve relative to it). Note the game **silently falls back to GL33**
when the metallib is missing — an A/B run can measure GL33 while you believe it is Metal.
Always confirm `Metal: loaded .../PoseidonShaders.metallib` in the log.

### Tooling — screenshot capture is unreliable, and fails silently
**Symptom:** `--auto-screenshot` writes 4/4 PNGs on one run and 1/4 on the next *identical*
run, aborting the game with exit code 2; `--test-type screenshot` reports "expected output
file was not created" although the PNG does appear (written later, during shutdown, so it
shows the "Shutdown..." screen); the harness `screenshot` verb returns ok and never writes
at all (both backends). **Cause (partial):** `GameApplication.cpp:1357-1370` arms the
capture, renders exactly ONE `AppIdle()`, then verifies the file exists and aborts if not —
capture completion within that single frame is not guaranteed. Why it completes sometimes
and not others is NOT yet diagnosed. **Aggravating:** `EngineMetal::CaptureScreenshotIfPending`
(`EngineMetal_Readback.cpp:272`) clears `_pendingScreenshotPath` *before* attempting
`ReadCapture()` and returns with no log when it fails — the request vanishes without a trace.
**Consequence:** do not trust a capture-based A/B run without checking PNG size (a real
gameplay frame is ~800KB-1.5MB; ~34KB means black or the shutdown screen).

### Game data — Parallels VM disk does not automount to /Volumes
**Symptom:** Windows 11 VM's C: drive never appears under `/Volumes` even with Guest
Shared Folders automount on; VM also auto-suspends when idle. **Cause:** Parallels
guest-disk sharing not exposing the volume on this setup. **Fix:** `prlctl resume
"Windows 11"`, add a temporary share (`prlctl set --shf-host on` +
`--shf-host-add <name> --path <mac-dir>`), robocopy from the guest to `\\Mac\<name>`,
then remove the share, `--shf-host off`, and `prlctl suspend`. (Already done once —
data lives in `packages/`.)
