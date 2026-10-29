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
#include "notehelpers.h"

#include <cmath>
#include <string>

#include "engraving/dom/chord.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/note.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tie.h"
#include "engraving/editing/transpose.h"

using namespace mu::engraving;
namespace nt = mu::agent::notation;

mu::agent::notes::Stored mu::agent::notes::storePitch(const Staff* staff, const nt::Pitch& p, const Fraction& tick, bool written)
{
    static const std::string steps = "CDEFGAB";
    const int tpc = step2tpc(static_cast<int>(steps.find(p.step)), static_cast<AccidentalVal>(p.alter));
    Interval v = staff->transpose(tick);
    Stored s;
    if (written) {
        s.pitch = p.midi() + v.chromatic;
        s.tpc2 = tpc;
        s.tpc1 = v.isZero() ? tpc : Transpose::transposeTpc(tpc, v, true);
    } else {
        s.pitch = p.midi() - staff->pitchOffset(tick);
        s.tpc1 = tpc;
        if (v.isZero()) {
            s.tpc2 = tpc;
        } else {
            v.flip();
            s.tpc2 = Transpose::transposeTpc(tpc, v, true);
        }
    }
    return s;
}

nt::Pitch mu::agent::notes::printedPitch(const Note* n, bool written)
{
    const Staff* staff = n->staff();
    const Fraction tick = n->chord()->tick();
    const int pitch = written ? n->pitch() - staff->transpose(tick).chromatic : n->pitch() + staff->pitchOffset(tick);
    const int tpc = written ? n->tpc2() : n->tpc1();
    nt::Pitch p;
    p.step = static_cast<char>(tpc2stepName(tpc).unicode());
    p.alter = static_cast<int>(tpc2alter(tpc));
    p.octave = static_cast<int>(std::floor((pitch - p.alter) / 12.0)) - 1;
    return p;
}

Tie* mu::agent::notes::addTieBetween(Note* start, Note* end)
{
    Score* score = start->score();
    Tie* tie = Factory::createTie(start);
    tie->setStartNote(start);
    tie->setTrack(start->track());
    tie->setTick(start->chord()->segment()->tick());
    if (end->tieBack()) {
        score->undoRemoveElement(end->tieBack());
    }
    tie->setEndNote(end);
    tie->setTicks(end->chord()->segment()->tick() - start->chord()->segment()->tick());
    score->undoAddElement(tie);
    tie->addTiesToJumpPoints();
    return tie;
}
