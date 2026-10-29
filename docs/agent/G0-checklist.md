# Gate G0 checklist

From `PHASE0.md` §6. The conductor ticks the automated rows; Liam does the rest and
signs off in chat. Installed 2026-10-29 as `/Applications/MuseScore Agent.app`
(4.7.6, build 32ebe61, MacBook Pro).

## Automated (done by the conductor)

- [x] `scripts/check-identity.sh` passes on the built and the installed bundle (11/11).
- [x] Finder defaults unchanged after install: `.mscz` and `.musicxml` open in
      MuseScore 4, `musescore://` links go to MuseScore 4. The agent build is only in
      "Open With".
- [x] Official settings snapshot taken before first launch:
      `g0-snapshot/20261029-1927/` (git-ignored). `scripts/g0-compare-settings.sh`
      passes against it.
- [x] `scripts/sync-upstream.sh --dry-run` runs: up to date at v4.7.6.

## Conductor's run of the manual checks (2026-10-29, 20:19–20:25)

Done with two throwaway scores (`g0-test-1/2.mscz`, made with music21, converted by the
agent build's own `mscore -o`).

- [x] Agent build launches; first launch creates its own
      `~/Library/Application Support/MuseScore/MuseScoreAgent/` folder.
- [x] Both apps run at once as separate processes.
- [x] Quitting MuseScore 4 leaves MuseScore Agent running.
- [x] Opening a second score in the agent build spawns a second **agent** process;
      the official app is not touched.
- [x] Official settings: the only non-log changes (`update_request_history.json` at
      20:19:40, `workspaces/Default.mws` at 20:21:18) were written by the official app
      itself (its update check, and its quit at 20:21:18). No agent process had any
      file open under `MuseScore4/`.
- [~] Quitting MuseScore Agent with MuseScore 4 open: not completed. The agent build's
      first-launch dialogs ("Getting started", a Muse Sounds promo) block quit and
      don't respond to background automation. Needs Liam: dismiss them once, then quit.
- [~] Muse Sounds: **not installed for 4.7 on this Mac**. Both apps log
      `MuseSampler library not found: ~/Library/Application Support/MuseSampler/lib/…`;
      the `/usr/local/lib` copy (May 2024) is not on 4.7's search path. Both apps fall
      back to MS Basic identically. Re-test on the Mac mini, or after installing Muse
      Sounds through Muse Hub.

## Liam's checks

1. **First launch.** Open *MuseScore Agent* from /Applications. If macOS blocks it
   ("cannot verify"), open it from System Settings > Privacy & Security > Open Anyway
   (an ad-hoc signed build can trigger this once). It should start fresh, as a new
   install would, with "MuseScore Agent" in the menu bar.
   Set its scores folder in Preferences if you like (it defaults to
   `~/Documents/MuseScoreAgent`).
2. **Side by side.** Open MuseScore 4 as well, each with a different score.
   - Open a second score in each app (File > Open): the new window belongs to the
     app you opened it from.
   - Quit MuseScore Agent: MuseScore 4 stays open. Reopen it, then quit MuseScore 4:
     the agent build stays open.
3. **Muse Sounds.** In the agent build, open a score that uses Muse Sounds and press
   play. Note what happens (plays / silent / falls back to MuseScore Basic / error).
4. **Double-click.** Double-click an `.mscz` in Finder: it opens in MuseScore 4.
5. **Settings untouched.** After using the agent build, run
   `scripts/g0-compare-settings.sh`. It should say PASS. If you used MuseScore 4 in
   between, changes it lists may be your own (recent files, palettes); tell me and
   we'll look at them together.
6. **Daily use.** Use the agent build for real work for a few days. Report any crash
   that the official app doesn't also have.

Sign-off: tell me "G0 passes" (or what failed) in chat; the conductor records it in
`DECISIONS.md` and `STATE.md`.
