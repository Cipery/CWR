---
description: Curate docs/brain/ — distill recent work into the brain and prune stale entries
---

Run a maintenance pass on the project brain at `docs/brain/`. Goal: keep it the
accurate, durable, non-redundant source of project knowledge. Be conservative —
prefer a few high-value durable facts over volume.

## Steps

1. **Read the brain.** Open `docs/brain/INDEX.md`, `NORTH_STAR.md`, `DECISIONS.md`,
   `GOTCHAS.md`, `PATTERNS.md`.

2. **Scan recent work for durable knowledge** not yet captured:
   - The newest handoff/decision docs under `docs/` (e.g. `ls -t docs/*HANDOFF* 2>/dev/null | head -5`).
   - Recent commits: `git log --oneline -20`.
   - The AI agent's own cross-session memory, for decisions/gotchas worth promoting
     into the in-repo brain.
   Extract only **load-bearing decisions** (with their *why*) and **gotchas**
   (symptom · cause · fix). Skip anything recoverable from the code or already in
   CLAUDE.md / docs/ — link to those instead.

3. **Update the files:**
   - Append new decisions to `DECISIONS.md` (newest first; date · decision — why).
   - Append new traps to `GOTCHAS.md`.
   - Add new durable cross-cutting conventions to `PATTERNS.md` (pointer-style).
   - Add/refresh one-liners in `INDEX.md`.

4. **Prune.** Strike or delete any entry that is now false, superseded, or
   contradicted by a newer one. Never leave two conflicting facts. Merge duplicates.
   Keep one fact per entry.

5. **Verify links.** `[[wikilinks]]` must resolve to a `docs/brain/<name>.md` file;
   relative paths to external docs must still exist.

6. **Report** a short diff summary: what was added, pruned, merged. Do NOT commit
   unless asked.
