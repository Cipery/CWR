# Decisions (ADR-lite)

Newest first. Format per entry: **date · decision** — why · (alternatives rejected).
Append when a load-bearing product/architecture choice is made. Don't restate code;
capture the *reasoning* a future reader can't recover from the diff.

---

### 2026-07-24 · Metal backend pulled forward; stop GL perf tuning
Gameplay works on GL but town views are draw-call-bound (~40 µs/call CPU in Apple's
GL→Metal translation layer; measured via perf traces after UBO stalls were fixed).
— *Why:* fighting a deprecated translation layer is sunk cost when native Metal
(~1-2 µs/call) is the plan anyway, and Metal's native zero-to-one depth also fixes
the clip-control precision loss. *Alternatives rejected:* deeper GL batching/state
reduction — diminishing returns on a backend that becomes reference-only.
Mechanics: `metal-backend` branch off `macos-port`; `macos-port` stays the playable
baseline; GL33 kept for A/B validation. UBO ring-buffer fix committed on
session-wide trace evidence (8× avg improvement) without a town A/B (user call).

### 2026-07-24 · Target arm64 (Apple Silicon) only
No x86_64/universal binary for now. — *Why:* the dev machine is Apple Silicon and
universal builds are pure build-matrix overhead until there is a distribution need.
*Alternatives rejected:* universal binary — deferred, revisit at packaging time (Phase 3).

### 2026-07-24 · Bring-up on native macOS OpenGL 4.1, native Metal backend as the endgame
Port runs first on macOS's deprecated-but-working GL (engine requests exactly 3.3 Core
Forward-Compatible, which macOS grants); a native Metal backend is the final phase,
plugged in via the real selection surface — the `GraphicsBackend` enum/registry in
`GraphicsEngineFactory.hpp` (not just an `Engine` factory function). — *Why:* GL gets us
to a playable build with near-zero renderer work; Metal is a large project that must not
block bring-up. *Alternatives rejected:* (1) ANGLE/Zink/MoltenVK translation layers —
added complexity before evidence of need; (2) Metal-first — blocks everything on the
hardest workstream.

### 2026-07-24 · Game data staged in `packages/` from the user's Steam install (Parallels VM)
1.72 GB incl. `Remastered/` copied to `packages/ARMA Cold War Assault/`. — *Why:*
`packages/` is the repo's designated git-ignored staging area; binaries are useless
without data, and local data enables real smoke tests from Phase 0 on. Data is APL-SA —
never committed (see [[NORTH_STAR]]).
