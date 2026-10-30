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

#include <map>
#include <string>
#include <vector>

#include "global/types/ret.h"
#include "global/types/val.h"

#include "notation/ast.h"
#include "registry/commandtypes.h"

namespace mu::engraving {
class MasterScore;
}

//! The pitch group (P2.4, docs/agent/specs/pitch.md): set_pitch and transpose. Each operation
//! is one write step: it validates everything first, then edits the master score through the
//! engraving calls the UI ends in, checks the effect, and fills `out` with the result and the
//! compact notation of the touched bars. They run inside the runner's transaction and never
//! open, lock or commit anything themselves (CLAUDE.md rules 3 and 4). Kept free of AgentEnv
//! so the unit tests call them with a core Transaction on a fixture.
//!
//! set_pitch reproduces the last block of Score::upDown (cmd.cpp:2072-2091) on one note and its
//! tie chain: remove the accidental of every linked copy, Score::undoChangePitch, and for a TAB
//! staff Score::undoChangeFretting with StringData::convertPitch's fretting.
//!
//! transpose is Transpose::transpose (transpose.cpp:41), the engine the Transpose dialog's
//! NotationInteraction::transpose wraps (notationinteraction.cpp:6014-6024), called with a
//! range selection built from Selection::setRange + updateSelectedElements (what
//! Score::selectRange does for a chord, score.cpp:3591-3604) or a list selection, the selection
//! filter set to "everything" for the call and put back after it.
namespace mu::agent::pitch {
using Out = std::map<std::string, muse::Val>;

muse::Ret setPitch(const Params& p, mu::engraving::MasterScore* score, Out& out);
muse::Ret transpose(const Params& p, mu::engraving::MasterScore* score, Out& out);

//! "P1", "A1", "d2", ... "P8": index i is Interval::allIntervals[i] (interval.cpp:124-159)
const std::vector<std::string>& intervalNames();
//! -1 when the name is not one of intervalNames()
int intervalIndex(const std::string& name);

//! "F#4", "Bbb3", "C-1": NOTATION.md section 2 spelling (accidentals #, ##, b, bb), octave -1..9.
//! False with a reason for anything else.
bool parsePitchName(const std::string& text, notation::Pitch& out, std::string* why = nullptr);
}
