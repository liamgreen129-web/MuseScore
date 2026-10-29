# Spec sheet: `notes` group (MuseScore Studio 4.7.6)

Scout: `api-scout`, task P0.S-notes. Source: branch `agent/4.7` at `v4.7.6`.
All paths are relative to the repo root. `cmd.cpp` = `src/engraving/editing/cmd.cpp`,
`edit.cpp` = `src/engraving/editing/edit.cpp`, `noteentry.cpp` = `src/engraving/dom/noteentry.cpp`,
`ni.cpp` = `src/notation/internal/notationinteraction.cpp`,
`nni.cpp` = `src/notation/internal/notationnoteinput.cpp`,
`nac.cpp` = `src/notationscene/internal/notationactioncontroller.cpp`.

Commands: write_voice, add_note_to_chord, remove_note, set_duration, tie, tuplet, grace,
clear_range, exchange_voices, batch.

Error codes below are *proposed names* for the conductor's error table; the four from the
assessment (E_RANGE, E_VOICE_OCCUPIED, E_NO_SCORE, E_MODAL_OPEN) are reused where they fit.

---

## 0. Group-wide findings (referenced from every section as G1–G10)

**G1. What the transaction lock really does.**
- `NotationUndoStack::prepareChanges` calls `Score::startCmd` unless the stack is already
  locked (`src/notation/internal/notationundostack.cpp:106-117`). `startCmd` returns early if
  locked (`cmd.cpp:346-348`) or if a macro is already active (`cmd.cpp:354-357`); otherwise it
  resets `MScore` error to `MS_NO_ERROR` (`cmd.cpp:359`), resets `CmdState` and opens the macro
  (`cmd.cpp:365`). `UndoMacro`'s constructor snapshots the score's `InputState` and selection
  (`src/engraving/editing/undo.cpp:544-548`) and restores them on undo (`undo.cpp:550-563`).
- `lock()` is a single bool, not a counter: `UndoStack::setLocked(bool)` (`undo.cpp:283-286`,
  field `undo.h:318`). A nested `lock()/unlock()` pair unlocks the outer transaction.
- While locked: inner `startCmd` and `endCmd` are no-ops (`cmd.cpp:346-348`, `cmd.cpp:408-410`);
  `UndoStack::beginMacro`, `endMacro`, `reopen` are no-ops (`undo.cpp:294-296`, `undo.cpp:415-417`,
  `undo.cpp:446-448`); `NotationUndoStack::prepareChanges/rollbackChanges/commitChanges` are
  no-ops (`notationundostack.cpp:112-114`, `125-127`, `138-140`). Undo commands still land in
  the open macro via `pushAndPerform` (`undo.cpp:309-331`).
- Therefore the core must `unlock()` *before* `commitChanges()` or `rollbackChanges()`; this is
  exactly what the plugin API does (`src/engraving/api/v1/score.cpp:608-622`).
- Without the lock, an inner `startCmd` is already harmless (`cmd.cpp:354-357`); the damage
  comes from an inner `endCmd`, which closes and commits the outer macro (`cmd.cpp:406-447`).
- If no macro is active, `pushAndPerform` performs the change and deletes the command without
  recording it (`undo.cpp:311-319`): the edit happens but is not undoable. The core must confirm
  that `prepareChanges` actually opened a macro (`UndoStack::hasActiveCommand`, `undo.h:272`)
  before editing; if the stack was already locked or a macro was already active (for example a
  plugin between `startCmd`/`endCmd`), `prepareChanges` silently does nothing
  (`notationundostack.cpp:112-114`, `cmd.cpp:354-357`).
- Rollback: `endCmd(true)` calls `activeCommand()->unwind()` (`cmd.cpp:422-424`), which undoes
  and deletes every child (`undo.cpp:242-250`); the now-empty macro is discarded
  (`cmd.cpp:435-436`) and no `ScoreChanges` is sent (`cmd.cpp:444-446`).
- An empty macro (no undo children) is discarded at commit (`cmd.cpp:435-436`), so a no-op
  request leaves the undo index unchanged.

**G2. `MScore::_error` decides commit vs rollback.** `endCmd` forces a rollback if
`MScore::_error` holds a non-warning error (`cmd.cpp:418-420`). Under the lock the inner
`startCmd` does not reset it (`cmd.cpp:346-348` returns before `cmd.cpp:359`), so an error set by
step N of a request survives to the outer commit and rolls back the whole request. Several paths
in this group set it instead of returning a status (example: `cmdExchangeVoice` sets
`NO_STAFF_SELECTED`, `edit.cpp:4553-4556`). The core must read `MScore::_error` after every
engraving call and map it to a structured error.

**G3. The notation-layer wrappers show dialogs.** `NotationNoteInput::addNote`, `padNote`,
`putNote`, `removeNote`, `addTie`, `doubleNoteInputDuration`, `halveNoteInputDuration` call
`m_interaction->checkAndShowError()` (`nni.cpp:465`, `481`, `501`, `521`, `887`, `980`, `992`),
as does `NotationInteraction::deleteSelection` (`ni.cpp:5448`).
`checkAndShowError` → `MScoreErrorsController::checkAndShowMScoreError`
(`ni.cpp:8456-8462`, `src/notation/internal/mscoreerrorscontroller.cpp:35-190`) opens an
`interactive()->info` dialog (`mscoreerrorscontroller.cpp:190`) and also *clears* `MScore::_error`
(`mscoreerrorscontroller.cpp:45`). The controller adds its own error dialog for tuplets
(`nac.cpp:999-1000`). Rule 4 ("never show dialogs") therefore forces this group to call the
`mu::engraving::Score` methods the wrappers call, inside the core's transaction, rather than the
wrappers themselves. Each section names the exact inner call.

**G4. Two engraving paths open modal message boxes.**
- `Score::putNote(const Position&, bool)` asks "Note input will remove measure repeat"
  (`noteentry.cpp:386-398`) via `MessageBox::warning` → `interactive()->warningSync`
  (`src/engraving/infrastructure/messagebox.cpp:28-46`).
- `Score::addTuplet` warns "Cannot create tuplet with ratio %1 for duration %2" when
  `ticks / ratio.denominator` is not a valid `TDuration` (`edit.cpp:287-293`).
- (`cmd.cpp:3879` is a third one, outside this group.)
Neither may be reached: pre-validate, or use a path that does not contain them
(`Score::addPitch` removes a measure repeat without asking, `noteentry.cpp:210-214`).

**G5. Side effects on user state that the core must undo.** The engraving calls in this group
change the score's `InputState` (`Score::inputState()`, `score.h:759-761`), the selection, and
(for clearing) depend on the selection filter:
- `setNoteRest` selects the new element unless given an external `InputState`
  (`cmd.cpp:1264-1271`); `Score::addNote` likewise (`edit.cpp:614-620`) and moves the input
  position (`edit.cpp:622-628`).
- `Score::addTuplet` selects an element and sets `inputState().duration` (`edit.cpp:330-339`).
- `Score::padToggle` sets `m_is` duration/dots and leaves them changed unless
  `toggleForSelectionOnly` (`src/engraving/dom/score.cpp:2982-3083`, restore only at
  `score.cpp:3260-3264`).
- `Score::cmdAddTie` moves `m_is` track/segment (`edit.cpp:2088-2091`).
- `changeCRlen` and `cmdDeleteSelection` call `deselectAll()` and reselect
  (`cmd.cpp:1716`, `cmd.cpp:1771`, `edit.cpp:4150-4176`).
The core can snapshot/restore with `Score::inputState()` / `Score::setInputState()`
(`score.h:759-761`) and the selection filter with `Score::selectionFilter()` (`score.h:678-679`).
A saved selection may reference elements the request deleted (remove_note deletes the selected
note), so it must not be restored from raw pointers.

**G6. Mode preconditions.** Many paths branch on `noteEntryMode()`:
`cmdTieNoteList` returns the whole chord in note-input mode (`edit.cpp:2009-2011`),
`padToggle` returns early (`score.cpp:3147-3151`), `cmdDeleteSelection` reselects via the input
state (`edit.cpp:4151-4157`). `NotationInteraction::deleteSelection` deletes *text* if text
editing is active (`ni.cpp:5435-5441`). The core must either refuse (E_BUSY proposed) or leave
note input / element editing before editing; this spec assumes normal mode.

**G7. Master score vs excerpt.** `undoAddCR` replicates a chord/rest to every linked staff
(`edit.cpp:7396-7481`), `undoAddElement` replicates notes, grace chords and ties
(`edit.cpp:6801-6830`, tie branch `edit.cpp:7281-7310`), `undoRemoveElement` removes linked
copies (`edit.cpp:7484-7503`). Editing from a part propagates to the score
(`src/engraving/tests/parts_tests.cpp:1297-1360`). But `cmdExchangeVoice` silently returns when
the selection's score is an excerpt (`edit.cpp:4563-4565`). Recommendation from the code: always
edit `IMasterNotation::masterScore()` (`src/notation/imasternotation.h:45`).

**G8. No layout between steps.** Layout runs in `Score::update`, called from `endCmd`
(`cmd.cpp:426`), which is a no-op under the lock. Every path used below is structural
(segments, ticks, tracks). `Score::putNote(const PointF&...)` is not: it maps a screen point
through layout (`noteentry.cpp:332-336`, `Score::getPosition` `score.cpp:1114`) and must not be
used.

**G9. Pitch values.** `NoteVal.pitch` is concert (sounding) MIDI pitch; `tpc1` is concert
spelling, `tpc2` transposed spelling (`noteentry.cpp:143-165`: for non-concert display the
chromatic transposition is added to `pitch` and `tpc2` derived). If `tpc1`/`tpc2` are left
invalid, `Note::setNval` derives them from key (`src/engraving/dom/note.cpp:3018-3050`).
`noteValForPosition` also fills `string`/`fret` via `StringData::convertPitch`
(`noteentry.cpp:164`), which linked tablature staves rely on.

**G10. Notes about the UI's note-name entry.** The keyboard path `note-c`…`note-b`
(`nac.cpp:127-133`) → `NotationNoteInput::addNote` (`nni.cpp:438-467`) →
`Score::cmdAddPitch(params…)` (`cmd.cpp:5160-5177`) → `cmdAddPitch(step…)` (`cmd.cpp:5179-5227`)
→ `Score::putNote(Position, replace)` (`cmd.cpp:5224`) → `setNoteRest` (`noteentry.cpp:527-529`).
It takes a *diatonic step* whose octave is guessed from the previous note or clef by
`Score::resolveNoteInputParams` (`cmd.cpp:5090-5154`, called at `nac.cpp:793`) and an accidental from input state, so it cannot express an exact
pitch+spelling, and it contains the G4 dialog. The engine-level common denominator used by
every entry path (keyboard `noteentry.cpp:527`, `addPitch` `noteentry.cpp:232`, rest entry
`edit.cpp:4612-4614`, pad rest `score.cpp:3104`, plugin cursor `cursor.cpp:459`, paste
`src/engraving/dom/paste.cpp:461`) is `Score::setNoteRest`.

---

### write_voice  ·  command://notation/write_voice

**UI entry point.** There is no single UI action; the UI writes a voice by note input,
one event at a time. The calls under that, by event type:
- Start/position: `NotationNoteInput::startNoteInput` (`nni.cpp:101-161`), voice choice via
  `setCurrentVoice` (`nni.cpp:681-692`).
- Note: `note-<letter>` → G10 chain → `Score::putNote(Position)` (`noteentry.cpp:362-541`) →
  `Score::setNoteRest` (`cmd.cpp:1093-1274`).
- Chord tone: `chord-<letter>` (`nac.cpp:135-141`) → `cmdAddPitch(step, addFlag=true)` →
  `Score::addNote(chord, nval, …)` (`cmd.cpp:5206`).
- Rest: `rest` → `NotationInteraction::putRestToSelection` (`ni.cpp:5590-5618`) →
  `putRest` → `Score::cmdEnterRest` (`edit.cpp:4584-4593`, opens its own command) →
  `enterRest` (`edit.cpp:4599-4617`) → `setNoteRest` with an empty `NoteVal`.
- Duration: `pad-note-N`/`pad-dot` (`nac.cpp:108-123`) → `NotationNoteInput::padNote`
  (`nni.cpp:469-482`) → `Score::padToggle` (`score.cpp:2971`), which in note-input mode only
  sets `m_is` duration (`score.cpp:2986-3083`).
- Tuplet: `triplet` etc. (`nac.cpp:183-192`) → `NotationActionController::putTuplet`
  (`nac.cpp:986-1009`) → `NotationNoteInput::addTuplet` (`nni.cpp:702-722`):
  `expandVoice()`, `changeCRlen(cr, inputState.duration())`, `Score::addTuplet(cr, ratio, …)`;
  subsequent notes fill the tuplet's rests via `setNoteRest`.
- Tie: `tie` in note input → `NotationNoteInput::addTie` (`nni.cpp:879-888`) →
  `Score::cmdAddTie` (`edit.cpp:2052-2172`), which *creates the next note*; tying two existing
  notes is `cmdToggleTie` (see `tie`).
- Grace: `acciaccatura` etc. → `Score::cmdAddGrace` → `setGraceNote` (see `grace`).

The engine path that reproduces this without the UI wrappers, without dialogs (G3, G4) and
without touching the user's input state or selection (G5), is the plugin cursor's: a private
`InputState` passed as `externalInputState` to
- `Score::addPitch(NoteVal&, bool addFlag, InputState*)` (`noteentry.cpp:171-280`) for the
  first note of a chord, and `addPitch(nval, true, &is)` for further chord tones
  (`noteentry.cpp:176-186` → `addPitchToChord`, `noteentry.cpp:282-325`);
- `Score::enterRest(const TDuration&, InputState*)` (`edit.cpp:4599-4617`) for rests;
- both end in `setNoteRest(..., externalInputState)` (`noteentry.cpp:232-235`,
  `edit.cpp:4612-4614`), which with an external state updates that state instead of selecting
  (`cmd.cpp:1264-1269`). Plugin precedent: `Cursor::addNote`/`addRest`
  (`src/engraving/api/v1/cursor.cpp:445-480`, private state created at `cursor.cpp:74`).

What `setNoteRest` does per event (`cmd.cpp:1093-1274`), which the spec relies on:
- voices 2–4: `expandVoice(segment, track)` first (`cmd.cpp:1108-1110`), which fills an empty
  measure with a measure rest (`cmd.cpp:696-697`) or fills to the neighbouring chord/rests with
  rests (`cmd.cpp:676-700`);
- `makeGap` (`cmd.cpp:1116`, body `cmd.cpp:1290-1462`) removes what is there: chord/rests
  (`cmd.cpp:1395`), whole tuplets it runs into (`cmd.cpp:1391`), two-chord tremolos
  (`cmd.cpp:1367-1368`), non-time-anchored annotations on that track (`cmd.cpp:1440-1455`) and
  slurs in the gap (`cmd.cpp:1459`); it splits a longer existing chord/rest with clones
  (`cmd.cpp:1429`);
- the duration is split by `toDurationList(dd, true)` (`cmd.cpp:1131`) and the pieces tied
  (`cmd.cpp:1176-1182`); if it overruns the measure it continues in the next measure with a tie
  (`cmd.cpp:1205-1236`, `connectTies` `cmd.cpp:1238`);
- grace notes of a chord it replaces are moved onto the new chord (`cmd.cpp:1158-1163`);
- durations are "local (stretched) time" (comment `cmd.cpp:1088-1090`), i.e. written values;
  inside a tuplet the written value is used (`makeGap` divides by the tuplet ratio,
  `cmd.cpp:1320`).

Tuplets: the UI order is "make a chord/rest of the tuplet's total length, then
`Score::addTuplet` on it" (`nni.cpp:716-717`). `Score::addTuplet` (`edit.cpp:266-341`) builds the
tuplet and calls `cmdCreateTuplet` (`edit.cpp:4494-4545`), which replaces the chord/rest with
the first element and fills the rest with rests of `baseLen`. Then each inner event is written
with `setNoteRest` at the tuplet element's segment; `setNoteRest` picks up the tuplet from the
existing chord/rest (`cmd.cpp:1103-1104`). Nested tuplets: `addTuplet` on an element that is
already in a tuplet sets the outer one as parent (`edit.cpp:319-321`). The plugin's
`Cursor::addTuplet` (`cursor.cpp:516-571`) instead builds the `Tuplet` itself and calls
`cmdCreateTuplet` directly, skipping `Score::addTuplet`'s style-flag logic
(`edit.cpp:296-310`) and its dialog; it refuses cross-measure tuplets (`cursor.cpp:533-540`).

Ties across bar lines: with per-bar content (the notation's bar sums are validated), each bar's
notes are written separately and the tie added afterwards between existing notes, using the UI
"tie" path on existing notes: `Score::cmdToggleTie` (`edit.cpp:2175-2290`) → file-static
`createAndAddTie(start, end)` (`edit.cpp:2027-2050`) → `undoAddElement(tie)`. `cmdAddTie` must
not be used for this: it creates a new note (`edit.cpp:2122`).

**What must be cleared first, and why.** The voice's existing content in the bar range must be
removed before writing, using the UI's range-delete on a voice-filtered range (see
`clear_range`): `Score::cmdDeleteSelection` → `deleteRange` (`edit.cpp:3966-3997`). Reasons from
the code:
1. Overwriting with `setNoteRest` moves the old chord's grace notes onto the new chord
   (`cmd.cpp:1158-1163`); clearing removes graces (`edit.cpp:3874-3900`).
2. Overwriting deletes only non-time-anchored annotations of the track and slurs in the gap
   (`cmd.cpp:1440-1459`); clearing applies one explicit selection-filter policy to
   annotations and spanners (`edit.cpp:3985-3986`).
3. For voices 2–4, clearing a whole bar removes the voice from that bar entirely: after removal
   `setRests` skips a full measure of an empty non-zero voice (`edit.cpp:527-537`) and makes
   partial fills *gap* rests (`edit.cpp:566`). Writing then starts from a clean, empty voice.
4. For voice 1, clearing leaves (full-measure) rests (`edit.cpp:3953`, `setRests`
   `edit.cpp:484-584`), and voice 1 stays complete.
5. Ties into the range from outside are removed with the old notes (`RemoveElement` →
   `removeNote`, `src/engraving/editing/addremoveelement.cpp:203-224`), so a re-tie from the
   preceding bar is an explicit step.

**Selection set-up.**
- Clearing: a range selection over the bars on one staff, with the selection filter's voices
  set to the target voice only, element-type filter per policy (see `clear_range`).
- Writing: none. Use a private `InputState`: `setTrack(staffIdx*VOICES + voice)`,
  `setSegment(first ChordRest segment of the bar)`, `setDuration(TDuration)`. The first
  ChordRest segment at the bar's tick exists when voice 1 is complete, which the engine keeps
  true (`setRests` skips only non-zero voices, `edit.cpp:525-528`; `setNoteRest` stops at a
  missing voice-1 element, `cmd.cpp:1218-1224`); the core should still look it up with
  `Measure::findSegment` and fail with E_CORRUPT (proposed) if absent. Default note-entry method of a new
  `InputState` is `BY_NOTE_NAME` (`src/engraving/dom/input.h:156`), which keeps `addPitch` on the
  `setNoteRest` branch (`noteentry.cpp:228-236`) and makes it advance (`noteentry.cpp:274-278`).
- `addPitch(addFlag=true)` adds to the chord at `is.lastSegment()` (`noteentry.cpp:176-186`);
  `moveToNextInputPos` sets `lastSegment` to the segment just written (`input.cpp:395-402`).
- Ties: a list selection of the tie's start note(s), then `cmdToggleTie` (see `tie`).

**Arguments.**
```json
{
  "type": "object",
  "required": ["staff", "voice", "fromMeasure", "content"],
  "properties": {
    "staff":  { "$ref": "#/$defs/StaffRef" },
    "voice":  { "type": "integer", "minimum": 1, "maximum": 4 },
    "fromMeasure": { "type": "integer", "minimum": 1, "description": "1-based measure index (not the displayed number)" },
    "toMeasure":   { "type": "integer", "minimum": 1, "description": "inclusive; default = fromMeasure + bars in content - 1" },
    "content": { "type": "string", "description": "compact notation, one segment per bar (grammar: NOTATION.md, P1.3)" },
    "pitchMode": { "enum": ["written", "sounding"], "description": "see Open questions" },
    "keepAnnotations": { "type": "boolean", "default": true, "description": "selection-filter policy for the clear step" },
    "tieFromPrevious": { "type": "boolean", "default": false }
  },
  "$defs": {
    "StaffRef": { "type": "object", "required": ["part"],
      "properties": { "part": { "type": "string" }, "staffIndex": { "type": "integer", "minimum": 0, "default": 0 } } }
  }
}
```

**Validation before mutation.**
- Score open, not read-only (`endCmd` forces rollback on `readOnly()`, `cmd.cpp:418`) → E_NO_SCORE / E_READ_ONLY.
- Undo stack not locked and no active macro (G1) → E_BUSY. Not in note input / text edit (G6) → E_BUSY.
- Staff resolves; voice 1–4 (`isVoiceIndexValid`, `src/notation/notationtypes.h:564-567`) → E_RANGE.
- Measures exist; `toMeasure − fromMeasure + 1` equals the number of bars in `content` → E_RANGE.
- Each bar sums to the measure's *actual* length in the staff's local time: `Measure::ticks()`
  scaled by `staff->timeStretch(tick)` (`setNoteRest`/`makeGap` work in stretched time,
  `cmd.cpp:1088-1090`, `cmd.cpp:1312`; `Measure::stretchedLen` used at `edit.cpp:526`). Pickups and
  irregular bars differ from the time signature → E_BAR_SUM with bar and offset.
- Every event duration is a single `TDuration` (with dots) or a tie chain of them; a value that
  `toDurationList` would split (`cmd.cpp:1131`) is rejected or must come pre-split → E_DURATION.
- Tuplets: total length is one valid `TDuration`; `total / ratio.denominator` is a valid
  `TDuration` (else the G4 dialog, `edit.cpp:287-293`); base length ≥ 1/512 unless a measure rest
  (`edit.cpp:268-271`); contents sum to `ratio.numerator × baseLen`; tuplet does not cross the bar
  line (`cursor.cpp:533-540` states the same rule) → E_TUPLET.
- Measure is not a multi-measure-rest view measure: resolve real measures (`InputState::setSegment`
  maps MM-rest segments to the first real measure, `input.cpp:363-370`) → handled, not an error.
- Measure repeat on the target staff in any bar: `addPitch` deletes it without asking
  (`noteentry.cpp:210-214`) → E_MEASURE_REPEAT unless an explicit `replaceMeasureRepeats` flag.
- Staff type: drum staff → `addPitch` overrides the voice from the drumset (`noteentry.cpp:193-204`)
  and requires a valid drum pitch; tablature → string/fret must be supplied (G9) → E_UNSUPPORTED_STAFF in v1.
- Pitches 0–127; spelled pitches convert to `pitch`/`tpc1`/`tpc2` per G9 → E_PITCH.
- Excerpt-only tracks: edit the master score (G7).
- Grace notes attach to a chord, never to a rest (`setGraceNote` takes a `Chord*`, `cmd.cpp:863`) → E_GRACE.
- Explicit ties: start and end pitches equal; end note is the next chord in the same voice →
  E_TIE. A tie out of the last bar requires the next bar's first chord in that voice to have the
  pitch (searched via `searchTieNote`, `src/engraving/dom/utils.cpp:576-676`).
- Cross-staff notes (`staffMove`) in the range are not expressible; warn or refuse (Open questions).

**Post-condition.** Walk the track over the bar range after the edits (before commit):
- the sequence of chord/rests (tick, written duration, tuplet membership and ratio, pitches with
  `tpc`, grace notes) equals the parsed content;
- each bar's track sums to the measure length (voice 1) or the voice is absent/complete
  (voices 2–4);
- every requested tie exists (`Note::tieFor()->endNote()` is the intended note) and no others
  were created in the range;
- `MScore::_error == MS_NO_ERROR` (G2).
Any mismatch → rollback, E_POSTCONDITION with the first differing event.

**Nested commands.** `addPitch`, `enterRest`, `setNoteRest`, `Score::addTuplet`,
`setGraceNote`, `cmdDeleteSelection`, `deleteRange` do not call `startCmd`/`endCmd`.
`cmdToggleTie` does (`edit.cpp:2217`, `edit.cpp:2287`) and, in one branch, `cmdAddTie`
(`edit.cpp:2208-2210`, which itself calls `startCmd`/`endCmd`, `edit.cpp:2065`, `edit.cpp:2171`).
`cmdEnterRest` does (`edit.cpp:4590-4592`); call `enterRest` instead. All are safe under the lock
(G1). Do not call `NotationNoteInput`/`NotationInteraction` wrappers (G3).

**Linked parts / excerpts.** Chord/rests via `undoAddCR` (`edit.cpp:7396-7481`), including
linked tuplet clones (`edit.cpp:7450-7470`); notes and grace chords via `undoAddElement`
(`edit.cpp:6801-6830`); ties via the tie branch (`edit.cpp:7281-7310`); removals via
`undoRemoveElement` (`edit.cpp:7484-7503`). Tracks not present in an excerpt are skipped
(`edit.cpp:7418-7421`). No per-part work is needed when editing the master score (G7).

**Crash and corruption risks.**
- Modal dialogs (G4): `Score::putNote` with a measure repeat; `Score::addTuplet` with an invalid
  base length. A modal opened inside the locked transaction leaves the transaction open while
  the dialog waits (`warningSync`, `src/framework/global/internal/interactive.cpp:206-211`).
- `undoAddCR` asserts every note has a valid tpc in debug builds (`edit.cpp:7437-7446`); the
  plugin API sets tpc from pitch before adding a note for this reason
  ("Will crash otherwise", `src/engraving/api/v1/elements.cpp:372`).
- `setNoteRest` asserts the segment is a ChordRest segment (`cmd.cpp:1096`).
- `addPitch(addFlag=true)` asserts `is.lastSegment()` (`noteentry.cpp:177-180`) and returns null
  if the element there is not a chord (`noteentry.cpp:181-184`).
- `setNoteRest` silently stops if `makeGap` returns zero (`cmd.cpp:1118-1121`) or at end of score
  (`cmd.cpp:1209-1211`); a voice-1 gap ends the loop (`cmd.cpp:1221-1223`). Only the
  post-condition catches these.
- Plugin cautionary example: `Cursor::addNote` passes `NoteVal(pitch)` only (`cursor.cpp:458-459`),
  so spelling is whatever `pitch2tpc` picks from the key (`note.cpp:3031-3034`); `Cursor` has no
  tie or grace API.
- `toDurationList` splitting (`cmd.cpp:1131`) creates extra tied notes the notation did not ask for.

**Existing upstream tests.**
- `src/engraving/tests/parts_tests.cpp:1297` `inputFromParts`: `setNoteRest` in voices 1–4 from
  parts propagates to the score (fixture `parts_data/input-from-parts.mscz`).
- `src/engraving/tests/note_tests.cpp:367` `tpc`, `:447` `noteLimits`, `:519`
  `LongNoteAfterShort_183746`: `cmdAddPitch` note entry (fixtures in `note_data/`).
- `src/engraving/tests/durationtype_tests.cpp:48-75`: note entry setup pattern
  (`inputState().setTrack/setSegment/setDuration/setNoteEntryMode` + `cmdAddPitch`).
- `src/engraving/tests/tuplet_tests.cpp:60-93`: `cmdCreateTuplet` helper; `nestedTuplets_addStaff`
  (fixtures `tuplet_data/`).
- `src/engraving/tests/selectionrange_tests.cpp:233-264` `deleteVoice`: voice-filtered range delete
  (fixtures `selectionrange_data/selectionrangedelete03/04*.mscx`).
- No notation-layer tests exist (`src/notation/tests/CMakeLists.txt:34`).

**Scenarios.**
- NOTES-WV-01: rewrite a 4-bar voice-1 melody (S5); export equals golden; one undo restores.
- NOTES-WV-02: add a voice-2 line under an existing voice 1 in bars 3–4 (S6); bars outside
  untouched, voice 2 absent elsewhere.
- NOTES-WV-03: voice 2 only in beats 1–2 of a bar, rest of bar is a gap (no visible rest).
- NOTES-WV-04: triplets and a 5:4 sixteenth tuplet inside one bar (S11).
- NOTES-WV-05: nested tuplet (3:2 inside 5:4).
- NOTES-WV-06: tie across the bar line inside the range and out of the range's last bar (S12).
- NOTES-WV-07: chords with mixed spellings (F♯/G♭) in a transposing instrument, both pitch modes.
- NOTES-WV-08: grace notes before and after (S13).
- NOTES-WV-09: pickup bar and irregular bar; bar-sum error names bar and beat.
- NOTES-WV-10: bar with a measure repeat → E_MEASURE_REPEAT, score unchanged, no dialog.
- NOTES-WV-11: linked part open: content appears in score and part (S20).
- NOTES-WV-12: rewrite voice 1 with dynamics and chord symbols present; `keepAnnotations` keeps them.
- NOTES-WV-13: user in note-input mode → E_BUSY; user selection and input duration unchanged after a successful write.

**Open questions.**
1. Pitch mode: should the notation carry written or sounding pitch for transposing instruments?
   The engine stores concert `pitch`+`tpc1` and transposed `tpc2` (G9).
2. Grace-note order: a new grace chord gets `graceIndex` 0 (`src/engraving/dom/chord.cpp:299`) and
   is inserted at the front of `m_graceNotes` (`chord.cpp:689-696`). Whether index 0 renders
   farthest from or nearest to the main note was not traced into layout; the insertion order for
   several graces must be confirmed by a test.
3. Whether to use `Score::addTuplet` (UI, has dialog and style-flag logic) or the
   `Cursor::addTuplet` construction (no dialog) — both reach `cmdCreateTuplet`. With
   pre-validation, `Score::addTuplet` never reaches the dialog.
4. Tablature and drum staves: string/fret assignment and drumset voice override (`noteentry.cpp:193-204`)
   need their own rules; proposed v1 refuses them.
5. Cross-staff notes and staff-local time signatures (`timeStretch`) in the range: not exercised
   by any test found; behaviour needs fixtures.
6. Whether gap rests in voices 2–4 should be produced by writing a rest and then `deleteItem`
   on it (UI path, `edit.cpp:2916-2925`) or by leaving the gap unwritten; the notation must say
   which.
7. Whether `warningSync` (G4) spins a nested event loop that would let other UI commands run
   inside the open macro was not traced into the framework provider.

---

### add_note_to_chord  ·  command://notation/add_note_to_chord

**UI entry point.** Two UI routes, both in `NotationActionController`:
- `chord-<letter>` (Shift+letter, `nac.cpp:135-141`) → `handleNoteAction` (`nac.cpp:787-836`) →
  `NotationNoteInput::addNote(params, CurrentChord)` (`nni.cpp:438-467`, undo text "Add note to
  chord" `nni.cpp:448`) → `Score::cmdAddPitch(params, addFlag=true, …)` (`cmd.cpp:5160`) →
  with a selected note: `Score::addNote(chord, nval, forceAccidental, m_is.articulationIds())`
  (`cmd.cpp:5184-5213`). The pitch comes from a diatonic step placed above the chord's top note by
  `Score::resolveNoteInputParams` (`cmd.cpp:5101-5111`).
- `interval<N>` (Alt+digit, `nac.cpp:511`) → `NotationInteraction::addIntervalToSelectedNotes`
  (`ni.cpp:6045`) → `Score::addInterval` (`cmd.cpp:714`).
In note-input mode the cursor variant is `addPitch(nval, true)` → `addPitchToChord`, which
handles tied chords by adding the note to every chord of the tie chain
(`noteentry.cpp:282-305` → `addNoteToTiedChord`, `edit.cpp:632-697`).

The exact-pitch call both routes reach is `Score::addNote(Chord*, const NoteVal&, bool
forceAccidental, const std::set<SymId>&, InputState*)` (`edit.cpp:586-630`); for a tied chord
`Score::addNoteToTiedChord` (`edit.cpp:632-697`).

**Selection set-up.** None if `Score::addNote` is called with the chord and a private
`InputState` (`edit.cpp:614-618` skip the selection and move only the private state). The UI
route requires the target note selected (`cmd.cpp:5184-5185`).

**Arguments.**
```json
{
  "type": "object",
  "required": ["target", "pitch"],
  "properties": {
    "target": { "description": "handle of a chord or any note in it, or {staff, voice, measure, beat|tick}" },
    "pitch":  { "description": "spelled pitch, e.g. \"F#4\"; or {midi, tpc}" },
    "pitchMode": { "enum": ["written", "sounding"] },
    "extendThroughTies": { "type": "boolean", "default": true },
    "forceAccidental": { "type": "boolean", "default": false }
  }
}
```

**Validation before mutation.**
- Target resolves (handle re-validated) to a `Chord` with `noteType() == NORMAL` or a grace
  chord; a rest → E_NOT_A_CHORD.
- Pitch 0–127 → E_PITCH. Pitch already in the chord (`Chord::findNote`) → E_DUPLICATE_NOTE:
  `addNote` does not check (`edit.cpp:586-596`), while the note-input `putNote` path treats a
  duplicate as "remove it" (`noteentry.cpp:481-489`).
- Tied chord and `extendThroughTies`: `addNoteToTiedChord` returns null if the pitch is already in
  any chord of the chain (`edit.cpp:640-644`) → E_DUPLICATE_NOTE.
- Tablature/drum staves → E_UNSUPPORTED_STAFF in v1 (string/fret, drumset head; G9).
- Trill cue note chords: the UI refuses edits on them elsewhere (`score.cpp:2975`,
  `edit.cpp:4089`) → E_RANGE.

**Post-condition.** The chord (and, if requested, every chord of its tie chain) contains a note
with the requested `pitch`, `tpc1`, `tpc2`; note count increased by exactly one per chord; tie
chain intact (`addNoteToTiedChord` creates ties between the new notes, `edit.cpp:661-689`).

**Nested commands.** None (`addNote`, `addNoteToTiedChord` do not call `startCmd`). Safe under the lock.

**Linked parts / excerpts.** `undoAddElement(note)` → note branch replicates to linked chords
(`edit.cpp:6801-6830`); accidental via `undoAddElement(a)` (`edit.cpp:597-605`); ties via the tie
branch (`edit.cpp:7281-7310`).

**Crash and corruption risks.**
- Without an external `InputState`, `addNote` selects the note and advances the user's input
  position (`edit.cpp:619`, `edit.cpp:622-628`) (G5).
- `addNoteToTiedChord` asserts the chord has notes (`edit.cpp:634-636`).
- Plugin cautionary example: `Chord::add` in the plugin API calls `setTpcFromPitch` before
  `undoAddElement` because a note without tpc crashes (`elements.cpp:367-377`).

**Existing upstream tests.** `src/engraving/tests/note_tests.cpp:447` `noteLimits` (adds notes
with `cmdAddPitch(…, addFlag=true)`), `:498` `alteredUnison` (fixture `note_data/altered-unison.mscx`).

**Scenarios.**
- NOTES-ANC-01: add a third above a single note; spelling as requested.
- NOTES-ANC-02: add to the first chord of a three-chord tie chain; all three get the note, tied.
- NOTES-ANC-03: duplicate pitch → E_DUPLICATE_NOTE, score unchanged.
- NOTES-ANC-04: target is a rest → E_NOT_A_CHORD.

**Open questions.**
1. Should `extendThroughTies` default to true (note-input behaviour, `noteentry.cpp:300-305`) or
   false (normal-mode Shift+letter behaviour, `cmd.cpp:5212`)? The UI differs by mode.

---

### remove_note  ·  command://notation/remove_note

**UI entry point.** Select a note, press Delete: `action://notation/delete` (`nac.cpp:231`) →
`NotationInteraction::deleteSelection` (`ni.cpp:5427-5451`) → `Score::cmdDeleteSelection`
(`edit.cpp:4002-4178`, list branch `edit.cpp:4012-4147`) → `Score::deleteItem(note)`
(`edit.cpp:2726`): if the chord has more than one note, `undoRemoveElement(note)`
(`edit.cpp:2821-2825`); otherwise the chord is removed and replaced with a rest of the same
duration, keeping tuplet membership (`edit.cpp:2831-2863`); a grace chord is removed without a
rest (`edit.cpp:2864-2869`).
(The `remove-note` action, `nac.cpp:195` → `NotationNoteInput::removeNote` `nni.cpp:506-522`, is
the mouse note-input toggle via `putNote(PointF)`; not usable, G8.)

**Selection set-up.** Single selection of the note (`Score::select(note, SelectType::SINGLE)`,
`score.h:666`), then `cmdDeleteSelection`. Equivalent direct call: `Score::deleteItem(note)`;
`cmdDeleteSelection` adds the trill-cue refusal (`edit.cpp:4089-4092`), link de-duplication
(`edit.cpp:4135-4144`) and reselection (`edit.cpp:4150-4176`).

**Arguments.**
```json
{
  "type": "object",
  "required": ["note"],
  "properties": {
    "note": { "description": "note handle, or {staff, voice, measure, beat|tick, pitch}" },
    "whenLastNote": { "enum": ["rest", "error"], "default": "rest" }
  }
}
```

**Validation before mutation.**
- Handle resolves to a `Note` (E_STALE_HANDLE / E_NOT_A_NOTE).
- Trill cue note → E_RANGE (UI silently skips it, `edit.cpp:4089-4092`).
- Last note of a chord and `whenLastNote == "error"` → E_LAST_NOTE.
- Generated elements are never removed (`edit.cpp:2732-2750`) → E_NOT_A_NOTE.

**Post-condition.** The chord no longer contains a note with that pitch/tpc, or (last note) a
rest of the chord's duration and tuplet occupies the tick on the track; ties to and from the
note are gone (`addremoveelement.cpp:203-224`).

**Nested commands.** None in `cmdDeleteSelection`/`deleteItem`. Safe under the lock.

**Linked parts / excerpts.** `undoRemoveElement` removes linked notes (`edit.cpp:7484-7495`);
the replacement rest goes through `undoAddCR` (`edit.cpp:2846`) and is re-attached to linked
tuplets (`edit.cpp:2848-2862`).

**Crash and corruption risks.**
- `deleteItem(note)` selects `chord->downNote()` (`edit.cpp:2823`); `cmdDeleteSelection` then
  reselects (G5). The caller's saved selection may point at the deleted note (G5).
- Removing a note that is the end of a tie with several jump points changes the tie type
  instead of deleting it (`addremoveelement.cpp:211-216`).
- Plugin cautionary example: plugin `Chord::remove` refuses to remove the last note
  (`elements.cpp:385-397`) rather than replacing the chord with a rest.

**Existing upstream tests.** `src/engraving/tests/selectionfilter_tests.cpp:320`
`notesInChordsDeleteRange` (range variant); `links_tests.cpp:78`, `:181` (`cmdDeleteSelection`
on linked staves/parts); `partialtie_tests.cpp` uses `deleteItem` on chords and tie segments.

**Scenarios.**
- NOTES-RN-01: remove the middle note of a triad.
- NOTES-RN-02: remove the only note → rest of the same duration, inside a triplet.
- NOTES-RN-03: remove a tied note → both tie ends cleaned.
- NOTES-RN-04: remove a note in a linked part's source; part updated.

**Open questions.** None.

---

### set_duration  ·  command://notation/set_duration

**UI entry point.** Normal mode, chord/rest selected, press a duration key: `pad-note-N`
(`nac.cpp:108-118`) → `NotationActionController::padNote` (`nac.cpp:838-873`) →
`NotationNoteInput::padNote` (`nni.cpp:469-482`, undo "Pad note") → `Score::padToggle(pad)`
(`score.cpp:2971-3266`): sets `m_is` duration, clears dots (`score.cpp:2986-3080`), then for each
selected chord/rest `changeCRlen(cr, m_is.duration())`, or `undoChangeChordRestLen` for grace
chords (`score.cpp:3238-3246`). Dots: `pad-dot*` (`nac.cpp:119-122`) in normal mode →
`toggleDotsForSelection` (`nac.cpp:860-863`, `ni.cpp:5727-5740`) → `padToggle(DOT, true)`.
Related: `inc-duration-dotted`/`dec-duration-dotted` → `increaseDecreaseDuration`
(`nac.cpp:221-222`, `ni.cpp:5824-5835`) → `cmdIncDecDuration`.

Inner call: `Score::changeCRlen(ChordRest*, const TDuration&)` (`cmd.cpp:1680-1689`) →
`changeCRlen(cr, Fraction, fillWithRest=true)` (`cmd.cpp:1691-1855`). The plugin API calls the
same pair, including the grace special case (`elements.cpp:285-302`).

**Selection set-up.** None if `changeCRlen` is called directly. The UI route needs the
chord/rest selected and mutates `m_is` duration permanently (`score.cpp:2986-3083`, G5).

**Arguments.**
```json
{
  "type": "object",
  "required": ["target", "duration"],
  "properties": {
    "target": { "description": "chord/rest handle or {staff, voice, measure, beat|tick}" },
    "duration": { "description": "compact duration, e.g. \"q.\", \"e\", \"h..\"" },
    "allowOverwrite": { "type": "boolean", "default": false, "description": "lengthening consumes following events" }
  }
}
```

**Validation before mutation.**
- Target resolves to a chord or rest; measure repeat → refused (`changeCRlen` returns,
  `cmd.cpp:1693-1697`); MM-rest view element → resolve to real measure (`score.cpp:3168-3173`).
- Duration is a valid `TDuration` → E_DURATION.
- Trill cue note chord → E_RANGE (`score.cpp:2973-2978`).
- Lengthening inside a tuplet beyond the tuplet's remaining length: `splitGapToMeasureBoundaries`
  returns empty and nothing happens (`cmd.cpp:1641-1652`, `cmd.cpp:1766-1769`) → E_TUPLET.
- Lengthening past the end of the score is truncated (`cmd.cpp:1665-1668`) → E_RANGE.
- Lengthening overwrites following chord/rests via `makeGap` (`cmd.cpp:1788`) and splits across
  bar lines into tied chords (`cmd.cpp:1810-1851`): require `allowOverwrite` → E_VOICE_OCCUPIED
  if following events are notes and the flag is false.

**Post-condition.** The chord/rest at the original tick has the requested `durationType` and
dots; if the duration crossed a bar line, a tied chain with total = requested; following
content shortened or removed as computed; bar sums intact.

**Nested commands.** None in `changeCRlen`. Safe under the lock.

**Linked parts / excerpts.** `undoChangeChordRestLen` uses `undoChangeProperty`
(`edit.cpp:6336-6340`), which follows links; new rests/chords via `setRest`/`addChord` →
`undoAddCR` (`edit.cpp:350-363`, `edit.cpp:392-430`).

**Crash and corruption risks.**
- Shortening removes ties, two-chord tremolos, glissandi and guitar bends starting at the chord
  (`cmd.cpp:1720-1738`).
- `changeCRlen` deselects all and reselects (`cmd.cpp:1716`, `cmd.cpp:1771`, `cmd.cpp:1861-1867`) (G5).
- `toDurationList` empty → silent return (`cmd.cpp:1741-1744`).

**Existing upstream tests.** `src/engraving/tests/durationtype_tests.cpp:48`, `:77`, `:106`,
`:139` (half/double/inc/dec duration); `measure_tests.cpp:459-466` (`changeCRlen` after delete);
`chordsymbol_tests.cpp` (one `changeCRlen` call).

**Scenarios.**
- NOTES-SD-01: quarter → dotted quarter, following eighth consumed (with `allowOverwrite`).
- NOTES-SD-02: half → quarter, rest fills.
- NOTES-SD-03: lengthen across the bar line → tied pair.
- NOTES-SD-04: lengthen inside a triplet beyond its end → E_TUPLET, unchanged.
- NOTES-SD-05: grace note duration change (eighth → sixteenth).

**Open questions.**
1. Whether to mirror the UI's two-step "duration key then dot key" through `padToggle` (which
   leaves `m_is` modified) or call `changeCRlen` with the dotted `TDuration` directly; both end in
   `changeCRlen` (`score.cpp:3244`).

---

### tie  ·  command://notation/tie

**UI entry point.** `tie` (`nac.cpp:235`) → `NotationActionController::addTie`
(`nac.cpp:1343-1362`). Normal mode → `NotationInteraction::addTieToSelection` (`ni.cpp:5477-5487`)
→ `Score::cmdToggleTie()` (`edit.cpp:2175-2290`). For each selected note it looks for the next
same-pitch note with `searchTieNote` (`edit.cpp:2188-2203`, `utils.cpp:576-676`); if all notes
have one, it creates ties with `createAndAddTie` (`edit.cpp:2229-2235`, `edit.cpp:2027-2050`);
a note that already has a tie has it removed (toggle, `edit.cpp:2238-2246`); a chord followed
by a repeat jump gets a partial tie (`edit.cpp:2248-2252`). If the notes are all at one tick and
*any* lacks a next note, it calls `cmdAddTie()` instead (`edit.cpp:2208-2210`), which **adds a new
note** using the input-state duration (`edit.cpp:2052-2172`, `edit.cpp:2122`). In note-input mode
the action always goes to `cmdAddTie` (`nni.cpp:879-888`). `chord-tie` (`nac.cpp:236`) →
`addTiedNoteToChord` → `cmdAddTie(true)` (`ni.cpp:5497-5503`).

**Selection set-up.** List selection of the start note(s) (single note, or all notes of a chord
for a chord tie). `cmdTieNoteList` uses `selection().element()` for one note, else
`selection().noteList()` (`edit.cpp:2005-2021`). Must be in normal mode (G6), or a single
selected note expands to the whole chord (`edit.cpp:2009-2011`).

**Arguments.**
```json
{
  "type": "object",
  "required": ["from"],
  "properties": {
    "from": { "description": "note handle(s), or a chord handle meaning all its notes" },
    "to":   { "description": "optional expected end note handle(s); verified, not passed to the engine" },
    "action": { "enum": ["add", "remove"], "default": "add" }
  }
}
```

**Validation before mutation.**
- Every `from` note resolves; for `add`: it has no `tieFor` (else `cmdToggleTie` would remove
  it, `edit.cpp:2238-2246`) → E_ALREADY_TIED.
- For `add`: `searchTieNote(note)` is non-null for every note (else the `cmdAddTie` branch adds
  notes, `edit.cpp:2208-2210`) → E_NO_TIE_TARGET; and the found note is in the same track and
  is the expected `to`: `searchTieNote` scans every track of the part and falls back to another
  voice when the same voice has no match (`utils.cpp:640-672`) → E_TIE_VOICE.
- For `remove`: every note has `tieFor` → E_NOT_TIED.
- Ties into repeat jump points (partial ties, `edit.cpp:2248-2252`) are outside v1 unless asked → E_RANGE.

**Post-condition.** `note->tieFor()` exists and `tieFor()->endNote()` is the expected note for
every `from` note (`add`), or `tieFor()` is null (`remove`); no notes were added (count of chords
and notes in the two segments unchanged).

**Nested commands.** `cmdToggleTie` calls `startCmd`/`endCmd` (`edit.cpp:2217`, `edit.cpp:2287`);
the `cmdAddTie` branch also (`edit.cpp:2065`, `edit.cpp:2171`). No-ops under the lock (G1).

**Linked parts / excerpts.** `createAndAddTie` → `undoAddElement(tie)` → tie branch finds linked
chords/notes and clones the tie (`edit.cpp:7281-7310`); `tie->addTiesToJumpPoints()`
(`edit.cpp:2044`).

**Crash and corruption risks.**
- The `cmdAddTie` fallback is destructive (adds notes, moves `m_is`, `edit.cpp:2088-2091`).
- In the tie branch of `undoAddElement`, `cr2` is dereferenced (`cr2->staffIdx()`) when the
  tie has no end note (`edit.cpp:7281-7292`); full ties always have an end note
  (`createAndAddTie` uses `Factory::createPartialTie` when there is none, `edit.cpp:2030`), so
  this is only reachable with a malformed `Tie`.
- Plugin cautionary example: no tie API in plugins (assessment, confirmed: no tie call in
  `src/engraving/api/v1`).

**Existing upstream tests.** `src/engraving/tests/partialtie_tests.cpp:97`, `:266`, `:302`, `:364`
(`cmdToggleTie`), `:522` `toggleTiePartialThenRestore`; `note_tests.cpp:291` `grace`
(`cmdAddTie` on a grace note); `parts_tests.cpp:1255` `partTies` (fixture `parts_data/linked-ties*.mscx`).

**Scenarios.**
- NOTES-TIE-01: tie two existing same-pitch quarters within a bar.
- NOTES-TIE-02: tie across the bar line (S12); linked part has the tie.
- NOTES-TIE-03: chord tie where one note has no partner → E_NO_TIE_TARGET, no notes added.
- NOTES-TIE-04: next same pitch exists only in voice 2 → E_TIE_VOICE.
- NOTES-TIE-05: remove an existing tie.

**Open questions.**
1. `createAndAddTie` is file-static (`edit.cpp:2027`), so the only public route to "tie A to B" is
   `cmdToggleTie` with a selection. If the conductor wants handle-to-handle ties without
   selection, the alternative is replicating `createAndAddTie`'s five steps
   (`edit.cpp:2029-2049`) in `src/agent/`; that is a design decision, not resolved here.

---

### tuplet  ·  command://notation/tuplet

**UI entry point.** `duplet`…`nonuplet`, `custom-tuplet` (`nac.cpp:183-191`) →
`putTuplet(int)` builds `TupletOptions` with `autoBaseLen` and style defaults
(`nac.cpp:1011-1028`) → `putTuplet(options)` (`nac.cpp:986-1009`): refuses with an error dialog
if `!canAddTupletToSelectedChordRests()` (`nac.cpp:999-1001`); normal mode →
`NotationInteraction::addTupletToSelectedChordRests` (`ni.cpp:5792-5810`) →
`Score::addTuplet(cr, ratio, numberType, bracketType)` per selected non-grace chord/rest
(`edit.cpp:266-341`) → `cmdCreateTuplet` (`edit.cpp:4494-4545`). Note-input mode →
`NotationNoteInput::addTuplet` (`nni.cpp:702-722`): `expandVoice`, `changeCRlen(cr,
inputState.duration())`, then `addTuplet`. `tuplet-dialog` opens the "Other…" dialog
(`nac.cpp:192`, `nac.cpp:2113`).

**Selection set-up.** The chord/rest that becomes the tuplet. To get a tuplet of a given total
length at a position, first make the chord/rest at that tick exactly that long
(`changeCRlen`, as `nni.cpp:716` does), then call `Score::addTuplet` on it. Selection itself is
only needed for the UI wrapper (`ni.cpp:5799`).

**Arguments.**
```json
{
  "type": "object",
  "required": ["at", "ratio", "total"],
  "properties": {
    "at": { "description": "chord/rest handle or {staff, voice, measure, beat|tick}" },
    "ratio": { "type": "string", "pattern": "^[0-9]+:[0-9]+$", "description": "actual:normal, e.g. 3:2" },
    "total": { "description": "total written duration of the tuplet, e.g. \"q\"" },
    "numberType": { "enum": ["number", "ratio", "none"] },
    "bracketType": { "enum": ["auto", "bracket", "none"] }
  }
}
```
`autoBaseLen` (`nac.cpp:1018`) computes the denominator with
`Tuplet::computeTupletDenominator` (`src/engraving/dom/tuplet.cpp:858-871`).

**Validation before mutation.**
- Target resolves to a chord/rest, not grace, not trill cue (`ni.cpp:5779-5781`).
- `total` is a single valid `TDuration`; `total / ratio.denominator` is a valid `TDuration`
  (else the modal dialog, `edit.cpp:287-293`) → E_TUPLET.
- Base length ≥ 1/512 unless a measure rest (`edit.cpp:268-271`, `ni.cpp:5783-5786`) → E_TUPLET.
- Not in an MM-rest view measure (`edit.cpp:273-276`) → resolve to the real measure.
- Tuplet stays inside the bar (`cursor.cpp:533-540`) and inside any enclosing tuplet
  (`cmd.cpp:1641-1652`) → E_TUPLET.
- If the chord/rest is longer or shorter than `total`, `changeCRlen` will be applied first;
  lengthening consumes following events (see `set_duration`) → E_VOICE_OCCUPIED unless allowed.

**Post-condition.** A `Tuplet` with the requested ratio, `ticks() == total`, `baseLen`, number
and bracket types exists at the tick on the track; its elements sum to
`ratio.numerator × baseLen`; the first element keeps the original chord's pitches
(`edit.cpp:4507-4532`), others are rests (`edit.cpp:4536-4543`).

**Nested commands.** None (`addTuplet`, `cmdCreateTuplet`, `changeCRlen`). Safe under the lock.

**Linked parts / excerpts.** Elements via `undoAddCR`, which clones the tuplet into linked staves
(`edit.cpp:7450-7470`); `removeChordRest` removes linked originals (`edit.cpp:4629-4663`).

**Crash and corruption risks.**
- Modal dialog in `Score::addTuplet` (`edit.cpp:287-293`) and in the controller (`nac.cpp:1000`) (G3, G4).
- `addTuplet` selects an element and sets `inputState().duration` (`edit.cpp:336-338`) (G5).
- `cmdCreateTuplet` returns silently if the computed note count has a zero denominator
  (`edit.cpp:4499-4502`); only the post-condition catches it.
- Plugin cautionary example: `Cursor::addTuplet` (`cursor.cpp:516-571`) bypasses
  `Score::addTuplet` and sets no number/bracket style flags.

**Existing upstream tests.** `src/engraving/tests/tuplet_tests.cpp:60-115` (`createTuplet` helper
and `tuplet1`), `:117` `join1`, `:142-165` `split1–4`, `:168` `addStaff` (nested tuplets),
`:192` `saveLoad`; `selectionrange_tests.cpp:289` `deletePartialNestedTuplets`.

**Scenarios.**
- NOTES-TU-01: quarter → eighth triplet (S11).
- NOTES-TU-02: half note → 5:4 quarters with the half's pitch kept on element 1.
- NOTES-TU-03: invalid base length (e.g. 7:5 on a dotted eighth) → E_TUPLET, no dialog.
- NOTES-TU-04: tuplet in a linked part's source staff; part shows it.

**Open questions.** See write_voice Open question 3.

---

### grace  ·  command://notation/grace

**UI entry point.** `acciaccatura`, `appoggiatura`, `grace4`, `grace16`, `grace32`,
`grace8after`, `grace16after`, `grace32after` (`nac.cpp:402-409`) →
`NotationInteraction::addGraceNotesToSelectedNotes(type)` (`ni.cpp:5742-5774`): duration
`DIVISION / denominator` with `DIVISION = 480` (`src/engraving/types/constants.h:105`), i.e.
quarter for GRACE4, eighth for ACCIACCATURA/APPOGGIATURA/GRACE8_AFTER, sixteenth, thirty-second
(`ni.cpp:5748-5770`) → `Score::cmdAddGrace(type, len)` (`cmd.cpp:3784-3794`) → for each selected
note `Score::setGraceNote(chord, note->pitch(), type, len)` (`cmd.cpp:863-898`). The UI creates
the grace at the main note's pitch; the user then repitches it.

**Selection set-up.** For the UI path, the main note(s) selected. `setGraceNote` itself takes the
main `Chord*` and an explicit pitch, so no selection is needed if it is called directly with the
requested pitch.

**Arguments.**
```json
{
  "type": "object",
  "required": ["main", "type", "pitch"],
  "properties": {
    "main": { "description": "handle of the main chord or a note in it" },
    "type": { "enum": ["acciaccatura", "appoggiatura", "grace4", "grace16", "grace32", "grace8after", "grace16after", "grace32after"] },
    "pitch": { "description": "spelled pitch" },
    "pitchMode": { "enum": ["written", "sounding"] }
  }
}
```

**Validation before mutation.**
- `main` resolves to a chord (a rest → E_GRACE); if it is itself a grace chord, `setGraceNote`
  attaches to its parent (`cmd.cpp:868-872`) — accept or refuse explicitly.
- Pitch 0–127 → E_PITCH.

**Post-condition.** The main chord's `graceNotes()` count increased by one; the new grace chord
has the requested `noteType`, duration, pitch and spelling.

**Nested commands.** None. Safe under the lock.

**Linked parts / excerpts.** `undoAddElement(chord)` grace branch replicates to linked chords
(`edit.cpp:6801-6830`, condition at `edit.cpp:6810`).

**Crash and corruption risks.**
- Spelling: if the main chord has a note of the same pitch, its `NoteVal` (tpc) is copied
  (`cmd.cpp:880-882`); otherwise `setTpcFromPitch` (`cmd.cpp:883-885`), which may not be the
  requested spelling. A follow-up tpc change is needed (pitch group's `set_pitch` path).
- `cmdAddGrace` selects each new grace note (`cmd.cpp:3791`) (G5).

**Existing upstream tests.** `src/engraving/tests/note_tests.cpp:291` `grace` (setGraceNote,
tie, tremolo, articulation; fixtures `note_data/grace*.mscx`), `:345` `graceAfterSlashSave`;
`selectionfilter_tests.cpp:357` `gracesAndSlurs`.

**Scenarios.**
- NOTES-GR-01: acciaccatura a step below the main note (S13).
- NOTES-GR-02: two grace notes before one main note; order as written.
- NOTES-GR-03: grace-after (grace8after) at end of a trill.
- NOTES-GR-04: main is a rest → E_GRACE.

**Open questions.**
1. Ordering of several graces (see write_voice Open question 2).
2. Whether to create at the main pitch then repitch (UI sequence) or pass the requested pitch to
   `setGraceNote` (one call); both are engine-supported (`cmd.cpp:863`).

---

### clear_range  ·  command://notation/clear_range

**UI entry point.** Range-select, Delete: `action://notation/delete` (`nac.cpp:231`) →
`NotationInteraction::deleteSelection` (`ni.cpp:5427-5451`) → `Score::cmdDeleteSelection`
(`edit.cpp:4002-4178`) → range branch → `Score::deleteRange(startSeg, endSeg, track1, track2,
selectionFilter(), rangeContainsMultiNoteChords)` (`edit.cpp:4007-4011`, `edit.cpp:3966-3997`):
1. `deleteOrShortenOutSpannersFromRange` (`edit.cpp:3985`): spanners wholly in range removed,
   slurs touching the range removed, hairpins/ottavas/trills/vibratos shortened out, each only if
   the filter allows the voice and type;
2. `deleteAnnotationsFromRange` (`edit.cpp:3986`): segment annotations on allowed voices and
   types;
3. per allowed track `deleteRangeAtTrack` (`edit.cpp:3781-3955`): chords removed (or only the
   filtered notes), rests removed **only if `selected()`** (`edit.cpp:3902-3908`), tuplets
   deleted whole if selectable (`edit.cpp:3858-3860`), graces and anchored elements per filter
   (`edit.cpp:3874-3900`), then rests written back with `setRests` (`edit.cpp:3953`).
Voices 2–4: a full empty bar gets no rests, partial fills become gap rests (`edit.cpp:527-537`,
`edit.cpp:566`). Voice 1 gets rests, full-measure rests where the bar is whole (`edit.cpp:538-544`).
Not to be confused with `time-delete` (`nac.cpp:389` → `removeSelectedRange` `ni.cpp:6449-6458`
→ `cmdTimeDelete`), which removes time (structure group).

**Selection set-up.** Required, because rest deletion depends on `selected()` flags set by
`Selection::updateSelectedElements` (`src/engraving/dom/select.cpp:679`), which applies the score's
selection filter. Sequence the code requires:
1. Save `Score::selectionFilter()` (`score.h:678-679`); set voices to the requested set and the
   element types to the policy (`SelectionFilter::setFiltered`,
   `src/engraving/dom/selectionfilter.h:97-104`; types `selectionfilter.h:29-77`).
2. Range-select: `Score::select(measure, SelectType::RANGE, staffIdx)` for the first and last
   measure (`Score::selectRange` measure branch, `score.cpp:3548-3567`) and extend to further
   staves; or `Selection::setRangeTicks(t1, t2, staffStart, staffEnd)` + `updateSelectedElements`
   (`select.cpp:885-901`). Beat-level ranges: `select(chordRest, RANGE)` (`score.cpp:3528+`).
3. `Score::cmdDeleteSelection()`.
4. Restore the filter and the user's selection (G5).
The upstream test pattern is exactly this (`selectionrange_tests.cpp:233-254`).

**Arguments.**
```json
{
  "type": "object",
  "required": ["staves", "from", "to"],
  "properties": {
    "staves": { "type": "array", "items": { "$ref": "#/$defs/StaffRef" }, "minItems": 1 },
    "from": { "description": "{measure, beat|tick}" },
    "to":   { "description": "{measure, beat|tick}, exclusive" },
    "voices": { "type": "array", "items": { "type": "integer", "minimum": 1, "maximum": 4 }, "default": [1,2,3,4] },
    "include": { "type": "array", "items": { "enum": ["dynamics","hairpins","fingering","lyrics","chordSymbols","otherText","articulations","ornaments","slurs","ties","figuredBass","ottavas","pedals","otherLines","arpeggios","glissandi","fretDiagrams","breaths","tremolos","graceNotes"] },
      "description": "element types to delete with the notes; default = all (UI default filter)" }
  }
}
```

**Validation before mutation.**
- Staves contiguous (a range selection is `staffStart..staffEnd`, `select.h:145-146`) → E_RANGE.
- `from < to`, inside the score → E_RANGE. Range boundaries that cut through a tuplet: a partial
  tuplet is handled recursively (`edit.cpp:3861-3865`); allowed but flagged in the response.
- MM-rest views: `selectRange` widens to the whole MM block (`score.cpp:3549`); resolve real
  measures and use `setRangeTicks`.
- Mode (G6).

**Post-condition.** For each cleared voice: voice 1 contains only rests over the range and still
sums per bar; voices 2–4 contain no chords in the range (gap rests or nothing); annotations and
spanners of excluded types still present; nothing outside the range or other voices changed.

**Nested commands.** None (`cmdDeleteSelection`, `deleteRange`). Safe under the lock.

**Linked parts / excerpts.** `removeChordRest` removes every linked copy (`edit.cpp:4629-4663`);
`undoRemoveElement` removes linked annotations (`edit.cpp:7484-7495`); spanner shortening uses
`undoChangeProperty` and de-duplicates linked groups (`deleteOrShortenOutSpannersFromRange`).

**Crash and corruption risks.**
- `IF_ASSERT_FAILED(!isLocked())` on selection updates (`select.cpp:680-683`); no caller in
  `src/engraving` or `src/notation` locks the selection (only checked, `select.h:108-110`).
- `Selection::setRange` asserts `staffEnd > staffStart` (`select.cpp:860`).
- Slurs ending (not starting) at a removed chord: `removeChordRest` removes slurs/trills that
  *start* at the chord (`edit.cpp:4633-4639`); with the SLUR filter excluded, slurs ending in the
  range are not removed by `deleteOrShortenOutSpannersFromRange` → Open question 1.
- Using `NotationInteraction::deleteSelection` shows an error dialog (`ni.cpp:5448`) (G3).

**Existing upstream tests.** `src/engraving/tests/selectionrange_tests.cpp:233-264`
(`deleteVoice1/2`), `:266` `deleteSkipAnnotations`, `:289` `deletePartialNestedTuplets`, `:158`,
`:196` (spanners); `selectionfilter_tests.cpp:103-208` (per-type and per-voice filters), `:320`,
`:357`; `links_tests.cpp:78`, `:181`; `parts_tests.cpp` (`cmdDeleteSelection` ×6).

**Scenarios.**
- NOTES-CR-01: clear voice 2 in bars 3–4; voice 2 disappears, voice 1 untouched.
- NOTES-CR-02: clear voice 1 in bars 3–4 keeping dynamics and chord symbols.
- NOTES-CR-03: clear beats 2–3 of one bar across two staves.
- NOTES-CR-04: range cuts a nested tuplet.
- NOTES-CR-05: user's selection filter has "Dynamics" off; the command's result does not depend on it.

**Open questions.**
1. Behaviour of slurs/hairpins whose *end* lies on a deleted chord when their type is excluded
   from the filter: not covered by a test found; needs a fixture.

---

### exchange_voices  ·  command://notation/exchange_voices

**UI entry point.** `voice-x12`…`voice-x34` (`nac.cpp:329-334`) →
`NotationInteraction::swapVoices(v1, v2)` (`ni.cpp:6026-6043`, checks selection, equal and
valid indices) → `Score::cmdExchangeVoice(s, d)` (`edit.cpp:4551-4577`): requires a **range**
selection, else sets `MsError::NO_STAFF_SELECTED` (`edit.cpp:4553-4556`); returns silently if the
selection's score is an excerpt (`edit.cpp:4563-4565`); works on **whole measures** from the
measure of `tickStart` through the measure containing `tickEnd` (`edit.cpp:4558-4576`) via
`undoExchangeVoice` per measure (`edit.cpp:6346-6457`) → `ExchangeVoice` command
(`src/engraving/editing/editvoicing.cpp:37-53`) → `Measure::exchangeVoice`
(`src/engraving/dom/measure.cpp:2310-2339`): swaps segment elements (`measure.cpp:2312-2314`,
`Segment::swapElements` `src/engraving/dom/segment.cpp:1137-1147`; tuplets follow via
`ChordRest::setTrack` → `processSiblings`, `src/engraving/dom/chordrest.cpp:826-862`) and slur
tracks (`measure.cpp:2316-2336`). If voice 1 is involved, gaps in voice 1 are filled with rests
(`edit.cpp:6428-6456`).

**Selection set-up.** Range selection over the measures and staves (as in `clear_range` step 2).

**Arguments.**
```json
{
  "type": "object",
  "required": ["staves", "fromMeasure", "toMeasure", "voiceA", "voiceB"],
  "properties": {
    "staves": { "type": "array", "items": { "$ref": "#/$defs/StaffRef" }, "minItems": 1 },
    "fromMeasure": { "type": "integer", "minimum": 1 },
    "toMeasure":   { "type": "integer", "minimum": 1 },
    "voiceA": { "type": "integer", "minimum": 1, "maximum": 4 },
    "voiceB": { "type": "integer", "minimum": 1, "maximum": 4 }
  }
}
```
Measure granularity is deliberate: the engine swaps whole measures (`edit.cpp:4558-4576`).

**Validation before mutation.**
- `voiceA != voiceB`, both valid (`ni.cpp:6032-6038`) → E_RANGE.
- Contiguous staves, measures exist → E_RANGE.
- Score is the master score (G7); in an excerpt the call is a silent no-op (`edit.cpp:4563-4565`).
- Range selection present before the call; otherwise `NO_STAFF_SELECTED` rolls back the whole
  transaction at commit (G2).

**Post-condition.** For each measure and staff, the chord/rest sequences of the two voices are
exchanged (compare before/after snapshots by tick, duration, pitches); voice 1 complete in every
measure; slurs that started/ended in the range moved with their notes.

**Nested commands.** None. Safe under the lock.

**Linked parts / excerpts.** `undoExchangeVoice` handles linked staves in the same score
(`edit.cpp:6421-6424`) and excerpts with track mapping, using `CloneVoice` when only one of the
two voices exists in the part (`edit.cpp:6370-6419`).

**Crash and corruption risks.**
- `ExchangeVoice::undo` calls `checkMultiVoices`, `redo` relies on `exchangeVoice` doing it
  (`editvoicing.cpp:45-53`, `measure.cpp:2338`).
- Non-slur spanners (hairpins, lines, ties as note-attached) are not re-tracked by
  `Measure::exchangeVoice` (`measure.cpp:2316-2336` handles `isSlur()` only); upstream tests cover
  glissandi and the range case (`exchangevoices_tests.cpp:64`, `:84`).

**Existing upstream tests.** `src/engraving/tests/exchangevoices_tests.cpp:44` `slurs`, `:64`
`glissandi`, `:84` `rangeSelection` (with undo), `:109` `undoChangeVoice`; fixtures in
`exchangevoices_data/`.

**Scenarios.**
- NOTES-XV-01: swap voices 1 and 2 in bars 5–8; undo restores byte for byte.
- NOTES-XV-02: swap 1↔2 where voice 2 is empty in one bar; voice 1 gets rests there.
- NOTES-XV-03: swap in a score with a linked part where the part lacks voice 2.
- NOTES-XV-04: call issued while viewing a part; still applied to the score.

**Open questions.**
1. Hairpins and other voice-specific lines in swapped measures keep their track
   (`measure.cpp:2316-2336` touches slurs only); whether that is acceptable is a musical
   decision; the `rangeSelection` fixture should be inspected to see what upstream expects.

---

### batch  ·  command://notation/batch

Design belongs to the conductor. This section lists only what the code requires.

**UI entry point.** None. The closest precedent is the plugin API's
`curScore.startCmd()`/`endCmd()` (`src/engraving/api/v1/score.cpp:591-622`): `prepareChanges`
then `lock()`; on finish `unlock()` then `commitChanges()` or `rollbackChanges()`, then
`notationChanged().notify()`. `NotationInteraction::apply` notifies notation change itself
(`ni.cpp:321-330`); `commitChanges` only notifies undo-stack state (`notationundostack.cpp:132-145`).
An alternative mechanism exists: `INotationUndoStack::mergeCommands(startIdx)`
(`src/notation/internal/inotationundostack.h:53`, `undo.cpp:377-390`), used by the UI after drags
(`ni.cpp:1368`).

**Selection set-up.** Per operation (see each command). Between operations the selection and
input state carry over the previous operation's side effects (G5).

**Arguments.** `{ "name": string, "operations": [ { "command": string, "args": object } ] }`
(shape only; ordering, progress and cancellation are design).

**Validation before mutation.** What the code forces:
- Before opening: stack not locked and no active macro (G1), else `prepareChanges` is a no-op
  and every edit becomes un-undoable (`undo.cpp:311-319`).
- `MScore::_error` is reset only by the outer `startCmd` (`cmd.cpp:359`); check it after every
  operation (G2).
- Operations that need the selection must set it themselves; operations must not call
  `lock()`/`unlock()` (single bool, G1).

**Post-condition.** Per operation; plus after commit the undo index advanced by exactly one if
anything changed (`cmd.cpp:435-436`, `undo.cpp:425-436`) and one `ScoreChanges` was sent
(`cmd.cpp:444-446`).

**Nested commands.** Inner `startCmd`/`endCmd` in any operation are no-ops under the lock
(`cmd.cpp:346-348`, `cmd.cpp:408-410`). In this group: `cmdToggleTie` (`edit.cpp:2217/2287`),
`cmdAddTie` (`edit.cpp:2065/2171`), `cmdEnterRest` (`edit.cpp:4590-4592`). Commit/rollback while
locked are no-ops: unlock first (`notationundostack.cpp:125-127`, `138-140`).
`Score::undoRemoveStaleTieJumpPoints` (`edit.cpp:8098-8170`) opens and merges its own commands;
it is called from repeat-list rebuilding (`src/engraving/dom/repeatlist.cpp:223`), not from this
group's paths.

**Linked parts / excerpts.** Per operation; nothing batch-specific.

**Crash and corruption risks.**
- Inner `endCmd` without the lock commits the outer macro early (`cmd.cpp:406-447`).
- A non-warning `MScore` error anywhere rolls back the whole batch at commit (`cmd.cpp:418-420`):
  correct for all-or-nothing, but the core must report *which* step set it.
- No layout until commit (G8): a later operation must not depend on layout of an earlier one.
- The undo stack's `UndoMacro` records the input state and selection at open time and restores
  them on undo (`undo.cpp:544-563`); restoring the user's selection inside the macro before
  commit means redo restores the agent's final selection (`undo.cpp:566-579`).
- Rollback `unwind()` undoes and deletes child commands without calling `cleanup`
  (`undo.cpp:242-250`).

**Existing upstream tests.** Every engraving test wraps edits in `startCmd`/`endCmd`
(e.g. `tuplet_tests.cpp:90-92`, `exchangevoices_tests.cpp:56-58`); none exercises the locked
nested case. `src/engraving/tests/readwriteundoreset_tests.cpp` covers undo reset after read.

**Scenarios.**
- NOTES-B-01: write_voice + 8 chord symbols in one batch; one undo entry (S7).
- NOTES-B-02: failure at step 7 (invalid tuplet) leaves the score byte-identical (S16).
- NOTES-B-03: a step that sets `MScore` error (exchange_voices without range in a hand-made
  low-level call) → whole batch rolled back, error names the step.
- NOTES-B-04: batch containing `tie` (inner `startCmd`/`endCmd`) produces one undo entry.
- NOTES-B-05: batch while a plugin holds the stack locked → E_BUSY, no edits applied.

**Open questions.**
1. Whether `Score::undoRedo` (`cmd.cpp:371-398`) is safe to call while a macro is active or the
   stack is locked was not traced; the core must not allow undo/redo inside a batch until it is.

---

## Corrections to COMMANDS.md, assessment.md and the task brief

1. Task brief / CLAUDE.md-adjacent: the action controller is at
   `src/notationscene/internal/notationactioncontroller.cpp` and `notationuiactions.cpp`, not
   `src/notation/internal/` (`git ls-files` shows only `src/notationscene/internal/…`).
2. assessment.md (write_voice): "checks that each bar sums to the time signature" — the engine
   works in the measure's actual, staff-stretched length (`cmd.cpp:1088-1090`, `edit.cpp:526`);
   pickups and irregular bars do not equal the time signature.
3. assessment.md (transactions): "Score::startCmd is not re-entrant … would close the outer one
   early" — the inner `startCmd` is already a no-op when a macro is active (`cmd.cpp:354-357`);
   the inner `endCmd` is what closes the outer macro. Also, with the stack locked,
   `commitChanges`/`rollbackChanges` are no-ops (`notationundostack.cpp:125-127`, `138-140`), so
   `unlock()` must come first; the assessment's order "lock → edits → unlock → commit" is right,
   but rollback must also follow unlock.
4. assessment.md / CLAUDE.md rule 4 ("prefer INotationInteraction"): for this group every
   relevant `INotationInteraction`/`INotationNoteInput` method calls `checkAndShowError`, which
   opens a dialog (G3), and `Score::putNote`/`Score::addTuplet` can open modal boxes (G4). The
   group must call `mu::engraving::Score` methods (the same ones the wrappers call).
5. assessment.md table row "cmdDeleteSelection on a range selection filtered to one voice": correct,
   but that call also deletes annotations and spanners on the voice's track (dynamics, chord
   symbols, staff text sit on voice 1, `edit.cpp:3985-3986`) unless the element-type filter
   excludes them; and rest deletion depends on a real selection (`edit.cpp:3902-3908`), so the
   filter must be set on the score, not only passed to `deleteRange`.
6. assessment.md: "write_voice … enters the new content the way note input would, including ties
   across bar lines": note input's own tie command (`cmdAddTie`) creates notes; joining two
   written notes is `cmdToggleTie`, which can silently fall back to `cmdAddTie`
   (`edit.cpp:2208-2210`) and can tie across voices (`utils.cpp:640-672`).
