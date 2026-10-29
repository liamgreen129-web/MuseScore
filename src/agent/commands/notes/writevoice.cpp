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
#include "writevoice.h"

#include "notehelpers.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>

#include "engraving/dom/chord.h"
#include "engraving/dom/clef.h"
#include "engraving/dom/durationtype.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/input.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/selectionfilter.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/textbase.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/tuplet.h"
#include "engraving/editing/transpose.h"

#include "notation/parser.h"
#include "notation/validate.h"
#include "notation/writer.h"
#include "read/scoreprinter.h"
#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::agent::notes;
using namespace mu::engraving;
using muse::Ret;
namespace nt = mu::agent::notation;

namespace {
//! One (bar, voice) line of the content, matched to the score.
struct Target {
    const nt::VoiceLine* line = nullptr;
    Measure* measure = nullptr;
    int bar = 0;
    int voice = 1;
    track_idx_t track = 0;
};

//! An event as written: where its chord ended up, for ties and grace notes afterwards.
struct Written {
    const nt::Event* event = nullptr;
    const Target* target = nullptr;
    Chord* chord = nullptr; // null for rests
    bool lastInLine = false;
};

struct Ctx {
    MasterScore* score = nullptr;
    Staff* staff = nullptr;
    staff_idx_t staffIdx = 0;
    bool written = false; // pitch mode
};

Fraction frac(const nt::Rational& r)
{
    return Fraction(static_cast<int>(r.num), static_cast<int>(r.den));
}

nt::Rational rat(const Fraction& f)
{
    return nt::Rational(f.numerator(), f.denominator());
}

DurationType durType(nt::BaseDuration b)
{
    switch (b) {
    case nt::BaseDuration::Breve: return DurationType::V_BREVE;
    case nt::BaseDuration::Whole: return DurationType::V_WHOLE;
    case nt::BaseDuration::Half: return DurationType::V_HALF;
    case nt::BaseDuration::Quarter: return DurationType::V_QUARTER;
    case nt::BaseDuration::Eighth: return DurationType::V_EIGHTH;
    case nt::BaseDuration::Sixteenth: return DurationType::V_16TH;
    case nt::BaseDuration::ThirtySecond: return DurationType::V_32ND;
    case nt::BaseDuration::SixtyFourth: return DurationType::V_64TH;
    case nt::BaseDuration::OneTwentyEighth: return DurationType::V_128TH;
    }
    return DurationType::V_QUARTER;
}

TDuration tdur(const nt::Duration& d)
{
    TDuration t(durType(d.base));
    t.setDots(d.dots);
    return t;
}

std::string where(int bar, int voice)
{
    return "m" + std::to_string(bar) + " v" + std::to_string(voice);
}

Stored store(const Ctx& c, const nt::Pitch& p, const Fraction& tick)
{
    return storePitch(c.staff, p, tick, c.written);
}

NoteVal noteVal(const Ctx& c, const nt::Pitch& p, const Fraction& tick)
{
    const Stored s = store(c, p, tick);
    NoteVal nv(s.pitch);
    // one spelling given, the other derived by Note::setNval exactly as store() computes it
    if (c.written) {
        nv.tpc2 = s.tpc2;
    } else {
        nv.tpc1 = s.tpc1;
    }
    return nv;
}

//! Grace types by duration (Score::cmdAddGrace, notationinteraction.cpp:5772).
std::optional<NoteType> graceType(nt::GraceKind kind, bool after, const nt::Duration& d)
{
    if (d.dots) {
        return std::nullopt;
    }
    if (after) {
        switch (d.base) {
        case nt::BaseDuration::Eighth: return NoteType::GRACE8_AFTER;
        case nt::BaseDuration::Sixteenth: return NoteType::GRACE16_AFTER;
        case nt::BaseDuration::ThirtySecond: return NoteType::GRACE32_AFTER;
        default: return std::nullopt;
        }
    }
    if (kind == nt::GraceKind::Acciaccatura) { // slashed; imports also give it 16th and 32nd values
        const bool ok = d.base == nt::BaseDuration::Eighth || d.base == nt::BaseDuration::Sixteenth
                        || d.base == nt::BaseDuration::ThirtySecond;
        return ok ? std::optional<NoteType>(NoteType::ACCIACCATURA) : std::nullopt;
    }
    switch (d.base) {
    case nt::BaseDuration::Quarter: return NoteType::GRACE4;
    case nt::BaseDuration::Eighth: return NoteType::APPOGGIATURA;
    case nt::BaseDuration::Sixteenth: return NoteType::GRACE16;
    case nt::BaseDuration::ThirtySecond: return NoteType::GRACE32;
    default: return std::nullopt;
    }
}

//! The written length of a tuplet's placeholder chord/rest: its contents scaled by normal/actual.
Fraction tupletTotal(const nt::Tuplet& t, const nt::Rational& barLen)
{
    return frac(nt::itemsDuration(t.items, barLen) * nt::Rational(t.normal, t.actual));
}

// --- validation (nothing has changed yet)

struct Checker {
    const Ctx& c;
    std::vector<std::string> problems;

    void add(const std::string& s) { problems.push_back(s); }

    void pitchInRange(const nt::Pitch& p, const Fraction& tick, const std::string& at)
    {
        const int sp = store(c, p, tick).pitch;
        if (sp < 0 || sp > 127) {
            add(at + ": " + nt::writePitch(p) + " is outside MIDI pitches 0-127");
        }
    }

    void graces(const nt::Event& e, const Fraction& tick, const std::string& at)
    {
        const bool any = !e.graceBefore.empty() || !e.graceAfter.empty();
        if (!any) {
            return;
        }
        if (e.kind != nt::EventKind::Note && e.kind != nt::EventKind::Chord) {
            add(at + ": grace notes need a note or chord to attach to, not a rest");
            return;
        }
        auto group = [&](const std::vector<nt::Event>& gs, bool after) {
            for (const nt::Event& g : gs) {
                if (g.kind != nt::EventKind::Note && g.kind != nt::EventKind::Chord) {
                    add(at + ": a grace group may hold only notes and chords");
                    continue;
                }
                if (!graceType(e.graceBeforeKind, after, g.duration)) {
                    add(at + ": grace notes " + std::string(after ? "after a note" : (e.graceBeforeKind == nt::GraceKind::Acciaccatura
                                                                                     ? "with a slash (g/)" : "before a note"))
                        + " can be " + (after || e.graceBeforeKind == nt::GraceKind::Acciaccatura ? "8, 16 or 32" : "4, 8, 16 or 32")
                        + ", undotted, not "
                        + nt::writeDuration(g.duration));
                }
                for (const nt::NoteHead& h : g.heads) {
                    if (h.tie) {
                        add(at + ": ties on grace notes are not supported");
                    }
                    pitchInRange(h.pitch, tick, at);
                }
                if (!g.graceBefore.empty() || !g.graceAfter.empty()) {
                    add(at + ": grace notes cannot have grace notes");
                }
            }
        };
        group(e.graceBefore, false);
        group(e.graceAfter, true);
    }

    void items(const std::vector<nt::Item>& items, const Target& t, const nt::Rational& barLen, Fraction& pos, int depth)
    {
        const std::string at = where(t.bar, t.voice);
        for (const nt::Item& item : items) {
            if (const nt::Event* e = std::get_if<nt::Event>(&item)) {
                if (!e->marks.empty() || e->slurOpen || e->slurClose) {
                    add(at + ": write_voice writes notes and rhythms only; take the marks and slurs out of the content "
                        "and add them with the expression and text commands (dynamics, chord symbols and staff text "
                        "already on the staff are kept)");
                    return;
                }
                if (e->kind == nt::EventKind::Gap && t.voice == 1) {
                    add(at + ": '_' (gap) is for voices 2-4; voice 1 must be complete, write a rest instead");
                }
                if (e->kind == nt::EventKind::Gap && depth > 0) {
                    add(at + ": '_' (gap) cannot be inside a tuplet; write a rest");
                }
                if (e->kind == nt::EventKind::FullBarRest && (items.size() != 1 || depth > 0)) {
                    add(at + ": R (full-bar rest) must be the only item of its bar");
                }
                const Fraction tick = t.measure->tick() + pos;
                for (const nt::NoteHead& h : e->heads) {
                    pitchInRange(h.pitch, tick, at);
                }
                graces(*e, tick, at);
                if (e->kind != nt::EventKind::FullBarRest) {
                    pos += frac(e->duration.value());
                }
                continue;
            }
            const nt::Tuplet& tu = std::get<nt::Tuplet>(item);
            const Fraction total = tupletTotal(tu, barLen);
            const TDuration td(total);
            if (!td.isValid() || td.fraction() != total) {
                add(at + ": tuplet {" + std::to_string(tu.actual) + ":" + std::to_string(tu.normal) + "} takes "
                    + rat(total).toString() + " of written time, which is not one note value (dotted or not)");
                continue;
            }
            if (!TDuration::isValid(total / tu.normal)) {
                add(at + ": tuplet {" + std::to_string(tu.actual) + ":" + std::to_string(tu.normal) + "} over "
                    + rat(total).toString() + " would have a unit of " + rat(total / tu.normal).toString()
                    + ", which is not a note value");
                continue;
            }
            Fraction inner;
            this->items(tu.items, t, barLen, inner, depth + 1);
            pos += total;
        }
    }
};

//! Ties the content asks for: each must land on the same pitch in the next event of the voice,
//! which is the next item of the content or, after the last bar written, the first chord of
//! the next bar in the score.
void checkTies(const Ctx& c, const std::vector<Target>& targets, std::vector<std::string>& problems)
{
    std::map<std::pair<int, int>, const Target*> byBarVoice;
    for (const Target& t : targets) {
        byBarVoice[{ t.bar, t.voice }] = &t;
    }
    for (const Target& t : targets) {
        // flatten the line (graces and gaps excluded from the sequence: gaps break a tie)
        std::vector<const nt::Event*> evs;
        std::function<void(const std::vector<nt::Item>&)> flat = [&](const std::vector<nt::Item>& items) {
            for (const nt::Item& item : items) {
                if (const nt::Event* e = std::get_if<nt::Event>(&item)) {
                    evs.push_back(e);
                } else {
                    flat(std::get<nt::Tuplet>(item).items);
                }
            }
        };
        flat(t.line->items);
        for (size_t i = 0; i < evs.size(); ++i) {
            for (const nt::NoteHead& h : evs[i]->heads) {
                if (!h.tie) {
                    continue;
                }
                const std::string at = where(t.bar, t.voice) + ": " + nt::writePitch(h.pitch) + "~";
                if (i + 1 < evs.size()) {
                    bool ok = false;
                    for (const nt::NoteHead& n : evs[i + 1]->heads) {
                        ok = ok || n.pitch.midi() == h.pitch.midi();
                    }
                    if (!ok) {
                        problems.push_back(at + " is not followed by the same pitch in the voice");
                    }
                    continue;
                }
                // last event of the bar: into the next bar
                auto next = byBarVoice.find({ t.bar + 1, t.voice });
                if (next != byBarVoice.end()) {
                    continue; // checked below, from the next line's first event
                }
                Measure* nm = t.measure->nextMeasure();
                const ChordRest* cr = nm ? c.score->findCR(nm->tick(), t.track) : nullptr;
                const int sp = store(c, h.pitch, t.measure->tick()).pitch;
                if (!cr || !cr->isChord() || cr->tick() != nm->tick() || !toChord(cr)->findNote(sp)) {
                    problems.push_back(at + " ties out of the last bar written, but the next bar's first event in v"
                                       + std::to_string(t.voice) + " is not that pitch");
                }
            }
        }
        // a tie from the previous line into this one
        auto prev = byBarVoice.find({ t.bar - 1, t.voice });
        if (prev != byBarVoice.end() && !evs.empty()) {
            const nt::Event* last = nullptr;
            std::function<void(const std::vector<nt::Item>&)> lastOf = [&](const std::vector<nt::Item>& items) {
                for (const nt::Item& item : items) {
                    if (const nt::Event* e = std::get_if<nt::Event>(&item)) {
                        last = e;
                    } else {
                        lastOf(std::get<nt::Tuplet>(item).items);
                    }
                }
            };
            lastOf(prev->second->line->items);
            if (last) {
                for (const nt::NoteHead& h : last->heads) {
                    if (!h.tie) {
                        continue;
                    }
                    bool ok = false;
                    for (const nt::NoteHead& n : evs.front()->heads) {
                        ok = ok || n.pitch.midi() == h.pitch.midi();
                    }
                    if (!ok) {
                        problems.push_back(where(t.bar - 1, t.voice) + ": " + nt::writePitch(h.pitch)
                                           + "~ is not followed by the same pitch at the start of m" + std::to_string(t.bar));
                    }
                }
            }
        }
    }
}

// --- writing

struct Writer {
    const Ctx& c;
    std::vector<Written>& written;
    std::string error;

    bool fail(const std::string& s)
    {
        if (error.empty()) {
            error = s;
        }
        return false;
    }

    ChordRest* lastCR(const InputState& is, track_idx_t track)
    {
        Segment* s = is.lastSegment();
        EngravingItem* e = s ? s->element(track) : nullptr;
        return e && e->isChordRest() ? toChordRest(e) : nullptr;
    }

    //! The UI's "delete a rest in voice 2-4" (Score::deleteItem, edit.cpp:2916-2924), without
    //! its merging of neighbouring gaps, so each '_' of the content stays one gap rest.
    void makeGap(Rest* rest)
    {
        rest->undoChangeProperty(Pid::GAP, true);
        rest->undoChangeProperty(Pid::BEAM_MODE, BeamMode::AUTO);
        for (EngravingObject* r : rest->linkList()) {
            Rest* rr = toRest(r);
            if (rr != rest && rr->track() % VOICES) {
                rr->undoChangeProperty(Pid::GAP, true);
            }
        }
    }

    bool items(const std::vector<nt::Item>& items, const Target& t, InputState& is, const nt::Rational& barLen)
    {
        const std::string at = where(t.bar, t.voice);
        for (size_t i = 0; i < items.size(); ++i) {
            if (const nt::Event* e = std::get_if<nt::Event>(&items[i])) {
                if (!event(*e, t, is)) {
                    return false;
                }
                continue;
            }
            const nt::Tuplet& tu = std::get<nt::Tuplet>(items[i]);
            // the UI's order (NotationNoteInput::addTuplet): a chord/rest of the tuplet's whole
            // length, then Score::addTuplet on it; validation made sure it has no dialog to show
            c.score->enterRest(TDuration(tupletTotal(tu, barLen)), &is);
            ChordRest* placeholder = lastCR(is, t.track);
            if (!placeholder || !placeholder->isRest()) {
                return fail(at + ": could not make room for a tuplet");
            }
            Segment* seg = placeholder->segment();
            Tuplet* tuplet = c.score->addTuplet(placeholder, Fraction(tu.actual, tu.normal), TupletNumberType::SHOW_NUMBER,
                                                TupletBracketType::AUTO_BRACKET);
            if (!tuplet) {
                return fail(at + ": MuseScore did not create the tuplet {" + std::to_string(tu.actual) + ":"
                            + std::to_string(tu.normal) + "}");
            }
            is.setSegment(seg);
            if (!this->items(tu.items, t, is, barLen)) {
                return false;
            }
        }
        return true;
    }

    bool event(const nt::Event& e, const Target& t, InputState& is)
    {
        const std::string at = where(t.bar, t.voice);
        switch (e.kind) {
        case nt::EventKind::FullBarRest:
            if (t.voice > 1) {
                c.score->expandVoice(is.segment(), t.track); // an empty voice-2-4 bar becomes a measure rest
            }
            // voice 1 is a measure rest already: the clear left it so
            written.push_back({ &e, &t, nullptr, false });
            return true;
        case nt::EventKind::Rest:
        case nt::EventKind::Gap: {
            c.score->enterRest(tdur(e.duration), &is);
            ChordRest* cr = lastCR(is, t.track);
            if (!cr || !cr->isRest()) {
                return fail(at + ": could not write a rest");
            }
            if (e.kind == nt::EventKind::Gap) {
                makeGap(toRest(cr));
            }
            written.push_back({ &e, &t, nullptr, false });
            return true;
        }
        case nt::EventKind::Note:
        case nt::EventKind::Chord: {
            is.setDuration(tdur(e.duration));
            const Fraction tick = is.segment() ? is.segment()->tick() : t.measure->tick();
            NoteVal nv = noteVal(c, e.heads.front().pitch, tick);
            Note* n = c.score->addPitch(nv, false, &is);
            if (!n) {
                return fail(at + ": MuseScore did not write " + nt::writePitch(e.heads.front().pitch));
            }
            Chord* chord = n->chord();
            for (size_t k = 1; k < e.heads.size(); ++k) {
                NoteVal nk = noteVal(c, e.heads[k].pitch, chord->tick());
                if (!c.score->addPitch(nk, true, &is)) {
                    return fail(at + ": MuseScore did not add " + nt::writePitch(e.heads[k].pitch) + " to the chord");
                }
            }
            written.push_back({ &e, &t, chord, false });
            return true;
        }
        }
        return fail(at + ": unknown event");
    }

    //! Grace notes, added after the main chords exist. Each new grace chord goes to the front of
    //! the chord's list (Chord::add, graceIndex 0). graceNotesBefore() reads that list forwards
    //! and graceNotesAfter() backwards (chord.cpp:2107, 2132), so the list must end up as
    //! "before" graces in order, then "after" graces reversed: add the afters first in order,
    //! then the befores last to first.
    bool graces(const Written& w)
    {
        const nt::Event& e = *w.event;
        Chord* ch = w.chord;
        std::vector<std::pair<const nt::Event*, bool> > all;
        for (const nt::Event& g : e.graceAfter) {
            all.push_back({ &g, true });
        }
        for (auto g = e.graceBefore.rbegin(); g != e.graceBefore.rend(); ++g) {
            all.push_back({ &*g, false });
        }
        for (auto it = all.begin(); it != all.end(); ++it) {
            const nt::Event& g = *it->first;
            const NoteType type = *graceType(e.graceBeforeKind, it->second, g.duration);
            const int len = frac(g.duration.value()).ticks();
            const Stored s0 = store(c, g.heads.front().pitch, ch->tick());
            Note* gn = c.score->setGraceNote(ch, s0.pitch, type, len);
            if (!gn) {
                return fail(where(w.target->bar, w.target->voice) + ": MuseScore did not add a grace note");
            }
            gn->undoChangeProperty(Pid::TPC1, s0.tpc1);
            gn->undoChangeProperty(Pid::TPC2, s0.tpc2);
            Chord* gc = gn->chord();
            for (size_t k = 1; k < g.heads.size(); ++k) {
                const Stored sk = store(c, g.heads[k].pitch, ch->tick());
                NoteVal nv(sk.pitch);
                nv.tpc1 = sk.tpc1;
                nv.tpc2 = sk.tpc2;
                InputState scratch; // Score::addNote moves the input position it is given
                if (!c.score->addNote(gc, nv, false, {}, &scratch)) {
                    return fail(where(w.target->bar, w.target->voice) + ": MuseScore did not add a grace chord tone");
                }
            }
        }
        return true;
    }
};

Tie* addTie(Note* start, Note* end)
{
    return addTieBetween(start, end);
}

// --- post-condition

void strip(std::vector<nt::Item>& items)
{
    for (nt::Item& item : items) {
        if (nt::Event* e = std::get_if<nt::Event>(&item)) {
            e->marks.clear();
            e->slurOpen = e->slurClose = 0;
            for (nt::Event& g : e->graceBefore) {
                g.marks.clear();
            }
            for (nt::Event& g : e->graceAfter) {
                g.marks.clear();
            }
        } else {
            nt::Tuplet& t = std::get<nt::Tuplet>(item);
            t.slurOpen = t.slurClose = 0;
            strip(t.items);
        }
    }
}

bool onlyGaps(const std::vector<nt::Item>& items)
{
    for (const nt::Item& item : items) {
        const nt::Event* e = std::get_if<nt::Event>(&item);
        if (!e || e->kind != nt::EventKind::Gap) {
            return false;
        }
    }
    return true;
}

std::string describe(const EngravingItem* e, int bar, const Measure* m)
{
    std::string s = e->typeName();
    if (e->isTextBase()) {
        s += " \"" + toTextBase(e)->plainText().toStdString() + "\"";
    }
    s += " at m" + std::to_string(bar);
    const Fraction off = e->tick() - m->tick();
    if (off.isNotZero()) {
        s += "+" + rat(off).toString();
    }
    return s;
}
}

Ret mu::agent::notes::writeVoice(MasterScore* score, const WriteVoiceArgs& args, WriteVoiceResult& result)
{
    if (!score) {
        return makeErr(Err::NoScore, "no score is open");
    }
    const int nstaves = static_cast<int>(score->nstaves());
    if (args.staff < 1 || args.staff > nstaves) {
        return makeErr(Err::Range, "staff " + std::to_string(args.staff) + " is outside 1-" + std::to_string(nstaves));
    }
    Ctx c;
    c.score = score;
    c.staffIdx = static_cast<staff_idx_t>(args.staff - 1);
    c.staff = score->staff(c.staffIdx);

    // --- parse
    const nt::ParseResult parsed = nt::parse(args.content);
    if (!parsed.ok()) {
        std::string msg = "the content does not parse:";
        for (size_t i = 0; i < parsed.errors.size() && i < 8; ++i) {
            msg += "\n  " + parsed.errors[i].toString();
        }
        return makeErr(Err::InvalidArg, msg);
    }

    std::vector<Measure*> measures;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        measures.push_back(m);
    }

    bool headerWritten = false;
    std::vector<Target> targets;
    std::vector<std::string> problems;
    std::set<std::pair<int, int> > seen;
    std::vector<const nt::Line*> attrLines;
    for (const nt::Line& line : parsed.doc.lines) {
        if (const nt::StaffHeader* h = std::get_if<nt::StaffHeader>(&line)) {
            if (h->staff != args.staff) {
                problems.push_back("the content's header is for staff " + std::to_string(h->staff) + ", but staff is "
                                   + std::to_string(args.staff));
            }
            for (const nt::KeyValue& kv : h->attrs) {
                if (kv.key == "pitch" && kv.value == "written") {
                    headerWritten = true;
                }
            }
            continue;
        }
        if (std::holds_alternative<nt::BarAttrs>(line) || std::holds_alternative<nt::HarmonyLine>(line)) {
            attrLines.push_back(&line); // allowed only when they say what the score already says (below)
            continue;
        }
        const nt::VoiceLine* vl = std::get_if<nt::VoiceLine>(&line);
        if (!vl) {
            continue; // comments
        }
        const int bar = vl->bar.index;
        if (bar < 1 || bar > static_cast<int>(measures.size())) {
            problems.push_back("m" + std::to_string(bar) + " is outside the score (bars 1-" + std::to_string(measures.size()) + ")");
            continue;
        }
        if (!seen.insert({ bar, vl->voice }).second) {
            problems.push_back(where(bar, vl->voice) + " appears twice in the content");
            continue;
        }
        Target t;
        t.line = vl;
        t.measure = measures[bar - 1];
        t.bar = bar;
        t.voice = vl->voice;
        t.track = c.staffIdx * VOICES + (vl->voice - 1);
        targets.push_back(t);
    }
    if (targets.empty() && problems.empty()) {
        return makeErr(Err::InvalidArg, "the content has no voice lines (\"m<N> v<V> | ...\")");
    }
    if (args.pitch == PitchMode::Sounding && headerWritten) {
        problems.push_back("the content's header says pitch=written, but pitch is sounding");
    }
    c.written = args.pitch == PitchMode::Written || (args.pitch == PitchMode::Default && headerWritten);
    if (!problems.empty()) {
        std::string msg = problems.front();
        for (size_t i = 1; i < problems.size() && i < 8; ++i) {
            msg += "; " + problems[i];
        }
        return makeErr(Err::InvalidArg, msg);
    }
    std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) {
        return a.bar != b.bar ? a.bar < b.bar : a.voice < b.voice;
    });
    result.fromBar = targets.front().bar;
    result.toBar = targets.back().bar;

    // bar attributes and harmony lines may be passed through only unchanged
    read::ReadOptions ropt;
    ropt.fromBar = result.fromBar;
    ropt.toBar = result.toBar;
    ropt.staves = { args.staff };
    ropt.writtenPitch = c.written;
    if (!attrLines.empty()) {
        const nt::Document before = read::printScore(score, ropt);
        for (const nt::Line* l : attrLines) {
            if (std::find(before.lines.begin(), before.lines.end(), *l) == before.lines.end()) {
                const bool harmony = std::holds_alternative<nt::HarmonyLine>(*l);
                return makeErr(Err::InvalidArg, std::string("the content changes ")
                               + (harmony ? "a chord-symbol line; write_voice writes notes only, use add_chord_symbol"
                                  : "bar attributes (time, key, clef, barlines, marks); write_voice writes notes only, use the "
                                  "structure commands")
                               + " (lines that match the score as it is are accepted and ignored)");
            }
        }
    }

    // --- validate against the score
    Checker check { c, {} };
    for (const Target& t : targets) {
        const Fraction tick = t.measure->tick();
        const std::string at = "m" + std::to_string(t.bar);
        if (c.staff->isTabStaff(tick) || c.staff->isDrumStaff(tick)) {
            return makeErr(Err::InvalidArg, at + ": staff " + std::to_string(args.staff)
                           + " is a tablature or percussion staff at this bar; write_voice v1 writes pitched staves only");
        }
        if (t.measure->isMeasureRepeatGroup(c.staffIdx)) {
            return makeErr(Err::InvalidArg, at + " is a measure repeat on staff " + std::to_string(args.staff)
                           + "; remove the measure repeat first (writing over it would delete it silently)");
        }
        for (Segment* s = t.measure->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
            EngravingItem* e = s->element(t.track);
            if (e && e->isChord() && toChord(e)->staffMove() != 0) {
                return makeErr(Err::InvalidArg, where(t.bar, t.voice) + " has cross-staff notes; write_voice v1 cannot "
                               "write those and would move them back onto staff " + std::to_string(args.staff));
            }
        }
        if (c.staff->timeStretch(tick) != Fraction(1, 1)) {
            return makeErr(Err::InvalidArg, at + ": staff " + std::to_string(args.staff)
                           + " has a local time signature here; write_voice v1 does not write those");
        }
        const nt::Rational barLen = rat(t.measure->ticks());
        nt::Document one;
        one.lines.push_back(*t.line);
        for (const nt::Problem& p : nt::validate(one, [&](int, int) { return std::optional<nt::Rational>(barLen); })) {
            if (p.code != "E_TIE") { // ties are checked across lines and into the score below
                check.add(p.message);
            }
        }
        Fraction pos;
        check.items(t.line->items, t, barLen, pos, 0);
    }
    checkTies(c, targets, check.problems);
    if (!check.problems.empty()) {
        std::string msg = check.problems.front();
        for (size_t i = 1; i < check.problems.size() && i < 8; ++i) {
            msg += "; " + check.problems[i];
        }
        if (check.problems.size() > 8) {
            msg += "; and " + std::to_string(check.problems.size() - 8) + " more";
        }
        return makeErr(Err::InvalidArg, msg);
    }

    // --- record what clearing may take with it (reported, not refused)
    std::set<std::pair<int, int> > writtenSet;
    for (const Target& t : targets) {
        writtenSet.insert({ t.bar, t.voice });
    }
    struct IncomingTie {
        Note* start;
        int pitch;
        const Target* target;
    };
    std::vector<IncomingTie> incoming;
    std::vector<std::pair<const EngravingItem*, std::string> > watched; // compared by address only
    struct MidBarClef {
        Fraction tick;
        ClefType type;
        std::string what;
    };
    std::vector<MidBarClef> clefs;
    for (const Target& t : targets) {
        if (!writtenSet.count({ t.bar - 1, t.voice })) {
            if (Measure* pm = t.measure->prevMeasure()) {
                for (Segment* s = pm->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
                    EngravingItem* e = s->element(t.track);
                    if (!e || !e->isChord()) {
                        continue;
                    }
                    for (Note* n : toChord(e)->notes()) {
                        const Tie* tie = n->tieFor();
                        if (tie && tie->endNote() && tie->endNote()->chord()->measure() == t.measure) {
                            incoming.push_back({ n, n->pitch(), &t });
                        }
                    }
                }
            }
        }
        // a range delete takes mid-bar clefs and breaths on the track with it (deleteRangeAtTrack):
        // clefs are put back afterwards, breaths reported
        for (Segment* s = t.measure->first(); s; s = s->next()) {
            EngravingItem* e = s->element(t.track);
            if (!e || s->tick() <= t.measure->tick() || s->tick() >= t.measure->endTick()) {
                continue;
            }
            if (s->isClefType() && e->isClef() && !e->generated()) {
                clefs.push_back({ s->tick(), toClef(e)->clefType(), describe(e, t.bar, t.measure) });
            } else if (s->isBreathType()) {
                watched.push_back({ e, describe(e, t.bar, t.measure) });
            }
        }
    }

    // --- clear: the UI's range delete, one voice of one bar at a time
    SelectionFilter filter;
    if (args.keepAnnotations) {
        for (ElementsSelectionFilterTypes f : { ElementsSelectionFilterTypes::DYNAMIC, ElementsSelectionFilterTypes::HAIRPIN,
                                                ElementsSelectionFilterTypes::CHORD_SYMBOL, ElementsSelectionFilterTypes::OTHER_TEXT,
                                                ElementsSelectionFilterTypes::FIGURED_BASS, ElementsSelectionFilterTypes::FRET_DIAGRAM,
                                                ElementsSelectionFilterTypes::OTTAVA, ElementsSelectionFilterTypes::PEDAL_LINE,
                                                ElementsSelectionFilterTypes::OTHER_LINE }) {
            filter.setFiltered(f, false);
        }
    }
    for (const Target& t : targets) {
        Segment* s1 = t.measure->first(SegmentType::ChordRest);
        Measure* nm = t.measure->nextMeasure();
        Segment* s2 = nm ? nm->first(SegmentType::ChordRest) : nullptr;
        if (!s1) {
            return makeErr(Err::Internal, "m" + std::to_string(t.bar) + " has no chord/rest segment");
        }
        score->deleteRange(s1, s2, t.track, t.track + 1, filter, false);
        if (t.voice > 1) {
            // A range delete leaves rests where a voice 2-4 had notes next to gaps. The voice is
            // being rewritten whole, so remove what is left, as the UI does once a voice-2-4 bar
            // holds only deleted rests (Score::deleteItem, edit.cpp:2929-2940).
            std::vector<Rest*> left;
            bool other = false;
            for (Segment* s = t.measure->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
                EngravingItem* e = s->element(t.track);
                if (e && e->isRest() && !toRest(e)->tuplet()) {
                    left.push_back(toRest(e));
                } else if (e) {
                    other = true;
                }
            }
            if (!other && !c.staff->trackHasLinksInVoiceZero(t.track)) {
                for (Rest* r : left) {
                    score->undoRemoveElement(r);
                }
                score->checkSpanner(t.measure->tick(), t.measure->endTick());
            }
        }
    }

    // annotations still on the staff after the clear: writing must not lose them
    std::set<int> barsSeen;
    for (const Target& t : targets) {
        if (!barsSeen.insert(t.bar).second) {
            continue;
        }
        for (Segment* s = t.measure->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
            for (EngravingItem* a : s->annotations()) {
                if (a->staffIdx() == c.staffIdx) {
                    watched.push_back({ a, describe(a, t.bar, t.measure) });
                }
            }
        }
    }

    // --- write
    std::vector<Written> written;
    Writer w { c, written, {} };
    for (const Target& t : targets) {
        Segment* seg = t.measure->findSegment(SegmentType::ChordRest, t.measure->tick());
        if (!seg) {
            return makeErr(Err::Internal, "m" + std::to_string(t.bar) + " has no chord/rest segment at its start");
        }
        if (t.voice > 1 && onlyGaps(t.line->items)) {
            continue; // the clear removed the voice from this bar, which is what a line of gaps says
        }
        InputState is; // private: the user's input state and selection are never touched (G5)
        is.setTrack(t.track);
        is.setSegment(seg);
        const size_t first = written.size();
        if (!w.items(t.line->items, t, is, rat(t.measure->ticks()))) {
            return makeErr(Err::Refused, w.error);
        }
        if (written.size() > first) {
            written.back().lastInLine = true;
        }
    }
    for (const Written& wr : written) {
        if (wr.chord && (!wr.event->graceBefore.empty() || !wr.event->graceAfter.empty()) && !w.graces(wr)) {
            return makeErr(Err::Refused, w.error);
        }
    }

    // --- ties: the content's, then the ones into the range that still land
    for (size_t i = 0; i < written.size(); ++i) {
        const Written& wr = written[i];
        if (!wr.chord) {
            continue;
        }
        for (const nt::NoteHead& h : wr.event->heads) {
            if (!h.tie) {
                continue;
            }
            const int sp = store(c, h.pitch, wr.chord->tick()).pitch;
            Note* start = wr.chord->findNote(sp);
            Chord* endChord = nullptr;
            const bool nextInContent = !wr.lastInLine
                                       || writtenSet.count({ wr.target->bar + 1, wr.target->voice });
            if (nextInContent) {
                // the next written event of this voice
                for (size_t j = i + 1; j < written.size(); ++j) {
                    if (written[j].target->voice == wr.target->voice) {
                        endChord = written[j].chord;
                        break;
                    }
                }
            } else if (Measure* nm = wr.target->measure->nextMeasure()) {
                ChordRest* cr = score->findCR(nm->tick(), wr.target->track);
                endChord = cr && cr->isChord() && cr->tick() == nm->tick() ? toChord(cr) : nullptr;
            }
            Note* end = endChord ? endChord->findNote(sp) : nullptr;
            if (!start || !end) {
                return makeErr(Err::PostCondition, where(wr.target->bar, wr.target->voice) + ": the tie from "
                               + nt::writePitch(h.pitch) + " has no note to land on");
            }
            addTie(start, end);
        }
    }
    for (const IncomingTie& it : incoming) {
        ChordRest* cr = score->findCR(it.target->measure->tick(), it.target->track);
        Note* end = cr && cr->isChord() && cr->tick() == it.target->measure->tick() ? toChord(cr)->findNote(it.pitch) : nullptr;
        if (end && !it.start->tieFor()) {
            addTie(it.start, end);
        } else if (!end) {
            result.tiesRemoved.push_back("the tie from m" + std::to_string(it.target->bar - 1) + " into "
                                         + where(it.target->bar, it.target->voice) + " (the new first event is not that pitch)");
        }
    }

    // mid-bar clef changes, put back where a chord or rest of the staff starts at their tick (the
    // UI's clef drop, Score::undoChangeClef); otherwise reported
    for (const MidBarClef& mc : clefs) {
        Measure* m = score->tick2measure(mc.tick);
        Segment* cs = m ? m->findSegment(SegmentType::Clef, mc.tick) : nullptr;
        if (cs && cs->element(c.staffIdx * VOICES)) {
            continue; // still there
        }
        ChordRest* at = nullptr;
        for (voice_idx_t v = 0; v < VOICES && !at; ++v) {
            ChordRest* cr = score->findCR(mc.tick, c.staffIdx * VOICES + v);
            at = cr && cr->tick() == mc.tick ? cr : nullptr;
        }
        if (at) {
            score->undoChangeClef(c.staff, at, mc.type);
        } else {
            result.annotationsRemoved.push_back(mc.what + " (no note or rest starts there now)");
        }
    }

    // --- post-condition: print the bars back; they must say exactly what was written
    const nt::Document after = read::printScore(score, ropt);
    std::map<std::pair<int, int>, nt::VoiceLine> printed;
    for (const nt::Line& l : after.lines) {
        if (const nt::VoiceLine* vl = std::get_if<nt::VoiceLine>(&l)) {
            nt::VoiceLine copy = *vl;
            strip(copy.items);
            printed[{ vl->bar.index, vl->voice }] = copy;
        }
    }
    for (const Target& t : targets) {
        auto it = printed.find({ t.bar, t.voice });
        const bool absent = it == printed.end();
        if (absent ? (t.voice > 1 && onlyGaps(t.line->items)) : it->second.items == t.line->items) {
            continue;
        }
        return makeErr(Err::PostCondition, where(t.bar, t.voice) + ": wrote \"" + nt::writeItems(t.line->items)
                       + "\" but the score now has \"" + (absent ? std::string("(nothing)") : nt::writeItems(it->second.items))
                       + "\"; nothing was changed");
    }

    std::unordered_set<const EngravingItem*> live;
    score->scanElements([&live](EngravingItem* e) { live.insert(e); });
    for (const Target& t : targets) {
        for (Segment* s = t.measure->first(); s; s = s->next()) {
            for (EngravingItem* a : s->annotations()) {
                live.insert(a);
            }
        }
    }
    for (const auto& [item, what] : watched) {
        if (!live.count(item)) {
            result.annotationsRemoved.push_back(what);
        }
    }
    result.text = nt::write(after);
    return muse::make_ok();
}
