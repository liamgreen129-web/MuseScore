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
#include "selectionguard.h"

#include <algorithm>
#include <unordered_set>

#include "engraving/dom/masterscore.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/select.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/spanner.h"

using namespace mu::agent::core;
using namespace mu::engraving;

void SelectionGuard::save(MasterScore* score)
{
    m_score = score;
    m_saved = true;
    m_items.clear();
    const Selection& sel = score->selection();
    if (sel.isRange()) {
        m_kind = Kind::Range;
        m_tick1 = sel.tickStart();
        m_tick2 = sel.tickEnd();
        m_staff1 = sel.staffStart();
        m_staff2 = sel.staffEnd();
    } else if (sel.isList()) {
        m_kind = Kind::List;
        m_items = sel.elements();
    } else {
        m_kind = Kind::None;
    }
    m_input = score->inputState();
    m_inputTick = m_input.segment() ? m_input.segment()->tick() : Fraction(-1, 1);
    m_inputLastTick = m_input.lastSegment() ? m_input.lastSegment()->tick() : Fraction(-1, 1);
}

void SelectionGuard::clear()
{
    if (m_score) {
        m_score->deselectAll();
    }
}

void SelectionGuard::restore()
{
    if (!m_saved || !m_score) {
        return;
    }
    m_saved = false;
    m_score->deselectAll();

    if (m_kind == Kind::Range) {
        const size_t nstaves = m_score->nstaves();
        const Fraction end = m_score->endTick();
        const Fraction t1 = std::min(m_tick1, end);
        const Fraction t2 = std::min(m_tick2, end);
        const staff_idx_t s2 = std::min<staff_idx_t>(m_staff2, nstaves);
        if (t1 < t2 && m_staff1 < s2) {
            m_score->selection().setRangeTicks(t1, t2, m_staff1, s2);
            m_score->selection().updateSelectedElements();
        }
    } else if (m_kind == Kind::List && !m_items.empty()) {
        // the saved pointers may have been freed by the edit: only reselect what the live tree holds
        std::unordered_set<const EngravingItem*> live;
        m_score->scanElements([&live](EngravingItem* e) { live.insert(e); });
        std::vector<EngravingItem*> keep;
        for (EngravingItem* e : m_items) {
            if (live.count(e)) {
                keep.push_back(e);
            }
        }
        if (!keep.empty()) {
            m_score->select(keep, SelectType::ADD, 0);
        }
    }

    InputState& is = m_score->inputState();
    Slur* slur = m_input.slur();
    is = m_input;
    auto segAt = [this](const Fraction& t) -> Segment* {
        return t < Fraction(0, 1) || t >= m_score->endTick() ? nullptr : m_score->tick2segment(t, true, SegmentType::ChordRest);
    };
    is.setSegment(segAt(m_inputTick));
    is.setLastSegment(segAt(m_inputLastTick));
    bool slurAlive = false;
    if (slur) {
        for (const auto& [tick, sp] : m_score->spanner()) {
            if (sp == slur) {
                slurAlive = true;
                break;
            }
        }
    }
    is.setSlur(slurAlive ? slur : nullptr);
    if (is.track() >= m_score->ntracks()) {
        is.setTrack(0);
    }
    m_items.clear();
}
