# Gate G2 report

Gate G2 (assessment.md, Phase 2, as changed by DECISIONS 2026-10-30 "G2 has no soak"):
*Wave A merged per protocol; S01-S20 and every group scenario pass with undo/redo round
trips; all unit tests and suites green.*

Run by the conductor on 2026-10-30 on Liam's Mac mini (Apple M6, 12 cores, 16 GB, macOS
27.0.1), build of `agent/4.7` at 70cdf29dd2, signed with a new local "MuseScore Agent Local"
certificate. Corpus fixtures regenerated here with music21 10.5.2 (19/19 round-trip with
identical note counts).

## 1. Wave A merged: done

`agent/4.7-nfhpdz` (the cloud session's Wave A: notes, expression-marks, expression-lines,
text, pitch) was built, fixed and verified on the mini, then fast-forwarded into `agent/4.7`
(36 commits, no force-push). COMMANDS.md marks the five groups merged.

It compiled with no errors. What the first real run found, all fixed (DECISIONS 2026-10-30):

| Where | What |
|---|---|
| score_read | a bar with a measure repeat read past the MeasureRepeat object (crash risk); now a whole-bar rest plus `% detail: … is a measure repeat` |
| transpose | by degrees past G9 / below C-1 was not refused (MuseScore folds the step back an octave) |
| add_lyric | an extender with no chord left is removed inside the edit (it used to depend on a later layout) |
| test runs | the background test instance could still come to the front; it now uses the "prohibited" activation policy |
| unit tests | six tests had wrong premises (never run in the cloud); corrected |
| harness | goldens compared the export's creation date; now ignored |

## 2. Scenarios: PASS

`tests/agent/scenarios/run.py`: **51 of 51 ok**, 0 FAIL, 0 incomplete, 0 SKIP (178 s),
including S01-S20 and every group scenario (N, EM, EL, TX, PT, R), each with the undo/redo
invariants. Goldens added for N03, N04, S07-S10, S14, S18, TX01-TX07 and PT01-PT07 after
review.

Three scenarios (PT06, TX05, S14) failed on upstream undo behaviour. With Liam's go-ahead
(DECISIONS 2026-10-30), the smallest fixes were made in upstream files, each tagged
`// AGENT:` and covered by a unit test; they are candidates for upstream PRs (rule 8):

| Scenario | Upstream fault | Fix |
|---|---|---|
| PT06 | undo of a first key signature on a staff with only a generated one left an explicit C key signature | `ChangeKeySig::flip` asks the key list; `KeySig::setProperty` no longer clears GENERATED when GENERATED itself is set |
| TX05 | layout dropped a one-chord melisma without undo whenever a neighbour was undone | `Lyrics::removeInvalidSegments` keeps it unless text entry asks (typing the next syllable behaves as before) |
| S14 | undo of a range delete (UI or write_voice) lost the beam's own properties | `RemoveElement` puts the chord back into its non-generated beam on undo |

## 3. Unit tests and suites: PASS

| Suite | Result |
|---|---|
| `scripts/test.sh` (agent_tests) | 253/253 |
| `tests/agent/test_server.py`, `test_hello.py`, `test_read.py`, `test_core.py`, `test_export.py` | all PASS |
| upstream `engraving_tests` | 722 pass; 10 MidiRenderer bend/vibrato tests fail with or without the agent patches (checked) |

## 4. Not covered here

- No soak (dropped from G2 by Liam). The soak harness remains for G3.
- G0's manual checks were not repeated on the mini: the reverse-quit check, and Muse Sounds
  playback (Muse Sounds is not installed on the mini).
- The official app on the mini is 4.7.5, not 4.7.6 (STATE blocker 1).
- The music21 pin (10.5.2) is inferred, not read from the cloud environment.

**STOP: G2 sign-off by Liam.**
