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
#include "validate.h"

#include <iterator>
#include <map>
#include <memory>

#include "writer.h"

using namespace mu::agent::notation;

static std::optional<Rational> parseTime(const std::string& v)
{
    if (v == "C") {
        return Rational(1);
    }
    if (v == "C/") {
        return Rational(1);
    }
    const size_t slash = v.find('/');
    if (slash == std::string::npos) {
        return std::nullopt;
    }
    try {
        return Rational(std::stoll(v.substr(0, slash)), std::stoll(v.substr(slash + 1)));
    } catch (...) {
        return std::nullopt;
    }
}

BarLengthFn mu::agent::notation::barLengthsFromDocument(const Document& doc)
{
    // staff -> (bar index -> length); time signatures carry forward, len= applies to one bar
    auto lengths = std::make_shared<std::map<int, std::map<int, Rational> > >();
    auto times = std::make_shared<std::map<int, std::map<int, Rational> > >();
    int staff = 0;
    for (const Line& line : doc.lines) {
        if (const StaffHeader* h = std::get_if<StaffHeader>(&line)) {
            staff = h->staff;
            for (const KeyValue& kv : h->attrs) {
                if (kv.key == "time") {
                    if (auto t = parseTime(kv.value)) {
                        (*times)[staff][1] = *t;
                    }
                }
            }
        } else if (const BarAttrs* a = std::get_if<BarAttrs>(&line)) {
            for (const KeyValue& kv : a->attrs) {
                if (kv.key == "time") {
                    if (auto t = parseTime(kv.value)) {
                        (*times)[staff][a->bar.index] = *t;
                    }
                } else if (kv.key == "len") {
                    if (auto l = parseTime(kv.value)) {
                        (*lengths)[staff][a->bar.index] = *l;
                    }
                }
            }
        }
    }
    return [lengths, times](int st, int index) -> std::optional<Rational> {
        auto ls = lengths->find(st);
        if (ls != lengths->end()) {
            auto it = ls->second.find(index);
            if (it != ls->second.end()) {
                return it->second;
            }
        }
        auto ts = times->find(st);
        if (ts == times->end() || ts->second.empty()) {
            return std::nullopt;
        }
        auto it = ts->second.upper_bound(index);
        if (it == ts->second.begin()) {
            return std::nullopt;
        }
        return std::prev(it)->second;
    };
}

namespace {
// Flattened events of one voice, for the tie check
struct FlatEvent {
    const Event* event;
    int bar;
};

void flatten(const std::vector<Item>& items, int bar, std::vector<FlatEvent>& out)
{
    for (const Item& item : items) {
        if (const Event* e = std::get_if<Event>(&item)) {
            if (e->kind != EventKind::Gap) {
                out.push_back({ e, bar });
            }
        } else {
            flatten(std::get<Tuplet>(item).items, bar, out);
        }
    }
}

void checkTuplets(const std::vector<Item>& items, const std::string& where, const Rational& barLength, std::vector<Problem>& out)
{
    for (const Item& item : items) {
        const Tuplet* t = std::get_if<Tuplet>(&item);
        if (!t) {
            continue;
        }
        // contents must fill a whole number of the tuplet's unit: actual units in total
        const Rational inner = itemsDuration(t->items, barLength);
        const Rational unit = inner / Rational(t->actual);
        if (unit.num != 1 || (unit.den & (unit.den - 1)) != 0) {
            out.push_back({ "E_TUPLET", where + ": tuplet {" + std::to_string(t->actual) + ":" + std::to_string(t->normal)
                            + "} contents total " + inner.toString() + ", which is not " + std::to_string(t->actual)
                            + " equal note values" });
        }
        checkTuplets(t->items, where, barLength, out);
    }
}
}

std::vector<Problem> mu::agent::notation::validate(const Document& doc, const BarLengthFn& barLength)
{
    std::vector<Problem> problems;
    std::map<std::pair<int, int>, std::vector<FlatEvent> > voices; // (staff, voice) -> events in order
    int staff = 0;

    for (const Line& line : doc.lines) {
        if (const StaffHeader* h = std::get_if<StaffHeader>(&line)) {
            staff = h->staff;
            continue;
        }
        const VoiceLine* v = std::get_if<VoiceLine>(&line);
        if (!v) {
            continue;
        }
        const std::string where = "m" + std::to_string(v->bar.index) + " v" + std::to_string(v->voice)
                                  + (staff ? " (s" + std::to_string(staff) + ")" : "");
        const std::optional<Rational> len = barLength(staff, v->bar.index);
        if (!len) {
            problems.push_back({ "E_RANGE", where + ": bar length unknown (no time signature, or the bar is outside the score)" });
        } else {
            const Rational sum = itemsDuration(v->items, *len);
            if (sum != *len) {
                const bool shortBy = sum < *len;
                const Rational diff = shortBy ? *len - sum : sum - *len;
                problems.push_back({ "E_BAR_SUM", where + " is " + diff.toString() + (shortBy ? " short" : " too long")
                                     + " (has " + sum.toString() + ", bar is " + len->toString() + ")" });
            }
            checkTuplets(v->items, where, *len, problems);
        }
        flatten(v->items, v->bar.index, voices[{ staff, v->voice }]);
    }

    for (const auto& [key, events] : voices) {
        for (size_t i = 0; i < events.size(); ++i) {
            const Event& e = *events[i].event;
            for (const NoteHead& h : e.heads) {
                if (!h.tie) {
                    continue;
                }
                bool landed = false;
                if (i + 1 < events.size()) {
                    for (const NoteHead& n : events[i + 1].event->heads) {
                        landed = landed || n.pitch.midi() == h.pitch.midi();
                    }
                }
                if (!landed) {
                    problems.push_back({ "E_TIE", "m" + std::to_string(events[i].bar) + " v" + std::to_string(key.second)
                                         + ": " + writePitch(h.pitch) + "~ is not followed by the same pitch in the voice" });
                }
            }
        }
    }
    return problems;
}
