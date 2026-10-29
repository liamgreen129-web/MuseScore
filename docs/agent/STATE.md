# State

Updated by the conductor at the end of every session and before every STOP.

- **Machine:** MacBook Pro (M1 Pro, 16 GB) for now; Mac mini later (DECISIONS 2026-10-29)
- **Phase:** 2
- **Current task:** P2.1 transaction core: implemented and tested; reviewer pass, then tag `core-v1`. Next P2.3 write_voice (conductor) while P2.2 (scenario driver, subagent) finishes.
- **Last finished (2026-10-29):** P1.1 server, P1.2 registry/hello/discovery, P1.3 NOTATION.md (approved), P1.4 parser/writer/validator/score printer, P1.5 synthetic fixtures (21, merged by path), P1.6 corpus fixtures (18, regenerate with tools/fixtures/make_corpus_fixtures.py), P1.7 read commands (score_overview, score_read, score_validate, score_changes, score_export, score_render), P1.8 G1 run (signed off by Liam 2026-10-29), P2.1 core (see CORE.md)
- **Test status at last commit:** scripts/test.sh 45/45 (17 core + 28 notation/printer; 40/40 fixtures round-trip); tests/agent/test_server.py 29/29, test_hello.py 32/32, test_read.py 39/39, test_core.py 45/45, test_export.py 160/160 (last run at G1)
- **Next:**
  1. Fold in the reviewer's findings on the core, rerun everything, commit, tag `core-v1`, push the tag.
  2. Merge P2.2 when its implementer reports (merge protocol: reviewer, rerun all tests here).
  3. P2.3 write_voice and the rest of the notes group (conductor), against CORE.md.
  4. P2.4 Wave A (max 2 implementers at once), once P2.2 is merged.
- **Blockers / waiting on Liam:**
  1. Update the daily app 4.7.4 → 4.7.6.
- **Background jobs:** P2.2 command-implementer (own worktree under .claude/worktrees/); reviewer on the P2.1 diff
- **Worktrees in use:** none
- **Notes for the next session:**
  - Builds are signed with the self-signed "MuseScore Agent Local" identity in the login keychain (untrusted on purpose; codesign accepts it, and the designated requirement pins its leaf hash, so macOS privacy grants survive rebuilds). On the Mac mini, create it again (DECISIONS 2026-10-29) or builds fall back to ad-hoc.
  - Liam's preference: use judgement, ask only direct questions; never leave the session idle when a non-STOP task is available.
  - Subagent worktrees are cut from the fork's default branch (upstream master), not agent/4.7: tell subagents to work from agent/4.7, and bring results over by path.
  - Integration tests launch the app with `open -n -g -j` and MUSESCORE_AGENT_BACKGROUND=1 so it never covers Liam's windows. Kill leftovers with `pkill -f agent.install/mscore.app`.
  - Run gtests with scripts/test.sh (build.tests tree); AGENT_DUMP_DIR=dir dumps every fixture's printed notation.
  - Qt offscreen/minimal platforms crash MuseScore 4.7.6; headless runs are background GUI runs.
