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
#include "writer.h"

#include <algorithm>
#include <set>

using namespace mu::agent::notation;

static std::string quote(const std::string& s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

std::string mu::agent::notation::writePitch(const Pitch& p)
{
    std::string out(1, p.step);
    switch (p.alter) {
    case 2: out += "##"; break;
    case 1: out += "#"; break;
    case -1: out += "b"; break;
    case -2: out += "bb"; break;
    default: break;
    }
    return out + std::to_string(p.octave);
}

std::string mu::agent::notation::writeDuration(const Duration& d)
{
    static const char codes[] = { 'b', 'w', 'h', 'q', 'e', 's', 't', 'x', 'y' };
    return std::string(1, codes[static_cast<int>(d.base)]) + std::string(d.dots, '.');
}

int mu::agent::notation::markRank(const Mark& m)
{
    static const std::set<std::string> artic = { "st", "sti", "ten", "tst", "ac", "mar", "up", "dn", "harm", "open", "stop" };
    static const std::set<std::string> orn = { "tr", "mord", "imord", "turn", "iturn" };
    static const std::set<std::string> dyn = { "pppp", "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff", "ffff",
                                               "fp", "sf", "sfz", "sffz", "sfp", "rfz", "fz", "dyn" };
    const std::string& c = m.code;
    if (artic.count(c)) {
        return 0;
    }
    if (orn.count(c)) {
        return 1;
    }
    if (c == "ferm") {
        return 2;
    }
    if (c == "br" || c == "cae") {
        return 3;
    }
    if (dyn.count(c)) {
        return 4;
    }
    if (c == "<" || c == ">" || c == "!") {
        return 5;
    }
    if (m.bracket) {
        return 6;
    }
    if (c.empty() || c == "sys" || c == "exp") {
        return 7;
    }
    if (c == "tempo") {
        return 8;
    }
    if (c.compare(0, 2, "ly") == 0) {
        return 9;
    }
    if (c == "xs") {
        return 10;
    }
    if (c == "clef") {
        return 11;
    }
    return 12; // sym
}

std::string mu::agent::notation::writeMark(const Mark& m)
{
    std::string out = "+" + m.code;
    if (m.bracket) {
        out += m.bracket;
    }
    if (!m.arg.empty()) {
        out += ":" + m.arg;
    }
    if (m.text) {
        out += quote(*m.text);
    }
    if (!m.paren.empty()) {
        out += "(" + m.paren + ")";
    }
    return out;
}

static std::string writeMarks(std::vector<Mark> marks)
{
    std::stable_sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) { return markRank(a) < markRank(b); });
    std::string out;
    for (const Mark& m : marks) {
        out += writeMark(m);
    }
    return out;
}

static std::string writeBareEvent(const Event& e)
{
    std::string out;
    switch (e.kind) {
    case EventKind::Rest: out = "r:" + writeDuration(e.duration); break;
    case EventKind::Gap: out = "_:" + writeDuration(e.duration); break;
    case EventKind::FullBarRest: out = "R"; break;
    case EventKind::Note:
        out = writePitch(e.heads.at(0).pitch) + ":" + writeDuration(e.duration) + (e.heads.at(0).tie ? "~" : "");
        break;
    case EventKind::Chord: {
        const bool allTied = std::all_of(e.heads.begin(), e.heads.end(), [](const NoteHead& h) { return h.tie; });
        out = "[";
        for (size_t i = 0; i < e.heads.size(); ++i) {
            if (i) {
                out += ",";
            }
            out += writePitch(e.heads[i].pitch) + (!allTied && e.heads[i].tie ? "~" : "");
        }
        out += "]:" + writeDuration(e.duration) + (allTied ? "~" : "");
        break;
    }
    }
    return out + writeMarks(e.marks);
}

static std::string writeGraces(const std::vector<Event>& graces)
{
    std::string out;
    for (size_t i = 0; i < graces.size(); ++i) {
        if (i) {
            out += " ";
        }
        out += writeBareEvent(graces[i]);
    }
    return out;
}

static std::string writeItem(const Item& item)
{
    if (const Tuplet* t = std::get_if<Tuplet>(&item)) {
        return std::string(t->slurOpen, '(') + "{" + std::to_string(t->actual) + ":" + std::to_string(t->normal) + " "
               + writeItems(t->items) + "}" + std::string(t->slurClose, ')');
    }
    const Event& e = std::get<Event>(item);
    std::string out;
    if (e.graceBeforeKind != GraceKind::None) {
        out += (e.graceBeforeKind == GraceKind::Acciaccatura ? "g/(" : "g(") + writeGraces(e.graceBefore) + ") ";
    }
    out += std::string(e.slurOpen, '(') + writeBareEvent(e) + std::string(e.slurClose, ')');
    if (!e.graceAfter.empty()) {
        out += " ga(" + writeGraces(e.graceAfter) + ")";
    }
    return out;
}

std::string mu::agent::notation::writeItems(const std::vector<Item>& items)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) {
            out += " ";
        }
        out += writeItem(items[i]);
    }
    return out;
}

static std::string writeBarRef(const BarRef& b)
{
    return "m" + std::to_string(b.index) + (b.display ? "(" + *b.display + ")" : "");
}

static std::string writeKeyValues(const std::vector<KeyValue>& kvs)
{
    std::string out;
    for (const KeyValue& kv : kvs) {
        out += " " + kv.key + "=" + kv.value;
    }
    return out;
}

std::string mu::agent::notation::write(const Document& doc)
{
    std::string out;
    for (const Line& line : doc.lines) {
        if (const StaffHeader* h = std::get_if<StaffHeader>(&line)) {
            out += "# s" + std::to_string(h->staff) + " " + quote(h->name) + writeKeyValues(h->attrs);
        } else if (const BarAttrs* a = std::get_if<BarAttrs>(&line)) {
            out += writeBarRef(a->bar) + " @" + writeKeyValues(a->attrs);
        } else if (const VoiceLine* v = std::get_if<VoiceLine>(&line)) {
            out += writeBarRef(v->bar) + " v" + std::to_string(v->voice) + " | " + writeItems(v->items);
        } else if (const HarmonyLine* hl = std::get_if<HarmonyLine>(&line)) {
            out += writeBarRef(hl->bar) + " h |";
            for (const HarmonyItem& hi : hl->items) {
                out += " " + (hi.chord ? "{" + *hi.chord + "}" : std::string("_")) + ":" + writeDuration(hi.duration);
            }
        } else if (const Comment* c = std::get_if<Comment>(&line)) {
            out += "%" + c->text;
        }
        out += "\n";
    }
    return out;
}
