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

#include "engraving/dom/input.h"
#include "engraving/types/fraction.h"
#include "engraving/types/types.h"

namespace mu::engraving {
class EngravingItem;
class MasterScore;
}

namespace mu::agent::core {
//! The user's selection and note-input settings, saved before an agent edit and put back
//! after it. Engraving paths select what they create (setNoteRest, addNote, addTuplet) and
//! some read the selection (addHairpin reads isRange), so a transaction clears it first and
//! restores it last. A list selection is restored by element, but only for elements still in
//! the score (the saved pointers are checked against a walk of the live tree, never trusted);
//! a range selection is restored by ticks and staves, clamped to the score. The note-input
//! state is restored whole (track, duration, accidental, beam mode, ...), segments by tick.
class SelectionGuard
{
public:
    void save(mu::engraving::MasterScore* score);
    void clear();
    void restore();

private:
    mu::engraving::MasterScore* m_score = nullptr;
    bool m_saved = false;
    enum class Kind {
        None,
        Range,
        List
    } m_kind = Kind::None;
    mu::engraving::Fraction m_tick1;
    mu::engraving::Fraction m_tick2;
    mu::engraving::staff_idx_t m_staff1 = 0;
    mu::engraving::staff_idx_t m_staff2 = 0;
    std::vector<mu::engraving::EngravingItem*> m_items; // compared by address only, never dereferenced unchecked
    // the whole note-input state (G5): engraving paths move its segment, track and duration.
    // Its pointers are not trusted on restore: segments are found again by tick, a slur is kept
    // only if it is still in the score.
    mu::engraving::InputState m_input;
    mu::engraving::Fraction m_inputTick { -1, 1 };
    mu::engraving::Fraction m_inputLastTick { -1, 1 };
};
}
