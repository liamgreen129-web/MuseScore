# State

Updated by the conductor at the end of every session and before every STOP.

- **Machine:** MacBook Pro (M1 Pro, 16 GB) for now; Mac mini later (DECISIONS 2026-10-29)
- **Phase:** 1
- **Current task:** P1.8 Gate G1 run, then **STOP for Liam's G1 sign-off**
- **Last finished (2026-10-29):** P1.1 server, P1.2 registry/hello/discovery, P1.3 NOTATION.md (approved), P1.4 parser/writer/validator/score printer, P1.5 synthetic fixtures (21, merged by path), P1.6 corpus fixtures (18, regenerate with tools/fixtures/make_corpus_fixtures.py), P1.7 read commands (score_overview, score_read, score_validate, score_changes, score_export, score_render)
- **Test status at last commit:** scripts/test.sh 28/28 (39/39 fixtures round-trip pitch+rhythm, each read < 2 s); tests/agent/test_server.py 29/29, test_hello.py 31/31, test_read.py 36/36
- **Next:**
  1. G1 run: the G1 bullets in assessment.md (round-trip on every fixture: done in gtests; score_export matches a UI save: not yet checked; 30-page read < 2 s: done with the quartet movements). Write the G1 report in docs/agent/G1-report.md, then STOP.
  2. After G1: Phase 2 (P2.1 transaction core: core-v1 freeze). Core inputs already collected in DECISIONS ("P0.S findings that bind the core" and later lines).
- **Blockers / waiting on Liam:**
  1. Update the daily app 4.7.4 → 4.7.6.
  2. Optional: create the "MuseScore Agent Local" code-signing certificate (Keychain Access) so macOS stops re-asking for Documents access after rebuilds; scripts/sign.sh uses it automatically.
- **Background jobs:** none
- **Worktrees in use:** none
- **Notes for the next session:**
  - Liam's preference: use judgement, ask only direct questions; never leave the session idle when a non-STOP task is available.
  - Subagent worktrees are cut from the fork's default branch (upstream master), not agent/4.7: tell subagents to work from agent/4.7, and bring results over by path.
  - Integration tests launch the app with `open -n -g -j` and MUSESCORE_AGENT_BACKGROUND=1 so it never covers Liam's windows. Kill leftovers with `pkill -f agent.install/mscore.app`.
  - Run gtests with scripts/test.sh (build.tests tree); AGENT_DUMP_DIR=dir dumps every fixture's printed notation.
  - Qt offscreen/minimal platforms crash MuseScore 4.7.6; headless runs are background GUI runs.
