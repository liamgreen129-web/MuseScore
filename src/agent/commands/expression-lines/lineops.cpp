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
// add_slur and add_line (docs/agent/specs/expression-lines.md), on the engraving model.
//
// UI paths reproduced (CLAUDE.md rule 4, "below the wrappers"):
// - add_slur: NotationInteraction::doAddSlur's list branch with two chords
//   (notationinteraction.cpp:2928-2963, 3016-3021) -> Score::addSlur(first, second, nullptr)
//   (edit.cpp:698-759). No selection is needed: the list branch only orders the two chords and
//   skips the add when an AUTO slur with the same ends exists; both are done here as validation.
// - add_line: NotationInteraction::applyPaletteElementToRange's SLine branch
//   (notationinteraction.cpp:2646-2662): template clone, setScore, styleChanged,
//   Score::cmdAddSpanner(spanner, staff, startSegment, endSegment, false) (cmd.cpp:603-633).
//   The start and end segments are the ones a range selection of the two events gives
//   (score.cpp:3591-3604 for chord/rests, 3546-3568 for whole bars), computed here directly so
//   the user's selection is never touched and applyPaletteElement's checkAndShowError (a dialog)
//   is never reached (spec F5, Q2 option 2).

#include "expressionlines.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <vector>

#include "core/handles.h"
#include "core/postconditions.h"
#include "read/scoreprinter.h"
#include "notation/writer.h"
#include "registry/errors.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/ottava.h"
#include "engraving/dom/part.h"
#include "engraving/dom/pedal.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/textline.h"
#include "engraving/dom/trill.h"
#include "engraving/dom/volta.h"
#include "engraving/types/constants.h"

using namespace mu::agent;
using namespace mu::engraving;
using muse::Ret;
using muse::RetVal;
using muse::Val;
using muse::ValMap;

namespace {
using Err = mu::agent::Err; // engraving has an Err too

bool isNumber(const Val& v)
{
    return v.type() == Val::Type::Double || v.type() == Val::Type::Int || v.type() == Val::Type::Int64;
}

bool isInteger(const Val& v)
{
    if (!isNumber(v)) {
        return false;
    }
    const double d = v.toDouble();
    return std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e9;
}

std::string str(const muse::String& s)
{
    return s.toStdString();
}

std::string formatNumber(double d)
{
    std::ostringstream os;
    os.precision(6);
    os << d;
    return os.str();
}

std::vector<Measure*> allMeasures(MasterScore* score)
{
    std::vector<Measure*> out;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        out.push_back(m);
    }
    return out;
}

//! Positional bar number (the notation's m<N>), 0 if not found
int barOf(MasterScore* score, const Measure* target)
{
    int i = 0;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        ++i;
        if (m == target) {
            return i;
        }
    }
    return 0;
}

std::string beatText(const Measure* m, const Fraction& tick)
{
    return formatNumber(1.0 + static_cast<double>((tick - m->tick()).ticks()) / Constants::DIVISION);
}

std::string where(MasterScore* score, const ChordRest* cr)
{
    const Measure* m = cr->measure();
    std::string s = "m" + std::to_string(barOf(score, m)) + " beat " + beatText(m, cr->tick())
                    + " s" + std::to_string(cr->staffIdx() + 1) + " v" + std::to_string(cr->voice() + 1);
    if (cr->isGrace()) {
        s += " (grace)";
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------------------------

struct EventRef {
    ChordRest* cr = nullptr;
    staff_idx_t staff = 0;
    int bar = 0;
};

RetVal<staff_idx_t> resolveStaff(MasterScore* score, const Val& v, const std::string& what)
{
    RetVal<staff_idx_t> rv;
    rv.val = 0;
    const int nstaves = static_cast<int>(score->nstaves());
    if (isNumber(v)) {
        if (!isInteger(v)) {
            rv.ret = makeErr(Err::InvalidArg, what + ".staff must be a whole staff number or {part, index}");
            return rv;
        }
        const int n = static_cast<int>(v.toDouble());
        if (n < 1 || n > nstaves) {
            rv.ret = makeErr(Err::Range, what + ".staff " + std::to_string(n) + " is outside staves 1-" + std::to_string(nstaves));
            return rv;
        }
        rv.ret = muse::make_ok();
        rv.val = static_cast<staff_idx_t>(n - 1);
        return rv;
    }
    if (v.type() != Val::Type::Map) {
        rv.ret = makeErr(Err::InvalidArg, what + ".staff must be a staff number (as score_read numbers them) or {part, index}");
        return rv;
    }
    const ValMap m = v.toMap();
    for (const auto& [k, val] : m) {
        if (k != "part" && k != "index") {
            rv.ret = makeErr(Err::InvalidArg, what + ".staff has unknown key '" + k + "' (expected part, index)");
            return rv;
        }
    }
    auto pit = m.find("part");
    if (pit == m.end() || pit->second.type() != Val::Type::String) {
        rv.ret = makeErr(Err::InvalidArg, what + ".staff.part must be the part's name as score_overview lists it");
        return rv;
    }
    int index = 1;
    if (auto iit = m.find("index"); iit != m.end() && !iit->second.isNull()) {
        if (!isInteger(iit->second)) {
            rv.ret = makeErr(Err::InvalidArg, what + ".staff.index must be a whole number (1 = the part's top staff)");
            return rv;
        }
        index = static_cast<int>(iit->second.toDouble());
    }
    const std::string name = pit->second.toString();
    std::vector<Part*> found;
    std::string names;
    for (Part* part : score->parts()) {
        if (str(part->partName()) == name) {
            found.push_back(part);
        }
        names += (names.empty() ? "" : ", ") + ("\"" + str(part->partName()) + "\"");
    }
    if (found.empty()) {
        rv.ret = makeErr(Err::Range, what + ".staff: no part named \"" + name + "\"; parts are " + names);
        return rv;
    }
    if (found.size() > 1) {
        rv.ret = makeErr(Err::InvalidArg, what + ".staff: " + std::to_string(found.size()) + " parts are named \"" + name
                         + "\"; give the staff number instead");
        return rv;
    }
    const int partStaves = static_cast<int>(found.front()->nstaves());
    if (index < 1 || index > partStaves) {
        rv.ret = makeErr(Err::Range, what + ".staff.index " + std::to_string(index) + " is outside 1-" + std::to_string(partStaves)
                         + " for \"" + name + "\"");
        return rv;
    }
    rv.ret = muse::make_ok();
    rv.val = found.front()->staves().at(static_cast<size_t>(index - 1))->idx();
    return rv;
}

//! The chord/rest an address names: {handle} or {measure, beat|tick, staff, voice, grace}
RetVal<EventRef> resolveEvent(MasterScore* score, const Val& v, const std::string& what)
{
    RetVal<EventRef> rv;
    auto fail = [&](Ret r) {
        rv.ret = r;
        return rv;
    };
    if (v.type() != Val::Type::Map) {
        return fail(makeErr(Err::InvalidArg, what + " must be an object: {handle} or {measure, beat, staff, voice}"));
    }
    const ValMap m = v.toMap();

    if (auto h = m.find("handle"); h != m.end()) {
        if (m.size() != 1) {
            return fail(makeErr(Err::InvalidArg, what + ": give either a handle or a position, not both"));
        }
        if (h->second.type() != Val::Type::String) {
            return fail(makeErr(Err::InvalidArg, what + ".handle must be a string"));
        }
        RetVal<EngravingItem*> e = core::resolveHandle(score, h->second.toString());
        if (!e.ret) {
            return fail(e.ret);
        }
        EngravingItem* item = e.val;
        if (item->isNote()) {
            item = toNote(item)->chord();
        }
        if (!item->isChordRest()) {
            return fail(makeErr(Err::InvalidArg, what + ".handle names a " + std::string(item->typeName())
                                + ", not a chord, note or rest"));
        }
        EventRef ev;
        ev.cr = toChordRest(item);
        ev.staff = ev.cr->staffIdx();
        ev.bar = barOf(score, ev.cr->measure());
        rv.ret = muse::make_ok();
        rv.val = ev;
        return rv;
    }

    static const std::set<std::string> keys { "measure", "beat", "tick", "staff", "voice", "grace" };
    for (const auto& [k, val] : m) {
        if (!keys.count(k)) {
            return fail(makeErr(Err::InvalidArg, what + " has unknown key '" + k
                                + "' (expected handle, or measure, beat|tick, staff, voice, grace)"));
        }
    }
    auto mit = m.find("measure");
    if (mit == m.end() || !isInteger(mit->second)) {
        return fail(makeErr(Err::InvalidArg, what + ".measure is required: the bar number as score_read prints it (m<N>)"));
    }
    const int bar = static_cast<int>(mit->second.toDouble());
    const std::vector<Measure*> measures = allMeasures(score);
    if (bar < 1 || bar > static_cast<int>(measures.size())) {
        return fail(makeErr(Err::Range, what + ".measure " + std::to_string(bar) + " is outside bars 1-" + std::to_string(measures.size())));
    }
    Measure* measure = measures.at(static_cast<size_t>(bar - 1));

    auto sit = m.find("staff");
    if (sit == m.end()) {
        return fail(makeErr(Err::InvalidArg, what + ".staff is required: a staff number or {part, index}"));
    }
    RetVal<staff_idx_t> staff = resolveStaff(score, sit->second, what);
    if (!staff.ret) {
        return fail(staff.ret);
    }

    int voice = 1;
    if (auto vit = m.find("voice"); vit != m.end() && !vit->second.isNull()) {
        if (!isInteger(vit->second) || vit->second.toDouble() < 1 || vit->second.toDouble() > 4) {
            return fail(makeErr(Err::InvalidArg, what + ".voice must be 1-4"));
        }
        voice = static_cast<int>(vit->second.toDouble());
    }

    auto bit = m.find("beat");
    auto tit = m.find("tick");
    const bool hasBeat = bit != m.end() && !bit->second.isNull();
    const bool hasTick = tit != m.end() && !tit->second.isNull();
    if (hasBeat && hasTick) {
        return fail(makeErr(Err::InvalidArg, what + ": give beat or tick, not both"));
    }
    int offset = 0; // ticks from the start of the bar
    if (hasBeat) {
        if (!isNumber(bit->second) || !std::isfinite(bit->second.toDouble())) {
            return fail(makeErr(Err::InvalidArg, what + ".beat must be a number (1 = start of the bar, in quarter notes)"));
        }
        offset = static_cast<int>(std::lround((bit->second.toDouble() - 1.0) * Constants::DIVISION));
    } else if (hasTick) {
        if (!isInteger(tit->second)) {
            return fail(makeErr(Err::InvalidArg, what + ".tick must be a whole number of ticks from the start of the bar (480 per quarter)"));
        }
        offset = static_cast<int>(tit->second.toDouble());
    }
    const std::string place = "m" + std::to_string(bar) + " beat " + formatNumber(1.0 + static_cast<double>(offset) / Constants::DIVISION)
                              + " s" + std::to_string(staff.val + 1) + " v" + std::to_string(voice);
    if (offset < 0 || offset >= measure->ticks().ticks()) {
        return fail(makeErr(Err::Range, what + ": " + place + " is outside the bar (it holds "
                            + formatNumber(static_cast<double>(measure->ticks().ticks()) / Constants::DIVISION) + " quarter beats)"));
    }

    const track_idx_t track = staff.val * VOICES + static_cast<track_idx_t>(voice - 1);
    const Fraction tick = measure->tick() + Fraction::fromTicks(offset);
    ChordRest* cr = nullptr;
    if (Segment* seg = measure->findSegment(SegmentType::ChordRest, tick)) {
        if (EngravingItem* e = seg->element(track); e && e->isChordRest()) {
            cr = toChordRest(e);
        }
    }
    if (!cr) {
        std::string beats;
        for (Segment* s = measure->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
            if (EngravingItem* e = s->element(track); e && e->isChordRest()) {
                beats += (beats.empty() ? "" : ", ") + beatText(measure, s->tick());
            }
        }
        return fail(makeErr(Err::Range, what + ": no chord or rest starts at " + place
                            + (beats.empty() ? std::string(" (that voice is empty in this bar)")
                               : " (events in that voice start on beats " + beats + ")")));
    }

    if (auto git = m.find("grace"); git != m.end() && !git->second.isNull()) {
        if (!isInteger(git->second) || git->second.toDouble() < 1) {
            return fail(makeErr(Err::InvalidArg, what + ".grace must be a whole number from 1"));
        }
        if (!cr->isChord()) {
            return fail(makeErr(Err::InvalidArg, what + ": " + place + " is a rest, which has no grace notes"));
        }
        std::vector<Chord*> graces = toChord(cr)->graceNotes();
        std::stable_sort(graces.begin(), graces.end(), [](const Chord* a, const Chord* b) { return a->isBefore(b); });
        const int g = static_cast<int>(git->second.toDouble());
        if (g > static_cast<int>(graces.size())) {
            return fail(makeErr(Err::Range, what + ": the chord at " + place + " has " + std::to_string(graces.size())
                                + " grace chord(s)"));
        }
        cr = graces.at(static_cast<size_t>(g - 1));
    }

    EventRef ev;
    ev.cr = cr;
    ev.staff = staff.val;
    ev.bar = bar;
    rv.ret = muse::make_ok();
    rv.val = ev;
    return rv;
}

//! The bar a volta address names: {measure} (or {handle}, its bar)
RetVal<Measure*> resolveBar(MasterScore* score, const Val& v, const std::string& what)
{
    RetVal<Measure*> rv;
    rv.val = nullptr;
    if (v.type() != Val::Type::Map) {
        rv.ret = makeErr(Err::InvalidArg, what + " must be an object: {measure}");
        return rv;
    }
    const ValMap m = v.toMap();
    if (m.count("handle")) {
        RetVal<EventRef> ev = resolveEvent(score, v, what);
        if (!ev.ret) {
            rv.ret = ev.ret;
            return rv;
        }
        rv.ret = muse::make_ok();
        rv.val = ev.val.cr->measure();
        return rv;
    }
    for (const auto& [k, val] : m) {
        if (k == "beat" || k == "tick" || k == "grace") {
            rv.ret = makeErr(Err::InvalidArg, what + ": a volta covers whole bars, so give {measure} only (no " + k + ")");
            return rv;
        }
        if (k != "measure" && k != "staff" && k != "voice") {
            rv.ret = makeErr(Err::InvalidArg, what + " has unknown key '" + k + "' (expected measure)");
            return rv;
        }
    }
    auto mit = m.find("measure");
    if (mit == m.end() || !isInteger(mit->second)) {
        rv.ret = makeErr(Err::InvalidArg, what + ".measure is required: the bar number as score_read prints it (m<N>)");
        return rv;
    }
    const int bar = static_cast<int>(mit->second.toDouble());
    const std::vector<Measure*> measures = allMeasures(score);
    if (bar < 1 || bar > static_cast<int>(measures.size())) {
        rv.ret = makeErr(Err::Range, what + ".measure " + std::to_string(bar) + " is outside bars 1-" + std::to_string(measures.size()));
        return rv;
    }
    rv.ret = muse::make_ok();
    rv.val = measures.at(static_cast<size_t>(bar - 1));
    return rv;
}

RetVal<std::string> optString(const Params& p, const std::string& key, const std::vector<std::string>& allowed,
                              const std::string& def)
{
    RetVal<std::string> rv;
    rv.ret = muse::make_ok();
    rv.val = def;
    auto it = p.find(key);
    if (it == p.end() || it->second.isNull()) {
        return rv;
    }
    if (it->second.type() != Val::Type::String) {
        rv.ret = makeErr(Err::InvalidArg, "argument '" + key + "' must be a string");
        return rv;
    }
    rv.val = it->second.toString();
    if (!allowed.empty() && std::find(allowed.begin(), allowed.end(), rv.val) == allowed.end()) {
        std::string list;
        for (const std::string& a : allowed) {
            list += (list.empty() ? "" : ", ") + a;
        }
        rv.ret = makeErr(Err::InvalidArg, "argument '" + key + "' has value '" + rv.val + "'; expected one of " + list);
    }
    return rv;
}

bool has(const Params& p, const std::string& key)
{
    auto it = p.find(key);
    return it != p.end() && !it->second.isNull();
}

// ---------------------------------------------------------------------------------------------
// Post-condition helpers
// ---------------------------------------------------------------------------------------------

std::set<const Spanner*> spannersAt(MasterScore* score, const Fraction& tick)
{
    std::set<const Spanner*> out;
    for (const auto& iv : score->spannerMap().findOverlapping(tick.ticks(), tick.ticks() + 1)) {
        out.insert(iv.value);
    }
    return out;
}

bool inSpannerMap(MasterScore* score, const Spanner* sp)
{
    for (const auto& iv : score->spannerMap().findOverlapping(sp->tick().ticks(), sp->tick().ticks() + 1)) {
        if (iv.value == sp) {
            return true;
        }
    }
    return false;
}

//! How many copies Score::undoAddElement makes of a per-staff spanner (slur, ottava, trill,
//! pedal, staff text line) added on `track`: the staff itself, and every linked staff that
//! maps the voice or, for voice 1, any linked staff (VOICE1_COPY_TYPES), edit.cpp:6940-7007.
size_t expectedStaffCopies(const Staff* ostaff, track_idx_t track)
{
    size_t n = 0;
    for (const Staff* staff : ostaff->staffList()) {
        if (staff == ostaff || ostaff->getLinkedTrackInStaff(staff, track) != muse::nidx || track2voice(track) == 0) {
            ++n;
        }
    }
    return n;
}

//! System objects (volta, system text line) get one copy on staff 0 of every score plus each
//! score's system-object staves (edit.cpp:6719-6731)
size_t expectedSystemCopies(MasterScore* score)
{
    size_t n = 0;
    for (Score* s : score->scoreList()) {
        n += s->staff(0) ? 1 : 0;
        for (Staff* st : s->systemObjectStaves()) {
            n += (st && st->idx() != muse::nidx) ? 1 : 0;
        }
    }
    return n;
}

//! The compact notation of bars from..to on the given staves (1-based), all voices
std::string printBars(MasterScore* score, int from, int to, std::vector<int> staves)
{
    std::sort(staves.begin(), staves.end());
    staves.erase(std::unique(staves.begin(), staves.end()), staves.end());
    read::ReadOptions opt;
    opt.fromBar = from;
    opt.toBar = to;
    opt.staves = staves;
    return notation::write(read::printScore(score, opt));
}

// ---------------------------------------------------------------------------------------------
// Line templates (palettecreator.cpp:1154-1301)
// ---------------------------------------------------------------------------------------------

const std::vector<std::string> KINDS { "pedal", "ottava", "trill", "volta", "textLine" };
const std::vector<std::string> OTTAVA_TYPES { "8va", "8vb", "15ma", "15mb", "22ma", "22mb" };
const std::vector<std::string> TRILL_TYPES { "trill", "upprall", "downprall", "prallprall" };
const std::vector<std::string> PEDAL_STYLES { "line", "asterisk", "straight", "angledEnd", "angledBoth", "angledStart" };
const std::vector<std::string> VOLTA_TYPES { "closed", "open" };
const std::vector<std::string> HOOKS { "none", "hook90", "hook45", "hook90t", "arrow", "arrowFilled" };
const std::vector<std::string> PLACEMENTS { "default", "above", "below" };

HookType hookType(const std::string& s)
{
    if (s == "hook90") {
        return HookType::HOOK_90;
    } else if (s == "hook45") {
        return HookType::HOOK_45;
    } else if (s == "hook90t") {
        return HookType::HOOK_90T;
    } else if (s == "arrow") {
        return HookType::ARROW;
    } else if (s == "arrowFilled") {
        return HookType::ARROW_FILLED;
    }
    return HookType::NONE;
}

//! Which kind-specific arguments each kind takes; any other is refused so nothing is silently ignored
const std::map<std::string, std::set<std::string> > KIND_ARGS {
    { "pedal", { "pedalStyle" } },
    { "ottava", { "ottavaType" } },
    { "trill", { "trillType" } },
    { "volta", { "endings", "voltaType", "text" } },
    { "textLine", { "text", "continueText", "endText", "endHook", "system" } },
};
const std::set<std::string> ALL_KIND_ARGS { "pedalStyle", "ottavaType", "trillType", "endings", "voltaType", "text",
                                            "continueText", "endText", "endHook", "system" };
}

// =============================================================================================
// add_slur
// =============================================================================================

Ret mu::agent::lines::addSlur(MasterScore* score, const Params& p, std::map<std::string, Val>& out)
{
    // 1. validate (nothing changes before the call to Score::addSlur)
    if (!has(p, "start") || !has(p, "end")) {
        return makeErr(Err::InvalidArg, "start and end are required");
    }
    RetVal<EventRef> start = resolveEvent(score, p.at("start"), "start");
    if (!start.ret) {
        return start.ret;
    }
    RetVal<EventRef> end = resolveEvent(score, p.at("end"), "end");
    if (!end.ret) {
        return end.ret;
    }
    RetVal<std::string> direction = optString(p, "direction", { "auto", "up", "down" }, "auto");
    if (!direction.ret) {
        return direction.ret;
    }
    RetVal<std::string> style = optString(p, "style", { "solid", "dashed", "dotted", "wideDashed" }, "solid");
    if (!style.ret) {
        return style.ret;
    }

    ChordRest* cr1 = start.val.cr;
    ChordRest* cr2 = end.val.cr;
    // the list branch drops non-chords (notationinteraction.cpp:2939-2941)
    if (!cr1->isChord()) {
        return makeErr(Err::InvalidArg, "start (" + where(score, cr1) + ") is a rest; a slur starts and ends on chords or notes");
    }
    if (!cr2->isChord()) {
        return makeErr(Err::InvalidArg, "end (" + where(score, cr2) + ") is a rest; a slur starts and ends on chords or notes");
    }
    if (cr1 == cr2) {
        return makeErr(Err::InvalidArg, "start and end are the same chord (" + where(score, cr1)
                       + "); a slur joins two different chords");
    }
    // trill cue notes are skipped silently (notationinteraction.cpp:2957-2960)
    if (toChord(cr1)->isTrillCueNote() || toChord(cr2)->isTrillCueNote()) {
        return makeErr(Err::InvalidArg, "a trill cue note cannot start or end a slur");
    }
    // a cross-part slur keeps track2 = track while its end is in another part (edit.cpp:741-746)
    if (cr1->part() != cr2->part()) {
        return makeErr(Err::InvalidArg, "start (" + where(score, cr1) + ") and end (" + where(score, cr2)
                       + ") are in different parts; a slur stays within one part (cross-staff is fine within a part)");
    }
    if (cr1->staff() != cr2->staff() && cr2->staff()->isLinked(cr1->staff())) {
        return makeErr(Err::InvalidArg, "start and end are on two linked staves of one part (e.g. a staff and its tablature); "
                       "put both ends on the same staff");
    }
    // the UI orders the two items itself (isBefore); the agent asks for them in order
    if (cr2->isBefore(cr1)) {
        return makeErr(Err::Range, "end (" + where(score, cr2) + ") comes before start (" + where(score, cr1) + "); swap them");
    }
    if (!cr1->isBefore(cr2)) {
        return makeErr(Err::InvalidArg, "start (" + where(score, cr1) + ") and end (" + where(score, cr2)
                       + ") are at the same time; a slur needs its end after its start");
    }
    // doAddSlur adds nothing when an AUTO slur with these ends exists (notationinteraction.cpp:3018-3020).
    // Deliberately stricter than the UI: it would stack a second slur on an up/down one with the
    // same ends; the agent refuses any slur that already joins these two chords.
    if (Slur* existing = cr1->slur(cr2)) {
        return makeErr(Err::Exists, "a slur from " + where(score, cr1) + " to " + where(score, cr2) + " already exists ("
                       + core::makeHandle(existing) + ")");
    }

    // 2. edit
    const std::set<const Spanner*> before = spannersAt(score, cr1->tick());
    Slur* slur = score->addSlur(cr1, cr2, nullptr);
    if (!slur) {
        return makeErr(Err::Refused, "MuseScore did not add the slur");
    }
    if (direction.val != "auto") {
        slur->undoChangeProperty(Pid::SLUR_DIRECTION, PropertyValue(direction.val == "up" ? DirectionV::UP : DirectionV::DOWN));
    }
    if (style.val != "solid") {
        const SlurStyleType st = style.val == "dashed" ? SlurStyleType::Dashed
                                 : style.val == "dotted" ? SlurStyleType::Dotted : SlurStyleType::WideDashed;
        slur->undoChangeProperty(Pid::SLUR_STYLE_TYPE, PropertyValue(st));
    }

    // 3. check the effect on the spanner model (no layout until commit, spec F2)
    if (before.count(slur) || !inSpannerMap(score, slur)) {
        return core::expect(false, "the new slur is not in the score's spanner list");
    }
    if (Ret r = core::expect(slur->tick() == cr1->tick() && slur->tick2() == cr2->tick()
                             && slur->track() == cr1->track() && slur->track2() == cr2->track()
                             && slur->startElement() == cr1 && slur->endElement() == cr2,
                             "the slur's ends are not the chords asked for"); !r) {
        return r;
    }
    const size_t copies = expectedStaffCopies(cr1->staff(), cr1->track());
    if (Ret r = core::expect(slur->linkList().size() == copies,
                             "the slur reached " + std::to_string(slur->linkList().size()) + " of "
                             + std::to_string(copies) + " linked staves"); !r) {
        return r;
    }
    if (direction.val != "auto") {
        const DirectionV want = direction.val == "up" ? DirectionV::UP : DirectionV::DOWN;
        if (Ret r = core::expect(slur->slurDirection() == want, "the slur's direction was not set"); !r) {
            return r;
        }
    }

    out["handle"] = Val(core::makeHandle(slur));
    out["startBar"] = Val(start.val.bar);
    out["endBar"] = Val(end.val.bar);
    out["bars"] = Val(printBars(score, start.val.bar, end.val.bar,
                                { static_cast<int>(cr1->staffIdx()) + 1, static_cast<int>(cr2->staffIdx()) + 1 }));
    return muse::make_ok();
}

// =============================================================================================
// add_line
// =============================================================================================

Ret mu::agent::lines::addLine(MasterScore* score, const Params& p, std::map<std::string, Val>& out)
{
    // 1. validate
    RetVal<std::string> kind = optString(p, "kind", KINDS, "");
    if (!kind.ret) {
        return kind.ret;
    }
    if (kind.val.empty()) {
        return makeErr(Err::InvalidArg, "kind is required: pedal, ottava, trill, volta or textLine");
    }
    if (!has(p, "start") || !has(p, "end")) {
        return makeErr(Err::InvalidArg, "start and end are required");
    }
    const std::set<std::string>& mine = KIND_ARGS.at(kind.val);
    for (const std::string& a : ALL_KIND_ARGS) {
        if (has(p, a) && !mine.count(a)) {
            return makeErr(Err::InvalidArg, "argument '" + a + "' does not apply to kind " + kind.val);
        }
    }
    RetVal<std::string> placement = optString(p, "placement", PLACEMENTS, "default");
    if (!placement.ret) {
        return placement.ret;
    }
    RetVal<std::string> ottavaType = optString(p, "ottavaType", OTTAVA_TYPES, "8va");
    RetVal<std::string> trillType = optString(p, "trillType", TRILL_TYPES, "trill");
    RetVal<std::string> pedalStyle = optString(p, "pedalStyle", PEDAL_STYLES, "line");
    RetVal<std::string> voltaType = optString(p, "voltaType", VOLTA_TYPES, "closed");
    RetVal<std::string> endHook = optString(p, "endHook", HOOKS, "hook90");
    RetVal<std::string> text = optString(p, "text", {}, "");
    RetVal<std::string> continueText = optString(p, "continueText", {}, "");
    RetVal<std::string> endText = optString(p, "endText", {}, "");
    for (const RetVal<std::string>* r : { &ottavaType, &trillType, &pedalStyle, &voltaType, &endHook, &text, &continueText, &endText }) {
        if (!r->ret) {
            return r->ret;
        }
    }
    bool system = false;
    if (has(p, "system")) {
        if (p.at("system").type() != Val::Type::Bool) {
            return makeErr(Err::InvalidArg, "argument 'system' must be true or false");
        }
        system = p.at("system").toBool();
    }
    std::vector<int> endings { 1 };
    if (has(p, "endings")) {
        const Val& e = p.at("endings");
        if (e.type() != Val::Type::List || e.toList().empty()) {
            return makeErr(Err::InvalidArg, "endings must be a non-empty list of ending numbers, e.g. [1] or [1, 2]");
        }
        endings.clear();
        for (const Val& n : e.toList()) {
            if (!isInteger(n) || n.toDouble() < 1 || n.toDouble() > 100) {
                return makeErr(Err::InvalidArg, "endings must be whole numbers from 1 (the passes that play this ending)");
            }
            endings.push_back(static_cast<int>(n.toDouble()));
        }
        std::sort(endings.begin(), endings.end());
        if (std::adjacent_find(endings.begin(), endings.end()) != endings.end()) {
            return makeErr(Err::InvalidArg, "endings lists the same number twice");
        }
    }

    const bool isVolta = kind.val == "volta";
    const bool isSystemLine = isVolta || (kind.val == "textLine" && system);
    staff_idx_t staffIdx = 0;
    Segment* startSeg = nullptr;
    Segment* endSeg = nullptr;
    int startBar = 0;
    int endBar = 0;

    if (isVolta) {
        // a range selection of whole bars (score.cpp:3546-3568)
        RetVal<Measure*> m1 = resolveBar(score, p.at("start"), "start");
        if (!m1.ret) {
            return m1.ret;
        }
        RetVal<Measure*> m2 = resolveBar(score, p.at("end"), "end");
        if (!m2.ret) {
            return m2.ret;
        }
        startBar = barOf(score, m1.val);
        endBar = barOf(score, m2.val);
        if (endBar < startBar) {
            return makeErr(Err::Range, "end bar m" + std::to_string(endBar) + " comes before start bar m" + std::to_string(startBar));
        }
        startSeg = m1.val->first(SegmentType::ChordRest);
        endSeg = m2.val->nextMeasure() ? m2.val->last() : nullptr;
        if (!startSeg) {
            return makeErr(Err::Range, "bar m" + std::to_string(startBar) + " has no chord/rest segment to start a volta on");
        }
    } else {
        RetVal<EventRef> start = resolveEvent(score, p.at("start"), "start");
        if (!start.ret) {
            return start.ret;
        }
        RetVal<EventRef> end = resolveEvent(score, p.at("end"), "end");
        if (!end.ret) {
            return end.ret;
        }
        ChordRest* cr1 = start.val.cr;
        ChordRest* cr2 = end.val.cr;
        if (cr1->isGrace() || cr2->isGrace()) {
            return makeErr(Err::InvalidArg, "a line starts and ends on chords or rests, not on grace notes");
        }
        if (cr1->staffIdx() != cr2->staffIdx()) {
            return makeErr(Err::InvalidArg, "start (" + where(score, cr1) + ") and end (" + where(score, cr2)
                           + ") are on different staves; a line goes on one staff");
        }
        if (cr2->tick() < cr1->tick()) {
            return makeErr(Err::Range, "end (" + where(score, cr2) + ") comes before start (" + where(score, cr1) + "); swap them");
        }
        // a range selection from cr1 to cr2 (score.cpp:3591-3604, chordrest.cpp:811-820)
        staffIdx = isSystemLine ? 0 : cr1->staffIdx(); // firstStaffOnly (notationinteraction.cpp:2649-2651)
        startSeg = cr1->segment();
        endSeg = cr2->nextSegmentAfterCR(SegmentType::ChordRest | SegmentType::EndBarLine | SegmentType::Clef);
        startBar = start.val.bar;
        endBar = end.val.bar;
    }

    // 2. build the template the palette would hold, then apply it as the palette does
    Spanner* templ = nullptr;
    if (kind.val == "pedal") {
        Pedal* pedal = Factory::createPedal(score->dummy());
        const std::string& s = pedalStyle.val;
        if (s == "line") {
            pedal->setEndHookType(HookType::HOOK_90);
        } else if (s == "asterisk") {
            pedal->setLineVisible(false);
            pedal->setEndHookType(HookType::ROSETTE);
        } else if (s == "straight") {
            pedal->setBeginHookType(HookType::HOOK_90);
            pedal->setEndHookType(HookType::HOOK_90);
        } else if (s == "angledEnd") {
            pedal->setBeginHookType(HookType::HOOK_90);
            pedal->setEndHookType(HookType::HOOK_45);
        } else if (s == "angledBoth") {
            pedal->setBeginHookType(HookType::HOOK_45);
            pedal->setEndHookType(HookType::HOOK_45);
        } else {
            pedal->setBeginHookType(HookType::HOOK_45);
            pedal->setEndHookType(HookType::HOOK_90);
        }
        pedal->setBeginText(pedal->propertyDefault(Pid::BEGIN_TEXT).value<String>());
        pedal->setContinueText(pedal->propertyDefault(Pid::CONTINUE_TEXT).value<String>());
        pedal->setEndText(pedal->propertyDefault(Pid::END_TEXT).value<String>());
        templ = pedal;
    } else if (kind.val == "ottava") {
        Ottava* ottava = Factory::createOttava(score->dummy());
        const size_t i = static_cast<size_t>(std::find(OTTAVA_TYPES.begin(), OTTAVA_TYPES.end(), ottavaType.val) - OTTAVA_TYPES.begin());
        static const OttavaType types[] = { OttavaType::OTTAVA_8VA, OttavaType::OTTAVA_8VB, OttavaType::OTTAVA_15MA,
                                            OttavaType::OTTAVA_15MB, OttavaType::OTTAVA_22MA, OttavaType::OTTAVA_22MB };
        ottava->setOttavaType(types[i]);
        ottava->styleChanged();
        templ = ottava;
    } else if (kind.val == "trill") {
        Trill* trill = Factory::createTrill(score->dummy());
        const size_t i = static_cast<size_t>(std::find(TRILL_TYPES.begin(), TRILL_TYPES.end(), trillType.val) - TRILL_TYPES.begin());
        static const TrillType types[] = { TrillType::TRILL_LINE, TrillType::UPPRALL_LINE, TrillType::DOWNPRALL_LINE,
                                           TrillType::PRALLPRALL_LINE };
        trill->setTrillType(types[i]); // always: it creates the ornament linkedClone dereferences (trill.cpp:210-217)
        templ = trill;
    } else if (kind.val == "volta") {
        Volta* volta = Factory::createVolta(score->dummy());
        volta->setVoltaType(voltaType.val == "open" ? Volta::Type::OPEN : Volta::Type::CLOSED);
        std::string label = text.val;
        if (!has(p, "text")) {
            for (int e : endings) {
                label += (label.empty() ? "" : ", ") + std::to_string(e) + ".";
            }
        }
        volta->setText(TextBase::plainToXmlText(String::fromStdString(label)));
        volta->setEndings(endings);
        templ = volta;
    } else {
        TextLine* line = new TextLine(score->dummy(), system); // as the palette builds it (palettecreator.cpp:1271-1280)
        line->setBeginText(TextBase::plainToXmlText(String::fromStdString(text.val)));
        if (has(p, "continueText")) {
            line->setContinueText(TextBase::plainToXmlText(String::fromStdString(continueText.val)));
        }
        if (has(p, "endText")) {
            line->setEndText(TextBase::plainToXmlText(String::fromStdString(endText.val)));
        }
        line->setEndHookType(hookType(endHook.val));
        templ = line;
    }

    Spanner* spanner = toSpanner(templ->clone());
    delete templ;
    spanner->setScore(score);
    spanner->styleChanged();

    // what cmdAddSpanner will derive from the segments (cmd.cpp:603-633)
    const bool measureAnchor = spanner->anchor() == Spanner::Anchor::MEASURE;
    const Fraction tick1 = measureAnchor ? startSeg->measure()->tick() : startSeg->tick();
    Fraction tick2 = !endSeg ? score->lastSegment()->tick()
                     : endSeg == startSeg ? startSeg->measure()->last()->tick() : endSeg->tick();
    if (measureAnchor) {
        Measure* endMeasure = score->tick2measureMM(tick2);
        if (!endMeasure) {
            delete spanner;
            return makeErr(Err::Range, "the line's end is past the last bar");
        }
        if (endMeasure->tick() != tick2) {
            tick2 = endMeasure->endTick();
        }
    }
    const track_idx_t track = staffIdx * VOICES;
    const ElementType type = spanner->type();

    const std::set<const Spanner*> before = spannersAt(score, tick1);
    score->cmdAddSpanner(spanner, staffIdx, startSeg, endSeg, false); // ownership passes to the score's undo stack
    if (placement.val != "default") {
        spanner->undoChangeProperty(Pid::PLACEMENT, PropertyValue(placement.val == "above" ? PlacementV::ABOVE : PlacementV::BELOW));
    }

    // 3. check the effect on the spanner model
    if (before.count(spanner) || !inSpannerMap(score, spanner)) {
        return core::expect(false, "the new " + kind.val + " is not in the score's spanner list");
    }
    if (Ret r = core::expect(spanner->type() == type && spanner->tick() == tick1 && spanner->tick2() == tick2
                             && spanner->track() == track && spanner->track2() == track,
                             "the " + kind.val + " is not where it was put"); !r) {
        return r;
    }
    if (Ret r = core::expect(spanner->tick2() > spanner->tick(), "the " + kind.val + " has no length"); !r) {
        return r;
    }
    const size_t copies = isSystemLine ? expectedSystemCopies(score) : expectedStaffCopies(score->staff(staffIdx), track);
    if (Ret r = core::expect(spanner->linkList().size() == copies,
                             "the " + kind.val + " reached " + std::to_string(spanner->linkList().size()) + " of "
                             + std::to_string(copies) + " staves it should be copied to"); !r) {
        return r;
    }
    if (isSystemLine) {
        if (Ret r = core::expect(spanner->systemFlag(), "the " + kind.val + " lost its system flag"); !r) {
            return r;
        }
    }
    if (kind.val == "trill") {
        if (Ret r = core::expect(toTrill(spanner)->ornament() != nullptr, "the trill has no ornament"); !r) {
            return r;
        }
    }

    out["handle"] = Val(core::makeHandle(spanner));
    out["kind"] = Val(kind.val);
    out["startBar"] = Val(startBar);
    out["endBar"] = Val(endBar);
    out["bars"] = Val(printBars(score, startBar, endBar, { static_cast<int>(staffIdx) + 1 }));
    return muse::make_ok();
}
