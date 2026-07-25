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

### Game data — Parallels VM disk does not automount to /Volumes
**Symptom:** Windows 11 VM's C: drive never appears under `/Volumes` even with Guest
Shared Folders automount on; VM also auto-suspends when idle. **Cause:** Parallels
guest-disk sharing not exposing the volume on this setup. **Fix:** `prlctl resume
"Windows 11"`, add a temporary share (`prlctl set --shf-host on` +
`--shf-host-add <name> --path <mac-dir>`), robocopy from the guest to `\\Mac\<name>`,
then remove the share, `--shf-host off`, and `prlctl suspend`. (Already done once —
data lives in `packages/`.)
