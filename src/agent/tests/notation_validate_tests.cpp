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

#include <algorithm>

#include "notation/parser.h"
#include "notation/validate.h"

using namespace mu::agent::notation;

namespace {
std::vector<Problem> check(const std::string& text)
{
    const ParseResult r = parse(text);
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].toString());
    return validate(r.doc, barLengthsFromDocument(r.doc));
}
}

TEST(Agent_NotationValidate, BarsThatAddUpPass)
{
    EXPECT_TRUE(check("# s1 \"x\" time=4/4\nm1 v1 | C4:q D4:q E4:h\nm2 v2 | C4:h _:h\nm3 v1 | R").empty());
}

TEST(Agent_NotationValidate, ShortAndLongBarsSayByHowMuch)
{
    const auto p = check("# s1 \"x\" time=4/4\nm12 v1 | C4:q D4:q E4:e\nm13 v1 | C4:w D4:e");
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(p[0].code, "E_BAR_SUM");
    EXPECT_EQ(p[0].message, "m12 v1 (s1) is 3/8 short (has 5/8, bar is 1)");
    EXPECT_EQ(p[1].message, "m13 v1 (s1) is 1/8 too long (has 9/8, bar is 1)");
}

TEST(Agent_NotationValidate, TimeChangesAndPickups)
{
    EXPECT_TRUE(check("# s1 \"x\" time=4/4\n"
                      "m1 @ len=1/4\nm1 v1 | C4:q\n"
                      "m2 v1 | C4:w\n"
                      "m3 @ time=6/8\nm3 v1 | C4:q. D4:q.\n"
                      "m4 v1 | C4:h.\n").empty());
}

TEST(Agent_NotationValidate, TupletsCount)
{
    EXPECT_TRUE(check("# s1 \"x\" time=2/4\nm1 v1 | {3:2 C4:e D4:e E4:e} {5:4 C4:s D4:s E4:s F4:s G4:s}").empty());
    const auto p = check("# s1 \"x\" time=2/4\nm1 v1 | {3:2 C4:e D4:e} C4:q.");
    // a tuplet {3:2} of two eighths is malformed, and the bar no longer adds up either
    EXPECT_TRUE(std::any_of(p.begin(), p.end(), [](const Problem& x) { return x.code == "E_TUPLET"; }));
}

TEST(Agent_NotationValidate, TiesMustLand)
{
    EXPECT_TRUE(check("# s1 \"x\" time=2/4\nm1 v1 | C5:h~\nm2 v1 | C5:q [E4,G4]:q~\nm3 v1 | [E4,G4]:h").empty());
    const auto p = check("# s1 \"x\" time=2/4\nm1 v1 | C5:h~\nm2 v1 | D5:h");
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].code, "E_TIE");
    EXPECT_EQ(p[0].message, "m1 v1: C5~ is not followed by the same pitch in the voice");
    // enharmonic tie (B#4 to C5) lands: same sounding pitch
    EXPECT_TRUE(check("# s1 \"x\" time=2/4\nm1 v1 | B#4:h~\nm2 v1 | C5:h").empty());
}

TEST(Agent_NotationValidate, UnknownBarLength)
{
    const auto p = check("m1 v1 | C4:q");
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].code, "E_RANGE");
}
