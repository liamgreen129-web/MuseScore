# State

Updated by the conductor at the end of every session and before every STOP.

- **Machine:** MacBook Pro (M1 Pro, 16 GB) for now; Mac mini later (DECISIONS 2026-10-29)
- **Phase:** 2
- **Current task:** P2.3 write_voice (conductor), design done, no code yet (see notes below). P2.1 done: tag `core-v1` (e57ee76e91) pushed. P2.2 delivered on branch `worktree-agent-aea2c55d063305eea` @ 77d7b23bb4, in review.
- **Last finished (2026-10-29):** P1.1 server, P1.2 registry/hello/discovery, P1.3 NOTATION.md (approved), P1.4 parser/writer/validator/score printer, P1.5 synthetic fixtures (21, merged by path), P1.6 corpus fixtures (18, regenerate with tools/fixtures/make_corpus_fixtures.py), P1.7 read commands (score_overview, score_read, score_validate, score_changes, score_export, score_render), P1.8 G1 run (signed off by Liam 2026-10-29), P2.1 core (see CORE.md)
- **Test status at last commit:** scripts/test.sh 50/50 (22 core/handles + 28 notation/printer); tests/agent/test_server.py 29/29, test_hello.py 32/32, test_read.py 39/39, test_core.py 53/53, test_export.py 160/160 (full run at G1; spot-checked after core)
- **Next:**
  1. P2.2 merge: a `reviewer` subagent was reviewing the branch when the session paused (its result may be lost; if so, spawn a new reviewer on `git diff a049c90814 77d7b23bb4`). Then rebase onto agent/4.7, run all tests + `python3 -I tests/agent/scenarios/run.py` here, merge, update COMMANDS/STATE. Implementer's notes: goldens blank `<programRevision>`; S01–S20 SKIP until tools exist, then `--update-goldens` and review; S17's note that writes lack expectStateVersion is now obsolete (core adds it); batch validation errors say "operation N:", runtime errors "step N (cmd):"; parts-linked.mscz opens with needSave=true; system python3 is 3.9.
  2. P2.3 write_voice (design below), then the rest of the notes group.
  3. P2.4 Wave A (max 2 implementers), after P2.2 is merged; point them at CORE.md.
- **Blockers / waiting on Liam:**
  1. Update the daily app 4.7.4 → 4.7.6.
- **Background jobs:** reviewer on the P2.2 branch (may not survive the pause)
- **Worktrees in use:** .claude/worktrees/agent-aea2c55d063305eea (P2.2, branch worktree-agent-aea2c55d063305eea); remove after merge
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
