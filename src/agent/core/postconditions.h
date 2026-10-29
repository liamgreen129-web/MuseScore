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

#include <string>
#include <vector>

#include "global/types/ret.h"

namespace mu::engraving {
class MasterScore;
}

namespace mu::agent::core {
//! Checks a write runs after its edit, inside the transaction (most engraving and
//! INotationInteraction calls return void, so the effect has to be looked at).

//! E_POSTCONDITION with \a what unless \a ok
muse::Ret expect(bool ok, const std::string& what);

//! Voice 1 of every staff fills each bar in from..to (1-based, inclusive; 0 = to the end).
//! One message per short or long bar: "m3 v1 (s2) holds 3/4 in a bar of 4/4".
std::vector<std::string> barSumProblems(const mu::engraving::MasterScore* score, int fromBar = 1, int toBar = 0);

//! E_POSTCONDITION listing the first few barSumProblems, or ok
muse::Ret barsAddUp(const mu::engraving::MasterScore* score, int fromBar = 1, int toBar = 0);
}
