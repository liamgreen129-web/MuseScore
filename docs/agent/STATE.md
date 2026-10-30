# State

Updated by the conductor at the end of every session and before every STOP.

- **Machine:** MacBook Pro (M1 Pro, 16 GB) for now; Mac mini later (DECISIONS 2026-10-29)
- **Phase:** 2
- **Current task:** cloud session (2026-10-30) on branch `agent/4.7-nfhpdz`, Linux, no build possible (Qt cannot be downloaded: DECISIONS 2026-10-30). Everything here is **unverified** until rebuilt and tested on the Mac.
- **Last finished (cloud, unverified):** printer marks for system lines (EL04 tightened); `test_modal` test hook (S18); rest of the notes group (add_note_to_chord, remove_note, set_duration, exchange_voices, with unit tests and scenarios N03/N04); expression-lines and expression-marks moved onto address/address.h. Text and pitch groups being written by two command-implementers on branches `agent/text-impl` and `agent/pitch-impl`.
- **Test status:** last Mac run was at 8e7a2425d2 (agent/4.7): scripts/test.sh 137/137, all suites PASS, scenarios 29 ok / 6 SKIP. Nothing on agent/4.7-nfhpdz has been compiled.
- **Progress (estimate):** Phase 2 about 80% written, 65% verified; whole plan about 45% verified.
- **Next, in order:**
  1. **On the Mac:** `git fetch && git checkout agent/4.7-nfhpdz`; scripts/build.sh; scripts/test.sh; all tests/agent/*.py; scenarios/run.py (N03, N04 and any new text/pitch scenarios need `--update-goldens` once, then review the goldens). Fix compile errors first (none of this was compiled). Then merge into agent/4.7 per protocol.
  2. Merge text and pitch (after their reviews), adding their register lines to allcommands.cpp.
  3. P2.5: S01-S20 all passing, 8-hour soak (tests/agent/harness/soak.py --minutes 480), G2 report, then **STOP for Liam's G2 sign-off**.
  4. Follow-up: can closing a score after a big range delete crash the real app (DECISIONS 2026-10-29, upstream bug)?
- **Cloud session: what works there and what does not.**
  - The build and every test need this Mac: scripts/env.sh points at ~/Qt/6.10.2/macos and ~/musescore_deps_macos; the integration tests and scenarios launch the macOS app bundle (agent.install/mscore.app) with `open`; signing uses the "MuseScore Agent Local" keychain identity. A Linux cloud box has none of these, so it cannot build, run the unit tests, or run scenarios unless it sets up a Linux build of agent_tests itself (Qt 6.10.2 + MuseScore Linux deps; not tried, and the gtest tree alone is a large build).
  - Corpus fixtures (tests/fixtures/corpus/, 20 files) are git-ignored and not in the repo; tools/fixtures/make_corpus_fixtures.py regenerates them (needs network and a working mscore build). Synthetic fixtures are committed.
  - Rule 7 still holds: a gate or merge needs test results. Work done in the cloud goes on a branch (not agent/4.7) and is verified on the Mac before it is merged: rebuild, scripts/test.sh, all tests/agent/*.py, scenarios.
  - Good cloud work without a build: spec-driven code for steps 1-5 with unit tests written alongside, reviews, docs. Mark every such commit "unverified" in its message until the Mac run passes.
  - Never push to upstream musescore/MuseScore; never force-push; the official app and Liam's real scores are off limits (CLAUDE.md).
- **Blockers / waiting on Liam:**
  1. Update the daily app 4.7.4 → 4.7.6.
- **Background jobs:** cloud session: command-implementers for text and pitch; reviewer on the notes commits.
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
