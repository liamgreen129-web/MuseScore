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
#pragma once

//! The compact notation (docs/agent/NOTATION.md) as data. Plain C++ with no MuseScore
//! dependencies: the parser builds it from text, the score printer builds it from the
//! engraving model, the writer turns it back into canonical text, and write_voice (P2.3)
//! applies it to the score.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mu::agent::notation {
//! Exact rational, in whole notes (a quarter is 1/4).
struct Rational {
    int64_t num = 0;
    int64_t den = 1;

    Rational() = default;
    Rational(int64_t n, int64_t d = 1);

    Rational operator+(const Rational& o) const;
    Rational operator-(const Rational& o) const;
    Rational operator*(const Rational& o) const;
    Rational operator/(const Rational& o) const;
    bool operator==(const Rational& o) const { return num == o.num && den == o.den; }
    bool operator!=(const Rational& o) const { return !(*this == o); }
    bool operator<(const Rational& o) const;
    bool isZero() const { return num == 0; }
    std::string toString() const; // "3/8", "1"
};

enum class BaseDuration {
    Breve, Whole, Half, Quarter, Eighth, Sixteenth, ThirtySecond, SixtyFourth, OneTwentyEighth
};

struct Duration {
    BaseDuration base = BaseDuration::Quarter;
    int dots = 0;

    Rational value() const; // undotted base value times (2 - 1/2^dots)
    bool operator==(const Duration& o) const { return base == o.base && dots == o.dots; }
};

struct Pitch {
    char step = 'C';  // 'A'..'G'
    int alter = 0;    // -2..2
    int octave = 4;   // scientific: C4 = middle C

    int midi() const; // C4 = 60
    bool operator==(const Pitch& o) const { return step == o.step && alter == o.alter && octave == o.octave; }
};

struct NoteHead {
    Pitch pitch;
    bool tie = false; // tied to the same pitch in the next event of the voice
    bool operator==(const NoteHead& o) const { return pitch == o.pitch && tie == o.tie; }
};

//! One "+..." mark (NOTATION.md section 5). Kept generic so reads never lose a mark;
//! the parser checks codes against the table and the writer prints them back unchanged.
struct Mark {
    std::string code;              // "st", "mf", "<", "8va", "ly2", "" for plain staff text
    char bracket = 0;              // '[' line start, ']' line end, 0 none
    std::string arg;               // after ':' ("short", "up", a SymId)
    std::optional<std::string> text; // quoted text
    std::string paren;             // inside "( )", e.g. "q=132"
    bool operator==(const Mark& o) const
    {
        return code == o.code && bracket == o.bracket && arg == o.arg && text == o.text && paren == o.paren;
    }
};

enum class EventKind {
    Note, Chord, Rest, Gap, FullBarRest
};

enum class GraceKind {
    None, Appoggiatura, Acciaccatura
};

struct Event {
    EventKind kind = EventKind::Note;
    std::vector<NoteHead> heads; // Note: one, Chord: low to high, others: none
    Duration duration;           // unused for FullBarRest
    std::vector<Mark> marks;
    int slurOpen = 0;            // "(" before the event
    int slurClose = 0;           // ")" after the event

    GraceKind graceBeforeKind = GraceKind::None;
    std::vector<Event> graceBefore; // g( ... ) / g/( ... )
    std::vector<Event> graceAfter;  // ga( ... )

    bool operator==(const Event& o) const;
};

struct Tuplet;
using Item = std::variant<Event, Tuplet>;

struct Tuplet {
    int actual = 3;
    int normal = 2;
    std::vector<Item> items;
    int slurOpen = 0;
    int slurClose = 0;
    bool operator==(const Tuplet& o) const;
};

struct BarRef {
    int index = 0;                  // m<N>: 1-based position in the score
    std::optional<std::string> display; // m<N>(<display>)
    bool operator==(const BarRef& o) const { return index == o.index && display == o.display; }
};

struct KeyValue {
    std::string key;
    std::string value;
    bool operator==(const KeyValue& o) const { return key == o.key && value == o.value; }
};

struct StaffHeader {
    int staff = 0;
    std::string name;
    std::vector<KeyValue> attrs;
    bool operator==(const StaffHeader& o) const { return staff == o.staff && name == o.name && attrs == o.attrs; }
};

struct BarAttrs {
    BarRef bar;
    std::vector<KeyValue> attrs;
    bool operator==(const BarAttrs& o) const { return bar == o.bar && attrs == o.attrs; }
};

struct VoiceLine {
    BarRef bar;
    int voice = 1; // 1..4
    std::vector<Item> items;
    bool operator==(const VoiceLine& o) const { return bar == o.bar && voice == o.voice && items == o.items; }
};

struct HarmonyItem {
    std::optional<std::string> chord; // none = gap
    Duration duration;
    bool operator==(const HarmonyItem& o) const { return chord == o.chord && duration == o.duration; }
};

struct HarmonyLine {
    BarRef bar;
    std::vector<HarmonyItem> items;
    bool operator==(const HarmonyLine& o) const { return bar == o.bar && items == o.items; }
};

struct Comment {
    std::string text;
    bool operator==(const Comment& o) const { return text == o.text; }
};

using Line = std::variant<StaffHeader, BarAttrs, VoiceLine, HarmonyLine, Comment>;

struct Document {
    std::vector<Line> lines;
    bool operator==(const Document& o) const { return lines == o.lines; }
};

//! Total duration of a voice line's items (grace notes count zero, tuplets scaled).
//! FullBarRest contributes `barLength`.
Rational itemsDuration(const std::vector<Item>& items, const Rational& barLength);
}
