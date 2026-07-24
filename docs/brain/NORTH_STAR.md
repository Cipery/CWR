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
**NATIVE METAL RENDERER COMPLETE (2026-07-24, M0-M6 in one day):** on branch
`metal-backend` the game runs on a native Metal 3 backend — menu, missions, alpha,
MSAA, shadows all A/B-equivalent to GL33, **9.1× faster** (181 fps vs 19.9 fps
mission avg on M4 Pro). `Auto` selects Metal; GL33 stays as reference via
`--render gl33`. Each milestone RFL-reviewed (opus xhigh + sol high) and committed
separately. Remaining: merge `metal-backend` → `macos-port` (user decision),
Phase 3 packaging/CI/paths, Windows/Linux builds still untested since port start.
