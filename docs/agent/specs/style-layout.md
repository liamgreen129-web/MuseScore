# Spec sheet: `style-layout` group

Scout: `api-scout`, task P0.S-style-layout. Source: branch `agent/4.7` at v4.7.6.
Every `file:line` below refers to the working tree at that tag. Paths are relative to
the repository root.

Commands: set_style, set_measure_stretch, lock_systems, set_offset.

Shared facts from other sheets are referred to by their IDs (structure G1–G10,
pitch P1–P11, expression-lines F1–F7); they are not repeated here.

---

## 0. Group-wide findings (read before any command section)

### SL1. Everything in this group is per score, not per piece

All four edits act on one `Score` (master or one excerpt) and none of them reach the
other scores on their own:

| Edit | Where the state lives | Propagates to other scores? |
|---|---|---|
| Style value | `Score::m_style` (`src/engraving/dom/score.h:733-734`, `1203`); an excerpt copies the master's style once, at creation (`src/engraving/dom/score.cpp:196-226`) | No. `ChangeStyleValues` changes only `m_score` (`src/engraving/editing/editstyle.cpp:135-159`) |
| Measure stretch | `Measure::m_userStretch` (`src/engraving/dom/measure.h:394`) | No. `Pid::USER_STRETCH` has `PropertyGroup::NONE` and `link=false` (`src/engraving/dom/property.cpp:255`), so `propertyPropagation` returns `NONE` for every linked measure (`src/engraving/dom/engravingitem.cpp:1473-1479`) |
| System lock | `Score::m_systemLocks` (`score.h:1063-1066`, `1189`) | No. `AddSystemLock::redo` adds the lock to `startMB()->score()` only (`src/engraving/editing/editsystemlocks.cpp:53-57`); nothing in `excerpt.cpp` / `masterscore.cpp` copies locks (`git grep -i systemlock` finds none) |
| Offset | `EngravingItem::m_offset` | Partly: master → part items whose position is still linked to master; part → master never (the part item is unlinked instead). Details in set_offset |

So unlike the structure group (structure G4, "resolve against the master"), every
command here needs an explicit **target score**: `"score"` or a part name, resolved to
`IMasterNotation::masterScore()` (`src/notation/imasternotation.h:45`) or
`excerpts()[i]->notation()` (`imasternotation.h:54`, `src/notation/iexcerptnotation.h:49`).
The UI always acts on the notation being viewed (`context()->currentNotation()`,
e.g. `src/notationscene/widgets/editstyle.cpp:2081`). The undo stack is shared, so edits
on any score land in the one master macro (`score.cpp:6286`, structure G4).

### SL2. Transactions: no nested startCmd/endCmd, no unwind, no dialogs on the engraving paths

`git grep` finds no `startCmd`, `endCmd`, `update()`, `activeCommand()->unwind`,
`MessageBox` or `interactive()` in `src/engraving/editing/editsystemlocks.cpp`,
`src/engraving/editing/editstyle.cpp` or `src/engraving/editing/transpose.cpp`, and none in
`Score::cmdAddStretch` / `Score::resetUserStretch` (`src/engraving/editing/cmd.cpp:2494-2523`,
`2584-2605`) or `Score::cmdConcertPitchChanged` (`score.cpp:2919-2968`). None of the
`NotationInteraction` wrappers used by this group call `checkAndShowError()`
(`src/notation/internal/notationinteraction.cpp:5948-6011`, `6317-6324`, `6531-6536`).
All engraving calls go through `Score::undo` → `UndoStack::pushAndPerform`
(`score.cpp:4352-4355`), so they must run after the core's `prepareChanges` (pitch P7).

**The one hazard is `NotationStyle`, not the engraving layer:** every `INotationStyle`
setter calls `Score::update()` (see SL3).

### SL3. `INotationStyle::setStyleValue` calls `Score::update()` mid-transaction

`NotationStyle::setStyleValue` (`src/notation/internal/notationstyle.cpp:50-64`) is
`undoChangeStyleVal(...)` then `score()->update()` (`notationstyle.cpp:59-60`).
`resetStyleValue`, `resetStyleValues`, `resetAllStyleValues` and `applyToAllParts` do the
same (`notationstyle.cpp:66-106`, `97`). `Score::update()` is `update(true)`
(`score.h:596`): it lays out every open score for the pending layout range, then
`cmdState().reset()` (`cmd.cpp:468-520`), which clears the layout flags, tick range and
staff range (`cmd.cpp:208-222`). Inside an agent batch that:
- runs a full layout in the middle of the batch (a style change always sets layout-all,
  `score.cpp:1326-1340` → `masterscore.cpp:328-331`), on top of any half-done structural
  steps (structure G6 / OQ-1);
- throws away earlier steps' tick/staff ranges and layout flags. The changed-object and
  changed-style-id lists survive, because `buildScoreChanges` reads them from the undo
  macro, not from `cmdState` (`cmd.cpp:104-153`, `src/engraving/editing/undo.cpp:632-647`),
  and it re-widens the tick range from the changed objects (`cmd.cpp:130-143`). The staff
  range and layout flags are lost.

The UI tolerates this because the Edit Style dialog holds one macro open for its whole
lifetime (`prepareChanges` in `showEvent`, `editstyle.cpp:1271`; `commitChanges` in
`accept`, `1602-1608`) and calls `setStyleValue` for every widget change
(`editstyle.cpp:2079-2082`), so the UI itself calls `update()` mid-macro routinely. The
agent's batches mix style with other edits, so it must not.

**Rule:** call `Score::undoChangeStyleVal` / `undoChangeStyleValues`
(`src/engraving/editing/edit.cpp:5577-5588`) on the target score, and
`Score::cmdConcertPitchChanged` for `concertPitch` (as `notationstyle.cpp:56-57` does).
After commit, notify `INotationStyle::styleChanged()` on each affected notation, as the
dialog does after its own commit (`editstyle.cpp:1605`). The master `Notation` forwards
that to `notationChanged` (`src/notation/internal/notation.cpp:84-86`); the Inspector's
mmrest model and the QML style pages listen to it
(`src/inspector/qml/MuseScore/Inspector/notation/mmrests/mmrestsettingsmodel.cpp:40`,
`src/notationscene/qml/MuseScore/NotationScene/styledialog/abstractstyledialogmodel.cpp:41`).

`Score::update` is a no-op while `Score::m_updatesLocked` is set (`cmd.cpp:470-472`,
`lockUpdates` at `530-533`); the project loader uses that (`src/project/internal/notationproject.cpp:177-214`).
Whether the core should use it as a guard against stray `update()` calls is an open
question (OQ-SL1), not a recommendation.

### SL4. Layout-dependent code in this group

Like structure G6, several functions here read the layout that existed at the start of
the transaction:
- `MeasureBase::system()` is just the parent pointer (`src/engraving/dom/measurebase.h:82`).
  A measure created earlier in the same transaction is parented to the **dummy system**
  (`src/engraving/dom/masterscore.cpp:546`; `src/engraving/editing/edit.cpp:1195`), whose
  measure list is empty. `System::first()/last()` are `m_ml.front()/back()`
  (`src/engraving/dom/system.h:140-141`) and `System::isLocked()` is
  `m_ml.front()->isStartOfSystemLock()` (`src/engraving/dom/system.cpp:286-289`): **calling
  them on the dummy system is undefined behaviour (front() of an empty vector)**. Affected:
  `toggleSystemLock`, `toggleScoreLock`, `addRemoveSystemLocks(…, lock=true)`,
  `moveMeasureToPrevSystem/NextSystem`, `Selection::selectedSystems`
  (`editsystemlocks.cpp:138-189`, `205-216`, `285-347`; `src/engraving/dom/select.cpp:409-430`).
- `nextMM()/prevMM()/mmRest()` reflect the multimeasure rests built by the last layout
  (structure G7). Used by `makeIntoSystem`, `removeLayoutBreaksOnAddSystemLock`,
  `addRemoveSystemLocks` (`editsystemlocks.cpp:231`, `257`, `268`, `407`) and by
  `Score::cmdAddStretch` (`cmd.cpp:2592`).
- `undoChangeProperty(Pid::OFFSET, …)` records the element's **current laid-out position**
  for autoplace (`src/engraving/dom/engravingobject.cpp:472-479` →
  `src/engraving/rendering/score/autoplace.cpp:384-392`); see set_offset.

### SL5. Multimeasure rests and measure addressing

`Sid::createMultiMeasureRests` defaults to false for the master (`src/engraving/style/styledef.cpp:519`)
but is forced to true when a part is created (`score.cpp:219-222`). So in parts the UI's
measure-range functions usually operate on **mmrest measures**: `Selection::startMeasureBase/endMeasureBase`
use `tick2measureMM` when mmrests are on (`select.cpp:363-406`), and `cmdAddStretch` walks
`firstMeasureMM()/nextMeasureMM()` (`cmd.cpp:2590-2604`). An mmrest `Measure` is created
and owned by layout (`src/engraving/rendering/score/measurelayout.cpp:194-238`). Structure G7
says "address underlying measures"; that rule does not carry over unchanged here (see
set_measure_stretch and lock_systems, and OQ-SL3).

### SL6. Plugin API is a weak model for this group

- `apiv1::MStyle::setValue` calls `undoChangeStyleVal` for every key including
  `concertPitch` (`src/engraving/api/v1/style.cpp:71-86`), skipping
  `cmdConcertPitchChanged`'s key-signature and chord-symbol transposition
  (`score.cpp:2919-2968`). Diverges from the UI.
- `apiv1::ScoreElement::set` converts every `P_TYPE::POINT` by `spatium()`
  (`src/engraving/api/v1/scoreelement.cpp:201-202`, `95-98`), while the Inspector uses
  `spatium()` only when `offsetIsSpatiumDependent()` and `DPMM` otherwise
  (`src/inspector/qml/MuseScore/Inspector/abstractinspectormodel.cpp:626-631`). Diverges
  for non-spatium-dependent items.
- No plugin API exists for system locks (`git grep -i systemlock src/engraving/api` is empty).
- `Measure.userStretch` is a plain property (`src/engraving/api/v1/elements.h:2067`) that
  ends in `undoChangeProperty` (`scoreelement.cpp:218-223`), the same as the UI's measure
  properties dialog.

---

### set_style  ·  command://notation/set_style

**UI entry point.**
- Format → Style… opens `EditStyle`; each widget change calls
  `EditStyle::setStyleValue` → `INotationStyle::setStyleValue`
  (`src/notationscene/widgets/editstyle.cpp:2079-2082`) →
  `NotationStyle::setStyleValue` (`src/notation/internal/notationstyle.cpp:50-64`), which
  returns early if the value is unchanged (`52-54`), calls
  `Score::cmdConcertPitchChanged(bool)` for `Sid::concertPitch` (`56-57`), otherwise
  `Score::undoChangeStyleVal(id, value)` and `Score::update()` (`59-60`), then notifies
  `styleChanged` (`63`). The whole dialog session is one macro "Edit style"
  (`editstyle.cpp:1271`, `1602-1608`; Cancel rolls back, `1614-1619`).
- Format → Page settings… (`src/notationscene/widgets/pagesettings.cpp`) uses the same
  `setStyleValue` per field (`146-149`) and, for "apply to all parts", calls
  `Score::undoChangeStyleValues` on each excerpt directly (`311-341`).
- The concert-pitch toolbar button: `NotationActionController::toggleConcertPitch`
  (`src/notationscene/internal/notationactioncontroller.cpp:2175-2192`, registered at `384`)
  wraps `setStyleValue(concertPitch, …)` in its own prepare/commit with the undo names
  "Display concert pitch" / "Display transposed".
- Inspector "set as style" buttons: `AbstractInspectorModel` wraps `setStyleValue` in
  "Edit style" (`src/inspector/qml/MuseScore/Inspector/abstractinspectormodel.cpp:599-601`).
- Toggle actions `toggle-mmrest` / `toggle-hide-empty` call `Score::cmdToggleMmrest` /
  `cmdToggleHideEmpty` (`notationactioncontroller.cpp:480-483`), which are
  `deselectAll()` + `undoChangeStyleVal` (`src/engraving/editing/cmd.cpp:5039-5056`).

**Engraving functions the agent calls (SL3):**
- `Score::undoChangeStyleVal(Sid, const PropertyValue&)` (`src/engraving/dom/score.h:511`,
  `src/engraving/editing/edit.cpp:5577-5583`) or `undoChangeStyleValues(map)` for several
  ids at once (`score.h:512`, `edit.cpp:5585-5588`). Both push one `ChangeStyleValues`
  (`src/engraving/editing/editstyle.h:51-70`).
- `Score::cmdConcertPitchChanged(bool)` (`score.h:790`, `score.cpp:2919-2968`) for
  `Sid::concertPitch` only.
- Reset to default: `Score::resetStyleValue(Sid)` / `resetStyleValues(set)`
  (`score.cpp:5666-5686`), which are `undoChangeStyleVal(s)` with
  `DefaultStyle::defaultStyle()` values.

What `ChangeStyleValues::flip` does (`editstyle.cpp:135-159` in `src/engraving/editing/`):
skips unchanged values; for each changed id calls `changeStyleValue` (`104-133`), which
sets the value and then:
- chord-symbol ids (`chordExtensionMag`, `chordExtensionAdjust`, `chordModifierMag`,
  `chordModifierAdjust`, `chordDescriptionFile`, `verticallyStackModifiers`,
  `chordAlignmentExcludeModifiers`, `musicalTextFont`) **reload the chord list**
  (`changeChordStyle`, `31-51`), which reads `chords.xml` and `Sid::chordDescriptionFile`
  from disk;
- `Sid::spatium` calls `Score::spatiumChanged`, which rescales every element's offset and
  every staff (`score.cpp:1300-1307`; `src/engraving/dom/engravingitem.cpp:234-239`);
- `Sid::defaultsVersion` changes the default-style version (`editstyle.cpp:122-124`);
- `Sid::createMultiMeasureRests` true → false removes every system lock containing an
  mmrest (`editstyle.cpp:125-129` → `editsystemlocks.cpp:442-456`);
then, if anything changed, `Score::styleChanged()` (`flip`, `156-158`): `styleChanged()` on
every element and staff type, padding table rebuilt, `setLayoutAll()` (`score.cpp:1326-1340`).
**Every style change therefore triggers a full relayout of that score at commit.**
The music font (`Sid::musicalSymbolFont`) is not reloaded by `ChangeStyleValues`; it is
re-read at the start of every layout (`score.cpp:6030-6031`), so it takes effect at commit.
(`ChangeStyle`, the whole-style command, does call `setEngravingFont` explicitly,
`editstyle.cpp:81-83`.)

**Selection set-up.** None. The function takes explicit arguments. Exception:
`cmdToggleMmrest` deselects first (`cmd.cpp:5042`) because toggling mmrests deletes or
creates mmrest measures at the next layout; the Edit Style dialog does not. When
`createMultiMeasureRests` changes, the core must not restore a saved selection that holds
pointers into mmrest measures (restore by tick/staff instead, or clear).

**Arguments.**
```json
{
  "type": "object",
  "required": ["values"],
  "properties": {
    "target": { "type": "string", "default": "score",
                "description": "\"score\" for the master, or a part name (SL1)" },
    "values": {
      "type": "object",
      "description": "Map of Sid identifier → value. Keys are the C++ enum names (e.g. \"spatium\", \"minMeasureWidth\", \"createMultiMeasureRests\").",
      "additionalProperties": true
    },
    "reset": { "type": "array", "items": { "type": "string" },
               "description": "Sids to reset to DefaultStyle::defaultStyle()" }
  }
}
```
Sid typing:
- Name → `Sid`: the XML tag equals the enum identifier (`styleDef(sidAndXmlTag, …)`,
  `src/engraving/style/styledef.cpp:37`), so `MStyle::styleIdx(String)`
  (`src/engraving/style/style.cpp:802-811`, linear scan) and `QMetaEnum::fromType<Sid>()`
  (`Q_ENUM_NS(Sid)`, `styledef.h:2195`; used by the plugin, `api/v1/style.cpp:43-57`) both
  work. Unknown name → `Sid::NOSTYLE` (`-1`, `styledef.h:51`).
- Type: `MStyle::valueType(sid)` is the type of the default value
  (`style.cpp:788-791`, `styledef.h:2228`). `MStyle::set` performs **no type check**
  (`style.cpp:68-84`), and the typed getters only `assert` in debug builds
  (`style.h:50-59`). The core must convert JSON to exactly that `P_TYPE`.
- Units the agent must know: page sizes and margins are `REAL` inches
  (`styledef.cpp:41-49`, `210.0 / INCH`); `Sid::spatium` is `REAL` in internal units,
  `1.75 mm * DPMM` by default (`styledef.cpp:752`; `DPI = 1200`, `DPMM = DPI/INCH`,
  `src/engraving/dom/mscore.h:66-71`; Page settings converts with `DPMM`/`DPI`,
  `pagesettings.cpp:466-470`); `P_TYPE::SPATIUM` ids are in staff spaces (`Spatium`,
  e.g. `minMeasureWidth` `8.0_sp`, `styledef.cpp:143`); enums are ints.

**Validation before mutation.**
| Check | Source | Error |
|---|---|---|
| Target score exists | SL1 | `E_NO_PART` |
| Name maps to a Sid, `0 <= sid < Sid::STYLES` | `styledef.h:51`, `2191` | `E_UNKNOWN_STYLE` |
| JSON value converts to `MStyle::valueType(sid)` | `style.cpp:68-84`, `788-791` | `E_STYLE_TYPE` |
| Deny `defaultsVersion` (changes the default-resolution version, `editstyle.cpp:122-124`) | | `E_STYLE_READONLY` |
| Deny `chordDescriptionFile` unless the value is one of the bundled names: `ChordList::read` opens absolute paths from disk (`src/engraving/dom/chordlist.cpp:2146-2172`) | | `E_STYLE_VALUE` |
| `musicalSymbolFont`, `musicalTextFont`, `dynamicsFont`: name is in `engravingFonts()->fonts()`; unknown names silently fall back (`src/engraving/internal/engravingfontsprovider.cpp:69-78`) | | `E_STYLE_VALUE` |
| `spatium > 0` (`Score::spatiumChanged` divides by old/new, `engravingitem.cpp:234-239`) | | `E_STYLE_VALUE` |
| Page geometry consistent: `pagePrintableWidth <= pageWidth − leftMargin`; there is no right-margin Sid, the right margin is `pageWidth − pagePrintableWidth − leftMargin` (`src/engraving/dom/page.cpp:200`, `209`; layout uses `pagePrintableWidth`, `src/engraving/rendering/score/systemlayout.cpp:138`). Page settings recomputes `pagePrintableWidth` whenever width or a margin changes (`pagesettings.cpp:254-258`, `351-357`, `380-405`, `426-439`) | | `E_STYLE_VALUE` |

**Post-condition.** `score->style().styleV(sid) == value` for every id; for
`concertPitch`, also the style flag. `ChangeStyleValues::flip` silently skips unchanged
values, so an unchanged request is a no-op, not an error. Report the ids whose value
actually changed (they also appear in `ScoreChanges::changedStyleIdSet`, `undo.cpp:637-641`).

**Nested commands.** None (SL2). `cmdConcertPitchChanged` calls
`Transpose::transposeKeys` and `Transpose::undoTransposeHarmony` (`score.cpp:2945`,
`2959`), no startCmd/endCmd. Safe under the lock, provided the agent does **not** go
through `NotationStyle` (SL3).

**Linked parts / excerpts.** Not propagated (SL1). Each score has its own `MStyle`; a
master change does not reach parts and vice versa. "Apply to all parts" in the UI is
available only from a part (`canApplyToAllParts` = `!isMaster()`,
`notationstyle.cpp:82-85`) and copies the **entire** style of that part onto every
excerpt with `ChangeStyle` (`notationstyle.cpp:87-99`), which also calls `update()` per
excerpt (`97`). For an agent "all parts" option, loop `undoChangeStyleValues` over
`masterScore()->excerpts()` instead (as Page settings does, `pagesettings.cpp:333-341`);
this changes only the requested ids. `cmdConcertPitchChanged` only transposes the staves
of the score it is called on (`score.cpp:2927`).

**Crash and corruption risks.**
- Wrong `P_TYPE` stored silently in release builds (`style.cpp:68-84`); debug builds
  assert on read (`style.h:50-59`). Validation is the only guard.
- `cmdConcertPitchChanged` has `assert(!h->realizedHarmony().valid())`
  (`score.cpp:2965`), debug only.
- Turning mmrests off removes system locks that contain mmrests from inside
  `ChangeStyleValues::flip` (`editstyle.cpp:125-129`). Those removals are pushed as new
  undo commands from within a flip; on a later redo of the macro, `pushAndPerform` with no
  active macro performs them unrecorded (`undo.cpp:309-320`). Upstream behaviour, same as
  the UI; consequence: undo/redo of "mmrests off" may not restore locks exactly. Scenario
  SL-STY-06 checks it.
- Musical-font coupling: when the music font changes, the dialog also sets
  `dynamicsFont` (unless `dynamicsOverrideFont`) and, if "optimize" is ticked, every
  engraving default of the new font (`editstyle.cpp:2253-2266`). The engraving layer does
  none of that. Whether set_style should mirror it is OQ-SL2.

**Existing upstream tests.**
- `src/engraving/tests/measure_tests.cpp:483-494` (`createMultiMeasureRests` via
  `undoChangeStyleVal` in startCmd/endCmd), `500-560` (measure-number style ids).
- `src/engraving/tests/readwriteundoreset_tests.cpp:86`, `96` (mmrest style + undo).
- `src/engraving/tests/system_dividers_tests.cpp:47` (`undoChangeStyleValues` with two ids).
- `src/engraving/tests/fretdiagram_tests.cpp:77`, `src/engraving/tests/timesig_tests.cpp:503`.
- None exercise `NotationStyle`, concert pitch, spatium or part styles.

**Scenarios.**
- SL-STY-01: set `spatium` to 1.5 mm on the master; offsets of manually moved items scale; undo restores byte-for-byte export.
- SL-STY-02: set `minMeasureWidth` and `measureSpacing` in one call; one undo entry; `changedStyleIdSet` holds both.
- SL-STY-03: set `concertPitch=true` on a score with a Bb clarinet and chord symbols; key signature and chord symbols of the clarinet staff change; undo restores.
- SL-STY-04: set `createMultiMeasureRests=false` on a part only; master unchanged.
- SL-STY-05: batch: insert 2 measures, then set a style value, then add a dynamic; ScoreChanges covers all three steps (guards SL3).
- SL-STY-06: part with system locks containing mmrests; set `createMultiMeasureRests=false`; locks removed; undo then redo; compare locks.
- SL-STY-07: unknown Sid → `E_UNKNOWN_STYLE`; wrong type (string for `spatium`) → `E_STYLE_TYPE`; score untouched.
- SL-STY-08: page margins: set odd left margin; printable width adjusted so the right margin is preserved.

**Open questions.** OQ-SL1, OQ-SL2, OQ-SL6 below.

---

### set_measure_stretch  ·  command://notation/set_measure_stretch

**UI entry point.** Three UI paths, all ending in `undoChangeProperty(Pid::USER_STRETCH, …)`
on a `Measure`:
1. Format → Stretch → Increase / Decrease layout stretch (`stretch+` / `stretch-`, default
   shortcuts `}` / `{`): `NotationActionController::addStretch(±STRETCH_STEP)` with
   `STRETCH_STEP = 0.1` (`src/notationscene/internal/notationactioncontroller.cpp:47`,
   `368-369`), which returns unless the selection is a range (`1733-1751`), then
   `NotationInteraction::addStretch` (`src/notation/internal/notationinteraction.cpp:6317-6324`):
   `startEdit("Increase/Decrease layout stretch")` → `Score::cmdAddStretch(value)` →
   `apply()`. `Score::cmdAddStretch` (`src/engraving/editing/cmd.cpp:2584-2605`) loops
   `firstMeasureMM()/nextMeasureMM()` over measures with `tickStart <= tick < tickEnd`,
   computes `max(0, m->userStretch() + val)` and calls
   `m->undoChangeProperty(Pid::USER_STRETCH, stretch)`.
2. Reset layout stretch (`reset-stretch`, `notationactioncontroller.cpp:371`, `1753-1771`):
   `NotationInteraction::resetStretch` (`notationinteraction.cpp:6531-6536`) →
   `Score::resetUserStretch` (`cmd.cpp:2494-2523`): from the selection's start/end segment
   measures (or first/last MM measure when there is no selection), `undoChangeProperty(USER_STRETCH, 1.0)`.
3. Measure properties dialog: `MeasurePropertiesDialog::apply`
   (`src/notationscene/widgets/measureproperties.cpp:308-346`) sets an **absolute** value
   with `m_measure->undoChangeProperty(Pid::USER_STRETCH, layoutStretch->value())` (`329`),
   spin box minimum 0, step 0.1, default 1.0 (`measureproperties.ui:359-375`).

The Inspector's Appearance section also has a "measure width" item bound to
`Pid::USER_STRETCH` in percent (`src/inspector/qml/MuseScore/Inspector/general/appearance/appearancesettingsmodel.cpp:47-53`,
`151-155`), via `AbstractInspectorModel::setPropertyValue` → `undoChangeProperty`
(`abstractinspectormodel.cpp:471-496`). How measures reach that list was not traced (OQ-SL4).

**Recommended agent call:** for each target `Measure* m`,
`m->undoChangeProperty(Pid::USER_STRETCH, value)` (path 3, explicit, no selection). For a
relative change, compute `max(0.0, m->userStretch() + delta)` as `cmdAddStretch` does.
Calling `Score::cmdAddStretch` instead would need a range selection on the target score
(F6) and would act on mmrest measures (below).

Property facts: `Measure::getProperty(USER_STRETCH)` returns `userStretch()`
(`src/engraving/dom/measure.cpp:2895-2896`), which is `1.0` in `LayoutMode::FLOAT`
(`measure.cpp:3250-3253`; FLOAT is dev-mode only,
`src/appshell/qml/MuseScore/AppShell/notationstatusbarmodel.cpp:197`). `setProperty`
stores the double and `triggerLayout()`s (`measure.cpp:2925-2932`). Default 1.0
(`measure.cpp:2951-2952`). Layout clamps the effective value to `[0.1, 10.0]`
(`src/engraving/rendering/score/horizontalspacing.cpp:735`). Saved as `<stretch>`
(`src/engraving/rw/write/measurewrite.cpp:68`, `property.cpp:255`).

**Selection set-up.** None for the recommended call. (The UI's increase/decrease needs a
range selection, `notationactioncontroller.cpp:1745-1747`, `cmd.cpp:2586-2588`.)

**Arguments.**
```json
{
  "type": "object",
  "required": ["from", "to"],
  "properties": {
    "target": { "type": "string", "default": "score" },
    "from":   { "type": "integer", "minimum": 1, "description": "measure (structure G8 / OQ-2)" },
    "to":     { "type": "integer", "minimum": 1 },
    "value":  { "type": "number", "minimum": 0, "description": "absolute stretch, 1.0 = default" },
    "delta":  { "type": "number", "description": "relative change; UI step is 0.1" },
    "reset":  { "type": "boolean", "description": "set 1.0 (Reset layout stretch)" }
  },
  "oneOf": [ {"required": ["value"]}, {"required": ["delta"]}, {"required": ["reset"]} ]
}
```

**Validation before mutation.**
| Check | Source | Error |
|---|---|---|
| Target score exists | SL1 | `E_NO_PART` |
| `from <= to`, both resolve to `Measure`s of the target score | structure G8 | `E_RANGE` |
| `value >= 0` (UI minimum 0; `cmdAddStretch` floors at 0) | `measureproperties.ui:366-368`, `cmd.cpp:2599-2601` | `E_RANGE` |
| Warn (not error) when outside `[0.1, 10]`: layout clamps | `horizontalspacing.cpp:735` | warning |
| mmrest policy (SL5, OQ-SL3) | | `E_MMREST` or warning |

**Post-condition.** For each measure, `m->getProperty(Pid::USER_STRETCH).toDouble()` equals
the requested value (in PAGE/LINE/SYSTEM modes this is the stored value,
`measure.cpp:3250-3253`).

**Nested commands.** None. Safe under the lock. `undoChangeProperty` returns early when the
value and flags are unchanged (`src/engraving/dom/engravingobject.cpp:440-442`).

**Linked parts / excerpts.** Not propagated (SL1): `USER_STRETCH` is
`PropertyGroup::NONE`, `link=false` (`property.cpp:255`), so even linked measures in parts
get `PropertyPropagation::NONE` (`engravingitem.cpp:1477-1479`), and an edit from a part
does not unlink anything. Stretch is set per score; the UI acts on the viewed score.
Measures in parts may also be unlinked copies (structure G5), which changes nothing here.

**Crash and corruption risks.**
- **Multimeasure rests.** With mmrests on, the UI's `cmdAddStretch` changes the stretch of
  the **mmrest measure** (it walks `nextMeasureMM`, `cmd.cpp:2590`). An mmrest measure is
  created by layout with the constructor default (`measure.cpp:194`,
  `measurelayout.cpp:228-231`), `createMMRest` does not copy the underlying measures'
  stretch (`measurelayout.cpp:233-238`), and the writer walks the underlying measure list
  from `score->first()` (`src/engraving/rw/write/writer.cpp:200`). Reading these together:
  the UI's change on an mmrest is not saved, and a change to the underlying measures has no
  visible effect while the mmrest is displayed. Verify with scenario SL-STR-04 (OQ-SL3).
- No crash paths found. `Score::resetUserStretch` with no selection falls back to the whole
  score (`cmd.cpp:2503-2513`); the agent should not rely on it.

**Existing upstream tests.** None: `git grep` finds no `USER_STRETCH`, `cmdAddStretch` or
`resetUserStretch` in `src/engraving/tests` or `src/notation/tests`.

**Scenarios.**
- SL-STR-01: set bars 5–8 to 1.5; bars 5–8 report 1.5, others 1.0; export shows wider bars; undo restores.
- SL-STR-02: delta −0.3 three times on a bar at 0.5 → floors at 0.0 (matches `cmdAddStretch`), warning about the layout clamp.
- SL-STR-03: set stretch in the master; the part's linked bars keep 1.0; then set in the part; master unchanged.
- SL-STR-04: part with mmrests on: stretch a range inside an mmrest; report policy result; save/reload and compare.
- SL-STR-05: reset bars 1–4 after SL-STR-01; all 1.0.

**Open questions.** OQ-SL3, OQ-SL4.

---

### lock_systems  ·  command://notation/lock_systems

**UI entry point.** All system-lock edits are static functions of
`mu::engraving::EditSystemLocks` (`src/engraving/editing/editsystemlocks.h:36-59`), reached
through thin `NotationInteraction` wrappers (`notationinteraction.cpp:5948-6011`), each
`startEdit(name)` → function → `apply()`, no `checkAndShowError`:

| Action code (`notationactioncontroller.cpp:274-279`; titles `notationuiactions.cpp:646-669`, `1654-1665`) | Wrapper | Engraving function | Inputs |
|---|---|---|---|
| `make-into-system` "Create system from selection" | `makeIntoSystem` (`5984-5995`) | `makeIntoSystem(score, first, last)` (`editsystemlocks.cpp:247-283`) | `selection().startMeasureBase()/endMeasureBase()` |
| `apply-system-lock` "Add/remove system lock" | `applySystemLock` (`5997-6002`) | `applyLockToSelection(score)` (`349-390`) | selection; removes the lock if `last` ends one, else `makeIntoSystem(first,last)`, or `makeIntoSystem(first->system()->first(), last)` for a single measure |
| `toggle-system-lock` "Lock/unlock selected system(s)" | `toggleSystemLock` (`5970-5975`) | `toggleSystemLock(score, selectedSystems)` (`138-159`) | `Selection::selectedSystems()` (`select.cpp:409-430`) |
| `toggle-score-lock` "Lock/unlock all systems" | `toggleScoreLock` (`5977-5982`) | `toggleScoreLock(score)` (`161-189`) | `score->systems()` |
| Format → Measures per system… (Breaks dialog) | `addRemoveSystemLocks(type, n)` (`6004-6011`) | `addRemoveSystemLocks(score, interval, lock)` (`191-245`) | selection range; `lock=true` locks every current system in range; otherwise removes locks contained in the range and, if `interval>0`, adds one lock per `interval` measures |
| `move-measure-to-prev-system` / `-next-system` | `moveMeasureToPrevSystem/NextSystem` (`5948-5968`) | same names (`285-347`) | selection end/start measure |

The Breaks dialog (`src/notationscene/widgets/breaksdialog.cpp:59-84`) calls
`interaction->selectAll()` when there is no range (`90-108`). That is the P8 hazard
(`Score::update` mid-transaction); the agent must not copy it.

Primitives: `undoAddSystemLock(score, lock)` first removes line/no-breaks inside the lock
and section/page breaks before its last measure (`removeLayoutBreaksOnAddSystemLock`,
`editsystemlocks.cpp:119-123`, `404-415`, via `MeasureBase::undoSetBreak`,
`src/engraving/dom/measurebase.cpp:510-557`), then pushes `AddSystemLock` (`38-73`);
`undoRemoveSystemLock` pushes `RemoveSystemLock` (`79-113`, `125-128`).

**Recommended agent calls** (explicit arguments, no selection):
- mode `system` (the musical intent "these bars on one line"):
  `EditSystemLocks::makeIntoSystem(score, first, last)`. It trims or removes overlapping
  locks before adding the new one (`251-282`). Layout-independent apart from mmrest
  neighbours (SL4).
- mode `unlock`: for each lock in `score->systemLocks()->locksContainedInRange(first, last)`
  (`src/engraving/dom/systemlock.cpp:80-95`) `EditSystemLocks::undoRemoveSystemLock`,
  which is the `interval == 0, lock == false` branch of `addRemoveSystemLocks`
  (`editsystemlocks.cpp:220-227`). Locks that only partly overlap the range are not
  touched by the UI either.
- mode `every` (n bars per system): `addRemoveSystemLocks(score, n, false)` needs the
  selection; either set a range selection on the target score (F6, then restore) or
  repeat its loop (`220-244`) with explicit endpoints. Same engraving primitives either way.
- mode `freeze` (lock the current line breaks): `addRemoveSystemLocks(score, 0, true)` or
  `toggleScoreLock`. **Layout-dependent** (SL4): only valid when every measure in range
  has been laid out in this transaction's starting layout.

The layout honours locks only in Page and Single-page view modes
(`src/engraving/rendering/score/systemlayout.cpp:150-151`); a lock forces the measures onto
one system even when they overflow it (`doBreak = !systemLock && …`, `206`) and ends the
system at the lock end or before the next lock start (`259-260`).

**Selection set-up.** None for `system` and `unlock`. For `every` via the UI function, a
range selection on the **target score** (each score has its own selection) covering
`first..last`; restore afterwards.

**Arguments.**
```json
{
  "type": "object",
  "required": ["mode"],
  "properties": {
    "target": { "type": "string", "default": "score", "description": "SL1" },
    "mode":   { "enum": ["system", "unlock", "every", "freeze"] },
    "from":   { "type": "integer", "minimum": 1 },
    "to":     { "type": "integer", "minimum": 1 },
    "every":  { "type": "integer", "minimum": 1, "description": "mode=every only" }
  }
}
```
Horizontal frames (`HBox`) are valid lock ends (`src/engraving/dom/systemlock.h:37-38`);
whether `from/to` can name frames depends on structure OQ-2.

**Validation before mutation.**
| Check | Source | Error |
|---|---|---|
| Target score exists | SL1 | `E_NO_PART` |
| `from`, `to` resolve to `Measure` or `HBox` **of the target score** (the lock is stored in `startMB()->score()`, `editsystemlocks.cpp:55`, while lookups use the `score` argument, `251-252`) | `systemlock.h:37-38` (assert only) | `E_RANGE` |
| `first == last` or `first->isBefore(last)` | `systemlock.h:39` (assert only) | `E_RANGE` |
| With mmrests on, map `from/to` to the covering mmrest measures the way the UI does (`tick2measureMM`, `select.cpp:383`, `404`) or refuse (OQ-SL3) | SL5 | `E_MMREST` |
| `freeze` / any use of `system()`: every measure in range has a real system (`mb->system() != score->dummy()->system()` and non-empty `measures()`) | SL4 | `E_NEEDS_LAYOUT` |
| `every >= 1` | `editsystemlocks.cpp:225-244` | `E_RANGE` |

**Post-condition.** On the target score after the call (before layout):
- `system`: `score->systemLocks()->lockStartingAt(first)` is non-null and its `endMB() == last`
  (`systemlock.cpp:57-61`); no other lock contains a measure in `first..last`.
- `unlock`: `locksContainedInRange(first, last)` is empty.
- `every`: each lock in range spans `every` measures except possibly the last.
After commit, layout may re-anchor locks on mmrest measures
(`measurelayout.cpp:241` → `src/engraving/rendering/score/layoutcontext.cpp:465-470` →
`editsystemlocks.cpp:458-501`), so read-back after commit should compare by tick range,
not by pointer. Report the resulting systems (first/last measure of each) from the
post-commit layout.

**Nested commands.** None (SL2). Safe under the lock. Note that layout itself pushes lock
changes during the commit's `update(false)` (`cmd.cpp:426`, before `endMacro` at `436`),
so they land in the agent's macro.

**Linked parts / excerpts.** Not propagated (SL1). A lock made in the master does not
affect any part and vice versa; to lock parts, call per excerpt score with that score's
measures. Because parts default to mmrests on (SL5), part locks usually start or end on
mmrest measures. Breaks removed by `undoAddSystemLock` are removed only from the target
score's measures (`measurebase.cpp:549-557` uses `score()->undoAddElement` / cleanup on
`this`); whether layout breaks themselves propagate is covered by structure add_break.

**Crash and corruption risks.**
- **Dummy-system UB** (SL4): `toggleSystemLock`, `toggleScoreLock`,
  `addRemoveSystemLocks(lock=true)`, `moveMeasureTo*System`, `applyLockToSelection` with one
  measure (`editsystemlocks.cpp:388`) call `System::first()/isLocked()` on the measure's
  parent; for a measure created earlier in the same transaction that is the dummy system
  with an empty measure list (`system.h:140-141`, `system.cpp:286-289`). Do not offer
  these on such measures; `makeIntoSystem` with explicit endpoints avoids it.
- **Silent duplicate:** `SystemLocks::add` uses `std::map::emplace` keyed by start measure
  (`systemlock.cpp:39-46`); adding a second lock with the same start is silently dropped,
  and undoing that `AddSystemLock` later erases the *other* lock by key
  (`systemlock.cpp:48-55`). Never push `undoAddSystemLock` without first removing locks
  that start at or overlap the range; `makeIntoSystem` does this (`251-279`). The sanity
  check that would catch it is debug-only (`systemlock.cpp:107-129`).
- `SystemLock`'s constructor checks types and order with `assert` only
  (`systemlock.h:37-39`); release builds accept a VBox or reversed range.
- `updateSystemLocksOnCreateMMRests` asserts `startMB->isAfter(first) && endMB->isAfter(last)`
  (`editsystemlocks.cpp:496`), debug only; reached during layout with mmrests.
- Rollback leaks: `UndoCommand::unwind` undoes and deletes child commands without
  `cleanup` (`src/engraving/editing/undo.cpp:242-250`), so an `AddSystemLock` rolled back
  that way leaks its `SystemLock` (freed only in `cleanup`, `editsystemlocks.cpp:59-65`).
  Memory only.
- Persistence: locks are written as the start/end measures' EIDs
  (`src/engraving/rw/write/twrite.cpp:527-535`) and read back through the EID register
  (`src/engraving/rw/read410/tread.cpp:4672-4696`; a missing measure hits
  `IF_ASSERT_FAILED` and the lock is dropped, `4691-4693`). Whether a lock anchored on an
  mmrest measure survives save/reload is OQ-SL5.
- Interactions: adding a line/page/section break inside a lock removes the lock
  (`cmd.cpp:4978-5013` → `editsystemlocks.cpp:392-402`); deleting measures trims locks
  (`edit.cpp:7952` → `editsystemlocks.cpp:417-440`); "Reset to default layout" removes all
  locks (`cmd.cpp:2739`); deleting a lock indicator removes its lock (`edit.cpp:3334-3335`).

**Existing upstream tests.** `src/engraving/tests/system_locks_tests.cpp` with fixture
`src/engraving/tests/system_locks_data/system_locks-1.mscx`: read (`40-65`),
`addRemoveSystemLocks` remove/lock/every-4 (`67-119`, uses `cmdSelectAll` inside its own
startCmd/endCmd), `makeIntoSystem` (`121-145`), move prev/next (`147-174`),
`toggleSystemLock` / `toggleScoreLock` (`176-`). All on the master, none with parts or
mmrests.

**Scenarios.**
- SL-LCK-01: `system` bars 3–6 in a score laid out 4 per line; bars 1–2, 3–6, 7–… form systems; neighbours' locks trimmed (mirror upstream `makeIntoSystem` test).
- SL-LCK-02: `every` 4 on bars 1–16; four locks of 4 bars; existing line breaks inside are removed.
- SL-LCK-03: `unlock` bars 1–16 after SL-LCK-02; no locks; undo restores them.
- SL-LCK-04: `system` bars 9–12 in a part with mmrests on, bars 9–12 empty; lock anchored as the UI would; save, reload, compare (OQ-SL5).
- SL-LCK-05: batch: insert 2 bars at bar 5, then `freeze` bars 1–8 → `E_NEEDS_LAYOUT`, batch rolled back, no crash.
- SL-LCK-06: master lock does not change the part's systems.
- SL-LCK-07: `system` with `from > to` → `E_RANGE`; with a VBox endpoint → `E_RANGE`.

**Open questions.** OQ-SL3, OQ-SL5, OQ-SL7.

---

### set_offset  ·  command://notation/set_offset

**UI entry point.**
1. Inspector → Appearance → Offset (X/Y fields, in staff spaces or mm):
   `AppearanceSettingsModel` builds a point item for `Pid::OFFSET`
   (`src/inspector/qml/MuseScore/Inspector/general/appearance/appearancesettingsmodel.cpp:59-65`)
   → `AbstractInspectorModel::setPropertyValue` (`abstractinspectormodel.cpp:471-496`):
   one macro "Edit offset" (`beginCommand`/`endCommand`, `867-885`); for each item, flags
   `STYLED` become `UNSTYLED` (`486-490`), value converted by `valueToElementUnits`
   (`613-632`: `P_TYPE::POINT` × `element->spatium()` if `offsetIsSpatiumDependent()`, else ×
   `DPMM`), then `item->undoChangeProperty(Pid::OFFSET, value, flags)` (`492`). Reset uses
   `undoResetProperty(Pid::OFFSET)` (`504-523`). For a selected Note, Stem or Hook, the
   Appearance offset is applied to the **parent Chord** (`appearancesettingsmodel.cpp:83-104`);
   the Notehead section has its own `Pid::OFFSET` item for the note itself
   (`src/inspector/qml/MuseScore/Inspector/notation/notes/noteheads/noteheadsettingsmodel.cpp:97`).
2. Keyboard nudge (arrow keys on a selected text or articulation):
   `NotationInteraction::nudge` (`notationinteraction.cpp:4245-4280`, called from
   `notationactioncontroller.cpp:1121`, `1180`): `undoChangeProperty(OFFSET, offset ± step·spatium, UNSTYLED)`.
3. Mouse drag: `EngravingItem::drag` sets the offset directly and marks it changed
   (`src/engraving/dom/engravingitem.cpp:2165-2182`); the edit-data machinery records it.
   Not a model for the agent.

**Recommended agent call:** the Inspector path:
`item->undoChangeProperty(Pid::OFFSET, PropertyValue::fromValue(PointF(x, y) * unit), flags)`
with `flags = (item->propertyFlags(OFFSET) == STYLED) ? UNSTYLED : item->propertyFlags(OFFSET)`,
`unit = item->offsetIsSpatiumDependent() ? item->spatium() : DPMM`. Reset:
`item->undoResetProperty(Pid::OFFSET)` (`src/engraving/dom/engravingobject.cpp:359-366`).

Units: the offset is stored in internal units (`m_offset`, `engravingitem.cpp:1149-1150`,
`1207-1208`). `offsetIsSpatiumDependent()` is `sizeIsSpatiumDependent() || ON_STAFF`
(`engravingitem.cpp:284-287`). `EngravingItem::spatium()` is the staff's spatium (staff
scaling included) for staff items and the score spatium for system items
(`engravingitem.cpp:258-265`). The plugin API always multiplies by `spatium()` (SL6).

**What `undoChangeProperty(OFFSET)` does beyond setting the value.**
- If the new value differs, it calls `setOffsetChanged(true, /*absolute*/false, new − old)`
  (`engravingobject.cpp:472-479`), which stores
  `ldata.autoplace.changedPos = item->pos() + diff` and `offsetChanged = RELATIVE_OFFSET`
  (`autoplace.cpp:384-392`), i.e. from the **current laid-out position**.
- At the next layout (the commit's `endCmd`, `cmd.cpp:426`), autoplace rebases the edit
  (`autoplace.cpp:42-45`, `131-134`, `213-216`): `rebaseOffset` (`281-331`) always resets
  `MIN_DISTANCE` to its style default (`315`, `324`, `329`); if a staff-relative item with a
  styled/unstyled `PLACEMENT` was moved across the staff it **flips placement** and
  shifts the offset by the staff height (`297-318`); and `rebaseMinDistance` (`341-381`) may
  rewrite `MIN_DISTANCE` (to −999 sp when moved into the staff, `352`) and the offset again
  (`362-372`). All these are `undoChangeProperty` calls made during layout, so they join the
  agent's macro. This happens whether or not autoplace is on for the item: the rebase is
  computed before the `item->autoplace()` check (`autoplace.cpp:42-51`).
- Then `changeProperties` sets `OFFSET` and its flags on the item and its links (below),
  and clears `GENERATED` (`engravingobject.cpp:493-496`).
- `EngravingItem::setProperty(OFFSET)` triggers layout (`engravingitem.cpp:1207-1208`,
  `1263`). `Rest` overrides it to also relayout the measure for full-measure rests
  (`src/engraving/dom/rest.cpp:652-657`).
- Spanners: the visible offset belongs to the **segment** (`SpannerSegment`); `OFFSET` is not
  in the segment's delegate list (`src/engraving/dom/spanner.cpp:136-151`), so setting it on
  the `Spanner` changes an unused value. Segments also have `OFFSET2` for the end point
  (`spanner.cpp:161-192`). Several segment types take the styled default from style
  (`src/engraving/dom/hairpin.cpp:276-280`, `src/engraving/dom/ottava.cpp:166-167`,
  `src/engraving/dom/textline.cpp:275-283`).

**Selection set-up.** None. Explicit element handle.

**Arguments.**
```json
{
  "type": "object",
  "required": ["handle"],
  "properties": {
    "handle": { "type": "string", "description": "element handle (EID + location); for lines, a segment handle (spanner + segment index)" },
    "x": { "type": "number", "description": "staff spaces (or mm when the item is not spatium-dependent)" },
    "y": { "type": "number" },
    "relative": { "type": "boolean", "default": false, "description": "add to the current offset (nudge semantics)" },
    "reset": { "type": "boolean", "description": "undoResetProperty(OFFSET)" },
    "end": { "type": "boolean", "default": false, "description": "set OFFSET2 of a line segment instead" }
  }
}
```
The response should state which unit applied (`offsetIsSpatiumDependent()`).

**Validation before mutation.**
| Check | Source | Error |
|---|---|---|
| Handle valid and re-validated against the live tree (CLAUDE.md rule 2) | | `E_STALE_HANDLE` |
| Element is an `EngravingItem` that is not a structural container (Measure, System, Page, Segment, Staff-level boxes) and not `generated()` | `cmd.cpp:163-165` skips generated items in the UI's reset | `E_NOT_MOVABLE` |
| Element is a spanner, not a segment → refuse or map to its first segment | `spanner.cpp:136-151` | `E_USE_SEGMENT` |
| Note/Stem/Hook: decide Inspector-Appearance semantics (move chord) vs notehead semantics | `appearancesettingsmodel.cpp:83-104`, `noteheadsettingsmodel.cpp:97` | — (schema choice, OQ-SL8) |
| Element was created or moved earlier in the same transaction (not laid out) | SL4, `autoplace.cpp:384-392` | `E_NEEDS_LAYOUT` |
| At most one set_offset per element per transaction (a second call stores `changedPos` from the stale position and only the second delta) | `engravingobject.cpp:476-477`, `autoplace.cpp:384-392` | `E_DUPLICATE` |
| Finite numbers; magnitude bound (e.g. ±100 sp) to keep items on the page; text is restricted to page bounds only when dragged (`engravingitem.cpp:2185-2200`) | | `E_RANGE` |

**Post-condition.** After commit (layout done), read back `getProperty(Pid::OFFSET)`,
`propertyFlags(OFFSET)`, `PLACEMENT` and `MIN_DISTANCE`. Because of the autoplace rebase,
the stored offset may differ from the request and the placement may have flipped; report
the final values, not the requested ones. Before commit, the only safe check is that
`getProperty(OFFSET)` equals the requested value.

**Nested commands.** None. Safe under the lock. `undoChangeProperty` returns without a
command when value and flags are unchanged (`engravingobject.cpp:440-442`).

**Linked parts / excerpts.** `Pid::OFFSET` is `PropertyGroup::POSITION`, `link=false`
(`src/engraving/dom/property.cpp:108`). `changeProperties` walks
`linkListForPropertyPropagation()` (`engravingobject.cpp:398-428`) and asks
`EngravingItem::propertyPropagation` (`engravingitem.cpp:1469-1530`):
- linked item in the **same score on another staff** (linked staves, e.g. standard + TAB):
  `NONE` for position properties (`1486-1497`);
- edit **from the master** to a part item: `PROPAGATE` only if the part item's position is
  still linked to master (`1525-1527`; `isPropertyLinkedToMaster` →
  `isPositionLinkedToMaster()`, `2637-2641`); system items propagate only from the top
  system object (`1520-1523`);
- edit **from a part**: `UNLINK` (`1514-1518`): the part item gets
  `POSITION_LINKED_TO_MASTER = false` (`unlinkPropertyFromMaster`, `2656-2663`) and the
  master is untouched. Later master edits no longer reach it until it is relinked
  (`setProperty(POSITION_LINKED_TO_MASTER, true)` copies the master's position values back,
  `engravingitem.cpp:1228-1236`, `1415-1442`).
- Spanner segments propagate only between first/last segments of linked spanners with the
  same placement; middle segments never (`spanner.cpp:356-380`). For segments,
  `POSITION_LINKED_TO_MASTER` lives on the spanner (`spanner.cpp:145`, `383-391`).
- Propagated copies get the value but not `setOffsetChanged`, so no autoplace rebase in the
  part (`engravingobject.cpp:474-478` acts on `this` only).

**Crash and corruption risks.**
- Stale layout data (SL4): `changedPos` uses `item->pos()`; for an item with no layout yet
  the rebase computes a wrong `p` (`autoplace.cpp:285-286`) and writes it back as an offset.
  Guarded by the `E_NEEDS_LAYOUT` / `E_DUPLICATE` checks.
- Placement flip (`autoplace.cpp:304-317`) is a side effect the user did not ask for; the
  response must report it.
- `NotationInteraction::nudge` has an `IF_ASSERT_FAILED` return after `startEdit`
  (`notationinteraction.cpp:4259-4261`), leaving the command open; not used by the agent.
- No upstream crash reports found for this path.

**Existing upstream tests.**
- `src/engraving/tests/system_dividers_tests.cpp:62-68`: `undoChangeProperty(Pid::OFFSET, …)`
  on a system divider inside startCmd/endCmd, save-compare.
- `src/engraving/tests/spanners_tests.cpp:656-699`: unstyled OFFSET on a pedal survives
  write/read (uses `setProperty`, not the undo path).
- `src/engraving/tests/parts_tests.cpp:1210-1248` (`partPropertyLinking`): editing a
  dynamic's placement in a part unlinks its position; relinking restores. Fixture
  `partPropertyLinking.mscx` is reusable for offset linking.

**Scenarios.**
- SL-OFF-01: dynamic below the staff, offset (0, 2) sp; read back y ≈ 2 sp, flags UNSTYLED, MIN_DISTANCE reset; undo restores STYLED.
- SL-OFF-02: same dynamic, offset (0, −8) sp (across the staff): placement flips to above, offset rebased; response reports both.
- SL-OFF-03: offset a staff text in the master; the part copy moves too; then offset it in the part; master unchanged and the part item now reports position unlinked.
- SL-OFF-04: hairpin over a system break: offset the second segment; only that segment moves; part's linked hairpin second segment unchanged.
- SL-OFF-05: `relative` nudge ×3 on a fingering (one call, delta 0.3 sp) equals UI nudge result.
- SL-OFF-06: add a dynamic and offset it in one batch → `E_NEEDS_LAYOUT`, batch rolled back.
- SL-OFF-07: reset offset on an item with a styled default (ottava); value equals style default × spatium.
- SL-OFF-08: chord symbol on a staff with 75 % staff scaling: offset 1 sp is 0.75 × score spatium internally.

**Open questions.** OQ-SL8, OQ-SL9.

---

## Open questions

- **OQ-SL1 (stray `Score::update()`).** `NotationStyle` setters call `update()` (SL3);
  other UI helpers do too (pitch P8). `Score::lockUpdates(true)` makes `update()` a no-op
  for that score (`cmd.cpp:470-472`). Whether the core should hold it during a batch (and
  release it before `commitChanges`, whose `endCmd` calls `update(false)`) is a core
  decision; side effects of `lockUpdates` beyond `update()` were not checked, and it is per
  score (an excerpt's `update()` checks the excerpt's flag).
- **OQ-SL2 (font coupling).** Should set_style for `musicalSymbolFont` also set
  `dynamicsFont` and the font's engraving defaults as the dialog does
  (`editstyle.cpp:2253-2266`), or stay a single-value edit?
- **OQ-SL3 (mmrests and measure ranges).** With mmrests on (default in parts), the UI's
  stretch and lock functions act on mmrest measures (SL5). For stretch the effect on an
  mmrest is (by reading) not saved; for locks the layout re-anchors them
  (`editsystemlocks.cpp:458-501`). Policy choice for the agent: (a) mirror the UI (resolve
  to mmrest measures via `tick2measureMM`), (b) address underlying measures only, or
  (c) refuse ranges inside an mmrest. Needs scenarios SL-STR-04 and SL-LCK-04.
- **OQ-SL4 (Inspector measure width).** `AppearanceSettingsModel` exposes `USER_STRETCH`
  (`appearancesettingsmodel.cpp:47-53`); how `Measure` objects get into the Inspector's
  element list for a range selection was not traced. Irrelevant if the agent uses the
  dialog path.
- **OQ-SL5 (locks on mmrests across save/reload).** A lock whose start or end is an mmrest
  measure is written by that measure's EID (`twrite.cpp:531-532`); mmrest measures are
  not in the written measure list (`writer.cpp:200`). Whether the EID resolves on reload
  (`tread.cpp:4680-4686`) or the lock is dropped (`4691-4693`) was not determined.
- **OQ-SL6 (page geometry API).** Expose raw page Sids (agent must keep
  `pagePrintableWidth` consistent) or a page sub-command with width/height/margins that
  derives `pagePrintableWidth` like Page settings (`pagesettings.cpp:311-331`)?
- **OQ-SL7 (lock modes).** Which modes to expose (`system`, `unlock`, `every`, `freeze`,
  move-measure-to-prev/next) and whether `freeze`/move should be allowed at all given the
  layout dependency (SL4) and structure OQ-1.
- **OQ-SL8 (offset target for notes).** Inspector Appearance moves the chord for a selected
  note/stem/hook; the Notehead section moves the note. Which does `set_offset` on a note
  handle mean?
- **OQ-SL9 (generic overlap).** `set_property(handle, "OFFSET", …)` in the `generic` group
  reaches the same `undoChangeProperty` and has the same autoplace and unit issues; the two
  groups should share one conversion (Inspector units, not the plugin's, SL6).
