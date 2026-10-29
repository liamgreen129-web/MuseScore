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

#include "global/types/ret.h"
#include "global/types/retval.h"
#include "engraving/types/fraction.h"

namespace mu::engraving {
class EngravingItem;
class MasterScore;
}

namespace mu::agent::core {
//! A handle names one element between requests without holding a pointer (CLAUDE.md rule 2):
//!
//!   <Type>:<eid>@m<bar>[+<offset>].s<staff>.v<voice>[#<key>]
//!   e.g. Note:Jx3kQ0aB1cd_Pq9rS2tUvwx@m12+1/4.s1.v1#60
//!
//! bar, staff and voice are 1-based like the notation; offset is the position in the bar as
//! a reduced fraction of a whole note; key is a fingerprint (a note's MIDI pitch, a chord or
//! rest's duration) so that a new element allocated at a freed address, which the EID
//! register reports under the dead element's EID, is not mistaken for it.
//!
//! Resolving never asks the EID register for an object (it can hand back freed memory and
//! asserts on unknown EIDs): it walks the live score at the handle's location, collects the
//! elements of that type there, and keeps the one whose EID and fingerprint match.
struct Handle {
    std::string type;
    std::string eid;
    int bar = 0;
    mu::engraving::Fraction offset { 0, 1 };
    int staff = 0;
    int voice = 0;
    std::string key;
};

std::string formatHandle(const Handle& h);
muse::RetVal<Handle> parseHandle(const std::string& text);

//! Handle for an element in the score; gives it an EID if it has none (not an undoable change:
//! saving does the same, twrite.cpp:501).
std::string makeHandle(const mu::engraving::EngravingItem* item);

//! E_INVALID_ARG for a malformed handle, E_RANGE for a location outside the score,
//! E_STALE_HANDLE when nothing at that location matches.
muse::RetVal<mu::engraving::EngravingItem*> resolveHandle(mu::engraving::MasterScore* score, const std::string& handle);
}
