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

#include "engraving/types/fraction.h"

#include "notation/ast.h"

namespace mu::engraving {
class Note;
class Staff;
class Tie;
}

namespace mu::agent::notes {
//! What a note stores for a notation pitch: concert pitch without any ottava, and both
//! spellings (NOTATION.md section 2; the inverse of the score printer's spell()).
struct Stored {
    int pitch = 0;
    int tpc1 = -1;
    int tpc2 = -1;
};

Stored storePitch(const mu::engraving::Staff* staff, const notation::Pitch& p, const mu::engraving::Fraction& tick, bool written);

//! The pitch as score_read prints it (sounding includes the ottava; written leaves out the transposition)
notation::Pitch printedPitch(const mu::engraving::Note* n, bool written);

//! A tie between two existing notes: createAndAddTie's recipe (edit.cpp:2027-2050, file-static
//! upstream), which is what the UI's tie between notes (Score::cmdToggleTie) runs.
mu::engraving::Tie* addTieBetween(mu::engraving::Note* start, mu::engraving::Note* end);
}
