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
#include "global/types/val.h"

#include "engraving/types/fraction.h"
#include "engraving/types/types.h"

#include "registry/commandtypes.h"

namespace mu::engraving {
class ChordRest;
class MasterScore;
class Measure;
}

//! Addresses shared by every write command (DECISIONS 2026-10-30), so Claude learns one shape:
//!   staff:    3  |  {"number": 3}  |  {"part": "Violin", "index": 1}     (1-based, as "# s3" in score_read)
//!   position: {"measure": 12, "beat": 2.5}  |  {"measure": 12, "offset": "1/12"}
//!             measure = the bar by position (pickup = 1, as m<N> in score_read); beat = quarter notes
//!             from 1; offset = whole notes from the start of the bar (for triplet positions)
//!   target:   a handle string, or {staff, voice?, measure, beat|offset} in one object
//! Built from the expression-marks group's helpers (P2.4).
namespace mu::agent::address {
bool has(const Params& p, const char* key);
std::string number(double v); // 2.5, 3, 1.333

std::vector<mu::engraving::Measure*> allMeasures(mu::engraving::MasterScore* score);
int barOf(mu::engraving::MasterScore* score, const mu::engraving::Measure* m);
std::string beatOf(const mu::engraving::Measure* m, const mu::engraving::Fraction& tick); // "beat 2.5"
std::string where(mu::engraving::MasterScore* score, const mu::engraving::ChordRest* cr); // "m3 beat 2 (staff 1, voice 1)"

muse::Ret resolveStaff(mu::engraving::MasterScore* score, const muse::Val& v, mu::engraving::staff_idx_t& out);
muse::Ret resolvePosition(mu::engraving::MasterScore* score, const muse::Val& v, const std::string& name,
                          mu::engraving::Measure*& m, mu::engraving::Fraction& tick);
//! The chord or rest starting exactly at tick on track (never a gap rest); the error lists the beats
//! where that voice's events start
muse::Ret chordRestAt(mu::engraving::MasterScore* score, mu::engraving::Measure* m, const mu::engraving::Fraction& tick,
                      mu::engraving::track_idx_t track, mu::engraving::ChordRest*& cr);
//! `p[key]`: a handle string (a note means its chord) or {staff, voice?, measure, beat|offset}
muse::Ret resolveTarget(mu::engraving::MasterScore* score, const Params& p, const char* key, mu::engraving::ChordRest*& cr);
}
