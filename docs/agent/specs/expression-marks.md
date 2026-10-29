# Spec sheet: `expression-marks`

Group: `add_dynamic`, `add_hairpin`, `add_articulation`, `add_ornament`, `add_fermata`, `add_breath`.
Source: MuseScore Studio v4.7.6 (`agent/4.7` working tree). Every `file:line` below is in that tree.
Written by `api-scout` for P0.S.

Abbreviations: `nac` = `src/notationscene/internal/notationactioncontroller.cpp`,
`nua` = `src/notationscene/internal/notationuiactions.cpp`,
`ni` = `src/notation/internal/notationinteraction.cpp`,
`edit.cpp` / `cmd.cpp` = `src/engraving/editing/…`, other engraving files are under `src/engraving/dom/`.

Facts already established in other sheets are referenced, not repeated:
expression-lines **F1–F8** (`docs/agent/specs/expression-lines.md`), notes **G1–G10**
(`docs/agent/specs/notes.md`), text **§0.1–0.9** (`docs/agent/specs/text.md`).

---

## 0. Shared findings (apply to every command in this group)

**M1. How the UI reaches these edits.** There are three UI routes, and they behave differently:

| Edit | Keyboard / menu action | Palette (click with a selection) | Other |
|---|---|---|---|
| Dynamic | `add-dynamic` (Ctrl+D) → `toggleDynamicPopup` (`nac:338`, `ni:5880-5934`) | Dynamics palette (`src/palette/internal/palettecreator.cpp:238-262`) → `applyPaletteElement` | Dynamic popup (`src/notationscene/qml/MuseScore/NotationScene/elementpopups/dynamicpopupmodel.cpp:153-175`) |
| Hairpin | `add-hairpin`, `add-hairpin-reverse` (`nac:339-341`, `nua:1270-1281`) → `addHairpinsToSelection` (`ni:5573-5589`) | Dynamics palette, hairpins (`palettecreator.cpp:265-280`) | Popup "add hairpin to dynamic" (`dynamicpopupmodel.cpp:177-214`), grip drag from a dynamic (`ni:5537-5571`) |
| Articulation | `add-staccato/-tenuto/-sforzato/-marcato` (`nac:178-181`) → `NotationActionController::toggleArticulation` (`nac:946-974`) → `toggleArticulationForSelection` (`ni:5688-5725`); `add-up-bow/-down-bow` (`nac:473-474`) → `NotationInteraction::toggleArticulation` (`ni:8279-8282`) | Articulations palette (`palettecreator.cpp:689-771`) | — |
| Ornament | `add-turn`, `add-trill`, `add-mordent` … (`nac:458-471`) → `toggleOrnament` (`ni:8284-8287`) | Ornaments palette (`palettecreator.cpp:773-836`) | — |
| Fermata | none (no `fermata` action in `nac` or `nua`) | Breaths & pauses palette (`palettecreator.cpp:949-981`) | — |
| Breath / caesura | none | Breaths & pauses palette (`palettecreator.cpp:983-991`) | — |

**M2. The interaction-layer entry points, and which are usable inside the agent's transaction.**
- `applyPaletteElement` ends in `checkAndShowError()` (`ni:2342`) — unusable as-is (expression-lines F5).
- `toggleDynamicPopup` (both branches) and `addTextToItem` end in text-edit mode: `startEditText(dynamic)` after
  `apply()` (`ni:5900-5901`) or `addText` → `startEditText` (`ni:6241`). Under the lock `apply()` does not close
  the macro (F1), so a command is active when `TextBase::startEdit` runs, which asserts
  `!score()->undoStack()->hasActiveCommand()` (`src/engraving/editing/textedit.cpp:114`). `addTextToItem` also
  raises `NO_NOTE_REST_SELECTED` and shows a dialog on a bad target (`ni:6203-6205`). **Never use them.**
- `addHairpinsToSelection`, `toggleArticulationForSelection`, `toggleArticulation(SymId)`, `toggleOrnament(SymId)`
  call `startEdit`/`apply` only (`ni:5579-5581`, `ni:5720-5724`, `ni:8271-8277`); none calls `checkAndShowError`.
  They are callable under the lock (F1: wrapper transaction calls become no-ops, but `apply()` still fires
  `notationChanged` mid-transaction). All four are selection-driven (F6 primitives apply).
- The `NotationActionController` wrappers must not be used: `toggleArticulation` starts note input when nothing is
  selected (`nac:960-966`) and, in note input with the "add articulations to next note" preference, only stores the
  articulation in the input state (`nac:968-969`).

**M3. Recommendation: call the engraving functions the UI ends up in, with explicit targets.** Every engraving
function used below takes its target as an argument (a `ChordRest`, `Chord`, `BarLine` or `Dynamic`), contains no
`startCmd`/`endCmd`/`unwind()` (`git grep` over `chord.cpp`, `chordrest.cpp`, `rest.cpp`, `note.cpp`,
`barline.cpp`, `dynamic.cpp`, `ornament.cpp`, `articulation.cpp`, `hairpin.cpp`, `breath.cpp`, `fermata.cpp`, and
`edit.cpp:4290-4520`, `cmd.cpp:2130-2500` finds none) and no `MessageBox` (none in those files; the only
`MessageBox` uses in `src/engraving/editing` are `cmd.cpp:3879` and `edit.cpp:289`, both outside this group). The
palette's own target functions are `EngravingItem::drop(EditData&)` overrides; `EditData` can be built with no
view (`src/engraving/editing/editdata.h:268`, `EditData(MuseScoreView* v = nullptr)`), as the upstream tests do
(`src/engraving/tests/breath_tests.cpp:58-64`, `src/engraving/tests/textbase_tests.cpp:52-56`). The `drop`
branches used here read only `data.dropElement` (cited per command). The one exception is
`Score::addHairpin(Hairpin*, cr1, cr2)`, which reads the score selection (`edit.cpp:4380`, see `add_hairpin`).

**M4. `acceptDrop` is not needed and partly layout-dependent.** `ChordRest::acceptDrop` returns `true` at once for
FERMATA, BREATH and DYNAMIC (`chordrest.cpp:150-172`). For anything else it falls through to
`Measure::acceptDrop` (`chordrest.cpp:188`), which reads `system()` and staff bounding boxes
(`measure.cpp:1380-1395`) and returns `false` for measures not yet laid out in this transaction (G8). The agent
validates targets itself (per command) and must not gate on `acceptDrop`.

**M5. Palette elements arrive through a serialise/deserialise round trip.** `applyDropPaletteElement` re-creates the
palette element from `e->mimeData()` (`ni:2796-2805`), so every property the palette set survives: the dynamic's
type and text, the fermata's time stretch, the breath's pause. The agent must set the same properties on the
element it builds, before the drop (listed per command).

**M6. Voice assignment (dynamics and hairpins).** Only Dynamic, Expression and Hairpin have voice-assignment
properties (`dynamic.h:112`; text §0.6 for the others). The default is `ALL_VOICE_IN_INSTRUMENT` for both
(`textbase.cpp:2781-2782`, `hairpin.cpp:771-772`). After adding, the UI calls
`setInitialTrackAndVoiceAssignment(track, false)` (`ni:2816-2821` palette, `ni:6233-6235` add-text,
`edit.cpp:4325-4327` hairpin action, `ni:2482-2483` and `ni:2657-2658` palette hairpins). That function reads the
**user preference** `score/dynamicsApplyToAllVoices` (default `true`,
`src/engraving/internal/engravingconfiguration.cpp:53`, `115`, `343-346`): on → track = voice 1 of the staff,
`ALL_VOICE_IN_INSTRUMENT`; off → the CR's own track, `CURRENT_VOICE_ONLY` (`engravingitem.cpp:1317-1330`). It uses
plain `setTrack`/`setProperty`, not undo helpers, and runs after `undoAddElement` has already made the linked
clones, so clones in parts keep their pre-call track and assignment. Two UI paths set the assignment *before*
adding instead: the hairpin-grip dynamic (`ni:5895-5899`: `setTrack`, `setVoiceAssignment`, then
`undoAddElement`) and `addHairpinToDynamic` (`edit.cpp:4443-4445`). See Open questions Q2.

**M7. Snapping, flipping and other cross-element behaviour is layout-time.** Hairpin↔dynamic snapping is decided in
layout by `HairpinSegment::findStartDynamicOrExpression` / `findEndDynamicOrExpression`, which require the same
`track`, `placement` and `VOICE_ASSIGNMENT` on both (`hairpin.cpp:361-413`, `415-470`). The popup's
`autoFlipHairpinsType` (`ni:5837-5878`, called at `dynamicpopupmodel.cpp:166`) reads `dynamicSnappedBefore/After`,
which come from layout data (`hairpin.cpp:557-584`), so it cannot see hairpins or dynamics added earlier in the same
transaction (G8). Ornament auxiliary notes and cue notes are computed during layout (`chord.cpp:1186-1199` →
`ornament.cpp:251-332`, `393-429`, called from `src/engraving/rendering/score/modifydom.cpp:81` via
`measurelayout.cpp:999`), and the fermata glyph is flipped Above/Below in layout (`src/engraving/rendering/score/tlayout.cpp:1926-1935`).
Post-conditions must use model fields (`subtype()`, tick, track), never layout results.

**M8. Linked parts.** All six types go through `Score::undoAddElement` (`edit.cpp:6692`) with
`addToLinkedStaves = true`; all are in the per-staff list (`edit.cpp:6907-6908` ARTICULATION/ORNAMENT, `6917`
HAIRPIN, `6927` BREATH, `6928` DYNAMIC, `6945` FERMATA), so a clone goes to every staff in `ostaff->staffList()`
(`edit.cpp:6959`, loop `6968`). DYNAMIC and HAIRPIN are in `VOICE1_COPY_TYPES` (`edit.cpp:6973-6998`) and reach a
linked staff without the voice mapping when in voice 1 (`edit.cpp:7000-7007`); ARTICULATION, ORNAMENT, FERMATA and
BREATH are not, so a voice 2–4 mark is skipped on a linked staff with no linked voice. DYNAMIC is skipped on linked
staves whose `rstaff()` differs (`edit.cpp:7013-7036`). Branches: articulation family `edit.cpp:7054-7095`;
segment-parented (dynamic, fermata) `edit.cpp:7133-7172`; spanners (hairpin) `edit.cpp:7188-7245`; breath
`edit.cpp:7339-7352`. Edit the master score (G7).

**M9. Plugin API comparison (cautionary).** `Cursor::add` (`src/engraving/api/v1/cursor.cpp:271-430`):
- ARTICULATION/ORNAMENT → `Chord::addInternal` → `undoAddElement` (`cursor.cpp:335-347`,
  `src/engraving/api/v1/elements.cpp:367-379`): no duplicate check (a second staccato is added, not toggled), no
  double-articulation split, no anchor inference, silently nothing on a rest.
- DYNAMIC, FERMATA, BREATH → `default:` → `undoAddElement` on the cursor's segment with the cursor's track
  (`cursor.cpp:288-291`, `427-429`): no voice-assignment initialisation (a voice-2 dynamic keeps
  `ALL_VOICE_IN_INSTRUMENT`, the state `checkVoiceAssignmentCompatibleWithTrack` exists to prevent,
  `engravingitem.cpp:1332-1340`), no fermata duplicate check or placement by voice.
- BREATH via the cursor is re-parented by `undoAddElement`'s breath branch to a Breath segment at the cursor's
  **start** tick, i.e. before the note, not after it (`edit.cpp:7341-7347`). At a measure start it goes into the
  previous measure (`edit.cpp:7344-7346`); at tick 0, `tick2measure(0)` is the first measure
  (`src/engraving/dom/utils.cpp:90-91`), `prevMeasure()` is null and `m->undoGetSegment` at `edit.cpp:7347`
  dereferences null. The UI path never reaches this because it passes `cr->endTick()` (> 0).
- Hairpins: `Cursor::add` has no spanner branch; `cmd("add-hairpin")` dispatches the UI action
  (`src/engraving/api/v1/qmlpluginapi.cpp:408-425`) → `addHairpinsToSelection`. See Open questions Q12.

**M10. Proposed error codes (in addition to text §0.9: `E_NO_SCORE`, `E_RANGE`, `E_BAD_STAFF`, `E_BAD_VOICE`,
`E_NO_CHORDREST`, `E_BAD_ARG`, `E_POSTCONDITION`).** `E_NOT_A_CHORD` (articulation/ornament on a rest; the UI
silently ignores rests), `E_GRACE` (target is a grace chord where the UI result is undefined, see per command),
`E_EXISTS` (duplicate mark, where the conductor chooses refusal over replace/no-op), `E_NO_BARLINE` (fermata on a
barline that is not an end barline), `E_EMPTY_SPAN` (hairpin end ≤ start), `E_ENGRAVING` (`MScore::_error` set by
the call, G2).

**M11. No upstream gtests exercise the interaction or palette paths for this group** (notation tests contain only
mocks: `git ls-files src/notation/tests`). Engraving-level coverage is listed per command.

---

### add_dynamic  ·  command://notation/add_dynamic

**UI entry point.**
- Palette (main UI route): Dynamics palette builds each element with `Dynamic::setDynamicType(String)`
  (`palettecreator.cpp:259-262`), which parses the name and sets both type and XML text
  (`dynamic.cpp:230-240`, `242-259`). `applyPaletteElement` → list selection: `filterTargetElements` returns the
  selected elements (`src/engraving/dom/utils.cpp:1811-1917`) → `applyDropPaletteElement` per target
  (`ni:2521-2525`); range selection: the TextBase branch drops one dynamic per staff on
  `firstSegment->firstElementForNavigation(staff)` (`ni:2665-2679`). Target `Note::drop` forwards to
  `Chord::drop` (`note.cpp:2099-2111`) → `ChordRest::drop` (`chord.cpp:1597-1598`); a `Rest` forwards to
  `ChordRest::drop` (`rest.cpp:236-237`). `ChordRest::drop` DYNAMIC (`chordrest.cpp:285-290`): `setTrack(cr
  track)`, `checkVoiceAssignmentCompatibleWithTrack()`, parent = CR segment, `undoAddElement`. Then
  `setInitialTrackAndVoiceAssignment(target->track(), Ctrl)` (`ni:2816-2821`) and select (`ni:2823-2825`; Dynamic
  does not override `needStartEditingAfterSelecting`, `engravingitem.h:294`, so no edit mode).
- Palette dropped on an existing dynamic (selection = the dynamic): `Dynamic::drop` replaces type and text with
  `undoChangeProperty(DYNAMIC_TYPE)` then `undoChangeProperty(TEXT)` (`dynamic.cpp:318-325`).
- Ctrl+D: `add-dynamic` → `toggleDynamicPopup` → `addTextToItem(DYNAMICS)` (`ni:5933`) → `Score::addText`
  DYNAMICS (`edit.cpp:827-834`: empty Dynamic, `ChordRest::undoAddAnnotation`, `chordrest.cpp:1284-1295`) →
  `setInitialTrackAndVoiceAssignment` (`ni:6233-6235`) → text edit; the popup then sets TEXT and DYNAMIC_TYPE
  (`dynamicpopupmodel.cpp:159-162`) and auto-flips adjacent hairpins (`dynamicpopupmodel.cpp:166`). Typed text is
  committed by `TextBase::endEdit`: `undoChangeProperty(TEXT, xml)` and, for dynamics,
  `undoChangeProperty(DYNAMIC_TYPE, xml)` (string form → parsed, `textedit.cpp:239-243`, `dynamic.cpp:480-486`).
  Unusable inside a transaction (M2).

**Recommended call (palette parity, no edit mode):**
```
Dynamic* d = Factory::createDynamic(score->dummy()->segment());   // factory.h:297
d->setDynamicType(String(xmlOrName));          // dynamic.cpp:230-240: sets type + xml text (palette does this)
[d->setVelocity(v);]                           // dynamic.h:76, optional; palette elements carry it via M5
[d->setVoiceAssignment(va);]                   // only if Q2 resolves to "set before add" (precedent ni:5898)
EditData ed; ed.dropElement = d;
cr->drop(ed);                                  // ChordRest::drop DYNAMIC, chordrest.cpp:285-290
d->setInitialTrackAndVoiceAssignment(cr->track(), false);   // ni:2819 parity, unless Q2 says otherwise
```
`cr` is the target `ChordRest` (call `ChordRest::drop` on it directly; going through a `Note` reaches the same
branch, `note.cpp:2111`, `chord.cpp:1598`).

**Selection set-up.** None.

**Arguments.**
```json
{
  "type": "object",
  "required": ["staff", "at"],
  "properties": {
    "staff": {"$ref": "#/defs/staffRef"},
    "voice": {"type": "integer", "minimum": 1, "maximum": 4, "default": 1},
    "at": {"$ref": "#/defs/position"},
    "dynamic": {"enum": ["pppppp","ppppp","pppp","ppp","pp","p","mp","mf","f","ff","fff","ffff","fffff","ffffff",
                         "fp","pf","sf","sfz","sff","sffz","sfff","sfffz","sfp","sfpp","rfz","rf","fz","m","r","s","z","n"]},
    "text": {"type": "string", "description": "custom text, e.g. 'subito p'; plain text, escaped before parsing"},
    "velocity": {"type": "integer", "minimum": 1, "maximum": 127},
    "voice_assignment": {"enum": ["preference", "all_voices", "this_voice"], "default": "preference"},
    "if_exists": {"enum": ["replace", "error", "add"], "default": "replace"}
  },
  "oneOf": [{"required": ["dynamic"]}, {"required": ["text"]}]
}
```
The names are the `TConv` XML names (`src/engraving/types/typesconv.cpp:1362-1431`), which
`parseDynamicText` accepts (`dynamic.cpp:248-254`). `voice_assignment` and `if_exists` depend on Q1/Q2.

**Validation before mutation.**
- Score, staff, voice, measure/beat resolve (`E_NO_SCORE`, `E_BAD_STAFF`, `E_BAD_VOICE`, `E_RANGE`); a
  ChordRest at the exact tick on that track (`E_NO_CHORDREST`), resolved as in text §0.6 (not `findCR`).
- Grace chord as target: the UI addresses the parent chord (dynamic lands on the CR's segment, `chordrest.cpp:288`);
  resolve to the parent's tick or `E_GRACE`.
- `dynamic` must be one of the enum names; `text` must be non-empty (`E_BAD_ARG`). Escape plain text with
  `TextBase::plainToXmlText` (`textbase.h:318`) before `setDynamicType(String)`. A text with no dynamic token parses
  to `DynamicType::OTHER` (`dynamic.cpp:258`), which has velocity −1 in `DYN_LIST` (`dynamic.cpp:51`) and does not
  drive playback; report this as a warning or refuse (Q15).
- Existing Dynamic in `seg->annotations()` with the same track (and assignment): apply `if_exists` (Q1). The UI
  palette with a note selected adds a second dynamic (`chordrest.cpp:285-290` has no check).

**Post-condition.** The CR segment's `annotations()` contains `d` (`Segment::add` pushes DYNAMIC to
`m_annotations`, `segment.cpp:708`); `d->dynamicType()` and `d->xmlText()` as requested;
`d->track()` and `getProperty(Pid::VOICE_ASSIGNMENT)` match M6; `velocity()` if given (`dynamic.cpp:142-145`); one
linked clone per linked staff that M8 says should get one (`d->linkList()`).

**Nested commands.** None (M3). Safe under the lock. `MScore::_error` is not set by this path.

**Linked parts / excerpts.** Segment branch, `edit.cpp:7133-7172`; DYNAMIC is a voice-1 copy type and is skipped
on differing `rstaff` (M8). The palette order (track and `checkVoiceAssignmentCompatibleWithTrack` *before*
`undoAddElement`, `chordrest.cpp:286-289`) gives clones a voice-1 track when the assignment is all-voices; the
Ctrl+D order (`Score::addText` then `setInitialTrackAndVoiceAssignment`) leaves clones on the CR's voice with
`ALL_VOICE_IN_INSTRUMENT`. Use the palette order. `Pid::DYNAMIC_TYPE` is always-linked
(`src/engraving/dom/property.cpp:199`); `Pid::TEXT` follows text §0.3; `Pid::VELOCITY` is not always-linked
(`property.cpp:206`).

**Crash and corruption risks.**
- Text edit mode inside the transaction asserts (`textedit.cpp:114`, M2).
- `setInitialTrackAndVoiceAssignment` is not undoable and does not reach clones (M6). Undo still works because
  undo removes the element and redo re-adds the same object (`AddElement`).
- Do not call `autoFlipHairpinsType` mid-transaction expecting it to see new hairpins (M7).

**Existing upstream tests.** `src/engraving/tests/textbase_tests.cpp:49-58` (`addDynamic` helper: `new Dynamic`,
`setXmlText`, `ChordRest::drop` — exactly the recommended path, without type), `:60-65` (`createDynamic`,
fixture `textbase_data/createDynamic-ref.mscx`), `:67-110` (dynamic text editing, outside a command for
`startEdit`). `src/engraving/tests/dynamic_tests.cpp:41-80` (properties round-trip only).

**Scenarios.**
- EXM-DYN-01 `mf` on beat 1 of m1, voice 1, preference on → track 0, all-voices, type MF, text = `DYN_LIST` text.
- EXM-DYN-02 `p` on a voice-2 note, preference on vs off (track and assignment differ).
- EXM-DYN-03 custom text "subito p" → type P, text "subito <sym>dynamicPiano</sym>".
- EXM-DYN-04 custom text "dolce" → type OTHER (warning/refusal per Q15).
- EXM-DYN-05 velocity 100 on `f`; read back `VELOCITY`.
- EXM-DYN-06 dynamic on an existing dynamic's slot with `if_exists=replace` → one dynamic, new type.
- EXM-DYN-07 score with a part: clone exists in the part with matching type, track and assignment.
- EXM-DYN-08 dynamic on a rest.
- EXM-DYN-09 bad position (no CR at tick) → `E_NO_CHORDREST`, score unchanged, undo index unchanged.
- EXM-DYN-10 batch: `add_dynamic` then `undo` → both gone in one step.

**Open questions.** Q1, Q2, Q3, Q15.

---

### add_hairpin  ·  command://notation/add_hairpin

**UI entry point.**
- Action `add-hairpin` / `add-hairpin-reverse` (`nua:1270-1281`, `nac:339-341`) →
  `NotationInteraction::addHairpinsToSelection(type)` (`ni:5573-5589`) → `Score::addHairpins(type)`
  (`edit.cpp:4298-4330`):
  - multi-staff range: per staff, `firstChordRest`/`lastChordRest` on **voice 1** of that staff, then
    `addHairpin(type, cr1, cr2)`; a staff with no voice-1 CR gets `addHairpin(type, tickStart, tickEnd,
    staff2track)` (`edit.cpp:4303-4312`);
  - list or single-staff range: `getSelectedStartEndChordRests` (`edit.cpp:177-200`) takes the earliest and latest
    CR in `selection().elements()` across all voices, and sets `MScore::setError(NO_NOTE_REST_SELECTED)` if there is
    none (`edit.cpp:195-197`) → `addHairpin(type, cr1, cr2)` (`edit.cpp:4316-4322`);
  - then `setInitialTrackAndVoiceAssignment(hairpin->track(), false)` on each (`edit.cpp:4325-4327`).
- `Score::addHairpin(HairpinType, cr1, cr2)` (`edit.cpp:4332-4351`) creates the hairpin, sets begin/continue text
  "cresc."/"(cresc.)" or "dim."/"(dim.)" for the line types (`edit.cpp:4340-4346`), then
  `Score::addHairpin(Hairpin*, cr1, cr2)` (`edit.cpp:4374-4407`): track = `cr1->track()`, start = `cr1->tick()`;
  end = `selection().tickEnd()` **if the score selection is a range** (`edit.cpp:4380-4381`), else
  `cr2->endTick()` (or `cr1`'s end including ties when there is no `cr2`), then shortened to the first segment
  inside the last note's duration that carries a DYNAMIC on the same track (`edit.cpp:4383-4398`);
  `undoAddElement` (`edit.cpp:4406`).
- Palette (list selection of one CR or two CRs on one staff): `score->addHairpin(clone, cr1, cr2)` then
  `setInitialTrackAndVoiceAssignment(cr1->track(), Ctrl)` (`ni:2458-2486`). The palette hairpin elements are set
  only with `setHairpinType` (`palettecreator.cpp:272-280`), so palette cresc./dim. lines get their text from
  style, not from `edit.cpp:4340-4346`. Range selection: `cmdAddSpanner` per staff with voice-1 track and
  `startSegment`/`endSegment` ticks (`ni:2646-2662`, `cmd.cpp:603-633`).
- From a dynamic (popup): `Score::addHairpinToDynamic(hairpin, dynamic)` (`edit.cpp:4409-4446`): starts at the
  dynamic, ends at the end of the chord at/before it (or the measure end), stopping at the next dynamic, and copies
  the dynamic's voice assignment *before* `undoAddElement` (`edit.cpp:4443-4445`).

**Recommended call.** `Score::addHairpin(type, cr1, cr2)` (`score.h:974`, public) with the score selection
cleared first (`Score::deselectAll()`), so the end tick follows the list rule (`edit.cpp:4383-4398`), then
`hairpin->setInitialTrackAndVoiceAssignment(hairpin->track(), false)` for parity with `edit.cpp:4326` (or the Q2
alternative). For an explicit end tick that is not a CR end, `Score::addHairpin(type, sTick, eTick, track)`
(`edit.cpp:4353-4372`, `score.h:975`) — this variant ignores the selection and does not shorten at dynamics. Using
`addHairpinsToSelection` instead is possible (M2) but brings selection set-up, the cross-voice `cr1` choice and the
`NO_NOTE_REST_SELECTED` error.

**Selection set-up.** For the recommended call: the selection must **not** be a range, or `edit.cpp:4380-4381`
silently replaces the end with the selection's end. Save, clear, call, restore (G5). For the
`addHairpinsToSelection` alternative: list selection `{cr1, cr2}` (F6), or a range for multi-staff.

**Arguments.**
```json
{
  "type": "object",
  "required": ["kind", "staff", "from", "to"],
  "properties": {
    "kind": {"enum": ["cresc", "dim", "cresc_line", "dim_line"]},
    "staff": {"$ref": "#/defs/staffRef"},
    "staves": {"type": "array", "items": {"$ref": "#/defs/staffRef"}, "description": "optional: one hairpin per staff, like a multi-staff range"},
    "voice": {"type": "integer", "minimum": 1, "maximum": 4, "default": 1},
    "from": {"$ref": "#/defs/position", "description": "the first note under the hairpin"},
    "to": {"$ref": "#/defs/position", "description": "the last note under the hairpin (inclusive); hairpin ends at its end tick"},
    "end_tick_exact": {"type": "boolean", "default": false, "description": "treat `to` as the end tick itself (addHairpin by ticks)"},
    "voice_assignment": {"enum": ["preference", "all_voices", "this_voice"], "default": "preference"}
  }
}
```
`kind` maps to `HairpinType` `CRESC_HAIRPIN`, `DIM_HAIRPIN`, `CRESC_LINE`, `DIM_LINE` (`hairpin.h:32-38`).

**Validation before mutation.**
- Positions and staff resolve; CR at `from` and at `to` on the track (`E_NO_CHORDREST`); `from` ≤ `to`
  (`E_EMPTY_SPAN` if the resulting end tick ≤ start tick). Same staff for both ends (hairpins cannot be cross-staff:
  `Spanner::canBeCrossStaff` lists only slur, tie, arpeggio, chord bracket, glissando, note line,
  `spanner.cpp:825-838`; `setTrack2` is a no-op otherwise, `spanner.cpp:1440-1447`).
- `end_tick_exact`: the end must lie inside the score; whether it must coincide with a segment is Q4.
- Overlap with an existing hairpin on the same track: the UI allows it (no check in `edit.cpp:4374-4407`); Q5.
- Read `MScore::_error` after the call (G2); the recommended call does not set it, `addHairpins` can
  (`edit.cpp:196`).

**Post-condition.** A Hairpin `h` in `score->spannerMap().findOverlapping(tick, tick2)` (safe mid-transaction, F2)
with `tick()`, `tick2()`, `track()` (after M6), `hairpinType()`, and for line kinds `beginText()` "cresc."/"dim.".
No segments exist until commit (F2); `segmentsEmpty()` is expected to be true.

**Nested commands.** None (M3). Safe under the lock. `addHairpinsToSelection`'s post-apply grip edit is skipped
because `segmentsEmpty()` is true under the lock (`ni:5583`).

**Linked parts / excerpts.** Spanner branch (`edit.cpp:7188-7245`): clone per linked staff with `track` /
`track2` = linked track (`edit.cpp:7201-7204`). HAIRPIN is a voice-1 copy type (M8). `setInitialTrackAndVoiceAssignment`
after the add changes only the master (M6): with the preference on and a `cr1` in voice 2–4, the master moves to
voice 1 while part clones stay on the CR's voice with `ALL_VOICE_IN_INSTRUMENT`. With `cr1` in voice 1 there is no
difference. Snapping to dynamics in the part then depends on those fields matching (M7).

**Crash and corruption risks.**
- Selection-dependent end tick (`edit.cpp:4380`) — a leftover range selection gives a wrong hairpin, not an error.
- `HAIRPIN_TYPE` changes via `undoChangeProperty` also reset styled offsets (`engravingobject.cpp:443-459`);
  relevant only if a later command flips the type.
- Hairpins across systems need nothing at add time (F3). A hairpin ending on the first beat of the next system snaps
  across the break in layout (`dynamic.cpp:272-301`).

**Existing upstream tests.** `src/engraving/tests/hairpin_tests.cpp:37-52` (type round-trip only). Read-compat
fixtures with hairpins: `src/engraving/tests/compat114_data/hairpin.mscx`, `compat206_data/hairpin.mscx`. No test
calls `addHairpin*`.

**Scenarios.**
- EXM-HP-01 cresc from m1 b1 to m1 b4 (quarters) → tick 0, tick2 1920, CRESC_HAIRPIN.
- EXM-HP-02 dim spanning a system break (m4–m6): one spanner, two segments after commit.
- EXM-HP-03 add_dynamic `p` at m1 b1, add_hairpin m1 b1–b4, add_dynamic `f` at m2 b1, in one batch → after commit
  the hairpin's front and back segments snap to both dynamics (read `ldata()->itemSnappedBefore/After`).
- EXM-HP-04 `cresc_line` → begin text "cresc.", continue text "(cresc.)".
- EXM-HP-05 hairpin in voice 2 with preference on/off; check master and part clone fields (M6).
- EXM-HP-06 user has a range selection when the command runs → hairpin end still equals `to`'s end.
- EXM-HP-07 `staves: [1,2]` → one hairpin per staff.
- EXM-HP-08 `from` after `to` → `E_EMPTY_SPAN`, score unchanged.

**Open questions.** Q2, Q4, Q5, Q12.

---

### add_articulation  ·  command://notation/add_articulation

**UI entry point.** Three paths with different semantics:
1. **Keyboard staccato/tenuto/accent/marcato** (`nac:178-181`, `nua:2450-2476`) →
   `toggleArticulationForSelection(sym)` (`ni:5688-5725`): collects the selected notes (or the chord of a selected
   articulation, `ni:5694-5704`); mode = Remove if *every* note's chord already has `sym` (after normalising
   Below→Above and splitting combined symbols, `ni:3051-3066`), else Insert (`ni:5706-5707`); returns without
   editing if any note is a trill cue note (`ni:5710-5713`); then `Chord::updateArticulations({sym}, mode)` per
   chord (`ni:5721-5723`). `updateArticulations` (`chord.cpp:1759-1873`) first splits any combined articulation on
   the chord into its parts (`chord.cpp:1767-1793`); in Insert mode adding an accent removes a marcato and vice
   versa (`chord.cpp:1816-1828`), and new ones inherit a common explicit anchor (`chord.cpp:1860-1871`); Remove
   mode handles only accent, marcato, tenuto, staccato (`chord.cpp:1832-1859`). **Pressing staccato twice removes
   it.**
2. **Keyboard up-bow/down-bow** (`nac:473-474`) → `NotationInteraction::toggleArticulation(sym)` → `execute` →
   `Score::toggleArticulation(SymId)` (`cmd.cpp:2142-2166`): per selected note/chord (once per chord), build an
   Articulation and call `Score::toggleArticulation(el, a)` (`cmd.cpp:2443-2488`): if the chord already has the same
   `subtype()` it is **removed** (`cmd.cpp:2453-2457`); any Tapping on the chord is removed (`cmd.cpp:2459-2464`);
   a single articulation is added (`cmd.cpp:2466-2471`); a combined one is split and only the missing parts are
   added (`cmd.cpp:2473-2487`). Per-chord toggle: a mixed selection ends mixed.
3. **Palette** (`palettecreator.cpp:689-771`): list → `applyDropPaletteElement` on each selected note
   (`ni:2521-2525`; a selected articulation means its chord, `ni:2490-2500`); range → one note per chord, honouring
   the selection filter (`ni:2735-2748`, `2750-2769`), skipping barlines (`ni:2758`). `Note::drop` → `Chord::drop`
   (`note.cpp:2111`) → ARTICULATION branch (`chord.cpp:1473-1525`): if the chord already has the same subtype, the
   existing one is **selected and nothing changes** (`chord.cpp:1477-1483`); else the anchor is inferred from the
   chord's explicitly anchored articulations (`chord.cpp:1486-1512`); if the chord has no articulations the new one
   is added as-is with `undoAddElement` (`chord.cpp:1519-1520`), else via `Score::toggleArticulation(chord, a)`
   (`chord.cpp:1521-1522`).

`Chord::hasArticulation` compares `subtype()` (`chord.cpp:1735-1743`), and `Articulation::subtype` maps `…Below` to
`…Above` and strips `…Turned` (`articulation.cpp:117-130`), so Above/Below variants count as the same mark.

**Recommended call (palette semantics: add, never remove).** For each target chord:
```
Articulation* a = Factory::createArticulation(score->dummy()->chord());   // factory.h:63
a->setSymId(symAbove);
if (chord->hasArticulation(a)) { delete a; /* no-op or E_EXISTS, Q6 */ }
else { EditData ed; ed.dropElement = a; chord->drop(ed); }              // chord.cpp:1473-1525
```
The pre-check means `Chord::drop` never reaches its `score()->select(oa)` side effect (`chord.cpp:1483`). If the
conductor prefers keyboard semantics (toggle; accent/marcato replace), call `Chord::updateArticulations({sym},
mode)` directly (`chord.h:304`), with `mode` computed as `ni:5706-5707` does.

**Selection set-up.** None for the recommended call. (`toggleArticulationForSelection` and
`Score::toggleArticulation(SymId)` need a list selection of notes, F6.)

**Arguments.**
```json
{
  "type": "object",
  "required": ["articulation", "staff"],
  "properties": {
    "articulation": {"enum": ["accent","staccato","staccatissimo","tenuto","tenuto_staccato","marcato",
      "accent_staccato","marcato_staccato","marcato_tenuto","staccatissimo_stroke","staccatissimo_wedge","stress",
      "tenuto_accent","unstress","soft_accent","soft_accent_staccato","soft_accent_tenuto",
      "soft_accent_tenuto_staccato","fade_in","fade_out","volume_swell","wiggle_sawtooth","wiggle_sawtooth_wide",
      "vibrato_large_faster","vibrato_large_slowest","mute_open","mute_closed","harmonic","up_bow","down_bow",
      "snap_pizzicato","half_open"]},
    "staff": {"$ref": "#/defs/staffRef"},
    "voice": {"type": "integer", "minimum": 1, "maximum": 4, "default": 1},
    "at": {"$ref": "#/defs/position"},
    "range": {"$ref": "#/defs/range", "description": "every chord in the range on that voice, like a palette range"},
    "anchor": {"enum": ["auto", "above", "below", "chord"], "default": "auto"}
  },
  "oneOf": [{"required": ["at"]}, {"required": ["range"]}]
}
```
The enum is the master Articulations palette list (`palettecreator.cpp:722-762`, Above forms); the name-to-SymId
table is the implementer's.

**Validation before mutation.**
- Target resolves to a CR (`E_NO_CHORDREST`); it must be a **Chord** (`E_NOT_A_CHORD`): every UI path ignores rests
  (`Score::toggleArticulation(el,a)` returns false for non-chords, `cmd.cpp:2446-2452`; `toggleArticulationForSelection`
  uses notes only, `ni:5694`; `Rest::acceptDrop` does not accept ARTICULATION, `rest.cpp:161-187`, and
  `Rest::drop` deletes it, `rest.cpp:196-204`).
- Grace chords are valid targets (the selection's note gives `note->chord()` = the grace chord, `cmd.cpp:2446-2447`);
  linked copies use `findLinkedChord` (`edit.cpp:7082-7083`).
- Trill cue notes are not addressable through structural positions; no check needed (`ni:5710-5713` exists only
  for user selections).
- Existing same-subtype articulation: no-op / `E_EXISTS` (Q6).
- Chord has a Tapping: `Score::toggleArticulation(chord, a)` removes it when any articulation is added to a chord
  that already has articulations (`cmd.cpp:2459-2464`, reached from `chord.cpp:1522`); refuse or warn (Q8).

**Post-condition.** `chord->articulations()` contains an element with `subtype() == subtype(symAbove)`; for a
combined symbol on a chord that already had articulations, the parts are present instead (Q7). The pointer passed
to `Chord::drop` is **not** in the score in that case (`cmd.cpp:2473-2487` adds clones and returns true; the
original is neither added nor deleted) — re-read the chord, never use the passed pointer.

**Nested commands.** None (M3). Safe under the lock.

**Linked parts / excerpts.** Articulation-family branch (`edit.cpp:7054-7095`): clone per linked staff, parent =
the linked CR at the same tick on the linked track (`edit.cpp:7080-7090`). If the ChordRest segment is missing in
a linked score the loop `break`s, skipping the remaining staves (`edit.cpp:7076-7079`). Not a voice-1 copy type
(M8). Removal (toggle-off paths) uses `undoRemoveElement`, which removes linked copies.

**Crash and corruption risks.**
- Combined articulations are stored combined when the chord had none, and split when it had some
  (`chord.cpp:1519-1522`, `cmd.cpp:2473-2487`); the keyboard path splits any combined ones it meets
  (`chord.cpp:1767-1793`). The read side must accept both forms.
- `Score::toggleArticulation(SymId)` leaks the combined articulation it creates when the split path returns true
  (`cmd.cpp:2157-2159` deletes only on false); not a crash.
- Plugin `Cursor.add(Articulation)` adds duplicates (M9).

**Existing upstream tests.** `src/engraving/tests/note_tests.cpp:291-340` (articulation on a grace chord, created
with `Factory::createArticulation(gc)`), `src/engraving/tests/copypastesymbollist_tests.cpp` with
`copypastesymbollist_data/copypastesymbollist-articulation*.mscx` and `-staccato-rest*.mscx` (paste path, useful
fixtures), read-compat `compat206_data/articulations-double.mscx` (combined symbols).

**Scenarios.**
- EXM-ART-01 staccato on one note → one staccato.
- EXM-ART-02 staccato twice on the same note in one batch → still one staccato (palette semantics), second reports
  no-op.
- EXM-ART-03 accent on a chord with a marcato → both present (palette) — or marcato replaced if Q6 picks keyboard
  semantics.
- EXM-ART-04 accent-staccato on an empty chord → one combined articulation; on a chord with a tenuto → separate
  accent and staccato.
- EXM-ART-05 staccato on a rest → `E_NOT_A_CHORD`.
- EXM-ART-06 range m1–m2 voice 1 → one staccato per chord, none on rests.
- EXM-ART-07 grace chord target; part clone on the linked grace chord.
- EXM-ART-08 up-bow on a chord with a tapping → behaviour per Q8.

**Open questions.** Q6, Q7, Q8, Q14.

---

### add_ornament  ·  command://notation/add_ornament

**UI entry point.**
- Keyboard/menu `add-turn`, `add-turn-inverted`, `add-turn-slash`, `add-turn-up`, `add-turn-inverted-up`,
  `add-trill`, `add-short-trill`, `add-mordent`, `add-haydn`, `add-tremblement`, `add-prall-mordent`, `add-shake`,
  `add-shake-muffat`, `add-tremblement-couperin` (`nac:458-471`, `nua:1887-1970`) → `toggleOrnament(sym)`
  (`ni:8284-8287`) → `Score::toggleOrnament(SymId)` (`cmd.cpp:2175-2199`): same loop as articulations but builds an
  `Ornament` (`Factory::createOrnament`, `cmd.cpp:2188`) and calls `Score::toggleArticulation(el, ornament)`
  (`cmd.cpp:2190`) — **toggle**: adding the same ornament again removes it (`cmd.cpp:2453-2457`).
- Palette (`palettecreator.cpp:773-821`) → `Chord::drop` ORNAMENT, same branch as articulations
  (`chord.cpp:1473-1525`): add, never remove.
- **Ornament vs trill line.** `add-trill` adds the `ornamentTrill` *symbol* as an Ornament on the chord
  (`nac:463`). The trill *line* is a `Trill` spanner from the same palette (`palettecreator.cpp:823-833`) and
  belongs to `add_line kind=trill` (expression-lines). A trill line owns its own Ornament (`chord.cpp:1193-1199`).
  Adding an `ornamentTrill` to a chord that starts a trill line is not prevented by any path above.

**Recommended call.** As add_articulation, with `Factory::createOrnament(score->dummy()->chord())`
(`factory.h:69`), `setSymId`, `chord->hasArticulation` pre-check (ornaments live in `m_articulations`,
`chord.cpp:701-721`), then `chord->drop(ed)`. Optional ornament properties (`Pid::INTERVAL_ABOVE`,
`INTERVAL_BELOW`, `ORNAMENT_SHOW_ACCIDENTAL`, `ORNAMENT_SHOW_CUE_NOTE`, `START_ON_UPPER_NOTE`,
`ornament.cpp:133-203`) can be set before the drop (Q13).

**Selection set-up.** None.

**Arguments.**
```json
{
  "type": "object",
  "required": ["ornament", "staff"],
  "properties": {
    "ornament": {"enum": ["turn","turn_inverted","turn_slash","turn_up","turn_up_s","trill","short_trill","mordent",
      "haydn","tremblement","prall_mordent","up_prall","precomp_mordent_upper_prefix","up_mordent","down_mordent",
      "prall_down","prall_up","line_prall","precomp_slide","shake","shake_muffat","tremblement_couperin",
      "pince_couperin"]},
    "staff": {"$ref": "#/defs/staffRef"},
    "voice": {"type": "integer", "minimum": 1, "maximum": 4, "default": 1},
    "at": {"$ref": "#/defs/position"},
    "range": {"$ref": "#/defs/range"},
    "interval_above": {"type": "string"}, "interval_below": {"type": "string"},
    "show_accidental": {"enum": ["default", "always", "any_alteration", "never"]},
    "start_on_upper_note": {"type": "boolean"}
  },
  "oneOf": [{"required": ["at"]}, {"required": ["range"]}]
}
```
Enum = master Ornaments palette (`palettecreator.cpp:790-814`); `shake` = `ornamentShake3`, `shake_muffat` =
`ornamentShakeMuffat1` as the actions use (`nac:469-470`). The interval/accidental fields depend on Q13.

**Validation before mutation.** As add_articulation (`E_NO_CHORDREST`, `E_NOT_A_CHORD`: `toggleOrnament` acts only on
notes/chords, `cmd.cpp:2179-2180`). Duplicate: no-op / `E_EXISTS` (Q6). Tapping removal also applies
(`cmd.cpp:2459-2464`, Q8).

**Post-condition.** `chord->articulations()` contains an element with `isOrnament()` and the requested `symId()`
(`subtype()` = `symId` for ornaments: none of these names ends in Below or Turned, `articulation.cpp:117-130`).
Do not check `noteAbove()`, `cueNoteChord()` or accidentals before commit (M7).

**Nested commands.** None at add time (M3). At commit, layout (`ModifyDom::cmdUpdateNotes`) runs
`Ornament::computeNotesAboveAndBelow`, which may call `undoRemoveElement` on a cloned note's tie
(`ornament.cpp:280-283`) and, when a cue note is shown, `Score::cmdAddParenthesesToNotes` →
`undoChangeProperty` (`ornament.cpp:421-422`, `cmd.cpp:3586-3602`). These run inside `endCmd` before the macro
closes (text §0.4), so they land in the agent's undo entry.

**Linked parts / excerpts.** As add_articulation (articulation-family branch, `edit.cpp:7054-7095`). The Ornament
copy constructor clones the cue-note chord and accidentals (`ornament.cpp:49-71`).

**Crash and corruption risks.** Trill-line ornaments need `setTrillType()` before adding (DECISIONS P0.S,
`trill.cpp:213`); that is the line command's concern, not this one. No crash path found for symbol ornaments.

**Existing upstream tests.** `copypastesymbollist_data/copypastesymbollist-ornament*.mscx` (paste path). No test
calls `toggleOrnament`.

**Scenarios.**
- EXM-ORN-01 mordent on one note.
- EXM-ORN-02 `trill` symbol on a note; confirm no Trill spanner is created.
- EXM-ORN-03 turn twice → one turn (palette semantics).
- EXM-ORN-04 ornament on a rest → `E_NOT_A_CHORD`.
- EXM-ORN-05 turn with `interval_above` = minor second; after commit the cue note shows (if Q13 accepted).
- EXM-ORN-06 part clone present.

**Open questions.** Q6, Q8, Q13, Q14.

---

### add_fermata  ·  command://notation/add_fermata

**UI entry point.** Palette only (Breaths & pauses, `palettecreator.cpp:949-981`; elements built with
`Factory::makeFermata` + `setSymIdAndTimeStretch`, `palettecreator.cpp:976-980`).
- On a note/chord/rest (`Note::acceptDrop` defers to `ChordRest::acceptDrop`, which accepts FERMATA,
  `note.cpp:1798`, `chordrest.cpp:151`) → `ChordRest::drop` FERMATA (`chordrest.cpp:257-283`): placement = below for
  even voices 2/4 (`track & 1`), else above (`chordrest.cpp:258`); if a Fermata on the same track is already in the
  segment's annotations: same `subtype()` → **no change** (the new one is deleted, `chordrest.cpp:261-263`);
  different subtype → **replaced** via `undoChangeElement` keeping the old placement (`chordrest.cpp:264-270`);
  otherwise track and parent = the CR's, `undoAddElement` (`chordrest.cpp:280-283`).
- On a barline: `BarLine::acceptDrop` accepts a fermata only on an end-barline segment (`barline.cpp:379-380`);
  `BarLine::drop` has the same no-change/replace/add logic with the barline's track and segment
  (`barline.cpp:458-477`).
- Range selection: every chord and rest in the range gets one (`ni:2750-2769`, `placeMultiple` default true,
  `engravingitem.h:229`); barlines inside a range are skipped (`ni:2757-2759`).

**Recommended call.**
```
Fermata* f = Factory::createFermata(score->dummy()->segment());   // factory.h:102
f->setSymIdAndTimeStretch(symAbove);     // fermata.cpp:261-265 — call exactly once on a fresh fermata
EditData ed; ed.dropElement = f;
target->drop(ed);                        // ChordRest::drop (chordrest.cpp:257-283) or BarLine::drop (barline.cpp:458-477)
```
`setSymIdAndTimeStretch` sets the time stretch to the type's default only if it is −1 and **resets it to −1
otherwise** (`fermata.cpp:264`; initial value −1, `fermata.h:95`), so a second call clears the stretch. Defaults:
very short 1.25, short/short-Henze 1.5, normal 2.0, long/long-Henze 3.0, very long 4.0 (`fermata.cpp:194-209`).
`ChordRest::drop` returns either the new fermata, the pre-existing one (and has deleted `f`), or `f` after a
replacement; compare the returned pointer with `f` before touching `f` again.

**Selection set-up.** None.

**Arguments.**
```json
{
  "type": "object",
  "required": ["staff"],
  "properties": {
    "fermata": {"enum": ["normal","short","long","long_henze","short_henze","very_long","very_short"], "default": "normal"},
    "staff": {"$ref": "#/defs/staffRef"},
    "staves": {"enum": ["one", "all"], "default": "one", "description": "Q10"},
    "voice": {"type": "integer", "minimum": 1, "maximum": 4, "default": 1},
    "at": {"$ref": "#/defs/position", "description": "a chord or rest"},
    "barline": {"type": "object", "properties": {"measure": {"type": "integer"}}, "description": "the end barline of this measure"},
    "time_stretch": {"type": "number", "exclusiveMinimum": 0}
  },
  "oneOf": [{"required": ["at"]}, {"required": ["barline"]}]
}
```
Names map to the master palette list (`palettecreator.cpp:966-974`; FermataType map `fermata.cpp:267-297`).

**Validation before mutation.**
- `at`: CR at the exact tick on the track (`E_NO_CHORDREST`); rests are valid (the UI accepts them,
  `chordrest.cpp:151`, `rest.cpp:186`).
- `barline`: the measure's `EndBarLine` segment at `measure->endTick()` must exist and hold a BarLine on
  `staffIdx * VOICES` (`E_NO_BARLINE`), matching `barline.cpp:379-380`. A mid-measure barline is refused (same
  check).
- Existing fermata on that track: same type → no-op (report), different type → replace (UI behaviour), or
  `E_EXISTS` per Q6/Q10.
- `time_stretch`: if given, `undoChangeProperty(Pid::TIME_STRETCH)` after the drop, or `setTimeStretch` before it
  (`fermata.h:76`).

**Post-condition.** The CR's segment (or the barline's segment) `annotations()` holds a Fermata with the target track
and `subtype()` equal to the requested type's Above `SymId` (`fermata.cpp:70-78` normalises Below → Above; layout
may flip the stored `symId`, M7); `timeStretch()` > 0.

**Nested commands.** None (M3). `Fermata::added()`/`removed()` call `setUpTempoMapLater()` (`fermata.cpp:304-320`);
the tempo map is rebuilt in `Score::update` at commit (text §0.4).

**Linked parts / excerpts.** Segment branch (`edit.cpp:7133-7172`); for a barline-segment fermata the measure is
moved back when the tick is a measure start (`edit.cpp:7154-7157`). Not a voice-1 copy type (M8). Replacement uses
`undoChangeElement` (`edit.cpp:5970-5988`), which builds a `ChangeElement` for every linked copy; `ChangeElement`
relinks the new element to the old one's links (`src/engraving/editing/addremoveelement.cpp:390-396`).

**Crash and corruption risks.**
- Calling `setSymIdAndTimeStretch` twice gives stretch −1 (above).
- A fermata on an end barline is ignored by playback: the tempo map reads fermatas only from ChordRest and TimeTick
  segments, and only in the master (`src/engraving/dom/score.cpp:572-579`). Q11.
- The fermata applies to one staff only. In the UI the user selects every staff; the agent needs `staves: "all"` or
  the caller loops (Q10).

**Existing upstream tests.** None call the fermata drop. Read-compat fixture `compat206_data/fermata.mscx`.

**Scenarios.**
- EXM-FER-01 fermata on the last note of m4, staff 1 → placement above, stretch 2.0.
- EXM-FER-02 fermata on a rest in voice 2 → placement below.
- EXM-FER-03 fermata on the final end barline → in the EndBarLine segment; playback tempo unaffected (Q11).
- EXM-FER-04 `short` on a note that already has a `normal` fermata → replaced, one fermata.
- EXM-FER-05 same type twice → one fermata, no-op reported.
- EXM-FER-06 `staves: all` on a 4-staff score → four fermatas at the same tick.
- EXM-FER-07 barline in mid-measure / non-existent → `E_NO_BARLINE`.
- EXM-FER-08 part clone present; undo removes master and clone.

**Open questions.** Q6, Q10, Q11.

---

### add_breath  ·  command://notation/add_breath

**UI entry point.** Palette only (Breaths & pauses, `palettecreator.cpp:983-991`: each element from
`Breath::BREATH_LIST` with `setSymId` and `setPause`). On a note/chord/rest (`Note::acceptDrop` →
`ChordRest::acceptDrop` accepts BREATH, `chordrest.cpp:161`) → `ChordRest::drop` BREATH (`chordrest.cpp:197-211`):
position reset, track = the CR's track ("allow breath marks in voice > 1"), placement below for even voices
(`chordrest.cpp:202-203`), parent = `m->undoGetSegment(SegmentType::Breath, endTick())` where `m` is the CR's
measure (`chordrest.cpp:207-208`), `undoAddElement` (`chordrest.cpp:209`). **The breath goes at the end tick of the
target CR, i.e. after the note**, in a dedicated Breath segment. Barlines and measures do not accept breaths
(`barline.cpp:373-391`).

**Recommended call.**
```
Breath* b = Factory::createBreath(score->dummy()->segment());   // factory.h:87
b->setSymId(sym);
b->setPause(pauseFor(sym));       // BREATH_LIST: 0.0 for breath marks, 2.0 for caesuras (breath.cpp:39-50)
EditData ed; ed.dropElement = b;
cr->drop(ed);                     // chordrest.cpp:197-211
```
The palette sets `pause` explicitly (`palettecreator.cpp:989`); `Factory::createBreath` leaves it 0.0
(`breath.cpp:56-61`), so a caesura without `setPause` has no playback pause.

**Selection set-up.** None.

**Arguments.**
```json
{
  "type": "object",
  "required": ["staff", "after"],
  "properties": {
    "breath": {"enum": ["comma","tick","salzedo","upbow","caesura_curved","caesura","caesura_short","caesura_thick",
                        "chant_caesura","caesura_single_stroke"], "default": "comma"},
    "staff": {"$ref": "#/defs/staffRef"},
    "voice": {"type": "integer", "minimum": 1, "maximum": 4, "default": 1},
    "after": {"$ref": "#/defs/position", "description": "the note or rest the mark follows; the mark sits at its end tick"},
    "pause": {"type": "number", "minimum": 0, "description": "seconds; default from BREATH_LIST"}
  }
}
```
Enum order = `Breath::BREATH_LIST` (`breath.cpp:39-50`). An alternative address "at tick T" can be resolved to the
CR on the track whose `endTick() == T`; there is no UI path that places a breath at a tick without such a CR.

**Validation before mutation.**
- CR at `after` on the track (`E_NO_CHORDREST`). Grace chords: `endTick()` of a grace chord is not the musical
  "after" position; refuse (`E_GRACE`) — the UI result there was not traced (Q9).
- **Existing breath at that segment and track: must be checked.** `ChordRest::drop` has no duplicate check
  (`chordrest.cpp:197-211`). `Segment::add` stores a BREATH in the per-track slot and only logs when the slot is
  occupied (`segment.cpp:830-837`, `checkElement` `segment.cpp:657-673` logs with `LOGD`, `abort()` commented out at `:671`), so
  the first breath drops out of `m_elist` while still parented to the segment; undoing the second add clears the
  slot (`segment.cpp:986-987`), leaving neither visible. Look up
  `cr->measure()->findSegment(SegmentType::Breath, cr->endTick())->element(track)` first; then no-op, replace (change
  `Pid::SYMBOL` and `Pid::PAUSE` with `undoChangeProperty`, `breath.cpp:106-125`), or `E_EXISTS` (Q9).
- `pause` ≥ 0.

**Post-condition.** `cr->measure()->findSegment(SegmentType::Breath, cr->endTick())` exists and its `element(track)`
is a Breath with the requested `symId()` and `pause()`. For a CR that ends at the measure end, the segment is in the
CR's own measure at tick `measure->endTick()` (`chordrest.cpp:207`).

**Nested commands.** None (M3). `Breath::added()`/`removed()` call `setUpTempoMapLater()` (`breath.cpp:179-195`);
the tempo map reads breath pauses from the master only (`score.cpp:530-563`).

**Linked parts / excerpts.** Breath branch (`edit.cpp:7339-7352`): for every staff, including the original, the
segment is recomputed from the breath's tick; when the tick is a measure start it moves to the previous measure
(`edit.cpp:7343-7346`), so a breath after the last note of a measure stays in that measure in every part. Not a
voice-1 copy type (M8).

**Crash and corruption risks.**
- Duplicate breath (above): silent loss of an element, reachable from the UI too.
- Plugin `Cursor.add(Breath)` at tick 0 dereferences a null measure (`edit.cpp:7344-7347`, M9). The recommended call
  always passes `cr->endTick()` > 0.

**Existing upstream tests.** `src/engraving/tests/breath_tests.cpp:41-75` (drop a comma on every selected element,
`breath_data/breath.mscx`, refs `breath01-ref.mscx`, `breath02-ref.mscx`, undo); `src/engraving/tests/parts_tests.cpp:353-483`
(`createPartBreath`, `doAddBreath` via `note->drop`, add/undo/redo/remove in parts; fixtures
`parts_data/part-breath*.mscx`, `part-empty-parts.mscx`). These are the closest existing tests to any command in
this group and exercise exactly the recommended call.

**Scenarios.**
- EXM-BR-01 comma after beat 2 of m1 → Breath segment at tick 960, pause 0.
- EXM-BR-02 caesura after the last note of m4 → segment in m4 at m4's end tick, pause 2.0; tempo map has a pause.
- EXM-BR-03 breath after a voice-2 note → placement below.
- EXM-BR-04 second breath at the same place → per Q9, never two elements fighting for one slot.
- EXM-BR-05 score with parts: clone present; undo/redo (mirror `parts_tests.cpp:398-446`).
- EXM-BR-06 breath after the last note of the score.

**Open questions.** Q9.

---

## Open questions

- **Q1. Duplicate dynamics.** The palette with a note selected adds a second Dynamic on the same segment and track
  (`chordrest.cpp:285-290`); the palette on a selected dynamic replaces type and text (`dynamic.cpp:318-325`).
  Should `add_dynamic` replace (the `Dynamic::drop` route), refuse (`E_EXISTS`), or add?
- **Q2. Voice assignment timing and source.** Parity with the palette/action means a non-undoable
  `setInitialTrackAndVoiceAssignment` after the add, which leaves part clones with different fields when the target
  is in voice 2–4 (M6). Two UI paths set the assignment before adding (`ni:5895-5899`, `edit.cpp:4443-4445`).
  Should the agent set `VOICE_ASSIGNMENT` before the drop for dynamics (the palette's `checkVoiceAssignmentCompatibleWithTrack`,
  `chordrest.cpp:287`, then fixes the track for clones too)? For hairpins `Score::addHairpin` sets the track from
  `cr1` (`edit.cpp:4376`, `4401`), so a pre-set assignment does not move the clone track; the alternatives are
  `addHairpin(type, sTick, eTick, track)` with the adjusted track (`edit.cpp:4353-4372`) or an undoable
  `undoChangeProperty(Pid::VOICE_ASSIGNMENT…)` after the add, which is skipped when the value is unchanged
  (`engravingobject.cpp:440-442`) and so cannot be relied on to reset the voice. And should the assignment come from
  the user preference (`engravingconfiguration.cpp:343-346`) or an explicit argument (same as text Q4)?
- **Q3. Auto-flipping hairpins.** The popup flips adjacent hairpin types after a dynamic is chosen
  (`dynamicpopupmodel.cpp:166`, `ni:5837-5878`); the palette does not. It reads layout snap data
  (`hairpin.cpp:557-584`), so inside a transaction it only sees elements laid out before the transaction. Leave it
  out (palette parity)?
- **Q4. Hairpin end at an arbitrary tick.** `addHairpin(type, sTick, eTick, track)` accepts any tick
  (`edit.cpp:4365-4369`). The grip-drag path creates TimeTick anchors before using a tick
  (`EditTimeTickAnchors::updateAnchors`, `edit.cpp:4455`). Whether layout handles a hairpin whose `tick2` has no
  segment on the staff was not traced. Restrict `end_tick_exact` to CR starts/ends and measure ends, or call
  `EditTimeTickAnchors::updateAnchors` first?
- **Q5. Overlapping hairpins.** No UI path checks for an existing hairpin over the same span and track. Refuse,
  replace or allow?
- **Q6. Articulation/ornament semantics.** Palette = add-only, existing one left alone (`chord.cpp:1477-1483`);
  keyboard = toggle (and accent/marcato replace each other, `chord.cpp:1816-1828`); ornament actions = toggle
  (`cmd.cpp:2453-2457`). The sheet recommends add-only with a no-op report. Confirm, and decide no-op vs `E_EXISTS`
  for duplicates (also fermata same-type and breath).
- **Q7. Combined articulations.** Store combined symbols as given (palette on an empty chord, `chord.cpp:1519-1520`)
  or always split (keyboard path, `chord.cpp:1767-1793`)? Affects what `score_read` must render.
- **Q8. Tapping removal.** `Score::toggleArticulation(chord, a)` removes any Tapping whenever it adds something,
  despite the comment that it is for "a tap of different hand" (`cmd.cpp:2459-2464`). Guard against it (refuse /
  warn when the chord has a Tapping), or accept UI parity?
- **Q9. Breath duplicates and grace targets.** Replace the existing breath's symbol/pause, no-op, or `E_EXISTS`
  (anything but a second add, see add_breath). Also: what does `ChordRest::endTick()` give for a grace chord, and
  should breaths after grace notes be allowed at all?
- **Q10. Fermata on all staves.** The UI adds one per selected CR; musically a fermata usually goes on every staff.
  Offer `staves: "all"` (loop over staves at the tick, resolving a CR per staff), or leave looping to the caller?
- **Q11. Barline fermatas and playback.** They do not affect the tempo map (`score.cpp:572-579`). Document only, or
  refuse barline fermatas in favour of the last note?
- **Q12. The 4.7.6 `cmd("add-hairpin")` crash fix.** The assessment cites a 4.7.6 release-note fix for a plugin
  calling `cmd("add-hairpin")` between `startCmd` and `endCmd`. Under the plugin's lock, `addHairpinsToSelection`'s
  post-apply grip edit is guarded by `!hairpins.front()->segmentsEmpty()` (`ni:5583`). Whether that guard is the fix
  cannot be confirmed without reading history, which this task forbids. It does not affect the recommended call.
- **Q13. Ornament options.** Expose interval above/below, accidental visibility, cue note and start-on-upper-note
  (`ornament.cpp:133-203`) in v1, or leave them to `generic/set_property`?
- **Q14. Range application.** Should `add_articulation` / `add_ornament` (and `add_fermata`) accept a range and apply
  to every chord in it, as palette range application does (`ni:2750-2769`, including the selection-filter checks at
  `ni:2739` and `2752`, which the agent would not apply)?
- **Q15. Custom dynamic text without a dynamic token.** It becomes `DynamicType::OTHER` with no playback velocity
  (`dynamic.cpp:51`, `258`). Allow with a warning, refuse and point to `add_text kind=expression`, or require an
  explicit `velocity`?

## Corrections to the brief, DECISIONS.md and assessment.md

- **DECISIONS.md, "P0.S findings" line 1** cites `notationinteraction.cpp:1372, 2084, 2241` for
  `applyPaletteElement` / `apply()`. Those three lines are in `endDrag` (`ni:1361`), `dropSingle` (`ni:1934`) and
  `dropRange` (`ni:2175`). `applyPaletteElement`'s call is at `ni:2342`. `apply()` itself (`ni:321-330`) does
  **not** call `checkAndShowError`; it commits and notifies. The brief repeats "apply … end in checkAndShowError()";
  only some callers of `apply()` do.
- **assessment.md (Write model table, Expression row):** "element creation through INotationInteraction" does not
  hold for this group. Dynamics' only interaction entry points end in text edit mode and assert inside a
  transaction (M2); fermatas and breaths have no interaction method other than `applyPaletteElement`. Hairpins,
  articulations and ornaments have usable selection-driven methods, but the recommended calls are engraving-level
  with explicit targets (M3).
- **assessment.md:131** lists `toggleArticulationForSelection(id)` as the articulation entry point. It is a toggle
  (a second call removes the mark, `ni:5706-5707`, `chord.cpp:1832-1859`), it is bound only to the four keyboard
  articulations (`nac:178-181`), and its Remove mode does nothing for other symbols (`chord.cpp:1856-1857`).
- COMMANDS.md: no errors found for this group.
