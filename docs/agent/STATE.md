# State

Updated by the conductor at the end of every session and before every STOP.

- **Machine:** Liam's Mac mini (M6, 12 cores, 16 GB, 33 GB free; preflight in DECISIONS 2026-10-30). Moved and built here.
- **Phase:** 2
- **Current task:** verifying agent/4.7-nfhpdz on the Mac mini before the merge into agent/4.7. Built, unit tests and integration tests green; 3 scenarios fail on upstream behaviour (below). **Not merged**: rule 7, and the three need Liam's call.
- **Last finished (cloud, unverified):** Wave A is complete in code: notes (all ten commands), expression-lines, expression-marks, text (add_text, add_chord_symbol, add_lyric) and pitch (set_pitch, transpose), all registered in allcommands.cpp. Each group was reviewed twice by the reviewer agent, and every finding is fixed or recorded in DECISIONS. Also done: printer marks for system lines, tempo text without SMuFL symbols, the test_modal hook (S18), the expression groups moved onto address/address.h, MCP schemas with no "type" for any-typed arguments, specs/pitch.md and specs/text.md corrected, and the scenarios README.
- **Checks run here:** `g++ -std=c++20 -fsyntax-only -Wall -Wextra -DNO_QT_SUPPORT -DENGRAVING_NO_API` on every src/agent .cpp that does not include Qt (all command libraries, the printer, the notation library and all unit-test files): no errors. The Qt-dependent files (server, writes, app glue, read/export/session/test commands) could not be checked. Nothing has been linked or run.
- **Test status (Mac mini, 2026-10-30):** scripts/test.sh 250/250; tests/agent/test_server, test_hello, test_read, test_core, test_export all PASS; corpus fixtures 19/19 regenerated (music21 10.5.2); scenarios 48 ok / 3 FAIL: PT06 (undo of a transpose that keyed a C staff with only a generated key signature leaves an explicit C KeySig: ChangeKeySig::flip reads currentKeyTick(0) == 0 as "an event at 0" even with an empty key list, editkeysig.cpp:46, and GENERATED is not restored); TX05 (melisma_to "self" uses TEMP_MELISMA_TICKS, which layout's removeInvalidSegments sets to 0 without undo once the separator exists, lyrics.cpp:588-602, so redo does not restore the saved state); S14 (write_voice then undo on the regenerated Bach chorale drops a Beam element with an explicit stem direction: the corpus fixtures were regenerated with music21 10.5.2 and may encode differently from the MacBook's set). All three are upstream engine behaviour reached through the same calls as the UI; fixing them needs an upstream change (rule 5) or a decision to accept or work around them.
- **Progress (estimate):** Phase 2 is about 90% written and 65% verified; the whole plan is about 45% verified.
- **Next, in order:**
  1. **On the Mac:** `git fetch && git checkout agent/4.7-nfhpdz`, then `scripts/build.sh`. Fix compile errors first; the Qt-side files were never checked.
  2. Run `scripts/test.sh` (it now includes text_tests and pitch_tests) and all of `tests/agent/*.py`.
  3. Run `scenarios/run.py`; N03, N04, TX01-07 and PT01-07 need `--update-goldens` once, then review the goldens.
  4. Check that test_modal's dialog does not bring the background instance forward (DECISIONS 2026-10-30).
  5. Merge into agent/4.7 per protocol.
  6. P2.5: S01-S20 and all group scenarios passing (no soak: DECISIONS 2026-10-30), the G2 report, then **STOP for Liam's G2 sign-off**.
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
