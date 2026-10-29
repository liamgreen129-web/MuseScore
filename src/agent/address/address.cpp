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
#include "address.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "engraving/dom/chord.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"

#include "core/handles.h"
#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::engraving;
using muse::Ret;
using muse::Val;

namespace {
bool isNumber(const Val& v)
{
    return v.type() == Val::Type::Double || v.type() == Val::Type::Int || v.type() == Val::Type::Int64;
}

bool isWhole(const Val& v)
{
    return isNumber(v) && std::isfinite(v.toDouble()) && v.toDouble() == std::floor(v.toDouble());
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const char* STAFF_SHAPES = "a staff number (as # sN in score_read), {\"number\": N} or {\"part\": \"<name>\", \"index\": 1}";
}

bool address::has(const Params& p, const char* key)
{
    auto it = p.find(key);
    return it != p.end() && !it->second.isNull();
}

std::string address::number(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s(buf);
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s;
}

std::vector<Measure*> address::allMeasures(MasterScore* score)
{
    std::vector<Measure*> out;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        out.push_back(m);
    }
    return out;
}

int address::barOf(MasterScore* score, const Measure* target)
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

std::string address::beatOf(const Measure* m, const Fraction& tick)
{
    const double q = static_cast<double>((tick - m->tick()).ticks()) / Constants::DIVISION;
    return "beat " + number(q + 1.0);
}

std::string address::where(MasterScore* score, const ChordRest* cr)
{
    const Measure* m = cr->measure();
    return "m" + std::to_string(barOf(score, m)) + " " + beatOf(m, cr->tick()) + " (staff "
           + std::to_string(cr->staffIdx() + 1) + ", voice " + std::to_string(cr->voice() + 1) + ")";
}

Ret address::resolveStaff(MasterScore* score, const Val& v, staff_idx_t& out)
{
    const int nstaves = static_cast<int>(score->nstaves());
    auto byNumber = [&](const Val& n) -> Ret {
        if (!isWhole(n)) {
            return makeErr(Err::InvalidArg, "a staff number must be a whole number");
        }
        const int num = static_cast<int>(n.toDouble());
        if (num < 1 || num > nstaves) {
            return makeErr(Err::Range, "staff " + std::to_string(num) + " is outside 1-" + std::to_string(nstaves));
        }
        out = static_cast<staff_idx_t>(num - 1);
        return muse::make_ok();
    };
    if (isNumber(v)) {
        return byNumber(v);
    }
    if (v.type() != Val::Type::Map) {
        return makeErr(Err::InvalidArg, std::string("staff must be ") + STAFF_SHAPES);
    }
    const muse::ValMap m = v.toMap();
    for (const auto& [k, _] : m) {
        if (k != "part" && k != "index" && k != "number") {
            return makeErr(Err::InvalidArg, "staff has an unknown key '" + k + "' (part, index or number)");
        }
    }
    if (m.count("number")) {
        if (m.count("part") || m.count("index")) {
            return makeErr(Err::InvalidArg, "staff takes either number, or part (+ index), not both");
        }
        return byNumber(m.at("number"));
    }
    if (!m.count("part") || m.at("part").type() != Val::Type::String) {
        return makeErr(Err::InvalidArg, std::string("staff must be ") + STAFF_SHAPES);
    }
    int index = 1;
    if (m.count("index")) {
        if (!isWhole(m.at("index")) || m.at("index").toDouble() < 1) {
            return makeErr(Err::InvalidArg, "staff.index must be a whole number from 1");
        }
        index = static_cast<int>(m.at("index").toDouble());
    }
    const std::string want = lower(m.at("part").toString());
    std::vector<const Part*> found;
    std::string names;
    for (const Part* part : score->parts()) {
        const std::string name = part->partName().toStdString();
        names += (names.empty() ? "" : ", ") + ("\"" + name + "\"");
        if (lower(name) == want) {
            found.push_back(part);
        }
    }
    if (found.empty()) {
        return makeErr(Err::Range, "no part named \"" + m.at("part").toString() + "\"; the parts are " + names);
    }
    if (found.size() > 1) {
        return makeErr(Err::InvalidArg, std::to_string(found.size()) + " parts are named \"" + m.at("part").toString()
                       + "\"; give the staff number instead (score_overview lists them)");
    }
    const Part* part = found.front();
    if (index > static_cast<int>(part->nstaves())) {
        return makeErr(Err::Range, "part \"" + part->partName().toStdString() + "\" has " + std::to_string(part->nstaves())
                       + " staff(s); index " + std::to_string(index) + " is outside it");
    }
    out = part->staves().at(index - 1)->idx();
    return muse::make_ok();
}

Ret address::resolvePosition(MasterScore* score, const Val& v, const std::string& name, Measure*& m, Fraction& tick)
{
    if (v.type() != Val::Type::Map) {
        return makeErr(Err::InvalidArg, name + " must be an object: {\"measure\": N, \"beat\": B}");
    }
    const muse::ValMap pos = v.toMap();
    if (!pos.count("measure") || !isWhole(pos.at("measure"))) {
        return makeErr(Err::InvalidArg, name + ".measure must be a whole number (the bar by position, as m<N> in score_read)");
    }
    if (pos.count("beat") && pos.count("offset")) {
        return makeErr(Err::InvalidArg, name + " takes beat or offset, not both");
    }
    const int bar = static_cast<int>(pos.at("measure").toDouble());
    const std::vector<Measure*> bars = allMeasures(score);
    if (bar < 1 || bar > static_cast<int>(bars.size())) {
        return makeErr(Err::Range, "bar " + std::to_string(bar) + " is outside the score (1-" + std::to_string(bars.size()) + ")");
    }
    m = bars.at(bar - 1);

    Fraction rel;
    if (pos.count("beat")) {
        const Val& b = pos.at("beat");
        if (!isNumber(b) || !std::isfinite(b.toDouble()) || b.toDouble() < 1.0) {
            return makeErr(Err::InvalidArg, name + ".beat must be a number from 1 (quarter-note beats)");
        }
        const double ticks = (b.toDouble() - 1.0) * Constants::DIVISION;
        const double rounded = std::round(ticks);
        if (std::fabs(ticks - rounded) > 1e-6) {
            return makeErr(Err::InvalidArg, "beat " + number(b.toDouble()) + " does not fall on a tick; give offset \"n/d\" "
                           "(whole notes from the start of the bar) for triplet positions");
        }
        rel = Fraction::fromTicks(static_cast<int>(rounded));
    } else if (pos.count("offset")) {
        const Val& o = pos.at("offset");
        bool ok = false;
        rel = o.type() == Val::Type::String ? Fraction::fromString(muse::String::fromStdString(o.toString()), &ok) : Fraction();
        if (!ok || !rel.isValid() || rel.negative()) {
            return makeErr(Err::InvalidArg, name + ".offset must be a fraction \"n/d\" of a whole note, e.g. \"3/8\"");
        }
        rel.reduce();
    }
    if (rel >= m->ticks()) {
        return makeErr(Err::Range, "m" + std::to_string(bar) + " is " + m->ticks().toString().toStdString() + " long; "
                       + beatOf(m, m->tick() + rel) + " is past its end");
    }
    tick = m->tick() + rel;
    return muse::make_ok();
}

Ret address::chordRestAt(MasterScore* score, Measure* m, const Fraction& tick, track_idx_t track, ChordRest*& cr)
{
    cr = nullptr;
    if (Segment* seg = m->findSegment(SegmentType::ChordRest, tick)) {
        EngravingItem* e = seg->element(track);
        if (e && e->isChordRest() && !(e->isRest() && toRest(e)->isGap())) {
            cr = toChordRest(e);
        }
    }
    if (cr) {
        return muse::make_ok();
    }
    std::string starts;
    for (Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
        EngravingItem* e = s->element(track);
        if (e && e->isChordRest() && !(e->isRest() && toRest(e)->isGap())) {
            starts += (starts.empty() ? "" : ", ")
                      + number(1.0 + static_cast<double>((s->tick() - m->tick()).ticks()) / Constants::DIVISION);
        }
    }
    const std::string place = "m" + std::to_string(barOf(score, m)) + " " + beatOf(m, tick) + " (staff "
                              + std::to_string(track / VOICES + 1) + ", voice " + std::to_string(track % VOICES + 1) + ")";
    return makeErr(Err::Range, "no note or rest starts at " + place + "; "
                   + (starts.empty() ? std::string("that voice has nothing in this bar")
                      : "events in that voice start at beats " + starts));
}

Ret address::resolveTarget(MasterScore* score, const Params& p, const char* key, ChordRest*& cr)
{
    cr = nullptr;
    const std::string name(key);
    if (!has(p, key)) {
        return makeErr(Err::InvalidArg, "missing " + name + ": a handle, or {\"staff\": N, \"measure\": M, \"beat\": B, \"voice\": 1}");
    }
    const Val& v = p.at(key);
    if (v.type() == Val::Type::String) {
        muse::RetVal<EngravingItem*> r = core::resolveHandle(score, v.toString());
        if (!r.ret) {
            return r.ret;
        }
        EngravingItem* e = r.val;
        if (e->isNote()) {
            e = toNote(e)->chord();
        }
        if (!e->isChordRest() || (e->isRest() && toRest(e)->isGap())) {
            return makeErr(Err::InvalidArg, name + " must name a note, chord or rest; it names a " + e->typeName());
        }
        cr = toChordRest(e);
        return muse::make_ok();
    }
    if (v.type() != Val::Type::Map) {
        return makeErr(Err::InvalidArg, name + " must be a handle or {\"staff\": N, \"measure\": M, \"beat\": B, \"voice\": 1}");
    }
    const muse::ValMap obj = v.toMap();
    for (const auto& [k, _] : obj) {
        if (k != "staff" && k != "voice" && k != "measure" && k != "beat" && k != "offset") {
            return makeErr(Err::InvalidArg, name + " has an unknown key '" + k + "' (staff, voice, measure, beat or offset)");
        }
    }
    if (!obj.count("staff")) {
        return makeErr(Err::InvalidArg, name + ".staff is required");
    }
    staff_idx_t staff = 0;
    if (Ret r = resolveStaff(score, obj.at("staff"), staff); !r) {
        return r;
    }
    int voice = 1;
    if (obj.count("voice")) {
        if (!isWhole(obj.at("voice")) || obj.at("voice").toDouble() < 1 || obj.at("voice").toDouble() > 4) {
            return makeErr(Err::Range, name + ".voice must be 1-4");
        }
        voice = static_cast<int>(obj.at("voice").toDouble());
    }
    Measure* m = nullptr;
    Fraction tick;
    if (Ret r = resolvePosition(score, v, name, m, tick); !r) {
        return r;
    }
    return chordRestAt(score, m, tick, staff * VOICES + static_cast<track_idx_t>(voice - 1), cr);
}
