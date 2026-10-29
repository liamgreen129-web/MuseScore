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

#include <vector>

#include "engraving/dom/chord.h"
#include "engraving/dom/durationtype.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
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
