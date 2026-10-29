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
#include "handles.h"

#include <cctype>
#include <vector>

#include "engraving/dom/arpeggio.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tie.h"
#include "engraving/infrastructure/eid.h"

#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::agent::core;
using namespace mu::engraving;
using muse::RetVal;

namespace {
std::string fingerprint(const EngravingItem* e)
{
    if (e->isNote()) {
        return std::to_string(toNote(e)->pitch());
    }
    if (e->isChordRest()) {
        return toChordRest(e)->actualTicks().reduced().toString().toStdString();
    }
    return {};
}

bool validEidText(const std::string& s)
{
    if (s.empty() || s.size() > EID::MAX_STR_SIZE) {
        return false;
    }
    int separators = 0;
    for (const char c : s) {
        if (c == '_') {
            ++separators;
        } else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '/') {
            return false;
        }
    }
    return separators == 1 && s.front() != '_' && s.back() != '_';
}

bool readInt(const std::string& s, size_t& pos, int& out)
{
    const size_t start = pos;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    if (pos == start || pos - start > 9) {
        return false;
    }
    out = std::stoi(s.substr(start, pos - start));
    return true;
}

const Measure* measureAt(const MasterScore* score, int bar)
{
    int idx = 0;
    for (const Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        if (++idx == bar) {
            return m;
        }
    }
    return nullptr;
}

int barOf(const MasterScore* score, const Measure* target)
{
    int idx = 0;
    for (const Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        ++idx;
        if (m == target) {
            return idx;
        }
    }
    return 0;
}

void addChordRest(ChordRest* cr, std::vector<EngravingItem*>& out)
{
    out.push_back(cr);
    for (Lyrics* l : cr->lyrics()) {
        out.push_back(l);
    }
    if (cr->isChord()) {
        Chord* ch = toChord(cr);
        for (Note* n : ch->notes()) {
            out.push_back(n);
            if (n->tieFor()) {
                out.push_back(n->tieFor());
            }
            for (EngravingItem* e : n->el()) { // fingering, note-attached text and symbols
                out.push_back(e);
            }
            for (Spanner* sp : n->spannerFor()) { // glissandi, note-anchored lines
                out.push_back(sp);
            }
        }
        if (ch->arpeggio()) {
            out.push_back(ch->arpeggio());
        }
        for (Articulation* a : ch->articulations()) {
            out.push_back(a);
        }
        for (Chord* g : ch->graceNotes()) {
            addChordRest(g, out);
        }
    }
}

//! Every element that can carry a handle at (tick, track), found by walking the structure
std::vector<EngravingItem*> elementsAt(MasterScore* score, Measure* m, const Fraction& tick, track_idx_t track)
{
    std::vector<EngravingItem*> out;
    // measure-level items (the bar itself, markers, jumps, breaks) belong to no staff or voice
    out.push_back(m);
    for (EngravingItem* e : m->el()) {
        out.push_back(e);
    }
    for (Segment* s = m->first(); s; s = s->next()) {
        if (s->tick() != tick) {
            continue;
        }
        if (EngravingItem* e = s->element(track)) {
            if (e->isChordRest()) {
                addChordRest(toChordRest(e), out);
            } else {
                out.push_back(e);
            }
        }
        for (EngravingItem* a : s->annotations()) {
            if (a->track() == track) {
                out.push_back(a);
            }
        }
    }
    for (const auto& iv : score->spannerMap().findOverlapping(tick.ticks(), tick.ticks())) {
        Spanner* sp = iv.value;
        if (sp->tick() == tick && sp->track() == track) {
            out.push_back(sp);
        }
    }
    return out;
}
}

std::string mu::agent::core::formatHandle(const Handle& h)
{
    std::string s = h.type + ":" + h.eid + "@m" + std::to_string(h.bar);
    if (h.offset.numerator() != 0) {
        s += "+" + h.offset.reduced().toString().toStdString();
    }
    s += ".s" + std::to_string(h.staff) + ".v" + std::to_string(h.voice);
    if (!h.key.empty()) {
        s += "#" + h.key;
    }
    return s;
}

RetVal<Handle> mu::agent::core::parseHandle(const std::string& text)
{
    RetVal<Handle> rv;
    auto bad = [&](const std::string& why) {
        rv.ret = makeErr(Err::InvalidArg, "malformed handle \"" + text.substr(0, 80) + "\": " + why);
        return rv;
    };
    const size_t colon = text.find(':');
    const size_t at = text.find('@');
    if (colon == std::string::npos || at == std::string::npos || colon == 0 || at < colon) {
        return bad("expected <Type>:<eid>@m<bar>...");
    }
    Handle h;
    h.type = text.substr(0, colon);
    h.eid = text.substr(colon + 1, at - colon - 1);
    if (!validEidText(h.eid)) {
        return bad("the element id is not valid");
    }
    size_t pos = at + 1;
    if (pos >= text.size() || text[pos++] != 'm' || !readInt(text, pos, h.bar) || h.bar < 1) {
        return bad("expected m<bar>");
    }
    if (pos < text.size() && text[pos] == '+') {
        int num = 0, den = 0;
        ++pos;
        if (!readInt(text, pos, num) || pos >= text.size() || text[pos++] != '/' || !readInt(text, pos, den) || den == 0) {
            return bad("expected +<num>/<den> after the bar");
        }
        h.offset = Fraction(num, den).reduced();
    }
    if (text.compare(pos, 2, ".s") != 0 || (pos += 2, !readInt(text, pos, h.staff)) || h.staff < 1) {
        return bad("expected .s<staff>");
    }
    if (text.compare(pos, 2, ".v") != 0 || (pos += 2, !readInt(text, pos, h.voice)) || h.voice < 1 || h.voice > 4) {
        return bad("expected .v<voice 1-4>");
    }
    if (pos < text.size()) {
        if (text[pos] != '#') {
            return bad("unexpected text after the voice");
        }
        h.key = text.substr(pos + 1);
    }
    rv.ret = muse::make_ok();
    rv.val = h;
    return rv;
}

std::string mu::agent::core::makeHandle(const EngravingItem* item)
{
    if (!item) {
        return {};
    }
    EID eid = item->eid();
    if (!eid.isValid()) {
        eid = item->assignNewEID();
    }
    const MasterScore* score = item->masterScore();
    const Fraction tick = item->tick();
    // the measure that owns it, not the one at its tick: an end barline or a courtesy clef sits at
    // the next bar's tick but in this bar's last segment
    const Measure* m = item->isMeasure() ? toMeasure(item) : item->findMeasure();
    if (!m) {
        m = score->tick2measure(tick);
    }
    const bool perStaff = item->track() != muse::nidx;

    Handle h;
    h.type = item->typeName();
    h.eid = eid.toStdString();
    h.bar = m ? barOf(score, m) : 0;
    h.offset = m ? (tick - m->tick()).reduced() : Fraction(0, 1);
    h.staff = perStaff ? static_cast<int>(item->staffIdx()) + 1 : 1;
    h.voice = perStaff ? static_cast<int>(item->voice()) + 1 : 1;
    h.key = fingerprint(item);
    return formatHandle(h);
}

RetVal<EngravingItem*> mu::agent::core::resolveHandle(MasterScore* score, const std::string& text)
{
    RetVal<EngravingItem*> rv;
    rv.val = nullptr;
    RetVal<Handle> parsed = parseHandle(text);
    if (!parsed.ret) {
        rv.ret = parsed.ret;
        return rv;
    }
    const Handle& h = parsed.val;
    Measure* m = const_cast<Measure*>(measureAt(score, h.bar));
    if (!m) {
        rv.ret = makeErr(Err::Range, "handle " + text + ": bar " + std::to_string(h.bar) + " is outside the score");
        return rv;
    }
    if (h.staff > static_cast<int>(score->nstaves())) {
        rv.ret = makeErr(Err::Range, "handle " + text + ": staff " + std::to_string(h.staff) + " is outside the score");
        return rv;
    }
    const track_idx_t track = static_cast<track_idx_t>(h.staff - 1) * VOICES + static_cast<track_idx_t>(h.voice - 1);
    for (EngravingItem* e : elementsAt(score, m, m->tick() + h.offset, track)) {
        if (h.type != e->typeName() || fingerprint(e) != h.key) {
            continue;
        }
        const EID eid = e->eid();
        if (eid.isValid() && eid.toStdString() == h.eid) {
            rv.ret = muse::make_ok();
            rv.val = e;
            return rv;
        }
    }
    rv.ret = makeErr(Err::StaleHandle, "handle " + text + " no longer names an element in the score "
                     "(it was removed, moved or changed); read the bars again for fresh handles");
    return rv;
}
