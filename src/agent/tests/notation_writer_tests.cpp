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
#include "notation/writer.h"

using namespace mu::agent::notation;

namespace {
// NOTATION.md section 9 examples, verbatim: canonical text must survive parse -> write unchanged.
const char* CANONICAL[] = {
    // 9.1 Bach BWV 66.6
    "# s1 \"Soprano\" clef=treble key=f# time=C\n"
    "m1(0) @ len=1/4\n"
    "m1(0) v1 | C#5:e B4:e\n"
    "m2(1) v1 | A4:q B4:q C#5:q+ferm E5:q\n"
    "m3(2) v1 | C#5:q B4:q A4:q+ferm C#5:q\n"
    "m4(3) v1 | A4:e B4:e G#4:q F#4:q+ferm A4:q\n",
    // 9.2 Mozart K. 545
    "# s1 \"Piano (right hand)\" clef=treble key=C time=C\n"
    "m1 v1 | C5:h E5:q G5:q\n"
    "m2 v1 | (B4:q. C5:s D5:s) C5:q r:q\n"
    "m4 v1 | G5:q (F5:e+tr E5:s F5:s) E5:q r:q\n",
    // 9.3 Beethoven Op. 18/1
    "# s1 \"Violin I\" clef=treble key=F time=3/4\n"
    "m5 v1 | F5:q~+< (F5:e G5:s F5:s E5:e+st+!) F5:e+st\n"
    "m6 v1 | (G5:h Bb4:q)\n",
    // 9.4 Highland Laddie
    "# s1 \"Fiddle\" clef=treble key=G time=2/4\n"
    "m1(0) @ tempo=\"allegro\"(q=132) len=1/8\n"
    "m3(2) v1 | g/(B4:e C5:e) D5:q B4:q\n"
    "m8(7) v1 | (G4:s A4:s B4:s C5:s) D5:e A4:e\n"
    "m9(8) @ repeat=end len=3/8\n",
    // 9.5 Lantern Song
    "# s1 \"Voice\" clef=treble key=G time=4/4\n"
    "m1 v1 | D5:q+ly\"Light\" B4:e+ly\"the\" C5:e+ly\"lan-\" D5:q+ly\"tern\" G5:q~+ly\"high\"\n"
    "m2 v1 | G5:q (F#5:e+ly\"glow\" E5:e) D5:h\n"
    "# s2 \"Trumpet in Bb\" clef=treble transposes=-M2 key=G time=4/4\n"
    "m1 v1 | r:h {3:2 D5:e+mf E5:e F#5:e} G5:q+ac\n"
    "m2 v1 | g/(A5:e) G5:h r:h\n"
    "# s3 \"Piano\" clef=treble key=G time=4/4\n"
    "m1 v1 | B4:h C5:h\n"
    "m1 v2 | G4:q F#4:q [E4,G4]:h\n"
    "m1 h | {G}:h {Cmaj7}:h\n"
    "m2 v2 | G4:h _:h\n",
    // edge cases
    "m1 v1 | [C4~,E4,G4]:q [C4,E4,G4]:q~ [C4,E4,G4]:h\n"
    "m2 v1 | C5:h ga(B4:s C5:s) g(D5:e) C5:h\n"
    "m3 v3 | R+ferm\n"
    "m4 v1 | ({3:2 C5:q {3:2 D5:e E5:e F5:e} G5:q}) C5:h+\"say \\\"hi\\\"\"\n"
    "% comment kept\n",
};
}

TEST(Agent_NotationWriter, CanonicalTextRoundTripsUnchanged)
{
    for (const char* text : CANONICAL) {
        const ParseResult r = parse(text);
        ASSERT_TRUE(r.ok()) << r.errors[0].toString() << "\n" << text;
        EXPECT_EQ(write(r.doc), text);
    }
}

TEST(Agent_NotationWriter, ParseWriteParseIsStable)
{
    for (const char* text : CANONICAL) {
        const ParseResult a = parse(text);
        const ParseResult b = parse(write(a.doc));
        ASSERT_TRUE(b.ok());
        EXPECT_EQ(a.doc, b.doc);
    }
}

TEST(Agent_NotationWriter, CanonicalisesMarkOrderAndSpacing)
{
    const ParseResult r = parse("m1  v1 |   C5:q+mf+st+ly\"a\"+ferm    D5:q+!+ac");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(write(r.doc), "m1 v1 | C5:q+st+ferm+mf+ly\"a\" D5:q+ac+!\n");
}

TEST(Agent_NotationWriter, AllTiedChordPrintsOneTilde)
{
    const ParseResult r = parse("m1 v1 | [C4~,E4~]:h");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(write(r.doc), "m1 v1 | [C4,E4]:h~\n");
}
