# State

Updated by the conductor at the end of every session and before every STOP.

- **Machine:** MacBook Pro (M1 Pro, 16 GB) for now; Mac mini later (see DECISIONS 2026-10-29)
- **Phase:** 1
- **Current task:** P1.7 read commands (score_overview, score_read, score_export, score_render, score_changes, score_validate)
- **Last finished:** P1.4 printer + parser (28/28 tests; G1 round-trip met on 18 corpus scores); P1.3 approved; P1.6; P1.2; P1.1
- **Next:** P1.7, then merge P1.5 synthetic fixtures and run them through the same tests, then G1 (STOP)
- **Blockers:**
  1. Liam to update the daily app 4.7.4 → 4.7.6.
- **Background jobs:** fixture-builder subagent on P1.5 (own worktree), writes tests/fixtures/synthetic/ + tools/fixtures/
- **Worktrees in use:** none
