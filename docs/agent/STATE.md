# State

Updated by the conductor at the end of every session and before every STOP.

- **Machine:** MacBook Pro (M1 Pro, 16 GB) for now; Mac mini later (see DECISIONS 2026-10-29)
- **Phase:** 0
- **Current task:** P0.2 waiting at STOP (Qt 6.10.2 archive and prebuilt deps are >1 GB downloads)
- **Last finished:** P0.1 preflight; P0.2 Homebrew packages; P0.3 fork, `agent/4.7` at v4.7.6, kit committed
- **Next:** with Liam's OK: Qt + deps download (P0.2 rest); meanwhile P0.4 identity patch (show diff first) and P0.5 scripts
- **Blockers:**
  1. Go-ahead for the >1 GB Qt and deps downloads.
  2. Xcode CLT only. Find out at first configure/install whether full Xcode is needed (another >1 GB STOP).
  3. Liam to update the daily app 4.7.4 → 4.7.6.
- **Background jobs:** none
- **Worktrees in use:** none
