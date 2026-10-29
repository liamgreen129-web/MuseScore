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

#include <set>
#include <string>
#include <vector>

#include "global/types/ret.h"

namespace mu::engraving {
class MasterScore;
}

namespace mu::agent::notes {
//! A point in the score: a bar (1-based, by position) and a beat in quarter notes from 1.
struct BarBeat {
    int bar = 1;
    double beat = 1.0;   // 1 = the downbeat
    bool hasBeat = false;
};

struct ClearRangeArgs {
    int staffFrom = 1;          // 1-based, inclusive; staves are contiguous, as a range selection is
    int staffTo = 1;
    BarBeat from;
    BarBeat to;                 // without a beat: through the end of that bar; with one: up to, not including, it
    std::set<int> voices { 1, 2, 3, 4 };
    //! Element types deleted with the notes (selection-filter names, specs/notes.md); empty = all,
    //! as the UI's default filter
    std::vector<std::string> include;
};

struct ClearRangeResult {
    int fromBar = 0;
    int toBar = 0;
    std::string text; // the bars of those staves as score_read prints them
};

//! The selection-filter type names clear_range accepts, in the spec's order
const std::vector<std::string>& clearRangeTypes();

//! clear_range (specs/notes.md): the UI's range delete (Score::deleteRange from a range selection)
//! on a voice- and type-filtered selection. Runs inside an open transaction; validates first.
//! The score's selection filter is set for the call and put back; the user's selection is put
//! back by the transaction.
muse::Ret clearRange(mu::engraving::MasterScore* score, const ClearRangeArgs& args, ClearRangeResult& result);
}
