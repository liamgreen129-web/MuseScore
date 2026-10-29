# Execution plan for Claude Code

This is the plan Claude Code executes, session by session, on the Mac mini. The
assessment (`assessment.md`) says *what* and *why*; `PHASE0.md` is the detailed
runbook for the first phase; this file says *who does what, in what order, and where
to stop*. `STATE.md` records where the work currently is.

## Roles

| Role | Who | Does | Never does |
|---|---|---|---|
| **Conductor** | The main Claude Code session | Owns `agent/4.7`, the canonical build tree, the frozen core, every merge, every gate run, `STATE.md` | Accept a subagent's "done" without re-running the tests itself |
| `api-scout` | Subagent, read-only | Maps one command group onto the 4.7.6 code the UI uses; writes a spec sheet | Edit source, build |
| `command-implementer` | Subagent, own worktree | Implements one command group against a frozen core and its spec sheet, with unit tests and scenario scripts | Touch the core, upstream files, or another group's files |
| `reviewer` | Subagent, read-only, fresh context | Reviews one diff against its spec sheet and the rules in `CLAUDE.md` | See the implementer's reasoning; fix things itself |
| `fixture-builder` | Subagent, own worktree | Builds synthetic stress scores and golden exports | Modify Liam's real scores |
| **Liam** | Human | Approves installs, reviews the notation design, supplies real scores, signs off every gate | — |

Definitions are in `.claude/agents/`. The conductor writes every subagent prompt
using the template at the end of this file.

## Where subagents are used, and where they are not

Parallel work only where units are independent, fully specified, and checkable by a
test the conductor can run. Single-threaded everywhere else.

- **Single-threaded (conductor only):** toolchain, identity patch, MCP server port,
  registry, transaction core, handle resolution, compact notation grammar, parser and
  printer, `write_voice`, every merge, every gate, Phase 4 polish.
- **Parallel:** spec-sheet research (from Phase 0, while the first build runs),
  fixture generation, and command groups in Phases 2 and 3 *after* the core is frozen.

### Build-tree rules for parallel implementers

- The conductor's `build.release` tree is canonical. Only the conductor installs.
- Each implementer worktree has its own build directory. ccache is mandatory, with
  `CCACHE_BASEDIR` set to the worktrees' common parent so different worktree paths
  still hit the cache.
- **At most 2 implementers at once.** ninja already saturates the CPU, and each build
  tree costs roughly 15–20 GB. Before spawning, the conductor checks `df -h` and keeps
  at least 30 GB free.
- Each command group lives in its own directory, `src/agent/commands/<group>/`, with
  its own `register.cpp`. The single list that includes all groups is edited only by the
  conductor, at merge time, so implementers never conflict.

### Merge protocol (one group at a time)

1. Implementer reports: branch name, files changed, tests added, its own test results.
2. Conductor spawns `reviewer` with the diff, the spec sheet and `CLAUDE.md`, nothing else.
3. Blocking review findings go back to the same implementer (SendMessage), not to a new one.
4. Conductor rebases the branch onto `agent/4.7`, builds the canonical tree, runs all
   unit tests and **all** scenarios (not only the new ones), and runs a 30-minute soak.
5. Only then merge, update `COMMANDS.md` status, update `STATE.md`.

## Task list

`STOP` marks a point where the conductor reports to Liam and waits.
`∥` marks tasks that run in parallel with the task above.

### Phase 0 · toolchain and vanilla build (runbook: `PHASE0.md`)

| ID | Task | Owner | Depends |
|---|---|---|---|
| P0.1 | Preflight checks, recorded in `DECISIONS.md`. **STOP** | Conductor | — |
| P0.2 | Install Homebrew packages, Qt 6.10.2, prebuilt deps, ccache. **STOP before any `sudo`, Xcode install or download over 1 GB** | Conductor | P0.1 |
| P0.3 | Fork, `agent/4.7` at `v4.7.6`, commit docs and `.claude/` | Conductor | P0.1 |
| P0.4 | Identity patch (one commit, `// AGENT:` tags) and `check-identity.sh` | Conductor | P0.3 |
| P0.5 | `env.sh`, `build.sh`, `install.sh`, `sync-upstream.sh` | Conductor | P0.4 |
| P0.6 | First build in the background, logged to `build.log`; install | Conductor | P0.2, P0.5 |
| P0.S | ∥ Spec sheets for all groups in `COMMANDS.md`, written to `specs/<group>.md`; up to 4 scouts at once (read-only, no build needed) | `api-scout` ×N | P0.3 |
| P0.7 | Gate G0 automated checks, then Liam's manual checks. **STOP: G0 sign-off** | Conductor + Liam | P0.6 |

### Phase 1 · module skeleton and reading

| ID | Task | Owner | Depends |
|---|---|---|---|
| P1.1 | Fetch muse_framework `rcontrol` at `ecdc6e7`; port server and HTTP transport into `src/agent/server/`; loopback, origin check, bearer token, port base 2412 | Conductor | G0 |
| P1.2 | Registry (rcommand shape), `hello`, preferences page, token file, per-instance discovery records | Conductor | P1.1 |
| P1.3 | Write `NOTATION.md`: the compact notation grammar, with worked examples from Liam's own repertoire. **STOP: Liam reviews the notation** | Conductor | P0.S |
| P1.4 | Printer (score → notation) and parser (notation → model), unit tests | Conductor | P1.3 approved |
| P1.5 | ∥ Synthetic stress fixtures (every tuplet ratio, nested tuplets, 4 voices, cross-staff, linked parts, lyrics with melismas, grace notes, pickups, irregular bars) | `fixture-builder` | P0.7 |
| P1.6 | Liam supplies 10–20 real scores into `tests/fixtures/real/` (git-ignored). **STOP until provided** | Liam | — |
| P1.7 | `score_overview`, `score_read`, `score_export`, `score_render`, `score_changes`, `score_validate` | Conductor | P1.4 |
| P1.8 | Gate G1 run. **STOP: G1 sign-off** | Conductor + Liam | P1.5–P1.7 |

### Phase 2 · transactions and core writes

| ID | Task | Owner | Depends |
|---|---|---|---|
| P2.1 | Transaction core: `AgentTransaction`, `SelectionGuard`, `HandleResolver`, error codes, post-condition helpers, `stateVersion`, batch. Commit headers and tag **`core-v1`** (the freeze) | Conductor | G1 |
| P2.2 | ∥ Python scenario driver: launch offscreen with `--agent-port`, run S-scripts over MCP, export, diff against goldens; soak generator from registry schemas | `command-implementer` (harness) | G1 |
| P2.3 | `write_voice` and the rest of the notes-and-rhythm group | Conductor | P2.1 |
| P2.4 | Wave A, max 2 at a time: `expression-marks` (dynamic, hairpin, articulation, ornament, fermata, breath), `expression-lines` (slur, line), `text` (text, chord symbol, lyric), `pitch` (set_pitch, transpose) | `command-implementer` ×4 | P2.1, P2.2 |
| P2.5 | Merge Wave A per protocol; S1–S20; 8-hour soak. **STOP: G2 sign-off** | Conductor | P2.3, P2.4 |

### Phase 3 · structure, instrumentation, generic layer

| ID | Task | Owner | Depends |
|---|---|---|---|
| P3.1 | Any core changes Wave B needs (core-v2), decided by the conductor before fan-out | Conductor | G2 |
| P3.2 | Wave B, max 2 at a time: `structure`, `instrumentation`, `generic` (set_property, remove, select, cmd allow-list), `style-layout` | `command-implementer` ×4 | P3.1 |
| P3.3 | Multi-instance forwarding (`instance` argument, `instances_list`) | Conductor | P3.1 |
| P3.4 | ASan/UBSan Debug build tree (separate, ~20 GB) | Conductor | P3.1 |
| P3.5 | Merge Wave B; S21–S40; 24-hour sanitizer soak. **STOP: G3 sign-off** | Conductor | P3.2–P3.4 |

### Phase 4 · polish and daily use

| ID | Task | Owner | Depends |
|---|---|---|---|
| P4.1 | Status-bar indicator, Agent preferences section, progress and cancellation, latency log | Conductor | G3 |
| P4.2 | `musescore-agent` skill (working method, notation legend, verify loop); tool descriptions | Conductor | G3 |
| P4.3 | Liam composes with it for two weeks; conductor turns every problem into a scenario. **STOP: G4 sign-off** | Liam + Conductor | P4.1, P4.2 |

### Phase 5 · MuseScore 5 and upstream — planned after G4.

## Session protocol for the conductor

At the start of every session:
1. Read `CLAUDE.md`, then `STATE.md`, then the current phase's rows above.
2. `git status`; confirm the branch and that the canonical build exists.
3. State in one line what this session will do, then do it.

At the end of every session (or before any `STOP`):
1. Update `STATE.md`: current task ID, what finished, what's next, open blockers,
   running background jobs and where they log.
2. Commit docs changes.

## Subagent prompt template

```
Task <ID>: <one-line goal>.
Read first: CLAUDE.md, docs/agent/PLAN.md (your role row), <spec sheet path>,
<frozen core headers, if implementing>.
You may change only: <paths>.
Definition of done: <tests that must exist and pass; output files; format>.
Report back: branch or output path, files changed, commands you ran and their
exact results, anything in the spec you found to be wrong.
Do not: <role's "never does" list>.
```
