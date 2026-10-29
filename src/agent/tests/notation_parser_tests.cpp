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
#include <gtest/gtest.h>

#include "notation/parser.h"

using namespace mu::agent::notation;

namespace {
VoiceLine onlyVoiceLine(const ParseResult& r)
{
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].toString());
    EXPECT_EQ(r.doc.lines.size(), 1u);
    return r.doc.lines.empty() ? VoiceLine {} : std::get<VoiceLine>(r.doc.lines.at(0));
}

const Event& ev(const VoiceLine& v, size_t i)
{
    return std::get<Event>(v.items.at(i));
}

ParseError firstError(const std::string& text)
{
    ParseResult r = parse(text);
    EXPECT_FALSE(r.ok()) << "expected an error for: " << text;
    return r.errors.empty() ? ParseError {} : r.errors[0];
}
}

TEST(Agent_NotationParser, NotesAndRests)
{
    const VoiceLine v = onlyVoiceLine(parse("m3 v1 | C4:q F#4:e. Bb3:s r:h.."));
    EXPECT_EQ(v.bar.index, 3);
    EXPECT_EQ(v.voice, 1);
    ASSERT_EQ(v.items.size(), 4u);
    EXPECT_EQ(ev(v, 0).kind, EventKind::Note);
    EXPECT_EQ(ev(v, 3).kind, EventKind::Rest);
    EXPECT_EQ(ev(v, 3).duration.dots, 2);
    // double sharp is "##", not "x" (x is the 64th duration)
    EXPECT_FALSE(parse("m3 v1 | Gx4:q").ok());
}

TEST(Agent_NotationParser, PitchSpelling)
{
    const VoiceLine v = onlyVoiceLine(parse("m1 v1 | C4:q F#4:q Bb3:q Ebb5:q G##2:q C-1:q"));
    ASSERT_EQ(v.items.size(), 6u);
    EXPECT_EQ(ev(v, 0).heads[0].pitch.midi(), 60);
    EXPECT_EQ(ev(v, 1).heads[0].pitch.alter, 1);
    EXPECT_EQ(ev(v, 2).heads[0].pitch.midi(), 58);
    EXPECT_EQ(ev(v, 3).heads[0].pitch.alter, -2);
    EXPECT_EQ(ev(v, 3).heads[0].pitch.midi(), 74);
    EXPECT_EQ(ev(v, 4).heads[0].pitch.alter, 2);
    EXPECT_EQ(ev(v, 5).heads[0].pitch.octave, -1);
    EXPECT_EQ(ev(v, 5).heads[0].pitch.midi(), 0);
}

TEST(Agent_NotationParser, Durations)
{
    const VoiceLine v = onlyVoiceLine(parse("m1 v1 | C4:b C4:w C4:h C4:q C4:e C4:s C4:t C4:x C4:y C4:q. C4:h.. C4:e..."));
    EXPECT_EQ(ev(v, 0).duration.value(), Rational(2));
    EXPECT_EQ(ev(v, 3).duration.value(), Rational(1, 4));
    EXPECT_EQ(ev(v, 8).duration.value(), Rational(1, 128));
    EXPECT_EQ(ev(v, 9).duration.value(), Rational(3, 8));
    EXPECT_EQ(ev(v, 10).duration.value(), Rational(7, 8));
    EXPECT_EQ(ev(v, 11).duration.value(), Rational(15, 64));
}

TEST(Agent_NotationParser, ChordsAndTies)
{
    const VoiceLine v = onlyVoiceLine(parse("m1 v1 | [C4,E4,G4]:h [C4~,E4,G4]:q [C4,E4]:q~ C5:h~"));
    EXPECT_EQ(ev(v, 0).kind, EventKind::Chord);
    EXPECT_EQ(ev(v, 0).heads.size(), 3u);
    EXPECT_TRUE(ev(v, 1).heads[0].tie);
    EXPECT_FALSE(ev(v, 1).heads[1].tie);
    EXPECT_TRUE(ev(v, 2).heads[0].tie && ev(v, 2).heads[1].tie);
    EXPECT_TRUE(ev(v, 3).heads[0].tie);
}

TEST(Agent_NotationParser, ChordMustBeLowToHigh)
{
    EXPECT_NE(firstError("m1 v1 | [G4,C4]:h").message.find("low to high"), std::string::npos);
}

TEST(Agent_NotationParser, FullBarRestAndGaps)
{
    const VoiceLine v = onlyVoiceLine(parse("m4 v2 | R"));
    EXPECT_EQ(ev(v, 0).kind, EventKind::FullBarRest);
    const VoiceLine g = onlyVoiceLine(parse("m4 v2 | C5:h _:h"));
    EXPECT_EQ(ev(g, 1).kind, EventKind::Gap);
    EXPECT_NE(firstError("m4 v1 | C5:h _:h").message.find("voices 2-4"), std::string::npos);
}

TEST(Agent_NotationParser, Tuplets)
{
    const VoiceLine v = onlyVoiceLine(parse("m1 v1 | {3:2 C5:e D5:e E5:e} {5:4 C5:s D5:s E5:s F5:s G5:s} {3:2 C5:q {3:2 D5:e E5:e F5:e} G5:q}"));
    ASSERT_EQ(v.items.size(), 3u);
    const Tuplet& t = std::get<Tuplet>(v.items[0]);
    EXPECT_EQ(t.actual, 3);
    EXPECT_EQ(t.normal, 2);
    EXPECT_EQ(t.items.size(), 3u);
    const Tuplet& nested = std::get<Tuplet>(v.items[2]);
    EXPECT_TRUE(std::holds_alternative<Tuplet>(nested.items[1]));
    EXPECT_EQ(itemsDuration(v.items, Rational(1)), Rational(1, 4) + Rational(1, 4) + Rational(1, 2));
}

TEST(Agent_NotationParser, GraceNotes)
{
    const VoiceLine v = onlyVoiceLine(parse("m1 v1 | g(D5:e) C5:q g/(B4:s C5:s) D5:q C5:h ga(B4:s C5:s)"));
    ASSERT_EQ(v.items.size(), 3u);
    EXPECT_EQ(ev(v, 0).graceBeforeKind, GraceKind::Appoggiatura);
    EXPECT_EQ(ev(v, 0).graceBefore.size(), 1u);
    EXPECT_EQ(ev(v, 1).graceBeforeKind, GraceKind::Acciaccatura);
    EXPECT_EQ(ev(v, 1).graceBefore.size(), 2u);
    EXPECT_EQ(ev(v, 2).graceAfter.size(), 2u);
    EXPECT_EQ(itemsDuration(v.items, Rational(1)), Rational(1)); // grace notes take no time
}

TEST(Agent_NotationParser, Slurs)
{
    const VoiceLine v = onlyVoiceLine(parse("m1 v1 | ((C5:e D5:e) E5:e F5:e) {3:2 (G5:e A5:e B5:e)} C6:q"));
    EXPECT_EQ(ev(v, 0).slurOpen, 2);
    EXPECT_EQ(ev(v, 1).slurClose, 1);
    EXPECT_EQ(ev(v, 3).slurClose, 1);
    const Tuplet& t = std::get<Tuplet>(v.items[4]);
    EXPECT_EQ(std::get<Event>(t.items[0]).slurOpen, 1);
}

TEST(Agent_NotationParser, Marks)
{
    const VoiceLine v = onlyVoiceLine(parse(
        "m1 v1 | C5:q+st+ac+mf+< D5:q+!+ferm:long+\"dolce\" E5:q+8va[+ly\"Glo-\"+ly2\"Sing\" F5:q+8va]+tempo\"Allegro\"(q=132)+sym:ornamentTurn"));
    EXPECT_EQ(ev(v, 0).marks.size(), 4u);
    EXPECT_EQ(ev(v, 0).marks[3].code, "<");
    EXPECT_EQ(ev(v, 1).marks[1].code, "ferm");
    EXPECT_EQ(ev(v, 1).marks[1].arg, "long");
    EXPECT_EQ(ev(v, 1).marks[2].code, "");
    EXPECT_EQ(*ev(v, 1).marks[2].text, "dolce");
    EXPECT_EQ(ev(v, 2).marks[0].bracket, '[');
    EXPECT_EQ(*ev(v, 2).marks[2].text, "Sing");
    EXPECT_EQ(ev(v, 3).marks[1].paren, "q=132");
    EXPECT_EQ(ev(v, 3).marks[2].arg, "ornamentTurn");
}

TEST(Agent_NotationParser, UnknownMarkIsAnError)
{
    const ParseError e = firstError("m1 v1 | C5:q+stacato");
    EXPECT_NE(e.message.find("unknown mark '+stacato'"), std::string::npos) << e.message;
    EXPECT_EQ(e.line, 1);
}

TEST(Agent_NotationParser, MarkArgumentChecks)
{
    EXPECT_FALSE(parse("m1 v1 | C5:q+ferm:huge").ok());
    EXPECT_FALSE(parse("m1 v1 | C5:q+xs:left").ok());
    EXPECT_FALSE(parse("m1 v1 | C5:q+ly").ok());
    EXPECT_FALSE(parse("m1 v1 | C5:q+ly10\"x\"").ok());
    EXPECT_FALSE(parse("m1 v1 | C5:q+st:1").ok());
    EXPECT_FALSE(parse("m1 v1 | C5:q+tempo").ok());
    EXPECT_FALSE(parse("m1 v1 | C5:q+txt[").ok());
    EXPECT_TRUE(parse("m1 v1 | C5:q+txt[\"rall.\"").ok());
    EXPECT_FALSE(parse("m1 v1 | C5:q+st[").ok());
}

TEST(Agent_NotationParser, Headers)
{
    const ParseResult r = parse(
        "# s3 \"Trumpet in Bb\" clef=treble transposes=-M2 key=F time=4/4\n"
        "m17(16) @ time=3/4 key=Eb mark=B repeat=start tempo=\"Allegro\"(q=132)\n"
        "m17(16) h | {Cm7}:h. \n"
        "% a comment\n");
    ASSERT_TRUE(r.ok()) << r.errors[0].toString();
    ASSERT_EQ(r.doc.lines.size(), 4u);
    const StaffHeader& h = std::get<StaffHeader>(r.doc.lines[0]);
    EXPECT_EQ(h.staff, 3);
    EXPECT_EQ(h.name, "Trumpet in Bb");
    EXPECT_EQ(h.attrs[1].value, "-M2");
    const BarAttrs& a = std::get<BarAttrs>(r.doc.lines[1]);
    EXPECT_EQ(a.bar.index, 17);
    EXPECT_EQ(*a.bar.display, "16");
    EXPECT_EQ(a.attrs[4].value, "\"Allegro\"(q=132)");
    const HarmonyLine& hl = std::get<HarmonyLine>(r.doc.lines[2]);
    EXPECT_EQ(*hl.items[0].chord, "Cm7");
}

TEST(Agent_NotationParser, ErrorsCarryLineAndColumnAndParsingContinues)
{
    const ParseResult r = parse("m1 v1 | C4:q\nm2 v1 | C4 D4:q\nm3 v1 | E4:q\nm4 v5 | C4:q");
    ASSERT_EQ(r.errors.size(), 2u);
    EXPECT_EQ(r.errors[0].line, 2);
    EXPECT_EQ(r.errors[0].col, 11);
    EXPECT_NE(r.errors[0].message.find("expected ':'"), std::string::npos) << r.errors[0].message;
    EXPECT_EQ(r.errors[1].line, 4);
    EXPECT_EQ(r.doc.lines.size(), 2u); // the two good lines
}

TEST(Agent_NotationParser, BarNumbersStartAtOne)
{
    EXPECT_FALSE(parse("m0 v1 | C4:q").ok());
}

TEST(Agent_NotationParser, WindowsLineEndingsAndBlankLines)
{
    const ParseResult r = parse("m1 v1 | C4:w\r\n\r\n   \nm2 v1 | D4:w\r\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.doc.lines.size(), 2u);
}
