# Spec sheet: `read` group (MuseScore Studio 4.7.6)

Scout: `api-scout`, task P0.S-read. Source: branch `agent/4.7` at `v4.7.6`.
Commands: score_overview, score_read, score_export, score_render, score_changes, score_validate.

All paths are relative to the repo root. Abbreviations:
`nac.cpp` = `src/notationscene/internal/notationactioncontroller.cpp`,
`ni.cpp` = `src/notation/internal/notationinteraction.cpp`,
`cmd.cpp` = `src/engraving/editing/cmd.cpp`, `undo.cpp`/`undo.h` = `src/engraving/editing/undo.{cpp,h}`,
`score.cpp`/`score.h` = `src/engraving/dom/score.{cpp,h}`,
`eps.cpp` = `src/project/internal/exportprojectscenario.cpp`,
`np.cpp` = `src/project/internal/notationproject.cpp`,
`bapi.cpp` = `src/converter/internal/compat/backendapi.cpp`,
`twrite.cpp` = `src/engraving/rw/write/twrite.cpp`,
`paint.cpp` = `src/engraving/rendering/score/paint.cpp`,
`npaint.cpp` = `src/notation/internal/notationpainting.cpp`.

Shared facts already established in `specs/notes.md` §0 (notes-G1…G10) and `specs/generic.md`
§0 (GG1…GG8) are cited, not repeated. For reads, "post-condition" means: what guarantees the
read reflects the committed, laid-out state. Error codes are proposed names for the conductor's
error table. The compact notation is not designed here (P1.3); this sheet says what the model
exposes and how to walk it.

---

## 0. Group-wide findings (R1–R10)

**R1. Getting at the score.** `IGlobalContext::currentProject()`, `currentMasterNotation()`,
`currentNotation()` and their change notifications (`src/context/iglobalcontext.h:39-48`).
`currentNotation()` is whatever tab the user is looking at and may be a part (excerpt);
`IMasterNotation::masterScore()` (`src/notation/imasternotation.h:45`) is the full score;
`IMasterNotation::excerpts()` (`imasternotation.h:54`) lists initialised parts, each with
`IExcerptNotation::notation()`, `name()`, `isInited()` (`src/notation/iexcerptnotation.h:37-49`).
`potentialExcerpts()` (`imasternotation.h:56`) are parts that do not exist in the score yet.
`INotation::elements()->msScore()` gives the engraving `Score*` of any notation
(`src/notation/inotationelements.h:35`). Project-level facts: `INotationProject::path()`,
`needSave()`, `isNewlyCreated()` (`src/project/inotationproject.h:41`, `65`, `58`).

**R2. When a read sees committed, laid-out state.**
- No macro open and stack not locked: `UndoStack::hasActiveCommand()` (`undo.h:272`),
  `UndoStack::isLocked()` (`undo.h:269`), or `INotationUndoStack::isLocked()`
  (`src/notation/internal/inotationundostack.h:59`). Between `startCmd` and `endCmd` the tree
  holds uncommitted edits and no layout has run (notes-G8).
- Layout runs only in `Score::update()` from `endCmd` (`cmd.cpp:426`) and from `undoRedo`
  (`cmd.cpp:392`). `update()` lays out **only scores that are open**, plus the score that
  issued the command: `if (s != this && !s->isOpen() && ms->scoreList().size() > 1 && !layoutAllParts) continue;`
  (`cmd.cpp:482-490`). `Score::autoLayoutEnabled()` is just `isOpen()` (`score.cpp:5996-5999`).
  So a part whose tab has never been opened can have stale or no layout. Upstream lays such
  scores out by hand before export (`eps.cpp:133-139`, `bapi.cpp:784-794`).
- Layout reflects the **current view mode** of that score (`Score::layoutMode()`, `score.h:927`):
  page, continuous (LINE), single system, etc. Page/system membership and positions are
  meaningless across modes.
- Reads must never call `Score::update()` (no-arg resets `cmdState` and discards change
  records, `score.h:596`, `cmd.cpp:518-520`) nor anything that reaches it (for example
  `cmdSelectAll`, pitch spec). Nothing in the walk below calls it.
- Layout-derived values (stale mid-transaction, mode-dependent): `MeasureBase::no()` (set in
  `MeasureLayout::adjustMeasureNo`, `src/engraving/rendering/score/measurelayout.cpp:1189-1203`),
  `Measure::system()`, multi-measure rests (`Measure::mmRest()`, `measure.h:315`), cross-measure
  display values (`ChordRest::durationType()`/`dots()` return the layout-set cross-measure
  value when `crossMeasure()==FIRST`, `src/engraving/dom/chordrest.h:99-120`; set by
  `ChordLayout::crossMeasureSetup`, `src/engraving/rendering/score/modifydom.cpp:68`), every
  bbox/pos, and spanner segments. Everything in R6 marked "structural" is not.

**R3. A user edit may be in progress when a request arrives.** Interaction state getters:
`isTextEditingStarted()` (`src/notation/inotationinteraction.h:152`), `isDragStarted()` (`:97`),
`isGripEditStarted()` (`:168`), `isEditingElement()` (`:174`), and
`noteInput()->isNoteInputMode()` (`src/notation/inotationnoteinput.h:36`). During text editing
every keystroke is its own committed macro, merged when editing ends
(`src/engraving/editing/textedit.cpp:162-170`, `183`, `198`, `235`). A read during text editing
therefore sees half-typed text that is already "committed". Reads should report these flags
rather than refuse; writes are the session/transaction layer's concern.

**R4. EIDs: how they really work.**
- *Storage.* An EID is two `uint64_t` (`src/engraving/infrastructure/eid.h:62-63`). It is **not
  stored in the object**: `EngravingObject::eid()` looks the object's address up in the master
  score's register, `masterScore()->eidRegister()->EIDFromItem(this)`
  (`src/engraving/dom/engravingobject.cpp:711-714`). One register per `MasterScore`, shared by all
  its parts (`src/engraving/dom/masterscore.h:95-96`, `214`). The register is two hash maps
  keyed by EID and by `EngravingObject*` (`src/engraving/infrastructure/eidregister.h:48-49`).
- *Assignment is lazy.* New elements have no EID. One is assigned by `assignNewEID()`
  (`engravingobject.cpp:721-724`), which upstream calls when saving
  (`TWrite::writeItemEid`, `twrite.cpp:495-511`), exporting MEI/MNX
  (`src/importexport/mei/internal/meiexporter.cpp:2689-2691`,
  `src/importexport/mnx/internal/export/mnxexporter.cpp:39-41`), and for automation and playback
  ids (`src/engraving/automation/internal/automationcontroller.cpp:287-289`,
  `src/engraving/playback/playbackmodel.cpp:409-411`). The plugin API's `eid` property is
  read-only and never assigns (`src/engraving/api/v1/scoreelement.h:104`).
  `EID::invalid().toStdString()` is a well-formed-looking string (both halves `UINT64_MAX`,
  `eid.cpp:135-153`), so the module must test `isValid()` (`eid.h:39`) before serialising.
- *Generation.* Random 128-bit (`eid.cpp:182-189`); sequential when `MScore::testMode`
  (`src/engraving/infrastructure/eidregister.cpp:33`, `eid.cpp:194-198`).
- *Persistence.* Written for every item that goes through `TWrite::writeItemProperties`
  (`twrite.cpp:550-552`), plus `Staff` (`twrite.cpp:2743`), `Measure`
  (`src/engraving/rw/write/measurewrite.cpp:59`) and the `Score` (`src/engraving/rw/write/writer.cpp:117`).
  Not written for `Part` (it has its own persistent `muse::ID`, `twrite.cpp:2485`,
  `src/engraving/dom/part.h:207`), `Segment`, `System`, `Page` (`twrite.cpp`, the `write`
  overloads for those types call no EID writer), nor for `generated()` items, which are
  skipped (`twrite.cpp:3587-3589`). Read back by `TRead::readItemEID`
  (`src/engraving/rw/read460/tread.cpp:578-586`) and the 4.10 reader
  (`src/engraving/rw/read410/tread.cpp:526`). Disabled entirely when the setting
  `engraving/compat/doNotSaveEIDsForBackCompat` is true (default false,
  `src/engraving/internal/engravingconfiguration.cpp:157`; checked at `twrite.cpp:497`).
  Linked part items write `<linkedTo>` with the main element's EID (`twrite.cpp:513-524`).
- *Stability.*

  | Event | Same object? | EID |
  |---|---|---|
  | Undo/redo of add/remove | yes: `AddElement`/`RemoveElement` keep the pointer (`src/engraving/editing/addremoveelement.cpp:90-96`, `273-279`) | unchanged |
  | Edit that replaces an element (re-created chords, clones, linked copies) | no | new object has none until assigned |
  | Save in the same session | yes | items lacking one get one at save (`twrite.cpp:501-504`) |
  | Close and reopen file | new objects | same values for items that were written |
  | Layout | systems, pages, spanner segments, MM-rest measures are rebuilt | never give these EIDs |

- *Freed-object hazard (confirmed, and wider than the assessment says).*
  (a) `~EngravingObject` never unregisters (`engravingobject.cpp:103-143`); `removeItem` exists
  only for read-time fixups and the writer's own temporaries
  (`eidregister.cpp:59-79`, comment at `:61`; `src/engraving/rw/read460/readcontext.cpp:335-336`;
  `twrite.cpp:3417`, `3600`). Elements are freed when a redo branch is discarded at the next
  commit (`undo.cpp:426-432` → `AddElement::cleanup(false)` deletes,
  `addremoveelement.cpp:90-96`), when stacks are trimmed (`undo.cpp:355-369`), by direct deletes
  (`src/engraving/editing/edit.cpp:4655-4658`, `cmd.cpp:539-554`), and by every layout for
  layout objects. Their register entries remain.
  (b) **Address reuse.** Because the key is the pointer, a new object allocated at a freed
  address reports the dead object's EID through `eid()` (`eidregister.cpp:90-94`), and calling
  `assignNewEID()` on it trips `assert(inserted)` on the pointer map while still inserting the new
  EID (`eidregister.cpp:44-48`), leaving two EIDs pointing at it. A check "the object found at the
  handle's location is the one the register maps the EID to" (assessment, handles paragraph)
  **passes** for a different element in this case.
  (c) `itemFromEID` on an unknown EID fails `IF_ASSERT_FAILED` (`eidregister.cpp:83-86`), which
  calls `assert()` (`src/framework/global/thirdparty/kors_logger/src/log_base.h:74-80`): an abort
  in the Debug/ASan tree planned for P3.4, a logged error in RelWithDebInfo.
  (d) `EID::fromStdString` does no character or length validation: a non-base64 character hits
  `UNREACHABLE` (`eid.cpp:117-118`, an assert), more than 11 characters silently overflow
  (`eid.cpp:123-133`).
- *What the code supports for handles.* Resolve by walking to the structural location and
  comparing `item->eid()` (no assert path) with the handle's EID; never call `itemFromEID` with
  client input; validate the string as `[A-Za-z0-9+/]{1,11}_[A-Za-z0-9+/]{1,11}` before
  `fromStdString`; and because of (b) also compare element type and an identity fingerprint
  (for a note: tick, track, pitch, tpc1) captured when the handle was issued. Only assign EIDs to
  structural, non-generated items (`EngravingItem::generated()`), never to layout objects.
  Assigning an EID is not an undo command and sends no notification; it changes the next save's
  bytes only by adding `<eid>` tags the save would add anyway (`twrite.cpp:501-504`).

**R5. Three measure numberings exist; only one is structural.**
- Ordinal among `Measure`s (boxes excluded): `MeasureBase::measureIndex()`
  (`src/engraving/dom/measurebase.cpp:661-679`), `Score::crMeasure(idx)` (`score.cpp:1805-1817`).
  Both are O(n) walks; calling them per measure makes a read O(n²).
- Displayed number `MeasureBase::no()+1`, assigned by layout with `noOffset`, irregular
  (excluded) measures and section restarts (`measurelayout.cpp:1189-1203`). Stale mid-transaction.
- Time-signature-map bar from `TimeSigMap::tickValues` (`src/engraving/dom/sig.cpp:320-350`),
  whose base is `SigEvent::bar()`, filled from `m->no()` when the map is rebuilt
  (`score.cpp:646`); so it inherits layout numbering.
The UI's status/accessibility text shows "Measure: <sigmap bar>; Displayed measure: <no()+1>;
Beat: <b>" (`EngravingItem::formatBarsAndBeats`, `src/engraving/dom/engravingitem.cpp:2617-2632`,
`barbeat()` at `:2484-2523`). Beat there is counted in the time signature's denominator unit
(`engravingitem.cpp:2503-2508`). Which numbering the agent's `{measure, beat}` uses is the
conductor's choice (P1.3); the facts above constrain it.

**R6. The structural walk (no layout needed).** Upstream walkers that do this: the Timeline
(`src/notationscene/widgets/timeline.cpp:1085-1137`: `firstMeasure()`/`nextMeasure()`, every
segment, `Measure::el()` for jumps/markers), the MusicXML exporter, and the converter's
`ScoreElementScanner` (`src/converter/internal/compat/scoreelementsscanner.cpp:130-254`). The
latter uses `Score::scanElements`, which also visits MM-rest measures and layout systems/pages
(`score.cpp:5883-5900`); do not use `scanElements` for a structural read.
- Measures: `Score::firstMeasure()` / `MeasureBase::nextMeasure()` (`score.h:855`,
  `measurebase.h:101`); boxes (VBox/HBox/TBox/FBox) appear only in the `MeasureBase` chain
  `Score::first()`/`next()`. Use the non-MM chain; `nextMeasureMM()` (`measurebase.h:103`) is
  layout's. Count: `Score::nmeasures()` (`score.cpp:4885-4892`).
- Measure facts: `ticks()` actual length (`measurebase.h:128`), `timesig()` nominal
  (`measure.h:199`), `repeatStart()/repeatEnd()` (`measurebase.h:149-152`), `repeatCount()`
  (`measure.h:255`), `el()` for layout breaks, jumps, markers, spacers (`measurebase.h:106-107`),
  `isEmpty(staffIdx)` (rests-only test incl. cross-staff and annotations, `measure.cpp:2448-2490`),
  measure repeats `isMeasureRepeatGroup` / `measureRepeatElement` (`measure.h:327-331`).
  Local time signatures: chord/rest ticks are staff-local; global = ticks ×
  `Staff::timeStretch(tick)` (`src/engraving/dom/staff.h:111`; used that way in
  `src/engraving/dom/check.cpp:85-100`).
- Segments: `Measure::first(SegmentType)` (`measure.h:207`), `Segment::next(SegmentType)`
  (`src/engraving/dom/segment.h:138`), `next1(SegmentType)` across measures (`segment.h:156`).
  `Segment::element(track)` (`segment.h:174`), track = staffIdx × 4 + voice
  (`score.h:462`). `Segment::annotations()` (`segment.h:226`) holds dynamics, chord symbols,
  staff/system/tempo/rehearsal text, fermatas and the like, each with its own track.
  Clef/KeySig/TimeSig/Breath/BarLine are their own segment types.
- Chords and rests: `Chord::notes()` (`chord.h:178`), `graceNotes()` (`chord.h:230`),
  `articulations()` (`chord.h:297-298`; ornaments are articulations), `ChordRest::lyrics()`
  (`chordrest.h:126`), `actualDurationType()` and `actualDots()` (the stored values,
  `chordrest.h:104`, `116`; prefer these to `durationType()`, R2), `ticks()`
  (`durationelement.h:62`), `tuplet()` (`durationelement.h:52`).
- Tuplets: `Tuplet::ratio()`, `elements()`, `baseLen()` (`src/engraving/dom/tuplet.h:91`, `97`,
  `111`); nesting via the tuplet's own `tuplet()`.
- Notes: `pitch()`, `tpc1()`, `tpc2()`, `string()`, `fret()` (`note.h:198-255`; concert vs
  written per notes-G9), `tieFor()`/`tieBack()` (`note.h:280-281`).
- Ties live on notes, not in the spanner map: `Score::addElement` adds slurs, hairpins, voltas,
  ottavas, pedals, trills, text lines, etc. to `m_spanner` (`score.cpp:1413-1452`) but a TIE goes
  to `Note::add` (`src/engraving/dom/note.cpp:1350-1358`; partial ties and laissez vibrer
  `note.cpp:1335-1349`).
- Spanners: `Score::spanner()` (multimap start tick → `Spanner*`, `score.h:960`),
  `spannerMap().findOverlapping(start, end)` (used at `score.cpp:6020`).
- Lyrics: `Lyrics::verse()`, `syllabic()`, `ticks()` (melisma length) (`src/engraving/dom/lyrics.h:64`,
  `67`, `75`).
- Parts/staves: `Score::parts()` (`score.h:630`), `Part::partName()`, `longName(tick)`,
  `instrument(tick)`, `instruments()`, `staves()` (`part.h:161`, `95`, `135-136`, `145`, `81`),
  `Instrument::transpose()` (`instrument.h:319`), `Staff::clef(tick)`, `key(tick)`,
  `concertKey(tick)`, `keyList()`, `transpose(tick)` (`staff.h:99`, `121-123`, `118`).
  `INotationParts::partList()` / `staffList()` are the UI's listing
  (`src/notation/inotationparts.h:40-41`).
- Score-wide maps: `Score::sigmap()`, `tempomap()` (`score.h:792-793`), `keyList()` (`score.h:827`).
- Excerpts: `MasterScore::excerpts()` (`masterscore.h:121-122`), `Excerpt::excerptScore()`,
  `name()`, `tracksMapping()` (`src/engraving/dom/excerpt.h:57`, `60`, `83`); master↔part items via
  `EngravingItem::findLinkedInScore()` (`engravingitem.h:500`), staves via
  `Staff::findLinkedInScore()` (`staff.h:254`).

**R7. Read-adjacent calls that mutate.** Every one of these is reachable from an obvious
"read" implementation:
- `MasterScore::sanityCheck` writes corruption flags and **silently rewrites a bad full-measure
  rest's ticks** (`check.cpp:152`, `162`, `203-206`). See score_validate.
- `SvgWriter::write` sets `score->setPrinting(true)` and the global `MScore::pdfPrinting`/
  `svgPrinting` and returns early on a bad page number **without resetting them**
  (`src/importexport/imagesexport/internal/svgwriter.cpp:65-75` vs reset at `:271-273`); the
  `BEATS_COLORS` option calls `setColor` on notes and rests (`svgwriter.cpp:201-228`).
- `PositionsWriter` (`spos`/`mpos`) calls `masterScore()->setExpandRepeats(true)`
  (`src/notation/internal/positionswriter.cpp:247`), which flips the play-repeats state
  (`src/engraving/dom/masterscore.cpp:168-175`).
- The `mscx` writer deletes the target file's parent directory recursively (score_export).
- `MscNotationWriter` and save call `Score::createThumbnail`, which switches the score to page
  mode and back with two full layouts (`src/engraving/dom/scorefile.cpp:74-103`).
- `NotationPainting::setViewMode` lays out and notifies the view (score_render).
- `MEI`/`MNX` writers assign EIDs to everything they export (R4).

**R8. Output depends on the user's preferences.** PNG: DPI, trim margin, transparency, grayscale
(`src/importexport/imagesexport/internal/pngwriter.cpp:48`, `53`, `67`, `74`); SVG: trim margin,
Illustrator compatibility, transparency (`svgwriter.cpp:87-109`); PDF: DPI, transparency,
grayscale (`src/importexport/imagesexport/internal/pdfwriter.cpp:69`, `152-158`); MusicXML:
`exportLayout` (default true), break type (default All), invisible elements (default false),
MU3 compatibility (`src/importexport/musicxml/internal/musicxmlconfiguration.cpp:55-64`, used at
`src/importexport/musicxml/internal/export/exportmusicxml.cpp:7494-7523`, `9402`); MIDI:
expand repeats, RPNs (`src/importexport/midi/internal/notationmidiwriter.cpp:62-63`). The
setters for these write the user's settings, so the module must not change them; for
deterministic output it has to pass explicit options or bypass the writer (score_render).

**R9. Writers registry.** `INotationWritersRegister::writer(suffix)`
(`src/project/inotationwritersregister.h:37`, map lookup in
`src/project/internal/notationwritersregister.cpp`). It is a `MODULE_CONTEXT_INTERFACE`
(`inotationwritersregister.h:29`); the converter's own injection has a null context and a
`FIXME` (`src/converter/internal/compat/backendapi.h:50`), so the agent module must resolve it
through its own module context.

**R10. Change notifications.** `MasterScore::changesChannel()` (`masterscore.h:103`; every
`Score`, master or part, forwards to the master's, `score.cpp:6297`) receives one `ScoreChanges`
per non-empty committed command (`cmd.cpp:428-446`) and per single undo or redo step
(`cmd.cpp:372-398`). `INotationUndoStack::changesChannel()` returns the same channel
(`src/notation/internal/notationundostack.cpp:250-257`). Delivery to receivers on the sending
thread is synchronous (`sendAuto` → `receiversCall`,
`src/framework/global/thirdparty/kors_async/async/internal/channelimpl.h:479-486`).
`INotation::notationChanged()` is a payload-less repaint notification (`src/notation/inotation.h:115`)
and is not reliably sent on every edit (for example `NotationUndoStack::commitChanges` sends only
`stackChanged`, `notationundostack.cpp:132-145`). Details in score_changes.

---

### score_overview  ·  command://notation/score_overview

**UI entry point.** No single UI function produces an overview. The UI pieces it mirrors:
Instruments panel → `INotationParts::partList()/staffList()` (`src/notation/inotationparts.h:40-41`);
Timeline → structural walk for tempo, time, key, rehearsal, barlines, jumps/markers
(`timeline.cpp:1085-1137`, meta builders `timeline.cpp:1159-1479`); status bar →
`EngravingItem::formatBarsAndBeats` (`engravingitem.cpp:2617-2632`) and view mode
(`src/appshell/qml/MuseScore/AppShell/notationstatusbarmodel.cpp:274-283`); title bar →
`INotationProject::needSave()` (`inotationproject.h:65`).

**Selection set-up.** None. The overview reports the user's selection read-only:
`Score::selection()` → `Selection::isNone/isRange/isList`, `staffStart/staffEnd`, `tickStart/
tickEnd`, `elements()` (`src/engraving/dom/select.h:102-120`, `153`), or
`INotationInteraction::selection()` (`inotationinteraction.h:84`). Selection holds raw pointers;
convert to structural positions at once.

**Arguments.** Proposed:
```json
{"type":"object","properties":{
  "score":{"enum":["master","current"],"default":"master"},
  "include":{"type":"array","items":{"enum":["parts","maps","marks","occupancy","selection","layout","state"]}}
},"additionalProperties":false}
```
Content, each with its source (all structural unless marked):
- parts → `Score::parts()`, names, instrument at tick 0 and instrument changes
  (`Part::instruments()`), staves per part, clef at tick 0 and clef changes (Clef segments),
  `Instrument::transpose()`, part `muse::ID` (`part.h:207`).
- maps → time signature changes (`TimeSig` segments, or `sigmap()`), key changes per staff
  (`KeySig` segments, `Staff::keyList()`), tempo (`TempoText` annotations; `tempomap()` for
  effective BPM).
- marks → rehearsal marks (segment annotations, `src/notation/internal/notationelements.cpp:126-146`
  is the UI's own search), section breaks (`LayoutBreak` in `MeasureBase::el()`), repeat
  barlines (`repeatStart/End`), voltas (spanners), jumps and markers (`Measure::el()`, Timeline
  `timeline.cpp:1122-1137`).
- occupancy → per staff, `!Measure::isEmpty(staffIdx)` (`measure.cpp:2448`), measure count
  `nmeasures()`.
- layout (layout-derived) → view mode, page count `Score::npages()` (`score.h:842`), displayed
  number of each measure if it differs from the ordinal (R5), MM-rest spans.
- state → file path, `needSave()`, `isNewlyCreated()`, `readOnly()` (`masterscore.h:98`),
  `mscVersion()` (`score.h:780`), corrupted-on-load (`EngravingProject::isCorruptedUponLoading()`,
  `src/engraving/engravingproject.cpp:163-166`, reachable as `masterScore()->project().lock()`,
  pattern at `src/notation/internal/mscnotationwriter.cpp:88`), which notation is current
  (`INotation::isMaster()`, `name()`), the R3 interaction flags, playback state
  (`IGlobalContext::playbackState()`, `iglobalcontext.h:51`), excerpts list with `isInited()`.
- `stateVersion` (score_changes).

**Validation before mutation.** No mutation. Checks:
- No current project / master notation → E_NO_SCORE.
- `score:"current"` resolves to a part that is not initialised → E_PART_NOT_FOUND.
- Macro open or stack locked (R2) → return data with `committed:false` (proposal) or
  E_BUSY; this is the conductor's policy call.

**Post-condition.** R2 holds (no active macro, not locked). Layout-derived fields carry the
view mode they were computed in. Response built in one synchronous handler on the GUI thread,
so no edit can interleave.

**Nested commands.** None. No `startCmd`/`endCmd`, no `update()`. Safe to call while the stack is
locked only in the sense that it mutates nothing; values would be uncommitted.

**Linked parts / excerpts.** Report from the master. Parts list from `IMasterNotation::excerpts()`;
part staff ↔ master staff via `Staff::findLinkedInScore` (`staff.h:254`) or
`Excerpt::tracksMapping()` (`excerpt.h:83`). Do not call `initExcerpts` on potential excerpts
(it creates part scores, `src/notation/internal/masternotation.cpp:466-476`).

**Crash and corruption risks.**
- Calling `isEmpty(muse::nidx)` is safe only because `measureRepeatCount` bounds-checks
  (`measure.cpp:3056-3060`); keep to real staff indices.
- Per-measure `measureIndex()`/`crMeasure()` → O(n²) on long scores (R5); count while walking.
- Selection pointers may refer to elements in a part score, not the master (GG7).

**Existing upstream tests.** No direct test. Walk APIs are exercised by every engraving test that
reads a fixture (`src/engraving/tests/utils/scorerw.h:45`); `all_elements_data/` has broad
fixtures (`random_elements.mscx`, `goldberg.mscx`, `moonlight.mscx`). The converter's scanner
has `src/converter/tests/scoreelementsscanner_tests.cpp`.

**Scenarios.**
- READ-OV-01: brass quintet fixture; parts, transpositions, clefs, keys, tempi, rehearsal
  marks equal a hand-written expectation.
- READ-OV-02: score with pickup and a mid-score `noOffset` measure; ordinal vs displayed numbers
  both reported correctly.
- READ-OV-03: score in continuous view; structural fields identical to page view.
- READ-OV-04: user in text-edit mode; overview reports the flag and does not disturb the edit.
- READ-OV-05: score with a part never opened; overview lists it without opening or laying it out.

**Open questions.**
1. Policy when a macro is open (R2): refuse with E_BUSY, or return with `committed:false`?
2. Which measure numbering is the public address (R5)? Facts favour the ordinal for addressing,
   with the displayed number as an extra field.

---

### score_read  ·  command://notation/score_read

**UI entry point.** None: the UI never serialises a range. The model paths are R6. The closest
upstream serialisers are MusicXML export (`exportmusicxml.cpp`, `saveXml` at `:8957`) and
the converter scanner (`scoreelementsscanner.cpp:237-254`). Accessibility text
(`formatBarsAndBeats`, `engravingitem.cpp:2617`) shows how the UI names positions.

**Selection set-up.** None. Never select to read (selection changes notify the view, GG7).

**Arguments.** Proposed (addressing per R5/P1.3, not designed here):
```json
{"type":"object","required":["from","to"],"properties":{
  "from":{"$ref":"#/defs/measureRef"},"to":{"$ref":"#/defs/measureRef"},
  "staves":{"type":"array","items":{"type":"object","properties":{
     "part":{"type":"string"},"staff":{"type":"integer","minimum":1}}}},
  "voices":{"type":"array","items":{"type":"integer","minimum":1,"maximum":4}},
  "score":{"enum":["master","current"],"default":"master"},
  "detail":{"enum":["notation","json"],"default":"notation"},
  "handles":{"type":"boolean","default":false}
},"additionalProperties":false}
```

**How to walk a range** (all structural, R6):
1. Resolve `from`/`to` to `Measure*` by one walk of `firstMeasure()/nextMeasure()`.
2. For each measure, each requested staff, each voice track: iterate ChordRest segments and take
   `element(track)`; record gaps (missing CRs inside a measure are legal in voices 2–4, and
   voice 1 gaps are what `sanityCheck` calls "Incomplete measure", `check.cpp:196-201`).
3. For each ChordRest: stored duration (`actualDurationType/actualDots`), `ticks()`,
   staff-local vs global time (R6), tuplet chain, grace chords before/after, notes with pitch,
   tpc1/tpc2, ties, articulations, lyrics (verse, syllabic, melisma ticks).
4. Annotations of the segment whose track lies in the requested staves (and system-level items
   such as tempo/rehearsal, which sit on the top staff track).
5. Spanners overlapping `[from.tick, to.endTick)` from `spannerMap().findOverlapping`, with
   start/end tick and track; slurs and hairpins that start before or end after the range must be
   marked as open-ended.
6. Measure-level items from `Measure::el()` and non-CR segments (clef, key, time, barline,
   breath).
7. Handles (if asked): EID per R4 (assign only to structural, non-generated items), plus the
   structural location and fingerprint.
8. JSON detail: properties via `getProperty`, using an explicit per-type Pid list; parent bubbling
   makes `isValid()` meaningless (GG6).

**Validation before mutation.** No mutation (except optional EID assignment, R4):
- E_NO_SCORE; E_RANGE if `from` > `to` or outside 1..nmeasures; E_PART_NOT_FOUND for an
  unknown part name or staff index; E_RANGE for a voice outside 1–4.
- E_BUSY or `committed:false` while a macro is open (R2).
- Size cap (proposal): refuse ranges whose output would exceed the response budget, with the
  measure count that fits.

**Post-condition.** R2. Structural data needs no layout; a read issued right after an agent commit
sees the committed tree (the commit's `endCmd` has returned). Layout-derived extras (displayed
number, system/page) are valid only for open scores (R2).

**Nested commands.** None. Safe under lock only as "no mutation"; mid-transaction data is
uncommitted and unlaid-out (notes-G8), which matters for the write commands that return the
bars they touched: they must read after `commitChanges`.

**Linked parts / excerpts.** Default to the master (notes-G7). Reading a part: walk the part's own
`Score` (its staff indices differ, `Excerpt::tracksMapping`). Handles must record which score they
belong to (GG5). Part items unlinked from master for position/appearance are reported per part.

**Crash and corruption risks.**
- Freed-object and address-reuse hazards when re-resolving handles (R4 b–d).
- Pointers from `ScoreChanges` or the selection must not be dereferenced in a later request.
- `scanElements` mixes layout objects into the walk (`score.cpp:5895-5899`); MM-rest measures
  carry copies of content (`score.cpp:5888-5892`), which would double-count.
- Corrupt scores (gaps, overfull voices, `check.cpp:196-217`) must not crash the walker: never
  assume voice-1 ticks sum to the measure length.

**Existing upstream tests.** `src/engraving/tests/eid_tests.cpp:36-56` (every registered EID maps
back to its item after reading `random_elements.mscx`); `tuplet_tests.cpp`, `lyrics_tests.cpp`,
`partialtie_tests.cpp`, `spanners_tests.cpp`, `links_tests.cpp`, `parts_tests.cpp` for fixtures with
tuplets, melismas, partial ties, spanners and linked parts (all under `src/engraving/tests/`).

**Scenarios.**
- READ-RD-01: every fixture in P1.5; read whole score, parse back (P1.4), pitch and rhythm equal
  (gate G1).
- READ-RD-02: nested tuplets (3:2 in 5:4) round-trip.
- READ-RD-03: tie across barline and partial ties into voltas reported on both notes.
- READ-RD-04: slur and hairpin starting before the range are marked open-ended.
- READ-RD-05: local time signature staff (3/4 against 6/8) reports staff-local ticks correctly.
- READ-RD-06: handles: read, undo the edit that created a note, make a new edit, re-use the
  handle → E_STALE_HANDLE, no crash (also under ASan, P3.4).
- READ-RD-07: cross-measure-values style on; reported durations are the stored ones.
- READ-RD-08: 30-page score whole read under 2 s (gate G1).

**Open questions.**
1. Should a read assign EIDs (making later saves carry them, harmless per R4) or should handles
   for EID-less items be structural only until the next save?
2. Fingerprint contents per element type for the address-reuse check (R4 b) — needs the
   conductor's handle design.

---

### score_export  ·  command://notation/score_export

**UI entry point.** File > Export (`"file-export"`, `src/project/internal/projectactionscontroller.cpp:92`)
→ `ExportDialogModel::exportScores` (`src/project/qml/MuseScore/Project/internal/Export/exportdialogmodel.cpp:418-477`),
which asks for a path with a file dialog (`ExportProjectScenario::askExportPath`, `eps.cpp:51-98`,
`selectSavingFileSync` at `:93`) and then calls `ExportProjectScenario::exportScores`
(`eps.cpp:100-247`) on the next event loop (`exportdialogmodel.cpp:471-475`). `exportScores`:
1. Looks up the writer by suffix (`eps.cpp:103-108`).
2. Initialises potential (never-created) parts that were selected (`eps.cpp:118-131`).
3. Lays out closed scores (`eps.cpp:133-139`).
4. Switches every exported notation to page view and restores it afterwards
   (`eps.cpp:142-144`, `163-166`).
5. Shows a progress dialog if the writer has progress (`eps.cpp:146-160`, `449-454`).
6. Writes per page / per part / multi-part through `doExportLoop` (`eps.cpp:182-238`), which opens
   a `FileStream`, calls the writer, and on conflict or error opens `questionSync` dialogs
   (`eps.cpp:346-384`, `386-394`, `396-447`).
7. Optionally reveals the folder in Finder (`eps.cpp:241-244`, `456-462`).
The plugin API's `writeScore` goes through the same `exportScores`
(`src/project/internal/engravingpluginapihelper.cpp:34-50`), so it can open the conflict dialog and
has no path sandbox. `NotationProject::exportProject` (`np.cpp:975-993`) is a dialog-free
`File` + writer call used by the converter.

What the agent needs is steps 1, 3, 4 and a writer call into a `Buffer` or a file it owns,
without 2, 5, 6's dialogs and 7. The converter does exactly the buffer part:
`BackendApi::processWriter` (`bapi.cpp:480-505`) and `exportScorePngs` (`bapi.cpp:287-326`) write
into `muse::io::Buffer`. But the converter always works on a **fresh project loaded from disk**
(`BackendApi::openProject`, `bapi.cpp:224-253`), switches that private copy to page view
(`bapi.cpp:775-782`) and lays out all its parts (`bapi.cpp:784-794`); it never touches an open
score.

**Formats** (writer, unit types, buffer-safe, side effects):

| Suffix | Writer (registration) | Units | Into a Buffer, no dialogs? | Notes |
|---|---|---|---|---|
| `musicxml`, `xml` | `MusicXmlWriter` (`src/importexport/musicxml/musicxmlmodule.cpp:64`; `musicxmlwriter.cpp:46-58`) | PER_PART | yes | needs page layout for `new-system`/`new-page` with defaults (R8); invisible elements dropped by default |
| `mxl` | `MxlWriter` (`musicxmlmodule.cpp:65`) | PER_PART | yes | compressed MusicXML |
| `mid`, `midi`, `kar` | `NotationMidiWriter` (`src/importexport/midi/midimodule.cpp:60`; `notationmidiwriter.cpp:48-80`) | PER_PART | yes | repeats/RPNs from prefs (R8) |
| `pdf` | `PdfWriter` (`src/importexport/imagesexport/imagesexportmodule.cpp:55`) | PER_PART (default), MULTI_PART via `writeList` | yes | `write` asserts unit type PER_PART (`pdfwriter.cpp:47-50`); page size from first page, so page view required |
| `png` | `PngWriter` (`imagesexportmodule.cpp:57`) | PER_PAGE | yes | no page-index check: a bad index gives a 0×0 image (`paint.cpp:203-206`) |
| `svg` | `SvgWriter` (`imagesexportmodule.cpp:56`) | PER_PAGE | yes | bad page index leaks printing flags; `BEATS_COLORS` recolours notes (R7) |
| `mscz` | `MscNotationWriter(Zip)` (`src/notation/notationmodule.cpp:63`; `mscnotationwriter.cpp:55-98`) | MULTI_PART | yes | writes the engraving project only; thumbnail on (`mscnotationwriter.cpp:88`) |
| `mscx` | `MscNotationWriter(Dir)` (`notationmodule.cpp:64`) | MULTI_PART | **no** | see risks: needs `file_path` meta and wipes the parent folder |
| `mnx`, `mei` | `NotationMnxWriter` (`src/importexport/mnx/mnxmodule.cpp:55`), `MeiWriter` (`src/importexport/mei/meimodule.cpp:59`) | — | yes | assign EIDs to every exported item (R4) |
| `spos`, `mpos` | `PositionsWriter` (`notationmodule.cpp:61-62`) | PER_PART | yes | flips play-repeats (R7); exclude |
| `wav` `mp3` `ogg` `flac` `aac`, `mp4` | audio/video writers (`src/importexport/audioexport/audioexportmodule.cpp:58-62`, `src/importexport/videoexport/videoexportmodule.cpp:55`) | — | not checked | progress/abort machinery (`inotationwriter.h:52`, `69-70`); out of scope for v1 |
| `brf`, `lrc` | Braille, LRC (`src/braille/braillemodule.cpp:50`, `src/importexport/lyricsexport/lyricsexportmodule.cpp:51`) | — | not checked | — |

No writer in the table above (except audio/video, not examined) calls `IInteractive` or a nested
event loop: grep of the MusicXML, MIDI, image, MEI, MNX, braille writers and
`mscnotationwriter.cpp` finds `interactive()` only in `meireader.cpp:79-89`, a reader.

**Lossless snapshot options for `.mscx` text.** The score XML can be produced without touching
the file system the way `MscSaver::writeMscz` does it: `RWRegister::writer(score->iocContext())->writeScore(score, &buffer, &writeOutData)`
(`src/engraving/rw/mscsaver.cpp:69-80`). Alternatively write `.mscz` into a `Buffer` with
`MscWriter{Zip, device}` (as `bapi.cpp:742-773` does for parts) and read `score.mscx` back with
`MscReader::readScoreFile` (pattern at `np.cpp:946-960`). `NotationProject::writeToDevice`
(`np.cpp:603-641`) produces the same container as Save (`writeProject`, `np.cpp:857-902`:
engraving project + audio settings + view settings + per-part view/solo-mute data) but without a
thumbnail; `MscNotationWriter` omits audio and view settings.

**Selection set-up.** None.

**Arguments.** Proposed:
```json
{"type":"object","required":["format"],"properties":{
  "format":{"enum":["musicxml","mxl","midi","pdf","mscz","mscx"]},
  "score":{"enum":["master","current"],"default":"master"},
  "part":{"type":"string","description":"excerpt name; omit for master"},
  "delivery":{"enum":["file","inline"],"default":"file"}
},"additionalProperties":false}
```
Files go only to the agent temp folder (CLAUDE.md rule 4); the module chooses the name.

**Validation before mutation.**
- E_NO_SCORE; E_PART_NOT_FOUND (unknown or potential part; do not init it, open question 2).
- E_UNSUPPORTED_FORMAT if `writers()->writer(suffix)` is null (R9) or the format is excluded.
- E_BUSY while a macro is open (R2) or the user is mid-edit (R3), because the export switches
  layout mode (step 4) and lays out.
- Destination path must be inside the agent temp folder after symlink resolution → E_PATH.

**Post-condition.** Writer returned `Ret` ok, buffer/file non-empty, and for `mscz` the container
reopens with a non-empty main file (the save path's own check, `np.cpp:938-973`). Output reflects
the committed tree (R2) laid out in page mode.

**Nested commands.** None. No writer calls `startCmd`/`endCmd`. Layout calls (`doLayout`,
`setViewMode`, `createThumbnail`) run outside the undo system and do not reset `cmdState`
(layout locks it, `src/engraving/rendering/score/scorelayout.cpp:41-48`, `54`).

**Linked parts / excerpts.** Each part is its own `INotation`; export it via
`IExcerptNotation::notation()`. A part whose tab was never opened must be laid out first
(`eps.cpp:133-139`). `mscz`/`mscx` always contain all initialised parts (`mscsaver.cpp:83-115`).

**Crash and corruption risks.**
- **`mscx` writer deletes a directory.** For `.mscx` the container path is the file's parent
  directory (`src/engraving/infrastructure/mscio.h:75-78`); `DirWriter::open` calls
  `Dir(m_rootPath).removeRecursively()` on it (`src/engraving/infrastructure/mscwriter.cpp:322-343`),
  after `MscNotationWriter` removes the file itself (`mscnotationwriter.cpp:69-77`). Pointed at
  `~/Desktop/x.mscx` it would empty the Desktop. Never use this writer; use the XML or zip route
  above.
- `SvgWriter` early return leaves `score->printing()` and global print flags set (R7): validate the
  page index first.
- `PngWriter`/`SvgWriter`/`PdfWriter` read user prefs (R8); passing `TRANSPARENT_BACKGROUND`
  explicitly is supported (`pngwriter.cpp:66-67`), DPI is not.
- `MusicXML` in continuous view would differ from the UI export (breaks come from layout,
  `exportmusicxml.cpp:7494-7509`).
- View-mode switching side effects (score_render R-options).
- `MscNotationWriter` with `createThumbnail=true` → `Score::createThumbnail` indexes
  `pages().at(0)` after switching to page mode (`scorefile.cpp:79-81`); guarded upstream only by
  `!score->pages().empty()` (`mscsaver.cpp:146`).
- Determinism for goldens: EIDs are random outside test mode (R4); MusicXML depends on prefs (R8).

**Existing upstream tests.** `src/importexport/musicxml/tests/musicxml_tests.cpp` (export goldens),
`src/importexport/midi/tests/midiexport_tests.cpp`, `src/importexport/mei/tests/mei_tests.cpp`,
`src/importexport/lyricsexport/tests/lrc_tests.cpp`, `src/engraving/tests/msczfile_tests.cpp`,
`src/engraving/tests/readwriteundoreset_tests.cpp:38-71` (write/read/undo round trips),
`src/engraving/tests/utils/scorecomp.h:33-35` (file comparison). No test covers
`ExportProjectScenario` or the image writers (`src/project/tests/` has none; no
`imagesexport/tests`).

**Scenarios.**
- READ-EX-01: `mscz` export of an edited, unsaved score; `score.mscx` inside equals a UI save's
  (gate G1), compared after stripping nothing (same session register).
- READ-EX-02: MusicXML export in continuous view equals one made in page view.
- READ-EX-03: export of a never-opened part lays it out and leaves it closed.
- READ-EX-04: `format:"mscx"` writes only the agent temp file; a sentinel file next to it survives.
- READ-EX-05: user's view mode, scroll position and selection identical before and after.
- READ-EX-06: unknown format → E_UNSUPPORTED_FORMAT; nothing written.

**Open questions.**
1. Accept `.mscx` only as text produced by `writeScore` into a buffer (recommended by the facts
   above), or also as a full unpacked folder in a dedicated empty temp subfolder?
2. Should exporting a potential (not yet created) part be refused, or created as the UI does
   (`eps.cpp:118-131`, non-undoable `initExcerpt`, `masternotation.cpp:466-476`)?
3. Fixed export options (DPI, MusicXML layout/breaks) cannot be passed to the writers without
   changing user settings (R8). Accept user-dependent output, or reimplement the PNG/PDF paths
   on `INotationPainting` (score_render) and pin MusicXML prefs only in the test harness?
4. Audio export: not examined (progress/abort, `WAIT_FOR_COMPLETION`, `inotationwriter.h:52`).

---

### score_render  ·  command://notation/score_render

**UI entry point.** The UI has no "render a bar range" feature. Page images come from the export
path: `PngWriter::write` (`pngwriter.cpp:42-88`) → `INotationPainting::paintPng`
(`npaint.cpp:218-226`) → `NotationPainting::doPaint` (`npaint.cpp:113-135`) →
`IScoreRenderer::paintScore` → `Paint::paintScore` (`paint.cpp:38-178`). SVG uses its own loop
over `page->elements()` (`svgwriter.cpp:52-275`). The UI's only canvas rectangle for a range is
the selection's: `NotationSelection::canvasBoundingRect()`
(`src/notation/internal/notationselection.cpp:114-131`) → `ScoreRangeUtilities::boundingArea`
(`src/notation/utilities/scorerangeutilities.cpp:34-89`), which takes segments and staff indices
directly, so **no selection is needed** to get a range's rectangles.

**What the renderer offers.**
- `ScorePaintOptions` (`src/engraving/rendering/iscorerenderer.h:110-124`): `fromPage/toPage`,
  `frameRect` (only items intersecting it are painted, and pages left/right of it are skipped,
  `paint.cpp:93-105`, `148`), `trimMarginPixelSize` (crop to the page's tight bbox,
  `paint.cpp:89-92`), `deviceDpi`, `printPageBackground`.
- `paintPng` sets `isPrinting=true`, which draws without selection colours
  (`src/engraving/dom/engravingitem.cpp:665-668`), without the interaction overlay
  (`npaint.cpp:129-134`) and without debug marks (`paint.cpp:155-157`).
- Page geometry: `pageSizeInch(opt)` (`paint.cpp:196-215`), `Page::pos()`, `Page::tbbox()`.
- Range geometry: `boundingArea` returns one canvas rectangle per system the range crosses,
  already offset by `page->pos()` (`scorerangeutilities.cpp:84`). Each rectangle spans the staff
  lines of the first..last visible staff plus 0.5 sp (`scorerangeutilities.cpp:58-80`) — **not**
  dynamics, lyrics, chord symbols or slurs above/below. It walks with `next1MMenabled`
  (`scorerangeutilities.cpp:102-105`), so start/end segments must come from the MM-aware chain
  when MM rests are shown (`Score::tick2segmentMM`, used at `src/engraving/editing/edit.cpp:4861`),
  and returns `{}` if `endSegment` is null (`scorerangeutilities.cpp:38-40`).
- Measure geometry precedent: `PositionsWriter::writeMeasuresPositions`
  (`positionswriter.cpp:221-239`): `measure->pagePos()`, `measure->ldata()->bbox()`,
  `measure->system()->pagePos()`, `system()->height()`, `system()->page()`, over the MM chain.

**Rendering without changing the user's view.** Page renders need page layout. Three ways exist
in the code; the choice is the conductor's:
- *A. Notation-level switch* (what Export does): `INotationPainting::setViewMode(PAGE)` → restore.
  `setViewMode` calls `Score::setLayoutMode` + `doLayout`, then notifies `viewModeChanged` and
  `notationChanged` (`npaint.cpp:48-63`). Listeners run synchronously: the paint view clamps its
  viewport (`src/notationscene/qml/MuseScore/NotationScene/abstractnotationpaintview.cpp:332-334`,
  `1047-1070`), the Inspector reloads (`src/inspector/qml/MuseScore/Inspector/inspectorlistmodel.cpp:383-387`),
  `NotationInteraction::onViewModeChanged` deselects VBox content when entering LINE mode
  (`ni.cpp:268-270`, `291-312`). The clamp can move the user's scroll position.
- *B. Score-level switch* (what every save's thumbnail does): `Score::switchToPageMode()`
  (`score.cpp:3422-3428`), render, `setLayoutMode(old)` + `doLayout()` (`scorefile.cpp:74-103`).
  No notifications; if restored before returning to the event loop the view never sees it.
  Cost: two full layouts when the user is not in page view, none when they are.
- *C. Private copy* (what the converter does): serialise the in-memory score, load it into a new
  `INotationProject` from `IProjectCreator::newProject` (`src/project/iprojectcreator.h:38`;
  `bapi.cpp:224-253`; `load` takes a path, so a temp file is needed), switch that copy to page view
  and render. Never touches the user's score; costs a save, a load and a full layout.
Rendering in the user's current continuous mode is also possible without any switch: in LINE
mode the score has one very wide "page" (`paint.cpp:52-57` comment, `pageSizeInch` from the page
bbox), but `PngWriter`'s image size is the page size × DPI (`pngwriter.cpp:56-60`), which for a
long score is a very large allocation; a crop must use `frameRect` and an image sized to the crop.

**Selection set-up.** None (use `boundingArea` with explicit segments).

**Arguments.** Proposed:
```json
{"type":"object","properties":{
  "page":{"type":"integer","minimum":1},
  "from":{"$ref":"#/defs/measureRef"},"to":{"$ref":"#/defs/measureRef"},
  "staves":{"type":"array"},
  "score":{"enum":["master","current"],"default":"master"},
  "format":{"enum":["png","svg"],"default":"png"},
  "dpi":{"type":"integer","minimum":36,"maximum":300,"default":150}
},"oneOf":[{"required":["page"]},{"required":["from","to"]}],"additionalProperties":false}
```

**Validation before mutation.**
- E_NO_SCORE; E_PART_NOT_FOUND.
- Page index < `pages().size()` **after** page layout → else E_PAGE_RANGE (protects the SVG flag
  leak and the 0×0 PNG, R7).
- Range valid (score_read rules) → E_RANGE.
- Every measure in the range has a system after layout (`Measure::system()` non-null, or its MM
  rest's, `scorerangeutilities.cpp:119-126`) → else E_NO_LAYOUT.
- Image pixel size under a cap → E_RANGE with the largest allowed DPI.
- E_BUSY under R2/R3 (layout is about to be switched; a drag or grip edit holds layout pointers).

**Post-condition.** PNG decodes with the expected size; layout mode of every touched score equals
its value before the call; `Score::printing()` false (`paint.cpp:177` restores it; SVG resets to
false, `svgwriter.cpp:271`). Content is the committed tree (R2) in page layout.

**Nested commands.** None. Layout only (outside undo). `Paint::paintScore` sets the global
`MScore::pdfPrinting` and does not restore it (`paint.cpp:70`); the next screen paint sets it back
(`npaint.cpp:186-196` passes `isPrinting=false`).

**Linked parts / excerpts.** Render a part through its own notation/score; lay it out first if it
was never opened (`eps.cpp:133-139`). Layout mode is per score (`score.h:927`).

**Crash and corruption risks.**
- Switching layout mode (A or B) deletes and rebuilds systems, pages and spanner segments
  (`scorelayout.cpp:61-68` for the empty case; layout generally). Anything holding layout
  pointers across the switch (selection of a spanner segment, `Selection::elements()`
  `select.h:113`; edit/grip data) can dangle. Option B has been exercised on every save with a
  thumbnail (`mscsaver.cpp:146-147`), but always from a save, not mid-edit.
- `pages().at(pi)` in `paintScore` is safe for out-of-range `fromPage` only because the loop
  bound uses the clamped `toPage` (`paint.cpp:76-82`); `pageSizeInch(opt)` returns an empty size
  (`paint.cpp:203-206`), so validate anyway.
- `boundingArea` dereferences `section.system->page()` (`scorerangeutilities.cpp:84`): a range in
  measures without layout crashes. Validate as above.
- Large images in continuous mode (above).

**Existing upstream tests.** None for painting or image writers. `src/engraving/tests/bsptree_tests.cpp`
covers `Page::items(rect)` lookup used by `frameRect`. `layoutelements_tests.cpp` lays out
`all_elements_data` fixtures.

**Scenarios.**
- READ-RN-01: render page 1 of a 3-page score in page view; PNG matches golden within tolerance.
- READ-RN-02: same with the user in continuous view; the user's view mode, scroll position and
  selection are unchanged afterwards.
- READ-RN-03: bars 12–19 spanning a system break → two crops (or one stacked image), each
  containing the bars and their dynamics and lyrics.
- READ-RN-04: range inside a multi-measure rest with MM rests shown → crop of the MM rest.
- READ-RN-05: page 99 → E_PAGE_RANGE; `MScore::svgPrinting` still false; screen draws page
  breaks normally.
- READ-RN-06: render of a never-opened part.

**Open questions.**
1. A, B or C (or render in the current mode)? The facts: A moves the user's viewport; B is silent
   but shares the layout-pointer exposure; C is isolated but slow.
2. Vertical extent of a range crop: `boundingArea` is staff-lines only. Union with the systems'
   bboxes, or with the bboxes of annotations/spanner segments in range? Not resolved from source.
3. Whether a layout-mode switch can free a `SpannerSegment` that is in the user's selection
   (layout code not traced to that level).

---

### score_changes  ·  command://notation/score_changes

**UI entry point.** No UI shows a change log. Upstream consumers of the same records: the
Inspector (`inspectorlistmodel.cpp:389`), the status bar
(`notationstatusbarmodel.cpp:274`), parts sync (`src/notation/internal/masternotation.cpp:140-144`),
`NotationParts` (`src/notation/internal/notationparts.cpp:330`), playback
(`src/engraving/playback/playbackmodel.cpp:75`), and the project's "needs save" flag
(`np.cpp:1035-1043`).

**What `ScoreChanges` carries** (`score.h:267-297`): `tickFrom`, `tickTo`, `staffIdxFrom`,
`staffIdxTo`, `isTextEditing`, `changedObjects` (map `EngravingObject*` → set of `CommandType`),
`changedTypes`, `changedPropertyIdSet`, `changedStyleIdSet`. Built by `buildScoreChanges`
(`cmd.cpp:125-154`) from `CmdState` and `UndoMacro::changesInfo` (`undo.cpp:627-663`).
Facts that limit its use:
- `changedObjects` are raw pointers. They are alive while the channel fires (removed items are
  held by their undo commands) but may be freed later (R4 a). Convert in the callback.
- `tickTo` is the largest *start* tick among changed items and `CmdState`'s end tick
  (`cmd.cpp:126-143`), not an end tick; widen to the containing measure's end.
- If `CmdState` had no tick (`startTick == -1`, `cmd.cpp:208-213`), `tickFrom` stays -1 even when
  objects changed, because the loop only lowers it when `startTick > tick` (`cmd.cpp:134-137`);
  `isValidBoundary()` is then false while `isValid()` may be true through `changedTypes`
  (`score.h:280-291`).
- Staff indices come from the master's shared `CmdState` (`score.cpp:6302-6309`), fed by
  `setLayout` calls from whichever score (master or part) did the edit, so they are not tagged
  with a score; recompute from `changedObjects` (`item->score()`, `staffIdx()`).
- For undo, command types are inverted (`undo.cpp:648-653`), and the record is built before the
  undo runs (`cmd.cpp:384-386`), after the redo runs (`cmd.cpp:387-389`).
- Sent per committed non-empty command (`cmd.cpp:444-446`) and per undo/redo step
  (`cmd.cpp:396-397`); `undoRedoToIndex` sends one per step (`notationundostack.cpp:94-99`).
  Not sent for rollbacks or empty macros (`cmd.cpp:428-446`).
- Not sent for anything outside the undo system: view mode, part open/close and excerpt init,
  solo/mute and view state (these mark the project unsaved separately, `np.cpp:1045-1061`),
  autosave's sanity-check repair of full-measure rests (`src/project/internal/projectautosaver.cpp:165`
  → `np.cpp:1131` → `check.cpp:203-206`), and the non-undoable side mutations of GG4.

**Why the undo index is not a `stateVersion`.** `INotationUndoStack::currentStateIndex()`
(`inotationundostack.h:64`, `notationundostack.cpp:223-230`) is `UndoStack::m_currentIndex`
(`undo.h:286`). It repeats (undo then a new edit lands on the same index with different content,
`undo.cpp:426-435`), shrinks when text editing merges its per-keystroke macros
(`textedit.cpp:162-165`, `undo.cpp:377-390`), and stays put when `reopen()` re-opens and re-commits
the last macro (`undo.cpp:444-458`, used by text editing `textedit.cpp:198`, `235`). The stack does
keep unique state ids (`m_stateList`, `m_nextState`, `undo.h:314-315`, assigned at `undo.cpp:434`)
but has no public accessor (only `isClean()` uses them, `undo.h:283`).
What the code supports instead: a module-owned counter incremented in a `changesChannel`
receiver (fires synchronously for every commit, undo and redo, R10), plus a per-open-score
session token so a reopened file does not reuse numbers.

**Selection set-up.** None.

**Arguments.** Proposed:
```json
{"type":"object","required":["since"],"properties":{
  "since":{"type":"integer","minimum":0},
  "score":{"enum":["master"],"default":"master"}
},"additionalProperties":false}
```
Proposed record kept per event (design is the conductor's): version, kind (command/undo/redo),
action name (`UndoStack::last()->actionName()`, `undo.h:290`, `undo.h:244`; for undo, read before
the step), origin (agent if the module's own transaction was committing, else user), measure range
(ordinal), master staff range, `changedTypes`, `changedPropertyIdSet`, `changedStyleIdSet`, and
EIDs of changed structural items that already have one.

**Validation before mutation.**
- E_NO_SCORE; E_SESSION_CHANGED if `since` belongs to a previous open of the score;
  E_VERSION_GONE if `since` is older than the retained history (bounded buffer).
- A `since` greater than the current version → E_RANGE.

**Post-condition.** The returned version equals the module's counter at response time; every
channel event after `since` is represented. Changes outside the undo system (list above) are not
detectable this way; report `nonUndoChangeSince` from `INotationProject::needSave()` notifications
if needed (`np.cpp:1045-1061`).

**Nested commands.** None. The receiver must not edit the score or call `update()`; it runs inside
`endCmd`/`undoRedo` (`cmd.cpp:397`, `445`).

**Linked parts / excerpts.** One channel per master covers edits made in any part
(`score.cpp:6297`). Map part staff indices to master via `Staff::findLinkedInScore`.

**Crash and corruption risks.**
- Holding `changedObjects` pointers past the callback (R4 a).
- Calling `item->tick()` on a removed element in the callback is what upstream itself does
  (`cmd.cpp:134`); safe at that moment only.
- Subscription lifetime: subscribe per `MasterScore` on `currentMasterNotationChanged`
  (`iglobalcontext.h:44`); a stale subscription on a closed score must not fire.

**Existing upstream tests.** `src/engraving/tests/playback/playbackmodel_tests.cpp:924-953` sends
hand-built `ScoreChanges` through `changesChannel`. No test checks `buildScoreChanges` itself.

**Scenarios.**
- READ-CH-01: user adds a dynamic in bar 5 between two agent requests; `score_changes(since)`
  reports bar 5, staff 1, type DYNAMIC, origin user (scenario S17 in the assessment).
- READ-CH-02: user undoes the agent's last batch; reported as undo of "Agent: …".
- READ-CH-03: user types a staff text (several keystrokes, merged); the version still increases
  and the final record shows the text change.
- READ-CH-04: user edits in a part tab; the record maps to master staves.
- READ-CH-05: close and reopen the score; old `since` → E_SESSION_CHANGED.

**Open questions.**
1. Retained history size and whether to coalesce text-editing keystroke events.
2. Whether the agent should also count `INotationProject::needSave()` notifications (non-undo
   changes) in `stateVersion`.

---

### score_validate  ·  command://notation/score_validate

**UI entry point.** Help/Diagnostics "Check for score corruptions" (`"check-for-score-corruptions"`,
`src/notationscene/internal/notationuiactions.cpp:2751-2754`, registered `nac.cpp:577`) →
`NotationActionController::checkForScoreCorruptions` (`nac.cpp:2351-2373`) →
`MasterScore::sanityCheck()` (`src/engraving/dom/check.cpp:112-134`) and then an `info`/`warning`
dialog (`nac.cpp:2362-2372`). The same `sanityCheck` runs on load
(`src/engraving/engravingproject.cpp:131-138`), before every save and autosave
(`NotationProject::canSave`, `np.cpp:1125-1138`; `projectautosaver.cpp:165`;
`projectactionscontroller.cpp:787-796`), and in Edit Staff (`src/notationscene/widgets/editstafftype.cpp:279`).
The UI toggle "mark-corrupted-measures" (`nac.cpp:80`, `notationuiactions.cpp:2737`) controls
drawing red boxes around flagged measures in the view (`src/engraving/rendering/score/debugpaint.cpp:298-315`;
option default true, `src/engraving/iengravingconfiguration.h:107`).

**What `sanityCheck` checks** (`Score::sanityCheckLocal`, `check.cpp:136-241`, for the master
and every part, `check.cpp:116`), per measure and staff:
- measure-repeat group without its MeasureRepeat element → "Corrupted measure" (`check.cpp:182-194`);
- voice 1 total ≠ measure length → "Incomplete measure" (`check.cpp:196-207`);
- voices 2–4 longer than the measure → "Voice too long" (`check.cpp:209-217`);
- any segment with content at a relative tick < 0 or > measure length → "Element outside measure"
  (`check.cpp:219-230`).
It returns `Err::FileCorrupted` (`src/engraving/engravingerrors.h:46`) with one translated,
HTML-tagged line per finding (`check.cpp:190`, `198`, `212`, `223`); measure numbers there are
1-based ordinals (`check.cpp:141`, `233`).

**It is not read-only.** It clears and sets `Score::m_corrupted` and each
`MStaff` corrupted flag (`check.cpp:152`, `162`, `192-193`, `200-201`, `214-215`, `226-227`), and
**when voice 1 is incomplete and starts with a full-measure rest it sets that rest's ticks to the
measure length** (`check.cpp:202-206`) by direct `setTicks`: not undoable, no `ScoreChanges`, no
layout. The UI does the same on every check, save and autosave, so this is "the UI's path", but
the agent must report it as a possible silent change.

**Other checks in the engine.**
- `Score::checkKeys()` (`check.cpp:248-271`, public `score.h:1035`): key map vs KeySig elements;
  read-only; returns bool, details only in debug log.
- `Score::checkScore()` (`check.cpp:49-104`): logs chord/rest gaps at debug level only; private
  (`score.h:1114`, private section from `score.h:1094`).
- `Score::checkSpanner()` (`src/engraving/editing/edit.cpp:4846-4894`) **removes** orphan spanners
  and shortens overlong ones with undo commands: an edit, not a check. Its detection rules
  (slur needs a CR at both ends on its tracks, other spanners need a start element, end ≤ last
  tick; `edit.cpp:4858-4881`) can be re-implemented read-only in the agent.
- Corruption at load: `EngravingProject::isCorruptedUponLoading()` (`engravingproject.cpp:163-166`);
  read errors `FileCorrupted`/`FileCriticallyCorrupted` (`engravingerrors.h:46-47`, messages
  `:95-99`).
- Agent-side invariants from gate G2 (every bar sums to its time signature = the "Incomplete
  measure" rule; no dangling spanner ends = the `checkSpanner` rules; tie partners exist on both
  notes, `note.cpp:1350-1358`).

**Selection set-up.** None.

**Arguments.** Proposed:
```json
{"type":"object","properties":{
  "checks":{"type":"array","items":{"enum":["sanity","keys","spanners","ties"]},
            "default":["sanity","spanners","ties"]}
},"additionalProperties":false}
```

**Validation before mutation.** E_NO_SCORE; E_BUSY while a macro is open (`sanityCheck` would
read and "fix" uncommitted state).

**Post-condition.** Findings returned as structured items `{score: "master"|part name, measure
(ordinal), staff, voice?, kind, found, expected}` parsed from the check's inputs rather than its
translated HTML strings (the strings are for dialogs). Report whether any full-measure rest was
repaired (compare the rest's ticks before and after, or detect the "Incomplete measure" +
full-measure-rest condition first). Report `corruptedOnLoad`.

**Nested commands.** None. No `startCmd`/`endCmd`; the repair is a direct mutation outside undo.

**Linked parts / excerpts.** `sanityCheck` already covers every initialised part via
`scoreList()` (`check.cpp:116`).

**Crash and corruption risks.**
- The silent full-measure-rest repair (above). It also flips the corrupted flags, which makes
  red boxes appear in the user's view when the debug option is on (`debugpaint.cpp:298-315`);
  renders are unaffected because `paintPageDebug` runs only when not printing (`paint.cpp:155-157`).
- Do not call `checkSpanner` for validation (it edits).
- Never use `NotationActionController::checkForScoreCorruptions` (it opens a dialog,
  `nac.cpp:2362-2372`).

**Existing upstream tests.** `src/engraving/tests/selectionrange_tests.cpp:323` asserts
`sanityCheck()` after range deletes. Deliberate corruption for fixtures: the dev tool
`CorruptScoreDevToolsModel::corruptOpenScore` sets every voice-1 chord/rest duration to 0/4 with
undo (`src/engraving/qml/MuseScore/Engraving/devtools/corruptscoredevtoolsmodel.cpp:32-66`).

**Scenarios.**
- READ-VA-01: clean fixture → no findings, no flags set, export unchanged byte-for-byte.
- READ-VA-02: fixture with an incomplete voice-1 measure → one "incomplete" finding with
  found/expected ticks.
- READ-VA-03: incomplete measure that holds a full-measure rest → finding plus `repaired:true`;
  no undo entry created.
- READ-VA-04: voice 2 overfull → "voice too long" with voice 2.
- READ-VA-05: orphan slur (hand-edited fixture) → spanner finding; score unchanged.
- READ-VA-06: run after each step of the soak (gate G2 invariants).

**Open questions.**
1. Accept the silent repair (same as UI, save and autosave), or reimplement the four rules
   read-only and never call `sanityCheck`? The latter would diverge from what the UI calls
   "corrupted" only if the rules drift upstream.
2. Should the module clear the corrupted flags it set, to avoid red boxes appearing in the
   user's view as a side effect of an agent request?

---

## Corrections to COMMANDS.md, assessment.md and the task brief

1. **assessment.md, Read model / Every response carries stateVersion (the undo index):** the undo
   index is not a version: it repeats after undo + new edit, shrinks on text-edit merges and stays
   put on `reopen()` (score_changes). A module-owned counter driven by `changesChannel` is needed.
2. **assessment.md, "score_export writes a MusicXML, mscx or MNX snapshot … into a memory buffer
   the way the converter module does":** the registered `mscx` writer cannot write to a buffer and
   deletes the target's parent directory recursively (`mscio.h:75-78`, `mscwriter.cpp:335-338`).
   Use the score XML writer into a buffer instead. Also, the converter never exports the open
   score: it loads a fresh copy from disk (`bapi.cpp:224-253`).
3. **assessment.md, handles:** "accept the handle only if the object found there is the one the
   EID register maps to" is defeated by address reuse (R4 b). Add type + fingerprint checks; and
   never call `itemFromEID` with client input (asserts, R4 c).
4. **assessment.md, "the register only unregisters objects in specific read-time cases":** also
   the writer's temporary TimeSig/KeySig (`twrite.cpp:3417`, `3600`). More importantly, EIDs are
   keyed by address, so new objects can inherit a dead object's EID (R4 b).
5. **assessment.md, score_render "a bar range cropped from its page by the selection's canvas
   rectangle":** no selection is needed (`ScoreRangeUtilities::boundingArea` takes segments), and
   that rectangle covers staff lines only, not elements above/below.
6. **assessment.md, "score_validate exposes MuseScore's corruption check":** that check mutates
   (corrupted flags; full-measure-rest ticks repair, `check.cpp:202-206`).
7. **assessment.md, "The export scenario forces page view, so the service switches the view mode
   and restores it":** correct for `ExportProjectScenario` (`eps.cpp:142-144`, `163-166`), but that
   switch notifies the view and can move the user's viewport; `Score::createThumbnail`'s silent
   switch is the alternative precedent (score_render).
8. **Task brief, "INotationWriter … which formats can be written to a path without dialogs":** the
   writers themselves never open dialogs; every dialog is in `ExportProjectScenario` (path picker,
   overwrite, retry, progress, reveal). The plugin API's `writeScore` uses that scenario and can
   show the overwrite dialog.
9. **COMMANDS.md** has no error for "edit in progress"; reads need one (E_BUSY) or a
   `committed:false` flag (R2, R3).
