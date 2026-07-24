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

### Threads — Foundation/Threads compiles on macOS but is runtime-broken (build gate lies)
**Symptom:** arm64 build succeeds, threaded code deadlocks/fails at runtime on macOS.
**Cause:** `PoSemaphore.cpp`/`MultiSync.hpp` use unnamed POSIX semaphores (`sem_init` = ENOSYS
stub on Darwin) and `PoCritical.cpp` uses glibc-only `PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP`
with a copy-assign init idiom. Both compile cleanly. **Fix:** Darwin backends
(`dispatch_semaphore_t`, `pthread_mutex_init` + recursive attr) — see `../MACOS_PORT.md`
Phase 0. Lesson: for this codebase, "it compiles on macOS" proves nothing about threads —
smoke-run a threaded binary.

### Game data — Parallels VM disk does not automount to /Volumes
**Symptom:** Windows 11 VM's C: drive never appears under `/Volumes` even with Guest
Shared Folders automount on; VM also auto-suspends when idle. **Cause:** Parallels
guest-disk sharing not exposing the volume on this setup. **Fix:** `prlctl resume
"Windows 11"`, add a temporary share (`prlctl set --shf-host on` +
`--shf-host-add <name> --path <mac-dir>`), robocopy from the guest to `\\Mac\<name>`,
then remove the share, `--shf-host off`, and `prlctl suspend`. (Already done once —
data lives in `packages/`.)
