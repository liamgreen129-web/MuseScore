# Spec sheet: `expression-lines`

Group: `add_slur`, `add_line` (kinds: pedal, ottava, trill, volta, text line).
Source: MuseScore Studio v4.7.6 (`agent/4.7` working tree). Every `file:line` below is
in that tree. Written by `api-scout` for P0.S.

Path correction for readers of the scout brief: the action controller and UI action
table are in `src/notationscene/internal/`, not `src/notation/internal/`
(`src/notationscene/internal/notationactioncontroller.cpp`,
`src/notationscene/internal/notationuiactions.cpp`).

---

## Shared facts (both commands)

These hold for every path in this group; the per-command sections refer back here.

**F1. Transaction wrappers are no-ops under the lock, but notifications still fire.**
`NotationInteraction::startEdit` calls `m_undoStack->prepareChanges`
(`src/notation/internal/notationinteraction.cpp:314-319`); `apply()` calls
`commitChanges()` and then notifies `dropChanged` or `notationChanged`
(`notationinteraction.cpp:321-330`). `NotationUndoStack::prepareChanges` /
`commitChanges` / `rollbackChanges` return early when the stack is locked
(`src/notation/internal/notationundostack.cpp:106-117`, `119-130`, `132-143`), and
`Score::startCmd` / `endCmd` also return early when locked
(`src/engraving/editing/cmd.cpp:344-349`, `406-411`). So every call in this group is
safe inside the core's `prepareChanges → lock → … → unlock → commitChanges`, and lands
in the outer macro. The notification inside `apply()` is *not* suppressed by the lock:
`notationChanged`/`dropChanged` fire mid-transaction (see Open questions Q1).

**F2. No layout happens between the edit and the commit.** Layout runs from
`Score::endCmd` → `update()` (`cmd.cpp:426`), which is skipped under the lock. New
spanners therefore have no layout segments until the core commits, except the single
placeholder `SlurTieSegment` that `Score::addSlur` attaches to the master slur
(`src/engraving/editing/edit.cpp:751-756`). Post-conditions must be checked on the
spanner model (`tick`, `tick2`, `track`, `track2`, start/end element, the spanner map),
never on segments or systems. `SpannerMap::findOverlapping` rebuilds its tree when
dirty (`src/engraving/dom/spannermap.cpp:84-88`, `153-157`), so it is safe to query
mid-transaction.

**F3. Spanners across systems and barlines need nothing at add time.** None of the
add paths look at systems; a spanner is just `tick`/`tick2`/`track`/`track2`
(e.g. `cmd.cpp:603-633`, `edit.cpp:736-750`). Splitting into per-system segments is
done by layout after commit. The only barline-aware add logic is the partial
(incoming/outgoing) slur path, which needs a barline in the selection
(`notationinteraction.cpp:2968-3014`) and is out of scope here.

**F4. Linked staves and excerpts.** All types in this group are added through
`Score::undoAddElement` with `addToLinkedStaves = true`:
- Slur, ottava, trill, pedal, staff text line take the per-staff branch: clone to every
  staff in `ostaff->staffList()` (`edit.cpp:6906-6948`, `6963-6968`), with
  `VOICE1_COPY_TYPES` (`edit.cpp:6973-6998`) letting a voice-1 item reach a linked staff
  that lacks the voice mapping (`edit.cpp:7000-7007`), then the spanner block sets
  `track`/`track2` on the clone and, for slurs, re-points start/end elements to the
  linked chords (`edit.cpp:7188-7232`).
- Volta and system text line (`isSystemTextLine` = text-line-base with SYSTEM flag,
  `src/engraving/dom/textlinebase.h:160-163`; Volta is constructed with
  `ElementFlag::SYSTEM`, `src/engraving/dom/volta.cpp:105-106`) take the system-object
  branch: one copy on `staff(0)` of every score in `scoreList()` plus each score's
  `systemObjectStaves()` (`edit.cpp:6704-6731`, `6762-6769`).
- Passing `ctrlModifier = true` for a system line instead copies it to the linked
  staves of the one staff and **clears its SYSTEM flag** (`edit.cpp:6719-6721`). The
  agent must always pass no modifiers.

**F5. `applyPaletteElement` must not be called as-is from the agent.**
`NotationInteraction::applyPaletteElement` ends with `checkAndShowError()`
(`notationinteraction.cpp:2342`), which calls
`MScoreErrorsController::checkAndShowMScoreError()`
(`notationinteraction.cpp:8456-8462`). If `MScore::_error` is set, that function
**resets the error** (`src/notation/internal/mscoreerrorscontroller.cpp:43`) and opens
an `interactive()->info` dialog (`mscoreerrorscontroller.cpp:189-196`). Under the lock
`startCmd` never runs, so it never clears a stale error (`cmd.cpp:359` is skipped by
`cmd.cpp:346-348`); an error set by an earlier step of the same batch would both pop a
dialog (forbidden by CLAUDE.md rule 4) and be erased before `endCmd` can turn it into a
rollback (`cmd.cpp:418-420`). Either the core guarantees `MScore::_error ==
MS_NO_ERROR` immediately before the call, or the command reproduces the palette's
range branch directly on `Score` (the "drop to Score where the UI does the same"
allowance). The calls that branch makes are listed exactly under `add_line`.

**F6. Selection set-up primitives (all on `INotationInteraction`).**
- `clearSelection()` (`notationinteraction.cpp:1122-1140`) must come first: if a range
  is already selected, `select(…, RANGE)` *extends* it instead of replacing it
  (`src/engraving/dom/score.cpp:3571-3589`).
- List selection: `select({a, b}, SelectType::REPLACE)` → `deselectAll`, then
  `selectAdd` each (`notationinteraction.cpp:1023-1026`, `score.cpp:3349-3354`,
  `3434-3462`). One element gives `isSingle()` (`src/engraving/dom/select.h:120`), two
  or more give a list. Use `REPLACE`, never a single-element `ADD`: `doSelect` toggles
  the element off when the user happens to be holding Ctrl
  (`notationinteraction.cpp:1011-1021`).
- Range selection: `select({crStart}, SelectType::RANGE, staffIdx)` on an empty
  selection creates `[crStart.segment, crStart.nextSegmentAfterCR(...))`
  (`score.cpp:3591-3604`); a second `select({crEnd}, SelectType::RANGE, staffIdx)`
  extends to `crEnd.nextSegmentAfterCR(ChordRest|EndBarLine|Clef)` and the staff span
  (`score.cpp:3571-3589`, `src/engraving/dom/select.cpp:1669-1702`,
  `src/engraving/dom/chordrest.cpp:811-820`). Selecting a `Measure` with `RANGE`
  selects the whole measure on that staff, or the covering multimeasure rest
  (`score.cpp:3548-3568`).
- If the end CR's `nextSegmentAfterCR` returns null, the extend branch is skipped and a
  fresh one-CR range replaces the selection (`score.cpp:3578`, then `3591-3604`). The
  core must check `selection()->range()` after set-up.
- Range selections build their element list through the user's selection filter
  (`select.cpp:502`, `556`, `829`); the slur range path reads that list (below).

**F7. Note-input mode changes behaviour.** `NotationActionController::addSlur` resets
the pending slur instead of adding one when note input is active with a slur pending
(`src/notationscene/internal/notationactioncontroller.cpp:1406-1423`);
`doAddSlur` hands the new slur to note input instead of finishing
(`notationinteraction.cpp:3031-3032`); `cmdAddOttava` skips selecting the new ottava in
note-entry mode (`edit.cpp:2370-2372`). The commands below assume normal mode.

**F8. No upstream tests exercise these add paths.** `git grep` finds no gtest calling
`addSlur`, `addSlurToSelection`, `doAddSlur`, `cmdAddSpanner`, `cmdAddOttava`,
`addOttavaToSelection` or `applyPaletteElement` in `src/engraving/tests` or
`src/notation/tests` (the latter has only mocks). The closest coverage is listed
per command.

---

### add_slur  ·  command://notation/add_slur

**UI entry point.**
- Action `add-slur` (menu Add › Lines, note-input toolbar; shortcut S):
  `UiAction("add-slur", …)` at `src/notationscene/internal/notationuiactions.cpp:2443-2449`,
  registered at `notationactioncontroller.cpp:238` → `NotationActionController::addSlur`
  (`notationactioncontroller.cpp:1406-1423`) → `INotationInteraction::addSlurToSelection`
  (`src/notation/inotationinteraction.h:203`) →
  `NotationInteraction::addSlurToSelection` (`notationinteraction.cpp:5505-5513`) →
  `doAddSlur(nullptr)` (`notationinteraction.cpp:2877-2966`) →
  `doAddSlur(first, second, template)` (`notationinteraction.cpp:2968-3037`) →
  `Score::addSlur` (`src/engraving/editing/edit.cpp:698-759`) →
  `Score::undoAddElement` (`edit.cpp:6692`).
- Palette (Lines › Slur, `src/palette/internal/palettecreator.cpp:1138-1139`):
  `applyPaletteElement` → `applyPaletteElementToList`/`ToRange` both call
  `doAddSlur(toSlur(element))` (`notationinteraction.cpp:2453-2456`, `2641-2644`),
  that is, the same function with a template. Prefer `addSlurToSelection` (no
  `checkAndShowError`, see F5).

**Selection set-up.** `doAddSlur` has three branches keyed on selection state:
- Range (`notationinteraction.cpp:2888-2920`): for **each track** in the staff range,
  first and last *chord* in `sel.uniqueElements()` on that track; one slur per track
  with ≥2 distinct chords. Never cross-voice, never cross-staff, and depends on the
  selection filter (F6).
- Single note (`notationinteraction.cpp:2921-2927`): `Score::addSlur(chord, nullptr)`,
  which picks the next chord/rest, following tie chains (`edit.cpp:700-733`), and then
  `select` + `startEditGrip` on the new segment (`notationinteraction.cpp:3033-3036`),
  which starts UI grip editing inside the transaction.
- List (`notationinteraction.cpp:2928-2963`): over all selected items (notes mapped to
  their chord), skip anything that is not a chord, a barline or a measure-header item;
  earliest item by `isBefore` is the start, latest is the end; then
  `doAddSlur(firstItem, secondItem)`. Order of selection does not matter.

**Required set-up for the agent: a two-element list selection.**
`clearSelection()`, then `select({startNote, endNote}, SelectType::REPLACE)` where each
is a `Note*` of the addressed chord (any note works because notes map to their chord at
`notationinteraction.cpp:2932-2934`; `upNote()` mirrors a user click). This is the only
branch that gives explicit endpoints, allows cross-voice and cross-staff slurs, and
never enters grip edit (`secondItem` is non-null so `notationinteraction.cpp:3033` is
false). Restore the user's selection afterwards (`Score::addSlur` does not change the
selection on this path).

**Arguments.**
```json
{
  "type": "object",
  "required": ["start", "end"],
  "properties": {
    "start": { "$ref": "#/defs/eventAddress" },
    "end":   { "$ref": "#/defs/eventAddress" },
    "direction": { "enum": ["auto", "up", "down"], "default": "auto" },
    "style": { "enum": ["solid", "dashed", "dotted", "wideDashed"], "default": "solid" }
  },
  "defs": {
    "eventAddress": {
      "type": "object",
      "description": "A chord. Either a handle, or a structural position.",
      "oneOf": [
        { "required": ["handle"], "properties": { "handle": { "type": "string" } } },
        { "required": ["measure", "staff", "voice"],
          "properties": {
            "measure": { "type": "integer", "minimum": 1 },
            "beat":    { "type": "number" },
            "tick":    { "type": "integer" },
            "staff":   { "type": "object", "properties": { "part": {"type": "string"}, "index": {"type": "integer", "minimum": 1} } },
            "voice":   { "type": "integer", "minimum": 1, "maximum": 4 },
            "grace":   { "type": "integer", "description": "index of a grace chord attached to the addressed chord, optional" }
          } }
      ]
    }
  }
}
```
`direction` and `style` are optional post-add property changes
(`Pid::SLUR_DIRECTION`, `Pid::SLUR_STYLE_TYPE` via `undoChangeProperty` on the new
slur); the UI sets them after adding, not through the add path. Including them is a
design choice for the conductor.

**Validation before mutation.** (Error codes are proposals.)
| Check | Source of the rule | Code |
|---|---|---|
| Score open, no modal, not in note input | F7 | `E_NO_SCORE`, `E_MODAL_OPEN`, `E_NOTE_INPUT_ACTIVE` |
| Each address resolves to a live `Chord` (a rest is not enough: the list branch drops non-chords at `notationinteraction.cpp:2939-2941`, which would leave one chord and make a self-slur) | `notationinteraction.cpp:2936-2949` | `E_STALE_HANDLE`, `E_RANGE`, `E_NOT_A_CHORD` |
| Start chord ≠ end chord (two notes of one chord collapse to one item; `doAddSlur(c, c)` → `Score::addSlur(c, c)` makes a zero-length slur with a 3sp end offset, `edit.cpp:753-755`) | `notationinteraction.cpp:2943-2948` | `E_SAME_CHORD` |
| Neither end is a trill cue note (silently skipped) | `notationinteraction.cpp:2957-2960` | `E_NOT_A_CHORD` |
| Both chords in the **same part**. A cross-part slur keeps `track2 = track` while its end element is in another part (`edit.cpp:741-746`); its start/end recomputation on clone/reload uses `track2` (`src/engraving/dom/spanner.cpp:791-809`), so the result differs from what was asked | `edit.cpp:741-746` | `E_CROSS_PART` |
| No existing slur with the same start and end and `AUTO` direction: `doAddSlur` silently adds nothing in that case | `notationinteraction.cpp:3018-3020`, `src/engraving/dom/chordrest.cpp:644-667` | `E_DUPLICATE` (or idempotent success; see Q4) |

**Post-condition.** After the call (still inside the transaction):
`startChord->slur(endChord)` (`chordrest.cpp:644-667`) returns a `Slur` that was not in
the pre-call snapshot of `score->spannerMap().findOverlapping(t, t+1)` at the start tick,
with `tick() == startChord->tick()`, `tick2() == endChord->tick()`,
`track() == startChord->track()` and `track2() == endChord->track()` (`edit.cpp:736-746`).
For linked staves, the number of new `SLUR` spanners linked to it
(`slur->linkList().size()`) equals the number of linked staves that carry the start
voice (F4). If no such slur, roll back with `E_NOT_APPLIED`.

**Nested commands.** No `startCmd`/`endCmd` inside. `doAddSlur` wraps in
`startEdit`/`apply` (`notationinteraction.cpp:2879`, `2965`), which are no-ops under the
lock but still notify `dropChanged` (`m_notifyAboutDropChanged = true` at
`notationinteraction.cpp:2880`; `apply()` at `321-330`). Safe under the lock.

**Linked parts / excerpts.** Propagates through `undoAddElement` (F4). For each linked
staff the clone gets `track = linkedTrack`, `track2 = linkedTrack + (track2 - track)`
(`edit.cpp:7200-7204`), and start/end elements are re-pointed to the linked chords with
those tracks (`edit.cpp:7211-7231`). If no linked element matches, the clone keeps the
master's pointer until `Score::addSpanner` recomputes start/end from tick and track
(`src/engraving/dom/score.cpp:4413-4421`, `spanner.cpp:717-809`). A voice-2 start in a
part that does not map voice 2 is skipped for that staff (`edit.cpp:7000-7007`; SLUR is
in `VOICE1_COPY_TYPES`, `edit.cpp:6989`, which only helps voice 1).

**Crash and corruption risks.**
- Single-note branch enters grip edit mid-transaction (`notationinteraction.cpp:3033-3036`);
  avoided by the two-element list selection.
- Dead condition at `notationinteraction.cpp:2951`: `firstChordRest` is never assigned
  in the list branch, so the check is really `secondItem == nullptr`; harmless with
  validated input, but the "one chord selected → slur to next" intent never runs on
  this branch.
- Cross-part slurs: see Validation; rejected.
- Cross-staff slur with a staff that has a *linked staff in the same score*: the clone's
  `track2 = linkedTrack + diff` (`edit.cpp:7203`) assumes the linked staff's neighbour is
  the counterpart of the original end staff (see Q3).
- Plugin API comparison: plugins cannot add a slur except via `cmd("add-slur")`.
  `Cursor::add` sends unknown types to `undoAddElement` with the segment as parent
  (`src/engraving/api/v1/cursor.cpp:288-291`, `426-428`), `Score::addElement` calls
  `parent->add` (`score.cpp:1398-1400`), and `Segment::add` hits
  `ASSERT_X("Segment::add() unknown …")` (`src/engraving/dom/segment.cpp:852-853`).

**Existing upstream tests.** None for the add path (F8). Fixtures to reuse:
`src/engraving/tests/copypaste_data/copypaste01/02/03/10.mscx` (slurs),
`copypaste22.mscx` (cross-staff slur, `copypaste_tests.cpp:165`),
`src/engraving/tests/readwriteundoreset_data/slurs.mscx`
(`readwriteundoreset_tests.cpp:42`), `partialtie_tests.cpp:446`
(`copyPartialTiesAndSlurs`).

**Scenarios.**
- `EL-01` Slur two adjacent quarter notes in one bar, voice 1; undo restores export byte for byte.
- `EL-02` Slur across a system break (assessment S-list "slur across a system break"); export shows one slur, two segments after layout.
- `EL-03` Slur across a barline into the next bar, and across a page break.
- `EL-04` Cross-voice slur (voice 1 → voice 2, same staff): `track2` is voice 2.
- `EL-05` Cross-staff slur on a piano grand staff (RH → LH, same part).
- `EL-06` Cross-part attempt (violin → viola) returns `E_CROSS_PART`, score unchanged.
- `EL-07` Start on a rest returns `E_NOT_A_CHORD`; two notes of one chord return `E_SAME_CHORD`.
- `EL-08` Duplicate slur returns `E_DUPLICATE` (or a no-op success per Q4) with no new spanner.
- `EL-09` Slur in a score with a part excerpt: slur appears in the part, linked; undo removes both.
- `EL-10` Slur from a grace note to its main chord.
- `EL-11` Slur ending in a 1st-ending bar and one jumping from 1st into 2nd ending (no repeat check on this path; record what the UI does).

**Open questions.** Q1, Q3, Q4 below.

---

### add_line  ·  command://notation/add_line

One command, `kind` ∈ {`pedal`, `ottava`, `trill`, `volta`, `textLine`}. All kinds
except 8va/8vb ottavas are added in the UI only through the palette (Lines palette,
`palettecreator.cpp:1129-1303`) or drag-and-drop; the Add › Lines menu only carries
`add-slur`, `add-hairpin`, `add-8va`, `add-8vb`, `add-noteline`
(`src/appshell/qml/MuseScore/AppShell/appmenumodel.cpp:771-776`).

#### Common path: palette apply to a range selection

**UI entry point.** Palette click → `UserPaletteController::applyPaletteElement`
(`src/palette/internal/paletteprovider.cpp:581-593`) →
`NotationInteraction::applyPaletteElement` (`notationinteraction.cpp:2309-2345`) →
`applyPaletteElementToRange` (`notationinteraction.cpp:2528`) → SLine branch
(`notationinteraction.cpp:2646-2662`):
```
startSegment = sel.startSegment(); endSegment = sel.endSegment();
firstStaffOnly = isSystemTextLine(element) && !(modifiers & Ctrl);
for staff i in [firstStaffOnly ? 0 : staffStart, firstStaffOnly ? 1 : staffEnd):
    spanner = element->clone(); spanner->setScore(score); spanner->styleChanged();
    score->cmdAddSpanner(spanner, i, startSegment, endSegment, Ctrl);
    if (spanner->hasVoiceAssignmentProperties()) setInitialTrackAndVoiceAssignment(...)
    selectAndStartEditIfNeeded(spanner);
```
`Score::cmdAddSpanner(Spanner*, staff_idx_t, Segment*, Segment*, bool)`
(`src/engraving/editing/cmd.cpp:603-633`):
- `track = track2 = staffIdx * VOICES` (voice 1), also on any existing segments (`cmd.cpp:605-610`).
- `tick = startSegment->tick()`, or the start measure's tick for `Anchor::MEASURE` (`cmd.cpp:612-614`).
- `tick2 = endSegment->tick()`; null end → `lastSegment()->tick()`; end == start →
  the measure's last segment (`cmd.cpp:616-623`); for `Anchor::MEASURE`, rounded up to
  the end of the containing (MM) measure (`cmd.cpp:624-629`).
- `undoAddElement(spanner, true, ctrl)` (`cmd.cpp:632`).

The list-selection branch (`notationinteraction.cpp:2458-2492`) differs: pedal ends at
`nextCR` of the last CR (`2461-2462`), voice-specific spanners (trill, hairpin, let
ring: `spanner.cpp:1424-1433`) get `setTrack(cr1->track())` by **direct mutation after**
`undoAddElement` (`notationinteraction.cpp:2484-2486`), so linked clones keep voice 1,
and it only applies when one CR or two CRs on one staff are selected
(`notationinteraction.cpp:2351-2364`); otherwise it falls through to per-element drops.
**Use the range branch.**

None of `hasVoiceAssignmentProperties()` is true for these kinds (overrides exist only
for dynamic, expression, hairpin, text: `src/engraving/dom/dynamic.h:112`,
`expression.h:59`, `hairpin.h:163`, `text.cpp:112`), so line 2657-2659 is inert here.

`selectAndStartEditIfNeeded(spanner)` (`notationinteraction.cpp:1069-1080`) selects the
spanner; with no segments (F2) it selects the spanner itself, whose
`needStartEditingAfterSelecting()` is the `EngravingItem` default `false`
(`src/engraving/dom/engravingitem.h:294`; only `LineSegment` overrides it,
`src/engraving/dom/line.h:67`), so no edit mode starts. A *palette* element may carry
segments into the clone (`Spanner::Spanner(const Spanner&)` clones segments,
`spanner.cpp:439-443`), which would start grip editing; a template from `Factory` has
none.

**Selection set-up.** `clearSelection()`; `select({crStart}, RANGE, staffIdx)`;
`select({crEnd}, RANGE, staffIdx)` (F6). For volta, select the first and last
`Measure` with `RANGE` on staff 0. Then read back `selection()->range()` and check
`startSegment`/`endSegment` before calling.

**How to call.** Two options, choice for the conductor (Q2):
1. `interaction->applyPaletteElement(templ, Qt::NoModifier)` with a template built as
   below. Requires `MScore::_error == MS_NO_ERROR` immediately before the call (F5).
2. Reproduce lines 2646-2662 on `Score`: `clone`, `setScore`, `styleChanged`,
   `cmdAddSpanner(spanner, staffIdx, startSeg, endSeg, false)`. No dialog, no selection
   change, same engraving calls. `cmdAddSpanner` is public (`src/engraving/dom/score.h:968`).

Templates: build with `Factory::createX(score->dummy())`, as `cmdAddOttava` does with
its own ottava (`edit.cpp:2336`, `2359`), and set properties *before* the clone so the
clone carries them.

**Common arguments.**
```json
{
  "type": "object",
  "required": ["kind", "start", "end"],
  "properties": {
    "kind":  { "enum": ["pedal", "ottava", "trill", "volta", "textLine"] },
    "start": { "$ref": "add_slur#/defs/eventAddress" },
    "end":   { "$ref": "add_slur#/defs/eventAddress",
               "description": "Last event covered, inclusive: the line ends where this chord/rest ends (UI range semantics)." },
    "staff": { "description": "Defaults to start.staff. Ignored for volta and system text line (always staff 0)." },
    "placement": { "enum": ["above", "below", "default"], "default": "default" }
  }
}
```
For volta, `start`/`end` may be `{ "measure": n }` only.

**Common validation.** `E_NO_SCORE`, `E_MODAL_OPEN`, `E_NOTE_INPUT_ACTIVE` (F7);
`start` and `end` resolve to live chord/rests on the same staff (`E_STALE_HANDLE`,
`E_RANGE`); `start.tick ≤ end.tick` (`E_RANGE`); after selection set-up the range is
valid with non-null `startSegment` (`cmd.cpp:613` dereferences it) and spans exactly
the requested staff (`E_SELECTION_FAILED`).

**Common post-condition.** Snapshot the spanners of the kind's `ElementType` overlapping
`[tick, tick2]` before the call; after it, exactly one new spanner of that type on the
target track with `tick()`/`tick2()` equal to the values `cmdAddSpanner` derives from
the selection's start and end segments (computed with the rules above, not predicted
from the CRs), plus one linked clone per linked staff or per score (F4). Otherwise
roll back with `E_NOT_APPLIED`.

**Common nested commands.** `applyPaletteElement` wraps in `startEdit`/`apply`
(`notationinteraction.cpp:2326`, `2338`): no-ops under the lock, `dropChanged` still
fires (F1). `cmdAddSpanner` and `undoAddElement` never call `startCmd`. Safe under the
lock.

**Common existing tests.** None for the add path (F8). `spanners_tests.cpp:656-697`
(`spanners18_pedalOverriddenStyledProperties`) creates a pedal via
`Factory::createPedal(score->dummy())` and checks styled properties, which confirms the
template construction pattern.

#### kind = pedal

- **Template.** `Factory::createPedal(score->dummy())` (`src/engraving/dom/factory.h:295`).
  The constructor resets hooks to `NONE` (`src/engraving/dom/pedal.cpp:125-140`,
  `187-189`); the palette's default "Pedal (with ped and line)" additionally sets
  `setEndHookType(HOOK_90)` and the begin/continue/end texts from `propertyDefault`
  (`palettecreator.cpp:1208-1213`). Variants: "ped and asterisk" (`LineVisible false`,
  end hook `ROSETTE`, `1215-1221`), "straight hooks" (`1223-1229`), angled hooks
  (`1231-1255`). Proposed `style` argument: `line` | `asterisk` | `straight` |
  `angledEnd` | `angledBoth` | `angledStart`, mapped exactly to those setters.
- **Range.** Segment anchor, voice 1 of the staff (`cmd.cpp:605-614`).
- **Linked.** PEDAL is in the per-staff list and `VOICE1_COPY_TYPES`
  (`edit.cpp:6922`, `6993`); spanner block `edit.cpp:7188-7232`.
- **Risks.** None found beyond the common ones. The palette's list-path pedal end rule
  (`notationinteraction.cpp:2461-2462`) gives a different `tick2` from the range path; use the range path only.
- **Scenarios.** `EL-20` pedal over 2 bars on piano LH; `EL-21` pedal crossing a
  system break; `EL-22` asterisk-style pedal; `EL-23` pedal in a score with a part
  excerpt, undo removes both.

#### kind = ottava

- **UI entry points (two).**
  - 8va / 8vb: actions `add-8va`/`add-8vb` (`notationuiactions.cpp:1252-1263`),
    registered at `notationactioncontroller.cpp:336-337` →
    `NotationInteraction::addOttavaToSelection(type)` (`notationinteraction.cpp:5526-5535`)
    → `startEdit("Add ottava")`, `Score::cmdAddOttava(type)`
    (`edit.cpp:2322-2373`), `apply()`.
  - 15ma/15mb/22ma/22mb: palette only (`palettecreator.cpp:1188-1206`), common path above.
- **`cmdAddOttava` semantics** (differ from the palette path):
  - Multi-staff range: one ottava per staff from the voice-1 first/last CR
    (`sel.firstChordRest(staff*VOICES)`, `select.cpp:281-284`, `201-235`),
    `tick = cr1->tick()`, `tick2 = cr2->endTick()`, `track = cr1->track()`
    (`edit.cpp:2326-2347`).
  - Otherwise (list, or range on one staff): `getSelectedStartEndChordRests` over all
    selected elements of any voice (`edit.cpp:177-201`), so `track` is the earliest CR's
    track and may be voice 2–4 (`edit.cpp:2349-2368`). If no CR is selected it sets
    `MScore::setError(NO_NOTE_REST_SELECTED)` (`edit.cpp:194-196`): under the lock this
    error survives to the final `commitChanges` and rolls back the **whole** transaction
    (`cmd.cpp:418-420`).
  - 8vb gets `setPlacement(BELOW)` + `styleChanged()` (`edit.cpp:2338-2341`, `2361-2364`).
    The palette path relies on the per-type placement style instead
    (`src/engraving/dom/ottava.cpp:173-196`).
  - Selects the new ottava through `Score::select` directly, outside the
    interaction's selection notification (`edit.cpp:2370-2372`).
- **Template (palette path).** `Factory::createOttava(score->dummy())`,
  `setOttavaType(t)` (which calls `styleChanged()`, `ottava.cpp:109-116`), as the palette
  does (`palettecreator.cpp:1201-1205`).
- **Score side effect.** Adding an ottava calls `staff()->updateOttava()` and
  `setPlaylistDirty()` (`score.cpp:1446-1458`); written pitch is unchanged, sounding
  pitch and display octave change.
- **Validation.** `type` ∈ {8va, 8vb, 15ma, 15mb, 22ma, 22mb}. Neither UI path checks
  for an overlapping ottava on the same staff; whether the agent rejects overlap
  (`E_OVERLAP`) is Q5.
- **Linked.** OTTAVA in per-staff list and `VOICE1_COPY_TYPES` (`edit.cpp:6918`, `6987`).
  `EXCLUDE_FROM_OTHER_PARTS` on ottavas is exercised in `parts_tests.cpp:1182-1186`.
- **Tests / fixtures.** `copypaste_data/copypaste07/08/09.mscx` (ottava start/end/whole,
  `copypaste_tests.cpp:130-140`); `parts_data/partExclusion.mscx`.
- **Scenarios.** `EL-30` 8va over 4 bars (action path); `EL-31` 15mb via template path;
  `EL-32` 8va on a 2-staff range gives one ottava per staff; `EL-33` 8va where the
  selection starts on a voice-2 note: record the track (`cmdAddOttava` path);
  `EL-34` ottava across a system break, undo restores export.

#### kind = trill

- **Template.** `Factory::createTrill(score->dummy())`, then **always**
  `setTrillType(t)`, even for the default `TRILL_LINE`. The constructor leaves
  `m_ornament == nullptr` (`src/engraving/dom/trill.cpp:189-198`); only `setTrillType`
  creates it (`trill.cpp:342-351`). The palette always calls it
  (`palettecreator.cpp:1265-1269`, `826-831`).
- **Crash risk (confirmed in source).** `Trill::linkedClone` dereferences `m_ornament`
  with no null check (`trill.cpp:210-217`). `undoAddElement` calls `linkedClone` for
  every linked staff or excerpt (`edit.cpp:7037`). A trill built without
  `setTrillType`, added to a score with parts or linked staves, is a null-pointer crash.
- **Types.** `TRILL_LINE`, `UPPRALL_LINE`, `DOWNPRALL_LINE`, `PRALLPRALL_LINE`
  (`palettecreator.cpp:1258-1260`).
- **Voice.** Trill is voice-specific (`spanner.cpp:1424-1433`) but the range path always
  puts it on voice 1 (`cmd.cpp:605`). Only the list path moves it to the CR's voice, by
  direct mutation that linked clones do not follow (`notationinteraction.cpp:2484-2486`).
  A voice-2 trill is Q6.
- **Ornament attachment.** `Trill::computeStartElement` parents the ornament to the start
  chord (`trill.cpp:253-263`); called by `Score::addSpanner` (`score.cpp:4413-4421`).
- **Linked.** TRILL in per-staff list and `VOICE1_COPY_TYPES` (`edit.cpp:6919`, `6988`).
- **Scenarios.** `EL-40` trill line over a tied whole note; `EL-41` trill in a score
  with a part excerpt (guards the `linkedClone` crash); `EL-42` prallprall line across
  a barline.

#### kind = volta

- **Template.** `Factory::createVolta(score->dummy())`: SYSTEM flag, placement above,
  `Anchor::MEASURE` (`volta.cpp:105-124`, `src/engraving/dom/volta.h:68`). Then
  `setEndings(list)` (sorted, `volta.cpp:129-133`), `setText("1.")` (= begin text,
  `volta.cpp:139-142`), `setVoltaType(CLOSED|OPEN)` (end hook `HOOK_90` or `NONE`,
  `volta.cpp:429-432`), as the palette does (`palettecreator.cpp:1154-1186`).
- **Range.** Measure-anchored: `tick` = start measure tick, `tick2` = end of the last
  selected measure, rounded through `tick2measureMM` (`cmd.cpp:612-629`;
  `src/engraving/dom/utils.cpp:101-118`). Always on staff 0 (`firstStaffOnly`,
  `notationinteraction.cpp:2649-2651`).
- **Linked.** System-object branch: master `staff(0)` plus every excerpt's `staff(0)`
  and all `systemObjectStaves()` (`edit.cpp:6719-6731`, `6762-6769`). VOLTA is absent
  from the per-staff list (`edit.cpp:6906-6948`), so the system-flag route is the only
  propagation; `ctrl` must be false (F4).
- **Repeats (structural).** Adding a volta does **not** add or check an end-repeat
  barline, a start repeat, or a play count; no volta-specific code exists in the add
  path (`git grep -i volta` over `src/notation/internal/notationinteraction.cpp` and
  `src/engraving/editing/*.cpp` finds only drop routing and tick shifting). Playback
  order comes from `RepeatList`, which merges overlapping voltas
  (`src/engraving/dom/repeatlist.cpp:465-493`). The playlist is marked dirty by
  `endCmd` when the score is dirty (`cmd.cpp:438-440`, `masterscore.cpp:157-162`).
  A useful first ending therefore needs `structure.set_repeat` (end-repeat barline)
  in the same batch; `add_line(volta)` stays a pure bracket add, like the UI.
- **Validation.** `endings` non-empty positive integers (`firstEnding()` returns 0 for
  empty, `volta.cpp:188-194`) → `E_BAD_ARGUMENT`; start ≤ end measure → `E_RANGE`.
  Overlap with an existing volta: UI does not check; Q5.
- **Multimeasure rests.** Selecting a measure covered by an MM rest selects the MM rest
  (`score.cpp:3548-3549`, `3396-3399`), and `tick2` rounding uses `tick2measureMM`
  (`cmd.cpp:625`). See Q7.
- **Tests / fixtures.** `repeat_tests.cpp:102-104` (`repeat06.mscx`, simple volta;
  expected sequence `"1;2;3; 2; 4;5;6"`) and the other volta cases in
  `repeat_tests.cpp:117-170`, `269-370`. The `repeat()` helper in that file is the
  natural post-commit check (unrolled measure order).
- **Scenarios.** `EL-50` 1st/2nd endings with `set_repeat` in one batch; the unrolled
  measure order matches `repeat06`; `EL-51` volta in a score with parts appears in
  every part; `EL-52` volta spanning a system break; `EL-53` volta alone, no repeat
  barline: added as the UI would, playback order unchanged; `EL-54` undo after
  `EL-50` restores the export and the repeat list.

#### kind = textLine

- **Template.** Staff text line: `Factory::createTextLine(score->dummy())`
  (`src/engraving/dom/factory.cpp:705`), which calls `new TextLine(parent)` with
  `system = false` (`src/engraving/dom/textline.cpp:158-192`). System text line:
  construct with `new TextLine(score->dummy(), true)`, as the palette does
  (`palettecreator.cpp:1276-1280`). Setting the flag afterwards with `setSystemFlag(true)`
  leaves the style chosen by the constructor's `initStyle()` (`textline.cpp:161-163`,
  `203-210`) unless `initStyle()` (public, `src/engraving/dom/textline.h:77`) is called
  again. Properties to expose: `beginText`, `continueText`, `endText`,
  `beginHookType`/`endHookType`, `lineVisible`, `diagonal`, as in
  `palettecreator.cpp:1271-1301`.
- **Range.** Segment anchor, voice 1 (`cmd.cpp:605-614`). A system text line goes on
  staff 0 only (`notationinteraction.cpp:2649-2651`).
- **Linked.** Staff text line: per-staff branch (`edit.cpp:6921`, `6992`,
  `7234-7244` for note-anchored parents, not used here). System text line:
  system-object branch (F4).
- **Out of scope.** Note-anchored lines (`add-noteline`, `notationactioncontroller.cpp:342`)
  take another path (`Score::addNoteLine`, `edit.cpp:2379`) and skip `addSpanner`
  (`score.cpp:1434-1436`).
- **Tests / fixtures.** `parts_data/part-textlines.mscx`, `part-textlines-parts.mscx`.
- **Scenarios.** `EL-60` staff text line "rit." over 3 bars; `EL-61` system text line in
  a 4-staff score appears once (staff 0) and in every part; `EL-62` text line across a
  system break.

#### add_line: crash and corruption risks (summary)

- Trill without `setTrillType` + parts or linked staves → null dereference in
  `Trill::linkedClone` (`trill.cpp:213`).
- `applyPaletteElement` → `checkAndShowError` can show a modal dialog and swallow a
  pending error (F5).
- `cmdAddOttava` with nothing selected sets `MScore` error → whole-batch rollback at
  commit (`edit.cpp:194-196`, `cmd.cpp:418-420`).
- `cmdAddSpanner` dereferences `startSegment` (`cmd.cpp:613`) and, for measure-anchored
  lines, the result of `tick2measureMM` (`cmd.cpp:625-626`); `measureByTick` returns
  null only for ticks past the last measure base's end
  (`src/engraving/dom/measurebase.cpp:943-947`). Validated selections keep both
  non-null.
- `ctrl` modifier on a system line clears the SYSTEM flag (`edit.cpp:6719-6721`).
- Plugin API: `Cursor::add` of any spanner reaches `Segment::add`'s `ASSERT_X`
  (`cursor.cpp:426-428`, `score.cpp:1398-1400`, `segment.cpp:852-853`); plugins can only
  reach these lines through `cmd("add-8va")`/`cmd("add-8vb")`.

#### add_line: open questions

Q1, Q2, Q5, Q6, Q7 below.

---

## Open questions

- **Q1. Mid-transaction notifications.** `apply()` notifies `notationChanged` or
  `dropChanged` even when the stack is locked (`notationinteraction.cpp:321-330`;
  `muse::async::Notification` dispatch semantics not traced). Whether listeners
  (view repaint, inspector, accessibility) read the score synchronously in a
  half-finished, un-laid-out state was not resolved from source. This applies to every
  command group that calls `NotationInteraction` methods.
- **Q2. Palette call vs. direct `cmdAddSpanner`.** Calling `applyPaletteElement` keeps
  the UI entry point but brings `checkAndShowError` (F5) and a selection change;
  reproducing `notationinteraction.cpp:2646-2662` on `Score` avoids both. Conductor's
  decision, and the same choice recurs in `expression-marks` (hairpins via palette) and
  `text`.
- **Q3. Cross-staff slur on a staff with an in-score linked staff** (e.g. a linked TAB
  staff). The clone's `track2 = linkedTrack + diff` (`edit.cpp:7200-7204`) assumes the
  staff layout around the linked staff matches the original. Not resolved; needs a
  fixture to observe.
- **Q4. Duplicate slur.** `doAddSlur` silently adds nothing when an `AUTO` slur with the
  same endpoints exists (`notationinteraction.cpp:3018-3020`). Error (`E_DUPLICATE`) or
  idempotent success?
- **Q5. Overlap policy.** Neither path rejects overlapping ottavas or voltas on the same
  staff (no check in `cmd.cpp:603-633` or `edit.cpp:2322-2373`). Whether the agent
  rejects, or matches the UI and allows, is a policy choice.
- **Q6. Voice-specific trills.** The range path puts trills on voice 1
  (`cmd.cpp:605`). The only UI way to get a voice-2 trill is the list path's direct
  `setTrack` (`notationinteraction.cpp:2484-2486`), which is not undo-recorded as a
  separate step and is not applied to linked clones. Support voice-2 trills or not?
- **Q7. Multimeasure rests on screen.** When `Sid::createMultiMeasureRests` is on,
  range selection and `tick2measureMM` operate on MM rests (`score.cpp:3548-3549`,
  `utils.cpp:101-118`). Whether a line can start or end inside an MM rest from the
  agent, and what the UI does then, was not traced.
- **Q8. Excerpt as the edit target.** All findings assume the edit is made on the
  master score. The behaviour when the agent's target notation is an excerpt (adding
  into a part, then propagating back) was not traced.
- **Q9. Cross-part slur after save and reload.** The exact write/read path for a slur
  whose `track2` disagrees with its end element (`edit.cpp:741-746`) was not followed
  through `rw/`. Validation rejects such slurs, so this only matters if Q8's answer
  changes the picture.
