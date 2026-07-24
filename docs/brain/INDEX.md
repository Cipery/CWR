# CWR — Project Brain (index)

One-liner pointers to the durable project knowledge. This file is auto-injected
at session start. Keep it to one line per entry; full content lives in the linked
files. When something here goes stale, fix it the same turn you notice.

- [[NORTH_STAR]] — what this project is, who it's for, the hard "NEVER" rules.
- [[DECISIONS]] — load-bearing product/architecture decisions + the *why*.
- [[GOTCHAS]] — traps that already bit us; read before touching the matching area.
- [[PATTERNS]] — recurring conventions (mostly pointers into CLAUDE.md / docs).

## How this folder is maintained
- **Read:** before non-trivial work, skim this index + the relevant file.
- **Write:** after a load-bearing decision -> append to [[DECISIONS]]; after hitting a
  trap -> append to [[GOTCHAS]]. One fact per entry. When a fact stops being true,
  strike it (~~like this~~) or delete it — never leave contradictory entries.
- **Curate:** run `/brain` to distill recent work into here and prune superseded rows.
- **Scope:** this is the *project's* in-repo memory (committed, Obsidian-visible).
  It is distinct from the AI agent's private cross-session memory. Link out to
  CLAUDE.md and docs/ instead of copying them.
