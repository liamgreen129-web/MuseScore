# Spec sheet: `instrumentation` group (MuseScore Studio 4.7.6)

Scout: `api-scout`, task P0.S-instrumentation. Source: branch `agent/4.7` at v4.7.6.
Every `file:line` refers to the working tree at that tag; paths are relative to the
repository root. Facts already established in `specs/notes.md` G1–G10 and
`specs/structure.md` G1–G10 (lock semantics, `MScore::_error`, `checkAndShowError`,
work on the master score) are referenced, not repeated.

Commands: add_part, remove_part, rename_part, set_instrument, add_staff, set_staff_type,
set_transposition.

---

## 0. Group-wide findings (referenced below as I1–I9)

### I1. The master's `INotationParts` mutators commit the outer transaction (blocking hazard)

`IMasterNotation::parts()` is a `MasterNotationParts` (src/notation/internal/masternotation.cpp:79).
Its overrides bracket their work with `startGlobalEdit` / `endGlobalEdit`
(src/notation/internal/masternotationparts.cpp:52-62):

```cpp
void MasterNotationParts::startGlobalEdit(...) { NotationParts::startEdit(actionName); undoStack()->lock(); }
void MasterNotationParts::endGlobalEdit()      { undoStack()->unlock(); NotationParts::apply(); }
```

The lock is a single bool (`UndoStack::setLocked`, src/engraving/editing/undo.cpp:283-286;
`NotationUndoStack::lock/unlock`, src/notation/internal/notationundostack.cpp:165-181).
Called inside the agent's transaction (stack already locked):
1. `startGlobalEdit`: `prepareChanges` is a no-op (notationundostack.cpp:112-114); `lock()` re-sets true.
2. `endGlobalEdit`: `unlock()` clears the **agent's** lock; `NotationParts::apply()` →
   `commitChanges()` (notationparts.cpp:846-851) now runs `Score::endCmd()`
   (notationundostack.cpp:132-145), which lays out and **closes the agent's macro**
   (cmd.cpp:406-447), including every earlier operation of the batch.
3. Every later edit in the batch runs with no active macro and is performed but not
   recorded (`UndoStack::pushAndPerform`, undo.cpp:309-331; notes.md G1), so it cannot be
   undone or rolled back; the agent's final `unlock()/commitChanges()` hits
   "no command active" (cmd.cpp:412-416).

Without the agent's lock the result is the same: `startCmd` returns early because a macro
is active (cmd.cpp:354-357), and `endGlobalEdit` still commits it.

Overrides affected (masternotationparts.h:37-54): `setParts` (64-100), `removeParts` (102-115),
`removeStaves` (117-130), `appendStaff` (132-157), `appendLinkedStaff` (159-184),
`replaceInstrument` (186-220), `replaceDrumset` (222-235), `addSystemObjects`,
`removeSystemObjects`, `moveSystemObjects`, `moveSystemObjectLayer*` (258-321).
These are exactly the Instruments-panel paths for add_part, remove_part, set_instrument,
add_staff and (via the Staff/Part Properties dialog) set_transposition.

**Rule for this group:** never call these overrides through `INotationParts` inside an agent
transaction. Two ways to reach the same code, both lock-safe (conductor's choice, OQ-1):
- **Option A, replicate the override body.** Each override is a short sequence of calls to
  the *base* `NotationParts` implementation on the master plus the same call on each excerpt's
  parts object. The base methods only use `startEdit/apply/rollback`
  (notationparts.cpp:841-856), which are no-ops under the lock (notes.md G1). A qualified call
  `masterParts->NotationParts::removeParts(ids)` needs `static_cast` to the internal class
  (`src/notation/internal/notationparts.h`, `masternotationparts.h`). Excerpt parts objects
  are plain `NotationParts` (src/notation/internal/notation.cpp:60), so calling them through
  `INotationParts` is already the base implementation.
- **Option B, call the engraving functions directly** (`EditPart`, `Score`, `Transpose`), as
  listed per command. No internal notation headers, no mid-transaction notifications.

### I2. Non-overridden `NotationParts` methods are lock-safe but notify mid-transaction

`setInstrumentName` (notationparts.cpp:412-438), `setInstrumentAbbreviature` (440-465),
`setStaffType` (534-556), `setStaffConfig` (558-578), `setPartVisible` (259-287),
`setPartSharpFlat` (289-320), `insertPart` (650-667) are not overridden by the master.
Under the lock their `startEdit`/`apply` do nothing to the undo stack, but `apply()` fires
`m_partsChanged.notify()` (notationparts.cpp:850), which `MasterNotation` forwards to
`notifyAboutNotationChanged()` (masternotation.cpp:87-89), and they fire item notifiers
(`notifyAboutPartChanged`, 1248-1255). structure.md G3 already judged the equivalent
`NotationInteraction::apply()` notification harmless.
`NotationParts::rollback()` (853-856) is a no-op under the lock, so a base method that
"rolls back" on failure (`replaceInstrument`, 704-707) leaves whatever it already did; in
that function nothing has been mutated before the failure point.

### I3. Master, excerpts and IDs

- Every excerpt score has its own `Part` and `Staff` objects carrying the master's IDs
  (src/engraving/dom/excerpt.cpp:362, 369, 604-605); `Score::partById` / `staffById`
  look them up per score (src/engraving/dom/score.cpp:5779-5799). The undo stack is the
  master's (score.cpp:6286).
- Part IDs are written to the file (`<Part id=...>`, src/engraving/rw/write/twrite.cpp:2485),
  so a part ID is a stable handle across save/load.
- `Score::undo(cmd)` pushes exactly one command on one object: `ChangePart`,
  `ChangeInstrumentLong/Short`, `ChangeStaffType`, `ChangeStaff` (editpart.cpp:129-197,
  src/engraving/editing/editstaff.cpp:277-336) do **not** reach the excerpts. Propagation to
  parts happens only where the code loops the excerpts explicitly (the `MasterNotationParts`
  overrides) or goes through links (`undoChangeProperty`, engravingobject.cpp:398-423;
  `InstrumentChange::setupInstrument`'s `linkList()` loop, instrchange.cpp:134-139).
- The excerpt notation list is `IMasterNotation::excerpts()` (imasternotation.h:54;
  masternotation.cpp:718), all inited (`createAndInitExcerptNotation`, masternotation.cpp:67,
  790). It is refreshed only when `changesChannel` fires after `endCmd`
  (masternotation.cpp:140-144, 608-650). Inside a transaction an excerpt deleted by
  remove_part is still in the list. `masterScore()->excerpts()` is updated immediately by
  `RemoveExcerpt::redo` (editexcerpt.cpp:90-94; masterscore.cpp:238-247).

### I4. No dialogs and no `MScore` errors on the engraving paths

None of `EditPart`, `NotationParts`, `MasterNotationParts`, `Transpose::transpositionChanged`
or `InstrumentChange::setupInstrument` call `interactive()`, `MessageBox` or `MScore::setError`
(grep over editpart.cpp, editstaff.cpp, instrchange.cpp, notationparts.cpp,
masternotationparts.cpp; transpose.cpp sets only `TRANSPOSE_NO_FRET_DIAGRAM` *warnings*, in
`transposeFretDiagram`, transpose.cpp:576, 590, 628, which `transpositionChanged` does not
reach). No path in this group calls `activeCommand()->unwind()`.
The dialogs are all in the UI layer and are bypassed by calling the code beneath them:
instrument picker (`SelectInstrumentsScenario`, src/instrumentsscene/internal/selectinstrumentscenario.cpp:61-115),
"delete instrument(s)?" warning when excerpts exist (layoutpaneltreemodel.cpp:143-145, 1027-1041),
"reset all formatting?" (parttreeitem.cpp:256-271), invalid-name warning in Staff/Part
Properties (src/notationscene/widgets/editstaff.cpp:516-520), and the nested `QEventLoop`
picker for a dropped instrument change (notationinteraction.cpp:2246-2282).

### I5. The UI ends note input and clears the selection first

`setParts`, `removeParts`, `removeStaves`, `moveParts`, `moveStaves` call
`endInteractionWithScore()` (notationparts.cpp:1242-1246): `clearSelection()` and
`noteInput()->endNoteInput(true)`, which resets the input track and segment
(src/notation/internal/notationnoteinput.cpp:371-400). Neither opens a dialog. The agent must
do the equivalent (or refuse with `E_BUSY` while note input or text editing is active,
notes.md G6) before structural part/staff changes, because the selection and `InputState`
can point into removed staves.

### I6. Layout and notifications after commit are automatic

`NotationParts` re-reads parts and staves from `changesChannel` after `endCmd`
(notationparts.cpp:322-348, 350-403), `MasterNotation` rebuilds excerpt notations from it
(masternotation.cpp:140-144), and the playback model listens to the same channel
(src/engraving/playback/playbackmodel.cpp:75-78). Option B therefore needs no extra
notification beyond the core's usual `notationChanged` after commit.

### I7. Instrument templates

- Lookup by instruments.xml id: `IInstrumentsRepository::instrumentTemplate(id)`
  (src/notation/iinstrumentsrepository.h:36-37) returns a static **dummy** template with an
  empty id when not found (src/notation/internal/instrumentsrepository.cpp:95-104);
  `mu::engraving::searchTemplate(id)` returns `nullptr` (src/engraving/dom/instrtemplate.cpp:721-731).
  The UI uses the repository (selectinstrumentscenario.cpp:95). Validate `templ.id == id`.
- `Instrument::fromTemplate` (instrument.cpp:1182 onward) copies names, ranges, transposition,
  clefs, string data and drumset.
- Example: `bb-clarinet` has longName "Clarinet", trait "*B♭", transposeDiatonic -1,
  transposeChromatic -2 (share/instruments/instruments.xml:2502-2515).

### I8. Names: part name vs long/short instrument name vs part-score title

- `Part::partName()` (part.h:161-162, field 204) is the mixer/track name and the default
  excerpt title (excerpt.cpp:1758-1759). The only undoable way to change it is `ChangePart`
  (editpart.cpp:136-157), i.e. replacing the instrument.
- Long and short names live on the `Instrument` at a tick (`Part::setLongNames/ShortNames`,
  part.cpp:192-200) and are what the score shows. They are stored as **XML text**:
  `nameAsPlainText()` unescapes them (instrument.cpp:1167-1170; `StaffName::toPlainText`,
  instrument.cpp:894-897).
- The part score's title is `Excerpt::name()`, changed undoably by `ChangeExcerptTitle`
  (editexcerpt.cpp:123-129; UI: `ExcerptNotation::undoSetName`, excerptnotation.cpp:98-116).
- Two different title formatters exist: new parts use the file-static
  `formatInstrumentTitleOnScore` (notationparts.cpp:51-84, handles only the Transposition
  trait), instrument replacement uses the public inline `formatInstrumentTitle`
  (src/notation/notationtypes.h:300-351, handles Tuning, Transposition, Course traits).

### I9. Plugin API (src/engraving/api/v1/score.cpp) diverges from the UI in this area

Use only as a warning; do not copy.
- `replaceInstrument` (score.cpp:166-181) → `EditPart::replacePartInstrument` on one score:
  no excerpts, no `Transpose::transpositionChanged` (so written notes and key signatures are not
  re-spelled), no staff-type change, part name = bare `trackName()` (editpart.cpp:314).
- `removeParts` (241-253) → `EditPart::removeParts` on one score: excerpt staves stay in
  the part scores, unlinked, and part scores built from the removed part are not deleted.
- `appendStaff` / `appendLinkedStaff` (349-373) → `EditPart::appendStaff` on one score: no
  staff added in part scores; `EditPart::doAppendStaff` skips `setBracketsAndBarlines`
  (editpart.cpp:645-665 vs notationparts.cpp:918).
- `insertPart(id, index)` (403-412) → `EditPart::insertPart` (editpart.cpp:711-730) inserts
  the staves **before** the part: `staffIdx(part)` for a part not yet in `m_parts` is the
  total staff count (score.cpp:5767-5777), so the staves always land at the bottom of the
  score while the part is inserted at `index`. For `index < nparts` staff order and part
  order disagree. Also no key signatures (no `adjustKeySigs`), no numbering ("Flute 2").
  `appendPart` (131-157 → score.cpp:4380-4393) has the same no-key-signature gap and
  silently substitutes the default template for an unknown id (score.cpp:150-154).
- `setStaffType(staff, int)` (231-239) casts any int to `StaffTypes`; out-of-range values
  silently become STANDARD (`StaffType::preset`, src/engraving/dom/stafftype.cpp:1087-1094).

---

### add_part  ·  command://notation/add_part

**UI entry point.**
Instruments panel "+" or `instruments` action → `InstrumentsActionsController::selectInstruments`
(src/instrumentsscene/internal/instrumentsactionscontroller.cpp:36, 49-60) → picker dialog
(selectinstrumentscenario.cpp:61-115) returns the full list of parts (existing entries flagged
`isExistingPart`, new ones carrying a template) plus a `ScoreOrder` →
`MasterNotationParts::setParts` (masternotationparts.cpp:64-100):
1. `keyList = score()->keyList()` before any change (68; `Score::keyList`, score.cpp:2705-2776).
2. `endInteractionWithScore()` (70; I5), `startGlobalEdit` (71; **I1 hazard**).
3. `doSetScoreOrder(order)` → `EditPart::setScoreOrder` → `ChangeScoreOrder` + bracket reset
   (notationparts.cpp:405-410; editpart.cpp:755-764).
4. `removeMissingParts` (1102-1125): removes every existing part absent from the list.
5. `insertNewParts(list, keyList)` (1127-1168), per new entry:
   `Instrument::fromTemplate`; `new Part(score)`; `setSoloist`; `setInstrument`;
   numbering `resolveNewInstrumentNumber` (1209-1235: existing parts with the same instrument
   id → count+1; otherwise 1 if the list adds several, else 0); `setPartName`,
   `setLongName`, `setShortName` with `formatInstrumentTitleOnScore` (51-84);
   `score()->undoInsertPart(part, partIdx)` (1161; edit.cpp:6472-6475);
   `appendStaves(part, templ, keyList)` (1045-1071): per template staff
   `Factory::createStaff`, `initStaff` (1080-1100: staff type preset or STANDARD, small,
   lines, bracket, bracket span, barline span, default clef), `undoInsertStaff(staff, i, true)`
   (1073-1078; edit.cpp:6528-6544 → `Measure::cmdAddStaves` creates rests and replicates
   time signatures, measure.cpp:1218-1291); then `adjustKeySigs(first, end, keyList)`
   (score.cpp:2669-2698), which writes key signatures transposed for the instrument unless
   concert pitch is on.
6. `updateSoloist` (1176-1187), `sortParts(list)` → one `SortStaves` (1189-1207;
   editstaff.cpp:224-242), `setBracketsAndBarlines()` (1237-1240; score.cpp:3988-3991).
7. Per excerpt: `removeMissingParts`, `sortParts` with its own current order,
   `setBracketsAndBarlines` (masternotationparts.cpp:80-96). New parts are **not** added to
   any excerpt.
8. `endGlobalEdit()` (99; commits).

The dialog places a new instrument by score order when the user does not move it:
`InstrumentsOnScoreListModel::insertInstrument` inserts before the first instrument whose
`ScoreOrder::instrumentSortingIndex` is larger (instrumentsonscorelistmodel.cpp:308-322);
if the resulting order no longer matches the score order it switches to a customized clone
(`verifyScoreOrder`, 365-377; `createCustomizedScoreOrder`, 379-388; `ScoreOrder::isScoreOrder`, scoreorder.h:94-95).

**Agent path.**
- Option A (I1): `static_cast<NotationParts*>(master->parts().get())->NotationParts::setParts(list, master->parts()->scoreOrder())`
  with `list` = every existing part (`isExistingPart = true`, `partId`, `isSoloist` = current)
  in current order plus the new `PartInstrument` at the target index. Lock-safe (base
  `setParts`, notationparts.cpp:227-243, uses `startEdit/apply`). Differences from the master
  override: `keyList` is read after `removeMissingParts` (no difference when nothing is
  removed), and the excerpt re-sort loop (masternotationparts.cpp:80-96) is skipped (with no
  removals each excerpt's sort is its current order).
- Option B: reproduce steps 1, 5, 6 on `masterScore()`; `formatInstrumentTitleOnScore` and
  `resolveNewInstrumentNumber` are file-static/private and must be copied. `doSetScoreOrder` is
  needed only if the order becomes customized.

**Selection set-up.** None. Clear selection / end note input first (I5).

**Arguments.**
```json
{
  "type": "object",
  "required": ["instrument"],
  "properties": {
    "instrument": {"type": "string", "description": "instruments.xml id, e.g. \"bb-clarinet\""},
    "index": {"type": "integer", "minimum": 0,
              "description": "0-based part position after insertion; omitted = score-order position, as the dialog does"},
    "soloist": {"type": "boolean", "default": false}
  }
}
```
Returns `{part: {id, index, name, longName, shortName, staves}}`.

**Validation before mutation.**
- No score → `E_NO_SCORE`; note input or text edit active → `E_BUSY` (I5).
- Template not found (`templ.id.isEmpty()` from the repository, I7) → `E_UNKNOWN_INSTRUMENT`.
- `index > parts().size()` → `E_RANGE`.
- Template `staffCount == 0` → `E_INVALID_ARG` (`appendStaves` returns with no staves,
  notationparts.cpp:1064-1066, leaving a staff-less part).

**Post-condition.** `parts().size()` grew by 1; `parts()[index]->instrumentId() == instrument`;
`part->nstaves() == templ.staffCount`; `score->nstaves()` grew by `staffCount`; each new staff
`idx()` is contiguous and in part order; each measure has an `MStaff` for each new staff
(`Measure::cmdAddStaves`, measure.cpp:1225-1233).

**Nested commands.** No `startCmd/endCmd` in `NotationParts::setParts` or in the engraving
calls. The master override is unusable (I1).

**Linked parts / excerpts.** No excerpt is created and no existing part score changes
(masternotationparts.cpp:80-96 only removes and re-sorts). `MasterNotation` marks potential
excerpts dirty from the parts notifier (masternotation.cpp:91-103, 703-707). Creating a part
score is a separate feature (`IMasterNotation::setExcerpts`, masternotation.cpp:478-526),
outside this command.

**Crash and corruption risks.**
- I1 (master override).
- Copying the plugin's `EditPart::insertPart` gives misordered staves (I9).
- `index` honoured blindly with a non-custom `ScoreOrder` leaves the order flag saying
  "orchestral" while parts are out of order; the dialog would switch to a customized order
  (instrumentsonscorelistmodel.cpp:365-388). See OQ-3.

**Existing upstream tests.** `src/engraving/api/tests/score_tests.cpp:1308` (`insertPart`,
plugin path, single part, index 0) and `:1396` (`insertPartApi`). Nothing exercises
`NotationParts::setParts`; `src/notation/tests/` contains only mocks.

**Scenarios.**
- INS-ADD-01: add `flute` to a string quartet with no index; it goes to the woodwind
  position of the score order, with key signature and rests in every bar.
- INS-ADD-02: add `bb-clarinet` to a score in Eb major, concert pitch off; the clarinet staff
  shows F major (written) and its name is "Clarinet in B♭".
- INS-ADD-03: add a second `flute`; names become "Flute 2" for the new part only (existing
  part keeps "Flute", per `resolveNewInstrumentNumber`).
- INS-ADD-04: add `piano` at index 0; two staves with brace, master and existing part
  scores unchanged.
- INS-ADD-05: unknown id → `E_UNKNOWN_INSTRUMENT`, score unchanged.
- INS-ADD-06: add a part in a batch, then write notes to it in the same batch; one undo
  removes both.

**Open questions.** OQ-1, OQ-3.

---

### remove_part  ·  command://notation/remove_part

**UI entry point.**
Instruments panel delete → `LayoutPanelTreeModel::removeRows` (layoutpaneltreemodel.cpp:131)
shows a warning only when part scores exist (143-145, 1027-1041) → `RootTreeItem::removeChildren`
(roottreeitem.cpp:109-130) → `masterNotation()->notation()->parts()->removeParts(ids)` = the
master's parts object (`MasterNotation::notation()` returns itself, masternotation.cpp:127-130)
→ `MasterNotationParts::removeParts` (masternotationparts.cpp:102-115):
1. `startGlobalEdit` (**I1 hazard**).
2. `NotationParts::removeParts` on the master (notationparts.cpp:858-889):
   `endInteractionWithScore` (I5); `doRemoveParts` → `EditPart::removeParts` (editpart.cpp:428-439)
   → per part `Score::cmdRemovePart` (score.cpp:2387-2401): for each staff `cmdRemoveStaff`
   (2782-2788: `adjustBracketsDel`, `undoRemoveStaff`, edit.cpp:6482-6521, which removes the
   staff's spanners, calls `Measure::cmdRemoveStaves` on every measure and mmrest,
   measure.cpp:1170-1212, unlinks and pushes `RemoveStaff`), then `undoRemovePart`;
   `setBracketsAndBarlines`. Then virtual `onPartsRemoved` → master override
   (masternotationparts.cpp:237-256) → `MasterScore::deleteExcerpt` (excerpt.cpp:494-545) for
   every excerpt whose `initialPartId` is a removed part (unlinks its staves and elements,
   pushes `RemoveExcerpt`). Then a `SortStaves` with the current order (identity mapping,
   notationparts.cpp:872-880) and `setBracketsAndBarlines`.
3. For each excerpt notation (still including the deleted ones, I3):
   `parts->removeParts(ids)` = base `NotationParts::removeParts` on that excerpt score.
4. `endGlobalEdit` (commits).

Top-of-score system objects (tempo, rehearsal marks…) on a removed top staff are kept
unless the next staff is itself a system-object staff (`allowRemoveWhenRemovingStaves`,
src/engraving/dom/utils.cpp:1145-1173), and the remaining staves shift up
(`Measure::removeStaves`, measure.cpp:1122-1140; `Segment::removeStaff`, segment.cpp:636-651).

**Agent path.**
- Option A (I1): `masterImpl->NotationParts::removeParts(ids)` (the qualified call still
  dispatches `onPartsRemoved` virtually to the master override, so part scores are deleted),
  then `excerpt->notation()->parts()->removeParts(ids)` for each `IMasterNotation::excerpts()`
  entry. Exactly the override body minus the global edit.
- Option B: on `masterScore()`: `EditPart::removeParts(master, parts)`; `deleteExcerpt` for
  each `masterScore()->excerpts()` entry whose `initialPartId()` is in the set (copy of
  masternotationparts.cpp:237-256); then for every remaining excerpt score, collect parts with
  matching IDs and `EditPart::removeParts(excerptScore, thoseParts)`. The identity
  `SortStaves` of notationparts.cpp:872-880 can be reproduced for fidelity (OQ-4).

**Selection set-up.** None. End note input, clear selection (I5).

**Arguments.**
```json
{
  "type": "object",
  "required": ["parts"],
  "properties": {
    "parts": {"type": "array", "minItems": 1,
              "items": {"$ref": "#/$defs/partRef"}}
  },
  "$defs": {
    "partRef": {"oneOf": [
      {"type": "object", "required": ["id"], "properties": {"id": {"type": "string", "description": "Part::id() as decimal uint64"}}},
      {"type": "object", "required": ["index"], "properties": {"index": {"type": "integer", "minimum": 0}}},
      {"type": "object", "required": ["name"], "properties": {"name": {"type": "string", "description": "matches partName or plain long name; must be unique"}}}
    ]}
  }
}
```
Returns removed part IDs and the names of part scores deleted.

**Validation before mutation.**
- Part not found → `E_BAD_PART`; name matches several parts → `E_AMBIGUOUS`.
- Removing every part → `E_LAST_PART` (the picker refuses an empty list,
  selectinstrumentscenario.cpp:78-82; the panel allows it at root,
  layoutpaneltreemodel.cpp:945-956; behaviour of a zero-staff score is untested, OQ-5).
- Note input / text edit active → `E_BUSY`.
- Duplicate entries → de-duplicate (`parts(ids)` filters by score order, notationparts.cpp:214-225).

**Post-condition.** No part with those IDs in `masterScore()->parts()` nor in any
`masterScore()->excerpts()[i]->excerptScore()->parts()`; `nstaves()` dropped by the removed
staff count; each excerpt whose `initialPartId` was removed is gone from
`masterScore()->excerpts()`.

**Nested commands.** None inside the engraving calls. Master override unusable (I1).

**Linked parts / excerpts.** Part scores built from the removed part are deleted (undoable);
the part is removed from every other part score that contains it. A *custom* part score whose
only part is removed and whose `initialPartId` is a different or invalid ID survives with no
parts (OQ-5).

**Crash and corruption risks.**
- I1.
- Plugin-style removal on the master only leaves orphan unlinked staves in part scores (I9).
- If the user is viewing a part score that gets deleted, the tab's fate is decided after
  commit by `MasterNotation::updateExcerpts` (masternotation.cpp:608-650); not traced (OQ-6).

**Existing upstream tests.** `score_tests.cpp:569` (`removeParts`, plugin path, no excerpts),
`:731` (`removePartsApi`). No test removes a part that has a part score.

**Scenarios.**
- INS-REM-01: remove the viola from a quartet; three parts left, brackets intact, undo
  restores notes.
- INS-REM-02: remove a part that has a generated part score; the part score disappears;
  undo restores it with its contents and links.
- INS-REM-03: remove the top part of a score with tempo marks and rehearsal marks; they stay
  on the new top staff.
- INS-REM-04: remove a part that appears in a custom two-part part score; the custom score
  keeps the other part.
- INS-REM-05: remove the only part → `E_LAST_PART`.
- INS-REM-06: batch: remove part, then add_part, then undo once → original score.

**Open questions.** OQ-1, OQ-4, OQ-5, OQ-6.

---

### rename_part  ·  command://notation/rename_part

**UI entry point.**
Instruments panel → instrument settings popup → `InstrumentSettingsModel::setInstrumentName`
/ `setAbbreviature` (src/instrumentsscene/qml/MuseScore/InstrumentsScene/internal/instrumentsettingsmodel.cpp:98-116),
key tick `Part::MAIN_INSTRUMENT_TICK` (45), on the **current** notation's parts (173-177) →
`NotationParts::setInstrumentName` (notationparts.cpp:412-438) / `setInstrumentAbbreviature`
(440-465), not overridden by the master (I2) → `EditPart::setInstrumentName` /
`setInstrumentAbbreviature` (editpart.cpp:396-412) → `ChangeInstrumentLong` /
`ChangeInstrumentShort` (169-197) on that one `Part` at that tick.
Second UI route: Staff/Part Properties dialog (`EditStaff::applyPartProperties`,
editstaff.cpp:510-559), which validates with `Text::validateText` (516-520) and goes through
`replaceInstrument` (see set_transposition), resetting the part name.
Part-score title: Parts dialog → `ExcerptNotation::undoSetName` (excerptnotation.cpp:98-116)
→ `ChangeExcerptTitle`.

**Agent path.** `EditPart::setInstrumentName(score, part, Part::MAIN_INSTRUMENT_TICK, xmlName)`
and/or `EditPart::setInstrumentAbbreviature(...)` directly (Option B), or the
`NotationParts` methods on the chosen score's parts object (lock-safe, I2). Optional part-score
title: `excerptScore->undo(new ChangeExcerptTitle(excerpt, title))`.

**Selection set-up.** None.

**Arguments.**
```json
{
  "type": "object",
  "required": ["part"],
  "properties": {
    "part": {"$ref": "#/$defs/partRef"},
    "longName": {"type": "string", "description": "plain text; empty string removes the name"},
    "shortName": {"type": "string"},
    "scope": {"enum": ["score", "partScore", "all"], "default": "score",
              "description": "score = full score only (UI behaviour from the score view); partScore = the part score built from this part; all = both"},
    "partScoreTitle": {"type": "string", "description": "optional new Excerpt::name()"}
  }
}
```
At least one of `longName`, `shortName`, `partScoreTitle` is required.

**Validation before mutation.**
- Part not found / ambiguous → `E_BAD_PART` / `E_AMBIGUOUS`.
- Nothing to change → `E_INVALID_ARG`.
- `scope` includes `partScore` but no excerpt has `initialPartId == part id` → `E_NO_PART_SCORE`.
- Escape plain text to XML before storing (`XmlWriter::xmlString`, as `Part::setPlainLongName`
  does, part.cpp:564-567) — the panel stores raw text (instrumentsettingsmodel.cpp:98-104 →
  editpart.cpp:402), so "A & B" from the panel is stored unescaped (OQ-7).
- `partScoreTitle` empty → `E_EMPTY_TEXT`.

**Post-condition.** `part->instrument(MAIN_INSTRUMENT_TICK)->nameAsPlainText() == longName`
and `abbreviatureAsPlainText() == shortName` (instrument.cpp:1167-1180) in each targeted
score; `excerpt->name() == partScoreTitle`.
Note: `NotationParts::setInstrumentName` returns early when names are equal (427-429), so
the post-condition also holds for a no-op; an empty macro is discarded at commit (notes.md G1).

**Nested commands.** None.

**Linked parts / excerpts.** Not propagated: each score's `Part` is separate (I3). Renaming in
the full score leaves the part score's name as it was, which is the UI behaviour when editing
from the full-score view. `partName()` (mixer name) is not changed by this path (I8).

**Crash and corruption risks.** None found. Mid-score instrument changes have their own names
at their tick; this command addresses only the main instrument (tick -1).

**Existing upstream tests.** `score_tests.cpp:365` (`setInstrumentName`), `:404`
(`setInstrumentAbbreviature`), `:506` (`setInstrumentNameApi`).

**Scenarios.**
- INS-REN-01: rename "Violin 1" to "Violino I", short "Vl. I"; system brackets show the new
  names; part score unchanged.
- INS-REN-02: same with `scope: all`; part score shows the new names too.
- INS-REN-03: name with `&` and `<`; shown literally on the score, round-trips through save.
- INS-REN-04: rename the part-score title only.
- INS-REN-05: undo restores all three.

**Open questions.** OQ-7, OQ-8.

---

### set_instrument  ·  command://notation/set_instrument

**UI entry point.**
Three UI routes, two engraving behaviours:
1. Instruments panel "Replace instrument" → `PartTreeItem::replaceInstrument`
   (parttreeitem.cpp:236-254): key = `{partId, current instrumentId, MAIN_INSTRUMENT_TICK}`;
   new staff type = `templ.staffTypePreset` or `StaffType::getDefaultPreset(templ.staffGroup)`
   → `MasterNotationParts::replaceInstrument(key, instrument, staffType)`.
2. Staff/Part Properties "Change instrument…" (`EditStaff::showReplaceInstrumentDialog`,
   editstaff.cpp:572-588, then `applyPartProperties` 555-556) → master `replaceInstrument`
   with no staff type argument.
3. Context menu `change-instrument` on an **instrument-change** marking
   (`InstrumentsActionsController::changeInstrument`, instrumentsactionscontroller.cpp:62-95):
   key tick = the marking's tick → master `replaceInstrument(key, instrument)`.

`MasterNotationParts::replaceInstrument` (masternotationparts.cpp:186-220):
`startGlobalEdit` (**I1**); `part = partModifiable(id)`; `isMainInstrument` (194;
`isMainInstrumentForPart`, notationtypes.h:295-298: key id == part's id **and** tick == -1);
`oldTranspose` (196); base `replaceInstrument` on master (198); same on every excerpt
(200-202); `Transpose::transpositionChanged(score(), part, MAIN_INSTRUMENT_TICK, oldTranspose)`
(205); if main instrument and a part score has `initialPartId == part`, retitle it to
`formatUniqueExcerptName(part->partName(), allNames)` (207-217); `endGlobalEdit`.

Base `NotationParts::replaceInstrument` (notationparts.cpp:689-713):
- main instrument → `EditPart::replacePartInstrument(score, part, newInstrument, newStaffType,
  formatInstrumentTitle(trackName, trait))` (701-702; editpart.cpp:306-347):
  `ChangePart` (instrument at tick -1 + part name, editpart.cpp:136-157);
  per **existing** staff, `ChangeStaff` if the default clef differs (318-340) and
  `ChangeStaffType` if a staff type was given (343-345).
- otherwise → `EditPart::replaceInstrumentAtTick` (349-364): find the `InstrumentChange`
  annotation at that tick (295-304), `setInit(true)`, `setupInstrument(&newInstrument)`
  (instrchange.cpp:89-157: clef changes via `undoChangeClef`, key signature for a
  non-octave transposition change via `undoChangeKeySig`, `ChangeInstrument` for every linked
  copy, `transpositionChanged` for the range, marking text "To <trackName>").

`Transpose::transpositionChanged(score, part, tick, oldV)` (transpose.cpp:474-485 → 403-472):
re-spells key signatures in every score linked through `part->staff(0)` when concert pitch is
off (`transposeKeys`, 301-361), rewrites every note's `TPC2` (written spelling) from `TPC1`
(448-455, linked copies follow via `undoChangeProperty`), and transposes chord symbols per
linked score (457-468). Concert pitch (`Note::pitch`) is never changed: the music keeps
sounding the same and the written notes move.

Adding a **new** mid-score instrument change (palette drop or text menu) is a different path:
`Score::addText(TextStyleType::INSTRUMENT_CHANGE, chordRest)` (edit.cpp:845-853) →
`undoAddAnnotation`; `undoAddElement` sets the copy's instrument to the current one and
transposes linked scores when needed (edit.cpp:7315-7338); then the UI's
`NotationInteraction::selectInstrument` (notationinteraction.cpp:2246-2282) calls
`setInit(true)`, `setupInstrument(&newInstrument)` and `cleanupDrumsetChanges` (2284-2307).
The `QEventLoop` picker is the only dialog; the engraving calls around it are dialog-free.

**Agent path.**
- Whole part (main instrument): Option A = masternotationparts.cpp:193-217 without the
  global edit (qualified `NotationParts::replaceInstrument` on master, `replaceInstrument`
  on each excerpt's parts, `transpositionChanged`, excerpt retitle). Option B = the same with
  `EditPart::replacePartInstrument(score, part, inst, staffType, formatInstrumentTitle(...))`
  per score where `partById` finds the part (`formatInstrumentTitle` is public,
  notationtypes.h:300).
- Existing instrument change at `at`: `EditPart::replaceInstrumentAtTick(master, part, tick, inst)`.
  The master override additionally runs it per excerpt and runs the main-range
  `transpositionChanged`; with the master's `setupInstrument` already updating linked copies
  (instrchange.cpp:134-139) and the transposition (141-151), those repeats are idempotent
  by construction (old == new on the second pass); OQ-9.
- New instrument change at `at`: the addText sequence above, master score only
  (`undoAddElement` replicates to linked staves).

**Selection set-up.** None.

**Arguments.**
```json
{
  "type": "object",
  "required": ["part", "instrument"],
  "properties": {
    "part": {"$ref": "#/$defs/partRef"},
    "instrument": {"type": "string", "description": "instruments.xml id"},
    "at": {"type": "object", "description": "omit for the whole part; {measure, beat|tick} for a mid-score instrument change"},
    "staffType": {"enum": ["template", "keep"], "default": "template",
                  "description": "template = Instruments-panel behaviour (template preset); keep = Staff Properties behaviour"}
  }
}
```

**Validation before mutation.**
- Part not found / ambiguous → `E_BAD_PART` / `E_AMBIGUOUS`. Required: the master override
  dereferences `part` unconditionally in `transpositionChanged` (masternotationparts.cpp:193-205)
  — a bad ID crashes it.
- Unknown template → `E_UNKNOWN_INSTRUMENT` (I7).
- `at` given: no ChordRest at that position → `E_NO_CHORDREST`; `at` at tick 0 → refuse
  (`E_RANGE`, use whole-part mode).
- Template staff count differs from the part's → report `staffCountMismatch` in the result:
  `replacePartInstrument` never adds or removes staves (editpart.cpp:318), so piano → flute
  keeps two staves and flute → piano keeps one. Whether to refuse is OQ-10.
- Pitched ↔ drumset change: notes keep MIDI pitch and are re-read as drum pitches; report it
  (OQ-10).

**Post-condition.** Whole part: `part->instrumentId() == instrument` in master and each
excerpt containing the part; `part->partName() == formatInstrumentTitle(...)`; each staff's
`staffType(0)->type()` equals the preset (template mode); for each note in the part's main
range, `tpc2 == transposeTpc(tpc1, -newTranspose)`. Mid-score: the `InstrumentChange` at `at`
has `instrument()->id() == instrument` and `part->instrument(tick)->id() == instrument`.

**Nested commands.** No `startCmd/endCmd` below the override. `setupInstrument` calls
`undoChangeClef`/`undoChangeKeySig`, which push into the active macro.

**Linked parts / excerpts.** Propagated: the override replaces the instrument in each part
score (masternotationparts.cpp:200-202) and transposition follows links (I3). The part-score
title is rewritten (207-217): `formatUniqueExcerptName` includes the part score's own current
name in the "taken" list (utils.cpp:1449-1459), so when the new part name equals the current
title the title becomes "<name> (1)" (OQ-11).

**Crash and corruption risks.**
- I1; null part (above).
- Staff count not adapted (above).
- `ChangeStaff` changes only the default clef; an explicit clef element already at tick 0
  keeps winning (`Staff::clefType`, staff.cpp:580-583).
- Plugin `replaceInstrument` skips transposition and part scores (I9).

**Existing upstream tests.** `score_tests.cpp:56` (`replaceInstrumentAtDomLevel`), `:99`
(`replaceInstrumentUndo`), `:142` (`replaceInstrumentRedo`), `:184`, `:220`, `:237`
(plugin path). `src/engraving/tests/instrumentchange_tests.cpp:68-132` covers adding and
changing `InstrumentChange` elements with `undoAddElement`/`ChangeInstrument`, not
`setupInstrument`; fixtures `instrumentchange_data/*.mscx` are reusable.

**Scenarios.**
- INS-SET-01: flute → oboe; name "Oboe", same staff, notes unchanged.
- INS-SET-02: flute → `bb-clarinet` in C major, concert pitch off; written notes up a tone,
  key signature D major, sounding pitch unchanged; part score follows.
- INS-SET-03: guitar (TAB staff) → piano with `staffType: template`; staff becomes standard;
  report says staff count 1 vs template 2.
- INS-SET-04: mid-score change at bar 9 of a clarinet part to `a-clarinet`; key signature and
  written notes change from bar 9 only.
- INS-SET-05: change an existing instrument change marking at bar 9 to `flute`.
- INS-SET-06: part with a part score titled "Flute"; flute → flute (same id) → title check
  for the "(1)" suffix (OQ-11).

**Open questions.** OQ-1, OQ-9, OQ-10, OQ-11.

---

### add_staff  ·  command://notation/add_staff

**UI entry point.**
- Unlinked staff: Instruments panel "Add staff" row → `StaffControlTreeItem::appendNewItem`
  (staffcontroltreeitem.cpp:41-54): `Factory::createStaff(part)`,
  `setDefaultClefType(part->instrument()->clefType(nstaves))` → `MasterNotationParts::appendStaff`
  (masternotationparts.cpp:132-157): `startGlobalEdit` (**I1**); `staff->setId(INVALID_ID)`;
  base `NotationParts::appendStaff` (notationparts.cpp:580-598) → `doAppendStaff`
  (904-921): copy the previous staff's key list, `undoInsertStaff(staff, localIndex, true)`
  (rests in every bar), `adjustKeySigs`, `updateBracesAndBarlines` (score.cpp:3943-3982),
  `setBracketsAndBarlines`, `instrument()->setClefType(localIndex, …)`. Then per excerpt:
  `excerptStaff = staff->clone()` (copies the new ID, staff.cpp:93-96, 1357-1359; no links,
  engravingobject.cpp:89) → excerpt `appendStaffLinkedToMaster` (notationparts.cpp:600-621:
  returns false if the part is not in that excerpt; else `doAppendStaff(createRests=false)`,
  `Link(staff, master)`, `Excerpt::cloneStaff2`) → on false `undoUnlink` + `delete`.
  `endGlobalEdit`.
- Linked staff (e.g. TAB under standard): staff settings popup "Create linked staff" →
  `StaffSettingsModel::createLinkedStaff` (staffsettingsmodel.cpp:285-301):
  `sourceStaff->clone()` → `MasterNotationParts::appendLinkedStaff` (159-184) → base
  `appendLinkedStaff` (notationparts.cpp:623-648: `doAppendStaff(createRests=false)`,
  `setLinks(nullptr)`, `Excerpt::cloneStaff(source, staff)`), then the same on each excerpt.

**Agent path.** Option A = the override bodies without the global edit (base calls on the
master, `appendStaffLinkedToMaster` / `appendLinkedStaff` on each excerpt's parts, which are
plain `NotationParts`; `appendStaffLinkedToMaster` calls `startEdit` and never `apply`, fine
under the lock). Option B = `EditPart::doAppendStaff` (editpart.cpp:645-665) + `setBracketsAndBarlines`
on the master, then per excerpt the clone + `doAppendStaff(false)` + `Link` + `cloneStaff2`
sequence; linked: `EditPart::appendLinkedStaff` (678-691) per score.

**Selection set-up.** None.

**Arguments.**
```json
{
  "type": "object",
  "required": ["part"],
  "properties": {
    "part": {"$ref": "#/$defs/partRef"},
    "linkedTo": {"type": "integer", "minimum": 0, "description": "0-based staff index within the part to link to; omit for an independent empty staff"},
    "clef": {"type": "string", "description": "optional ClefType name for an unlinked staff; default = instrument's clef for that index"},
    "staffType": {"type": "string", "description": "optional preset for a linked staff, applied afterwards with set_staff_type"}
  }
}
```

**Validation before mutation.**
- Part not found → `E_BAD_PART`; `linkedTo` out of range → `E_RANGE`.
- Part with zero staves → `E_INVALID_ARG` (`doAppendStaff` reads `staff(nstaves-1)`,
  notationparts.cpp:906-907; `Part::staff` returns null out of range, part.cpp:92-99).
- Upper bound on staves per part: none in the UI path (OQ-12).

**Post-condition.** `part->nstaves()` +1 in master and in every excerpt containing the part;
the new staff is last in the part; linked: `newStaff->isLinked(source)` and element counts
match; unlinked: every measure has a full-measure rest in voice 1 on the new staff.

**Nested commands.** None in the engraving calls.

**Linked parts / excerpts.** Propagated by the explicit excerpt loop only; the excerpt staff
is linked to the master's new staff.

**Crash and corruption risks.**
- I1.
- `instrument()->setClefType(index, …)` is a direct, non-undoable change to the instrument's
  clef list (notationparts.cpp:920; editpart.cpp:664); after undo the instrument keeps the
  extra entry (`Instrument::setClefType`, instrument.cpp:1009-1015). Effect: a later add_staff
  or instrument replacement reads that clef. Low impact.
- Plugin `appendStaff` skips part scores (I9).

**Existing upstream tests.** `score_tests.cpp:1027` (`appendStaff`), `:1061`
(`appendLinkedStaff`), `:1096`, `:1126` (plugin path, no excerpts).

**Scenarios.**
- INS-STF-01: add a third staff to piano; rests in all bars, brace spans three staves.
- INS-STF-02: add a staff to a part that has a part score; part score gains the linked staff.
- INS-STF-03: add a linked staff to guitar, then set_staff_type TAB_6COMMON on it; notes
  appear as frets.
- INS-STF-04: undo restores staff count in score and part.

**Open questions.** OQ-1, OQ-12.

---

### set_staff_type  ·  command://notation/set_staff_type

**UI entry point.** Staff settings popup → `StaffSettingsModel::setStaffType`
(staffsettingsmodel.cpp:138-156) on the **current** notation's parts (303-310) →
`NotationParts::setStaffType` (notationparts.cpp:534-556), not overridden (I2) →
`EditPart::setStaffType` (editpart.cpp:414-426) → `ChangeStaffType(staff, preset)`.
`ChangeStaffType::flip` (editstaff.cpp:312-336) replaces the staff type **at tick 0 only**,
toggles staff-line visibility when `invisible` changes, and on TAB → standard calls
`GuitarBend::adaptBendsFromTabToStandardStaff` (guitarbend.cpp:1023-1062), which sets note
visibility directly (not undoable; layout re-hides them on undo).
The popup offers only allowed types (`allStaffTypes`, staffsettingsmodel.cpp:79-126):
STANDARD always; PERCUSSION only if `instrument->useDrumset()`; TAB only if
`type.lines() <= stringData()->frettedStrings()`.

What happens to existing notes: nothing at edit time. On a TAB staff, layout calls
`ModifyDom::cmdUpdateNotes` (measurelayout.cpp:999; modifydom.cpp:81-127) →
`Chord::cmdUpdateNotes` (chord.cpp:1138-1152, TAB branch of 1138-1224) → `StringData::fretChords` (stringdata.cpp:175 onward),
which assigns string/fret with `undoChangeProperty` and marks unplayable notes as fret
conflicts. Layout runs in `endCmd` before the macro closes (cmd.cpp:426 then 436), so those
changes land in the same undo step. Clefs follow the staff group automatically when there is
no explicit clef (`Staff::clefType`, staff.cpp:580-605; percussion clef only for drumset
instruments, 586-588). Later `StaffTypeChange` elements keep overriding from their tick.

**Agent path.** `EditPart::setStaffType(score, staff, typeId)` (Option B) or
`parts->setStaffType(staffId, type)` on the chosen score (lock-safe, I2).

**Selection set-up.** None.

**Arguments.**
```json
{
  "type": "object",
  "required": ["part", "staff", "type"],
  "properties": {
    "part": {"$ref": "#/$defs/partRef"},
    "staff": {"type": "integer", "minimum": 0, "description": "0-based index within the part"},
    "type": {"enum": ["STANDARD","PERC_1LINE","PERC_2LINE","PERC_3LINE","PERC_5LINE",
                      "TAB_6SIMPLE","TAB_6COMMON","TAB_6FULL","TAB_4SIMPLE","TAB_4COMMON","TAB_4FULL",
                      "TAB_5SIMPLE","TAB_5COMMON","TAB_5FULL","TAB_UKULELE","TAB_BALALAJKA","TAB_DULCIMER",
                      "TAB_ITALIAN","TAB_FRENCH","TAB_7COMMON","TAB_8COMMON","TAB_9COMMON","TAB_10COMMON",
                      "TAB_7SIMPLE","TAB_8SIMPLE","TAB_9SIMPLE","TAB_10SIMPLE"]},
    "scope": {"enum": ["score", "partScore"], "default": "score"}
  }
}
```
Names are `enum class StaffTypes` (src/engraving/dom/stafftype.h:120-134).

**Validation before mutation.**
- Part/staff not found → `E_BAD_PART` / `E_BAD_STAFF`.
- Type id outside `[0, STAFF_TYPES)` → `E_INVALID_ARG` (`StaffType::preset` silently maps it to
  STANDARD, stafftype.cpp:1087-1094; the `!staffType` check in notationparts.cpp:541 never fires).
- Type not allowed by the UI rule above → `E_STAFF_TYPE_NOT_ALLOWED`.
- No-op: compare `staff->staffType(0)->type()` with the request; the UI's check compares
  pointers (notationparts.cpp:545) and never matches.

**Post-condition.** `staff->staffType(Fraction(0,1))->type() == type`.

**Nested commands.** None.

**Linked parts / excerpts.** Not propagated: one `Staff` object per call (I3); the linked staff
in a part score keeps its type (this is how a part score can show TAB while the score shows
standard notation).

**Crash and corruption risks.** Out-of-range id → silent STANDARD (above). TAB with notes out
of the instrument's range gives fret conflicts, not errors. Bend visibility on TAB → standard
is not undoable (above).

**Existing upstream tests.** `score_tests.cpp:443` (`setStaffType`), `:539` (`setStaffTypeApi`).
`src/engraving/tests/parts_tests.cpp:1488` (`staffStyles`) is compiled out (`#if 0`, 1487).

**Scenarios.**
- INS-STT-01: guitar standard staff → TAB_6COMMON; frets assigned, undo restores standard.
- INS-STT-02: flute → TAB_6COMMON → `E_STAFF_TYPE_NOT_ALLOWED`.
- INS-STT-03: drumset staff PERC_5LINE → PERC_1LINE.
- INS-STT-04: guitar part score staff → TAB with `scope: partScore`; full score unchanged.

**Open questions.** OQ-13.

---

### set_transposition  ·  command://notation/set_transposition

**UI entry point.** Staff/Part Properties dialog (`staff-properties` action,
src/notationscene/internal/notationactioncontroller.cpp:314) → `EditStaff::apply` (editstaff.cpp:316)
→ `applyPartProperties` (510-570): builds `interval` from `Interval::allIntervals[index]`
(src/engraving/dom/interval.cpp:124-160) + octaves, flipped when "down" (526-535);
`m_instrument.setTranspose(interval)` plus ranges and names (537-553); since the id is
unchanged it calls `notationParts()->replaceInstrument(m_instrumentKey, m_instrument)` (557-558)
on the **current** notation: the master → `MasterNotationParts::replaceInstrument`
(**I1**; full behaviour in set_instrument), a part score → base `NotationParts::replaceInstrument`,
which changes that part score's instrument only and runs **no** `transpositionChanged`.
Then, if the interval is unison or octave, prefer-sharps/flats is forced to NONE and
`setPartSharpFlat` runs (561-569; editpart.cpp:384-394, which re-runs `transpositionChanged`).
`m_instrumentKey.tick` is the key of the instrument in effect at the clicked tick (130-132), so
from inside a mid-score instrument range the dialog edits that instrument change.

There is no narrower undo command for the interval: `ChangePart` replaces the whole
`Instrument` object (editpart.cpp:136-157).

**Agent path.** Whole part: `Instrument inst = *part->instrument(MAIN_INSTRUMENT_TICK);
inst.setTranspose(interval);` then the set_instrument whole-part sequence with
`newStaffType = nullptr`, including `Transpose::transpositionChanged(master, part,
MAIN_INSTRUMENT_TICK, oldTranspose)`. Mid-score: `EditPart::replaceInstrumentAtTick` with the
modified copy of the instrument at that tick.
UI side effects the agent may choose not to copy (OQ-11): `replacePartInstrument` resets
`partName` to `formatInstrumentTitle(trackName, trait)` (notationparts.cpp:701), and the master
retitles the part score (masternotationparts.cpp:207-217). Passing the current
`part->partName()` and skipping the retitle uses the same engraving calls.

**Selection set-up.** None.

**Arguments.**
```json
{
  "type": "object",
  "required": ["part", "interval"],
  "properties": {
    "part": {"$ref": "#/$defs/partRef"},
    "interval": {"type": "object", "required": ["diatonic", "chromatic"],
                 "properties": {"diatonic": {"type": "integer"}, "chromatic": {"type": "integer"}},
                 "description": "written → sounding, as stored in Instrument::transpose(); Bb clarinet = {-1,-2}"},
    "at": {"type": "object", "description": "optional {measure, beat|tick} of an existing instrument change"},
    "preferSharpFlat": {"enum": ["auto", "none", "sharps", "flats"]}
  }
}
```

**Validation before mutation.**
- Part not found → `E_BAD_PART`.
- Interval consistency: `|chromatic| ≤ 127`; `diatonic mod 7` and `chromatic mod 12` must be one
  of the 26 pairs in `Interval::allIntervals` (interval.cpp:124-160) with matching octave count
  and sign, which is the set the dialog can express → else `E_INVALID_ARG`.
- `at` given and no `InstrumentChange` there → `E_NO_INSTRUMENT_CHANGE`
  (`replaceInstrumentAtTick` returns false, editpart.cpp:356-359).
- Percussion staves are skipped by `transpositionChanged` (transpose.cpp:437-439); a drumset
  part → `E_INVALID_ARG`.

**Post-condition.** `part->instrument(tick)->transpose() == interval` in master and in each
excerpt containing the part; for each note in range `tpc2 == transposeTpc(tpc1, flip(interval))`;
with concert pitch off, each non-generated key signature in range has
`key == transposeKey(concertKey, flip(interval))` (transpose.cpp:326-337).

**Nested commands.** None.

**Linked parts / excerpts.** Master path propagates (excerpt loop + links). Running it on a
part score alone, as the dialog does from a part view, leaves the score's instrument and
spelling unchanged; the agent should always run the master path.

**Crash and corruption risks.** I1; null part in the master override (set_instrument).
Concert pitch: in concert-pitch display the change is invisible until concert pitch is
turned off; `transposeKeys` is skipped while it is on (transpose.cpp:420-422), and whether
toggling concert pitch afterwards re-derives key signatures is not traced (OQ-14).

**Existing upstream tests.** None through instrument replacement.
`src/engraving/tests/keysig_tests.cpp:149` calls `Transpose::transpositionChanged` directly on a
range; its fixtures (`keysig_data/`) are the nearest reuse.

**Scenarios.**
- INS-TRN-01: horn part from F (-4,-7) to E♭ (-5,-9); written notes and key signatures
  move, sounding unchanged; part score follows.
- INS-TRN-02: set a C instrument to octave-down (-7,-12); key signature unchanged
  ((v - oldV) % 12 == 0), notes written an octave higher.
- INS-TRN-03: interval {1,5} (inconsistent) → `E_INVALID_ARG`.
- INS-TRN-04: concert pitch on: change succeeds, display unchanged; toggle concert pitch off
  and check key signatures.
- INS-TRN-05: undo restores interval, spelling and key signatures in score and part.

**Open questions.** OQ-1, OQ-11, OQ-14.

---

## Open questions

- **OQ-1 (core).** Option A (qualified calls into `NotationParts`/excerpt parts, needs the
  internal headers `src/notation/internal/notationparts.h` and `masternotationparts.h`) or
  Option B (engraving calls, copying two file-static helpers for add_part). Both avoid I1;
  A tracks upstream changes to the panel automatically, B keeps notifications out of the
  transaction. Conductor's decision.
- **OQ-2 (core).** Whether to propose an upstream fix for I1 (make `startGlobalEdit` a no-op
  when already locked). Not needed for the agent if OQ-1 is followed.
- **OQ-3.** add_part with explicit `index` against a non-custom `ScoreOrder`: copy the dialog's
  switch to a customized order (instrumentsonscorelistmodel.cpp:365-388) or leave the order
  as is. The engraving effect of a stale non-custom order on `setBracketsAndBarlines`
  (scoreorder.cpp:380 onward) was not traced.
- **OQ-4.** remove_part Option B: reproduce the identity `SortStaves` (notationparts.cpp:872-880)
  or skip it. `Score::sortStaves` (score.cpp:2868 onward) also rebuilds systems and moves system
  objects; whether an identity mapping has any visible effect was not determined.
- **OQ-5.** Behaviour of a score or custom part score left with zero parts; not exercised by
  any test found.
- **OQ-6.** What the UI does with an open tab whose part score is deleted by remove_part
  (after commit, `MasterNotation::updateExcerpts`, masternotation.cpp:608-650). Not traced
  into the tab/view code.
- **OQ-7.** Instrument names from the Instruments panel are stored unescaped as XML text
  (instrumentsettingsmodel.cpp:98-104 → editpart.cpp:402). Confirm the agent should escape
  (proposed) even though the panel does not.
- **OQ-8.** rename_part default scope: full score only (UI behaviour) or score plus its part
  score.
- **OQ-9.** Mid-score `replaceInstrumentAtTick` per excerpt after the master already updated
  linked copies: argued idempotent from instrchange.cpp:100-151; confirm with INS-SET-05.
- **OQ-10.** set_instrument when the template's staff count differs, or pitched ↔ drumset:
  allow and report (UI behaviour) or refuse.
- **OQ-11.** `MasterNotationParts::replaceInstrument` retitles the part score with
  `formatUniqueExcerptName(partName, names)` where `names` includes the score's own current
  title (masternotationparts.cpp:209-215; utils.cpp:1449-1459). Read from source only: changing
  transposition in Staff/Part Properties on a part whose part score is titled with the part
  name would retitle it "<name> (1)". Confirm with INS-SET-06 and decide whether the agent
  copies this retitle and the part-name reset.
- **OQ-12.** No maximum staves per part in the add-staff UI path; `initStaff` indexes template
  arrays by `MAX_STAVES` (notationparts.cpp:1090-1098) but `doAppendStaff` does not. Whether to
  cap add_staff.
- **OQ-13.** set_staff_type on a staff that has `StaffTypeChange` elements: only tick 0 changes
  (editstaff.cpp:314-316). Report or refuse?
- **OQ-14.** Whether toggling concert pitch after a transposition change made in concert-pitch
  mode re-derives written key signatures (concert-pitch toggle path not traced).

## Corrections to COMMANDS.md, assessment.md and the brief

1. assessment.md ("Instrumentation … INotationParts", and CLAUDE.md rule 4 "prefer
   INotationParts"): the master's `INotationParts` mutators for add, remove, replace and
   add-staff lock and unlock the undo stack themselves and commit through `endCmd`
   (I1), so they cannot run inside an agent transaction. The non-overridden ones
   (rename, staff type, staff config) are safe.
2. assessment.md Phase 3 bullet lists "add_part, remove_part, set_instrument, add_staff,
   set_transposition" and omits rename_part and set_staff_type, which COMMANDS.md includes.
3. assessment.md risk scenario "removing a part that other parts' chord symbols depend on":
   chord symbols are per-staff annotations and are removed with their staff
   (measure.cpp:1184-1191); no other part depends on them. The real cross-part dependency
   on removal is top-of-score system objects (tempo, rehearsal marks, repeats/jumps),
   which the engine keeps on the next staff (utils.cpp:1145-1173). INS-REM-03 covers it.
4. The brief's "creating a part does not create an excerpt" is correct
   (masternotationparts.cpp:80-96). The converse needs stating: removing a part **does**
   delete the part scores built from it (masternotationparts.cpp:237-256).
