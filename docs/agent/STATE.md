# State

Updated by the conductor at the end of every session and before every STOP.

- **Machine:** MacBook Pro (M1 Pro, 16 GB) for now; Mac mini later (DECISIONS 2026-10-29)
- **Phase:** 2
- **Current task:** paused at a clean point (2026-10-30) to move the work to a Claude cloud session. Everything is committed and pushed to `agent/4.7` (8e7a2425d2). No worktrees, no running subagents or app instances.
- **Last finished:** P2.4 Wave A first half merged: expression-lines (add_slur, add_line) and expression-marks (add_dynamic, add_hairpin, add_articulation, add_ornament, add_fermata, add_breath), each reviewed twice. Notes group: write_voice, clear_range, tie, tuplet, grace. Shared address module (src/agent/address). Read-printer end-mark fix. Goldens ignore element ids.
- **Test status at last commit (Mac, canonical tree):** scripts/test.sh 137/137; test_server, test_hello, test_read, test_core PASS; test_export spot-check 0 failures; check-identity PASS; scenarios/run.py 29 ok, 0 FAIL, 6 SKIP (S07-S10, S14 need text/pitch commands; S18 needs a test_modal hook).
- **Progress (estimate):** Phase 2 about 65%; whole plan about 45% (P0, P1 done; P3, P4 not started).
- **Next, in order:**
  1. Printer: system text lines (TextLine with the system flag) print no start or end mark. In collectSpanners (src/agent/read/scoreprinter.cpp), when startCR()/endCR() give nothing for a non-slur spanner, find the chord/rest at tick on the staff (any voice); EL04 has a `questions` note to tighten once fixed.
  2. Add a `test_modal` action to test_dispatch (src/agent/commands/test/) so S18 runs.
  3. Wave A second half: `text` (add_text, add_chord_symbol, add_lyric; specs/text.md) and `pitch` (set_pitch, transpose; specs/pitch.md), at most 2 command-implementers. Tell them: start from agent/4.7; use src/agent/address/address.h for every staff/position/target argument; put the engraving-only operations in their own static library like agent_notes/agent_lines/agent_marks, linked by agent and agent_tests; no default behaviour may depend on app preferences; error codes only from errors.h (no new group codes, DECISIONS 2026-10-29); a unit test per validation error path checking nothing changed; scenarios plus undo/redo; the conductor adds the allcommands line at merge. Review each with the `reviewer` agent, then merge per protocol (rerun everything on the canonical tree).
  4. Rest of the notes group (conductor): add_note_to_chord, remove_note, set_duration, exchange_voices (specs/notes.md), on the address module.
  5. Move expression-lines and expression-marks onto address/address.h (they still have their own parsers; behaviour is the same shape, so this is a refactor with their tests as the net).
  6. P2.5: S01-S20 all passing, 8-hour soak (tests/agent/harness/soak.py --minutes 480), G2 report, then **STOP for Liam's G2 sign-off**.
  7. Follow-up: can closing a score after a big range delete crash the real app (DECISIONS 2026-10-29, upstream bug)?
- **Cloud session: what works there and what does not.**
  - The build and every test need this Mac: scripts/env.sh points at ~/Qt/6.10.2/macos and ~/musescore_deps_macos; the integration tests and scenarios launch the macOS app bundle (agent.install/mscore.app) with `open`; signing uses the "MuseScore Agent Local" keychain identity. A Linux cloud box has none of these, so it cannot build, run the unit tests, or run scenarios unless it sets up a Linux build of agent_tests itself (Qt 6.10.2 + MuseScore Linux deps; not tried, and the gtest tree alone is a large build).
  - Corpus fixtures (tests/fixtures/corpus/, 20 files) are git-ignored and not in the repo; tools/fixtures/make_corpus_fixtures.py regenerates them (needs network and a working mscore build). Synthetic fixtures are committed.
  - Rule 7 still holds: a gate or merge needs test results. Work done in the cloud goes on a branch (not agent/4.7) and is verified on the Mac before it is merged: rebuild, scripts/test.sh, all tests/agent/*.py, scenarios.
  - Good cloud work without a build: spec-driven code for steps 1-5 with unit tests written alongside, reviews, docs. Mark every such commit "unverified" in its message until the Mac run passes.
  - Never push to upstream musescore/MuseScore; never force-push; the official app and Liam's real scores are off limits (CLAUDE.md).
- **Blockers / waiting on Liam:**
  1. Update the daily app 4.7.4 → 4.7.6.
- **Background jobs:** none.
- **Worktrees in use:** none.
- **Notes for the next session:**
  - **write_voice design (P2.3, decided, not yet coded)** — file `src/agent/commands/notes/` (own register.cpp; conductor adds to allcommands):
    - Args: `staff` (int, 1-based like score_read), `content` = notation voice lines `m<N> v<V> | ...` exactly as score_read prints (headers/comments ignored; bar attrs, harmony lines, `+marks` and slurs refused in v1 with a message naming the commands that add them), `pitch` sounding|written (default sounding), `keepAnnotations` (default true). Each (bar, voice) line replaces that voice in that bar.
    - Validate first: parse errors (line/col) → E_INVALID_ARG; bars exist; nt::validate with bar lengths from the score (Measure::ticks × staff timeStretch); `_` not in voice 1; grace on rest; pitch 0–127; tuplet total/normal must be a valid TDuration (else Score::addTuplet's MessageBox); refuse tab/drum staves and measure repeats.
    - Clear: Score::deleteRange(s1, s2, track, track+1, private SelectionFilter: only that voice; with keepAnnotations exclude DYNAMIC/HAIRPIN/CHORD_SYMBOL/OTHER_TEXT/FIGURED_BASS/FRET_DIAGRAM/OTTAVA/PEDAL_LINE/OTHER_LINE). Voice 1 becomes a measure rest; voices 2–4 vanish.
    - Write with a private InputState (setTrack, setSegment(bar's CR segment), setDuration): notes Score::addPitch(nv,false,&is), chord tones addPitch(nv,true,&is); NoteVal pitch = concert midi, tpc1 = spelled tpc (sounding mode) or tpc2 (written mode), the other left invalid so Note::setNval derives it; rests enterRest(d,&is); `_` = rest then undoChangeProperty(Pid::GAP,true) (UI deleteItem path); `R` in voices 2–4 = expandVoice; tuplets = rest of the total length, Score::addTuplet(cr, Fraction(actual,normal), SHOW_NUMBER, AUTO_BRACKET), then write inner items from that segment (nested: addTuplet on an element already in a tuplet); graces setGraceNote(chord, pitch, type, len) then fix tpc via undoChangeProperty; ties after all bars, with the createAndAddTie recipe (edit.cpp:2027) between created notes or into the next bar's note in the score.
    - Post-condition: print the written bars (printScore, that staff/voice), strip marks/slurs, compare items to the input exactly (pitch spelling, rhythm, ties, tuplets, graces); mismatch → E_POSTCONDITION naming the first difference. Return the printed lines.
    - Spec scenarios NOTES-WV-01..13 (specs/notes.md) are the test list; grace order (OQ2) must be confirmed by a test.
  - Builds are signed with the self-signed "MuseScore Agent Local" identity in the login keychain (untrusted on purpose; codesign accepts it, and the designated requirement pins its leaf hash, so macOS privacy grants survive rebuilds). On the Mac mini, create it again (DECISIONS 2026-10-29) or builds fall back to ad-hoc.
  - Liam's preference: use judgement, ask only direct questions; never leave the session idle when a non-STOP task is available.
  - Subagent worktrees are cut from the fork's default branch (upstream master), not agent/4.7: tell subagents to work from agent/4.7, and bring results over by path.
  - Integration tests launch the app with `open -n -g -j` and MUSESCORE_AGENT_BACKGROUND=1 so it never covers Liam's windows. Kill leftovers with `pkill -f agent.install/mscore.app`.
  - Run gtests with scripts/test.sh (build.tests tree); AGENT_DUMP_DIR=dir dumps every fixture's printed notation.
  - Qt offscreen/minimal platforms crash MuseScore 4.7.6; headless runs are background GUI runs.
