# Spec sheet: `pitch`

Group: `set_pitch`, `transpose`.
Source: MuseScore Studio v4.7.6 (`agent/4.7` working tree). Every `file:line` below is
in that tree. Written by `api-scout` for P0.S.

Path note: the action controller and UI action table are in
`src/notationscene/internal/` (`notationactioncontroller.cpp`, `notationuiactions.cpp`);
the Transpose dialog is `src/notationscene/widgets/transposedialog.{h,cpp,ui}`.

**Corrections (2026-10-30, from the implementation and its reviews; DECISIONS 2026-10-30):**
- The tpc range is -8 (`TPC_F_BBB`) to 40 (`TPC_B_SSS`); -1..33 is `TPC_F_BB`..`TPC_B_SS` (pitchspelling.h:40-51).
- `Note::transposeDiatonic` can return false: when the unaltered target degree is out of range (note.cpp:4206-4208); only the final result is octave-wrapped (:4231).
- `select(..., SelectType::REPLACE)` is a no-op (score.cpp:3377-3378): a list selection is `deselectAll` then `ADD`.
- Pitch space is sounding (8va included) or written, as NOTATION.md section 2; not written|concert without the ottava. PITCH-13's expectation is inverted accordingly.
- tpc2 cannot be predicted across a key change: `undoChangeKeySig` runs `Transpose::transpositionChanged` (edit.cpp:6078-6084), which respells tpc2 from the live (possibly respelled) staff transposition (staff.cpp:861-891). The post-condition checks pitch and tpc1 exactly and tpc2 after the call.
- `oneOf` arguments cannot be registered; the shipped arguments are single-typed (DECISIONS 2026-10-30, pitch group). Group error codes are mapped to errors.h.

---

## Shared facts (both commands)

**P1. How a note stores pitch.** A `Note` holds one MIDI pitch and two spellings:
- `m_pitch` is the **concert pitch as notated on the staff line, without any ottava**.
  Note entry computes it from the line and clef, then adds the instrument's transposition
  when the score is not in concert pitch (`src/engraving/dom/noteentry.cpp:143-163`).
  Ottava and capo are added only for playback: `ppitch() = m_pitch + ottaveCapoFret()`
  (`src/engraving/dom/note.cpp:2655-2677`, `2631-2648`). Written pitch is
  `epitch() = m_pitch - transposition()` when concert pitch is off (`note.cpp:2686-2689`),
  where `transposition()` is the instrument's chromatic transposition
  (`note.cpp:1674-1678`).
- `tpc1` is the concert spelling and `tpc2` the transposed (written) spelling
  (`src/engraving/api/v1/elements.h:1407-1412`). `tpc()` returns whichever matches the
  current concert-pitch view (`note.cpp:892-895`, `concertPitch()` is the style flag
  `Sid::concertPitch`, `src/engraving/dom/engravingitem.cpp:1848-1851`).
- The two are independent: "Change enharmonic spelling (current pitch mode only)" changes
  only one of them for a transposing instrument (`src/engraving/dom/score.cpp:959-968`).
- Tpc values are the line of fifths, `TPC_F_BBB` (-1) to `TPC_B_SSS` (33)
  (`src/engraving/dom/pitchspelling.h:40-51`); `tpcIsValid` checks that range
  (`src/engraving/dom/pitchspelling.cpp:48-51`); `pitchIsValid` is 0–127
  (`pitchspelling.h:129`).

**P2. The canonical spelling derivation is `Note::setNval`** (`note.cpp:3018-3054`),
used by note entry. Given a pitch and optionally `tpc1` or `tpc2`:
- neither given: `tpc1 = pitch2tpc(pitch, staff->concertKey(tick), Prefer::NEAREST)`
  (`note.cpp:3032-3035`), the key-aware default also used by `tpc1default`
  (`note.cpp:804-812`);
- only `tpc2` given: `tpc1 = transposeTpc(tpc2, staff->transpose(tick), true)`
  (`note.cpp:3036-3042`);
- `tpc2` missing: `tpc2 = transposeTpc(tpc1, flip(staff->transpose(tick)), true)`
  (`note.cpp:3044-3051`).
`Staff::transpose(tick)` is the *real* transposition, which may be enharmonically
adjusted from the instrument's interval to fit the key (`src/engraving/dom/staff.cpp:861-880`);
use it, not `Instrument::transpose()`, for tpc conversion. (Note: written pitch →
concert pitch uses the instrument's chromatic interval, `noteentry.cpp:156`.)

**P3. Every UI pitch edit bottoms out in `Score::undoChangePitch`**
(`src/engraving/editing/edit.cpp:5994-6000`): one `ChangePitch` per note in
`note->linkList()` via `undoStack()->pushAndPerform`. `ChangePitch::flip` calls
`Note::setPitch(pitch, tpc1, tpc2)` and stores the old values for undo
(`src/engraving/editing/editnote.cpp:43-58`). Callers: `Score::upDown`
(`src/engraving/editing/cmd.cpp:2087`), `Note::transpose` (`note.cpp:4255`),
`Note::transposeDiatonic` (`note.cpp:4234`), repitch entry (`noteentry.cpp:706`),
`changeAccidental2` (`cmd.cpp:2338`), guitar bends (`src/engraving/dom/guitarbend.cpp:165-168`).
`Note::setPitch(int,int,int)` only `assert`s tpc and pitch validity
(`note.cpp:791-798`, `777`), so in a release build bad values are stored silently.
Validation is the caller's job.

**P4. `ChangePitch` does not touch string/fret.** TAB fretting is recomputed during
layout: `ModifyDom::cmdUpdateNotes` → `Chord::cmdUpdateNotes` → `StringData::fretChords`
for TAB staves (`src/engraving/rendering/score/measurelayout.cpp:999`,
`src/engraving/rendering/score/modifydom.cpp:81-129`, `src/engraving/dom/chord.cpp:1138-1152`).
`sortChordNotes` invalidates a fretting whose `getPitch(string, fret) != pitch`
(`src/engraving/dom/stringdata.cpp:595-611`), then `fretChords` assigns new
string/fret through `undoChangeProperty` (`stringdata.cpp:249-297`). A note that cannot
be fretted is marked `fretConflict`, not rejected (`stringdata.cpp:259-270`, `300-305`).
With the user preference "prefer same string for transpose"
(`src/engraving/internal/engravingconfiguration.cpp:483`) a different algorithm runs
(`stringdata.cpp:586-593`, `511-582`). Layout only runs at commit (expression-lines F2),
so **TAB string/fret is only correct after commit**.

**P5. Drum staves.** Percussion notes' line and notehead are recomputed in layout from
the drumset: an invalid drum pitch is drawn on line 0, a valid one gets
`drumset->noteHead(pitch)` and `drumset->line(pitch)` (`src/engraving/dom/utils.cpp:1978-1999`,
called from `chord.cpp:1215-1222`). The whole transpose path skips percussion staves
(`src/engraving/editing/transpose.cpp:108-113`, `157-162`, `207-209`).

**P6. Accidentals.** Layout recomputes AUTO accidentals (`note.cpp:2206-2290`);
an accidental with a non-AUTO role is kept but its type is updated to the new
alteration (`note.cpp:2270-2283`). `Score::upDown` deliberately removes the accidental
from every linked copy before a non-octave pitch change (`cmd.cpp:2070-2086`);
`Note::transpose`/`transposeDiatonic` do not.

**P7. Transaction behaviour.** No function on either path calls `Score::startCmd` /
`endCmd` itself except the `NotationInteraction` wrappers (`startEdit`/`apply`,
`src/notation/internal/notationinteraction.cpp:314-330`), which are no-ops for the undo
stack under the lock but still notify `notationChanged` (expression-lines F1; verified
`src/notation/internal/notationundostack.cpp:105-143`, `cmd.cpp:344-349`, `406-411`).
`git grep` finds no `startCmd`/`endCmd` in `transpose.cpp` or `editnote.cpp`.
`UndoStack::pushAndPerform` needs an active macro or it applies the command without
recording it (`src/engraving/editing/undo.cpp:309-320`), so these calls must run after
the core's `prepareChanges`.

**P8. `NotationInteraction::selectAll()` is unsafe mid-transaction.** It calls
`Score::cmdSelectAll()` (`notationinteraction.cpp:1082-1092`), which ends with
`update()` (`src/engraving/dom/score.cpp:4288-4305`). `Score::update` runs
`doLayoutRange` for any pending layout range and then `cmdState().reset()`
(`cmd.cpp:468-520`). Inside a batch, that lays out half-done edits and **discards the
layout range and change records of earlier steps**, which `endCmd` needs for
`buildScoreChanges(cmdState(), …)` (`cmd.cpp:428`). Build whole-score ranges with
`select(…, RANGE)` on the first and last measures instead (expression-lines F6).
The Transpose dialog calls `selectAll()` when nothing is selected
(`src/notationscene/widgets/transposedialog.cpp:46-49`); the agent must not copy that.

**P9. No modal dialogs on these paths, except one.** `NotationInteraction::transpose`,
`movePitch`, `changeEnharmonicSpelling` do not call `checkAndShowError`. The UI's
`checkAndShowError()` for transpose is in the dialog's `onResolve` lambda in the
controller (`src/notationscene/internal/notationactioncontroller.cpp:1826-1831`), so
calling `INotationInteraction::transpose` directly shows no dialog. The transpose path
can set `MScore::setError(MsError::TRANSPOSE_NO_FRET_DIAGRAM, true)`
(`transpose.cpp:576`, `590`, `628`). That is a *warning* (second argument,
`src/engraving/dom/mscore.h:234`), so `endCmd` does not roll back on it
(`cmd.cpp:418-420`), and nothing on the agent path clears it. The core must read and
clear `MScore::_error` after the call and report it as a warning.

**P10. Selection set-up.** Use the primitives in expression-lines F6
(`clearSelection()` first, then `select({…}, REPLACE)` for lists or two
`select({cr}, RANGE, staffIdx)` calls for ranges). Range transposition reads the user's
**selection filter** in three places (P11); list selections do not.

**P11. Selection filter.** `Score::selectionFilter()` is per-score user state
(`src/engraving/dom/score.h:678-679`; interface
`src/notation/inotationselectionfilter.h:28-38`). `isFiltered(x)` means "x is included"
(`src/engraving/dom/selectionfilter.cpp:34-40`, `52-85`). The range transpose skips
voices the filter excludes (`transpose.cpp:178-185`), skips notes of a chord the
"notes in chord" filter excludes (`transpose.cpp:218-221`), and transposes chord symbols
and fret diagrams only when the filter includes them (`transpose.cpp:255-257`). The
core must either require the filter to be "all" or set it to all and restore it inside
the transaction (open question Q3).

---

### set_pitch  ·  command://notation/set_pitch

**UI entry point.** There is **no single UI method that sets an existing note to an
arbitrary pitch** while keeping the note object. The UI edits pitch in steps:
- Up/Down arrow (`pitch-up`/`pitch-down`, `notationactioncontroller.cpp:212-213`;
  `UiAction` at `src/notationscene/internal/notationuiactions.cpp:285-296`) →
  `NotationActionController::move` (`notationactioncontroller.cpp:1095-1137`) →
  `INotationInteraction::movePitch(d, CHROMATIC)` (`src/notation/inotationinteraction.h:142`;
  `notationinteraction.cpp:4217-4232`) → `Score::upDown(up, UpDownMode::CHROMATIC)`
  (`cmd.cpp:1924-2116`).
- Ctrl+Up/Down octave (`notationactioncontroller.cpp:214-215`) → same with
  `UpDownMode::OCTAVE`; Alt+Shift+Up/Down diatonic (`notationactioncontroller.cpp:453-454`,
  `1214-1229`) → `UpDownMode::DIATONIC`.
- Respelling: `enh-both` / `enh-current` (`notationactioncontroller.cpp:422-423`) →
  `NotationInteraction::changeEnharmonicSpelling` (`notationinteraction.cpp:6489-6494`) →
  `Score::changeEnharmonicSpelling` (`score.cpp:914-962`), which cycles through a
  fixed 3-way enharmonic table (`score.cpp:928-959`).
- Typing a letter name always starts note input and *replaces the chord* through
  `putNote`/`repitchNote` (`notationactioncontroller.cpp:787-829`;
  `noteentry.cpp:574-654`; `repitchReplaceNote` removes all notes of the chord and adds
  a new one, `noteentry.cpp:656-720`). That destroys the note's identity (new EID) and
  collapses chords, so it is not a `set_pitch` path.

**Recommended implementation: reproduce the last block of `Score::upDown` directly on
`Score`** (the "drop to Score where the UI does the same" allowance). For the addressed
note `n`:
1. Compute `(pitch, tpc1, tpc2)` per P2 (see Arguments).
2. For each `t` in `n->tiedNotes()` (`note.cpp:3901-3909`; follows partial ties through
   repeats, `note.cpp:3865-3899`): if `t`'s pitch or either tpc differs, remove the
   accidental from every note in `t->linkList()` via `doUndoRemoveElement`
   (`cmd.cpp:2081-2086`), then `score->undoChangePitch(t, pitch, tpc1, tpc2)`
   (`cmd.cpp:2087`, P3). This is the same per-chain loop `upDown` gets from
   `selection().uniqueNotes()` with `tied = true` (`cmd.cpp:1926`,
   `src/engraving/dom/select.cpp:1602-1627`) and the same loop
   `GuitarBend::setEndNotePitch` uses (`guitarbend.cpp:164-170`).
3. If the addressed staff is TAB: `stringData->convertPitch(pitch, staff, tick, &string, &fret)`
   (`stringdata.cpp:114-117`) and `score->undoChangeFretting(t, pitch, string, fret, tpc1, tpc2)`
   (`edit.cpp:6011-6022`), as `upDown` does in DIATONIC mode (`cmd.cpp:2088-2091`).
   For a standard staff with a linked TAB staff, do nothing extra; layout refrets (P4).
4. Drum staff: `tpc1 = tpc2 = pitch2tpc(pitch, Key::C, Prefer::NEAREST)` as `upDown`
   does (`cmd.cpp:1958-1966`).

Why not loop `movePitch`: (a) spelling is chosen per semitone step by `upDownChromatic`
(`cmd.cpp:1874-1916`) relative to the key and measure accidental state, so the final
spelling depends on the route; reaching a target spelling needs further
`changeEnharmonicSpelling` cycles. (b) Each call fires `notationChanged` under the lock
(P7). (c) On TAB staves `upDown` CHROMATIC moves the fret on the same string and
**returns from the whole function** when the fret goes out of range
(`cmd.cpp:1999-2011`); OCTAVE does the same when there is no next string
(`cmd.cpp:1978`, `1983`). (d) If the selection is a single rest, `movePitch` moves the
rest instead (`notationinteraction.cpp:4223-4225`).

**Plugin API (cautionary).** `note.pitch = x` is `API_PROPERTY_T(int, pitch, PITCH)`
(`src/engraving/api/v1/elements.h:1406`) → `ScoreElement::set` →
`undoChangeProperty(Pid::PITCH, …)` (`src/engraving/api/v1/scoreelement.cpp:220-222`)
→ `Note::setProperty` sets only `m_pitch` (`note.cpp:3148-3151`). `tpc1`/`tpc2` are
left unchanged (pitch and spelling disagree), the tie chain is not followed, and no
accidental is cleaned up. `note.tpc = x` sets only one of TPC1/TPC2
(`src/engraving/api/v1/elements.cpp:168-180`). Do not copy.

**Selection set-up.** None. The recommended path takes the note directly. (If the
conductor chooses the `movePitch` route instead, select the single note with
`select({note}, REPLACE)` per expression-lines F6.)

**Arguments.**
```json
{
  "type": "object",
  "required": ["target", "pitch"],
  "properties": {
    "target": {
      "description": "A note handle, or a structural address resolving to exactly one note",
      "oneOf": [
        {"type": "object", "required": ["handle"], "properties": {"handle": {"type": "string"}}},
        {"type": "object", "required": ["measure", "staff", "voice"],
         "properties": {
           "measure": {"type": "integer", "minimum": 1},
           "beat": {"type": "number"}, "tick": {"type": "integer"},
           "staff": {"type": "object", "properties": {"part": {"type": "string"}, "index": {"type": "integer", "minimum": 1}}},
           "voice": {"type": "integer", "minimum": 1, "maximum": 4},
           "grace": {"type": "integer", "description": "grace chord index before the main chord, optional"},
           "note": {"description": "which note of the chord: current spelled pitch (\"Eb4\") or 1-based index from the bottom",
                    "oneOf": [{"type": "string"}, {"type": "integer", "minimum": 1}]}
         }}
      ]
    },
    "pitch": {"description": "Spelled pitch with octave, C4 = MIDI 60 (\"F#4\", \"Bbb3\"), or a MIDI number",
              "oneOf": [{"type": "string", "pattern": "^[A-Ga-g](bb|b|#|##|x)?-?[0-9]$"}, {"type": "integer", "minimum": 0, "maximum": 127}]},
    "space": {"enum": ["written", "concert"], "description": "Which pitch the value names; default per Q1"},
    "spelling": {"enum": ["auto", "sharps", "flats"], "default": "auto",
                 "description": "Only when pitch is a MIDI number; auto = key-aware nearest (P2)"},
    "tab": {"type": "object", "description": "TAB staves only: explicit string (0 = top) and fret",
            "properties": {"string": {"type": "integer"}, "fret": {"type": "integer"}}}
  }
}
```
Conversion rules (all from P1/P2):
- Spelled pitch → `(step, alter, octave)`; `tpc = step2tpc(step, alter)`
  (`pitchspelling.h:90`); MIDI `= step2pitch(step) + 12*(octave+1) + alter`, which is
  the formula note entry uses with `octave` from `absStep` (`noteentry.cpp:150-152`).
  MuseScore's octave numbering puts middle C (60) at C4 and B#3 = 60; verify against
  `tpc2pitch` (`pitchspelling.h:92`) in a unit test.
- `space = concert`: `pitch` is `m_pitch`, `tpc1 = tpc`, `tpc2` derived (P2).
- `space = written`: `m_pitch = written + instrument->transpose().chromatic`
  (`noteentry.cpp:156`), `tpc2 = tpc`, `tpc1 = transposeTpc(tpc2, staff->transpose(tick), true)`
  (`noteentry.cpp:157-163`).
- Neither form includes ottava (P1).
- `spelling: sharps|flats` mirrors `Score::spellWithSharpsOrFlats`
  (`pitch2tpc(pitch, Key::C, Prefer::SHARPS|FLATS)`, `score.cpp:887-912`).

**Validation before mutation.**
| Check | Source | Error |
|---|---|---|
| Handle resolves to a live `Note` at its structural location | assessment §Addressing | `E_STALE_HANDLE` |
| Address resolves to exactly one note (not a rest, not ambiguous) | — | `E_NOT_A_NOTE` / `E_AMBIGUOUS` |
| Resulting concert `m_pitch` in 0–127 | `pitchspelling.h:129`; `note.cpp:777` asserts only | `E_PITCH_RANGE` |
| tpc valid and `tpc2pitch(tpc) % 12 == pitch % 12` (spelling matches pitch) | `pitchspelling.cpp:48-51` | `E_SPELLING_MISMATCH` |
| Derived other-view tpc: always valid, because `transposeTpc(…, true)` clamps into `TPC_MIN..TPC_MAX` (`transpose.cpp:718-725`, `pitchspelling.cpp:1140-1149`), but that range includes triple sharps and flats (`pitchspelling.h:42-48`). Reject a written spelling beyond double accidentals? (policy) | as cited | `E_SPELLING_RANGE` |
| Drum staff (`staff->part()->instrument(tick)->useDrumset()`, `cmd.cpp:1951-1955`): `drumset->isValid(pitch)`; spelled pitch and `space` rejected | `src/engraving/dom/drumset.h:103`, P5 | `E_DRUM_PITCH` |
| TAB staff with explicit `tab`: `stringData->getPitch(string, fret, staff, tick) == pitch + staff->pitchOffset(tick)` | `cmd.cpp:2017` (same check in `upDown`) | `E_TAB_MISMATCH` |
| TAB staff without `tab`: `convertPitch` returns true | `stringdata.cpp:104-117`, `404-409` (doc comment: returns false if the note cannot be fretted) | `E_TAB_UNPLAYABLE` |
| Every note in the tie chain is on a pitched staff and is not the end note of a guitar bend (Q6) | `guitarbend.cpp:134-145` | `E_TIE_CHAIN` |
| `tab` given on a non-TAB staff | — | `E_BAD_ARGUMENT` |
A linked TAB staff that cannot fret the new pitch is not an error in the UI (P4).
Return it as a warning, `W_TAB_FRET_CONFLICT`, read from `note->fretConflict()` on the
linked TAB note after commit.

**Post-condition.** Immediately after the calls (no layout needed, `ChangePitch` is
applied on push, P3): for every `t` in the original `tiedNotes()` and every link of `t`,
`pitch() == pitch`, `tpc1() == tpc1`, `tpc2() == tpc2`. Note identity (pointer and EID)
unchanged. After commit, for TAB staves (addressed or linked), the string/fret are
valid and `getPitch(string, fret)` matches (P4). Otherwise roll back with
`E_POSTCONDITION`.

**Nested commands.** None. `undoChangePitch`, `undoChangeFretting`,
`doUndoRemoveElement` push undo commands only (`edit.cpp:5994-6022`). No startCmd/endCmd,
no notification (P7). Safe under the lock.

**Linked parts / excerpts.** Propagates: `undoChangePitch` iterates `linkList()`,
covering linked staves in the same score (for example a linked TAB staff) and excerpt
copies (`edit.cpp:5996-5999`). Accidental removal also iterates links
(`cmd.cpp:2081-2086`). `undoChangeFretting` writes the same string/fret to every link
(`edit.cpp:6013-6017`), including a linked standard-staff note, where it is unused. Call
the helpers once per tie-chain note on the *addressed* staff only, never again on a
linked copy. Excerpts that are not open are not laid out at commit unless
`layoutAllParts` (`cmd.cpp:483-487`), so check excerpt post-conditions on the model.

**Crash and corruption risks.**
- Release builds store an invalid pitch/tpc silently (P3). Validation is the only guard.
- Changing one note of a tie chain without the rest leaves a tie between different
  pitches. `git grep` finds no code that repairs or removes such a tie on layout.
  `changeAccidental2` shows the UI's other policy: it **removes** the incoming and
  outgoing ties of a tied-into note whose pitch changes (`cmd.cpp:2317-2327`). This sheet
  follows `upDown` (change the whole chain).
- Microtonal notes: removing the accidental is safe because `updateAccidental`
  re-creates the microtonal accidental from `m_centOffset` (`note.cpp:2211-2226`).
- `undoChangeFretting` on a note whose `tab.string` is out of range is not checked by
  `ChangeFretting::flip` (`editnote.cpp:80-100`). Validate it first.

**Existing upstream tests.**
- `src/engraving/tests/note_tests.cpp:395-421` (`tpcTranspose`: `upDown` CHROMATIC, then
  a concert-pitch toggle; fixtures `note_data/tpc-transpose.mscx`, `-ref`).
- `note_tests.cpp:423-445` (`tpcTranspose2`), `note_tests.cpp:367-389` (`tpc`, entry
  spelling).
- `src/engraving/tests/tools_tests.cpp:200-224` (`changeEnharmonicBoth/Current`; fixture
  `tools_data/change-enharmonic.mscx` with five reference states).
- No test calls `undoChangePitch` directly; no test covers TAB or drum pitch change
  (`git grep` in `src/engraving/tests`).

**Scenarios.**
- `PITCH-01` Set a single note in C major to F#4 (concert, non-transposing). Check pitch
  66, tpc1 = tpc2 = F#, and an AUTO sharp after commit.
- `PITCH-02` Same pitch, respell only (F#4 → Gb4). Pitch unchanged, both tpcs changed,
  one undo entry.
- `PITCH-03` Middle note of a three-note tie chain. All three notes change, ties intact.
- `PITCH-04` Tie chain across a volta (partial ties). Every branch changes.
- `PITCH-05` Bb clarinet, `space = written`, D5 → concert pitch 72 (C5), tpc1 C, tpc2 D.
  Repeat with concert-pitch view on; result identical.
- `PITCH-06` Bb clarinet, MIDI number + `spelling: auto` in Eb major. Key-aware tpc1.
- `PITCH-07` Note with a user (bracketed) accidental. The accidental is removed on
  every linked copy (UI parity).
- `PITCH-08` Standard staff with a linked TAB staff. After commit, TAB string/fret
  match the new pitch; undo restores both.
- `PITCH-09` TAB-only staff with explicit `tab: {string, fret}`; mismatch rejected
  with `E_TAB_MISMATCH`.
- `PITCH-10` Drum staff: valid drumset pitch accepted (notehead and line update after
  commit); pitch not in the drumset rejected with `E_DRUM_PITCH`.
- `PITCH-11` Note in an excerpt-linked part. The part's note follows.
- `PITCH-12` Out of range (C-1 − 1, G9 + 1) and inconsistent spelling ("E#4" with
  MIDI 66) are rejected and the score is untouched.
- `PITCH-13` Note under an 8va: setting C5 stores `m_pitch` 72 and playback is 84.
- `PITCH-14` Batch: set_pitch on a note, then on another note in its tie chain, in one
  transaction. Last write wins; one undo entry.

**Open questions.** See Q1, Q2, Q5, Q6 at the end.

---

### transpose  ·  command://notation/transpose

**UI entry point.**
- Menu Tools › Transpose… (`UiAction("transpose")`, `notationuiactions.cpp:423-428`) →
  `registerAction("transpose", &Controller::openTransposeDialog)`
  (`notationactioncontroller.cpp:317`) → `openTransposeDialog` opens
  `musescore://notation/transpose` and calls `checkAndShowError()` on resolve
  (`notationactioncontroller.cpp:1826-1831`). The URI is registered to the widget
  `TransposeDialog` (`src/notationscene/notationscenemodule.cpp:84`). On accept,
  `TransposeDialog::apply` builds `TransposeOptions` and calls
  `INotationInteraction::transpose(options)` (`transposedialog.cpp:64`, `206-224`).
- `NotationInteraction::transpose` (`src/notation/inotationinteraction.h:234`;
  `notationinteraction.cpp:6014-6024`): `startEdit("Transposition")`,
  `Transpose::transpose(score(), mode, direction, key, interval, needTransposeKeys,
  needTransposeChordNames, needTransposeDoubleSharpsFlats)`, `apply()`, returns the bool.
- Engine: `Transpose::transpose` (`src/engraving/editing/transpose.cpp:41-294`).
- Related shortcuts on the same engine: `transpose-up`/`-down`
  (`notationactioncontroller.cpp:475-476`) → `transposeSemitone` →
  `Transpose::transpose(BY_INTERVAL, dir, Key::C, 1 /*augmented unison*/, trKeys=true,
  chordNames=true, doubleSharpsFlats=false)` (`notationinteraction.cpp:8310-8315`,
  `transpose.cpp:371-384`). `transposeDiatonicAlterations` →
  `DIATONICALLY, 1, true, true, true` (`notationinteraction.cpp:8317-8322`,
  `transpose.cpp:389-398`).

**Driving it without the dialog.** Call `INotationInteraction::transpose(options)`
directly with `TransposeOptions` (`src/notation/notationtypes.h:433-442`):
| Dialog control | Option | Mapping |
|---|---|---|
| Chromatic / Diatonic group, To key / By interval | `mode` | `chromatic ? (byKey ? TO_KEY : BY_INTERVAL) : DIATONICALLY` (`transposedialog.cpp:122-127`; enum `src/engraving/dom/mscore.h:98-100`) |
| Closest / Up / Down | `direction` | `CLOSEST` only for TO_KEY (`transposedialog.cpp:153-169`; enum `mscore.h:89-91`) |
| Key list | `key` | `Key(index - 7)`, so -7 = C♭ … +7 = C♯, **concert key** (`transposedialog.cpp:243-246`; label "Transpose to key (specified at concert pitch)", `transposedialog.ui:35`) |
| Interval list | `interval` | index into `Interval::allIntervals` 0–25 (perfect unison … perfect octave), `(diatonic, chromatic)` pairs at `src/engraving/dom/interval.cpp:124-159`; UI order identical (`transposedialog.ui:201-332`) |
| Degree list | `interval` | `index + 1`, 1 = second … 6 = seventh (`transposedialog.cpp:248-253`, `transposedialog.ui:410-441`) |
| Transpose key signatures / Keep degree alterations | `needTransposeKeys` | chromatic: key signatures; diatonic: keep alterations (`transposedialog.cpp:231-236`) |
| Transpose chord symbols and fretboard diagrams | `needTransposeChordNames` | `transposedialog.cpp:238-241` |
| Accidentals: single / double | `needTransposeDoubleSharpsFlats` | index 1 = double (`transposedialog.cpp:255-258`) |
Dialog defaults (first use): chromatic, to key, closest, C, keys on, chord names on,
double sharps/flats on, keep degree alterations on (`transposedialog.h:33-47`).
Don't reuse the dialog's static `previousState()`
(`transposedialog.cpp:260-264`): it is UI memory.

**What the engine does** (`transpose.cpp:41-294`):
- **Interval.** TO_KEY: the interval from the **concert** key of the first pitched staff in
  `[staffStart, staffEnd)` at the range start to `key` (`transpose.cpp:60-74`,
  `keydiff2Interval` `512-528`). Same key with CLOSEST is a no-op returning `true`
  (`transpose.cpp:80-81`); with UP/DOWN it is an octave (`76-79`). BY_INTERVAL:
  `allIntervals[interval]`, flipped for DOWN (`transpose.cpp:85-88`), **no bounds
  check** on `interval` (std::array `operator[]`). DIATONICALLY: steps negated for DOWN
  (`transpose.cpp:100-102`).
- **Silent option overrides.** Non-range selection forces key signatures off
  (`transpose.cpp:91-93`). Any whole-octave chromatic interval (including unison)
  forces key signatures and chord symbols off unless TO_KEY (`transpose.cpp:94-98`).
- **Notes, chromatic.** `Note::transpose(interval, useDouble)`: new pitch = pitch +
  chromatic; returns **false without changing the note** if out of 0–127.
  `tpc1 = transposeTpc(tpc1, interval)`; `tpc2` re-derived via `staff->transpose`
  (`note.cpp:4238-4256`). Spelling follows the interval exactly (C up a d2 = D♭),
  clamped to single accidentals unless `useDoubleSharpsFlats`
  (`transpose.cpp:718-725`, `pitchspelling.cpp:1140-1149`). Not key-aware.
- **Notes, diatonic.** `Note::transposeDiatonic(steps, keepAlterations, useDouble)`
  moves by scale degrees of the **current displayed key** `staff->key(tick)` and
  `epitch()/tpc()` (`note.cpp:4194-4236`). An out-of-range result is **silently
  octave-wrapped** by `clampPitch(newPitch, true)` (`note.cpp:4229`,
  `pitchspelling.cpp:1151-1163`); it always returns true.
- **Grace notes** are transposed with their parent chord in range mode
  (`transpose.cpp:227-233`).
- **Key signatures** (chromatic modes, `needTransposeKeys`, range only): only `KeySig`
  elements in `[s1, s2)` that are a key change (`KeySig::isChange`,
  `src/engraving/dom/keysig.cpp:149-159`), or at the range start and not a system-header
  key (`isFirstSystemKeySig`, `src/engraving/dom/utils.cpp:1461-1471`), are changed
  through `Score::undoChangeKeySig` with the new concert key
  (`transpose.cpp:234-253`). `undoChangeKeySig` derives each staff's written key from its
  instrument and `preferSharpFlat`, and handles every linked staff and excerpt
  (`edit.cpp:6028-6085`). Keys are transposed with `PreferSharpFlat::NONE`, so C major up
  an augmented unison becomes C♯ major, 7 sharps (`transpose.cpp:242`, `698-709`,
  `pitchspelling.cpp:1165-1182`). **No key signature is inserted at a mid-score range
  start or restored after the range end.** The only creation path is tick 0 when the
  range starts at tick 0 and a staff has no key signature (`transpose.cpp:276-292`).
  Transposing bars 9–16 with "keys" therefore changes only the key changes inside
  9–16, and a key change at bar 17 is not involved.
- **Range start snaps to the bar start** when the first selected CR is at the start of
  its bar, so the bar's key signature is included (`transpose.cpp:195-200`).
- **Chord symbols** (`needTransposeChordNames`). Range: every `Harmony`/`FretDiagram`
  annotation in a selected track that the filter includes (`transpose.cpp:255-271`).
  List: selected `Harmony`/`FretDiagram` elements (`transpose.cpp:119-132`). Each
  harmony and **all its links** get `TransposeHarmony` (chromatic, interval corrected
  for instrument transposition, `transpose.cpp:540-563`) or `TransposeHarmonyDiatonic`
  (by degrees of the harmony staff's key, `transpose.cpp:774-794`). Fret diagrams are
  re-derived from the transposed chord name; if no diagram pattern exists the diagram is
  cleared and the warning `TRANSPOSE_NO_FRET_DIAGRAM` is set (`transpose.cpp:565-639`).
  Chord symbols are **not** transposed by notes-only selections unless the harmony is in
  the list.
- **Skipped:** percussion staves (P5); disabled segments (`transpose.cpp:203-205`);
  linked staves already covered by another staff in the range (`transpose.cpp:166-175`);
  voices and chord notes excluded by the selection filter (P11).
- **Ties are not followed.** The range loop transposes notes in segments
  `[s1, s2)` only (`transpose.cpp:202-233`). The list loop uses `uniqueElements()`, not
  `uniqueNotes()` (`transpose.cpp:106`, `select.cpp:1576-1592`). A tie that crosses the
  range boundary ends up joining notes of different pitch.

**Selection set-up.**
- Range (the normal case, and required for TO_KEY and key signatures):
  `clearSelection()`, `select({firstCR}, RANGE, firstStaff)`, `select({lastCR}, RANGE,
  lastStaff)`. For whole bars, select the first and last `Measure` with `RANGE`
  (expression-lines F6). For the whole score, select the first and last measure across
  staff 0 and the last staff. **Do not use `selectAll()`** (P8). Then read back
  `selection()->range()` / `staffStart`/`staffEnd`. Staves must be contiguous: one call
  per contiguous block, all inside the same transaction.
- List (specific notes / chord symbols): `select({…}, REPLACE)`. A list selection has
  `staffStart = staffEnd = 0` and no start segment (`select.cpp:451-475`,
  `score.cpp:3388-3416`), so only the list branch runs (`transpose.cpp:187-190`).
- Dialog quirk the agent must not inherit. The dialog disables "To key" for non-range
  selections (`transposedialog.cpp:51-54`), but `restorePreviousState()` runs afterwards
  and can re-check it (`transposedialog.cpp:62`, `273-274`). With a list selection, TO_KEY
  then computes the interval from C, because the staff loop `[0, 0)` is empty
  (`transpose.cpp:63-72`). The agent rejects TO_KEY without a range.

**Arguments.**
```json
{
  "type": "object",
  "required": ["range", "by"],
  "properties": {
    "range": {
      "oneOf": [
        {"type": "object", "description": "bars/beats across contiguous staves",
         "properties": {
           "start": {"type": "object", "properties": {"measure": {"type": "integer"}, "beat": {"type": "number"}, "tick": {"type": "integer"}}},
           "end":   {"type": "object", "properties": {"measure": {"type": "integer"}, "beat": {"type": "number"}, "tick": {"type": "integer"}}},
           "staves": {"type": "array", "items": {"type": "object"}, "description": "part name + staff index; omitted = all staves"}
         }},
        {"type": "object", "required": ["handles"], "properties": {"handles": {"type": "array", "items": {"type": "string"}}}},
        {"const": "score"}
      ]
    },
    "by": {
      "oneOf": [
        {"type": "object", "required": ["interval", "direction"], "properties": {
          "interval": {"enum": ["P1","A1","d2","m2","M2","A2","d3","m3","M3","A3","d4","P4","A4","d5","P5","A5","d6","m6","M6","A6","d7","m7","M7","A7","d8","P8"]},
          "direction": {"enum": ["up", "down"]},
          "octaves": {"type": "integer", "minimum": 0, "maximum": 3, "default": 0}}},
        {"type": "object", "required": ["key"], "properties": {
          "key": {"type": "integer", "minimum": -7, "maximum": 7, "description": "concert key, sharps positive"},
          "direction": {"enum": ["closest", "up", "down"], "default": "closest"}}},
        {"type": "object", "required": ["degrees", "direction"], "properties": {
          "degrees": {"type": "integer", "minimum": 1, "maximum": 6, "description": "1 = second … 6 = seventh"},
          "direction": {"enum": ["up", "down"]},
          "keep_alterations": {"type": "boolean", "default": true}}}
      ]
    },
    "key_signatures": {"type": "boolean", "default": true},
    "chord_symbols": {"type": "boolean", "default": true},
    "double_accidentals": {"type": "boolean", "default": true},
    "ties": {"enum": ["reject", "extend"], "default": "reject"}
  }
}
```
- The interval names map 1:1 to `allIntervals` indexes 0–25 (`interval.cpp:126-158`).
- `octaves` covers compound intervals. The engine only knows intervals up to an octave,
  so the core makes a second call with index 25 (P8) in the same transaction. A
  whole-octave call forces keys and chord symbols off, which is harmless
  (`transpose.cpp:94-98`).
- `key` is a concert key, matching the dialog. Converting a written key for a
  transposing part is the agent's (Claude's) job.

**Validation before mutation.**
| Check | Source | Error |
|---|---|---|
| Interval index 0–25 (no bounds check in engine) | `transpose.cpp:85` | `E_BAD_INTERVAL` |
| Degrees 1–6 (dialog range) | `transposedialog.ui:410-441` | `E_BAD_INTERVAL` |
| `key` −7…+7 | `transposedialog.cpp:245` | `E_BAD_KEY` |
| TO_KEY or `key_signatures` with a non-range target | `transpose.cpp:91-93`, dialog `51-54` | `E_NEEDS_RANGE` |
| Range resolves to a live selection; staves contiguous | expression-lines F6 | `E_RANGE` |
| At least one non-percussion staff in range | P5 | `E_NOTHING_TO_TRANSPOSE` |
| Chromatic: every affected note's `pitch + chromatic` in 0–127 (engine returns false and leaves a partial transposition) | `note.cpp:4240-4243` | `E_PITCH_RANGE` |
| Diatonic: same check with the worst case ±(steps·2) semitones, or exact per-note computation, because the engine wraps by octaves instead of failing | `note.cpp:4229` | `E_PITCH_RANGE` |
| Ties crossing the range boundary (any note with `tieBack` start outside, or `tieFor` end outside): reject, or with `ties: extend` widen the range to the chain ends | `transpose.cpp:202-233`; chain via `note.cpp:3901-3909` | `E_TIE_CHAIN` |
| Selection filter is "all", or the core overrides and restores it (Q3) | P11 | `E_SELECTION_FILTER` |
| Text or element edit in progress | `notationinteraction.cpp:1122-1135` | handled by core |

**Post-condition.** `transpose()` returns `bool`, and `false` only means some chromatic
note was out of range (`transpose.cpp:116-118`, `223-225`). Treat `false` as
`E_PITCH_RANGE` and roll back, because partial changes remain. Before the call, snapshot
every note the engine will visit (same iteration rules as above) with `(pitch, tpc1,
tpc2)`. After the call:
- chromatic: each note's `pitch == old + chromatic` and `tpc1 == transposeTpc(old tpc1,
  interval, double)` (`transpose.cpp:718-725`);
- diatonic: each note changed, `pitch` in range, and every tie chain still has a single
  pitch and tpc1;
- keys: for each key change touched, `staff->keySigEvent(tick).concertKey() ==
  transposeKey(old, interval)` (`transpose.cpp:242`);
- chord symbols: each harmony's `rootTpc` moved;
- then read and clear `MScore::_error`. `TRANSPOSE_NO_FRET_DIAGRAM` becomes a warning
  in the response (P9).
If the snapshot shows no notes, chords or keys changed and the call was not the
documented same-key no-op, return `E_NOTHING_TO_TRANSPOSE`.

**Nested commands.** `NotationInteraction::transpose` wraps `startEdit`/`apply`. These
are no-ops under the lock but fire `notationChanged` mid-transaction (P7,
expression-lines Q1). `Transpose::transpose` itself has no startCmd/endCmd (P7).
`undoChangeKeySig` can call `Transpose::transpositionChanged`
(`edit.cpp:6078-6084`, `transpose.cpp:403-472`), which has no startCmd either. Safe
under the lock. Alternative if the notification is a problem: call
`Transpose::transpose(score, …)` directly, which is what every upstream test does
(`src/engraving/tests/transpose_tests.cpp:53-56`).

**Linked parts / excerpts.** Propagates. Notes via `undoChangePitch` → `linkList`
(P3). Harmonies loop over `linkList()` explicitly because `TransposeHarmony` itself
does not link (`transpose.cpp:551-561`). Key signatures via `undoChangeKeySig` over
`staffList()` (`edit.cpp:6033`). New tick-0 key signatures via `undoAddElement`
(`transpose.cpp:289`). Linked staves inside the range are de-duplicated so nothing is
transposed twice (`transpose.cpp:166-175`, list: `select.cpp:1576-1592`). **Fret
diagrams do not propagate:** `FretDataChange` updates only the one diagram
(`src/engraving/editing/editfretboarddiagram.cpp:89-99`) (Q4). Linked TAB staves
refret at layout (P4). Unopened excerpts are not laid out at commit
(`cmd.cpp:483-487`); the chord-symbol part test passes `layoutAllParts = true`
(`src/engraving/tests/chordsymbol_tests.cpp:252-259`).

**Crash and corruption risks.**
- Out-of-bounds `allIntervals[interval]` for an index outside 0–25 (`transpose.cpp:85`).
- **Partial transposition** when a chromatic note is out of range: the engine keeps
  going and leaves the earlier notes changed (`transpose.cpp:116-118`, `223-225`).
  `TransposeDialog::apply` ignores the return value (`transposedialog.cpp:219`).
- **Broken tie chains** at range edges (see above).
- **Non-undoable fret-diagram mutation.** In the "no chord symbol attached" branch,
  `transposeFretDiagram` attaches a temporary `Harmony` with `FretDiagram::setHarmony`,
  which is not undo-recorded (`transpose.cpp:603`, `src/engraving/dom/fret.cpp:957-967`).
  It deletes the harmony only on the success path (`transpose.cpp:633-634`); the
  early return at `transpose.cpp:626-629` leaves it attached outside undo. Rolling back
  the transaction does not remove it.
- **Chord-symbol undo is not an exact inverse.** `TransposeHarmony::flip` and
  `TransposeHarmonyDiatonic::flip` re-apply the flipped interval instead of restoring the
  stored tpcs (`transpose.cpp:748-760`, `774-794`). Clamping to single accidentals, or a
  diatonic step without "keep alterations" (alterations dropped,
  `transpose.cpp:671-692`), means undo or rollback can restore a different spelling
  (for example F♯ up a 2nd without alterations gives G, and undo gives F). Notes are
  unaffected because `ChangePitch` stores old values. No upstream test exercises
  diatonic chord-symbol transposition: `undoDiatonicTransposeChordSymbols` passes
  `transposeChordNames = false` (`transpose_tests.cpp:83-84`, `117-120`), and the two
  "Diatonic…FretDiagrams" tests use the chromatic helper (`transpose_tests.cpp:122-130`).
- **Layout-dependent key handling.** `isFirstSystemKeySig` reads `measure()->system()`
  (`utils.cpp:1461-1471`). In a batch after structural edits that have not been laid
  out, the system pointer is stale or null, and the key signature at the range start
  may be treated differently from the same call made after commit.
- `selectAll()` mid-transaction drops earlier change records (P8).

**Existing upstream tests.**
- `src/engraving/tests/transpose_tests.cpp:96-135`: undo round-trips for chromatic M2 up
  and diatonic 4th down; fixtures in `src/engraving/tests/transpose_data/` (notes,
  chord symbols, fret diagrams, each with `01-ref`/`02-ref`).
- `src/engraving/tests/keysig_tests.cpp:142-164` (`preferSharpFlat`, A1 up with keys;
  `keysig_data/preferSharpFlat-2.mscx`), `keysig_tests.cpp:129-141` (concert pitch).
- `src/engraving/tests/chordsymbol_tests.cpp:242-259` (`testTranspose`,
  `testTransposePart`; `chordsymbol_data/transpose*.mscx`), `341-355` (realize after
  transpose; note it calls `Transpose::transpose` outside any `startCmd`, line 346).
- All call `Transpose::transpose` after `cmdSelectAll()`, never through
  `NotationInteraction`. No test covers list selections, TO_KEY, tie chains, TAB or
  percussion skipping, or the selection filter.

**Scenarios.**
- `TRN-01` Whole score up M2 with keys and chord symbols (`transpose_data/undoTransposeChordSymbols.mscx`).
  Notes, keys and symbols move; one undo restores the reference.
- `TRN-02` TO_KEY: concert E♭ → concert D, closest, with a B♭ trumpet part. Concert key
  D, written key E; check `tpc2` spelling in the trumpet part.
- `TRN-03` Bars 9–16 only, with keys on. Notes move; no key signature is inserted at
  bar 9 or 17 (UI parity, documented behaviour).
- `TRN-04` Diatonic third up in G major with "keep alterations". C♯ becomes E♯ and the
  key signature is unchanged.
- `TRN-05` A1 up with single accidentals: B → B♯ is clamped to C (`clampEnharmonic`).
  Check tpc.
- `TRN-06` Compound interval: M10 up = M3 + `octaves: 1`. One undo entry.
- `TRN-07` Range whose last note ties out of the range: `E_TIE_CHAIN`; with
  `ties: extend` the whole chain moves.
- `TRN-08` Chromatic out of range (G9 up m2): `E_PITCH_RANGE`, score untouched.
- `TRN-09` Diatonic near the top: rejected rather than silently octave-wrapped.
- `TRN-10` Range with a drum staff and a pitched staff: drum staff untouched.
- `TRN-11` Standard staff + linked TAB: after commit the TAB string/fret are valid.
- `TRN-12` Excerpt open and closed: part notes, keys and chord symbols follow.
- `TRN-13` List of three chord symbols only: symbols move, notes do not.
- `TRN-14` Fret diagram with no chord symbol whose transposed shape is unknown: warning
  `TRANSPOSE_NO_FRET_DIAGRAM`; then roll back and check no stray `Harmony` stays on the
  diagram (exercises the corruption risk).
- `TRN-15` User selection filter excluding voice 2: per the Q3 decision, either
  rejected or voice 2 transposed and the filter restored.
- `TRN-16` TO_KEY with a list target: `E_NEEDS_RANGE`.
- `TRN-17` TO_KEY to the same key with `closest`: documented no-op, no undo entry
  content.

**Open questions.** See Q3, Q4, Q7, Q8 below.

---

## Open questions

- **Q1. Which pitch does the compact notation speak: written or concert?** `set_pitch`
  needs a default for `space`. The assessment's notation example says "sounds as
  written" but doesn't define the rule for transposing parts (`assessment.md:137-141`).
  The read and write sides must agree; conductor's decision. The ottava rule (P1:
  `m_pitch` excludes ottava) should be stated in the notation legend either way.
- **Q2. Spelling-only edits.** Should `set_pitch` also allow setting `tpc1` and `tpc2`
  independently (the `enh-current` capability, `score.cpp:959-968`)? The schema above
  derives the other spelling (P2), which loses a deliberately independent written
  spelling.
- **Q3. Selection filter policy.** Reject when the user's filter is not "all", or set
  it to all inside the transaction and restore it. Setting it through
  `INotationSelectionFilter::setSelectionTypeFiltered`
  (`inotationselectionfilter.h:33-34`) changes visible UI state; whether it notifies
  the selection-filter panel was not traced.
- **Q4. Fret diagrams in excerpts.** `FretDataChange` updates only the diagram passed
  in (`editfretboarddiagram.cpp:89-99`) and `transposeFretDiagram` does not loop links.
  Whether excerpt fret diagrams are separate linked objects that then diverge was not
  resolved from source. It needs a fixture.
- **Q5. TAB-addressed `set_pitch` without `tab`.** Use `convertPitch` +
  `undoChangeFretting` (as `upDown` DIATONIC does), or only `undoChangePitch` and let
  layout refret (as every standard-staff edit does for a linked TAB)? The two can pick
  different strings when "prefer same string" is on (P4).
- **Q6. Guitar bends.** A bend's amount is derived from end-note pitch minus start-note
  pitch (`guitarbend.cpp:134-145`). Whether changing either note's pitch should be
  allowed, rejected, or should update the bend was not resolved.
- **Q7. Range end at end of score.** `selection.endSegment()` is null for a range to
  the last measure, and the loop then runs to the end of the score (`transpose.cpp:201-202`).
  That is correct, but the tie-chain and snapshot pre-scan must use the same rule.
- **Q8. Calling `NotationInteraction::transpose` vs `Transpose::transpose`.** The former
  is the UI path but notifies mid-transaction (expression-lines Q1). The latter is what
  upstream tests use and avoids the notification. Conductor's decision (same trade-off
  as expression-lines Q2).

## Corrections to planning documents

- `assessment.md:131` lists `transpose(options)` among methods that "return void". It
  returns `bool` (`inotationinteraction.h:234`), but `false` only signals a chromatic
  out-of-range note, and the edit is left partially applied. Diatonic never returns
  false.
- `assessment.md` (Write model table) says `transpose` is built on "the native transpose
  used by the Transpose dialog" with "by interval or to key". The engine also has a
  diatonic mode. Intervals are limited to 26 entries up to one octave, and a mid-score
  range does not get key signatures inserted at its edges.
- `assessment.md` lists `set_pitch` in the notes group "built on setNoteRest, addNote…".
  No `INotationInteraction` method sets an arbitrary pitch on an existing note. The
  faithful path is `Score::undoChangePitch` plus the tie-chain and accidental handling
  copied from `Score::upDown`.
