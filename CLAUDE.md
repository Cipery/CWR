# CWR — Arma: Cold War Assault Remastered engine (macOS port fork)

## Project Brain — `docs/brain/` (READ + MAINTAIN)

`docs/brain/` is the project's durable, in-repo knowledge base (committed,
Obsidian-friendly). `docs/brain/INDEX.md` is auto-injected at session start.

- **READ:** before non-trivial work, skim `docs/brain/INDEX.md` and open the relevant
  file — `NORTH_STAR.md`, `DECISIONS.md`, `GOTCHAS.md`, `PATTERNS.md`. Resolve
  `[[wikilinks]]` to `docs/brain/<name>.md`.
- **MAINTAIN (same turn, not later):**
  - Load-bearing decision -> append to `DECISIONS.md` (date · decision — why · alternatives).
  - Hit a trap / debugged something non-obvious -> append to `GOTCHAS.md` (symptom · cause · fix).
  - A fact stopped being true -> strike or delete it; never leave contradictory entries.
- **DON'T duplicate** `CLAUDE.md` / `docs/**` — link to them. Brain holds *reasoning and
  traps*, not what the code or those docs already state.
- For a periodic curation pass, run `/brain`.
