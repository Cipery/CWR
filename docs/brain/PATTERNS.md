# Patterns & conventions

Recurring conventions for this codebase. This file is mostly **pointers** — the
authoritative rules live in CLAUDE.md and the project's rules/docs. Don't duplicate
them here; link, and only add a pattern here when it's genuinely cross-cutting and
not already written down elsewhere.

## Architecture
- Engine layout: `engine/README.md` (Poseidon core lib, PoseidonGL33/PoseidonOpenAL
  backends, PoseidonFormats C API); apps: `apps/README.md`; Rust: `mserver/README.md`.
- Renderer backends plug in via `engine/Poseidon/Graphics/Core/Engine.hpp` +
  `EngineFactory.hpp` (see `../MACOS_PORT.md` Phase 4).
- Platform code splits: `Foundation/Platform/`, `IO/Filesystem/platform/`
  (`*_win.cpp` / `*_posix.cpp`, selected in `engine/Poseidon/CMakeLists.txt`).

## Testing
- `tests/README.md` — CI compiles tests only. Rust test orchestrator: `engine/Trident/`.

## Conventions
- Upstream conventions: `CONTRIBUTING.md`, `.clang-format`, `.clang-tidy`.
- macOS port work: follow the phase plan in `../MACOS_PORT.md`; keep upstream
  (`upstream` remote) mergeable — prefer additive `APPLE` branches over rewriting
  existing platform forks.
