# State

Updated by the conductor at the end of every session and before every STOP.

- **Machine:** MacBook Pro (M1 Pro, 16 GB) for now; Mac mini later (DECISIONS 2026-10-29)
- **Phase:** 2
- **Current task:** P2.4 Wave A merges. Both groups were reviewed ("changes required") and sent back to their implementers with the fixes listed in DECISIONS 2026-10-29: expression-lines on branch worktree-agent-ab69e3b5b93ceb103, expression-marks on worktree-agent-a326fe74a251e643f. Both were told to rebase onto agent/4.7.
- **Last finished (2026-10-29):** P2.3 write_voice (10ede70fe5); read printer end-mark fix (ddc7dc16fe); P2.2 harness; P2.1 core (core-v1); P1 (G1 signed off).
- **Test status at last commit:** scripts/test.sh 61/61 (write_voice round trip: 66 synthetic + 102 corpus staves); test_server, test_hello, test_read, test_core PASS; test_export spot-check (bwv66, stress-combined, op74) 0 failures; scenarios 8 ok / 15 SKIP / 0 FAIL; check-identity PASS.
- **Next:**
  1. When each implementer reports, rerun everything here on its branch, then merge per protocol: drop or keep the allcommands commit; reconcile address shapes (staff as int | {part,index} in all groups); update COMMANDS.md.
  2. Then start the `text` and `pitch` implementers (Wave A, at most 2 at once). Tell them: engraving-only library per group (like agent_notes), default behaviour never depends on app preferences, error codes from errors.h only.
  3. The rest of the notes group (add_note_to_chord, remove_note, set_duration, tie, tuplet, grace, clear_range, exchange_voices). Conductor.
  4. Before G2: add a `test_modal` hook so S18 can run; record goldens for S01–S20 as their commands land.
  5. Follow-up: check whether the upstream close-time crash (DECISIONS 2026-10-29) can happen in the app when a score is closed after a big delete.
- **Blockers / waiting on Liam:**
  1. Update the daily app 4.7.4 → 4.7.6.
- **Background jobs:** the expression-lines and expression-marks implementers (review fixes).
- **Worktrees in use:** .claude/worktrees/agent-ab69e3b5b93ceb103 (lines) and .claude/worktrees/agent-a326fe74a251e643f (marks); remove each after its merge.
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
