# North Star

## What it is
Fork of Bohemia Interactive's GPL source release of the *Arma: Cold War Assault
Remastered* engine (codename **Poseidon** — the original Operation Flashpoint engine,
modernized to C++20 / CMake / Clang, SDL3 + OpenGL 3.3 + OpenAL, Windows x64 + Linux
x64). **Our mission: port the engine to macOS on Apple Silicon (arm64), ultimately
with a native Metal renderer.** Full research + phased plan: `../MACOS_PORT.md`.

## Who it's for
Ourselves and the macOS Arma/OFP community. Upstream (`BohemiaInteractive/CWR`) is a
locked repository — no PRs accepted there; all work lives on the `Cipery/CWR` fork.

## Hard "NEVER" rules
- **NEVER commit game data** (`packages/` — APL-SA licensed, not GPL; it is git-ignored
  and must stay that way).
- **NEVER use "ARMA" / "Operation Flashpoint" branding in a public release** — the
  trademarks are not granted by the GPL license; a public fork must be renamed.
- Code stays **GPL-3.0-or-later with Section 7 additional terms** — see `LICENSE`.
- `thirdparty/` is excluded from the project GPL (vendored code, own licenses).

## Current status (keep fresh)
**Playable on Apple Silicon (2026-07-24):** Phases 0-2 done in one day — client runs,
menu + missions play, audio works, HiDPI fixed, UBO sync stalls fixed. Remaining perf
ceiling = Apple GL translation layer (~40 µs/draw call) → **Metal backend pulled
forward** (see [[DECISIONS]]); work happens on `metal-backend` branch, `macos-port`
stays the playable baseline. Phase 3 (packaging/CI/paths) intentionally open.
Caveat: Windows/Linux builds untested since the port started (no CI yet).
