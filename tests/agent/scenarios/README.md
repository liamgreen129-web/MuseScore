# Scenario tests (P2.2)

Each scenario is one JSON file here: a fixture, a list of tool calls with expected outcomes,
golden exports, and the standard invariants. `run.py` runs them against the built app, one
background instance at a time, each on a fresh temp copy of its fixture.

```bash
python3 -I tests/agent/scenarios/run.py                 # all scenarios
python3 -I tests/agent/scenarios/run.py S01 R02 slur    # by id or file-name substring
python3 -I tests/agent/scenarios/run.py --list
python3 -I tests/agent/scenarios/run.py S05 --update-goldens   # write goldens, then review the diff
python3 -I tests/agent/scenarios/run.py --undo-tool undo --redo-tool redo --bundle path/to/mscore.app
```

Output is one `ok` / `FAIL` / `SKIP` line per check, prefixed with the scenario id, then a
summary. Exit 1 on any FAIL, 2 on a setup error. Standard library only; runs on the system
Python 3.9 and on 3.12.

A scenario is **SKIP** (not FAIL) when a tool it calls is not in the build's `tools/list`, or
it needs a test hook the build does not have; the line says which. The tool list comes from
one probe instance started with no score. Commands inside a `batch` step's `operations`
count as required tools.

## Why JSON and not Python

Scenarios are data: the conductor fixes argument shapes as the command schemas settle
without reading driver code, reviewers see a scenario's intent in one screen, and the same
files can feed the soak or a future ctest wrapper. The few things a static list cannot say
are covered by three small features rather than code: captures (`"capture"` then `${var}` in
later arguments), assertions on the export (`"assert"` steps) and test hooks (`"needsHooks"`).
If a future scenario needs real logic, add a step kind to `harness/scenario.py`, not Python
in the scenario.

## File format

```jsonc
{
  "id": "S01",                       // printed on every line; file name is <id>_<name>.json
  "title": "Add one dynamic to a note",
  "source": "assessment.md Phase 2, S1",
  "fixture": "corpus/bach-bwv66-6.mscz",   // synthetic/… or corpus/…; never tests/fixtures/real
  "specs": ["specs/expression-marks.md add_dynamic"],
  "manual": "…",                     // optional: what part is manual or needs a future hook
  "needsHooks": ["_test_modal"],     // optional: SKIP until these tools exist
  "requires": ["undo"],              // optional: extra tools that must exist
  "steps": [ … ],
  "goldens": [{"format": "musicxml", "path": "goldens/S01_single_dynamic.musicxml"}],
  "invariants": {"validate": true, "undoRedo": true, "format": "mscz",
                 "errorsLeaveScoreUnchanged": true},
  "questions": ["…"]                 // open points for the conductor; ignored by the driver
}
```

### Steps

A tool step:

| Key | Meaning |
|---|---|
| `tool` | Tool name (short name or `command://…` URI). `$undo` / `$redo` mean the configured undo/redo tools. |
| `args` | Arguments. Strings `"${v}"` are replaced by a captured value (type kept); `"bar ${v}"` interpolates. |
| `isError` | Expected `isError` (default false, or true when `error` is given). |
| `error` | Expected error code prefix of the text, e.g. `"E_RANGE"` (`"E_"` when the code is not settled). |
| `expect` | `{ "dotted.path": matcher }` on `structuredContent` (`parts.0.name`, `score.bars`). |
| `text` | A matcher on the first text content. |
| `capture` | `{ "v": "stateVersion" }` (a structuredContent path) or `{ "a": {"text": "read bars (\\d+)-", "int": true} }` (regex group 1 on the text). |
| `writes` | true: the step must make an undo entry (FAIL if the undo index did not move). |
| `note` | Printed with the step. |

An assert step: `{"assert": "unchanged"}` (the invariant-format export equals the start),
`"changed"`, `{"assert": "undoIndex", "delta": 0}`, `{"assert": "exportMember", "format":
"mscz", "member": "<regex on zip member names>", "text": matcher}` (every matching member,
e.g. one part's `.mscx`), `{"assert": "exportText", "format": "musicxml", "text": matcher}`.

A matcher is a literal (equality) or an object of operators that must all hold: `eq`, `ne`,
`contains`, `notContains`, `startswith`, `regex`, `len` (number or matcher), `gte`, `lte`,
`gt`, `lt`, `exists`, `type`, `in`, `all` (a list of matchers).

Write results are checked with a `score_read` step after the write rather than by a field of
the write's own result, because no spec names the output field that carries the touched
bars' notation yet. Once it is fixed, `expect` on that field is shorter.

### What the driver checks besides the steps

- **Every step that returns an error** left the undo index where it was and the export
  byte-for-byte unchanged (`errorsLeaveScoreUnchanged`).
- **Undo index tracking.** After each step the driver reads `hello`'s `score.undo.index`; when
  it moves, it records the export at that index. An explicit `$undo`/`$redo` step is checked
  against the export recorded for the index it lands on.
- **Goldens.** Each listed format is exported at the end and compared with the golden after
  normalisation (below). A missing golden is a FAIL that tells you to run `--update-goldens`.
  Goldens are written normalised, so they never change because of a date or a rebuild.
- **`validate`:** `score_validate` at the end reports `ok`, or no problem that the fixture did
  not already have when it was opened.
- **`undoRedo`:** undo one entry at a time back to the starting index, comparing each state's
  export with the one recorded when it was made; then redo to the end and compare with the
  edited export. Byte-for-byte on the invariant format (`mscz` by default: every member, so
  parts and style count). If the build has no undo tool this line is `SKIP: no undo tool`. A
  scenario with `undoRedo: true` whose steps made no undo entry FAILs; read-only scenarios set
  it false.
- The instance is alive after every step and at the end; a crash lists any macOS crash
  reports written since the instance started.

### Normalisation (`harness/exports.py`)

| Format | Compared as |
|---|---|
| musicxml | text, `<encoding-date>` removed |
| mxl | every zip member, XML normalised as above |
| midi | bytes |
| mscz | every member except thumbnails, `audiosettings.json`, `viewsettings.json`; the main `.mscx` compared as `score.mscx`; `<programRevision>` (the build's git hash) blanked in every `.mscx` |

## The scenarios

| Id | What | Fixture | State |
|---|---|---|---|
| R01 | overview, read, changes, render; musicxml + midi goldens | corpus/bach-bwv66-6 | runs today |
| R02 | validate clean, reads incl. a nested tuplet; mscz golden | synthetic/stress-combined | runs today |
| R03 | whole-score read refused with a range that fits; bad args refused, score untouched | corpus/beethoven-op74 | runs today |
| S01–S20 | assessment.md Phase 2 list | see each file | SKIP until the tools exist |

S17 (concurrent user edit) uses a second agent write as the stand-in for the user. S18 (modal
dialog) and S19 (playback) need test hooks (`_test_modal`, `_test_playback`) that do not
exist; their `manual` field says how to run them by hand meanwhile. S20 checks the linked
parts inside the mscz export, since there is no command to make a part the current view.

### Address conventions assumed (until the core fixes them)

The spec sheets do not agree on one address shape yet. The scenarios follow each command's
own spec: `staff: {part, index}` (1-based) and `at: {measure, beat}` for expression marks and
text (from text.md 0.6); `{measure, beat, staff, voice}` event addresses for `add_slur` and
`transpose`; `staff: {part, staffIndex}` (0-based) for `write_voice` (notes.md). `measure` is
the positional bar (`m` number in NOTATION.md, pickup = 1); `beat` is 1-based in quarter
beats. `write_voice` content separates bars with ` | `. Each file's `questions` lists what it
had to assume.

## Soak (`../harness/soak.py`)

```bash
python3 -I tests/agent/harness/soak.py --minutes 2 --seed 1 [--log run.jsonl]
python3 -I tests/agent/harness/soak.py --replay run.jsonl
```

Generates random calls that are valid against each tool's `inputSchema` from `tools/list`
(required, types, enum/const, bounds, item counts, lengths, patterns, oneOf/anyOf, `$ref`),
with bar, staff, voice and part-name values drawn from the open score so most calls land on
real music. A `batch` gets random operations of the other write commands (and is left out while
there are none). Never generated: `save`, `score_open`, `backup_snapshot`, `cmd`, `undo`,
`redo` (add more with `--exclude`). After every call: the instance is alive, and
`score_validate` shows no new problem. Every `--check-every` calls and before each instance is
stopped: undo to the start restores the original export and redo restores the edited one (with
no writes made: the export is still the original). Instances rotate every
`--calls-per-instance` calls over random synthetic and corpus fixtures.

Every event (launch, call with arguments and outcome, validate, undo check, crash with crash
report paths, stop) is written to the JSONL log as it happens, flushed per line. `--replay`
re-runs the log's launches, calls and undo checks in order and reports any call whose
`isError` and error code differ from the log, writing `<log>.replay.jsonl`. Default log
location: `$TMPDIR/musescore-agent-soak/`.

## Clean-up guarantees

The harness never opens a fixture in place (temp copy, removed on stop), stops every instance
it started (SIGTERM, then SIGKILL after 20 s), and deletes the export and render files its own
calls produced in the agent's exports folder. It only ever finds its own process (new pid after
`open -n`, confirmed by `hello.app.pid` on the port it found).
