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

#include <vector>

#include "notation/ast.h"

namespace mu::engraving {
class Score;
}

namespace mu::agent::read {
struct ReadOptions {
    int fromBar = 1;              // 1-based, by position
    int toBar = 0;                // 0 = last bar
    std::vector<int> staves;      // 1-based staff numbers; empty = all
    std::vector<int> voices;      // 1..4; empty = all
    bool writtenPitch = false;    // NOTATION.md section 2: sounding by default
};

//! The engraving model -> the compact notation (NOTATION.md). Read-only: never selects,
//! lays out, or touches the view. Walks structure only (read spec R6), so it works on a
//! score that has not been laid out; displayed bar numbers need a laid-out score.
notation::Document printScore(const mu::engraving::Score* score, const ReadOptions& options = {});

//! A Fraction (whole notes) as the fewest notation durations, longest first.
std::vector<notation::Duration> splitDuration(const notation::Rational& value);
}
