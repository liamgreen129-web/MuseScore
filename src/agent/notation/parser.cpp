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
#include "parser.h"

#include <cctype>
#include <set>
#include <sstream>

using namespace mu::agent::notation;

std::string ParseError::toString() const
{
    return "line " + std::to_string(line) + ", col " + std::to_string(col) + ": " + message;
}

namespace {
struct LineFail {
    int col;
    std::string message;
};

const std::set<std::string> PLAIN_MARKS = {
    // articulations
    "st", "sti", "ten", "tst", "ac", "mar", "up", "dn", "harm", "open", "stop",
    // ornaments
    "tr", "mord", "imord", "turn", "iturn",
    // breath
    "br", "cae",
    // dynamics
    "pppp", "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff", "ffff", "fp", "sf", "sfz", "sffz", "sfp", "rfz", "fz",
};
const std::set<std::string> LINE_MARKS = { "8va", "8vb", "15ma", "15mb", "ped", "trl", "lr", "txt" };
const std::set<std::string> TEXT_MARKS = { "", "sys", "exp", "dyn" };

class LineParser
{
public:
    LineParser(const std::string& s)
        : m_s(s) {}

    bool atEnd() const { return m_pos >= m_s.size(); }
    char peek(size_t ahead = 0) const { return m_pos + ahead < m_s.size() ? m_s[m_pos + ahead] : '\0'; }
    int col() const { return static_cast<int>(m_pos) + 1; }
    void skipWs()
    {
        while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\r')) {
            ++m_pos;
        }
    }

    bool eat(char c)
    {
        if (peek() == c) {
            ++m_pos;
            return true;
        }
        return false;
    }

    bool eatStr(const char* str)
    {
        const std::string t(str);
        if (m_s.compare(m_pos, t.size(), t) == 0) {
            m_pos += t.size();
            return true;
        }
        return false;
    }

    [[noreturn]] void fail(const std::string& msg) const { throw LineFail { col(), msg }; }

    void expect(char c, const char* what)
    {
        if (!eat(c)) {
            fail(std::string("expected '") + c + "' " + what);
        }
    }

    int readInt(const char* what)
    {
        const size_t start = m_pos;
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            ++m_pos;
        }
        if (start == m_pos) {
            fail(std::string("expected a number ") + what);
        }
        return std::stoi(m_s.substr(start, m_pos - start));
    }

    std::string readQuoted()
    {
        expect('"', "to open text");
        std::string out;
        while (!atEnd() && peek() != '"') {
            if (peek() == '\\' && (peek(1) == '"' || peek(1) == '\\')) {
                ++m_pos;
            }
            out += m_s[m_pos++];
        }
        expect('"', "to close text");
        return out;
    }

    //! A key=value value: runs to whitespace, but keeps quoted text and parentheses whole.
    std::string readValue()
    {
        std::string out;
        int depth = 0;
        bool inQuote = false;
        while (!atEnd()) {
            const char c = peek();
            if (!inQuote && depth == 0 && (c == ' ' || c == '\t')) {
                break;
            }
            if (c == '"') {
                inQuote = !inQuote;
            } else if (!inQuote && c == '(') {
                ++depth;
            } else if (!inQuote && c == ')') {
                --depth;
            }
            out += c;
            ++m_pos;
        }
        if (inQuote) {
            fail("unclosed quote in value");
        }
        return out;
    }

    std::vector<KeyValue> readKeyValues()
    {
        std::vector<KeyValue> kvs;
        skipWs();
        while (!atEnd()) {
            const size_t start = m_pos;
            while (!atEnd() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
                ++m_pos;
            }
            if (start == m_pos) {
                fail("expected key=value");
            }
            KeyValue kv;
            kv.key = m_s.substr(start, m_pos - start);
            expect('=', "after attribute name");
            kv.value = readValue();
            if (kv.value.empty()) {
                fail("empty value for '" + kv.key + "'");
            }
            kvs.push_back(kv);
            skipWs();
        }
        return kvs;
    }

    BarRef readBarRef()
    {
        expect('m', "to start a bar reference");
        BarRef ref;
        ref.index = readInt("after 'm'");
        if (ref.index < 1) {
            fail("bar numbers start at m1");
        }
        if (eat('(')) {
            const size_t start = m_pos;
            while (!atEnd() && peek() != ')') {
                ++m_pos;
            }
            ref.display = m_s.substr(start, m_pos - start);
            expect(')', "to close the displayed bar number");
        }
        return ref;
    }

    Duration readDuration()
    {
        Duration d;
        switch (peek()) {
        case 'b': d.base = BaseDuration::Breve; break;
        case 'w': d.base = BaseDuration::Whole; break;
        case 'h': d.base = BaseDuration::Half; break;
        case 'q': d.base = BaseDuration::Quarter; break;
        case 'e': d.base = BaseDuration::Eighth; break;
        case 's': d.base = BaseDuration::Sixteenth; break;
        case 't': d.base = BaseDuration::ThirtySecond; break;
        case 'x': d.base = BaseDuration::SixtyFourth; break;
        case 'y': d.base = BaseDuration::OneTwentyEighth; break;
        default: fail("expected a duration (b w h q e s t x y)");
        }
        ++m_pos;
        while (eat('.')) {
            ++d.dots;
        }
        if (d.dots > 3) {
            fail("at most three dots");
        }
        return d;
    }

    Pitch readPitch()
    {
        Pitch p;
        const char step = peek();
        if (step < 'A' || step > 'G') {
            fail("expected a pitch step A-G");
        }
        p.step = step;
        ++m_pos;
        if (eatStr("##")) {
            p.alter = 2;
        } else if (eat('#')) {
            p.alter = 1;
        } else if (eatStr("bb")) {
            p.alter = -2;
        } else if (eat('b')) {
            p.alter = -1;
        }
        if (eatStr("-1")) {
            p.octave = -1;
        } else if (std::isdigit(static_cast<unsigned char>(peek()))) {
            p.octave = peek() - '0';
            ++m_pos;
        } else {
            fail("expected an octave number after the pitch name");
        }
        return p;
    }

    Mark readMark()
    {
        expect('+', "to start a mark");
        Mark m;
        if (peek() == '<' || peek() == '>' || peek() == '!') {
            m.code = std::string(1, peek());
            ++m_pos;
        } else {
            while (std::isalnum(static_cast<unsigned char>(peek()))) {
                m.code += peek();
                ++m_pos;
            }
        }
        if (peek() == '[' || peek() == ']') {
            m.bracket = peek();
            ++m_pos;
        }
        if (eat(':')) {
            const size_t start = m_pos;
            while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_' || peek() == '.') {
                ++m_pos;
            }
            if (start == m_pos) {
                fail("expected a value after ':' in mark");
            }
            m.arg = m_s.substr(start, m_pos - start);
        }
        if (peek() == '"') {
            m.text = readQuoted();
        }
        if (eat('(')) {
            const size_t start = m_pos;
            while (!atEnd() && peek() != ')') {
                ++m_pos;
            }
            m.paren = m_s.substr(start, m_pos - start);
            expect(')', "to close the mark's parentheses");
        }
        std::string why;
        if (!isKnownMark(m, &why)) {
            fail(why);
        }
        return m;
    }

    void readMarks(std::vector<Mark>& marks)
    {
        while (peek() == '+') {
            marks.push_back(readMark());
        }
    }

    //! One event without slur parentheses or grace groups.
    Event readBareEvent()
    {
        Event e;
        if (eatStr("r:")) {
            e.kind = EventKind::Rest;
            e.duration = readDuration();
        } else if (eatStr("_:")) {
            e.kind = EventKind::Gap;
            e.duration = readDuration();
        } else if (peek() == 'R' && (peek(1) == '\0' || peek(1) == ' ' || peek(1) == '+' || peek(1) == ')' || peek(1) == '\t')) {
            ++m_pos;
            e.kind = EventKind::FullBarRest;
        } else if (eat('[')) {
            e.kind = EventKind::Chord;
            do {
                NoteHead h;
                h.pitch = readPitch();
                h.tie = eat('~');
                e.heads.push_back(h);
            } while (eat(','));
            expect(']', "to close the chord");
            expect(':', "before the chord's duration");
            e.duration = readDuration();
            if (eat('~')) {
                for (NoteHead& h : e.heads) {
                    h.tie = true;
                }
            }
            for (size_t i = 1; i < e.heads.size(); ++i) {
                if (e.heads[i].pitch.midi() < e.heads[i - 1].pitch.midi()) {
                    fail("chord notes must go from low to high");
                }
            }
        } else {
            e.kind = EventKind::Note;
            NoteHead h;
            h.pitch = readPitch();
            expect(':', "after the pitch (durations are written C4:q)");
            e.duration = readDuration();
            h.tie = eat('~');
            e.heads.push_back(h);
        }
        readMarks(e.marks);
        return e;
    }

    std::vector<Event> readGraceGroup()
    {
        std::vector<Event> graces;
        skipWs();
        while (!atEnd() && peek() != ')') {
            Event g = readBareEvent();
            if (g.kind != EventKind::Note && g.kind != EventKind::Chord) {
                fail("grace notes must be notes or chords");
            }
            graces.push_back(g);
            skipWs();
        }
        expect(')', "to close the grace notes");
        if (graces.empty()) {
            fail("empty grace group");
        }
        return graces;
    }

    Item readItem()
    {
        int open = 0;
        while (peek() == '(') {
            ++open;
            ++m_pos;
        }

        if (eat('{')) {
            Tuplet t;
            t.slurOpen = open;
            t.actual = readInt("for the tuplet's actual count");
            expect(':', "in the tuplet ratio");
            t.normal = readInt("for the tuplet's normal count");
            if (t.actual < 1 || t.normal < 1) {
                fail("tuplet counts must be positive");
            }
            skipWs();
            while (!atEnd() && peek() != '}') {
                t.items.push_back(readItem());
                skipWs();
            }
            expect('}', "to close the tuplet");
            if (t.items.empty()) {
                fail("empty tuplet");
            }
            while (eat(')')) {
                ++t.slurClose;
            }
            return t;
        }

        GraceKind graceKind = GraceKind::None;
        std::vector<Event> before;
        if (eatStr("g/(")) {
            graceKind = GraceKind::Acciaccatura;
        } else if (eatStr("g(")) {
            graceKind = GraceKind::Appoggiatura;
        }
        if (graceKind != GraceKind::None) {
            before = readGraceGroup();
            skipWs();
            while (peek() == '(') { // a slur may start on the main note after the graces
                ++open;
                ++m_pos;
            }
        }

        Event e = readBareEvent();
        e.slurOpen = open;
        e.graceBeforeKind = graceKind;
        e.graceBefore = std::move(before);
        while (eat(')')) {
            ++e.slurClose;
        }

        const size_t save = m_pos;
        skipWs();
        if (eatStr("ga(")) {
            e.graceAfter = readGraceGroup();
        } else {
            m_pos = save;
        }
        return e;
    }

    std::vector<Item> readItems()
    {
        std::vector<Item> items;
        skipWs();
        while (!atEnd()) {
            items.push_back(readItem());
            if (!atEnd() && peek() != ' ' && peek() != '\t') {
                fail(std::string("unexpected '") + peek() + "' (events are separated by spaces)");
            }
            skipWs();
        }
        return items;
    }

    std::vector<HarmonyItem> readHarmony()
    {
        std::vector<HarmonyItem> items;
        skipWs();
        while (!atEnd()) {
            HarmonyItem h;
            if (eatStr("_:")) {
                h.duration = readDuration();
            } else {
                expect('{', "to start a chord symbol");
                const size_t start = m_pos;
                while (!atEnd() && peek() != '}') {
                    ++m_pos;
                }
                h.chord = m_s.substr(start, m_pos - start);
                if (h.chord->empty()) {
                    fail("empty chord symbol");
                }
                expect('}', "to close the chord symbol");
                expect(':', "before the chord symbol's duration");
                h.duration = readDuration();
            }
            items.push_back(h);
            skipWs();
        }
        return items;
    }

    Line readLine()
    {
        skipWs();
        if (eat('%')) {
            return Comment { m_s.substr(m_pos) };
        }
        if (eat('#')) {
            skipWs();
            StaffHeader h;
            expect('s', "for the staff number (# s1 \"Name\" ...)");
            h.staff = readInt("after 's'");
            skipWs();
            h.name = readQuoted();
            h.attrs = readKeyValues();
            return h;
        }

        BarRef ref = readBarRef();
        skipWs();
        if (eat('@')) {
            BarAttrs a;
            a.bar = ref;
            a.attrs = readKeyValues();
            return a;
        }
        if (eat('h')) {
            skipWs();
            expect('|', "after the harmony line's 'h'");
            HarmonyLine hl;
            hl.bar = ref;
            hl.items = readHarmony();
            return hl;
        }
        expect('v', "for the voice (m12 v1 | ...), '@' for bar attributes or 'h' for chord symbols");
        VoiceLine vl;
        vl.bar = ref;
        vl.voice = readInt("after 'v'");
        if (vl.voice < 1 || vl.voice > 4) {
            fail("voices are v1 to v4");
        }
        skipWs();
        expect('|', "after the voice number");
        vl.items = readItems();
        for (const Item& it : vl.items) {
            const Event* e = std::get_if<Event>(&it);
            if (e && e->kind == EventKind::Gap && vl.voice == 1) {
                fail("gaps (_) are for voices 2-4; voice 1 uses rests");
            }
        }
        return vl;
    }

private:
    const std::string& m_s;
    size_t m_pos = 0;
};
}

bool mu::agent::notation::isKnownMark(const Mark& m, std::string* why)
{
    auto no = [why](const std::string& msg) {
        if (why) {
            *why = msg;
        }
        return false;
    };

    const std::string& c = m.code;
    if (m.bracket) {
        if (!LINE_MARKS.count(c)) {
            return no("'" + c + std::string(1, m.bracket) + "' is not a line; lines are 8va 8vb 15ma 15mb ped trl lr txt");
        }
        if (c == "txt" && m.bracket == '[' && !m.text) {
            return no("a text line needs its text: +txt[\"...\"");
        }
        return true;
    }
    if (PLAIN_MARKS.count(c)) {
        return (m.arg.empty() && !m.text && m.paren.empty()) ? true : no("mark '" + c + "' takes no value");
    }
    if (c == "<" || c == ">" || c == "!") {
        return m.arg.empty() && m.paren.empty() ? true : no("hairpin marks take only optional text");
    }
    if (c == "ferm") {
        return (m.arg.empty() || m.arg == "short" || m.arg == "long" || m.arg == "vlong") ? true
               : no("fermata is ferm, ferm:short, ferm:long or ferm:vlong");
    }
    if (c == "xs") {
        return (m.arg == "up" || m.arg == "down") ? true : no("cross-staff is xs:up or xs:down");
    }
    if (c == "sym" || c == "clef") {
        return !m.arg.empty() ? true : no("'" + c + "' needs a value, e.g. +" + c + ":...");
    }
    if (TEXT_MARKS.count(c)) {
        return m.text ? true : no(c.empty() ? "staff text needs quotes: +\"dolce\"" : "'" + c + "' needs quoted text");
    }
    if (c == "tempo") {
        return (m.text || !m.paren.empty()) ? true : no("tempo needs text, a metronome mark, or both: +tempo\"Allegro\"(q=120)");
    }
    if (c.size() >= 2 && c.compare(0, 2, "ly") == 0) {
        const std::string verse = c.substr(2);
        const bool okVerse = verse.empty() || (verse.size() == 1 && verse[0] >= '2' && verse[0] <= '9');
        if (!okVerse) {
            return no("lyric verses are ly, ly2 ... ly9");
        }
        return m.text ? true : no("a lyric needs its syllable: +ly\"word\"");
    }
    return no("unknown mark '+" + c + "' (see NOTATION.md section 5)");
}

ParseResult mu::agent::notation::parse(const std::string& text)
{
    ParseResult result;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        bool blank = true;
        for (char c : line) {
            if (c != ' ' && c != '\t') {
                blank = false;
                break;
            }
        }
        if (blank) {
            continue;
        }
        try {
            LineParser p(line);
            result.doc.lines.push_back(p.readLine());
        } catch (const LineFail& f) {
            result.errors.push_back({ lineNo, f.col, f.message });
        }
    }
    return result;
}
