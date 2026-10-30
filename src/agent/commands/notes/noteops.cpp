/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * MuseScore Agent
 * Copyright (C) 2026 Liam Green and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "noteops.h"

#include <algorithm>
#include <vector>

#include "engraving/dom/chord.h"
#include "engraving/dom/durationtype.h"
#include "engraving/dom/input.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/partialtie.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/utils.h"

#include "address/address.h"
#include "core/handles.h"
#include "notation/parser.h"
#include "notation/writer.h"
#include "notehelpers.h"
#include "read/scoreprinter.h"
#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::agent::notes;
using namespace mu::engraving;
using muse::Ret;
using muse::Val;
namespace nt = mu::agent::notation;

namespace {
std::string str(const Params& p, const char* key, const std::string& def = {})
{
    return address::has(p, key) ? p.at(key).toString() : def;
}

bool writtenMode(const Params& p)
{
    return str(p, "pitch", "sounding") == "written";
}

//! "E5", "F#4", "Bb3" -> a Pitch, using the notation's own parser on a one-note line
std::optional<nt::Pitch> parsePitch(const std::string& text)
{
    const nt::ParseResult r = nt::parse("m1 v1 | " + text + ":q");
    if (!r.ok() || r.doc.lines.size() != 1) {
        return std::nullopt;
    }
    const nt::VoiceLine* vl = std::get_if<nt::VoiceLine>(&r.doc.lines.front());
    const nt::Event* e = vl && vl->items.size() == 1 ? std::get_if<nt::Event>(&vl->items.front()) : nullptr;
    if (!e || e->kind != nt::EventKind::Note) {
        return std::nullopt;
    }
    return e->heads.front().pitch;
}

//! The bars around an element, as score_read prints them
void printBars(MasterScore* score, const Measure* from, const Measure* to, staff_idx_t staff, bool written, Out& out)
{
    read::ReadOptions opt;
    opt.fromBar = address::barOf(score, from);
    opt.toBar = address::barOf(score, to);
    opt.staves = { static_cast<int>(staff) + 1 };
    opt.writtenPitch = written;
    out["text"] = Val(nt::write(read::printScore(score, opt)));
    out["bars"] = Val(opt.fromBar == opt.toBar ? std::to_string(opt.fromBar)
                      : std::to_string(opt.fromBar) + "-" + std::to_string(opt.toBar));
}
}

// --- tie

Ret mu::agent::notes::tie(MasterScore* score, const Params& p, Out& out)
{
    ChordRest* cr = nullptr;
    if (Ret r = address::resolveTarget(score, p, "at", cr); !r) {
        return r;
    }
    if (!cr->isChord()) {
        return makeErr(Err::InvalidArg, address::where(score, cr) + " is a rest; ties join notes");
    }
    Chord* chord = toChord(cr);
    const bool written = writtenMode(p);
    const std::string action = str(p, "action", "add");
    if (action != "add" && action != "remove") {
        return makeErr(Err::InvalidArg, "action must be add or remove");
    }

    std::vector<Note*> notes;
    if (address::has(p, "note")) {
        const auto want = parsePitch(p.at("note").toString());
        if (!want) {
            return makeErr(Err::InvalidArg, "note must be a pitch such as \"E5\", \"F#4\" or \"Bb3\"");
        }
        for (Note* n : chord->notes()) {
            if (printedPitch(n, written).midi() == want->midi()) {
                notes.push_back(n);
            }
        }
        if (notes.empty()) {
            return makeErr(Err::InvalidArg, address::where(score, cr) + " has no " + nt::writePitch(*want));
        }
    } else {
        notes.assign(chord->notes().begin(), chord->notes().end());
    }

    std::vector<std::pair<Note*, Note*> > pairs;
    for (Note* n : notes) {
        const std::string name = nt::writePitch(printedPitch(n, written));
        if (action == "remove") {
            if (!n->tieFor()) {
                return makeErr(Err::InvalidArg, name + " at " + address::where(score, cr) + " is not tied");
            }
            continue;
        }
        if (n->tieFor()) {
            return makeErr(Err::Exists, name + " at " + address::where(score, cr) + " is already tied");
        }
        // the UI's search (Score::cmdToggleTie): the next note of that pitch. It may fall back to
        // another voice; a tie across voices is refused, and without a target the UI would add notes
        Note* end = searchTieNote(n);
        if (!end) {
            return makeErr(Err::InvalidArg, "nothing after " + name + " at " + address::where(score, cr)
                           + " has that pitch to tie to; write the next note first (write_voice)");
        }
        if (end->chord()->track() != chord->track()) {
            return makeErr(Err::InvalidArg, "the next " + name + " after " + address::where(score, cr)
                           + " is in another voice; ties stay in one voice");
        }
        if (end->chord()->tick() != chord->tick() + chord->actualTicks()) {
            return makeErr(Err::InvalidArg, "the next " + name + " is not the next event after "
                           + address::where(score, cr) + "; ties join neighbouring events");
        }
        pairs.push_back({ n, end });
    }

    const Measure* lastBar = chord->measure();
    if (action == "remove") {
        for (Note* n : notes) {
            if (Note* e = n->tieFor()->endNote()) {
                lastBar = e->chord()->measure();
            }
            score->undoRemoveElement(n->tieFor());
        }
        for (Note* n : notes) {
            if (n->tieFor()) {
                return makeErr(Err::PostCondition, "a tie was not removed; nothing was changed");
            }
        }
    } else {
        for (auto& [a, b] : pairs) {
            addTieBetween(a, b);
            lastBar = b->chord()->measure();
        }
        for (auto& [a, b] : pairs) {
            if (!a->tieFor() || a->tieFor()->endNote() != b) {
                return makeErr(Err::PostCondition, "a tie did not land on its note; nothing was changed");
            }
        }
    }
    out["notes"] = Val(static_cast<int>(notes.size()));
    printBars(score, chord->measure(), lastBar, chord->staffIdx(), written, out);
    return muse::make_ok();
}

// --- tuplet

Ret mu::agent::notes::tuplet(MasterScore* score, const Params& p, Out& out)
{
    ChordRest* cr = nullptr;
    if (Ret r = address::resolveTarget(score, p, "at", cr); !r) {
        return r;
    }
    if (cr->isGrace()) {
        return makeErr(Err::InvalidArg, "a grace note cannot become a tuplet");
    }
    if (cr->isMeasureRepeat()) {
        return makeErr(Err::InvalidArg, address::where(score, cr) + " is a measure repeat; write the bar out first");
    }
    const std::string ratio = str(p, "ratio");
    int actual = 0;
    int normal = 0;
    if (std::sscanf(ratio.c_str(), "%d:%d", &actual, &normal) != 2 || actual < 2 || normal < 1 || actual > 64 || normal > 64
        || ratio.find_first_not_of("0123456789:") != std::string::npos) {
        return makeErr(Err::InvalidArg, "ratio must be \"actual:normal\", e.g. \"3:2\" (three in the time of two)");
    }
    // Score::addTuplet's own checks, made first so its error dialog (edit.cpp:287-293) is never reached
    const TDuration d = cr->durationType();
    if (d < TDuration(DurationType::V_512TH) && d != TDuration(DurationType::V_MEASURE)) {
        return makeErr(Err::InvalidArg, address::where(score, cr) + " is too short for a tuplet");
    }
    const Fraction total = cr->ticks();
    const Fraction unit = total / normal;
    if (!TDuration::isValid(unit)) {
        return makeErr(Err::InvalidArg, "a " + ratio + " tuplet over " + total.toString().toStdString()
                       + " would have a unit of " + unit.toString().toStdString()
                       + ", which is not a note value; change the note's length first, or use another ratio");
    }
    TupletNumberType nt = TupletNumberType::SHOW_NUMBER;
    TupletBracketType bt = TupletBracketType::AUTO_BRACKET;
    const std::string number = str(p, "numberType", "number");
    const std::string bracket = str(p, "bracketType", "auto");
    if (number == "ratio") {
        nt = TupletNumberType::SHOW_RELATION;
    } else if (number == "none") {
        nt = TupletNumberType::NO_TEXT;
    } else if (number != "number") {
        return makeErr(Err::InvalidArg, "numberType must be number, ratio or none");
    }
    if (bracket == "bracket") {
        bt = TupletBracketType::SHOW_BRACKET;
    } else if (bracket == "none") {
        bt = TupletBracketType::SHOW_NO_BRACKET;
    } else if (bracket != "auto") {
        return makeErr(Err::InvalidArg, "bracketType must be auto, bracket or none");
    }

    const Fraction tick = cr->tick();
    const track_idx_t track = cr->track();
    Measure* m = cr->measure();
    Tuplet* t = score->addTuplet(cr, Fraction(actual, normal), nt, bt); // cr is replaced; not used again
    if (!t) {
        return makeErr(Err::Refused, "MuseScore did not create the tuplet");
    }
    Segment* seg = m->findSegment(SegmentType::ChordRest, tick);
    EngravingItem* first = seg ? seg->element(track) : nullptr;
    if (!first || !first->isChordRest() || toChordRest(first)->tuplet() != t || t->ratio() != Fraction(actual, normal)
        || t->ticks() != total || static_cast<int>(t->elements().size()) != actual) {
        return makeErr(Err::PostCondition, "the tuplet is not the one asked for; nothing was changed");
    }
    out["handle"] = Val(core::makeHandle(first));
    printBars(score, m, m, track2staff(track), writtenMode(p), out);
    return muse::make_ok();
}

// --- grace

Ret mu::agent::notes::grace(MasterScore* score, const Params& p, Out& out)
{
    ChordRest* cr = nullptr;
    if (Ret r = address::resolveTarget(score, p, "at", cr); !r) {
        return r;
    }
    if (!cr->isChord()) {
        return makeErr(Err::InvalidArg, address::where(score, cr) + " is a rest; grace notes attach to a note or chord");
    }
    if (cr->isGrace()) {
        return makeErr(Err::InvalidArg, "give the main note, not a grace note");
    }
    struct Kind {
        const char* name;
        NoteType type;
        int denominator; // the UI's DIVISION / denominator (notationinteraction.cpp:5748-5770)
    };
    static const Kind kinds[] = {
        { "acciaccatura", NoteType::ACCIACCATURA, 2 }, { "appoggiatura", NoteType::APPOGGIATURA, 2 },
        { "grace4", NoteType::GRACE4, 1 }, { "grace16", NoteType::GRACE16, 4 }, { "grace32", NoteType::GRACE32, 8 },
        { "grace8after", NoteType::GRACE8_AFTER, 2 }, { "grace16after", NoteType::GRACE16_AFTER, 4 },
        { "grace32after", NoteType::GRACE32_AFTER, 8 },
    };
    const std::string type = str(p, "type");
    const Kind* kind = nullptr;
    for (const Kind& k : kinds) {
        if (type == k.name) {
            kind = &k;
        }
    }
    if (!kind) {
        return makeErr(Err::InvalidArg, "type must be acciaccatura, appoggiatura, grace4, grace16, grace32, grace8after, "
                       "grace16after or grace32after");
    }
    const auto pitch = parsePitch(str(p, "note"));
    if (!pitch) {
        return makeErr(Err::InvalidArg, "note must be a pitch such as \"D5\", \"F#4\" or \"Bb3\"");
    }
    Chord* chord = toChord(cr);
    const bool written = writtenMode(p);
    const Stored s = storePitch(chord->staff(), *pitch, chord->tick(), written);
    if (s.pitch < 0 || s.pitch > 127) {
        return makeErr(Err::InvalidArg, nt::writePitch(*pitch) + " is outside MIDI pitches 0-127");
    }

    const size_t before = chord->graceNotes().size();
    Note* gn = score->setGraceNote(chord, s.pitch, kind->type, Constants::DIVISION / kind->denominator);
    if (!gn) {
        return makeErr(Err::Refused, "MuseScore did not add the grace note");
    }
    // setGraceNote spells from the key unless the main chord has the same pitch (cmd.cpp:880-885)
    gn->undoChangeProperty(Pid::TPC1, s.tpc1);
    gn->undoChangeProperty(Pid::TPC2, s.tpc2);
    if (chord->graceNotes().size() != before + 1 || gn->chord()->noteType() != kind->type || gn->pitch() != s.pitch
        || !(printedPitch(gn, written) == *pitch)) {
        return makeErr(Err::PostCondition, "the grace note is not the one asked for; nothing was changed");
    }
    out["handle"] = Val(core::makeHandle(gn));
    printBars(score, chord->measure(), chord->measure(), chord->staffIdx(), written, out);
    return muse::make_ok();
}

// --- shared by the edits below

namespace {
Ret refuseUnsupportedStaff(MasterScore* score, const ChordRest* cr)
{
    const Staff* staff = cr->staff();
    if (staff->isTabStaff(cr->tick()) || staff->isDrumStaff(cr->tick())) {
        return makeErr(Err::InvalidArg, address::where(score, cr) + " is on a tablature or percussion staff; "
                       "those are not supported yet (string/fret and drum heads)");
    }
    // a linked tablature staff would get the note without string and fret (specs/notes.md G9)
    for (const Staff* linked : staff->staffList()) {
        if (linked != staff && linked->isTabStaff(cr->tick())) {
            return makeErr(Err::InvalidArg, address::where(score, cr) + " is linked to a tablature staff; "
                           "that is not supported yet (string/fret)");
        }
    }
    return muse::make_ok();
}

//! The chords tied through the chord's first note, from the start of the chain: the chain
//! Score::addNoteToTiedChord walks (edit.cpp:637-677)
std::vector<Chord*> tieChain(Chord* chord)
{
    Note* ref = chord->notes().front();
    while (ref->tieBack() && !ref->incomingPartialTie() && ref->tieBack()->startNote()) {
        ref = ref->tieBack()->startNote();
    }
    std::vector<Chord*> out { ref->chord() };
    while (ref->tieFor() && ref->tieFor()->endNote()) {
        ref = ref->tieFor()->endNote();
        out.push_back(ref->chord());
    }
    return out;
}

//! `at` naming one note: a note handle, or a chord/target plus `note` (a pitch); a chord of one
//! note needs no pitch
Ret resolveNote(MasterScore* score, const Params& p, bool written, Note*& out)
{
    out = nullptr;
    if (address::has(p, "at") && p.at("at").type() == Val::Type::String) {
        muse::RetVal<EngravingItem*> r = core::resolveHandle(score, p.at("at").toString());
        if (!r.ret) {
            return r.ret;
        }
        if (r.val->isNote()) {
            if (address::has(p, "note")) {
                return makeErr(Err::InvalidArg, "at is a note handle; leave note out (or give the chord's handle and note)");
            }
            out = toNote(r.val);
            return muse::make_ok();
        }
    }
    ChordRest* cr = nullptr;
    if (Ret r = address::resolveTarget(score, p, "at", cr); !r) {
        return r;
    }
    if (!cr->isChord()) {
        return makeErr(Err::InvalidArg, address::where(score, cr) + " is a rest; there is no note to remove");
    }
    Chord* chord = toChord(cr);
    if (!address::has(p, "note")) {
        if (chord->notes().size() != 1) {
            return makeErr(Err::InvalidArg, address::where(score, cr) + " is a chord of " + std::to_string(chord->notes().size())
                           + " notes; say which with note (e.g. \"E5\")");
        }
        out = chord->notes().front();
        return muse::make_ok();
    }
    const auto want = parsePitch(p.at("note").toString());
    if (!want) {
        return makeErr(Err::InvalidArg, "note must be a pitch such as \"E5\", \"F#4\" or \"Bb3\"");
    }
    for (Note* n : chord->notes()) {
        if (printedPitch(n, written) == *want) {
            out = n;
            return muse::make_ok();
        }
    }
    std::string have;
    for (Note* n : chord->notes()) {
        have += (have.empty() ? "" : ", ") + nt::writePitch(printedPitch(n, written));
    }
    return makeErr(Err::InvalidArg, address::where(score, cr) + " has no " + nt::writePitch(*want) + " (it has " + have
                   + "; spelling counts)");
}

//! "q.", "e", "h.." -> its value; the notation's own parser on a one-rest line
std::optional<TDuration> parseDuration(const std::string& text)
{
    const nt::ParseResult r = nt::parse("m1 v1 | r:" + text);
    if (!r.ok() || r.doc.lines.size() != 1) {
        return std::nullopt;
    }
    const nt::VoiceLine* vl = std::get_if<nt::VoiceLine>(&r.doc.lines.front());
    const nt::Event* e = vl && vl->items.size() == 1 ? std::get_if<nt::Event>(&vl->items.front()) : nullptr;
    if (!e || e->kind != nt::EventKind::Rest || !e->marks.empty() || e->slurOpen || e->slurClose) {
        return std::nullopt;
    }
    static const std::pair<nt::BaseDuration, DurationType> types[] = {
        { nt::BaseDuration::Breve, DurationType::V_BREVE }, { nt::BaseDuration::Whole, DurationType::V_WHOLE },
        { nt::BaseDuration::Half, DurationType::V_HALF }, { nt::BaseDuration::Quarter, DurationType::V_QUARTER },
        { nt::BaseDuration::Eighth, DurationType::V_EIGHTH }, { nt::BaseDuration::Sixteenth, DurationType::V_16TH },
        { nt::BaseDuration::ThirtySecond, DurationType::V_32ND }, { nt::BaseDuration::SixtyFourth, DurationType::V_64TH },
        { nt::BaseDuration::OneTwentyEighth, DurationType::V_128TH },
    };
    for (const auto& [b, t] : types) {
        if (b == e->duration.base) {
            TDuration d(t);
            d.setDots(e->duration.dots);
            return d.isValid() ? std::optional<TDuration>(d) : std::nullopt;
        }
    }
    return std::nullopt;
}

ChordRest* chordRestStarting(MasterScore* score, const Fraction& tick, track_idx_t track)
{
    Segment* seg = score->tick2segment(tick, true, SegmentType::ChordRest);
    EngravingItem* e = seg && seg->tick() == tick ? seg->element(track) : nullptr;
    return e && e->isChordRest() ? toChordRest(e) : nullptr;
}
}

// --- add_note_to_chord

Ret mu::agent::notes::addNoteToChord(MasterScore* score, const Params& p, Out& out)
{
    ChordRest* cr = nullptr;
    if (Ret r = address::resolveTarget(score, p, "at", cr); !r) {
        return r;
    }
    if (!cr->isChord()) {
        return makeErr(Err::InvalidArg, address::where(score, cr) + " is a rest; write a note there first (write_voice)");
    }
    Chord* chord = toChord(cr);
    if (chord->isTrillCueNote()) {
        return makeErr(Err::Range, address::where(score, cr) + " is a trill's cue note, which MuseScore does not let you edit");
    }
    if (Ret r = refuseUnsupportedStaff(score, cr); !r) {
        return r;
    }
    const auto pitch = parsePitch(str(p, "note"));
    if (!pitch) {
        return makeErr(Err::InvalidArg, "note must be a pitch such as \"E5\", \"F#4\" or \"Bb3\"");
    }
    const bool written = writtenMode(p);
    const Stored s = storePitch(chord->staff(), *pitch, chord->tick(), written);
    if (s.pitch < 0 || s.pitch > 127) {
        return makeErr(Err::InvalidArg, nt::writePitch(*pitch) + " is outside MIDI pitches 0-127");
    }
    const bool extend = address::has(p, "extendThroughTies") ? p.at("extendThroughTies").toBool() : true;
    const bool force = address::has(p, "forceAccidental") && p.at("forceAccidental").toBool();
    const std::vector<Chord*> chain = extend ? tieChain(chord) : std::vector<Chord*> { chord };
    for (const Chord* c : chain) {
        // Score::addNote does not check (edit.cpp:586-596); the note-input path would remove it instead
        if (const Note* dup = c->findNote(s.pitch)) {
            return makeErr(Err::Exists, address::where(score, c) + " already has " + nt::writePitch(printedPitch(dup, written))
                           + (c == chord ? "" : " (a chord tied to the target; extendThroughTies false adds to the target only)"));
        }
    }
    std::vector<size_t> before;
    for (const Chord* c : chain) {
        before.push_back(c->notes().size());
    }

    NoteVal nv(s.pitch);
    // both spellings: Score::addNote reads one of them for forceAccidental (edit.cpp:597-598)
    nv.tpc1 = s.tpc1;
    nv.tpc2 = s.tpc2;
    // Score::addNoteToTiedChord's recipe (edit.cpp:632-697; private upstream): the note in every
    // chord of the chain, each tied to the next. A private InputState keeps Score::addNote off the
    // selection and the user's input position (edit.cpp:614-620)
    Note* last = nullptr;
    for (Chord* c : chain) {
        InputState is;
        Note* n = score->addNote(c, nv, force, {}, &is);
        if (!n) {
            return makeErr(Err::Refused, "MuseScore did not add the note at " + address::where(score, c));
        }
        if (last) {
            addTieBetween(last, n);
        }
        last = n;
    }

    Note* prev = nullptr;
    for (size_t i = 0; i < chain.size(); ++i) {
        Note* n = chain[i]->findNote(s.pitch);
        if (!n || chain[i]->notes().size() != before[i] + 1 || !(printedPitch(n, written) == *pitch)) {
            return makeErr(Err::PostCondition, "the note was not added as asked at " + address::where(score, chain[i])
                           + "; nothing was changed");
        }
        if (prev && (!prev->tieFor() || prev->tieFor()->endNote() != n)) {
            return makeErr(Err::PostCondition, "the added notes are not tied through the chain; nothing was changed");
        }
        prev = n;
    }
    out["handle"] = Val(core::makeHandle(chord->findNote(s.pitch)));
    out["chords"] = Val(static_cast<int>(chain.size()));
    printBars(score, chain.front()->measure(), chain.back()->measure(), chord->staffIdx(), written, out);
    return muse::make_ok();
}

// --- remove_note

Ret mu::agent::notes::removeNote(MasterScore* score, const Params& p, Out& out)
{
    const bool written = writtenMode(p);
    Note* note = nullptr;
    if (Ret r = resolveNote(score, p, written, note); !r) {
        return r;
    }
    Chord* chord = note->chord();
    const std::string name = nt::writePitch(printedPitch(note, written)) + " at " + address::where(score, chord);
    if (note->generated()) {
        return makeErr(Err::InvalidArg, name + " is generated by MuseScore and cannot be removed");
    }
    if (chord->isTrillCueNote()) {
        return makeErr(Err::Range, name + " is a trill's cue note, which MuseScore does not let you edit");
    }
    if (Ret r = refuseUnsupportedStaff(score, chord); !r) {
        return r;
    }
    const std::string whenLast = str(p, "whenLastNote", "rest");
    if (whenLast != "rest" && whenLast != "error") {
        return makeErr(Err::InvalidArg, "whenLastNote must be rest or error");
    }
    const bool last = chord->notes().size() == 1;
    if (last && whenLast == "error") {
        return makeErr(Err::InvalidArg, name + " is the only note of its chord (whenLastNote is error)");
    }

    const bool grace = chord->isGrace();
    Chord* mainChord = grace ? toChord(chord->explicitParent()) : nullptr;
    const Fraction tick = chord->tick();
    const Fraction ticks = chord->ticks();
    const track_idx_t track = chord->track();
    const Tuplet* tuplet = chord->tuplet();
    Measure* m = chord->measure();
    Note* tiedFrom = note->tieBack() ? note->tieBack()->startNote() : nullptr;

    // the Delete key's call for one selected note (Score::cmdDeleteSelection -> deleteItem,
    // edit.cpp:2821-2869): a note of a chord goes; the last note leaves a rest (a grace chord, nothing)
    score->deleteItem(note);

    if (tiedFrom && tiedFrom->tieFor() && tiedFrom->tieFor()->endNote() == note) {
        return makeErr(Err::PostCondition, "the tie into " + name + " was left behind; nothing was changed");
    }
    if (!last) {
        const auto& ns = chord->notes();
        if (std::find(ns.begin(), ns.end(), note) != ns.end()) {
            return makeErr(Err::PostCondition, name + " is still in its chord; nothing was changed");
        }
        out["handle"] = Val(core::makeHandle(chord));
    } else if (grace) {
        const auto& gs = mainChord->graceNotes();
        if (std::find(gs.begin(), gs.end(), chord) != gs.end()) {
            return makeErr(Err::PostCondition, "the grace note " + name + " is still there; nothing was changed");
        }
        out["handle"] = Val(core::makeHandle(mainChord));
    } else {
        ChordRest* now = chordRestStarting(score, tick, track);
        if (!now || !now->isRest() || now->ticks() != ticks || now->tuplet() != tuplet) {
            return makeErr(Err::PostCondition, "no rest of the same length took the place of " + name + "; nothing was changed");
        }
        out["handle"] = Val(core::makeHandle(now));
    }
    out["leftRest"] = Val(last && !grace);
    printBars(score, m, m, track2staff(track), written, out);
    return muse::make_ok();
}

// --- set_duration

Ret mu::agent::notes::setDuration(MasterScore* score, const Params& p, Out& out)
{
    ChordRest* cr = nullptr;
    if (Ret r = address::resolveTarget(score, p, "at", cr); !r) {
        return r;
    }
    if (cr->isMeasureRepeat()) {
        return makeErr(Err::InvalidArg, address::where(score, cr) + " is a measure repeat; write the bar out first");
    }
    if (cr->isChord() && toChord(cr)->isTrillCueNote()) {
        return makeErr(Err::Range, address::where(score, cr) + " is a trill's cue note, which MuseScore does not let you edit");
    }
    const std::string dtext = str(p, "duration");
    const auto d = parseDuration(dtext);
    if (!d) {
        return makeErr(Err::InvalidArg, "duration must be a note value in the compact notation, e.g. \"q\", \"q.\", \"e\", \"h..\"");
    }
    const bool allowOverwrite = address::has(p, "allowOverwrite") && p.at("allowOverwrite").toBool();
    const bool written = writtenMode(p);
    const std::string here = address::where(score, cr);
    Measure* m = cr->measure();
    const staff_idx_t staff = cr->staffIdx();

    if (cr->isGrace()) {
        // padToggle's grace branch (score.cpp:3238-3246): only the written value changes
        Chord* g = toChord(cr);
        score->undoChangeChordRestLen(g, *d);
        if (g->durationType() != *d) {
            return makeErr(Err::PostCondition, "the grace note's value did not change; nothing was changed");
        }
        out["handle"] = Val(core::makeHandle(g));
        printBars(score, m, m, staff, written, out);
        return muse::make_ok();
    }

    const Fraction tick = cr->tick();
    const track_idx_t track = cr->track();
    Tuplet* tuplet = cr->tuplet();
    const Fraction stretch = cr->staff()->timeStretch(tick);
    const Fraction oldEnd = cr->endTick();
    const Fraction newEnd = tick + actualTicks(d->fraction(), tuplet, stretch);
    const bool wasChord = cr->isChord();

    if (newEnd > oldEnd) {
        if (tuplet && newEnd > tuplet->tick() + tuplet->actualTicks()) {
            return makeErr(Err::Range, here + " is in a tuplet; it can grow only to the tuplet's end ("
                           + address::beatOf(tuplet->measure(), tuplet->tick() + tuplet->actualTicks()) + ")");
        }
        if (newEnd > score->endTick()) {
            return makeErr(Err::Range, "a " + dtext + " at " + here + " would run past the end of the score");
        }
        // what the longer value covers (changeCRlen's makeGap, cmd.cpp:1788)
        std::string notesCovered;
        for (Segment* s = cr->segment()->next1(SegmentType::ChordRest); s && s->tick() < newEnd; s = s->next1(SegmentType::ChordRest)) {
            EngravingItem* e = s->element(track);
            if (!e || !e->isChordRest()) {
                continue;
            }
            ChordRest* next = toChordRest(e);
            if (next->isMeasureRepeat()) {
                return makeErr(Err::InvalidArg, "a " + dtext + " at " + here + " would overwrite the measure repeat at "
                               + address::where(score, next) + "; write the bar out first");
            }
            if (!tuplet) {
                for (Tuplet* t = next->tuplet(); t; t = t->tuplet()) {
                    if (t->tick() + t->actualTicks() > newEnd) {
                        return makeErr(Err::InvalidArg, "a " + dtext + " at " + here + " would end inside the tuplet at "
                                       + address::where(score, next) + "; change the tuplet first");
                    }
                }
            }
            if (next->isChord()) {
                notesCovered += (notesCovered.empty() ? "" : ", ") + address::where(score, next);
            }
        }
        if (!notesCovered.empty() && !allowOverwrite) {
            return makeErr(Err::InvalidArg, "a " + dtext + " at " + here + " would overwrite the notes at " + notesCovered
                           + "; set allowOverwrite to replace them");
        }
    }

    // the duration key on a selected note or rest: Score::padToggle -> changeCRlen (score.cpp:3244)
    score->changeCRlen(cr, *d); // cr may be replaced; not used again

    ChordRest* now = chordRestStarting(score, tick, track);
    if (!now || now->isChord() != wasChord) {
        return makeErr(Err::PostCondition, "nothing of that kind starts at the old place any more; nothing was changed");
    }
    const Measure* lastBar = m;
    if (newEnd <= m->endTick()) {
        if (now->durationType() != *d || now->endTick() != newEnd) {
            return makeErr(Err::PostCondition, "the new value is not " + dtext + "; nothing was changed");
        }
    } else {
        // across the bar line: MuseScore splits it into tied notes (or rests) that add up (cmd.cpp:1810-1851)
        ChordRest* cur = now;
        while (cur && cur->endTick() < newEnd) {
            ChordRest* next = nullptr;
            if (cur->isChord()) {
                const Note* n = toChord(cur)->notes().front();
                next = n->tieFor() && n->tieFor()->endNote() ? n->tieFor()->endNote()->chord() : nullptr;
            } else {
                next = chordRestStarting(score, cur->endTick(), track);
                next = next && next->isRest() ? next : nullptr;
            }
            if (next && next->tick() != cur->endTick()) {
                next = nullptr;
            }
            cur = next;
        }
        if (!cur || cur->endTick() != newEnd) {
            return makeErr(Err::PostCondition, "the new value is not split into tied notes or rests that add up to "
                           + dtext + "; nothing was changed");
        }
        lastBar = cur->measure();
    }
    out["handle"] = Val(core::makeHandle(now));
    printBars(score, m, lastBar, staff, written, out);
    return muse::make_ok();
}

// --- exchange_voices

namespace {
//! The chords of a voice in a bar, as (tick, length, pitches): what has to swap. Rests are left
//! out: voice 1 gets rests where the other voice had none (edit.cpp:6428-6456)
struct ChordContent {
    Fraction tick;
    Fraction ticks;
    std::vector<int> pitches;
    bool operator==(const ChordContent& o) const { return tick == o.tick && ticks == o.ticks && pitches == o.pitches; }
};
using VoiceContent = std::vector<ChordContent>;

VoiceContent chordsOf(const Measure* m, track_idx_t track)
{
    VoiceContent out;
    for (const Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
        const EngravingItem* e = s->element(track);
        if (e && e->isChord()) {
            std::vector<int> pitches;
            for (const Note* n : toChord(e)->notes()) {
                pitches.push_back(n->pitch());
            }
            out.push_back({ s->tick(), toChord(e)->actualTicks(), pitches });
        }
    }
    return out;
}

//! Voice 1 fills the bar with no gap
bool voiceOneComplete(const Measure* m, staff_idx_t staff)
{
    const Fraction end = m->tick() + m->stretchedLen(m->score()->staff(staff)); // local time signatures stretch it
    Fraction at = m->tick();
    for (const Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
        const EngravingItem* e = s->element(staff * VOICES);
        if (!e || !e->isChordRest()) {
            continue;
        }
        const ChordRest* cr = toChordRest(e);
        if (cr->tick() != at || (cr->isRest() && toRest(cr)->isGap())) {
            return false;
        }
        at = cr->isMeasureRepeat() || cr->isFullMeasureRest() ? end : cr->endTick();
    }
    return at == end;
}

Ret wholeNumber(const Params& p, const char* key, int lo, int hi, int& out)
{
    if (!address::has(p, key)) {
        return makeErr(Err::InvalidArg, std::string("missing ") + key);
    }
    const double v = p.at(key).toDouble();
    if (v != static_cast<int>(v)) {
        return makeErr(Err::InvalidArg, std::string(key) + " must be a whole number");
    }
    out = static_cast<int>(v);
    if (out < lo || out > hi) {
        return makeErr(Err::Range, std::string(key) + " " + std::to_string(out) + " is outside " + std::to_string(lo) + "-"
                       + std::to_string(hi));
    }
    return muse::make_ok();
}
}

Ret mu::agent::notes::exchangeVoices(MasterScore* score, const Params& p, Out& out)
{
    if (!address::has(p, "staves") || p.at("staves").type() != Val::Type::List || p.at("staves").toList().empty()) {
        return makeErr(Err::InvalidArg, "staves must list at least one staff");
    }
    std::vector<staff_idx_t> staves;
    for (const Val& v : p.at("staves").toList()) {
        staff_idx_t si = 0;
        if (Ret r = address::resolveStaff(score, v, si); !r) {
            return r;
        }
        staves.push_back(si);
    }
    std::sort(staves.begin(), staves.end());
    staves.erase(std::unique(staves.begin(), staves.end()), staves.end());
    for (size_t i = 1; i < staves.size(); ++i) {
        if (staves[i] != staves[i - 1] + 1) {
            return makeErr(Err::Range, "staves must be next to each other (a range selection), e.g. [2, 3, 4]");
        }
    }
    // undoExchangeVoice also swaps every linked staff (edit.cpp:6421-6424); two linked staves in the
    // list would be swapped twice, back to where they were
    for (staff_idx_t si : staves) {
        for (const Staff* linked : score->staff(si)->staffList()) {
            if (linked->score() == score && linked->idx() != si
                && std::find(staves.begin(), staves.end(), linked->idx()) != staves.end()) {
                return makeErr(Err::InvalidArg, "staves " + std::to_string(si + 1) + " and " + std::to_string(linked->idx() + 1)
                               + " are linked; give only one of them (the other follows)");
            }
        }
    }
    const std::vector<Measure*> bars = address::allMeasures(score);
    const int nbars = static_cast<int>(bars.size());
    int from = 0;
    int to = 0;
    int a = 0;
    int b = 0;
    if (Ret r = wholeNumber(p, "fromMeasure", 1, nbars, from); !r) {
        return r;
    }
    if (Ret r = wholeNumber(p, "toMeasure", 1, nbars, to); !r) {
        return r;
    }
    if (to < from) {
        return makeErr(Err::Range, "toMeasure " + std::to_string(to) + " is before fromMeasure " + std::to_string(from));
    }
    if (Ret r = wholeNumber(p, "voiceA", 1, 4, a); !r) {
        return r;
    }
    if (Ret r = wholeNumber(p, "voiceB", 1, 4, b); !r) {
        return r;
    }
    if (a == b) {
        return makeErr(Err::Range, "voiceA and voiceB are both " + std::to_string(a) + "; give two different voices");
    }
    for (int i = from; i <= to; ++i) {
        for (staff_idx_t si : staves) {
            for (const Segment* s = bars[i - 1]->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
                const EngravingItem* e = s->element(si * VOICES);
                if (e && e->isMeasureRepeat()) {
                    return makeErr(Err::InvalidArg, "m" + std::to_string(i) + " of staff " + std::to_string(si + 1)
                                   + " is a measure repeat; write the bar out first");
                }
            }
        }
    }

    const voice_idx_t va = static_cast<voice_idx_t>(a - 1);
    const voice_idx_t vb = static_cast<voice_idx_t>(b - 1);
    std::vector<std::pair<VoiceContent, VoiceContent> > before;
    for (int i = from; i <= to; ++i) {
        for (staff_idx_t si : staves) {
            before.push_back({ chordsOf(bars[i - 1], si * VOICES + va), chordsOf(bars[i - 1], si * VOICES + vb) });
        }
    }

    // Score::cmdExchangeVoice's loop (edit.cpp:4571-4577) over the bars, without its range
    // selection: the master score, so linked staves and parts follow (edit.cpp:6364-6424)
    for (int i = from; i <= to; ++i) {
        score->undoExchangeVoice(bars[i - 1], va, vb, staves.front(), staves.back() + 1);
    }

    size_t k = 0;
    for (int i = from; i <= to; ++i) {
        for (staff_idx_t si : staves) {
            const auto& [wasA, wasB] = before[k++];
            if (chordsOf(bars[i - 1], si * VOICES + va) != wasB || chordsOf(bars[i - 1], si * VOICES + vb) != wasA) {
                return makeErr(Err::PostCondition, "voices " + std::to_string(a) + " and " + std::to_string(b) + " of m"
                               + std::to_string(i) + ", staff " + std::to_string(si + 1) + " did not swap; nothing was changed");
            }
            if (!voiceOneComplete(bars[i - 1], si)) {
                return makeErr(Err::PostCondition, "voice 1 of m" + std::to_string(i) + ", staff " + std::to_string(si + 1)
                               + " has a gap after the swap; nothing was changed");
            }
        }
    }

    read::ReadOptions opt;
    opt.fromBar = from;
    opt.toBar = to;
    for (staff_idx_t si : staves) {
        opt.staves.push_back(static_cast<int>(si) + 1);
    }
    opt.writtenPitch = writtenMode(p);
    out["text"] = Val(nt::write(read::printScore(score, opt)));
    out["bars"] = Val(from == to ? std::to_string(from) : std::to_string(from) + "-" + std::to_string(to));
    return muse::make_ok();
}
