# Spec sheet: `structure` group

Scout: `api-scout`, task P0.S-structure. Source: branch `agent/4.7` at v4.7.6.
Every `file:line` below refers to the working tree at that tag. Paths are relative to
the repository root.

Commands: insert_measures, remove_measures, split_measure, join_measures,
set_time_signature, set_key_signature, set_tempo, set_barline, set_repeat, add_break,
add_clef.

---

## 0. Group-wide findings (read before any command section)

These apply to every command below. Each command section refers back to them as G1–G9.

### G1. The action controller lives in `notationscene`, not `notation`

The UI action registrations are in `src/notationscene/internal/notationactioncontroller.cpp`
(and the action definitions in `src/notationscene/internal/notationuiactions.cpp`), not
`src/notation/internal/`. `INotationInteraction` is still in `src/notation/`.

### G2. Error dialogs: `checkAndShowError()` opens a modal-style info box and clears the error

`NotationInteraction::checkAndShowError()` (src/notation/internal/notationinteraction.cpp:8456-8462)
calls `MScoreErrorsController::checkAndShowMScoreError()`, which reads `MScore::_error`,
**resets it to `MS_NO_ERROR`** (src/notation/internal/mscoreerrorscontroller.cpp:39-44) and then
opens `interactive()->info(...)` (mscoreerrorscontroller.cpp:190-197) unless the user ticked
"don't show again" (mscoreerrorscontroller.cpp:46).

Interaction methods in this group that call it: `splitSelectedMeasure` (notationinteraction.cpp:5023),
`joinSelectedMeasures` (5038), `addBoxes(target)` on no selection (5056-5058, 5072-5075, 5131-5134),
`applyPaletteElement` (2343), `addTextToItem` (6205-6207).

Two consequences for the agent:
1. Calling any of these wrappers can open a dialog. Forbidden by CLAUDE.md rule 4.
2. Because the error is cleared before the agent's `commitChanges()`, `Score::endCmd`'s
   automatic rollback on error (src/engraving/editing/cmd.cpp:418-424) no longer fires. A
   partially applied edit (see split_measure, which sets an error *after* mutating) is then
   committed.

**Rule for this group:** call the engraving functions the wrappers call, inside the agent's
own transaction, and read `MScore::_error` yourself after each one. Do not call the
`INotationInteraction` wrappers listed above. The only interaction method that is safe to call
directly is `addBoxes(BoxType, int count, int beforeBoxIndex, bool)` (inotationinteraction.h:190;
notationinteraction.cpp:5148-5211), which has no error dialog, but it moves the selection and
pans the view (5203-5210); see insert_measures.

### G3. Transactions, lock, and nested commands

- `NotationUndoStack::prepareChanges/commitChanges/rollbackChanges` are no-ops while the stack
  is locked (src/notation/internal/notationundostack.cpp:112-114, 125-127, 138-140), and
  `Score::startCmd/endCmd` also return early when locked (cmd.cpp:346-348, 408-410). So the
  `startEdit()/apply()` pairs inside interaction wrappers collapse into the agent's macro.
  `NotationInteraction::apply()` still calls `notifyAboutNotationChanged()` after its no-op
  commit (notationinteraction.cpp:321-329); that is harmless.
- `UndoStack::pushAndPerform` appends to the active command regardless of the lock
  (src/engraving/editing/undo.cpp:309-331); with no active command it performs the change
  without recording it (undo.cpp:311-319).
- None of the engraving functions used in this group call `startCmd/endCmd` themselves.
- **Hazard: `activeCommand()->unwind()`.** `Score::cmdAddTimeSig` (src/engraving/editing/edit.cpp:1553,
  1569) and `Score::cmdRemoveTimeSig` (edit.cpp:1664) call `undoStack()->activeCommand()->unwind()`
  when the measure rewrite fails. `UndoCommand::unwind()` undoes and deletes **every child of the
  active macro** (undo.cpp:242-250). Inside an agent batch the active macro is the agent's whole
  transaction, so a failed time-signature change silently reverts all earlier operations in the
  batch and leaves the macro open and empty. For a global time signature the failure path sets
  no `MScore` error (edit.cpp:1137-1149 returns false without `setError`). The agent must detect
  it by post-condition and abort the batch immediately (no further operations may run after it).
- `endCmd` rolls back the whole macro if `MScore::_error` is set and not a warning
  (cmd.cpp:418-424). `startCmd` resets the error (cmd.cpp:359), but under a lock it does not run,
  so after each operation the agent must check `MScore::_error` and abort the batch if it is set.
  Several engraving functions set an error and return *before* mutating (fine) and one sets it
  *after* mutating (split_measure, G2 point 2).

### G4. Work on the master score

- The undo stack is shared: `Score::undoStack()` returns the master's (src/engraving/dom/score.cpp:6286).
- Several UI paths behave differently when run from a part (excerpt) score. Example: the global
  key-signature drop loops `score()->staves()` of the score it was dropped in
  (src/engraving/dom/measure.cpp:1597-1599), so from a part view it only reaches that part's
  staves and their linked master staves. `Score::cmdTimeDelete` redirects to the master when run
  on an excerpt (edit.cpp:5013-5032).
- Rule: resolve all structure commands against `IMasterNotation::masterScore()`
  (src/notation/imasternotation.h:45), whatever notation the user is viewing. Commands that are
  intrinsically per-score (line and page breaks, see add_break) take an explicit target score.

### G5. Measures in parts are not reliably linked

`MasterScore::insertMeasure` creates a separate, unlinked `Measure` in every score of
`scoreList()` (src/engraving/dom/masterscore.cpp:522-556), while measures that existed when a part
was created are linked by `Excerpt::cloneMeasure` (src/engraving/dom/excerpt.cpp:1092). The
engraving code therefore loops `scoreList()` explicitly whenever it changes measure-level
properties that must appear in parts (`REPEAT_START`, `REPEAT_END`, `TIMESIG_NOMINAL`: measure.cpp:1724-1727,
1738-1741; splitjoinmeasure.cpp:151-161, 289-300). `scoreList()` returns the master first, then
each excerpt (score.cpp:4363-4374). Any agent code that sets a measure property must do the same
loop, or call an engraving function that does.

### G6. Layout-dependent code inside a transaction

Layout runs only at `endCmd` (cmd.cpp:426). Within one transaction, code that depends on layout
sees stale or missing layout data for measures created earlier in the same transaction:
- `Measure::acceptDrop` returns false when `!system()` (measure.cpp:1385-1387). The whole palette
  drop path (`applyDropPaletteElement`, notationinteraction.cpp:2793) therefore silently does
  nothing on a measure that has not been laid out yet.
- `Score::undoChangeBarLineType` branches on `m->system()` and `isFirstInSystem()`
  (edit.cpp:5752, 5792).
- `Score::selectRange` for a measure uses `coveringMMRestOrThis()` and `lastMeasureMM()`
  (score.cpp:3549-3551), which reflect the multimeasure rests built by the last layout.

This group therefore avoids `acceptDrop` entirely and calls the engraving function each
`Measure::drop` case calls. Whether the core should lay out between structural operations in a
batch is an open question for the conductor (OQ-1).

### G7. Multimeasure rests

When `Sid::createMultiMeasureRests` is on, the UI selects mmrest measures. The engraving
functions used here convert to the underlying measures themselves (masterscore.cpp:492-496;
edit.cpp:1442-1444, 3373-3378, 5076-5078, 5781; splitjoinmeasure.cpp:210-215;
measurebase.cpp:550). The agent should resolve addresses with `Score::tick2measure` /
`Score::crMeasure` (underlying measures, score.cpp:1805-1817), never `tick2measureMM`, and pass
underlying measures in.

### G8. Addressing measures

- `Score::measure(int idx)` counts every `MeasureBase` including frames (score.cpp:1787-1797);
  `MeasureBase::index()` is the same index (measurebase.cpp:637-653). The UI's `addBoxes` index
  is this index (notationinteraction.cpp:5181).
- `Score::crMeasure(int idx)` counts only `Measure`s (score.cpp:1805-1817).
- The displayed number is `MeasureBase::no()` (src/engraving/dom/measurebase.h:144), which is
  affected by irregular measures, number offsets and section breaks.
The spec below writes `measure` as a 1-based index over `Measure`s (i.e. `crMeasure(measure-1)`).
Whether the public schema uses that or the displayed number is the conductor's decision (OQ-2).

### G9. No confirmation dialogs in these paths

There are no `interactive()->question` calls in the edit paths of this group. The only dialogs are:
- `insert-measures` / `append-measures` with no argument open `musescore://notation/selectmeasurescount`
  (src/notationscene/internal/notationactioncontroller.cpp:1708-1712);
- `measures-per-system` opens the breaks dialog (notationactioncontroller.cpp:316, 1821-1824);
- `measure-properties` opens the measure-properties dialog (notationactioncontroller.cpp:321, 2123-2127);
- the error info box of G2;
- `addText` enters text-edit mode (notationinteraction.cpp:6240-6246) rather than a dialog.
All are avoided by the direct engraving calls specified below.

### G10. Plugin API is not a model for this group

`apiv1::Cursor::add` (src/engraving/api/v1/cursor.cpp:270-428) diverges from the UI for every
structure element:
- KEYSIG: plain `undoAddElement` (cursor.cpp:298-302), skipping `undoChangeKeySig`
  (no transposition, no instrument-change handling, no linked-staff link records).
- TIMESIG: passes the cursor **track** as the `staffIdx` argument of `cmdAddTimeSig`
  (cursor.cpp:307). In the "same signature, same stretch" branch `cmdAddTimeSig` calls
  `staff(staffIdx)->nextTimeSig(...)` (edit.cpp:1504), which dereferences null when the track
  number is not a valid staff index.
- LAYOUT_BREAK: `MeasureBase::addInternal` (cursor.cpp:315-318), a direct non-undoable mutation.
- CLEF: plain `undoAddElement` (cursor.cpp:386-408), skipping `undoChangeClef`
  (no transposing-clef handling, no replacement of an existing clef).
- BAR_LINE: falls to `default: undoAddElement` into the current ChordRest segment
  (cursor.cpp:424-426); this matches the "adding a bar line through Cursor.add crashed MuseScore"
  report in assessment.md.
- `Score.appendMeasures` (src/engraving/api/v1/score.h:465) calls `Score::appendMeasures`
  (score.cpp:4399-4407), which uses the default `needDeselectAll = true`
  (score.h:938-946); otherwise the same as the UI's append.

---

### insert_measures  ·  command://notation/insert_measures

**UI entry point.**
- Actions `insert-measure`, `insert-measures`, `insert-measures-after-selection`,
  `insert-measures-at-start-of-score`, `append-measure`, `append-measures`
  (notationactioncontroller.cpp:284-301; definitions notationuiactions.cpp:697, 721).
- Count-taking variants go through `NotationActionController::addMeasures`
  (notationactioncontroller.cpp:1700-1714), which opens the count dialog when no argument is
  passed (1708), then `addBoxes(BoxType::Measure, count, target)` (1716-1725).
- `NotationInteraction::addBoxes(BoxType, int, AddBoxesTarget)` (notationinteraction.cpp:5041-5146)
  turns the selection into `beforeBoxIndex` and `moveSignaturesClef`, then calls
  `addBoxes(BoxType, int, int beforeBoxIndex, bool moveSignaturesClef)` (5148-5211), which does
  `startEdit`, then `count` times `score()->insertMeasure(ElementType::MEASURE, beforeBox, options)`
  (5196) with `createEmptyMeasures=false`, `moveSignaturesClef` as computed,
  `needDeselectAll=false`, `cloneBoxToAllParts=true` (5189-5193), then `apply()`, then selects the
  new range and calls `showItem` (5200-5210).
- `Score::insertMeasure` (edit.cpp:4715-4731) forwards measures to
  `MasterScore::insertMeasure` (masterscore.cpp:487-797).
- `moveSignaturesClef` is `true` for before-selection, start-of-score and end-of-score, `false`
  for after-selection (notationinteraction.cpp:5044), with a special case when the selected
  element is a clef/barline/time sig/key sig sitting on a measure boundary (5104-5124).

**Selection set-up.** None. Call `masterScore->insertMeasure(ElementType::MEASURE, beforeMB, options)`
`count` times with the UI's options (notationinteraction.cpp:5189-5193), `beforeMB` being the
underlying `Measure` the new measures go before, or `nullptr` to append. This is the call the UI
makes; it avoids the selection change and view pan of the wrapper. Alternative that stays on the
interaction layer: `INotationInteraction::addBoxes(BoxType::Measure, count, beforeBoxIndex, moveSignaturesClef)`
(inotationinteraction.h:190) with `beforeBoxIndex = beforeMB->index()` (MeasureBase index, G8),
accepting that it replaces the selection (restored by the selection guard) and pans the view.

**Arguments.**
```json
{
  "type": "object",
  "required": ["count"],
  "properties": {
    "count":   {"type": "integer", "minimum": 1, "maximum": 999},
    "before":  {"type": "integer", "minimum": 1, "description": "insert before this measure; omit to append at end"},
    "after":   {"type": "integer", "minimum": 1, "description": "insert after this measure (UI 'insert after selection': keeps signatures/clefs on the old measure, moves its end barline and end-repeat to the last new measure)"}
  },
  "oneOf": [{"required": ["before"]}, {"required": ["after"]}, {"not": {"anyOf": [{"required": ["before"]}, {"required": ["after"]}]}}]
}
```
Mapping: `before:N` → `beforeMB = crMeasure(N-1)`, `moveSignaturesClef=true`.
`after:N` → `beforeMB = crMeasure(N-1)->next()` (may be a frame or null), `moveSignaturesClef=false`.
Neither → `beforeMB=nullptr`, `moveSignaturesClef=true` (append; notationinteraction.cpp:5140-5142).
The upper bound 999 is a proposal, not a UI limit (the count dialog's range was not checked).

**Validation before mutation.**
- `count < 1` → `E_INVALID_ARG` (the UI returns silently: notationinteraction.cpp:5150-5152).
- Measure index out of range → `E_RANGE`.
- Target measure is inside a measure-repeat group continuing from the previous measure on any
  staff: `toMeasure(beforeMB)->isMeasureRepeatGroupWithPrevM(staffIdx)` for any staff →
  `E_MEASURE_REPEAT` (engraving sets `CANNOT_SPLIT_MEASURE_REPEAT` and returns null:
  masterscore.cpp:497-502).
- Score has no master score / not open → `E_NO_SCORE` (core).

**Post-condition.**
- `masterScore->nmeasures()` (or a `crMeasure` count) increased by exactly `count`.
- The first new measure starts at the old tick of `beforeMB` (or the old end tick when appending),
  and each has `ticks() == sigmap()->timesig(tick).nominal()` (masterscore.cpp:513-517, 555-556).
- Each new measure holds one full-measure rest per staff (masterscore.cpp:779-791).
- For every excerpt score, `tick2measure(firstNewTick)` is a new measure with the same length
  (masterscore.cpp:522).
- `MScore::_error == MS_NO_ERROR`.

**Nested commands.** None. `MasterScore::insertMeasure` uses `undo(new InsertMeasures(...))`
(masterscore.cpp:558), `doUndoRemoveElement/doUndoAddElement`, `undoInsertTime` (777) and
`undoAddCR` (789). Safe under lock.

**Linked parts / excerpts.** Propagates: the function loops `scoreList()` and creates a measure
in every score (masterscore.cpp:522-775); rests are added through `undoAddCR`, which adds to
linked staves (comment at masterscore.cpp:782). Caveat: the moved `REPEAT_START`/`REPEAT_END`
flags are set on `masterMeasure` only (masterscore.cpp:691-694, 716-719); see OQ-5.

**Crash and corruption risks.**
- Pickups: inserting before measure 1 creates a full-length measure (nominal sig, masterscore.cpp:513-517)
  and moves the initial clef/key/time to it (`isBeginning`, masterscore.cpp:509, 608-689); the
  pickup becomes an irregular measure 2. Not a crash, but the agent should report it.
- Empty score: inserting into a score with no measures calls `restoreInitialKeySigAndTimeSig`
  (masterscore.cpp:696-700; call at 699), which writes C major and 4/4 regardless of the previous score's
  signatures (edit.cpp:4791-4836).
- Index confusion between `Score::measure()` (frames counted) and `crMeasure()` (frames not
  counted) (G8). Inserting "before measure N" when a frame precedes measure N: the UI passes the
  measure, and `MasterScore::insertMeasure` inserts directly before it (after the frame).
- Interaction wrapper error dialog when nothing is selected (G2).

**Existing upstream tests.**
- src/engraving/tests/measure_tests.cpp: `insertMeasureMiddle` (47), `insertMeasureBegin` (61),
  `insertMeasureEnd` (75), `insertAtBeginning` (88), `insertBfClefChange` (101),
  `insertBfKeyChange` (133), `spanner_a` (178), `spanner_b` (200). Fixtures in
  src/engraving/tests/measure_data/ (`measure-insert_*`, `measure-1..10`).
- src/engraving/tests/parts_tests.cpp: `appendMeasure` (194), `insertMeasure` (217) with parts
  created by `TestUtils::createParts`.
- These tests call `Score::insertMeasure(MeasureBase*)` with default options
  (`needDeselectAll=true`, `moveSignaturesClef=true`), not the UI options.

**Scenarios.**
- STR-INS-01: insert 4 measures before bar 5 of a 4/4 piece; bars 5–8 are rests, old bar 5 is now bar 9.
- STR-INS-02: insert before a bar that carries a 3/4 change, key change and clef change; all three move to the first new bar.
- STR-INS-03: insert after a bar that ends with a final barline and an end repeat; both move to the new last bar (master and part).
- STR-INS-04: append 2 measures to a score with linked parts; parts gain 2 bars each.
- STR-INS-05: insert before bar 1 of a score with a pickup; result reported, pickup now bar 2.
- STR-INS-06: insert inside a 2-bar measure-repeat group → `E_MEASURE_REPEAT`, score unchanged.
- STR-INS-07: insert with multimeasure rests on; underlying bars correct after toggling mmrests off.
- STR-INS-08: undo restores the export byte for byte; redo re-applies.

**Open questions.** OQ-1, OQ-2, OQ-5.

---

### remove_measures  ·  command://notation/remove_measures

**UI entry point.**
- Action `time-delete` (Ctrl+Del) → `NotationInteraction::removeSelectedRange`
  (notationactioncontroller.cpp:389; notationuiactions.cpp:460; notationinteraction.cpp:6449-6458).
- Inspector "Delete measures" button → `NotationInteraction::removeSelectedMeasures`
  (src/inspector/qml/MuseScore/Inspector/measures/measuressettingsmodel.cpp:91;
  notationinteraction.cpp:6403-6447), which widens the selection to whole measures (6435-6436)
  before calling the same function.
- Both call `Score::cmdTimeDelete()` (edit.cpp:4975-5066) → `doTimeDelete` (5068-5116) →
  `checkTimeDelete` (4902-4968) → `deleteMeasures(mbStart, mbEnd)` (3367-3527) →
  `undoRemoveMeasures` per score (7865-7953).

**Selection set-up.** `cmdTimeDelete` reads `selection()` (edit.cpp:4977, 4991, 5008-5009).
Construct a measure range selection on the master: `masterScore->deselectAll()`, then
`masterScore->select(m1, SelectType::RANGE, 0)` and
`masterScore->select(m2, SelectType::RANGE, nstaves()-1)`, which goes through
`Score::selectRange` for measures (score.cpp:3548-3567). The staff range does not matter: the
delete acts on all tracks (edit.cpp:4919-4952, 5129). Alternative without selection, same
engraving effect for whole measures: `if (checkTimeDelete(m1->first(ChordRest), endSeg)) deleteMeasures(m1, m2)`
— this skips the excerpt redirect (5013-5032), irrelevant when the agent always uses the master (G4).
Prefer the selection path for fidelity.

**Arguments.**
```json
{
  "type": "object",
  "required": ["from"],
  "properties": {
    "from": {"type": "integer", "minimum": 1},
    "to":   {"type": "integer", "minimum": 1, "description": "inclusive; defaults to from"}
  }
}
```
Whole measures only. Partial-measure deletion (`cmdTimeDelete` with a partial range,
`doTimeDeleteForMeasure` edit.cpp:5118-5200) changes measure lengths and is out of scope.

**Validation before mutation.**
- `from > to`, or either out of range → `E_RANGE`.
- Deleting every measure of the score → proposed `E_RANGE` (agent policy; the engraving path
  allows it: `deleteMeasures` leaves `mAfterSel` null and skips the restore, edit.cpp:3434-3436).
- Measure-repeat group crossing the range boundary (`checkTimeDelete`, edit.cpp:4917-4927) →
  `E_MEASURE_REPEAT`. Pre-check the same four conditions with `startsAtBeginningOfMeasure =
  endsAtEndOfMeasure = true`, i.e. `m1->isMeasureRepeatGroupWithPrevM(s)` or
  `m2->isMeasureRepeatGroupWithNextM(s)` for any staff.
- Tuplet crossing the range boundary (edit.cpp:4929-4961) → `E_TUPLET_SPLIT`. With whole
  measures a tuplet cannot cross a barline, so this does not fire; keep the post-check.
- Frames between `from` and `to` are removed too (`RemoveMeasures` removes the whole
  `MeasureBase` chain: src/engraving/editing/editmeasures.cpp:293). Proposed: refuse with
  `E_RANGE` unless `include_frames: true`, or report removed frames (OQ-6).

**Post-condition.**
- Measure count decreased by `to-from+1`; the measure now at index `from` starts at the old tick
  of `from` (or `from` is past the end).
- Time signature and key at the new `from` equal what they were at old `to+1` (or the last
  deleted ones if the deletion removed the change): `deleteMeasures` reinstates them
  (edit.cpp:3386-3409, 3432-3472 for time sigs; 3411-3416, 3474-3520 for key sigs).
- `MScore::_error == MS_NO_ERROR` (`checkTimeDelete` sets `CANNOT_REMOVE_TIME_MEASURE_REPEAT`
  or `CANNOT_REMOVE_TIME_TUPLET` and returns false: edit.cpp:4925, 4963).
- Each excerpt lost the same tick range.

**Nested commands.** None. `cmdTimeDelete` calls `deselectAll()` at the end when not in note
entry (edit.cpp:5065-5067) and adjusts viewer canvases (`adjustCanvasPosition`, 3428-3431).
Safe under lock. If the score is in note-entry mode, `cmdTimeDelete` can call `appendMeasures(1)`
(5045-5051); the core should leave note-entry mode before structure commands (OQ-7).

**Linked parts / excerpts.** Propagates: `deleteMeasures` loops `scoreList()`
(edit.cpp:3421-3522) and calls `undoRemoveMeasures` on each score (3425).
Inconsistency in source: inside that loop the excerpt's `staffIdx` is used to index the
master's `lastDeletedTimeSigs` (3444), `this->staff(staffIdx)` (3450) and
`lastDeletedKeySigEvents` (3501), which are keyed by master staff index. See OQ-8.

**Crash and corruption risks.**
- `undoRemoveMeasures` asserts on missing segments and continues (edit.cpp:7877-7883) — a
  corrupted score is handled without a crash.
- Ties and note-anchored spanners crossing the range boundary are removed (edit.cpp:7905-7932);
  slurs are reconnected or deleted by `reconnectSlurs` (3378-3382). The agent should report
  removed spanners (use `ScoreChanges`).
- Assessment's "ugly case": deleting bars that contain spanner endpoints. Covered by
  upstream tests `spanner_A..D` and `spanners09..12` below.

**Existing upstream tests.**
- measure_tests.cpp: `spanner_A` (221), `spanner_B` (244), `spanner_C` (267), `spanner_D` (290),
  `deleteLast` (309), `gap` (328), `undoDelInitialVBox_269919` (447).
- src/engraving/tests/spanners_tests.cpp: `spanners09` (376), `spanners10` (409),
  `spanners11` (442), `spanners12` (475), all through `cmdTimeDelete`.
- src/engraving/tests/barline_tests.cpp: `deleteSkipBarlines` (415).
- No upstream test removes measures in a score with parts.

**Scenarios.**
- STR-REM-01: remove bars 3–4 of 8; bars 5–8 become 3–6.
- STR-REM-02: remove the bar that holds a 3/4 change; following bars keep 3/4 (reinstated sig).
- STR-REM-03: remove bar 1 holding the initial key and time sig; new bar 1 shows them.
- STR-REM-04: remove a range containing a hairpin end, a slur start and a tie across the boundary.
- STR-REM-05: remove bars in a score with two linked parts; parts match.
- STR-REM-06: remove bars inside a measure-repeat group → `E_MEASURE_REPEAT`, unchanged.
- STR-REM-07: remove the last bar (endSegment null path).
- STR-REM-08: remove with multimeasure rests on.
- STR-REM-09: remove a range spanning a vertical frame → error or frame-removal report per OQ-6.

**Open questions.** OQ-1, OQ-6, OQ-7, OQ-8.

---

### split_measure  ·  command://notation/split_measure

**UI entry point.** Action `split-measure` (notationactioncontroller.cpp:281;
notationuiactions.cpp:678) → `NotationInteraction::splitSelectedMeasure`
(notationinteraction.cpp:5002-5024): takes the selected note's chord or the selected rest,
`startEdit`, `SplitJoinMeasure::splitMeasure(score()->masterScore(), chordRest->tick())` (5020),
`apply()`, `checkAndShowError()` (5023). Engraving: src/engraving/editing/splitjoinmeasure.cpp:46-183.
A second UI route is Ctrl+drop of a barline onto a chord/rest, which calls the same function
(src/engraving/dom/chordrest.cpp:217-221).

**Selection set-up.** None. Call `SplitJoinMeasure::splitMeasure(masterScore, tick)` directly
(splitjoinmeasure.h:34). Do not call `splitSelectedMeasure` (G2: dialog and error reset).

**Arguments.**
```json
{
  "type": "object",
  "required": ["measure", "beat"],
  "properties": {
    "measure": {"type": "integer", "minimum": 1},
    "beat":    {"description": "position inside the measure where the second half starts; {beat: 2.5} or {tick: 960} per the shared addressing schema"}
  }
}
```
The split tick is absolute: `crMeasure(measure-1)->tick() + rtick`.

**Validation before mutation.** All of these must be pre-checked; the function has no null guard.
- `masterScore->tick2segment(tick, false, SegmentType::ChordRest)` must be non-null.
  `splitMeasure` dereferences it unguarded (splitjoinmeasure.cpp:48-50); a tick that is not on a
  ChordRest segment of some track crashes. → `E_NOT_ON_BEAT` (proposed).
- `segment->rtick() != 0` (splitjoinmeasure.cpp:50-53) → `E_INVALID_ARG` ("cannot split at first beat").
- `!segment->splitsTuplet()` (54-57) → `E_TUPLET_SPLIT`.
- No staff has `measure->isMeasureRepeatGroup(staffIdx)` (61-66) → `E_MEASURE_REPEAT`.
- **Too-short check happens after mutation.** The two resulting lengths are reduced and adjusted
  (117-141) and `CANNOT_SPLIT_MEASURE_TOO_SHORT` is set if either denominator exceeds 128
  (142-145), *after* both new measures were inserted and the original removed (103-112). The agent
  must replicate lines 117-141 before calling and refuse with `E_RANGE` if a denominator would
  exceed 128; if it ever fires anyway, the error forces `endCmd` rollback (cmd.cpp:418-424) of the
  whole transaction.

**Post-condition.**
- Measure count +1; measure `measure` has `ticks() == tick - oldTick`, the next has the
  remainder; both keep the original nominal time signature (splitjoinmeasure.cpp:120-121).
- Total ticks of the score unchanged.
- Start/end repeat flags carried over (151-161).
- `MScore::_error == MS_NO_ERROR`.

**Nested commands.** None. Uses `masterScore->insertMeasure` twice (103-105), `undoRemoveMeasures`
per score (108-111), `undoInsertTime` (112), `ScoreRange::write` (163), `undoChangeProperty` for
spanners (165-172). Safe under lock.

**Linked parts / excerpts.** Propagates: insertion and removal loop `scoreList()` (via
`insertMeasure` and lines 108-111), barline flags loop `scoreList()` (151-161).
Not propagated / not undoable: the staff-type-change re-creation uses `masterScore->addElement(stc)`
(180), a direct, master-only, non-undo mutation. Only relevant when a staff type change starts
at the split measure.

**Crash and corruption risks.**
- Null segment dereference (above).
- Post-mutation error (above).
- `m1->setTick`/`m2->setTick` (114-115) and `setTimesig` (120-121) are direct mutations of the
  two measures just inserted in this command; undo removes those measures, so they are not leaked.
- Splitting creates two irregular measures; `adjustToLen` does not set `Pid::IRREGULAR`
  (measure.cpp:1859-1900), so measure numbering counts both.

**Existing upstream tests.**
- src/engraving/tests/split_tests.cpp: `split01..08` (63-98), `split183846` (103),
  `split184061` (113), `split295207` (122), `splitTieAtStart` (127); fixtures src/engraving/tests/split_data/.
- measure_tests.cpp: `measureSplit` (581) — with parts created by `TestUtils::createParts`.

**Scenarios.**
- STR-SPL-01: split a 4/4 bar at beat 3 → 2/4 + 2/4 bars, notes unchanged.
- STR-SPL-02: split at a tick with no chord/rest boundary on any staff → `E_NOT_ON_BEAT`, no crash.
- STR-SPL-03: split inside a triplet → `E_TUPLET_SPLIT`.
- STR-SPL-04: split at beat 1 → `E_INVALID_ARG`.
- STR-SPL-05: split a bar with linked parts; parts split identically.
- STR-SPL-06: split a bar carrying a slur and a tie across the split point.
- STR-SPL-07: split a bar with an end repeat; repeat stays on the second half.
- STR-SPL-08: split in a 2-bar batch followed by another op; undo restores byte for byte.

**Open questions.** OQ-1.

---

### join_measures  ·  command://notation/join_measures

**UI entry point.** Action `join-measures` (notationactioncontroller.cpp:282; notationuiactions.cpp:684)
→ `NotationInteraction::joinSelectedMeasures` (notationinteraction.cpp:5026-5039): requires a range
selection, `SplitJoinMeasure::joinMeasures(masterScore, startMeasure->tick(), endMeasure->tick())`
(5035), `checkAndShowError()` (5038). Second UI route: Ctrl+Del on an end barline calls
`joinMeasures(m, next)` (edit.cpp:4979-4986). Engraving: splitjoinmeasure.cpp:189-344.

**Selection set-up.** None. Call `SplitJoinMeasure::joinMeasures(masterScore, m1->tick(), m2->tick())`
(splitjoinmeasure.h:37). Do not call the wrapper (G2).

**Arguments.**
```json
{
  "type": "object",
  "required": ["from", "to"],
  "properties": {
    "from": {"type": "integer", "minimum": 1},
    "to":   {"type": "integer", "minimum": 1, "description": "inclusive, > from"}
  }
}
```

**Validation before mutation.**
- `from >= to` → `E_RANGE`. The function does not check order: with `m1` after `m2`,
  `deleteMeasures` walks `prev()` from `mbEnd` looking for `mbStart` (edit.cpp:3389-3407) and
  never finds it. The UI cannot produce this; the agent must.
- Both measures exist (`tick2measure` non-null; the function returns silently otherwise,
  splitjoinmeasure.cpp:194-196) → `E_RANGE`.
- Measure-repeat groups crossing the boundaries (198-203) → `E_MEASURE_REPEAT`.
- Staff type change inside the range (204-207) → `E_STAFFTYPE_CHANGE` (proposed).
- Frames between `from` and `to` are deleted with the measures (`deleteMeasures` → `RemoveMeasures`,
  editmeasures.cpp:293). Same policy as remove_measures (OQ-6).
- A section break inside the range is lost with the deleted measures (the section-break element is
  on the measure, and the joined measure is newly created). Proposed: refuse with `E_RANGE` or
  report (OQ-9).

**Post-condition.**
- Measure count decreased by `to-from`; new measure at `from` has `ticks()` equal to the sum of
  the joined lengths (splitjoinmeasure.cpp:278-302) and nominal time sig of the old `from`
  (278, 291).
- Time signatures that were inside the range moved to the next measure if none is there
  (306-343).
- `MScore::_error == MS_NO_ERROR`.

**Nested commands.** None. Uses `deleteMeasures(m1, m2, true)` (252), `insertMeasure` (259),
`adjustToLen` (302), `ScoreRange::write` (304). Safe under lock.

**Linked parts / excerpts.** Propagates through `deleteMeasures` and `insertMeasure`
(scoreList loops, G5), the nominal time sig and repeat flags (289-300) and time sigs moved per
linked score (313-339). Direct non-undo mutations: `s->setStartElement(0)`/`setEndElement(0)` on
spanners (235, 238), `sp->setTick2(spEndTick)` (265), and `masterScore->addElement(stc)` (274,
master-only).

**Crash and corruption risks.**
- Reversed range (above).
- Non-undoable spanner pointer/tick writes (above): undo restores the measures but not those
  fields directly; layout recomputes start/end elements. Covered only by the upstream tests below.

**Existing upstream tests.**
- src/engraving/tests/join_tests.cpp: `join01..10` (100-145), `joinTieAtStart` (150); fixtures
  src/engraving/tests/join_data/. No test with parts.

**Scenarios.**
- STR-JOI-01: join bars 2–3 of 4/4 → one 8/4 bar, notes unchanged.
- STR-JOI-02: join across a 3/4 change; change moves to the bar after.
- STR-JOI-03: join a range with a hairpin and slur ending inside.
- STR-JOI-04: join with linked parts; parts match.
- STR-JOI-05: from > to → `E_RANGE`, no crash.
- STR-JOI-06: join the two halves produced by STR-SPL-01 in the same batch; export equals original.

**Open questions.** OQ-1, OQ-6, OQ-9.

---

### set_time_signature  ·  command://notation/set_time_signature

**UI entry point.**
- Palette click with a measure (range) selected: `NotationInteraction::applyPaletteElement`
  (notationinteraction.cpp:2310-2345) → `applyPaletteElementToRange` (2528) → clef/key/time
  branch (2555-2633) → `applyDropPaletteElement(score, m1, element, modifiers, staff2track(i))`
  (2629-2631) → `Measure::acceptDrop` (measure.cpp:1380, TIMESIG 1426) → `Measure::drop`
  (1515) → `score()->cmdAddTimeSig(this, staffIdx, ts, ctrl)` (1605-1607).
- Ctrl = local (single staff); otherwise global.
- If the range ends before the end of the score, the UI **restores** the old signature after the
  range (2558-2562, 2588-2627). A single whole-measure selection does not restore (2558-2559).
- Time-signature properties dialog: `INotationInteraction::addTimeSignature`
  (notationinteraction.cpp:6342-6347), always `local = true` (6345). Not suitable for a global
  change.
- Removing a time signature: `Score::deleteItem` → `cmdRemoveTimeSig` (edit.cpp:2765-2777,
  1626-1683).

**Selection set-up.** None. Call `masterScore->cmdAddTimeSig(m, staffIdx, ts, local)`
(edit.cpp:1438) directly. `ts` is a fresh `Factory::createTimeSig(masterScore->dummy()->segment())`
with `setSig(Fraction(n, d), TimeSigType::NORMAL)` (pattern in src/engraving/tests/timesig_tests.cpp:55-56).
`cmdAddTimeSig` **takes ownership and deletes `ts`** (edit.cpp:1469, 1619); the agent must not
touch it after the call. For a range with restore, make a second call at the measure after the
range with a copy of `staff(i)->timeSig(endTick)` (UI: notationinteraction.cpp:2609-2614).

**Arguments.**
```json
{
  "type": "object",
  "required": ["measure", "numerator", "denominator"],
  "properties": {
    "measure":     {"type": "integer", "minimum": 1},
    "numerator":   {"type": "integer", "minimum": 1, "maximum": 999},
    "denominator": {"enum": [1, 2, 4, 8, 16, 32, 64, 128]},
    "symbol":      {"enum": ["normal", "common", "cut"], "default": "normal"},
    "until":       {"type": "integer", "minimum": 1, "description": "optional: restore the previous signature after this measure (UI range behaviour)"},
    "staff":       {"description": "optional {part, staff}: local time signature on one staff (UI Ctrl+drop)"},
    "remove":      {"type": "boolean", "description": "remove the time signature at this measure (cmdRemoveTimeSig)"}
  }
}
```
Limits come from the UI's time dialog: numerator 1–999 (src/palette/widgets/timedialog.ui:78-82),
denominator 1–128 powers of two (src/palette/widgets/timedialog.cpp:177-200). `common`/`cut` map
to `TimeSigType::FOUR_FOUR`/`ALLA_BREVE`; that mapping was not traced further (OQ-10).

**Validation before mutation.**
- Measure out of range → `E_RANGE`; numerator/denominator outside the limits → `E_INVALID_ARG`.
- Local (`staff` given): every ChordRest in the affected range on that staff must be a
  full-measure rest, or engraving sets `CANNOT_CHANGE_LOCAL_TIMESIG_MEASURE_NOT_EMPTY`
  (edit.cpp:1052-1077, 1308-1314) → `E_LOCAL_TIMESIG_NOT_EMPTY`. Pre-check the same loop.
- Global change over measures with any local time signature that are not all full-measure
  rests: `rewriteMeasures` returns false without setting an error (edit.cpp:1137-1149), and the
  caller unwinds the whole macro (1569). Pre-check: any staff with
  `staff(si)->timeStretch(m->tick()) != 1` in the range and the range not all full-measure rests
  → `E_LOCAL_TIMESIG_CONFLICT` (proposed).
- Tuplets or other content that `ScoreRange::write` cannot re-bar cause `rewriteMeasures` to
  return false (edit.cpp:1220-1222) and the macro to be unwound; `range.cpp` sets
  `CORRUPTED_MEASURE` in some cases (src/engraving/dom/range.cpp:623, 658, 707). This cannot be
  fully pre-checked → rely on the post-condition and abort the batch (G3).
- `remove` on measure 1 or where no time sig exists → `E_INVALID_ARG`.
- `CANNOT_CHANGE_LOCAL_TIMESIG_HAS_EXCERPTS` exists (src/engraving/dom/mscore.h:161) and has a
  dialog text, but nothing in v4.7.6 sets it; local time signatures with parts are allowed and
  handled through `findLinkedInScore` (edit.cpp:1478-1491).

**Post-condition.**
- `masterScore->staff(s)->timeSig(m->tick())->sig()` equals the requested signature on every
  affected staff, and a `TimeSig` element exists in the `TimeSig` segment at `m->tick()`.
- The macro was not unwound: the agent's own earlier operations in the batch are still present
  (compare `undoStack()->activeCommand()->childCount()` before and after, or re-check an earlier
  post-condition). If it was unwound → `E_REWRITE_FAILED`, abort the batch, roll back.
- `MScore::_error == MS_NO_ERROR`.

**Nested commands.** No `startCmd/endCmd`, but `activeCommand()->unwind()` on failure
(edit.cpp:1553, 1569, 1664). This is the most dangerous call in the group under a batch (G3).
`cmdAddTimeSig` also calls `deselectAll()` (1440).

**Linked parts / excerpts.** Propagates: measure rewrite loops `scoreList()` in
`rewriteMeasures` (edit.cpp:1177-1212); time sigs are added per score and linked to the master's
(1574-1616); the same-length branch loops `scoreList()` (1506-1530). Local changes map the staff
into each part with `findLinkedInScore` (1478-1491).

**Crash and corruption risks.**
- Unwind of the whole transaction (G3).
- `staff(staffIdx)` must be a valid master staff index; the plugin API passes a track number
  here (G10).
- Pickup: when `m` is the first measure and is irregular, only its nominal sig changes and the
  rewrite starts at measure 2 unless measure 2 has its own sig (edit.cpp:1540-1545).
- Rewrite stops at the next time signature or section break, and reinstates the old signature
  after a section break (edit.cpp:1274-1428). Following measures up to that point are re-barred,
  which changes measure count and numbering — report this.
- `ts` leaked on the unwind paths (edit.cpp:1553, 1569 return without `delete ts`); minor.

**Existing upstream tests.**
- src/engraving/tests/timesig_tests.cpp: `timesig01` (50), `timesig02` (74, failing rewrite leaves
  the score unchanged, with `doLayout()` inside the command), `timesig03..08` (100-218),
  `timesig10` (263), `timesig_78216` (296), `timesig_11` (312), `endOfMeasureTimeSigChange` (420),
  `endOfMeasureMMRTimeSigChange` (453), `endOfMeasureTie` (521), `deleteMMRTimeSig` (576).
  Fixtures in src/engraving/tests/timesig_data/.
- barline_tests.cpp `barline02` (117) and clef_tests.cpp `clef2` (56) also call `cmdAddTimeSig`.
- No upstream test changes a time signature in a score with parts, and none runs it inside an
  outer macro with earlier operations.

**Scenarios.**
- STR-TS-01: 4/4 → 3/4 from bar 5 of 16 with notes; bars re-barred, ties created, counts reported.
- STR-TS-02: 3/4 for bars 5–8 only (`until`), 4/4 restored at bar 9.
- STR-TS-03: change in a score with linked parts; parts match.
- STR-TS-04: local 5/4 on one empty staff; non-empty staff → `E_LOCAL_TIMESIG_NOT_EMPTY`.
- STR-TS-05: change over a minim triplet that cannot be re-barred → `E_REWRITE_FAILED`, score unchanged.
- STR-TS-06: batch: insert_measures, then a failing set_time_signature, then add_break → whole batch rolled back, score unchanged, no later op applied.
- STR-TS-07: change on a score with a pickup bar.
- STR-TS-08: remove a mid-score time signature.
- STR-TS-09: change on a bar inside a multimeasure rest.

**Open questions.** OQ-1, OQ-3, OQ-10.

---

### set_key_signature  ·  command://notation/set_key_signature

**UI entry point.**
- Palette click with a measure selected: same path as time signatures to `Measure::drop`
  KEYSIG (measure.cpp:1587-1603): global → `undoChangeKeySig(s, tick(), k)` for every staff of the
  score (1597-1599); Ctrl → only the target staff (1592-1594).
- Mid-measure (list selection on a note/rest, Ctrl only): `ChordRest::drop` KEYSIG
  (chordrest.cpp:408-417) → `undoChangeKeySig(staff(), tick(), k)`. Without Ctrl it falls through
  to the measure (chordrest.cpp:436).
- Engraving: `Score::undoChangeKeySig(Staff*, const Fraction&, KeySigEvent)` (edit.cpp:6028-6084).
- Palette key signatures carry only a concert key (`KeySig::setKey(Key)`,
  src/palette/internal/palettecreator.cpp:294-306; src/engraving/dom/keysig.cpp:109-122); mode
  stays `KeyMode::UNKNOWN`. Atonal is `setConcertKey(C)`, `setCustom(true)`, `setMode(NONE)`
  (palettecreator.cpp:309-314).
- Removal: initial key signature cannot be deleted (`CANNOT_REMOVE_KEY_SIG`, edit.cpp:4076-4085).

**Selection set-up.** None. Build `KeySigEvent ev; ev.setConcertKey(Key(n)); ev.setMode(mode);`
(src/engraving/dom/key.h:70-79; key.cpp:205-210) and call
`masterScore->undoChangeKeySig(staff, m->tick(), ev)` for each master staff (global) or the one
staff (local). Pass the **concert** key: the function computes the written key for transposing
staves when concert pitch is off (edit.cpp:6052-6058).

**Arguments.**
```json
{
  "type": "object",
  "required": ["measure", "key"],
  "properties": {
    "measure": {"type": "integer", "minimum": 1},
    "beat":    {"description": "optional; mid-measure change, requires staff (UI allows mid-measure only for one staff)"},
    "key":     {"type": "integer", "minimum": -7, "maximum": 7, "description": "concert-pitch sharps(+)/flats(-)"},
    "mode":    {"enum": ["unknown", "major", "minor", "none"], "default": "unknown", "description": "'none' = atonal / open key"},
    "staff":   {"description": "optional {part, staff}: local change (UI Ctrl)"}
  }
}
```

**Validation before mutation.**
- Measure out of range → `E_RANGE`; key outside −7..7 → `E_INVALID_ARG`.
- `beat` without `staff` → `E_INVALID_ARG` (mirrors chordrest.cpp:410-417).
- `beat` not on a ChordRest of that staff → `E_NOT_ON_BEAT`.
- Drum staves are skipped by the engraving function (edit.cpp:6034-6036); a local change on a
  drum staff → `E_INVALID_ARG`.

**Post-condition.**
- For every targeted non-drum staff: `staff->keySigEvent(tick).concertKey() == key`
  and a `KeySig` exists in the `KeySig` segment at `tick` on that staff's track.
- For linked part staves of each targeted staff: same.
- `MScore::_error == MS_NO_ERROR`.

**Nested commands.** None. Uses `undo(new ChangeKeySig)`, `doUndoAddElement`, `undo(new Link)`
(edit.cpp:6062-6076) and `Transpose::transpositionChanged` when the staff interval changes
(6080-6083). Safe under lock.

**Linked parts / excerpts.** Propagates: loops `ostaff->staffList()` (linked staves in all scores,
edit.cpp:6033) and links the created key sigs (6071-6075). Global changes must loop the
**master's** staves (G4).

**Crash and corruption risks.**
- `tick2measure` null logs and continues (edit.cpp:6040-6043).
- Instrument-change key signatures after the tick are rewritten
  (`updateInstrumentChangeTranspositions`, edit.cpp:6062 call, definition 6088-6125); assessment's "key changes with
  transposing instruments" case.
- No other risk found in source.

**Existing upstream tests.**
- src/engraving/tests/keysig_tests.cpp: `keysig` (43), `keysig_78216` (109), `concertPitch` (129),
  `preferSharpFlat` (142), `keysigMode` (166); fixtures src/engraving/tests/keysig_data/.
- measure_tests.cpp `insertBfKeyChange` (133).

**Scenarios.**
- STR-KS-01: global change to 3 flats at bar 9; all pitched staves show it, drum staff untouched.
- STR-KS-02: change with a B♭ clarinet, concert pitch off; clarinet shows 1 flat for concert E♭.
- STR-KS-03: local change on one staff only.
- STR-KS-04: mid-measure local change at beat 3.
- STR-KS-05: change in a score with linked parts.
- STR-KS-06: atonal (`mode: none`).

**Open questions.** OQ-11.

---

### set_tempo  ·  command://notation/set_tempo

**UI entry point.**
- Action `tempo` (notationactioncontroller.cpp:364; notationuiactions.cpp:1407) →
  `NotationActionController::addText(TextStyleType::TEMPO)` (1457-1484) →
  `NotationInteraction::addTextToItem` (notationinteraction.cpp:6196-6210) → `addText`
  (6212-6248): `startEdit`, `score()->addText(TEMPO, item)` (6218), `apply()`, then
  **`startEditText(text)`** (6241) and selects the word (6244-6246). The tempo value is parsed when
  text editing ends (`TempoText::commitText`, src/engraving/dom/tempotext.cpp:493-500).
- Engraving: `Score::addText` TEMPO case (edit.cpp:959-1026): picks a default
  `<sym>metNote…</sym> = 80` string from the nominal time sig (965-1015), creates a `TempoText`
  on the ChordRest's segment, track 0, `setXmlText`, `setFollowText(true)`, `updateTempo()`,
  then `undoAddElement` (1017-1025).
- Inspector edits: `Pid::TEMPO` and `Pid::TEMPO_FOLLOW_TEXT`
  (src/inspector/qml/MuseScore/Inspector/notation/tempos/temposettingsmodel.cpp:61-68).

**Selection set-up.** None. Replicate edit.cpp:1017-1025 with the agent's text: create
`Factory::createTempoText(cr->segment())`, `setParent(segment)`, `setTrack(0)`,
`setXmlText(text)`, `setFollowText(true)`, `updateTempo()`, `masterScore->undoAddElement(tt)`.
Do not call `addTextToItem/addText` (text-edit mode, G9). To change an existing tempo mark at the
same segment, use `undoChangeProperty(Pid::TEXT, …)` and `Pid::TEMPO` on it (Inspector path)
rather than adding a second one; `Score::addText` itself does not check for an existing one.

**Arguments.**
```json
{
  "type": "object",
  "required": ["measure"],
  "properties": {
    "measure": {"type": "integer", "minimum": 1},
    "beat":    {"description": "default beat 1"},
    "bpm":     {"type": "number", "exclusiveMinimum": 0},
    "beat_unit": {"enum": ["whole", "half", "dotted-half", "quarter", "dotted-quarter", "eighth", "dotted-eighth", "sixteenth"], "default": "quarter"},
    "text":    {"type": "string", "description": "optional words, e.g. 'Allegro'; combined as '<text> <sym>…</sym> = <bpm>'"},
    "show_metronome": {"type": "boolean", "default": true}
  },
  "anyOf": [{"required": ["bpm"]}, {"required": ["text"]}]
}
```
`Pid::TEMPO` is in quarter notes per second: the quarter pattern factor is `1.0/60.0`
(tempotext.cpp:178) and a half note `1.0/30.0` (182). When `bpm` is given without a metronome
mark in the text, set `TEMPO_FOLLOW_TEXT` false and `Pid::TEMPO` to `bpm * factor(beat_unit)`.

**Validation before mutation.**
- Measure/beat resolves to a ChordRest segment (`chordOrRest(...)` returns null otherwise and
  `addText` returns null, edit.cpp:960-963) → `E_NOT_ON_BEAT`.
- `bpm` outside `MIN_TEMPO..MAX_TEMPO` is clamped silently by `TempoText::setTempo`
  (tempotext.cpp:324-332) → validate and return `E_INVALID_ARG` instead.
- Text that `updateTempo` cannot parse while `followText` is true leaves the previous tempo
  (tempotext.cpp:260-322) → post-condition catches it.

**Post-condition.**
- A `TempoText` exists in the segment's annotations on track 0 with the requested text.
- `masterScore->tempo(tick)` equals the requested BPS (tempo map is rebuilt later by
  `setUpTempoMapLater`, tempotext.cpp:224-228; check after commit/layout).
- Each excerpt has a linked tempo text at the same tick.

**Nested commands.** None. Safe under lock. `updateTempo` calls `undoChangeProperty(Pid::TEMPO)` on
the element before it is added (tempotext.cpp:289); the UI does the same (edit.cpp:1022).

**Linked parts / excerpts.** Propagates: `undoAddElement` replicates `TEMPO_TEXT` to staff 0 and
every system-object staff of every score (edit.cpp:6706-6760). Property changes on an existing
tempo text propagate through links (src/engraving/dom/engravingobject.cpp:398-427).

**Crash and corruption risks.** No crash path found. Two tempo marks at one tick are possible via
the UI path (no duplicate check, edit.cpp:1017-1025); the agent should replace instead.

**Existing upstream tests.** No upstream test adds a tempo text through `Score::addText(TEMPO, …)`
(`git grep` of src/engraving/tests for `addText(`/`TempoText`: the only `addText` call is
chordsymbol_tests.cpp:492 with `HARMONY_A`). links_tests.cpp `DISABLED_testMMRestLink` (476-515)
reads an existing tempo text and checks its mmrest link; it is disabled.

**Scenarios.**
- STR-TMP-01: ♩ = 120 at bar 1; playback tempo 2.0 BPS.
- STR-TMP-02: "Andante ♩. = 60" in 6/8 at bar 9.
- STR-TMP-03: replace an existing tempo mark at bar 9 (no duplicate).
- STR-TMP-04: tempo appears in every linked part.
- STR-TMP-05: bpm 0 or 10000 → `E_INVALID_ARG`.

**Open questions.** OQ-12.

---

### set_barline  ·  command://notation/set_barline

**UI entry point.**
- Palette barline on a measure: `applyPaletteElementToRange` drops onto each selected measure
  with the staff's track (notationinteraction.cpp:2537-2543) → `Measure::drop` BAR_LINE
  (measure.cpp:1698-1776). For non-repeat types (else branch, 1762-1775) it creates missing end
  barlines per staff with `undoGetSegmentR(EndBarLine, ticks())` and `undoAddElement`, then
  `BarLine::drop` (src/engraving/dom/barline.cpp:397-445) →
  `score()->undoChangeBarLineType(this, st, true)` (barline.cpp:442; Ctrl → single staff, 439).
- Mid-measure barline (palette barline on a chord/rest): `ChordRest::drop` BAR_LINE
  (chordrest.cpp:212-249) creates a `SegmentType::BarLine` segment at the chord's end tick in each
  linked score; Ctrl first splits the measure there (217-221).
- Inspector type change: `BarLine::undoChangeProperty(Pid::BARLINE_TYPE)` →
  `undoChangeBarLineType(this, v, true, true)` (barline.cpp:847-857).
- Engraving: `Score::undoChangeBarLineType(BarLine*, BarLineType, bool allStaves, bool replace)`
  (edit.cpp:5733-5965).

**Selection set-up.** None. For an end barline of measure `m`: ensure an end barline exists on
staff 0 (`m->undoGetSegmentR(SegmentType::EndBarLine, m->ticks())`; create as measure.cpp:1764-1772
if missing), then `masterScore->undoChangeBarLineType(bl, type, /*allStaves*/ !staff, /*replace*/ false)`.
Repeat types are handled by set_repeat, not here.

**Arguments.**
```json
{
  "type": "object",
  "required": ["measure", "type"],
  "properties": {
    "measure": {"type": "integer", "minimum": 1, "description": "barline at the END of this measure"},
    "type":    {"enum": ["normal", "double", "end", "reverse-end", "heavy", "double-heavy", "dashed", "dotted"]},
    "staff":   {"description": "optional {part, staff}: single staff (UI Ctrl)"},
    "beat":    {"description": "optional: mid-measure barline (ChordRest::drop path); ends the chord/rest at this beat"}
  }
}
```
Type names map to `BarLineType` via `TConv::fromXml` (src/engraving/types/typesconv.h:171);
the mscx tags were not verified against these names (OQ-13).

**Validation before mutation.**
- Measure out of range → `E_RANGE`; unknown type → `E_INVALID_ARG`.
- Repeat types → `E_INVALID_ARG` ("use set_repeat").
- `beat` must land on a ChordRest of the target staff → `E_NOT_ON_BEAT`.

**Post-condition.**
- `m->endBarLineType()` (or the staff's end barline `barLineType()`) equals `type` on all staves
  (or the one staff), in master and every excerpt (`tick2measure` in each score).
- `MScore::_error == MS_NO_ERROR`.

**Nested commands.** None. Safe under lock.

**Linked parts / excerpts.** Propagates through `sbl->staff()->staffList()` per staff
(edit.cpp:5838-5883), with explicit `Link`/`Unlink` handling for generated barlines (5873-5882).
When `allStaves` is true it uses the master's segment (5803-5822).

**Crash and corruption risks.**
- **Non-undoable add:** when a linked staff has no barline in that segment, the function creates
  one and calls `lscore->addElement(lbl)` (edit.cpp:5856-5868), not `undoAddElement`. Undo does not
  remove it. Pre-create missing end barlines with `undoAddElement` (as measure.cpp:1764-1772 does
  for the drop target's score) to keep this branch from running in the master; parts remain
  exposed (OQ-14).
- Layout dependence: `isFirstInSystem()` decides whether a following start repeat is kept
  (edit.cpp:5791-5793). Measures created earlier in the same batch have no system (G6).
- `undoChangeBarLineType` returns early if the type is unchanged (5735-5737); post-condition
  still holds.

**Existing upstream tests.**
- src/engraving/tests/barline_tests.cpp: `barline01..06` (73-278), `barline179726` (337, drops a
  barline with `EditData(0)` onto measures and barlines, helper at 314-329),
  `deleteSkipBarlines` (415); fixtures src/engraving/tests/barline_data/.
- `EditData dropData(0)` in barline_tests.cpp:316 shows `drop()` works with null callbacks, should
  the core prefer calling `Measure::drop` directly (bypassing `acceptDrop`).

**Scenarios.**
- STR-BAR-01: double barline at end of bar 8, all staves, parts match.
- STR-BAR-02: final barline at end of the last bar.
- STR-BAR-03: dashed barline on one staff only.
- STR-BAR-04: set back to normal (generated flag restored).
- STR-BAR-05: mid-measure barline at beat 3.
- STR-BAR-06: undo after STR-BAR-01 in a score with parts; parts have no leftover barline.

**Open questions.** OQ-1, OQ-13, OQ-14.

---

### set_repeat  ·  command://notation/set_repeat

Scope assumed here: start repeat, end repeat, and play count on measures. Voltas are spanners
(expression-lines `add_line`); D.S./D.C./Coda/Fine are `Jump`/`Marker` elements (OQ-15).

**UI entry point.**
- Palette start/end/end-start repeat barlines on a measure: `Measure::drop` BAR_LINE
  (measure.cpp:1698-1761): optional play count `undoChangeProperty(Pid::REPEAT_COUNT, …)` on this
  measure (1702-1704); START_REPEAT loops `scoreList()` and sets `REPEAT_START` on the measure at
  the same tick (1715-1728); END_REPEAT sets `REPEAT_END` likewise and calls
  `undoUpdatePlayCountText` (1729-1743); END_START sets `REPEAT_END` here and `REPEAT_START` on the
  next (1744-1761). Each refuses measure-repeat groups with `CANNOT_SPLIT_MEASURE_REPEAT`
  (1717-1722, 1731-1736, 1746-1751).
- Removal: delete the repeat barline → `Score::deleteItem` BAR_LINE (edit.cpp:3023-3058): loops
  `scoreList()`, sets `REPEAT_START`/`REPEAT_END` false and `REPEAT_COUNT` 2.
- Play count: measure properties dialog `REPEAT_COUNT` on the one measure
  (src/notationscene/widgets/measureproperties.cpp:327), opened by action `measure-properties`
  (dialog, G9).
- `Measure::setProperty(REPEAT_COUNT)` also calls `undoUpdatePlayCountText` (measure.cpp:2921-2924).

**Selection set-up.** None. Replicate measure.cpp:1715-1761 / edit.cpp:3030-3053 on the master:
for each `lscore` in `scoreList()`, `lscore->tick2measure(m->tick())->undoChangeProperty(Pid::REPEAT_START|REPEAT_END, value)`;
for `play_count`, `m->undoChangeProperty(Pid::REPEAT_COUNT, n)` then
`masterScore->undoUpdatePlayCountText(m)` (edit.cpp:5701-5731).
For an mmrest-covered measure use `mmRestFirst()` for start and `mmRestLast()` for end
(measure.cpp:1716, 1730) — with G7 addressing the agent already has the underlying measure.

**Arguments.**
```json
{
  "type": "object",
  "required": ["measure"],
  "properties": {
    "measure":    {"type": "integer", "minimum": 1},
    "start":      {"type": "boolean", "description": "start repeat at the beginning of this measure"},
    "end":        {"type": "boolean", "description": "end repeat at the end of this measure"},
    "play_count": {"type": "integer", "minimum": 2, "description": "times the repeated section plays; only with end"}
  },
  "minProperties": 2
}
```

**Validation before mutation.**
- Measure out of range → `E_RANGE`.
- `start: true` and `m->isMeasureRepeatGroupWithPrevM(s)` on any staff; `end: true` and
  `isMeasureRepeatGroupWithNextM(s)` → `E_MEASURE_REPEAT` (measure.cpp:1717-1751).
- `play_count` without an end repeat (existing or requested) → `E_INVALID_ARG`.

**Post-condition.**
- `m->repeatStart()` / `m->repeatEnd()` (measurebase.h:149-153) as requested in master and in
  `tick2measure(tick)` of every excerpt.
- `m->repeatCount() == play_count` (measure.cpp:2893-2894).
- Playback: `masterScore->repeatList()` reflects the repeat after layout (optional check).

**Nested commands.** None. Safe under lock.

**Linked parts / excerpts.** `REPEAT_START`/`REPEAT_END` propagate only because the code loops
`scoreList()` (G5). `REPEAT_COUNT` is set on the single measure in both UI paths
(measure.cpp:1703; measureproperties.cpp:327); whether it reaches parts depends on the measure
being linked (G5, OQ-16).

**Crash and corruption risks.** None found beyond G5/G6. Removing an end repeat via
`undoChangeBarLineType` uses layout state (edit.cpp:5752-5761); the direct property path above
does not.

**Existing upstream tests.** src/engraving/tests/repeat_tests.cpp `repeat01..37` (76-258) test
the repeat list (playback unrolling) of fixtures, not the edit. barline_tests.cpp `barline179726`
(337) drops onto repeat barlines.

**Scenarios.**
- STR-REP-01: repeat bars 5–8 (start at 5, end at 8); playback plays 5–8 twice.
- STR-REP-02: end repeat with play count 3; play-count text appears.
- STR-REP-03: remove a start repeat.
- STR-REP-04: repeat in a score with parts; parts show the repeats.
- STR-REP-05: repeat on bars inserted earlier in the same batch (G5 unlinked measures).

**Open questions.** OQ-15, OQ-16.

---

### add_break  ·  command://notation/add_break

**UI entry point.** Actions `system-break`, `page-break`, `section-break`
(notationactioncontroller.cpp:267-272; notationuiactions.cpp:634, 640, 672) →
`NotationInteraction::toggleLayoutBreak` (notationinteraction.cpp:5941-5946) →
`Score::cmdToggleLayoutBreak(type)` (src/engraving/editing/cmd.cpp:4900-5033). Palette breaks
route to the same function (notationinteraction.cpp:2429-2433, 2549-2553). The function
**toggles**: for a range it toggles the break on the last measure and, if more than one measure
is selected, also *before* the range (cmd.cpp:4931-4938). Breaks are set with
`MeasureBase::undoSetBreak(bool, LayoutBreakType)` (src/engraving/dom/measurebase.cpp:510-561).

**Selection set-up.** None. Call `mb->undoSetBreak(true|false, type)` on the target measure (or
frame) directly, after the same side calls the toggle makes when adding:
`EditSystemLocks::removeSystemLocksOnAddLayoutBreak(score, type, mb)` (cmd.cpp:4991,
5002, 5013), and for LINE/PAGE clear the other (4995-4997, 5006-5008). This gives
idempotent add/remove instead of a toggle and avoids the extra "break before range" behaviour.

**Arguments.**
```json
{
  "type": "object",
  "required": ["measure", "type"],
  "properties": {
    "measure": {"type": "integer", "minimum": 1, "description": "break after this measure"},
    "type":    {"enum": ["line", "page", "section", "none"], "description": "'none' = keep-together (NOBREAK)"},
    "remove":  {"type": "boolean", "default": false},
    "score":   {"description": "optional part name: line/page breaks are per score; default master"}
  }
}
```

**Validation before mutation.**
- Measure out of range → `E_RANGE`.
- `type: none` on a system-locked measure: the toggle returns without change
  (cmd.cpp:4976-4981) → `E_LOCKED` (proposed).
- `score` names no open excerpt → `E_RANGE`.

**Post-condition.** `mb->lineBreak()/pageBreak()/sectionBreak()/noBreak()` (measurebase.h:161-170)
as requested, and exactly one `LayoutBreak` element of that type in `mb->el()`
(`cleanupLayoutBreaks`, measurebase.cpp:565-603). For section breaks, the same on
`tick2measure(tick)` in every excerpt.

**Nested commands.** None. Safe under lock.

**Linked parts / excerpts.** Section breaks propagate to every score (`undoAddElement`
LAYOUT_BREAK/SECTION, edit.cpp:6864-6903). Line, page and no-break are local to the score they
are added in (they fall through to the general branch, edit.cpp:6905+). This matches the UI:
parts have their own system breaks.

**Crash and corruption risks.**
- `undoSetBreak(false, …)` and the PAGE/SECTION paths clear flags with direct setters
  (`setLineBreak(false)` etc., measurebase.cpp:516-546) and rely on `cleanupLayoutBreaks(true)`
  removing the element with `undoRemoveElement` (595-598). The flag is derived from the element on
  undo/redo (not traced further, OQ-17).
- `Measure::drop` LAYOUT_BREAK also uses direct `setLineBreak/setPageBreak(false)`
  (measure.cpp:1618-1643); the agent does not use that path.

**Existing upstream tests.** No test calls `cmdToggleLayoutBreak` or `undoSetBreak` directly
(grep). Layout-break handling appears in src/engraving/tests/layoutelements_tests.cpp and
links_tests.cpp fixtures only.

**Scenarios.**
- STR-BRK-01: system break after bar 4; render shows bar 5 starting a system.
- STR-BRK-02: page break after bar 16.
- STR-BRK-03: section break after bar 8 in a score with parts; parts get it.
- STR-BRK-04: line break in a part only; master unaffected.
- STR-BRK-05: add twice → idempotent; remove → gone.

**Open questions.** OQ-17.

---

### add_clef  ·  command://notation/add_clef

**UI entry point.**
- Palette clef on a measure: `applyPaletteElementToRange` applies clefs to every selected staff
  (notationinteraction.cpp:2564-2566) and restores the old clef after a multi-measure range
  (2588-2600); mid-measure via the first chord/rest when the range starts mid-measure (2571-2587).
  Measure target → `Measure::drop` CLEF → `score()->undoChangeClef(staff, this, clefType)`
  (measure.cpp:1582-1585). ChordRest target → `cmdInsertClef(clef, cr)` →
  `undoChangeClef(cr->staff(), cr, type)` (chordrest.cpp:253-255; cmd.cpp:3773-3777).
- Actions `clef-violin`, `clef-bass` (notationactioncontroller.cpp:488-489, 1447-1455) →
  `NotationInteraction::insertClef` (notationinteraction.cpp:8300-8303) →
  `Score::cmdInsertClef(ClefType)` (cmd.cpp:3763-3766), which uses the **note-input** cursor
  (`m_is.track()`, `m_is.cr()`); not usable outside note input.
- Engraving: `Score::undoChangeClef(Staff*, EngravingItem*, ClefType, bool forInstrumentChange, Clef*)`
  (edit.cpp:6135-6271).

**Selection set-up.** None. Call `masterScore->undoChangeClef(staff, target, clefType)` with
`target` = the `Measure` (start of measure) or the `ChordRest` on that staff at `beat`.

**Arguments.**
```json
{
  "type": "object",
  "required": ["measure", "staff", "clef"],
  "properties": {
    "measure": {"type": "integer", "minimum": 1},
    "beat":    {"description": "optional mid-measure position; must be a chord/rest on that staff"},
    "staff":   {"description": "{part, staff}"},
    "clef":    {"type": "string", "description": "ClefType mscx tag via TConv::fromXml, e.g. G, F, C3, C4, G8vb, PERC"}
  }
}
```
`TConv::fromXml(AsciiStringView, ClefType def)` (typesconv.h:81) gives the name mapping.

**Validation before mutation.**
- Measure/staff out of range → `E_RANGE`; unknown clef → `E_INVALID_ARG`.
- Clef staff group must match the staff type at the tick: the function silently skips staves where
  `ClefInfo::staffGroup(ct) != staffType->group()` (edit.cpp:6200-6204), e.g. a treble clef on a
  TAB or percussion staff → `E_INVALID_ARG` (pre-check with the same test).
- `beat` not on a ChordRest of that staff → `E_NOT_ON_BEAT`.

**Post-condition.**
- `staff->clef(tick) == clefType` (for transposing staves compare in the current concert-pitch
  mode, see edit.cpp:6208-6224) on the target staff and its linked staves.
- `MScore::_error == MS_NO_ERROR`.

**Nested commands.** None. Uses `ChangeClefType` and `doUndoAddElement` (edit.cpp:6225-6254).
Safe under lock.

**Linked parts / excerpts.** Propagates: loops `ostaff->staffList()` (edit.cpp:6173) and adds
linked clones (6243-6245). Also changes the clef inside an mmrest if present (6232-6241).

**Crash and corruption risks.**
- Placement rule: a clef at the start of a measure with a previous measure is stored at the end of
  the previous measure (`moveClef`, edit.cpp:6143-6146); for a `Measure` target this does not check
  for a section break, whereas a ChordRest at `rtick 0` does (6160-6165). After a section break the
  agent should pass the first ChordRest, not the Measure (OQ-18).
- On measure 1 the target is the header clef (`SegmentType::HeaderClef`, 6146-6148): this changes
  the staff's initial clef.
- `clef->setSmall(isSmall)` on an existing clef is a direct mutation (6259); layout-only property.

**Existing upstream tests.** src/engraving/tests/clef_tests.cpp: `clef1` (44), `clef2` (56),
`clef3` (76, calls `undoChangeClef(staff(0), m, ClefType::F)`); src/engraving/tests/clef_courtesy_tests.cpp;
measure_tests.cpp `insertBfClefChange` (101).

**Scenarios.**
- STR-CLF-01: bass clef at bar 9 on the cello staff; parts match.
- STR-CLF-02: change the initial clef of a staff (bar 1).
- STR-CLF-03: mid-measure treble clef at beat 3.
- STR-CLF-04: treble clef on a TAB staff → `E_INVALID_ARG`.
- STR-CLF-05: clef at the first bar after a section break.
- STR-CLF-06: clef on a transposing instrument with concert pitch on and off.

**Open questions.** OQ-18.

---

## Open questions

- **OQ-1 (layout inside a batch).** Within one transaction, measures created earlier have no
  `system()` and mmrest pointers reflect the last layout (G6). The commands above avoid
  `acceptDrop`, but `undoChangeBarLineType` (edit.cpp:5752, 5791) and `Score::selectRange`
  (score.cpp:3549-3551) still read layout state. Upstream tests call `score->doLayout()` inside a
  command (timesig_tests.cpp:84). Whether the core lays out between structural operations in a
  batch, or forbids mixing structure ops with dependent ops in one batch, is a core design
  decision.
- **OQ-2 (measure addressing).** 1-based `Measure` index (`crMeasure`) vs displayed number
  (`MeasureBase::no()`), and how frames are referenced (G8).
- **OQ-3 (detecting `unwind()`).** The cleanest detector for a time-signature rewrite failure.
  `UndoMacro`/`UndoCommand::childCount()` is used in `endCmd`'s log line (cmd.cpp:433); whether it is
  reachable and reliable from `INotationUndoStack` was not checked (the notation layer exposes no
  child count; the agent may need `masterScore->undoStack()->activeCommand()` directly).
- **OQ-5 (repeat flags when inserting measures with parts).** `MasterScore::insertMeasure` sets
  `REPEAT_START`/`REPEAT_END` on `masterMeasure` (the master's new measure) inside the per-score
  loop (masterscore.cpp:691-694, 716-719), and clears them on each score's own neighbour measure.
  Reading the code, an excerpt's new (unlinked, G5) measure does not receive the flag. Verify with
  scenario STR-INS-03 in a score with parts.
- **OQ-6 (frames inside measure ranges).** remove_measures and join_measures delete frames that lie
  between the endpoints (editmeasures.cpp:293). Refuse, allow with a flag, or report.
- **OQ-7 (note-entry mode).** `cmdTimeDelete` and `rewriteMeasures` change behaviour in note-entry
  mode (edit.cpp:5034-5065, 1250-1267). Should the core leave note input before any structure
  command?
- **OQ-8 (staff index mismatch in `deleteMeasures`).** In the per-score loop, excerpt staff indices
  index master-keyed containers (edit.cpp:3444, 3450, 3501). Effect on parts whose staves are not
  the first master staves is unverified; needs a scenario (remove the bar holding a key change in a
  part made from the 2nd instrument).
- **OQ-9 (section break inside a join).** What happens to a section break on a measure inside the
  joined range was not traced to a test.
- **OQ-10 (common/cut time).** How the palette's common/cut time sig sets `TimeSigType` and
  `sig()` (palette creator) was not traced.
- **OQ-11 (key mode).** The palette leaves `KeyMode::UNKNOWN`; whether to expose major/minor
  (stored via `Pid::KEYSIG_MODE`) or keep UNKNOWN to match the UI.
- **OQ-12 (tempo replacement).** Replace an existing tempo text at the tick (Inspector-style
  property change) or add a second one as the `tempo` action does.
- **OQ-13 (barline type names).** The exact `BarLineType` mscx tags accepted by `TConv::fromXml`
  (typesconv.h:171) were not listed.
- **OQ-14 (non-undoable barline add in parts).** `undoChangeBarLineType` uses `lscore->addElement`
  (edit.cpp:5865) for linked staves without a barline. When this branch runs for part scores is
  unverified; scenario STR-BAR-06 checks undo leaves no stray barline.
- **OQ-15 (set_repeat scope).** Do jumps and markers (D.S., D.C., Coda, Segno, Fine;
  `Measure::drop` MARKER/JUMP measure.cpp:1527-1532, replicated to all parts by
  `undoAddElement`, edit.cpp:6706-6714) belong here or in `text`?
- **OQ-16 (play count in parts).** `REPEAT_COUNT` is set on one measure in every UI path
  (measure.cpp:1703; measureproperties.cpp:327) and relies on measure links (G5). Loop `scoreList()`
  to be safe, or match the UI exactly?
- **OQ-17 (break flags on undo).** Whether undo of `undoSetBreak` restores the `LINE_BREAK` etc.
  flags through the `LayoutBreak` element's add/remove was not traced into `MeasureBase::add/remove`.
- **OQ-18 (clef after section break).** Confirm with STR-CLF-05 that passing the Measure after a
  section break puts the clef at the end of the previous section (edit.cpp:6143-6146) and that
  passing the first ChordRest avoids it.
