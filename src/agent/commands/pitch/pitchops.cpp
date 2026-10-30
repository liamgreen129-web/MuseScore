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
#include "pitchops.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <unordered_set>
#include <utility>

#include "engraving/dom/chord.h"
#include "engraving/dom/drumset.h"
#include "engraving/dom/fret.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/interval.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/linkedobjects.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/select.h"
#include "engraving/dom/selectionfilter.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftype.h"
#include "engraving/dom/stringdata.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/utils.h"
#include "engraving/editing/transpose.h"
#include "engraving/types/constants.h"

#include "address/address.h"
#include "commands/notes/notehelpers.h"
#include "core/handles.h"
#include "notation/writer.h"
#include "read/scoreprinter.h"
#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::engraving;
using muse::Ret;
using muse::Val;
namespace nt = mu::agent::notation;

namespace {
// ------------------------------------------------------------------ small helpers

constexpr int MAX_NOTATION_BARS = 32; // the result's notation stops here; score_read reads the rest

bool has(const Params& p, const char* key)
{
    return address::has(p, key);
}

bool isNumber(const Val& v)
{
    return v.type() == Val::Type::Double || v.type() == Val::Type::Int || v.type() == Val::Type::Int64;
}

bool isWhole(const Val& v)
{
    return isNumber(v) && std::isfinite(v.toDouble()) && v.toDouble() == std::floor(v.toDouble());
}

std::string joined(const std::vector<std::string>& v, const char* sep = ", ")
{
    std::string s;
    for (const std::string& x : v) {
        s += (s.empty() ? "" : sep) + x;
    }
    return s;
}

std::string notationOf(MasterScore* score, int fromBar, int toBar, std::vector<int> staves)
{
    std::sort(staves.begin(), staves.end());
    staves.erase(std::unique(staves.begin(), staves.end()), staves.end());
    read::ReadOptions o;
    o.fromBar = fromBar;
    o.toBar = std::max(fromBar, std::min(toBar, fromBar + MAX_NOTATION_BARS - 1));
    o.staves = staves;
    return nt::write(read::printScore(score, o));
}

std::string barsText(int from, int to)
{
    return from == to ? "m" + std::to_string(from) : "m" + std::to_string(from) + "-m" + std::to_string(to);
}

//! The chord a note's beat belongs to (a grace chord's parent)
Chord* mainChord(Chord* c)
{
    if (c->isGrace() && c->explicitParent() && c->explicitParent()->isChord()) {
        return toChord(c->explicitParent());
    }
    return c;
}

//! "m3 beat 2 (staff 1, voice 1) F#4", sounding pitch as score_read prints it
std::string describe(MasterScore* score, const Note* n)
{
    Chord* c = mainChord(n->chord());
    std::string s = address::where(score, c);
    if (n->chord() != c) {
        s += " (grace note)";
    }
    return s + " " + nt::writePitch(notes::printedPitch(n, false));
}

std::string spellingOf(int tpc)
{
    nt::Pitch p;
    p.step = static_cast<char>(tpc2stepName(tpc).unicode());
    p.alter = static_cast<int>(tpc2alter(tpc));
    std::string s(1, p.step);
    s += p.alter > 0 ? std::string(p.alter, '#') : std::string(-p.alter, 'b');
    return s;
}

bool isPercussionStaff(const Staff* s)
{
    // the transpose engine's test (transpose.cpp:157-162)
    if (s->staffType(Fraction(0, 1))->group() == StaffGroup::PERCUSSION) {
        return true;
    }
    return s->primaryStaff() && s->primaryStaff()->staffType(Fraction(0, 1))->group() == StaffGroup::PERCUSSION;
}

//! The selection filter is user state that the range transpose reads (spec P11). It is set to
//! "everything" for the engine call and put back right after it, so the user never sees it change.
class FilterOverride
{
public:
    explicit FilterOverride(Score* score)
        : m_score(score), m_saved(score->selectionFilter())
    {
        m_score->selectionFilter() = SelectionFilter();
    }

    ~FilterOverride() { m_score->selectionFilter() = m_saved; }

private:
    Score* m_score = nullptr;
    SelectionFilter m_saved;
};

// ================================================================== set_pitch

struct PitchArg {
    bool spelled = false;
    nt::Pitch name;
    int midi = 0;
};

//! pitch (a spelled pitch) or midi (a MIDI number): exactly one
Ret readPitchArg(const Params& p, PitchArg& out)
{
    const bool hasPitch = has(p, "pitch");
    const bool hasMidi = has(p, "midi");
    if (hasPitch == hasMidi) {
        return makeErr(Err::InvalidArg, hasPitch ? std::string("give pitch or midi, not both")
                       : std::string("missing pitch: a spelled pitch such as \"F#4\" (C4 = middle C), or midi: a MIDI number 0-127"));
    }
    if (hasPitch) {
        const Val& v = p.at("pitch");
        if (v.type() != Val::Type::String) {
            return makeErr(Err::InvalidArg, "pitch must be a spelled pitch such as \"F#4\"; give a number as midi");
        }
        std::string why;
        if (!pitch::parsePitchName(v.toString(), out.name, &why)) {
            return makeErr(Err::InvalidArg, "pitch \"" + v.toString() + "\": " + why);
        }
        out.spelled = true;
        out.midi = out.name.midi();
        return muse::make_ok();
    }
    const Val& v = p.at("midi");
    if (!isWhole(v)) {
        return makeErr(Err::InvalidArg, "midi must be a whole number 0-127 (60 = middle C)");
    }
    const double d = v.toDouble();
    if (d < 0 || d > 127) {
        return makeErr(Err::InvalidArg, "midi " + address::number(d) + " is outside 0-127");
    }
    out.midi = static_cast<int>(d);
    return muse::make_ok();
}

//! A note of `chord` chosen by note (its spelled pitch as score_read prints it, in the call's pitch
//! space) or note_index (1-based from the bottom); with neither the chord must have one note.
Ret pickNote(MasterScore* score, Chord* chord, const Params& p, bool written, Note*& out)
{
    const std::vector<Note*>& ns = chord->notes();
    std::vector<std::string> names;
    for (const Note* n : ns) {
        names.push_back(nt::writePitch(notes::printedPitch(n, written)));
    }
    const std::string where = address::where(score, mainChord(chord)) + (chord->isGrace() ? " (grace chord)" : "");
    const bool byPitch = has(p, "note");
    const bool byIndex = has(p, "note_index");
    if (byPitch && byIndex) {
        return makeErr(Err::InvalidArg, "give note or note_index, not both");
    }
    if (!byPitch && !byIndex) {
        if (ns.size() == 1) {
            out = ns.front();
            return muse::make_ok();
        }
        return makeErr(Err::InvalidArg, "the chord at " + where + " has " + std::to_string(ns.size()) + " notes ("
                       + joined(names) + "); give note (its pitch) or note_index (1 = the lowest)");
    }
    if (byIndex) {
        const Val& sel = p.at("note_index");
        if (!isWhole(sel) || sel.toDouble() < 1) {
            return makeErr(Err::InvalidArg, "note_index must be a whole number from 1 (1 = the lowest note)");
        }
        const int i = static_cast<int>(sel.toDouble());
        if (i > static_cast<int>(ns.size())) {
            return makeErr(Err::Range, "note_index " + std::to_string(i) + ": the chord at " + where + " has "
                           + std::to_string(ns.size()) + " note(s) (" + joined(names) + "), 1 = the lowest");
        }
        out = ns.at(static_cast<size_t>(i - 1));
        return muse::make_ok();
    }
    const Val& sel = p.at("note");
    if (sel.type() != Val::Type::String) {
        return makeErr(Err::InvalidArg, "note must be the chord tone's pitch, e.g. \"E4\" (or use note_index)");
    }
    nt::Pitch want;
    std::string why;
    if (!pitch::parsePitchName(sel.toString(), want, &why)) {
        return makeErr(Err::InvalidArg, "note \"" + sel.toString() + "\": " + why);
    }
    for (Note* n : ns) {
        if (notes::printedPitch(n, written) == want) {
            out = n;
            return muse::make_ok();
        }
    }
    return makeErr(Err::InvalidArg, "note: no " + sel.toString() + " in the chord at " + where + " (it holds "
                   + joined(names) + (written ? ", written pitch" : ", sounding pitch") + ")");
}

//! target (a Note or Chord handle) or staff + at (+ voice), the expression-marks shape on address.h;
//! then grace (1-based grace chord before it) and note / note_index
Ret resolveNote(MasterScore* score, const Params& p, bool written, Note*& out)
{
    out = nullptr;
    const bool byHandle = has(p, "target");
    const bool byPlace = has(p, "staff") || has(p, "at") || has(p, "voice");
    if (byHandle && byPlace) {
        return makeErr(Err::InvalidArg, "give target (a handle) or staff + at (+ voice), not both");
    }
    Chord* chord = nullptr;
    if (byHandle) {
        const Val& v = p.at("target");
        if (v.type() != Val::Type::String) {
            return makeErr(Err::InvalidArg, "target must be a Note or Chord handle (a string from score_read detail)");
        }
        muse::RetVal<EngravingItem*> r = core::resolveHandle(score, v.toString());
        if (!r.ret) {
            return r.ret;
        }
        EngravingItem* e = r.val;
        if (e->isNote()) {
            if (has(p, "note") || has(p, "note_index") || has(p, "grace")) {
                return makeErr(Err::InvalidArg, "target names one note already; note, note_index and grace go with a "
                                                "Chord handle or staff + at");
            }
            out = toNote(e);
            return muse::make_ok();
        }
        if (e->isRest()) {
            return makeErr(Err::InvalidArg, "target names a rest at " + address::where(score, toChordRest(e))
                           + "; set_pitch changes an existing note (write_voice writes new ones)");
        }
        if (!e->isChord()) {
            return makeErr(Err::InvalidArg, std::string("target must name a note or chord; it names a ") + e->typeName());
        }
        chord = toChord(e);
    } else {
        if (!has(p, "staff") || !has(p, "at")) {
            return makeErr(Err::InvalidArg, "missing target: a Note handle, or staff + at {\"measure\": M, \"beat\": B} (+ voice)");
        }
        staff_idx_t staff = 0;
        if (Ret r = address::resolveStaff(score, p.at("staff"), staff); !r) {
            return r;
        }
        int voice = 1;
        if (has(p, "voice")) {
            const Val& v = p.at("voice");
            if (!isWhole(v) || v.toDouble() < 1 || v.toDouble() > 4) {
                return makeErr(Err::Range, "voice must be 1-4");
            }
            voice = static_cast<int>(v.toDouble());
        }
        if (p.at("at").type() == Val::Type::Map) {
            for (const auto& [k, _] : p.at("at").toMap()) {
                if (k != "measure" && k != "beat" && k != "offset") {
                    return makeErr(Err::InvalidArg, "at has an unknown key '" + k + "' (measure, beat or offset)");
                }
            }
        }
        Measure* m = nullptr;
        Fraction tick;
        if (Ret r = address::resolvePosition(score, p.at("at"), "at", m, tick); !r) {
            return r;
        }
        ChordRest* cr = nullptr;
        if (Ret r = address::chordRestAt(score, m, tick, staff * VOICES + static_cast<track_idx_t>(voice - 1), cr); !r) {
            return r;
        }
        if (!cr->isChord()) {
            return makeErr(Err::InvalidArg, "there is a rest at " + address::where(score, cr)
                           + ", not a note; set_pitch changes an existing note (write_voice writes new ones)");
        }
        chord = toChord(cr);
    }
    if (has(p, "grace")) {
        const Val& g = p.at("grace");
        if (!isWhole(g) || g.toDouble() < 1) {
            return makeErr(Err::InvalidArg, "grace must be a whole number from 1 (the first grace note before the note)");
        }
        if (chord->isGrace()) {
            return makeErr(Err::InvalidArg, "target is a grace note already; leave out grace");
        }
        const GraceNotesGroup& before = chord->graceNotesBefore();
        const int i = static_cast<int>(g.toDouble());
        if (i > static_cast<int>(before.size())) {
            return makeErr(Err::Range, "grace " + std::to_string(i) + ": the note at " + address::where(score, chord)
                           + " has " + std::to_string(before.size()) + " grace note(s) before it");
        }
        chord = before.at(static_cast<size_t>(i - 1));
    }
    return pickNote(score, chord, p, written, out);
}

int spellMidi(int midi, Key key, const std::string& spelling)
{
    // sharps/flats as Score::spellWithSharpsOrFlats (score.cpp:887-912); auto as Note::setNval's
    // key-aware default (note.cpp:3032-3035)
    if (spelling == "sharps") {
        return pitch2tpc(midi, Key::C, Prefer::SHARPS);
    }
    if (spelling == "flats") {
        return pitch2tpc(midi, Key::C, Prefer::FLATS);
    }
    return pitch2tpc(midi, key, Prefer::NEAREST);
}

nt::Pitch pitchFromTpc(int tpc, int midi)
{
    nt::Pitch p;
    p.step = static_cast<char>(tpc2stepName(tpc).unicode());
    p.alter = static_cast<int>(tpc2alter(tpc));
    p.octave = static_cast<int>(std::floor((midi - p.alter) / 12.0)) - 1;
    return p;
}

struct Fretting {
    int string = -1;
    int fret = -1;
};
}

// ------------------------------------------------------------------ names

const std::vector<std::string>& mu::agent::pitch::intervalNames()
{
    // Interval::allIntervals order (interval.cpp:124-159), as the Transpose dialog lists them
    static const std::vector<std::string> names = {
        "P1", "A1", "d2", "m2", "M2", "A2", "d3", "m3", "M3", "A3", "d4", "P4", "A4",
        "d5", "P5", "A5", "d6", "m6", "M6", "A6", "d7", "m7", "M7", "A7", "d8", "P8",
    };
    return names;
}

int mu::agent::pitch::intervalIndex(const std::string& name)
{
    const std::vector<std::string>& n = intervalNames();
    auto it = std::find(n.begin(), n.end(), name);
    return it == n.end() ? -1 : static_cast<int>(it - n.begin());
}

bool mu::agent::pitch::parsePitchName(const std::string& text, nt::Pitch& out, std::string* why)
{
    auto fail = [why](const std::string& w) {
        if (why) {
            *why = w;
        }
        return false;
    };
    if (text.empty()) {
        return fail("empty; give a pitch such as \"F#4\" (C4 = middle C)");
    }
    if (text[0] < 'A' || text[0] > 'G') {
        return fail("must start with a capital step letter A-G, e.g. \"Bb3\"");
    }
    nt::Pitch p;
    p.step = text[0];
    size_t i = 1;
    if (text.compare(i, 2, "##") == 0) {
        p.alter = 2;
        i += 2;
    } else if (text.compare(i, 2, "bb") == 0) {
        p.alter = -2;
        i += 2;
    } else if (i < text.size() && text[i] == '#') {
        p.alter = 1;
        ++i;
    } else if (i < text.size() && text[i] == 'b') {
        p.alter = -1;
        ++i;
    }
    const std::string oct = text.substr(i);
    if (oct == "-1") {
        p.octave = -1;
    } else if (oct.size() == 1 && oct[0] >= '0' && oct[0] <= '9') {
        p.octave = oct[0] - '0';
    } else {
        return fail("needs an octave -1 to 9 after the step and accidental (#, ##, b, bb), e.g. \"F#4\"");
    }
    if (p.midi() < 0 || p.midi() > 127) {
        return fail("is MIDI " + std::to_string(p.midi()) + ", outside 0-127 (C-1 to G9)");
    }
    out = p;
    return true;
}

// ------------------------------------------------------------------ set_pitch

Ret mu::agent::pitch::setPitch(const Params& p, MasterScore* score, Out& out)
{
    // ---- arguments
    bool written = false;
    if (has(p, "space")) {
        const Val& s = p.at("space");
        if (s.type() != Val::Type::String || (s.toString() != "sounding" && s.toString() != "written")) {
            return makeErr(Err::InvalidArg, "space must be \"sounding\" (default, as score_read prints) or \"written\"");
        }
        written = s.toString() == "written";
    }
    PitchArg pv;
    if (Ret r = readPitchArg(p, pv); !r) {
        return r;
    }
    std::string spelling = "auto";
    if (has(p, "spelling")) {
        const Val& s = p.at("spelling");
        if (s.type() != Val::Type::String || (s.toString() != "auto" && s.toString() != "sharps" && s.toString() != "flats")) {
            return makeErr(Err::InvalidArg, "spelling must be auto, sharps or flats");
        }
        if (pv.spelled) {
            return makeErr(Err::InvalidArg, "spelling goes with midi only; pitch \"" + p.at("pitch").toString()
                           + "\" is already spelled");
        }
        spelling = s.toString();
    }
    bool hasTab = false;
    Fretting wantTab;
    if (has(p, "tab")) {
        const Val& t = p.at("tab");
        if (t.type() != Val::Type::Map) {
            return makeErr(Err::InvalidArg, "tab must be {\"string\": S, \"fret\": F} (string 0 = the highest string)");
        }
        const muse::ValMap m = t.toMap();
        for (const auto& [k, _] : m) {
            if (k != "string" && k != "fret") {
                return makeErr(Err::InvalidArg, "tab has an unknown key '" + k + "' (string, fret)");
            }
        }
        if (!m.count("string") || !m.count("fret") || !isWhole(m.at("string")) || !isWhole(m.at("fret"))) {
            return makeErr(Err::InvalidArg, "tab needs whole numbers string (0 = the highest string) and fret");
        }
        wantTab.string = static_cast<int>(m.at("string").toDouble());
        wantTab.fret = static_cast<int>(m.at("fret").toDouble());
        hasTab = true;
    }

    Note* note = nullptr;
    if (Ret r = resolveNote(score, p, written, note); !r) {
        return r;
    }
    Staff* staff = note->staff();
    const Fraction tick = note->chord()->tick();
    const Instrument* instrument = staff->part()->instrument(tick);
    const bool drum = instrument->useDrumset(); // upDown's test (cmd.cpp:1951-1955)
    const bool tab = staff->isTabStaff(tick);

    if (hasTab && !tab) {
        return makeErr(Err::InvalidArg, "tab is for TAB staves; staff " + std::to_string(staff->idx() + 1) + " is not one");
    }

    // ---- the stored values (spec P1/P2; NOTATION.md section 2: sounding includes ottava)
    notes::Stored s;
    if (drum) {
        if (pv.spelled) {
            return makeErr(Err::InvalidArg, "staff " + std::to_string(staff->idx() + 1) + " is a drum staff: give the "
                           "drum's MIDI number as midi (e.g. 38 = acoustic snare), not a spelled pitch");
        }
        if (has(p, "space") || has(p, "spelling")) {
            return makeErr(Err::InvalidArg, "space and spelling do not apply on a drum staff");
        }
        const Drumset* ds = instrument->drumset();
        if (!ds || !ds->isValid(pv.midi)) {
            return makeErr(Err::InvalidArg, "MIDI " + std::to_string(pv.midi) + " is not a sound in this staff's drumset");
        }
        s.pitch = pv.midi;
        s.tpc1 = s.tpc2 = pitch2tpc(pv.midi, Key::C, Prefer::NEAREST); // as upDown (cmd.cpp:1963-1964)
    } else {
        nt::Pitch name = pv.name;
        if (!pv.spelled) {
            const Key key = written ? staff->key(tick) : staff->concertKey(tick);
            name = pitchFromTpc(spellMidi(pv.midi, key, spelling), pv.midi);
        }
        s = notes::storePitch(staff, name, tick, written);
    }
    if (!pitchIsValid(s.pitch)) {
        const std::string asked = pv.spelled ? p.at("pitch").toString() : "MIDI " + std::to_string(pv.midi);
        return makeErr(Err::InvalidArg, std::string(written ? "written " : "sounding ") + asked + " on staff "
                       + std::to_string(staff->idx() + 1) + " would be stored as MIDI " + std::to_string(s.pitch)
                       + " (concert pitch without the 8va/8vb line it sits under), outside 0-127");
    }
    for (int t : { s.tpc1, s.tpc2 }) {
        if (!tpcIsValid(t)) {
            return makeErr(Err::InvalidArg, "that pitch has no valid spelling in the other pitch view of staff "
                           + std::to_string(staff->idx() + 1));
        }
        if (std::abs(static_cast<int>(tpc2alter(t))) > 2) {
            return makeErr(Err::InvalidArg, "the " + std::string(t == s.tpc1 ? "sounding" : "written") + " spelling would be "
                           + spellingOf(t) + " (a triple accidental); respell the pitch");
        }
    }
    const int writtenPitch = s.pitch - staff->transpose(tick).chromatic;
    if ((((tpc2pitch(s.tpc1) - s.pitch) % 12) + 12) % 12 != 0 || (((tpc2pitch(s.tpc2) - writtenPitch) % 12) + 12) % 12 != 0) {
        return makeErr(Err::InvalidArg, "the spelling " + spellingOf(s.tpc1) + " does not match MIDI " + std::to_string(s.pitch));
    }

    // ---- the tie chain (upDown takes uniqueNotes() with tied = true, select.cpp:1602-1627)
    const std::vector<Note*> chain = note->tiedNotes();
    for (Note* t : chain) {
        if (t->bendFor() || t->bendBack()) {
            return makeErr(Err::InvalidArg, describe(score, t) + " is part of a guitar bend, whose amount comes from its "
                           "notes' pitches; change the bend instead");
        }
        if (t->staff() != staff) {
            return makeErr(Err::InvalidArg, describe(score, t) + " is tied from another staff; set_pitch does not follow it");
        }
    }

    // ---- TAB fretting (Q5: convertPitch + undoChangeFretting, as upDown's DIATONIC mode, cmd.cpp:2088-2091)
    std::vector<Fretting> frets(chain.size());
    if (tab) {
        for (size_t i = 0; i < chain.size(); ++i) {
            Note* t = chain[i];
            const Fraction tt = t->chord()->tick();
            const StringData* sd = staff->part()->stringData(tt, staff->idx());
            if (!sd || sd->strings() == 0) {
                return makeErr(Err::Refused, "staff " + std::to_string(staff->idx() + 1) + " is a TAB staff without strings");
            }
            if (hasTab) {
                if (wantTab.string < 0 || wantTab.string >= static_cast<int>(sd->strings())) {
                    return makeErr(Err::InvalidArg, "tab.string " + std::to_string(wantTab.string) + " is outside 0-"
                                   + std::to_string(sd->strings() - 1) + " (0 = the highest string)");
                }
                if (wantTab.fret < 0 || wantTab.fret > sd->frets()) {
                    return makeErr(Err::InvalidArg, "tab.fret " + std::to_string(wantTab.fret) + " is outside 0-"
                                   + std::to_string(sd->frets()));
                }
                // upDown's own check (cmd.cpp:2008)
                if (sd->getPitch(wantTab.string, wantTab.fret, staff, tt) != s.pitch + staff->pitchOffset(tt)) {
                    return makeErr(Err::InvalidArg, "string " + std::to_string(wantTab.string) + " fret " + std::to_string(wantTab.fret)
                                   + " plays MIDI " + std::to_string(sd->getPitch(wantTab.string, wantTab.fret, staff, tt))
                                   + ", not MIDI " + std::to_string(s.pitch));
                }
                frets[i] = wantTab;
            } else if (t->pitch() == s.pitch && t->string() >= 0 && t->string() < static_cast<int>(sd->strings()) && t->fret() >= 0
                       && sd->getPitch(t->string(), t->fret(), staff, tt) == s.pitch + staff->pitchOffset(tt)) {
                // same pitch (a respelling or a no-op): keep the fretting the note has
                frets[i] = { t->string(), t->fret() };
            } else {
                Fretting f;
                if (!sd->convertPitch(s.pitch, staff, tt, &f.string, &f.fret)) {
                    return makeErr(Err::InvalidArg, "MIDI " + std::to_string(s.pitch) + " cannot be fretted on staff "
                                   + std::to_string(staff->idx() + 1) + "'s strings");
                }
                frets[i] = f;
            }
        }
    }

    // ---- edit (upDown's last block, cmd.cpp:2072-2091; once per chain note, never on a linked copy)
    bool changed = false;
    std::vector<Note*> pitched;
    for (size_t i = 0; i < chain.size(); ++i) {
        Note* t = chain[i];
        if (t->pitch() != s.pitch || t->tpc1() != s.tpc1 || t->tpc2() != s.tpc2) {
            for (EngravingObject* e : t->linkList()) {
                Note* ln = toNote(e);
                if (ln->accidental()) {
                    score->doUndoRemoveElement(ln->accidental());
                }
            }
            score->undoChangePitch(t, s.pitch, s.tpc1, s.tpc2);
            if (tab) {
                score->undoChangeFretting(t, s.pitch, frets[i].string, frets[i].fret, s.tpc1, s.tpc2);
            }
            pitched.push_back(t);
            changed = true;
        } else if (tab && hasTab && (t->string() != frets[i].string || t->fret() != frets[i].fret)) {
            score->undoChangeFretting(t, s.pitch, frets[i].string, frets[i].fret, s.tpc1, s.tpc2);
            changed = true;
        }
    }

    // ---- post-condition (ChangePitch applies on push, spec P3: no layout needed)
    for (size_t i = 0; i < chain.size(); ++i) {
        Note* t = chain[i];
        for (EngravingObject* e : t->linkList()) {
            Note* ln = toNote(e);
            if (ln->pitch() != s.pitch || ln->tpc1() != s.tpc1 || ln->tpc2() != s.tpc2) {
                return makeErr(Err::PostCondition, describe(score, t) + ": a copy holds MIDI " + std::to_string(ln->pitch())
                               + " " + spellingOf(ln->tpc1()) + "/" + spellingOf(ln->tpc2()) + " after the edit, not "
                               + std::to_string(s.pitch) + " " + spellingOf(s.tpc1) + "/" + spellingOf(s.tpc2));
            }
        }
        if (tab && (t->string() != frets[i].string || t->fret() != frets[i].fret)) {
            return makeErr(Err::PostCondition, describe(score, t) + " is on string " + std::to_string(t->string()) + " fret "
                           + std::to_string(t->fret()) + ", not string " + std::to_string(frets[i].string) + " fret "
                           + std::to_string(frets[i].fret));
        }
    }
    for (Note* t : pitched) {
        for (EngravingObject* e : t->linkList()) {
            if (toNote(e)->accidental()) {
                return makeErr(Err::PostCondition, describe(score, t) + " still has its old accidental");
            }
        }
    }
    if (std::find(note->chord()->notes().begin(), note->chord()->notes().end(), note) == note->chord()->notes().end()) {
        return makeErr(Err::PostCondition, "the note is no longer in its chord");
    }

    // ---- result
    int fromBar = 0;
    int toBar = 0;
    for (Note* t : chain) {
        const int b = address::barOf(score, mainChord(t->chord())->measure());
        fromBar = fromBar ? std::min(fromBar, b) : b;
        toBar = std::max(toBar, b);
    }
    out["handle"] = Val(core::makeHandle(note));
    out["status"] = Val(std::string(changed ? "changed" : "unchanged"));
    out["pitch"] = Val(nt::writePitch(notes::printedPitch(note, false)));
    out["writtenPitch"] = Val(nt::writePitch(notes::printedPitch(note, true)));
    out["tiedNotes"] = Val(static_cast<int>(chain.size()));
    if (tab) {
        out["string"] = Val(frets.front().string);
        out["fret"] = Val(frets.front().fret);
    }
    out["bars"] = Val(barsText(fromBar, toBar));
    out["notation"] = Val(notationOf(score, fromBar, toBar, { static_cast<int>(staff->idx()) + 1 }));
    return muse::make_ok();
}

// ================================================================== transpose

namespace {
struct ByArgs {
    TransposeMode mode = TransposeMode::BY_INTERVAL;
    TransposeDirection dir = TransposeDirection::UP;
    int index = 0;        // BY_INTERVAL: allIntervals index; DIATONICALLY: degrees (1 = second)
    Key key = Key::C;     // TO_KEY
    int octaves = 0;      // BY_INTERVAL: extra P8 calls
    bool keepAlterations = true;
};

Ret readDirection(const muse::ValMap& m, const std::vector<std::string>& allowed, const char* def, TransposeDirection& out)
{
    std::string d = def ? def : "";
    if (m.count("direction")) {
        if (m.at("direction").type() != Val::Type::String) {
            return makeErr(Err::InvalidArg, "by.direction must be one of " + joined(allowed));
        }
        d = m.at("direction").toString();
    } else if (!def) {
        return makeErr(Err::InvalidArg, "by.direction is required (" + joined(allowed) + ")");
    }
    if (std::find(allowed.begin(), allowed.end(), d) == allowed.end()) {
        return makeErr(Err::InvalidArg, "by.direction \"" + d + "\" is not one of " + joined(allowed));
    }
    out = d == "up" ? TransposeDirection::UP : d == "down" ? TransposeDirection::DOWN : TransposeDirection::CLOSEST;
    return muse::make_ok();
}

Ret checkKeys(const muse::ValMap& m, const std::vector<std::string>& allowed, const std::string& form)
{
    for (const auto& [k, _] : m) {
        if (std::find(allowed.begin(), allowed.end(), k) == allowed.end()) {
            return makeErr(Err::InvalidArg, "by." + k + " does not go with by." + form + " (" + joined(allowed) + ")");
        }
    }
    return muse::make_ok();
}

Ret readBy(const Params& p, ByArgs& by)
{
    if (!has(p, "by")) {
        return makeErr(Err::InvalidArg, "missing by: {\"interval\": \"M2\", \"direction\": \"up\"}, {\"key\": -2} or "
                                        "{\"degrees\": 2, \"direction\": \"up\"}");
    }
    if (p.at("by").type() != Val::Type::Map) {
        return makeErr(Err::InvalidArg, "by must be an object: {interval, direction, octaves?}, {key, direction?} or "
                                        "{degrees, direction, keep_alterations?}");
    }
    const muse::ValMap m = p.at("by").toMap();
    const int forms = static_cast<int>(m.count("interval") + m.count("key") + m.count("degrees"));
    if (forms != 1) {
        return makeErr(Err::InvalidArg, "by takes exactly one of interval, key or degrees");
    }
    if (m.count("interval")) {
        if (Ret r = checkKeys(m, { "interval", "direction", "octaves" }, "interval"); !r) {
            return r;
        }
        const Val& iv = m.at("interval");
        const int idx = iv.type() == Val::Type::String ? pitch::intervalIndex(iv.toString()) : -1;
        if (idx < 0) {
            return makeErr(Err::InvalidArg, "by.interval \"" + iv.toString() + "\" is not one of " + joined(pitch::intervalNames())
                           + "; for compound intervals add octaves (a ninth is M2 + octaves 1)");
        }
        if (Ret r = readDirection(m, { "up", "down" }, nullptr, by.dir); !r) {
            return r;
        }
        if (m.count("octaves")) {
            const Val& o = m.at("octaves");
            if (!isWhole(o) || o.toDouble() < 0 || o.toDouble() > 3) {
                return makeErr(Err::InvalidArg, "by.octaves must be a whole number 0-3");
            }
            by.octaves = static_cast<int>(o.toDouble());
        }
        if (idx == 0 && by.octaves == 0) {
            return makeErr(Err::InvalidArg, "a perfect unison (P1) with no octaves changes nothing");
        }
        by.mode = TransposeMode::BY_INTERVAL;
        by.index = idx;
        return muse::make_ok();
    }
    if (m.count("key")) {
        if (Ret r = checkKeys(m, { "key", "direction" }, "key"); !r) {
            return r;
        }
        const Val& k = m.at("key");
        if (!isWhole(k) || k.toDouble() < -7 || k.toDouble() > 7) {
            return makeErr(Err::InvalidArg, "by.key must be a whole number -7 to 7: the concert key as sharps (+) or flats (-), "
                                            "e.g. -2 for B-flat major / G minor");
        }
        if (Ret r = readDirection(m, { "closest", "up", "down" }, "closest", by.dir); !r) {
            return r;
        }
        by.mode = TransposeMode::TO_KEY;
        by.key = Key(static_cast<int>(k.toDouble()));
        return muse::make_ok();
    }
    if (Ret r = checkKeys(m, { "degrees", "direction", "keep_alterations" }, "degrees"); !r) {
        return r;
    }
    const Val& d = m.at("degrees");
    if (!isWhole(d) || d.toDouble() < 1 || d.toDouble() > 6) {
        return makeErr(Err::InvalidArg, "by.degrees must be a whole number 1-6 (1 = a second, 6 = a seventh)");
    }
    if (Ret r = readDirection(m, { "up", "down" }, nullptr, by.dir); !r) {
        return r;
    }
    if (m.count("keep_alterations")) {
        if (m.at("keep_alterations").type() != Val::Type::Bool) {
            return makeErr(Err::InvalidArg, "by.keep_alterations must be true or false");
        }
        by.keepAlterations = m.at("keep_alterations").toBool();
    }
    by.mode = TransposeMode::DIATONICALLY;
    by.index = static_cast<int>(d.toDouble());
    return muse::make_ok();
}

Ret readBool(const Params& p, const char* key, bool def, bool& out)
{
    out = def;
    if (!has(p, key)) {
        return muse::make_ok();
    }
    if (p.at(key).type() != Val::Type::Bool) {
        return makeErr(Err::InvalidArg, std::string(key) + " must be true or false");
    }
    out = p.at(key).toBool();
    return muse::make_ok();
}

//! What the engine is given: a range (segments and contiguous staff blocks) or a list
struct Target {
    bool list = false;
    Segment* s1 = nullptr;                  // range: first segment (a ChordRest segment)
    Segment* s2 = nullptr;                  // range: the segment it stops before; null = end of score
    std::vector<staff_idx_t> staves;        // range: sorted, unique, links dropped
    std::vector<std::pair<staff_idx_t, staff_idx_t> > blocks; // [first, end) contiguous runs of staves
    std::vector<EngravingItem*> items;      // list: notes, harmonies, fret diagrams (uniqueElements order)
};

//! What one engine call will visit, by the engine's own iteration rules (transpose.cpp:104-271)
struct Visit {
    std::vector<Note*> notes;
    struct KeyAt {
        KeySig* ks = nullptr;
        bool startKey = false;
    };
    std::vector<KeyAt> keys;               // range only: key signatures the KeySig branch looks at
    std::vector<Harmony*> harmonies;       // chord symbols (incl. a fret diagram's own)
    std::vector<FretDiagram*> bareFrets;   // fret diagrams with no chord symbol (the non-undoable branch)
    Segment* last = nullptr;               // range: the last segment visited
};

void addChordNotes(Chord* chord, Visit& v)
{
    for (Note* n : chord->notes()) {
        v.notes.push_back(n);
    }
    for (Chord* g : chord->graceNotes()) {
        for (Note* n : g->notes()) {
            v.notes.push_back(n);
        }
    }
}

//! The range branch's iteration (transpose.cpp:147-271) with the filter at "everything"
Visit scanRange(MasterScore* score, Segment* s1, Segment* s2, staff_idx_t a, staff_idx_t b)
{
    Visit v;
    std::vector<Staff*> sl;
    for (staff_idx_t i = a; i < b; ++i) {
        Staff* s = score->staff(i);
        if (isPercussionStaff(s)) {
            continue;
        }
        bool there = false;
        for (Staff* s2x : sl) {
            if (s2x == s || (s2x->links() && s2x->links()->contains(s))) {
                there = true;
                break;
            }
        }
        if (!there) {
            sl.push_back(s);
        }
    }
    std::vector<track_idx_t> tracks;
    for (Staff* s : sl) {
        for (voice_idx_t i = 0; i < VOICES; ++i) {
            tracks.push_back(s->idx() * VOICES + i);
        }
    }
    if (!s1) {
        return v;
    }
    if (s1->measure()->isMMRest()) {
        s1 = score->tick2segment(s1->tick(), true, s1->segmentType(), false);
    }
    if (s1->rtick().isZero()) {
        s1 = s1->measure()->first();
    }
    for (Segment* seg = s1; seg && seg != s2; seg = seg->next1()) {
        if (!seg->enabled()) {
            continue;
        }
        v.last = seg;
        for (track_idx_t track : tracks) {
            if (score->staff(track / VOICES)->staffType(s1->tick())->group() == StaffGroup::PERCUSSION) {
                continue;
            }
            EngravingItem* e = seg->element(track);
            if (!e) {
                continue;
            }
            if (e->isChord()) {
                addChordNotes(toChord(e), v);
            } else if (e->isKeySig()) {
                KeySig* ks = toKeySig(e);
                const bool startKey = seg->tick() == s1->tick() && !isFirstSystemKeySig(ks);
                if ((startKey || ks->isChange()) && !ks->isAtonal()) {
                    v.keys.push_back({ ks, startKey });
                }
            }
        }
        for (EngravingItem* e : seg->annotations()) {
            if ((!e->isHarmony() && !e->isFretDiagram()) || std::find(tracks.begin(), tracks.end(), e->track()) == tracks.end()) {
                continue;
            }
            if (e->isHarmony()) {
                v.harmonies.push_back(toHarmony(e));
            } else if (Harmony* h = toFretDiagram(e)->harmony()) {
                v.harmonies.push_back(h);
            } else {
                v.bareFrets.push_back(toFretDiagram(e));
            }
        }
    }
    return v;
}

//! The list branch's iteration (transpose.cpp:104-145)
Visit scanList(const std::vector<EngravingItem*>& items)
{
    Visit v;
    for (EngravingItem* e : items) {
        if (!e->staff() || e->staff()->staffType(e->tick())->group() == StaffGroup::PERCUSSION) {
            continue;
        }
        if (e->staff()->primaryStaff() && e->staff()->primaryStaff()->staffType(Fraction(0, 1))->group() == StaffGroup::PERCUSSION) {
            continue;
        }
        if (e->isNote()) {
            v.notes.push_back(toNote(e));
        } else if (e->isHarmony()) {
            EngravingObject* parent = e->parent();
            if (parent && parent->isFretDiagram()) {
                if (std::find(items.begin(), items.end(), parent) != items.end()) {
                    continue;
                }
            }
            v.harmonies.push_back(toHarmony(e));
        } else if (e->isFretDiagram()) {
            if (Harmony* h = toFretDiagram(e)->harmony()) {
                v.harmonies.push_back(h);
            } else {
                v.bareFrets.push_back(toFretDiagram(e));
            }
        }
    }
    return v;
}

//! Transpose::keydiff2Interval (transpose.cpp:512-528; private upstream, so copied)
Interval keydiffInterval(Key oKey, Key nKey, TransposeDirection dir)
{
    int diatonic = tpc2step(key2Tpc(nKey)) - tpc2step(key2Tpc(oKey));
    int chromatic = ((int(nKey) - int(oKey)) + (diatonic * TPC_DELTA_ENHARMONIC)) / TPC_DELTA_SEMITONE;
    if ((dir == TransposeDirection::CLOSEST && chromatic > 6) || (dir == TransposeDirection::DOWN && chromatic > 0)) {
        diatonic -= 7;
        chromatic -= 12;
    } else if ((dir == TransposeDirection::CLOSEST && chromatic < -6) || (dir == TransposeDirection::UP && chromatic < 0)) {
        diatonic += 7;
        chromatic += 12;
    }
    return Interval(diatonic, chromatic);
}

std::string intervalText(Interval iv)
{
    const bool down = iv.chromatic < 0 || (iv.chromatic == 0 && iv.diatonic < 0);
    if (down) {
        iv.flip();
    }
    int octaves = 0;
    while (iv.chromatic > 12 || (iv.chromatic == 12 && iv.diatonic > 7)) {
        iv.chromatic -= 12;
        iv.diatonic -= 7;
        ++octaves;
    }
    std::string name = "(" + std::to_string(iv.diatonic) + " steps, " + std::to_string(iv.chromatic) + " semitones)";
    for (size_t i = 0; i < Interval::allIntervals.size(); ++i) {
        if (Interval::allIntervals[i] == iv) {
            name = pitch::intervalNames().at(i);
        }
    }
    if (octaves) {
        name += " + " + std::to_string(octaves) + " octave(s)";
    }
    return name + (down ? " down" : " up");
}

//! One engine call: mode, direction, interval index / degrees, key, and what it means for notes
struct Call {
    TransposeMode mode = TransposeMode::BY_INTERVAL;
    TransposeDirection dir = TransposeDirection::UP;
    Key key = Key::C;
    int index = 0;
    Interval interval;      // chromatic modes: the interval the engine will use
    bool noop = false;      // TO_KEY to the same key, closest: the engine returns at once (transpose.cpp:80-81)
    bool trKeys = false;    // as the engine will see them after its own overrides (transpose.cpp:91-98)
    bool chords = false;
};

struct NoteState {
    int pitch = 0;
    int tpc1 = 0;
    int tpc2 = 0;
};

//! Note::transpose (note.cpp:4238-4257) on a state; false when the engine would refuse the note
bool simulateChromatic(const Note* n, const Interval& iv, bool dbl, NoteState& st)
{
    const int npitch = st.pitch + iv.chromatic;
    if (!pitchIsValid(npitch)) {
        return false;
    }
    const int ntpc1 = Transpose::transposeTpc(st.tpc1, iv, dbl);
    int ntpc2 = ntpc1;
    if (n->transposition()) {
        Interval v = n->staff()->transpose(n->tick());
        v.flip();
        ntpc2 = Transpose::transposeTpc(ntpc1, v, dbl);
    }
    st = { npitch, ntpc1, ntpc2 };
    return true;
}

//! Note::transposeDiatonic (note.cpp:4194-4236) without the store; false when the result leaves
//! 0-127 (the engine would refuse at 4206 or silently octave-wrap at 4231: both are refused here)
bool simulateDiatonic(const Note* n, int steps, bool keepAlterations, bool dbl, NoteState& st)
{
    int alter = 0;
    const Fraction tick = n->chord()->segment()->tick();
    const Key key = n->staff() ? n->staff()->key(tick) : Key::C;
    const int absStep = pitch2absStepByKey(n->epitch(), n->tpc(), key, alter);
    int newPitch = absStep2pitchByKey(absStep + steps, key);
    int newTpc = step2tpcByKey((absStep + steps) % STEP_DELTA_OCTAVE, key);
    if (!pitchIsValid(newPitch)) {
        return false;
    }
    if (keepAlterations) {
        newPitch += alter;
        newTpc += alter * TPC_DELTA_SEMITONE;
    }
    int newTpc1 = Tpc::TPC_INVALID;
    int newTpc2 = Tpc::TPC_INVALID;
    Interval v = n->staff() ? n->staff()->transpose(tick) : Interval(0);
    if (n->concertPitch()) {
        v.flip();
        newTpc1 = clampEnharmonic(newTpc, dbl);
        newTpc2 = Transpose::transposeTpc(newTpc, v, dbl);
    } else {
        newPitch += v.chromatic;
        newTpc1 = Transpose::transposeTpc(newTpc, v, dbl);
        newTpc2 = clampEnharmonic(newTpc, dbl);
    }
    if (!pitchIsValid(newPitch)) {
        return false;
    }
    st = { newPitch, newTpc1, newTpc2 };
    return true;
}

bool runEngine(MasterScore* score, const Target& t, const std::pair<staff_idx_t, staff_idx_t>* block, const Call& c, bool dbl)
{
    score->deselectAll();
    if (t.list) {
        score->select(t.items, SelectType::ADD, 0);
    } else {
        // what Score::selectRange does for a chord or a bar (score.cpp:3548-3604), with our own ends
        score->selection().setRange(t.s1, t.s2, block->first, block->second);
        score->selection().updateSelectedElements();
    }
    const bool ok = Transpose::transpose(score, c.mode, c.dir, c.key, c.index, c.trKeys, c.chords, dbl);
    score->deselectAll();
    return ok;
}

//! transpose.cpp:626-629 returns with a temporary Harmony still attached to the diagram outside the
//! undo stack (spec, crash and corruption risks). Take it off the way the success path does
//! (transpose.cpp:633-634), so neither commit nor rollback keeps it.
void detachStrayHarmonies(const std::vector<FretDiagram*>& bare)
{
    for (FretDiagram* fd : bare) {
        if (Harmony* h = fd->harmony()) {
            fd->remove(h);
            delete h;
        }
    }
}

Ret resolveRangeEnds(MasterScore* score, const muse::ValMap& r, const std::vector<staff_idx_t>& staves, Target& t,
                     int& fromBar)
{
    for (const char* key : { "start", "end" }) {
        if (!r.count(key)) {
            return makeErr(Err::InvalidArg, std::string("range.") + key + " is required: {\"measure\": M, \"beat\": B}");
        }
        if (r.at(key).type() != Val::Type::Map) {
            return makeErr(Err::InvalidArg, std::string("range.") + key + " must be an object: {\"measure\": M, \"beat\": B}");
        }
        for (const auto& [k, _] : r.at(key).toMap()) {
            if (k != "measure" && k != "beat" && k != "offset") {
                return makeErr(Err::InvalidArg, std::string("range.") + key + " has an unknown key '" + k + "' (measure, beat or offset)");
            }
        }
    }
    auto onStaves = [&](Segment* seg, std::vector<ChordRest*>* crs) {
        bool any = false;
        for (staff_idx_t s : staves) {
            for (voice_idx_t v = 0; v < VOICES; ++v) {
                EngravingItem* e = seg->element(s * VOICES + v);
                if (e && e->isChordRest()) {
                    any = true;
                    if (crs) {
                        crs->push_back(toChordRest(e));
                    }
                }
            }
        }
        return any;
    };
    std::vector<std::string> staffNames;
    for (staff_idx_t s : staves) {
        staffNames.push_back(std::to_string(s + 1));
    }

    Measure* m1 = nullptr;
    Fraction tick1;
    if (Ret ret = address::resolvePosition(score, r.at("start"), "range.start", m1, tick1); !ret) {
        return ret;
    }
    Segment* s1 = m1->findSegment(SegmentType::ChordRest, tick1);
    if (!s1 || !onStaves(s1, nullptr)) {
        return makeErr(Err::Range, "range.start: no note or rest starts at m" + std::to_string(address::barOf(score, m1)) + " "
                       + address::beatOf(m1, tick1) + " on staff " + joined(staffNames));
    }

    Measure* m2 = nullptr;
    Fraction tick2;
    if (Ret ret = address::resolvePosition(score, r.at("end"), "range.end", m2, tick2); !ret) {
        return ret;
    }
    const muse::ValMap endPos = r.at("end").toMap();
    const bool wholeBar = !endPos.count("beat") && !endPos.count("offset");
    Segment* s2 = nullptr;
    Fraction endPosTick = tick2;
    if (wholeBar) {
        // Score::selectRange's measure branch (score.cpp:3548-3553)
        s2 = m2 == score->lastMeasure() ? nullptr : m2->last();
        endPosTick = m2->endTick();
    } else {
        Segment* seg = m2->findSegment(SegmentType::ChordRest, tick2);
        std::vector<ChordRest*> crs;
        if (!seg || !onStaves(seg, &crs)) {
            return makeErr(Err::Range, "range.end: no note or rest starts at m" + std::to_string(address::barOf(score, m2)) + " "
                           + address::beatOf(m2, tick2) + " on staff " + joined(staffNames)
                           + "; end names the last note or rest (inclusive), or give only its measure for the whole bar");
        }
        ChordRest* longest = crs.front();
        for (ChordRest* cr : crs) {
            if (cr->endTick() > longest->endTick()) {
                longest = cr;
            }
        }
        // Score::selectRange's chord branch (score.cpp:3599-3600)
        s2 = longest->nextSegmentAfterCR(SegmentType::ChordRest | SegmentType::EndBarLine | SegmentType::Clef);
    }
    if (endPosTick < s1->tick()) {
        return makeErr(Err::InvalidArg, "range.end (m" + std::to_string(address::barOf(score, m2)) + " " + address::beatOf(m2, tick2)
                       + ") is before range.start (m" + std::to_string(address::barOf(score, m1)) + " " + address::beatOf(m1, tick1) + ")");
    }
    t.s1 = s1;
    t.s2 = s2;
    fromBar = address::barOf(score, m1);
    return muse::make_ok();
}

Ret resolveTargetRange(MasterScore* score, const Params& p, Target& t)
{
    if (!has(p, "range")) {
        return makeErr(Err::InvalidArg, "missing range: {\"score\": true}, {\"start\": {measure, beat}, \"end\": {measure, beat}, "
                                        "\"staves\": [...]} or {\"handles\": [...]}");
    }
    const Val& v = p.at("range");
    if (v.type() != Val::Type::Map) {
        return makeErr(Err::InvalidArg, "range must be an object: {\"score\": true, \"staves\"?}, {\"start\", \"end\", \"staves\"?} "
                                        "or {\"handles\": [...]}");
    }
    const muse::ValMap r = v.toMap();
    for (const auto& [k, _] : r) {
        if (k != "start" && k != "end" && k != "staves" && k != "handles" && k != "score") {
            return makeErr(Err::InvalidArg, "range has an unknown key '" + k + "' (score, start, end, staves; or handles)");
        }
    }
    std::vector<staff_idx_t> staves;
    if (r.count("handles")) {
        if (r.size() != 1) {
            return makeErr(Err::InvalidArg, "range takes handles alone, not with score, start, end or staves");
        }
        if (r.at("handles").type() != Val::Type::List || r.at("handles").toList().empty()) {
            return makeErr(Err::InvalidArg, "range.handles must be a non-empty list of Note, Chord or chord-symbol handles");
        }
        t.list = true;
        std::vector<EngravingItem*> items;
        for (const Val& h : r.at("handles").toList()) {
            if (h.type() != Val::Type::String) {
                return makeErr(Err::InvalidArg, "range.handles must hold handle strings");
            }
            muse::RetVal<EngravingItem*> rr = core::resolveHandle(score, h.toString());
            if (!rr.ret) {
                return rr.ret;
            }
            EngravingItem* e = rr.val;
            if (e->isChord()) {
                for (Note* n : toChord(e)->notes()) {
                    items.push_back(n);
                }
            } else if (e->isNote() || e->isHarmony() || e->isFretDiagram()) {
                items.push_back(e);
            } else {
                return makeErr(Err::InvalidArg, "range.handles: " + h.toString() + " is a " + e->typeName()
                               + "; transpose takes notes, chords, chord symbols and fret diagrams");
            }
        }
        // Selection::uniqueElements: one of a set of linked elements (select.cpp:1576-1592)
        for (EngravingItem* e : items) {
            bool there = false;
            for (EngravingItem* x : t.items) {
                if (x == e || (x->links() && x->links()->contains(e))) {
                    there = true;
                    break;
                }
            }
            if (!there) {
                t.items.push_back(e);
            }
        }
        return muse::make_ok();
    }
    if (r.count("staves")) {
        if (r.at("staves").type() != Val::Type::List || r.at("staves").toList().empty()) {
            return makeErr(Err::InvalidArg, "range.staves must be a non-empty list of staves (3, {\"number\": 3} or "
                                            "{\"part\": \"Flute\", \"index\": 1}); leave it out for every staff");
        }
        for (const Val& s : r.at("staves").toList()) {
            staff_idx_t idx = 0;
            if (Ret ret = address::resolveStaff(score, s, idx); !ret) {
                return ret;
            }
            staves.push_back(idx);
        }
    } else {
        for (staff_idx_t i = 0; i < score->nstaves(); ++i) {
            staves.push_back(i);
        }
    }
    std::sort(staves.begin(), staves.end());
    staves.erase(std::unique(staves.begin(), staves.end()), staves.end());
    if (r.count("score")) {
        if (r.at("score").type() != Val::Type::Bool || !r.at("score").toBool()) {
            return makeErr(Err::InvalidArg, "range.score must be true (the whole score); give start and end for part of it");
        }
        if (r.count("start") || r.count("end")) {
            return makeErr(Err::InvalidArg, "range takes score or start/end, not both");
        }
        Measure* first = score->firstMeasure();
        t.s1 = first ? first->first(SegmentType::ChordRest) : nullptr;
        t.s2 = nullptr;
        if (!t.s1) {
            return makeErr(Err::Range, "the score has no notes or rests");
        }
    } else {
        int fromBar = 0;
        if (Ret ret = resolveRangeEnds(score, r, staves, t, fromBar); !ret) {
            return ret;
        }
    }

    // a staff linked to one already in the range would be transposed twice across blocks
    // (the engine only de-duplicates inside one call, transpose.cpp:166-175)
    for (staff_idx_t s : staves) {
        bool linked = false;
        for (staff_idx_t k : t.staves) {
            if (score->staff(k)->isLinked(score->staff(s))) {
                linked = true;
                break;
            }
        }
        if (!linked) {
            t.staves.push_back(s);
        }
    }
    for (staff_idx_t s : t.staves) {
        if (!t.blocks.empty() && t.blocks.back().second == s) {
            t.blocks.back().second = s + 1;
        } else {
            t.blocks.push_back({ s, s + 1 });
        }
    }
    return muse::make_ok();
}

//! Earliest and latest beat (segment) of the notes in a tie chain: their main chords
void chainSpan(const std::vector<Note*>& chain, ChordRest*& first, ChordRest*& last)
{
    first = last = nullptr;
    for (Note* n : chain) {
        Chord* c = mainChord(n->chord());
        if (!first || c->tick() < first->tick()) {
            first = c;
        }
        if (!last || c->endTick() > last->endTick()) {
            last = c;
        }
    }
}
}

Ret mu::agent::pitch::transpose(const Params& p, MasterScore* score, Out& out)
{
    // ---- arguments
    ByArgs by;
    if (Ret r = readBy(p, by); !r) {
        return r;
    }
    bool trKeys = true;
    bool chords = true;
    bool dbl = true;
    if (Ret r = readBool(p, "key_signatures", true, trKeys); !r) {
        return r;
    }
    if (Ret r = readBool(p, "chord_symbols", true, chords); !r) {
        return r;
    }
    if (Ret r = readBool(p, "double_accidentals", true, dbl); !r) {
        return r;
    }
    bool extendTies = false;
    if (has(p, "ties")) {
        const Val& t = p.at("ties");
        if (t.type() != Val::Type::String || (t.toString() != "reject" && t.toString() != "extend")) {
            return makeErr(Err::InvalidArg, "ties must be reject (default) or extend");
        }
        extendTies = t.toString() == "extend";
    }
    if (by.mode == TransposeMode::DIATONICALLY && has(p, "key_signatures") && trKeys) {
        return makeErr(Err::InvalidArg, "a diatonic transposition (degrees) keeps the key signatures; key_signatures applies to "
                                        "interval and key (keep_alterations is the diatonic option)");
    }

    Target t;
    if (Ret r = resolveTargetRange(score, p, t); !r) {
        return r;
    }
    if (t.list) {
        if (by.mode == TransposeMode::TO_KEY) {
            return makeErr(Err::InvalidArg, "by.key needs a range (bars or \"score\"), not a list of handles: the interval comes from "
                                            "the range's key");
        }
        if (has(p, "key_signatures") && trKeys) {
            return makeErr(Err::InvalidArg, "key_signatures needs a range (bars or \"score\"), not a list of handles");
        }
        trKeys = false;
    }
    if (by.mode == TransposeMode::DIATONICALLY) {
        trKeys = by.keepAlterations; // the dialog's "keep degree alterations" goes in the same flag (transposedialog.cpp:231-236)
    }

    // ---- what the engine will visit; ties at the edges (spec: the engine does not follow ties)
    auto scanAll = [&](std::vector<Visit>& visits) {
        visits.clear();
        if (t.list) {
            visits.push_back(scanList(t.items));
        } else {
            for (const auto& b : t.blocks) {
                visits.push_back(scanRange(score, t.s1, t.s2, b.first, b.second));
            }
        }
    };
    std::vector<Visit> visits;
    bool extended = false;
    for (int round = 0;; ++round) {
        scanAll(visits);
        std::unordered_set<const Note*> inside;
        for (const Visit& v : visits) {
            inside.insert(v.notes.begin(), v.notes.end());
        }
        Note* inner = nullptr;
        Note* outer = nullptr;
        for (const Visit& v : visits) {
            for (Note* n : v.notes) {
                for (Note* x : n->tiedNotes()) {
                    if (!inside.count(x)) {
                        inner = n;
                        outer = x;
                        break;
                    }
                }
                if (outer) {
                    break;
                }
            }
            if (outer) {
                break;
            }
        }
        if (!outer) {
            break;
        }
        if (!extendTies) {
            return makeErr(Err::InvalidArg, describe(score, inner) + " is tied to " + describe(score, outer)
                           + ", outside what is transposed; include the whole tie chain, or pass ties: \"extend\"");
        }
        if (round >= 32) {
            return makeErr(Err::Internal, "could not widen the range over its tie chains");
        }
        extended = true;
        const std::vector<Note*> chain = inner->tiedNotes();
        // tiedNotes() follows partial ties through repeats and voltas (note.cpp:3865-3909): such a
        // chain is not one stretch of bars, and widening the range to cover it would also take every
        // bar in between. Refused; the caller names those bars itself.
        for (Note* x : chain) {
            // also a full tie whose jump points lead elsewhere (around a segno, note.cpp:3872-3884)
            bool jumps = false;
            if (x->tieFor() && x->tieFor()->tieJumpPoints()) {
                for (const TieJumpPoint* jp : *x->tieFor()->tieJumpPoints()) {
                    jumps = jumps || jp->active();
                }
            }
            if (jumps || (x->tieFor() && x->tieFor()->isPartialTie()) || (x->tieBack() && x->tieBack()->isPartialTie())) {
                return makeErr(Err::InvalidArg, "ties extend: the tie chain of " + describe(score, inner)
                               + " runs through a repeat or volta (partial ties), so it is not one stretch of bars; "
                                 "transpose those bars or notes explicitly");
            }
        }
        if (t.list) {
            for (Note* x : chain) {
                if (std::find(t.items.begin(), t.items.end(), x) == t.items.end()) {
                    t.items.push_back(x);
                }
            }
            continue;
        }
        ChordRest* first = nullptr;
        ChordRest* last = nullptr;
        chainSpan(chain, first, last);
        if (first->tick() < t.s1->tick()) {
            t.s1 = first->segment();
        }
        const Fraction endNow = t.s2 ? t.s2->tick() : score->endTick();
        if (last->endTick() > endNow) {
            t.s2 = last->nextSegmentAfterCR(SegmentType::ChordRest | SegmentType::EndBarLine | SegmentType::Clef);
        }
    }

    // ---- the calls, per block (or once for a list), with the engine's own option overrides
    const bool range = !t.list;
    auto makeCall = [&](TransposeMode mode, int index, Key fromKey) {
        Call c;
        c.mode = mode;
        c.dir = by.dir;
        c.key = by.key;
        c.index = index;
        c.trKeys = trKeys;
        c.chords = chords;
        if (mode == TransposeMode::DIATONICALLY) {
            return c;
        }
        if (mode == TransposeMode::TO_KEY) {
            if (fromKey == by.key) {
                if (by.dir == TransposeDirection::CLOSEST) {
                    c.noop = true;
                    return c;
                }
                c.interval = by.dir == TransposeDirection::UP ? Interval(7, 12) : Interval(-7, -12);
            } else {
                c.interval = keydiffInterval(fromKey, by.key, by.dir);
            }
        } else {
            c.interval = Interval::allIntervals[static_cast<size_t>(index)];
            if (by.dir == TransposeDirection::DOWN) {
                c.interval.flip();
            }
        }
        if (!range) {
            c.trKeys = false;
        }
        if ((c.interval.chromatic % PITCH_DELTA_OCTAVE) == 0 && mode != TransposeMode::TO_KEY) {
            c.trKeys = false;
            c.chords = false;
        }
        return c;
    };
    std::vector<std::vector<Call> > calls(visits.size());
    for (size_t i = 0; i < visits.size(); ++i) {
        Key fromKey = Key::C;
        if (by.mode == TransposeMode::TO_KEY) {
            // the key of the block's first pitched staff at the range start (transpose.cpp:60-73)
            const Fraction startTick = t.s1->tick();
            for (staff_idx_t s = t.blocks[i].first; s < t.blocks[i].second; ++s) {
                if (score->staff(s)->isPitchedStaff(startTick)) {
                    fromKey = score->staff(s)->concertKey(startTick);
                    break;
                }
            }
            calls[i].push_back(makeCall(TransposeMode::TO_KEY, 0, fromKey));
        } else if (by.mode == TransposeMode::DIATONICALLY) {
            calls[i].push_back(makeCall(TransposeMode::DIATONICALLY, by.index, fromKey));
        } else {
            if (by.index != 0) {
                calls[i].push_back(makeCall(TransposeMode::BY_INTERVAL, by.index, fromKey));
            }
            for (int o = 0; o < by.octaves; ++o) {
                calls[i].push_back(makeCall(TransposeMode::BY_INTERVAL, 25, fromKey));
            }
        }
    }

    bool allNoop = true;
    for (const auto& cs : calls) {
        for (const Call& c : cs) {
            allNoop = allNoop && c.noop;
        }
    }
    const int fromBar = t.list ? 0 : address::barOf(score, t.s1->measure());
    if (allNoop) {
        out["status"] = Val(std::string("unchanged"));
        out["interval"] = Val(std::string("none: the range is already in that key"));
        out["notes"] = Val(0);
        out["chordSymbols"] = Val(0);
        out["keySignatures"] = Val(0);
        return muse::make_ok();
    }

    // ---- predictions and the range check (the engine leaves a partial edit or octave-wraps)
    std::vector<std::pair<Note*, NoteState> > expect;
    struct KeyExpect {
        Staff* staff = nullptr;
        Fraction tick;
        Key key = Key::C;
    };
    std::vector<KeyExpect> keyExpect;
    struct HarmonyExpect {
        Harmony* h = nullptr;
        int root = 0;
        bool moves = false;
    };
    std::vector<HarmonyExpect> harmonyExpect;
    std::vector<FretDiagram*> bareFrets;
    size_t noteCount = 0;
    std::set<const Harmony*> harmoniesSeen;
    for (size_t i = 0; i < visits.size(); ++i) {
        const Visit& v = visits[i];
        for (Note* n : v.notes) {
            NoteState st { n->pitch(), n->tpc1(), n->tpc2() };
            for (const Call& c : calls[i]) {
                if (c.noop) {
                    continue;
                }
                const bool ok = c.mode == TransposeMode::DIATONICALLY
                                ? simulateDiatonic(n, by.dir == TransposeDirection::DOWN ? -c.index : c.index, by.keepAlterations, dbl, st)
                                : simulateChromatic(n, c.interval, dbl, st);
                if (!ok) {
                    return makeErr(Err::InvalidArg, describe(score, n) + " would go outside MIDI 0-127 ("
                                   + (c.mode == TransposeMode::DIATONICALLY ? std::to_string(c.index) + " degree(s) "
                                      + (by.dir == TransposeDirection::DOWN ? "down" : "up") : intervalText(c.interval))
                                   + "); nothing was changed");
                }
            }
            expect.push_back({ n, st });
            ++noteCount;
        }
        for (const Call& c : calls[i]) {
            if (c.noop) {
                continue;
            }
            if (c.trKeys && c.mode != TransposeMode::DIATONICALLY) {
                for (const Visit::KeyAt& k : v.keys) {
                    const KeySigEvent& ke = k.ks->keySigEvent();
                    const Key nKey = (!ke.forInstrumentChange() || k.startKey) ? Transpose::transposeKey(k.ks->concertKey(), c.interval)
                                     : k.ks->concertKey();
                    keyExpect.push_back({ k.ks->staff(), k.ks->tick(), nKey });
                }
            }
            if (c.chords) {
                for (Harmony* h : v.harmonies) {
                    HarmonyExpect he;
                    he.h = h;
                    he.root = h->rootTpc();
                    if (c.mode == TransposeMode::DIATONICALLY) {
                        he.moves = tpcIsValid(he.root);
                    } else {
                        // transposeHarmony's interval (transpose.cpp:547-549)
                        const Interval kv = h->staff()->transpose(h->tick());
                        const Interval iv = h->part()->instrument(h->tick())->transpose();
                        const Interval hi(c.interval.diatonic - kv.diatonic + iv.diatonic, c.interval.chromatic - kv.chromatic + iv.chromatic);
                        he.moves = tpcIsValid(he.root) && Transpose::transposeTpc(he.root, hi, dbl) != he.root;
                    }
                    if (harmoniesSeen.insert(h).second) {
                        harmonyExpect.push_back(he);
                    }
                }
                bareFrets.insert(bareFrets.end(), v.bareFrets.begin(), v.bareFrets.end());
            }
        }
    }
    if (t.list) {
        for (EngravingItem* e : t.items) {
            if ((e->isHarmony() || e->isFretDiagram()) && !chords) {
                return makeErr(Err::InvalidArg, "range.handles names chord symbols or fret diagrams but chord_symbols is false");
            }
        }
    }
    if (expect.empty() && keyExpect.empty() && harmonyExpect.empty() && bareFrets.empty()) {
        const bool allPerc = std::all_of(t.staves.begin(), t.staves.end(), [&](staff_idx_t s) {
            return isPercussionStaff(score->staff(s));
        });
        return makeErr(Err::InvalidArg, allPerc && !t.list
                       ? std::string("nothing to transpose: percussion staves are never transposed")
                       : std::string("nothing to transpose there: no pitched notes, chord symbols or key signatures"));
    }

    // ---- edit: the engine, once per call (P8 calls for octaves), the filter at "everything"
    {
        FilterOverride filter(score);
        for (size_t i = 0; i < calls.size(); ++i) {
            for (const Call& c : calls[i]) {
                if (c.noop) {
                    continue;
                }
                const bool ok = runEngine(score, t, t.list ? nullptr : &t.blocks[i], c, dbl);
                detachStrayHarmonies(bareFrets);
                if (!ok) {
                    return makeErr(Err::PostCondition, "MuseScore reported a note it could not transpose; rolled back");
                }
            }
        }
    }

    // ---- post-conditions
    // Pitch and tpc1 are checked exactly against the prediction. tpc2 is not predictable before the
    // call in chromatic modes: a key change in the range makes undoChangeKeySig run
    // Transpose::transpositionChanged (edit.cpp:6078-6084, transpose.cpp:403-472), which respells
    // tpc2 from tpc1 with the staff's new, possibly enharmonically adjusted, transposition
    // (Staff::transpose, staff.cpp:861-891). So tpc2 must be what either writer derives from the
    // live staff: Note::transpose's (note.cpp:4246-4253, double accidentals as asked) or
    // transpositionChanged's (always allowing double accidentals). Within one engine call
    // Note::transpose writes last (a key signature is changed before the chords at or after its
    // tick, transpose.cpp:198-252), so transpositionChanged's spelling survives only when a later
    // call changes a key in the note's part: only then is the second form accepted.
    auto laterKeyChangeInPart = [&](const Note* master) {
        if (dbl || t.list || keyExpect.empty()) {
            return false; // with double accidentals both forms are the same
        }
        const staff_idx_t si = master->staffIdx();
        size_t mine = t.blocks.size();
        for (size_t i = 0; i < t.blocks.size(); ++i) {
            if (si >= t.blocks[i].first && si < t.blocks[i].second) {
                mine = i;
            }
        }
        for (size_t j = mine + 1; j < t.blocks.size(); ++j) {
            for (const Staff* ps : master->part()->staves()) {
                if (ps->idx() >= t.blocks[j].first && ps->idx() < t.blocks[j].second) {
                    return true;
                }
            }
        }
        return false;
    };
    auto tpc2Ok = [dbl](const Note* ln, bool alsoTranspositionChanged) {
        if (!ln->transposition()) {
            return ln->tpc2() == ln->tpc1();
        }
        Interval v = ln->staff()->transpose(ln->tick());
        v.flip();
        return ln->tpc2() == Transpose::transposeTpc(ln->tpc1(), v, dbl)
               || (alsoTranspositionChanged && ln->tpc2() == Transpose::transposeTpc(ln->tpc1(), v, true));
    };
    const bool diatonic = by.mode == TransposeMode::DIATONICALLY;
    for (const auto& [n, st] : expect) {
        const bool later = laterKeyChangeInPart(n);
        for (EngravingObject* e : n->linkList()) {
            const Note* ln = toNote(e);
            const bool ok = ln->pitch() == st.pitch && ln->tpc1() == st.tpc1 && (diatonic ? ln->tpc2() == st.tpc2 : tpc2Ok(ln, later));
            if (!ok) {
                return makeErr(Err::PostCondition, describe(score, n) + ": a copy holds MIDI " + std::to_string(ln->pitch()) + " "
                               + spellingOf(ln->tpc1()) + "/" + spellingOf(ln->tpc2()) + ", expected " + std::to_string(st.pitch) + " "
                               + spellingOf(st.tpc1) + (diatonic ? "/" + spellingOf(st.tpc2) : std::string(" and the staff's written spelling")));
            }
        }
    }
    for (const KeyExpect& k : keyExpect) {
        if (k.staff->keySigEvent(k.tick).concertKey() != k.key) {
            return makeErr(Err::PostCondition, "the key signature of staff " + std::to_string(k.staff->idx() + 1) + " at m"
                           + std::to_string(address::barOf(score, score->tick2measure(k.tick))) + " is concert "
                           + std::to_string(int(k.staff->keySigEvent(k.tick).concertKey())) + ", expected "
                           + std::to_string(int(k.key)));
        }
    }
    for (const HarmonyExpect& he : harmonyExpect) {
        if (he.moves && he.h->rootTpc() == he.root) {
            return makeErr(Err::PostCondition, "a chord symbol (" + he.h->plainText().toStdString() + ") did not move");
        }
    }

    // ---- result
    std::string intervalDesc;
    if (by.mode == TransposeMode::DIATONICALLY) {
        intervalDesc = std::to_string(by.index) + (by.index == 1 ? " degree " : " degrees ")
                       + (by.dir == TransposeDirection::DOWN ? "down" : "up") + (by.keepAlterations ? ", alterations kept" : "");
    } else {
        Interval total;
        for (const Call& c : calls.front()) {
            if (!c.noop) {
                total.chromatic = static_cast<int8_t>(total.chromatic + c.interval.chromatic);
                total.diatonic = static_cast<int8_t>(total.diatonic + c.interval.diatonic);
            }
        }
        intervalDesc = intervalText(total);
    }
    out["status"] = Val(std::string("transposed"));
    out["interval"] = Val(intervalDesc);
    out["notes"] = Val(static_cast<int>(noteCount));
    out["chordSymbols"] = Val(static_cast<int>(harmonyExpect.size()));
    out["keySignatures"] = Val(static_cast<int>(keyExpect.size()));
    out["extendedForTies"] = Val(extended);

    std::vector<int> staves;
    int barFrom = 0;
    int barTo = 0;
    if (t.list) {
        for (EngravingItem* e : t.items) {
            const Measure* m = nullptr;
            if (e->isNote()) {
                m = mainChord(toNote(e)->chord())->measure();
            } else if (e->findMeasure()) {
                m = e->findMeasure();
            }
            if (!m) {
                continue;
            }
            const int b = address::barOf(score, m);
            barFrom = barFrom ? std::min(barFrom, b) : b;
            barTo = std::max(barTo, b);
            staves.push_back(static_cast<int>(e->staffIdx()) + 1);
        }
    } else {
        barFrom = address::barOf(score, t.s1->measure());
        const Segment* last = nullptr;
        for (const Visit& v : visits) {
            if (v.last && (!last || v.last->tick() > last->tick())) {
                last = v.last;
            }
        }
        barTo = last ? address::barOf(score, last->measure()) : barFrom;
        for (staff_idx_t s : t.staves) {
            if (!isPercussionStaff(score->staff(s))) {
                staves.push_back(static_cast<int>(s) + 1);
            }
        }
    }
    (void)fromBar;
    if (barFrom && !staves.empty()) {
        out["bars"] = Val(barsText(barFrom, barTo));
        out["notation"] = Val(notationOf(score, barFrom, barTo, staves));
        if (barTo - barFrom + 1 > MAX_NOTATION_BARS) {
            out["notationBars"] = Val(barsText(barFrom, barFrom + MAX_NOTATION_BARS - 1));
        }
    }
    return muse::make_ok();
}
