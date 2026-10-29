# MuseScore Agent — project brief for Claude Code

This repository is a fork of MuseScore Studio 4.7.6 that adds a native C++ "agent"
module: an in-process MCP server (streamable HTTP, loopback only) plus a transactional
score read/write command set built on MuseScore's own notation layer. The full design
is in `docs/agent/assessment.md` (the 28 Oct 2026 engineering assessment). This file is
the working brief; read it at the start of every session.

Owner: Liam. Build machine: Liam's home Mac mini (Apple silicon). Daily MuseScore stays
the official 4.7.6 app; this build is a **completely separate app** beside it.

## Start every session here

1. Read `docs/agent/STATE.md` (where the work is) and `docs/agent/PLAN.md` (roles,
   task list, STOP points, merge protocol). You are the **conductor** unless you were
   spawned as a subagent.
2. Follow the session protocol in PLAN.md; update STATE.md before ending or stopping.
3. Never pass a `STOP` in PLAN.md without Liam's explicit go-ahead in chat.

| File | Purpose |
|---|---|
| `docs/agent/assessment.md` | The design and its reasoning |
| `docs/agent/PLAN.md` | Who does what, in what order, where to stop |
| `docs/agent/PHASE0.md` | Detailed runbook for Phase 0 |
| `docs/agent/COMMANDS.md` | Command groups, owners, status, spec-sheet format |
| `docs/agent/specs/` | Spec sheets written by `api-scout` |
| `docs/agent/STATE.md` | Current task, next step, blockers, background jobs |
| `docs/agent/DECISIONS.md` | One dated line per decision |
| `.claude/agents/` | `api-scout`, `command-implementer`, `reviewer`, `fixture-builder` |

## Subagents, in one paragraph

Use them only where PLAN.md says: spec-sheet research, fixtures, and command groups
after the core is frozen (tag `core-v1`). The core, the notation, `write_voice`,
merges and gates are yours alone. Max 2 implementers at once (CPU and disk). Never
accept a subagent's "tests pass": rebuild the canonical tree and rerun everything.

## Non-negotiable rules

1. **Separate app, always.** Every build must keep the identity changes listed in
   `docs/agent/PHASE0.md` §3 (bundle id, application name, IPC server name, update
   module off, file-handler rank). A build that shares any of these with the official
   app can join its window group, read its settings, or answer its quit requests.
   `scripts/check-identity.sh` must pass before any build is installed.
2. **The score is the only state.** The module holds no element pointers between
   requests. Handles are EID + structural location and are re-validated against the live
   tree before use (the EID register can return freed objects after undo/delete).
3. **One request, one transaction.** `prepareChanges(name)` → `undoStack->lock()` →
   edits → `unlock()` → `commitChanges()` or `rollbackChanges()`. Notify
   `notationChanged` after commit. Validate before mutating. Check post-conditions,
   because most `INotationInteraction` methods return void.
4. **Same code paths as the UI.** Prefer `INotationInteraction` / `INotationParts` /
   `INotationStyle`; drop to `mu::engraving::Score` only where the UI does the same.
   Never simulate input. Never show dialogs, never call exit, never write outside the
   agent temp and backup folders.
5. **Minimal upstream footprint.** New code lives in `src/agent/`. Changes to upstream
   files are limited to registration lines and the identity patch, each tagged with a
   `// AGENT:` comment so `sync-upstream.sh` conflicts are easy to find.
6. **Command shapes follow MuseScore 5.** URIs like `command://notation/<name>`,
   typed input/output schemas, availability flags, mirroring `muse::rcommand`, so the
   work can rebase onto 5.0 and be proposed upstream.
7. **Gates are test results.** A phase is done when its gate passes, not when the code
   compiles. Do not move a gate.
8. **Upstream AI policy.** Anything destined for a PR to musescore/MuseScore must be
   reviewed and understood by Liam and disclosed as AI-assisted (see upstream
   `AI_POLICY.md` on master).

## Build commands (fill in once Phase 0 settles them)

```bash
# configure + build + install (arm64 only, RelWithDebInfo)
scripts/build.sh            # wraps ninja_build.sh with the agreed options
scripts/check-identity.sh   # verifies the separate-app identity in the built bundle
scripts/install.sh          # copies to /Applications/MuseScore Agent.app, ad-hoc signs
scripts/sync-upstream.sh    # fetch next 4.7.x tag, rebase agent/4.7, rebuild, test
```

## Key source locations (v4.7.6)

| What | Where |
|---|---|
| Module registration | `src/app/appfactory.cpp` |
| Notation API | `src/notation/inotation.h`, `inotationinteraction.h`, `inotationparts.h` |
| Undo / transactions | `src/notation/internal/inotationundostack.h`, `src/engraving/editing/cmd.cpp` |
| Transaction pattern to copy | `src/engraving/api/v1/score.cpp` (~lines 591–622) |
| Change records | `ScoreChanges` in `src/engraving/dom/score.h` |
| EIDs | `src/engraving/infrastructure/eid.h`, `eidregister.cpp` |
| Writers (export/render) | `src/project/inotationwriter.h`, `src/converter/internal/compat/backendapi.cpp` |
| Multi-process windows | `src/framework/multiwindows/internal/multiprocess/` |
| Upstream MCP server to port | muse_framework `framework/rcontrol/` (commit ecdc6e7) |

## Working style for this repo

- Before editing upstream files, show the diff you intend and why.
- Long builds: run in the background and report the result; never assume success.
- When a build or test fails, quote the first real error, not the tail of the log.
- Keep `docs/agent/DECISIONS.md` updated: one line per decision, with date and reason.
- `.claude/settings.json` denies edits to the official app, its settings folder, the
  Muse Sounds folder and Liam's real fixtures. Those rules cover the Edit/Write tools,
  not shell commands, so never touch those paths from Bash either.
- On first setup, append `.gitignore.agent-additions` to `.gitignore` and delete it.
