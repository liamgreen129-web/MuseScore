# Gate G1 report

Gate G1 (assessment.md, Phase 1): *for every fixture, score_read of the whole piece
re-entered through the notation parser reproduces the original pitch and rhythm content
exactly; score_export matches a save from the UI; a 30-page score reads in under 2 s.*

Run by the conductor on 2026-10-29, MacBook Pro (M1 Pro), build of `agent/4.7` signed
with the local "MuseScore Agent Local" certificate. Fixtures: 19 public-domain corpus
scores (`tools/fixtures/make_corpus_fixtures.py`, git-ignored output) and 21 synthetic
stress scores (`tests/fixtures/synthetic/`), 40 in all.

## 1. Round trip: PASS

`scripts/test.sh`, test `Agent_ScorePrinter.EveryFixtureRoundTripsPitchAndRhythm`.

For each fixture the whole score is printed by the same printer `score_read` uses, the
text is parsed back by the notation parser, and every staff and voice is compared with an
independent walk of the engraving model: the onset, duration and sounding pitch of every
note and rest, ties included. Tuplets, grace notes, chords, 4 voices, cross-staff notes,
transposing instruments, pickups, irregular bars and repeats are all covered by the
synthetic set. Each fixture also has to read in under 2 s.

| Result | |
|---|---|
| Fixtures | 40 of 40 identical |
| Whole run (print + parse + compare, all fixtures) | 2261 ms |
| Unit tests | 28/28 PASS |

## 2. score_export matches a save from the UI: PASS

`tests/agent/test_export.py` (new). For every fixture and format:

- **UI side:** the same bundle's converter, `mscore -o`. For `.mscz` it calls
  `NotationProject::save`, the function File > Save runs. For the other formats it calls the
  registered writer, which is the one File > Export calls.
- **Agent side:** `score_export` in a running instance with the fixture open.
- **Comparison, after removing what differs between any two saves:**
  - MusicXML: text, with `<encoding-date>` removed.
  - MXL: every zip member.
  - MIDI: byte for byte.
  - MSCZ: every zip member, comparing the main `.mscx` under one name. `Thumbnails/`,
    `audiosettings.json` and `viewsettings.json` are left out: File > Save writes these
    window and mixer states, and an export does not.

| Format | Result |
|---|---|
| MusicXML | 40 / 40 identical |
| MXL | 40 / 40 identical |
| MIDI | 40 / 40 byte-identical |
| MSCZ | 40 / 40 (`score.mscx` byte-identical) |

PDF is not compared byte for byte. It carries creation dates and font-subset ids, and
it uses the same registered writer as File > Export.

## 3. A 30-page score reads in under 2 s: PASS

Fixture `beethoven-op74` (the whole String Quartet op. 74, 4 parts) was added for this
check. The earlier "done with the quartet movements" in STATE.md was wrong, because the
longest of those movements is 23 pages.

| Score | Pages | Bars | Whole-score print | Read start to end over MCP |
|---|---|---|---|---|
| Beethoven op. 74 | 72 | 1114 | 95 ms | 47 ms in 7 calls with the old hint; 0.22 s in 9 calls in `test_read.py` (which also asks for the whole rest each time, and so pays for each refusal's full print) |
| Beethoven op. 18/1 i | 23 | 313 | 20 ms | 9 ms, 2 calls |
| Mozart K. 458 i | 19 | 283 | 108 ms | 27 ms, 2 calls |
| Haydn op. 74/1 i | 14 | 155 | 130 ms | 8 ms, 1 call |

The timings measure from the test client to the server and back over HTTP. "Whole-score print" is one
`score_read` with no range. For scores over the 40,000-character cap (about 10k tokens)
that call is refused, but the server still prints the whole score to measure it. "Read
start to end" follows the refusals' hints until the last bar.

**Fixed during the run:** the refusal said "read about N bars at a time". N was estimated
from the average bar size, so on uneven music the next read was refused again (twice for
op. 74). The refusal now gives the exact range that fits, worked out from the text just
printed: "read bars 1-240 next". `test_read.py` checks that the suggested range is
accepted, and that op. 74 reads to the end in under 2 s.

## Test suites at the end of the run

| Suite | Result |
|---|---|
| `scripts/test.sh` (gtest) | 28/28 PASS |
| `tests/agent/test_server.py` | 29/29 PASS |
| `tests/agent/test_hello.py` | 32/32 PASS |
| `tests/agent/test_read.py` | 39/39 PASS |
| `tests/agent/test_export.py` | 160/160 PASS |
| `scripts/check-identity.sh` | PASS |

## Open points carried to Phase 2 (none block G1)

From DECISIONS, P1.5:

1. A lyric extender's exact end. `ly"…_"` means "until the next syllable", but MuseScore
   stores a duration.
2. `len=` prints reduced fractions.
3. MusicXML can't carry nested tuplets, so the music21 reference printer is no oracle for them.
4. MuseScore's headless converter sometimes dies under load, and a rerun fixes it.

## Sign-off

- [x] Liam: G1 accepted (2026-10-29, in chat), go to Phase 2 (P2.1 transaction core, then the `core-v1` freeze).
