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
#include "postconditions.h"

#include "engraving/dom/chordrest.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/segment.h"

#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::engraving;

muse::Ret mu::agent::core::expect(bool ok, const std::string& what)
{
    return ok ? muse::make_ok() : makeErr(Err::PostCondition, what);
}

std::vector<std::string> mu::agent::core::barSumProblems(const MasterScore* score, int fromBar, int toBar)
{
    std::vector<std::string> problems;
    int idx = 0;
    for (const Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        ++idx;
        if (idx < fromBar) {
            continue;
        }
        if (toBar > 0 && idx > toBar) {
            break;
        }
        for (size_t staff = 0; staff < score->nstaves(); ++staff) {
            Fraction sum(0, 1);
            for (const Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
                if (const EngravingItem* el = s->element(staff * VOICES)) {
                    sum += toChordRest(el)->actualTicks();
                }
            }
            if (sum != m->ticks()) {
                problems.push_back("m" + std::to_string(idx) + " v1 (s" + std::to_string(staff + 1) + ") holds "
                                   + sum.toString().toStdString() + " in a bar of " + m->ticks().toString().toStdString());
            }
        }
    }
    return problems;
}

muse::Ret mu::agent::core::barsAddUp(const MasterScore* score, int fromBar, int toBar)
{
    const std::vector<std::string> problems = barSumProblems(score, fromBar, toBar);
    if (problems.empty()) {
        return muse::make_ok();
    }
    std::string msg = "bars no longer add up: " + problems.front();
    for (size_t i = 1; i < problems.size() && i < 4; ++i) {
        msg += "; " + problems[i];
    }
    if (problems.size() > 4) {
        msg += "; and " + std::to_string(problems.size() - 4) + " more";
    }
    return makeErr(Err::PostCondition, msg);
}
