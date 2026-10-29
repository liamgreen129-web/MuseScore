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
#include "clearrange.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/select.h"
#include "engraving/dom/selectionfilter.h"

#include "core/postconditions.h"
#include "notation/writer.h"
#include "read/scoreprinter.h"
#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::agent::notes;
using namespace mu::engraving;
using muse::Ret;
namespace nt = mu::agent::notation;

namespace {
using FT = ElementsSelectionFilterTypes;

const std::vector<std::pair<std::string, FT> >& typeTable()
{
    static const std::vector<std::pair<std::string, FT> > t = {
        { "dynamics", FT::DYNAMIC }, { "hairpins", FT::HAIRPIN }, { "fingering", FT::FINGERING }, { "lyrics", FT::LYRICS },
        { "chordSymbols", FT::CHORD_SYMBOL }, { "otherText", FT::OTHER_TEXT }, { "articulations", FT::ARTICULATION },
        { "ornaments", FT::ORNAMENT }, { "slurs", FT::SLUR }, { "ties", FT::TIE }, { "figuredBass", FT::FIGURED_BASS },
        { "ottavas", FT::OTTAVA }, { "pedals", FT::PEDAL_LINE }, { "otherLines", FT::OTHER_LINE }, { "arpeggios", FT::ARPEGGIO },
        { "glissandi", FT::GLISSANDO }, { "fretDiagrams", FT::FRET_DIAGRAM }, { "breaths", FT::BREATH },
        { "tremolos", FT::TREMOLO }, { "graceNotes", FT::GRACE_NOTE },
    };
    return t;
}

const VoicesSelectionFilterTypes VOICE_TYPES[] = {
    VoicesSelectionFilterTypes::FIRST_VOICE, VoicesSelectionFilterTypes::SECOND_VOICE,
    VoicesSelectionFilterTypes::THIRD_VOICE, VoicesSelectionFilterTypes::FOURTH_VOICE
};

//! A beat (quarter notes from 1) as an offset into the bar, or an error message
std::pair<Fraction, std::string> beatOffset(double beat, const Measure* m, const std::string& what)
{
    const double ticks = (beat - 1.0) * Constants::DIVISION;
    if (beat < 1.0 || std::fabs(ticks - std::round(ticks)) > 1e-6) {
        return { {}, what + " beat " + std::to_string(beat) + " is not a position (beats count quarter notes from 1)" };
    }
    const Fraction off = Fraction::fromTicks(static_cast<int>(std::lround(ticks)));
    if (off >= m->ticks()) {
        return { {}, what + " beat " + std::to_string(beat) + " is past the end of its bar" };
    }
    return { off, "" };
}

//! True if a chord or rest of one of the staves starts at `tick`
bool eventStarts(const MasterScore* score, const Fraction& tick, staff_idx_t s1, staff_idx_t s2)
{
    const Measure* m = score->tick2measure(tick);
    const Segment* seg = m ? m->findSegment(SegmentType::ChordRest, tick) : nullptr;
    if (!seg) {
        return false;
    }
    for (track_idx_t t = staff2track(s1); t < staff2track(s2 + 1); ++t) {
        if (seg->element(t)) {
            return true;
        }
    }
    return false;
}
}

const std::vector<std::string>& mu::agent::notes::clearRangeTypes()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& [name, type] : typeTable()) {
            n.push_back(name);
        }
        return n;
    }();
    return names;
}

Ret mu::agent::notes::clearRange(MasterScore* score, const ClearRangeArgs& a, ClearRangeResult& result)
{
    if (!score) {
        return makeErr(Err::NoScore, "no score is open");
    }
    const int nstaves = static_cast<int>(score->nstaves());
    if (a.staffFrom < 1 || a.staffTo > nstaves || a.staffFrom > a.staffTo) {
        return makeErr(Err::Range, "staves " + std::to_string(a.staffFrom) + "-" + std::to_string(a.staffTo)
                       + " are not a range within 1-" + std::to_string(nstaves));
    }
    if (a.voices.empty()) {
        return makeErr(Err::InvalidArg, "voices is empty");
    }
    for (int v : a.voices) {
        if (v < 1 || v > 4) {
            return makeErr(Err::Range, "voice " + std::to_string(v) + " is not 1-4");
        }
    }
    std::vector<FT> include;
    for (const std::string& name : a.include) {
        auto it = std::find_if(typeTable().begin(), typeTable().end(), [&](const auto& e) { return e.first == name; });
        if (it == typeTable().end()) {
            return makeErr(Err::InvalidArg, "unknown element type \"" + name + "\" in include");
        }
        include.push_back(it->second);
    }

    std::vector<Measure*> measures;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        measures.push_back(m);
    }
    const int nbars = static_cast<int>(measures.size());
    for (const BarBeat* p : { &a.from, &a.to }) {
        if (p->bar < 1 || p->bar > nbars) {
            return makeErr(Err::Range, "bar " + std::to_string(p->bar) + " is outside the score (bars 1-" + std::to_string(nbars) + ")");
        }
    }
    const Measure* mFrom = measures[a.from.bar - 1];
    const Measure* mTo = measures[a.to.bar - 1];
    Fraction t1 = mFrom->tick();
    if (a.from.hasBeat) {
        auto [off, err] = beatOffset(a.from.beat, mFrom, "from");
        if (!err.empty()) {
            return makeErr(Err::Range, err);
        }
        t1 += off;
    }
    Fraction t2 = mTo->endTick();
    if (a.to.hasBeat) {
        auto [off, err] = beatOffset(a.to.beat, mTo, "to");
        if (!err.empty()) {
            return makeErr(Err::Range, err);
        }
        t2 = mTo->tick() + off;
    }
    if (t2 <= t1) {
        return makeErr(Err::Range, "the range is empty: \"to\" must come after \"from\" (to with a beat is exclusive)");
    }
    const staff_idx_t s1 = static_cast<staff_idx_t>(a.staffFrom - 1);
    const staff_idx_t s2 = static_cast<staff_idx_t>(a.staffTo - 1);
    if (!eventStarts(score, t1, s1, s2)) {
        return makeErr(Err::Range, "nothing starts at \"from\" on those staves; start the range where a note or rest starts");
    }
    if (t2 != mTo->endTick() && !eventStarts(score, t2, s1, s2)) {
        return makeErr(Err::Range, "nothing starts at \"to\" on those staves; end the range where a note or rest starts");
    }
    result.fromBar = a.from.bar;
    result.toBar = a.to.hasBeat && t2 == mTo->tick() ? a.to.bar - 1 : a.to.bar;

    // the UI path: a range selection under the selection filter, then Score::deleteRange
    // (Score::cmdDeleteSelection's range branch, edit.cpp:4007-4011). Rests go only where the
    // selection marked them selected (edit.cpp:3902-3908), so the selection has to be real.
    SelectionFilter& filter = score->selectionFilter();
    const SelectionFilter saved = filter;
    SelectionFilter f;
    for (int v = 1; v <= 4; ++v) {
        f.setFiltered(VOICE_TYPES[v - 1], a.voices.count(v) > 0);
    }
    if (!include.empty()) {
        for (const auto& [name, type] : typeTable()) {
            f.setFiltered(type, std::find(include.begin(), include.end(), type) != include.end());
        }
    }
    filter = f;
    score->deselectAll();
    score->selection().setRangeTicks(t1, t2, s1, s2 + 1);
    score->selection().updateSelectedElements();
    Segment* startSeg = score->selection().startSegment();
    Segment* endSeg = score->selection().endSegment();
    if (startSeg) {
        score->deleteRange(startSeg, endSeg, staff2track(s1), staff2track(s2 + 1), f,
                           score->selection().rangeContainsMultiNoteChords());
    }
    score->deselectAll();
    filter = saved; // the user's filter, whatever happens next
    if (!startSeg) {
        return makeErr(Err::Internal, "the range selection did not resolve to segments");
    }

    // post-condition: no chord of a cleared voice starts in the range, voice 1 still adds up
    for (Segment* s = score->tick2segment(t1, true, SegmentType::ChordRest); s && s->tick() < t2;
         s = s->next1(SegmentType::ChordRest)) {
        for (staff_idx_t st = s1; st <= s2; ++st) {
            for (int v : a.voices) {
                EngravingItem* e = s->element(st * VOICES + (v - 1));
                if (e && e->isChord()) {
                    return makeErr(Err::PostCondition, "a chord of staff " + std::to_string(st + 1) + " voice "
                                   + std::to_string(v) + " survived the clear; nothing was changed");
                }
            }
        }
    }
    if (Ret r = core::barsAddUp(score, result.fromBar, a.to.bar); !r) {
        return r;
    }

    read::ReadOptions opt;
    opt.fromBar = result.fromBar;
    opt.toBar = a.to.bar;
    for (int st = a.staffFrom; st <= a.staffTo; ++st) {
        opt.staves.push_back(st);
    }
    result.text = nt::write(read::printScore(score, opt));
    return muse::make_ok();
}
