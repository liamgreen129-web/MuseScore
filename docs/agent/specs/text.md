# Spec sheet: `text` group

Commands: `add_text` (kinds: staff, system, expression, tempo, rehearsal), `add_chord_symbol`, `add_lyric`.
Source: MuseScore Studio v4.7.6, branch `agent/4.7`. Every file:line below is in that tree.
Scout: api-scout (P0.S-text). Read-only research; nothing here has been compiled or run.

Path note: the UI action code lives in `src/notationscene/internal/`, not `src/notation/internal/`
(`notationactioncontroller.cpp`, `notationuiactions.cpp`).

---

## 0. Shared findings (apply to every command in this group)

### 0.1 How the UI adds text (all kinds)

1. Action registration: `"staff-text"`, `"system-text"`, `"expression-text"`, `"rehearsalmark-text"`,
   `"chord-text"`, `"lyrics"`, `"tempo"` each call `NotationActionController::addText(TextStyleType)`
   (`src/notationscene/internal/notationactioncontroller.cpp:353-364`). Menu labels are in
   `src/notationscene/internal/notationuiactions.cpp:1335-1407` (Add > Text).
2. `NotationActionController::addText` (`notationactioncontroller.cpp:1457-1484`) chooses the target item:
   with a range selection, `rangeStartSegment()->firstElementForNavigation(startStaffIndex)` (`:1468-1471`);
   otherwise `interaction->contextItem()` (`:1473`). It then calls `INotationInteraction::addTextToItem(type, item)` (`:1483`).
3. `NotationInteraction::addTextToItem` (`src/notation/internal/notationinteraction.cpp:6196-6210`) checks
   `canAddTextToItem` (`:6141-6194`; for every kind in this group the item must be a NOTE, REST, MMREST,
   MEASURE_REPEAT or CHORD, `:6172-6190`). On failure it calls `MScore::setError(NO_NOTE_REST_SELECTED)` and
   `checkAndShowError()` (`:6203-6205`), which shows an error to the user (`:8456-8462`).
4. `NotationInteraction::addText` (`:6212-6247`):
   - ends note-input mode (`:6214-6216`);
   - opens its own transaction: `startEdit("Add text")` → `NotationUndoStack::prepareChanges` (`:6218`, `:314-318`);
   - calls the engraving function `Score::addText(type, item)` (`:6219`);
   - for elements with voice-assignment properties (Expression, Dynamic), calls
     `text->setInitialTrackAndVoiceAssignment(item->track(), false)` (`:6233-6235`);
   - commits: `apply()` → `commitChanges()` + `notifyAboutNotationChanged()` (`:6237`, `:321-330`);
   - scrolls (`showItem`, `:6238`) and enters text-edit mode (`startEditText(text)`, `:6241`, `:8165-8183`).
5. `Score::addText` (`src/engraving/editing/edit.cpp:761-1043`) creates the element with an empty (or a
   default) text and adds it with `undoAddElement` / `ChordRest::undoAddAnnotation`. It contains no
   `startCmd`/`endCmd`.
6. The text the user types is committed when edit mode ends: `NotationInteraction::endEditText`
   (`notationinteraction.cpp:4589-4603`) → `doEndEditElement` (`:4982-4988`) → `TextBase::endEdit`
   (`src/engraving/editing/textedit.cpp:138-260`). The decisive step is: reset the element to its pre-edit text,
   `undo->reopen()`, then `undoChangeProperty(Pid::TEXT, actualXmlText)` — "this also changes text of linked
   elements" (`textedit.cpp:233-239`), then type-specific work (`Lyrics::adjustPrevious`, `:250-254`) and
   `commitText()` → `score()->endCmd()` (`:259`, `:262-265`). Subclasses add steps:
   `TempoText::commitText` runs `updateTempo()` when follow-text is on (`src/engraving/dom/tempotext.cpp:493-499`);
   `Harmony::endEdit` re-parses and normalises the chord and patches linked chord symbols (`src/engraving/dom/harmony.cpp:782-853`).
   If the typed text is empty the element is removed again (`textedit.cpp:190-224`).

### 0.2 Why the agent cannot call `addTextToItem`

- It enters text-edit mode. `TextBase::startEdit` asserts that no command is active
  (`textedit.cpp:114`: `assert(!score()->undoStack()->hasActiveCommand())`). Inside the agent's transaction a
  command is active, so this assert fires in builds where `assert` is enabled.
- `apply()` calls `notifyAboutNotationChanged()` even when `commitChanges()` is a no-op under the lock
  (`notationinteraction.cpp:321-330`, `src/notation/internal/notationundostack.cpp:132-145`), so the view is
  notified in the middle of the agent's transaction.
- `showItem` pans the view (`notationinteraction.cpp:8354-8362`). A bad target shows an error to the user
  (`:6203-6205`).
- `MScore::setError(NO_NOTE_REST_SELECTED)` is a non-warning error (`src/engraving/dom/mscore.h:234`). If it is
  raised inside the agent's transaction, `Score::endCmd` forces a rollback of the whole batch
  (`src/engraving/editing/cmd.cpp:418-420`).
- It ends the user's note-input mode (`notationinteraction.cpp:6214-6216`).
- It cannot set the text. The text only arrives through key events in edit mode.

### 0.3 Non-interactive paths that the UI itself uses

Two recipes reproduce the UI result without edit mode. Both are plain engraving calls with no nested
`startCmd`/`endCmd`, so both are safe under `undoStack->lock()`.

**Recipe A: `Score::addText`, then the endEdit commit step.** This is the UI's own creation call followed by the
same property change that `TextBase::endEdit` makes:
```
TextBase* t = score->addText(type, chordRestOrNote);   // edit.cpp:761, public (score.h:574)
t->undoChangeProperty(Pid::TEXT, xml);                 // = textedit.cpp:239
// plus the per-type extras listed under each kind (updateTempo, voice assignment)
```
`Score::addText` creates the linked clones with the default text. The `Pid::TEXT` change then reaches the clones
through `EngravingObject::undoChangeProperty` → `changeProperties` (`src/engraving/dom/engravingobject.cpp:438-495`,
`:398-425`). Whether it propagates is decided per clone by `EngravingItem::propertyPropagation`
(`src/engraving/dom/engravingitem.cpp:1469-1512`). `Pid::TEXT` is in `PropertyGroup::TEXT` and is not
always-linked (`src/engraving/dom/property.cpp:156`). Across scores it propagates only when the destination's text
is linked to the master (`engravingitem.cpp:1501-1512`). This is the same rule the UI is subject to.

**Recipe B: build the element with its text, then add it.** This is the pattern used by chord-symbol and lyric
navigation in the UI (`notationinteraction.cpp:7334-7345`, `:6766-6815`), by palette drops (`ChordRest::drop`,
`src/engraving/dom/chordrest.cpp:311-342`) and by the upstream gtests (`chordsymbol_tests.cpp:136-141`). Linked
clones are made from the finished element, so they copy its text. For chord symbols this matters, because
`undoAddElement` then transposes each clone correctly (§ add_chord_symbol).

Recommendation (for the conductor to confirm): Recipe A for `add_text` kinds; Recipe B for `add_chord_symbol` and
`add_lyric`. Reasons are given in each section.

### 0.4 Transaction behaviour (common)

- `NotationUndoStack::prepareChanges/commitChanges/rollbackChanges` return early when locked
  (`notationundostack.cpp:106-145`). `Score::startCmd/endCmd` also return early when locked (`cmd.cpp:344-348`,
  `:406-410`). `UndoStack::beginMacro/endMacro/reopen` return early when locked (`src/engraving/editing/undo.cpp:292-296`,
  `:413-417`, `:444-448`). Individual undo commands are still recorded under the lock; only macro boundaries are skipped.
- None of the engraving calls used by the recipes call `startCmd`/`endCmd`: `Score::addText` (`edit.cpp:761-1043`),
  `Score::undoAddElement` (`edit.cpp:6692-7230+`), `ChordRest::undoAddAnnotation` (`chordrest.cpp:1284-1295`),
  `Measure::undoGetSegmentR` (`src/engraving/dom/measure.cpp:725-732`), `TempoText::updateTempo`
  (`tempotext.cpp:260-319`), `Lyrics::adjustPrevious` (`src/engraving/dom/lyrics.cpp:300-318`) and
  `Harmony::setHarmony` (`harmony.cpp:865-888`).
- Layout runs inside `Score::endCmd` before the macro closes (`cmd.cpp:426` then `:436`). Lyric layout can itself
  record undoable property changes (`src/engraving/rendering/score/lyricslayout.cpp:432-468`). These land in the
  agent's macro.
- `commitChanges()` does not notify `notationChanged` (`notationundostack.cpp:132-145`). The service must notify it,
  as `NotationInteraction::apply()` does (`notationinteraction.cpp:321-330`).
- The tempo map is rebuilt in `Score::update` when `m_needSetUpTempoMap` is set (`cmd.cpp:493-496`), i.e. during commit.

### 0.5 Linked parts (common)

`Score::undoAddElement` (`edit.cpp:6692`) handles two classes:
- **System objects**: REHEARSAL_MARK, SYSTEM_TEXT, TEMPO_TEXT, among others (`edit.cpp:6706-6714`). A copy goes to
  `staff(0)` of every score (master and each part) plus every `systemObjectStaves()` staff (`:6723-6732`). The
  original element is placed on the first entry in the list, which is the master's `staff(0)`
  (`:6750-6753`), and its track is reset to that staff's voice 1 (`ne->setTrack(ntrack)`, `:6792`). Clones keep
  the source voice (`:6759`). So a system object always ends up on the top staff whatever staff the anchor was on.
- **Staff annotations**: STAFF_TEXT, EXPRESSION, HARMONY, LYRICS. A copy goes to each linked staff
  (`ostaff->staffList()`, `:6958-6960`). These kinds are skipped on staves whose `rstaff()` differs
  (`:7013-7032`; HARMONY `:7024`, LYRICS `:7028`). For voice-1 elements they fall back to voice 1 of a linked
  staff with no matching linked voice (`VOICE1_COPY_TYPES`, `:6973-7006`). Segment-parented annotations get
  `m->undoGetSegment(...)` in each part score (`:7131-7172`).

### 0.6 Addressing (common)

- The UI only anchors these kinds to an existing ChordRest (`canAddTextToItem`, `notationinteraction.cpp:6172-6190`;
  `chordOrRest`, `edit.cpp:130-142`). Only chord symbols can be created at ticks without a ChordRest
  (§ add_chord_symbol).
- To resolve `{measure, beat|tick}` + staff + voice to a ChordRest, use `Score::tick2measure(tick)` →
  `findSegment(SegmentType::ChordRest, tick)` → `segment->element(track)`, and require an exact tick match.
  `Score::findCR` (`src/engraving/dom/score.cpp:4548-4578`) returns the ChordRest at or before the tick and maps
  to the multimeasure-rest measure (`:4550-4558`), so it is not suitable for exact addressing.
- Multimeasure rests: `ChordRest::undoAddAnnotation` redirects to the MM-rest's first segment when the CR's
  measure is an MM rest (`chordrest.cpp:1287-1290`). Addressing through underlying measures (`tick2measure`, not
  `tick2measureMM`) never hands it an MM-rest CR.
- Grace notes: lyrics on a grace note move to the parent chord (`edit.cpp:925-929`). For the other kinds,
  `chordOrRest` returns the grace chord itself (`edit.cpp:132-133`). The agent should address the parent chord's tick.

### 0.7 Plugin API (cautionary comparison)

- `PluginAPI::newElement` creates any element with `score->dummy()` as parent (`src/engraving/api/v1/qmlpluginapi.cpp:349-365`).
  A Harmony gets the dummy segment as parent (`src/engraving/dom/factory.cpp:220`).
- Setting `text` on an unadded element calls `setProperty` directly. On an added element it calls
  `undoChangeProperty` (`src/engraving/api/v1/scoreelement.cpp:220-225`).
- `Cursor::add` (`src/engraving/api/v1/cursor.cpp:271-430`): lyrics are parented to the current ChordRest with no
  verse calculation and no `adjustPrevious` (`:352-358`). Every other text kind goes to the `default:` branch,
  `undoAddElement` on whatever segment the cursor is on, with the cursor's track (`:428-430`). That means:
  no ChordRest requirement, no rehearsal auto-text, no tempo default or `updateTempo`, and no voice-assignment
  initialisation. A chord symbol whose text is set after `add` goes through `Harmony::setProperty(Pid::TEXT)`,
  so linked parts with a different concert-pitch setting get untransposed text (§ add_chord_symbol).
- Known crash: the assessment (`docs/agent/assessment.md:38`) records mcp-score's `addChordSymbol` segfault. See Open questions.

### 0.8 Text encoding (common)

The element stores MuseScore XML text (`TextBase::setXmlText`, `src/engraving/dom/textbase.cpp:2262-2267`).
Plain input must be escaped with `TextBase::plainToXmlText` (`src/engraving/dom/textbase.h:318`; `setPlainText`,
`:319`). Read back with `plainText()` / `xmlText()`.

### 0.9 Proposed error codes for this group

`E_NO_SCORE`, `E_RANGE` (measure/beat/tick outside the score or not inside the addressed measure),
`E_BAD_STAFF`, `E_BAD_VOICE`, `E_NO_CHORDREST` (no chord or rest at the exact tick on that track),
`E_EMPTY_TEXT`, `E_BAD_MARKUP` (raw-XML text rejected), `E_EXISTS` (duplicate chord symbol or lyric slot),
`E_BAD_ARG` (enum or combination invalid), `E_POSTCONDITION` (edit not found after the call; roll back).

---

### add_text  ·  command://notation/add_text

One command with `kind ∈ {staff, system, expression, tempo, rehearsal}`. The headings below cover all kinds;
per-kind differences follow in sub-parts.

**UI entry point.** Actions `staff-text`, `system-text`, `expression-text`, `tempo`, `rehearsalmark-text`
(`notationactioncontroller.cpp:353-356`, `:364`) → `NotationActionController::addText` (`:1457-1484`) →
`NotationInteraction::addTextToItem` (`notationinteraction.cpp:6196`) → `NotationInteraction::addText`
(`:6212-6247`) → `Score::addText` (`edit.cpp:761`) → edit mode → `TextBase::endEdit` (`textedit.cpp:138-260`).
The agent should use Recipe A (§0.3): `Score::addText(type, cr)`, then `undoChangeProperty(Pid::TEXT, xml)`,
then the per-kind extras below. Do not call `addTextToItem` (§0.2).

Second UI path (palette): dropping a prepared text element runs `NotationInteraction::applyDropPaletteElement`
(`notationinteraction.cpp:2774-2835`) → `ChordRest::drop` (`chordrest.cpp:191`). For STAFF_TEXT, SYSTEM_TEXT and
REHEARSAL_MARK this sets the track to the staff's voice 1 (`trackZeroVoice`, `chordrest.cpp:331-342`). For
TEMPO_TEXT and EXPRESSION it keeps the CR's voice (`:274-283`). The Add > Text path (`Score::addText` via
`undoAddAnnotation`) keeps the CR's track including voice for staff text (`chordrest.cpp:1292`). The two UI
paths therefore disagree on voice for staff text. Recipe A follows the Add > Text path.

**Selection set-up.** None. `Score::addText` takes the destination element explicitly (`edit.cpp:761`). After
commit, select the new element so that "the selection is left somewhere sensible"; the UI leaves the new text
selected and in edit mode (`notationinteraction.cpp:8165-8167`).

**Arguments.** Proposed:
```json
{
  "type": "object",
  "required": ["kind", "at", "staff"],
  "properties": {
    "kind":  {"enum": ["staff", "system", "expression", "tempo", "rehearsal"]},
    "at":    {"type": "object", "properties": {
                "measure": {"type": "integer", "minimum": 1},
                "beat":    {"type": "number"},
                "tick":    {"type": "integer", "minimum": 0}},
              "required": ["measure"], "oneOf": [{"required": ["beat"]}, {"required": ["tick"]}]},
    "staff": {"type": "object", "properties": {"part": {"type": "string"}, "index": {"type": "integer", "minimum": 1}},
              "required": ["part"]},
    "voice": {"type": "integer", "minimum": 1, "maximum": 4, "default": 1},
    "text":  {"type": "string", "description": "plain text; escaped with plainToXmlText"},
    "xml":   {"type": "string", "description": "raw MuseScore text markup (e.g. <sym>metNoteQuarterUp</sym>); mutually exclusive with text"},
    "tempo": {"type": "object", "description": "kind=tempo only",
              "properties": {"bpm": {"type": "number", "exclusiveMinimum": 0},
                             "beat_unit": {"enum": ["w", "h", "h.", "q", "q.", "e", "e.", "s"]},
                             "follow_text": {"type": "boolean"}}}
  }
}
```
`text` is required for staff, system, expression and tempo. It is optional for rehearsal: when omitted, the
auto-sequenced text from `Score::createRehearsalMarkText` is kept. For tempo, either `text`/`xml` or `tempo.bpm`
is required. If only `bpm` (+`beat_unit`) is given, the service builds `<sym>metNote…</sym> = N` in the same
format `Score::addText` uses (`edit.cpp:965-1013`).

**Validation before mutation.**
- No open score → `E_NO_SCORE`.
- `measure` beyond the last measure; `beat`/`tick` outside `[measure.tick, measure.endTick)` → `E_RANGE`.
- Part or staff not found → `E_BAD_STAFF`. Voice outside 1–4 → `E_BAD_VOICE`.
- No ChordRest at the exact tick on `staff*4 + voice-1`, or the element there is a gap rest → `E_NO_CHORDREST`.
  Report the nearest CR ticks. The UI cannot place these kinds without a CR (`notationinteraction.cpp:6172-6190`).
- `text`/`xml` empty after trimming (except rehearsal with no text) → `E_EMPTY_TEXT`. The UI deletes empty
  text (`textedit.cpp:190-224`).
- Both `text` and `xml` given; `tempo` object given with kind ≠ tempo → `E_BAD_ARG`.
- Markup validation for `xml`: see Open questions.

**Post-condition.** After commit, find the element by type in `segment->annotations()` at the CR's segment and
check its track (per kind below) and `xmlText()` == the requested xml. Check that `linkList()` has the expected
size (per kind). If it is missing, roll back with `E_POSTCONDITION`. Return its handle (EID + segment tick + track).

**Nested commands.** `Score::addText` has no `startCmd`/`endCmd` (`edit.cpp:761-1043`). Nor do
`undoChangeProperty`, `TempoText::updateTempo` or `setInitialTrackAndVoiceAssignment`. All are safe under the lock.

**Linked parts / excerpts.** Propagation happens through `undoAddElement` (§0.5) plus the `Pid::TEXT`
propagation rule (§0.3). Per kind below.

**Crash and corruption risks.**
- `canAddTextToItem` accepts MEASURE_REPEAT (`notationinteraction.cpp:6186`). `chordOrRest` returns it as a
  `Rest*` via `isRestFamily()` (`edit.cpp:134-135`; `src/engraving/dom/engravingobject.h:469`), although
  MeasureRepeat is not a Rest. The agent should only pass Chord/Rest elements found by exact addressing, never a
  MeasureRepeat. If the target tick is covered by a measure repeat, return `E_NO_CHORDREST`.
- No duplicate guard: two staff texts, tempos or rehearsal marks at the same tick are allowed by `Score::addText`
  and `Segment::add`. Whether to refuse is a design choice (Open questions).

**Existing upstream tests.** No test calls `Score::addText` for these kinds. `textbase_tests.cpp:109-117` adds
staff text through `ChordRest::drop` (palette path) and `:231-260` (lineBreakTest) reads staff text back.
`links_tests.cpp:476-516` (`DISABLED_testMMRestLink`, disabled) checks tempo-text links to MM-rest copies (fixture `testMMRestLink.mscx`). `tools_tests.cpp:113-188`
cover `cmdResequenceRehearsalMarks`. `tempomap_tests.cpp:49-276` read tempo maps from fixtures in
`src/engraving/tests/tempomap_data/`. `DISABLED_textedit_tests.cpp` is disabled. `src/notation/tests/` contains
only mocks and a CMakeLists, so there are no `NotationInteraction` tests.

**Scenarios.**
- TX-01 staff text "pizz." on beat 1 of bar 3, voice 1; export compare; one undo removes it.
- TX-02 staff text on voice 2 note: track keeps voice 2 (`chordrest.cpp:1292`).
- TX-03 staff text in a score with a linked part: text appears in the part; editing nothing else.
- TX-04 system text on staff 3: lands on master staff 0, voice 1 (§0.5) and in every part.
- TX-05 expression "dolce" with `dynamicsApplyToAllVoices` on vs off (track and voice assignment differ).
- TX-06 tempo "♩ = 132" at bar 1: `score->tempo(tick)` = 2.2 bps after commit; undo restores the previous tempo map.
- TX-07 tempo "Allegro" with `bpm: 144`: text has no metronome mark; tempo map 144.
- TX-08 tempo "Allegro" without bpm: tempo stays at 80 (UI parity, see tempo sub-part).
- TX-09 rehearsal mark with no text after an existing "A": gets "B".
- TX-10 rehearsal mark with explicit text "Intro".
- TX-11 bad address (beat between notes) → `E_NO_CHORDREST`, score unchanged.
- TX-12 batch: 3 texts + 1 bad one → rollback leaves the export identical.
- TX-13 address inside a multimeasure rest (MM rests shown): element anchored on the underlying measure; renders on the MM rest.

**Open questions.** See end of file (Q1–Q6, Q10).

#### add_text kind=staff

- `Score::addText` STAFF branch: `Factory::createStaffText(dummy()->segment(), STAFF)`, then
  `chordRest->undoAddAnnotation(textBox)` (`edit.cpp:809-816`). `undoAddAnnotation` sets the track to the CR's
  track (including voice) and the parent to the CR's segment (`chordrest.cpp:1284-1295`).
- Commit text: `t->undoChangeProperty(Pid::TEXT, xml)`.
- StaffText has no voice-assignment properties. Only Dynamic, Expression and Hairpin override
  `hasVoiceAssignmentProperties` (`src/engraving/dom/dynamic.h:112`, `expression.h:59`, `hairpin.h:70,163`).
- Linked: STAFF_TEXT branch (§0.5). It is copied to linked staves of the same `rstaff` only (`edit.cpp:7017`)
  and is parented per part via `undoGetSegment` (`:7133-7172`).
- Post-condition: STAFF_TEXT in `segment->annotations()` with `track == cr->track()`.

#### add_text kind=system

- `Score::addText` SYSTEM branch: `Factory::createSystemText(dummy()->segment(), SYSTEM)`, then `undoAddAnnotation`
  (`edit.cpp:818-825`). `undoAddElement` then puts the original on the master's top staff with track 0 and clones
  in every part and system-object staff (`edit.cpp:6706-6795`).
- Commit text: `t->undoChangeProperty(Pid::TEXT, xml)`. It propagates to clones per §0.3.
- Post-condition: SYSTEM_TEXT at the segment with `track == 0` in the master. `linkList().size()` ==
  number of scores + number of system-object staves (§0.5).
- Quirk: clones keep the anchor CR's voice (`edit.cpp:6759`) while the original is reset to voice 1
  (`:6792`). Anchoring to the voice-1 CR avoids the mismatch. See Q3.

#### add_text kind=expression

- `Score::addText` EXPRESSION branch: `Factory::createExpression(dummy()->segment())`, then `undoAddAnnotation`
  (`edit.cpp:836-843`).
- UI extra: `text->setInitialTrackAndVoiceAssignment(item->track(), false)` after the add
  (`notationinteraction.cpp:6233-6235`). This calls plain `setTrack`/`setProperty(Pid::VOICE_ASSIGNMENT)`, not the undo
  variants. The result depends on the user preference `configuration()->dynamicsApplyToAllVoices()`
  (`engravingitem.cpp:1317-1330`). With the preference on, the expression moves to voice 1 of the staff with
  ALL_VOICE_IN_INSTRUMENT; with it off, it stays on the CR's track with CURRENT_VOICE_ONLY. Replicate this call at
  the same point (after `Score::addText`, before commit) for UI parity.
- Linked: as staff text. The linked clones were created before `setInitialTrackAndVoiceAssignment` ran, so they
  keep the CR's track (see Q4).
- Post-condition: EXPRESSION at the segment. Track and `VOICE_ASSIGNMENT` must match the preference rule above.

#### add_text kind=tempo

- `Score::addText` TEMPO branch (`edit.cpp:959-1026`) builds a default text from the time signature's denominator
  ("♩ = 80", dotted for compound metres, `:965-1013`). It then calls `Factory::createTempoText(cr->segment())`,
  `setParent`, `setTrack(0)` (`:1019`), `setXmlText`, `setFollowText(true)` (`:1021`) and `updateTempo()`
  (`:1022`), and finally `undoAddElement` (`:1025`). So the tempo text always targets track 0, whatever the
  requested staff. As a TEMPO_TEXT system object it then follows §0.5.
- Commit text: `tt->undoChangeProperty(Pid::TEXT, xml)`, then `tt->updateTempo()` (public, `tempotext.h:82`).
  This mirrors `TempoText::commitText` (`tempotext.cpp:493-499`). `updateTempo` matches `<note> = N` patterns
  against `plainText()` (`tempotext.cpp:260-319`; patterns `:113-135`). On a match it records
  `undoChangeProperty(Pid::TEMPO, …)` and calls `updateScore()` → `setUpTempoMapLater()` (`:289-292`, `:224-228`).
  If nothing matches it changes nothing.
- Tempo text vs tempo map: the map is derived, not stored. `Score::setUpTempoMap` (`score.cpp:420-...`) rebuilds
  it from TEMPO_TEXT annotations whose `playTempoText()` is true, in the master only (`score.cpp:432-436`,
  `:572-590`). It runs in `Score::update` during commit (`cmd.cpp:493-496`). Setting `Pid::TEMPO` directly also
  schedules a rebuild (`tempotext.cpp:364-368`).
- UI parity trap: when the user replaces the default text with words only ("Allegro"), `updateTempo` finds no
  pattern and the tempo stays at the 80 bpm from the default text, which `Score::addText` already applied
  (`edit.cpp:1022`). Recipe A reproduces this. When `tempo.bpm` is given, the service should additionally record
  `tt->undoChangeProperty(Pid::TEMPO, BeatsPerSecond(bpm/60.0))`. If the text does not encode the bpm it should
  also set `Pid::TEMPO_FOLLOW_TEXT` to false. With follow-text left on, a later user edit of the text re-parses it
  (`tempotext.cpp:244-250`, `:493-499`). Design decision for the conductor: Q5.
- `TempoText::setTempo` clamps to `[MIN_TEMPO, MAX_TEMPO]` (`tempotext.cpp:324-332`). The limits are 5–999 bpm
  (`tempotext.cpp:43-44`). Validate `bpm` against them before mutating.
- Post-condition: TEMPO_TEXT at the segment with `track == 0` in the master. After commit,
  `score->tempo(tick)` (`score.h:799`) equals the expected beats per second.
- Overlap: `set_tempo` in group `structure` (`docs/agent/COMMANDS.md:19`) would create the same element. See Q5.

#### add_text kind=rehearsal

- `Score::addText` REHEARSAL_MARK branch (`edit.cpp:796-807`): `Factory::createRehearsalMark(cr->segment())`,
  `setTrack(0)`, `setXmlText(score()->createRehearsalMarkText(r))`, then `undoAddAnnotation`. That last call
  resets the track to the CR's track (`chordrest.cpp:1292`), and `undoAddElement` then moves the original to
  master track 0 (§0.5).
- `createRehearsalMarkText` sequences from the previous mark (A, B, …; numbers; `score.cpp:5154-...`). If `text`
  is given, follow with `undoChangeProperty(Pid::TEXT, xml)`. Otherwise keep the generated text.
- Related upstream command: `Score::cmdResequenceRehearsalMarks` (`score.h:1021`; tests `tools_tests.cpp:113-188`).
  It is outside this group.
- Post-condition: REHEARSAL_MARK at the segment, master `track == 0`, `xmlText()` as requested or generated.

---

### add_chord_symbol  ·  command://notation/add_chord_symbol

**UI entry point.**
- Typed chord symbol on a note or rest: action `chord-text` (`notationactioncontroller.cpp:360`) →
  `addText(HARMONY_A)` → `NotationInteraction::addText` (`notationinteraction.cpp:6212`) → `Score::addText`
  HARMONY branch (`edit.cpp:884-918`; requires a ChordRest or FretDiagram target, `:889-903`). Then edit mode,
  ending in `Harmony::endEdit` (`harmony.cpp:782-853`). Roman numerals and Nashville numbers use
  `roman-numeral-text` / `nashville-number-text` (`notationactioncontroller.cpp:361-362`).
- Chord symbol on a beat or mid-beat tick with no note (the UI's only way): while editing a chord symbol, the
  advance actions (`advance-1`, `advance-2`, … in `DURATIONS_FOR_TEXT_NAVIGATION`, `notationactioncontroller.cpp:57-...`,
  registered `:157-158`) call `navigateToTextElementByFraction` → `navigateToHarmony(ticks)`
  (`notationactioncontroller.cpp:2026-2040`; `notationinteraction.cpp:7298-7346`). Next/previous beat (`next-beat-TEXT`)
  calls `navigateToNearHarmony` (`notationactioncontroller.cpp:1999-2016`; `notationinteraction.cpp:7158-7240`).
  Both create the segment if missing with `measure->undoGetSegment(SegmentType::ChordRest, tick)`
  (`notationinteraction.cpp:7334`, `:7217`). They then build the harmony with `createHarmony(segment, track, type)`
  (`:8153-8163`) and add it with `score()->undoAddElement` (`:7341-7342`, `:7236-7237`). This is Recipe B, and the
  agent should use it for every tick, with or without a note.
- Palette drop of a chord symbol (`ChordRest::drop`, `chordrest.cpp:311-342`) is a third path with different
  pitch semantics (see Q7).

Agent recipe (Recipe B, from `navigateToHarmony` plus the parse step of `Harmony::endEdit`):
```
Measure* m = score->tick2measure(tick);
Segment* seg = m->undoGetSegment(SegmentType::ChordRest, tick);   // measure.h:265 → measure.cpp:725-732
// duplicate check (mirror findHarmonyInSegment, notationinteraction.cpp:8128-8150; it is private)
Harmony* h = Factory::createHarmony(score->dummy()->segment());   // = createHarmony, :8156-8161
h->setScore(score); h->setParent(seg); h->setTrack(track); h->setHarmonyType(type);
h->setHarmony(text);                       // = Harmony::endEdit harmony.cpp:813
h->setPlainText(h->harmonyName());         // = harmony.cpp:814 (and afterRead :446)
score->undoAddElement(h);
```
The parent and track must be set before `setHarmony`. Parsing reads the staff key for Nashville numbers
(`harmony.cpp:537-538`) and reads style (`Sid::lowerCaseMinorChords`, `Sid::chordSymbolSpelling`, `:527-540`).
`Harmony::endEdit`'s substitutions of Unicode accidentals for b/# (`harmony.cpp:787-808`) apply only to text the
user typed with symbols. The agent should do the same normalisation on input, or reject non-ASCII accidentals (Q8).

Why not Recipe A: after `Score::addText(HARMONY_A)`, `undoChangeProperty(Pid::TEXT)` reaches each linked clone
through `Harmony::setProperty(Pid::TEXT)`, which re-parses the same untransposed string (`harmony.cpp:1551-1556`).
The UI compensates in `Harmony::endEdit` by rewriting the TPCs of linked harmonies with a different concert-pitch
setting, using plain setters that are not recorded for undo (`harmony.cpp:821-852`). With Recipe B, the clone is
made from the parsed harmony (copy constructor copies `m_chords`, `harmony.cpp:339-358`). `undoAddElement` then
transposes it with the undoable `Transpose::undoTransposeHarmony` when the part's concert-pitch style differs
(`edit.cpp:7174-7187`; `src/engraving/editing/transpose.cpp:727-760`). The upstream tests use exactly this
order (`chordsymbol_tests.cpp:136-141`, `:187-192`).

**Selection set-up.** None. Select the new harmony after commit.

**Arguments.** Proposed:
```json
{
  "type": "object",
  "required": ["at", "staff", "text"],
  "properties": {
    "at":    {"$ref": "#/add_text/at", "description": "any tick inside the measure; a note is not required"},
    "staff": {"$ref": "#/add_text/staff"},
    "voice": {"type": "integer", "minimum": 1, "maximum": 4, "default": 1},
    "text":  {"type": "string", "description": "e.g. Cmaj7, F#m7b5, Bb/D, N.C.; '|' separates polychord parts (harmony.cpp:497)"},
    "notation": {"enum": ["standard", "roman", "nashville"], "default": "standard"},
    "if_exists": {"enum": ["error", "replace"], "default": "error"}
  }
}
```

**Validation before mutation.**
- `E_NO_SCORE`, `E_BAD_STAFF`, `E_BAD_VOICE` as for add_text.
- The tick must satisfy `measure.tick <= tick < measure.endTick`, else `E_RANGE`. `navigateToHarmony` walks to
  the measure that contains the tick (`notationinteraction.cpp:7314-7321`). A tick equal to `endTick` belongs to
  the next measure.
- The staff must not be a tablature-only or percussion staff: see Q9 (no source guard found in `Score::addText`).
- Empty `text` → `E_EMPTY_TEXT`.
- Existing chord symbol at (segment, track, text style), either a free Harmony or one attached to a FretDiagram
  (same test as `findHarmonyInSegment`, `notationinteraction.cpp:8128-8150`): with `if_exists=error` →
  `E_EXISTS`; with `replace`, see Q6.
- Dry-run parse (optional): building the Harmony and calling `setHarmony` before `undoAddElement` touches no
  score state except `parenthesesMode` on the unadded element (`harmony.cpp:467-487`). An unrecognised chord is
  not an error in the UI: it is stored and rendered as plain text with invalid TPCs (`harmony.cpp:880-886`,
  `:547-555`). Return it as a warning (`recognised: false`), not as an error.

**Post-condition.**
- A Harmony in `seg->annotations()` (or a FretDiagram's `harmony()`) with the requested track and
  `harmonyType()`. Its `plainText()` == `harmonyName()`.
- For STANDARD, `rootTpc()` (`harmony.h:253`) is valid, else report `recognised: false`.
- Linked part copies exist (`linkList()`), and in a part whose concert-pitch style differs from the master,
  their root is transposed by the staff interval.
- The segment exists in the master and in each part (`edit.cpp:7158`).

**Nested commands.** None. `Measure::undoGetSegmentR` adds the segment with `undoAddElement`
(`measure.cpp:725-732`). `Segment::add` for HARMONY calls `rebuildFretBox()` (`src/engraving/dom/segment.cpp:703-705`).
Neither opens a command. Safe under the lock.

**Linked parts / excerpts.** HARMONY is in `VOICE1_COPY_TYPES` (`edit.cpp:6973-6997`) and is skipped on staves
of another `rstaff` (`:7024`). The segment branch creates or gets the segment in each part score and adds the
clone (`:7131-7172`), then transposes it if the concert-pitch setting differs (`:7174-7187`). A voice-2..4 chord
symbol is not copied to a part staff that lacks that linked voice (`:7000-7007`).

**Crash and corruption risks.**
- `Harmony::setProperty(Pid::TEXT)` dereferences `explicitParent()` without a null check
  (`harmony.cpp:1558`), unlike the guarded reads at `:1607` and `:1633`. Never set `Pid::TEXT` on a Harmony
  with no parent. Recipe B never sets `Pid::TEXT`.
- Annotation-only ChordRest segments are valid. `Segment::checkEmpty` treats a segment with annotations as
  non-empty (`segment.cpp:1080-1093`), and `deleteMeasures` removes annotations explicitly (`edit.cpp:7887-7897`).
  `check.cpp:221` flags annotations whose rtick lies outside the measure, so the tick must be inside the measure
  (validated above).
- Do not use Recipe A for chord symbols: the linked-part transposition that `Harmony::endEdit` performs is not
  recorded for undo (`harmony.cpp:821-852`), so undo/redo of the edit can leave part chord symbols spelled
  differently from a fresh add. This concerns the UI's own path; the agent avoids it by using Recipe B.

**Existing upstream tests.** `src/engraving/tests/chordsymbol_tests.cpp`: `testAddLink` (`:132-144`) and
`testAddPart` (`:183-195`) use Recipe B with `undoAddElement` (fixtures `chordsymbol_data/add-link.mscx`, and
`add-part.mscx`, which is a master plus one part). `harmonyEditKeepsDisplayPosition` (`:146-181`) covers the
edit-mode path. `testAddHarmonyToFretDiagram` (`:481-496`) covers `Score::addText` on a fret diagram
(`add-to-fret.mscz`). `testTranspose` / `testTransposePart` (`:242-263`) use `transpose.mscx` and
`transpose-part.mscx` (Flute + part). `testNashvilleNumbers` (`:415-...`) uses `nashville-numbers.mscx`.
`realize-concert-pitch.mscx` (`<concertPitch>1`) is useful for the concert-pitch scenario.

**Scenarios.**
- CS-01 "Cmaj7" on beat 1 of bar 1 over a note.
- CS-02 "G7" on beat 3 of a bar whose staff has a half-note on beat 1 (annotation-only segment created).
- CS-03 "D/F#" at tick +240 (mid-beat eighth) in 4/4.
- CS-04 chord symbol over a rest; and in an empty (measure-rest) bar on beat 3.
- CS-05 transposing instrument (B♭ trumpet) with a linked part where concert pitch differs: part shows the
  transposed root; undo then redo leaves both spellings consistent.
- CS-06 unparseable "foo" → added, `recognised: false`; export compare.
- CS-07 duplicate at the same tick/track → `E_EXISTS`, unchanged.
- CS-08 eight chord symbols in one batch (assessment's reharmonisation case) → one undo step.
- CS-09 Nashville "4" in E major staff; Roman "V7".
- CS-10 voice-2 chord symbol in a score whose part does not link voice 2 → absent in the part.

**Open questions.** Q6, Q7, Q8, Q9, Q11.

---

### add_lyric  ·  command://notation/add_lyric

**UI entry point.**
- Action `lyrics` (`notationactioncontroller.cpp:363`) → `addText(LYRICS_ODD)` → `Score::addText` LYRICS branch
  (`edit.cpp:920-957`). The target must be a Note, Lyrics or Rest (`:921`); a grace note moves to its parent
  chord (`:925-929`). The verse is the number of lyrics already on the CR, or one past the highest partial-lyric-line
  verse starting at that tick (`:938-947`). The branch then calls `Factory::createLyrics(cr)`, `setTrack(cr->track())`,
  `setParent(cr)` and `setProperty(Pid::VERSE, no)` (`:950-953`), then `undoAddElement` (`:956`). Then edit mode.
- While editing, keys drive the rest. Lyrics refuses `-`, `_`, Enter, Up and Down as text input
  (`lyrics.cpp:261-298`), so they reach the actions:
  - Space: next chord, `navigateToLyrics` (`notationinteraction.cpp:6685-6832`).
  - `-` (`next-syllable`): hyphen, `navigateToNextSyllable` (`:6840-7097`).
  - `_` (`add-melisma`): extender, `addMelisma` (`:7705-7940`).
  - Enter (`add-lyric-verse`): next verse, `addLyricsVerse` (`:7943-...`).
  Registration is at `notationactioncontroller.cpp:161-168`. Each of these calls `score()->startCmd/endCmd`
  directly and then re-enters edit mode on the next lyric.
- The text is committed by `TextBase::endEdit` → `undoChangeProperty(Pid::TEXT)` (`textedit.cpp:239`), followed
  by `Lyrics::adjustPrevious()` (`textedit.cpp:250-254`; `lyrics.cpp:300-318`) and `Lyrics::endEdit` relayout
  (`lyrics.cpp:343-351`).

Data model (what the keys actually change):
- Verse: `Pid::VERSE`, 0-based. The UI labels it "Verse %1" with `verse+1` (`lyrics.cpp:85`). Setting it through
  `setProperty` also switches the text style between LYRICS_ODD and LYRICS_EVEN when the style is not overridden
  (`lyrics.cpp:454-463`, `:489`).
- Hyphen: `Pid::SYLLABIC` ∈ {SINGLE, BEGIN, MIDDLE, END} (default SINGLE, `lyrics.cpp:492-493`). There is no
  hyphen element to add. Layout creates the separator `LyricsLine` for BEGIN/MIDDLE syllables or for melismas
  (`lyricslayout.cpp:483-497`).
- Melisma: `Pid::LYRIC_TICKS` = tick of the last chord under the extender − tick of the lyric. "Lyrics::_ticks
  points to the beginning of the last spanned segment" (`lyricslayout.cpp:471-472`); `addMelisma` sets
  `endTick - fromLyrics->segment()->tick()` (`notationinteraction.cpp:7908`). A one-chord melisma uses
  `Lyrics::TEMP_MELISMA_TICKS` = `Fraction::eps()` (`src/engraving/dom/lyrics.h:49`;
  `notationinteraction.cpp:7781`), which layout reads as "just its own chord" (`lyricslayout.cpp:402-403`). Lyric
  `endTick()` = segment tick + ticks (`lyrics.cpp:225-228`).
- Layout repairs invalid melisma lengths through `undoChangeProperty(Pid::LYRIC_TICKS, …)`
  (`lyricslayout.cpp:432-468`), and these changes are recorded in the current macro (§0.4). A melisma end that
  does not land on a chord of the same track is therefore rewritten silently at commit.
- Verse number: there is no element. It is a text prefix ("1. Glo-"). Layout aligns it separately when
  `Sid::lyricsAlignVerseNumber` is on (`lyricslayout.cpp:55-80`).
- Uniqueness: `ChordRest::lyrics(verse, placement)` (`chordrest.cpp:1189-1197`) identifies the slot.
  `ChordRest::add` does not check for duplicates (`chordrest.cpp:558-564`).

Agent recipe (Recipe B, from `Score::addText` lines 950-956 and `navigateToLyrics` lines 6766-6775/6815):
```
Lyrics* l = Factory::createLyrics(cr);
l->setTrack(cr->track()); l->setParent(cr);
l->setProperty(Pid::VERSE, verse0);                 // as edit.cpp:953 (style odd/even follows)
if (placement) l->setPlacement(...) + setPropertyFlags(Pid::PLACEMENT, UNSTYLED)   // see Q12
l->setSyllabic(syllabic);                           // as :6774 / :7037
l->setTicks(melismaTicks);                          // 0, TEMP_MELISMA_TICKS, or lastChordTick - cr->tick()
l->setXmlText(TextBase::plainToXmlText(text));
score->undoAddElement(l);
l->adjustPrevious();                                // as textedit.cpp:253 (public, lyrics.h:80)
// neighbour fix-ups, as the UI does (below), all via undoChangeProperty
```
Neighbour consistency, as the UI enforces it:
- After `-`, the previous syllable goes SINGLE→BEGIN and END→MIDDLE, and the new one goes BEGIN→MIDDLE and
  SINGLE→END (`notationinteraction.cpp:7040-7063`). The previous syllable's melisma is cleared
  (`LYRIC_TICKS` 0, `:7064`).
- After a space, the next syllable goes END→SINGLE and MIDDLE→BEGIN, and the previous one goes BEGIN→SINGLE and
  MIDDLE→END (`:6781-6811`).
The agent must either apply the same corrections to the previous/next lyric in the same verse, placement and
staff with `undoChangeProperty(Pid::SYLLABIC, …)`, or reject inconsistent input. See Q13.

Why Recipe B rather than A: `Score::addText(LYRICS_ODD)` picks the verse itself (`edit.cpp:938-947`) and cannot
take a requested verse, syllabic or melisma. Recipe B sets them before the add, so linked clones copy them.
`Pid::VERSE`, `Pid::SYLLABIC` and `Pid::LYRIC_TICKS` are always-linked properties (`property.cpp:264-267`, link
column true; `engravingitem.cpp:1473-1475`), so later corrections also propagate.

**Selection set-up.** None. Select the new lyric after commit (the UI leaves it selected,
`notationinteraction.cpp:6818`).

**Arguments.** Proposed. One syllable per call, plus a sequence form, because the UI's main workflow is
continuous entry across chords:
```json
{
  "type": "object",
  "required": ["at", "staff"],
  "properties": {
    "at":    {"$ref": "#/add_text/at"},
    "staff": {"$ref": "#/add_text/staff"},
    "voice": {"type": "integer", "minimum": 1, "maximum": 4, "default": 1},
    "verse": {"type": "integer", "minimum": 1, "default": 1, "description": "1-based; stored as verse-1"},
    "placement": {"enum": ["above", "below"]},
    "syllables": {"type": "array", "minItems": 1, "items": {
        "type": "object", "required": ["text"],
        "properties": {
          "text":     {"type": "string"},
          "syllabic": {"enum": ["single", "begin", "middle", "end"], "default": "single"},
          "melisma_to": {"description": "{measure, beat|tick} of the last note under the extender, or 'self' for a one-chord extender"}
        }}},
    "if_exists": {"enum": ["error", "replace"], "default": "error"}
  }
}
```
Sequence semantics (proposal, mirroring `navigateToLyrics` at `notationinteraction.cpp:6719-6733`): syllable *k+1*
goes on the next chord in the same track after syllable *k*'s chord, or after the end of its melisma. Rests are
skipped unless they already carry a lyric in that verse. The conductor may prefer to bind this to the compact
notation instead; that is a design question.

**Validation before mutation.**
- `E_NO_SCORE`, `E_RANGE`, `E_BAD_STAFF`, `E_BAD_VOICE`.
- No Chord/Rest at the exact tick on the track → `E_NO_CHORDREST`. Lyrics on rests are allowed
  (`edit.cpp:932-933`).
- A grace note address: use the parent chord (`edit.cpp:925-929`), or reject with `E_NO_CHORDREST` (design choice).
- Empty syllable text → `E_EMPTY_TEXT` (the UI deletes empty lyrics, `textedit.cpp:190-224`).
- `cr->lyrics(verse0, placement)` exists → `E_EXISTS`, or with `replace` change the text using
  `undoChangeProperty(Pid::TEXT)` and the other properties on the existing lyric.
- `melisma_to` must resolve to a Chord (not a rest) on the same track, strictly after the lyric's tick, with no
  other lyric of the same verse and placement in between. Otherwise layout rewrites it (`lyricslayout.cpp:432-468`)
  → `E_BAD_ARG`.
- A sequence that runs past the last chord of the score → `E_RANGE` before any mutation.

**Post-condition.**
- `cr->lyrics(verse0, placement)` exists and has the expected `plainText()`, `syllabic()` and `ticks()`.
- After commit (post-layout), re-read `ticks()`. If layout changed it, report a warning with the new value: layout
  ran inside the transaction, so the state is consistent but differs from the request.
- Linked staves have a copy (`linkList()`).

**Nested commands.** None in the recipe. `Lyrics::adjustPrevious` uses `undoChangeProperty`
(`lyrics.cpp:312-314`). The UI helpers (`navigateToLyrics`, `navigateToNextSyllable`, `addMelisma`) call
`score()->startCmd/endCmd` directly. These would be no-ops under the lock (`cmd.cpp:346`, `:408`), but the
helpers require edit mode (`m_editData.element` must be the lyric, e.g. `notationinteraction.cpp:6842-6845`),
so they are unusable anyway.

**Linked parts / excerpts.** LYRICS goes through the ChordLine/Lyrics branch of `undoAddElement`
(`edit.cpp:7096-7110`): the clone is parented to `seg->element(linkedTrack)` in each linked staff. LYRICS is in
`VOICE1_COPY_TYPES` (`:6973-6997`) and is skipped on another `rstaff` (`:7028`).

**Crash and corruption risks.**
- `undoAddElement` does not null-check the linked ChordRest: `ncr = toChordRest(seg->element(linkedTrack));
  ne->setParent(ncr);` (`edit.cpp:7107-7108`). The agent should verify before mutating that every linked staff
  has a ChordRest at the tick on the linked track. Normally they do, because linked staves mirror content.
- Upstream bug in a path we avoid: `navigateToNextSyllable` calls `startCmd` (`notationinteraction.cpp:6971`)
  and then returns without `endCmd` when there is neither a next lyric nor a next CR (`:6975-6977`), which leaves
  a command open. The agent never calls this helper.
- Partial lyric lines (dashes and melismas across repeats and voltas, `PartialLyricsLine`) are created only by
  the UI helpers (`notationinteraction.cpp:6935-6950`, `:7067-7078`, `:7828-7838`, `:7912-7922`). The recipe does not create
  them; see Q14.
- `Lyrics::setProperty(Pid::LYRIC_TICKS)` clears `melismaEnd` on the old end CR using plain setters
  (`lyrics.cpp:433-447`). Layout regenerates the flag (comment `:436-438`).

**Existing upstream tests.** `src/engraving/tests/lyrics_tests.cpp:37-84` (`PartialLyricsLineVerse`, verse
changes via `undoChangeProperty`; fixture `lyrics_data/partialLyricsLineVerse.mscx`). Fixtures with lyrics:
`copypastesymbollist_data/copypastesymbollist-lyrics.mscx` (11 lyrics),
`playback/playbackcontext_data/lyrics_multiverses.mscx` (13 lyrics, multiple verses). No test adds a lyric
through `Score::addText`. `DISABLED_textedit_tests.cpp` is disabled. No `NotationInteraction` tests exist.

**Scenarios.**
- LY-01 single syllable "love" on bar 1 beat 1, verse 1.
- LY-02 "A-ma-zing grace" across four notes: syllabic BEGIN, MIDDLE, END, SINGLE; hyphens render.
- LY-03 melisma: "grace" held across three slurred notes: `ticks = tick(note3) - tick(note1)`; extender renders.
- LY-04 one-chord extender (`melisma_to: "self"`): ticks == eps.
- LY-05 verse 2 under existing verse 1: odd/even style set; text "2. Twas".
- LY-06 lyric on a rest (allowed); lyric on a grace note → parent chord.
- LY-07 duplicate verse slot → `E_EXISTS`; `replace` changes the text only.
- LY-08 `melisma_to` landing on a rest → `E_BAD_ARG`, unchanged.
- LY-09 score with linked part: lyrics appear in the part; one undo removes both.
- LY-10 adding "-ing" (END) after an existing SINGLE syllable: the previous syllable becomes BEGIN (UI parity).
- LY-11 the assessment's "lyrics with melismas" fixture case (S10): a full verse in one batch, export compare.

**Open questions.** Q12, Q13, Q14.

---

## Open questions

- **Q1. Raw markup validation.** `TextBase::setXmlText` stores any string (`textbase.cpp:2262-2267`). The source
  shows no validator the service could call before mutation. Should `xml` be limited to a whitelist (`<sym>`, `<b>`,
  `<i>`, `<u>`, `<font …>`), or dropped from v1?
- **Q2. Duplicates.** Should a second staff text, system text, tempo text or rehearsal mark at the same tick and
  track be refused? Neither the UI nor `Score::addText` refuses it.
- **Q3. System-text voice.** Should system, tempo and rehearsal anchors always be resolved to the voice-1 CR, so
  that the clones' track (`edit.cpp:6759`) matches the original's (`:6792`)? If voice 1 has no CR at the tick
  (another voice starts there), what then?
- **Q4. Expression voice assignment in parts.** The UI changes track and voice assignment after
  `undoAddElement`, without undo and without updating linked clones (`notationinteraction.cpp:6233-6235`,
  `engravingitem.cpp:1317-1330`). Should the agent copy this exactly (parity), or apply it to the element and its
  clones? Also, the behaviour depends on the user preference `dynamicsApplyToAllVoices`. Should the agent take
  `voice_assignment` as an explicit argument instead?
- **Q5. Tempo ownership and semantics.** `add_text(kind=tempo)` and `structure/set_tempo` (`COMMANDS.md:19`)
  would both create TEMPO_TEXT. Which owns tempo-map changes? When `bpm` is given with words-only text, should
  `TEMPO_FOLLOW_TEXT` be set false? `TempoText::setTempo` clamps to `MIN_TEMPO` = 5/60 and `MAX_TEMPO` = 999/60 beats per second, i.e. 5–999 bpm
  (`tempotext.cpp:43-44`, `:324-332`); validate `bpm` in [5, 999].
- **Q6. Replacing an existing chord symbol.** `undoChangeProperty(Pid::TEXT)` on the existing Harmony hits the
  linked-part transposition problem (`harmony.cpp:1551-1556` vs `:821-852`). The alternatives are
  `undoRemoveElement` of the old harmony plus Recipe B (clean transposition, new EID), or `undoChangeProperty`
  followed by explicit `Transpose::undoTransposeHarmony` on each linked clone. Not decided.
- **Q7. Chord-symbol pitch semantics.** Typed chord symbols are stored as displayed in the score's current
  concert-pitch state (no transposition in `Score::addText` or `navigateToHarmony`). `cmdConcertPitchChanged`
  re-spells them on toggle (`score.cpp:2919-2967`). Palette drops instead transpose to written pitch when concert
  pitch is off (`chordrest.cpp:313-319`). Should `add_chord_symbol` take text in concert pitch always (palette
  semantics) or as displayed (typing semantics)? This also affects how `score_read` reports chord symbols.
- **Q8. Accidental input.** Should the service apply the same Unicode-to-ASCII substitutions as `Harmony::endEdit`
  (`harmony.cpp:787-808`) before `setHarmony`, or reject them?
- **Q9. Unsupported staves.** `Score::addText` HARMONY has no staff-type guard (`edit.cpp:884-918`), and none was
  found in `canAddTextToItem` (`notationinteraction.cpp:6141-6194`). Should chord symbols on percussion or
  tablature staves be refused?
- **Q10. Target score.** If the user's active notation is a part (excerpt), should edits go to that excerpt's
  `Score` or always to the master? `TEXT` propagation differs by direction (`engravingitem.cpp:1501-1512`), and
  the tempo map is only rebuilt from the master (`score.cpp:432-436`).
- **Q11. The plugin `addChordSymbol` crash.** The assessment attributes it to "a chord symbol parsed its text
  before it had a parent" (`assessment.md:38`). In 4.7.6, `newElement` gives a Harmony the dummy segment as parent
  (`qmlpluginapi.cpp:363`, `factory.cpp:220`), so the cause could not be confirmed from source. Candidates found:
  the unguarded `explicitParent()` at `harmony.cpp:1558`, and `staff()` with track `nidx` during parse
  (`harmony.cpp:537`). Recipe B sets parent and track before parsing and never sets `Pid::TEXT`, which avoids
  both candidates, but this has not been demonstrated.
- **Q12. Lyric placement.** The UI never sets placement on a new lyric from Add > Lyrics. Placement comes from
  style (`lyrics.cpp:490-491`, `Sid::lyricsPlacement`), and navigation copies the previous lyric's placement and
  flags (`notationinteraction.cpp:6771-6773`). Should `placement` be in v1?
- **Q13. Syllabic neighbour repair vs strictness.** Repair neighbours the way the UI does (§ add_lyric), or
  reject inconsistent input? Repairing changes elements the caller did not name.
- **Q14. Partial lyric lines across repeats.** Out of scope for v1, or required? The logic lives only in
  edit-mode helpers (`notationinteraction.cpp:6935-6950`, `:7067-7078`, `:7828-7838`, `:7912-7922`) and would have to be
  re-implemented.
