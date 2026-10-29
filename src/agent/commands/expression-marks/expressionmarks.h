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

#include "registry/commandtypes.h"

namespace mu::engraving {
class MasterScore;
}

//! The expression-marks group (P2.4, docs/agent/specs/expression-marks.md): dynamics, hairpins,
//! articulations, ornaments, fermatas and breath marks. Each operation is one write step: it
//! validates everything first, then edits the master score through the engraving calls the
//! palette ends in (ChordRest::drop / Chord::drop / BarLine::drop / Dynamic::drop, and
//! Score::addHairpin for hairpins), checks the effect, and fills `out` with a handle, a status and
//! the compact notation of the touched bars. They run inside the runner's transaction and never
//! open, lock or commit anything themselves. Kept free of AgentEnv so the unit tests can call them
//! with a Transaction on a fixture.
namespace mu::agent::expression_marks {
using Out = std::map<std::string, muse::Val>;

muse::Ret addDynamic(const Params& p, mu::engraving::MasterScore* score, Out& out);
muse::Ret addHairpin(const Params& p, mu::engraving::MasterScore* score, Out& out);
muse::Ret addArticulation(const Params& p, mu::engraving::MasterScore* score, Out& out);
muse::Ret addOrnament(const Params& p, mu::engraving::MasterScore* score, Out& out);
muse::Ret addFermata(const Params& p, mu::engraving::MasterScore* score, Out& out);
muse::Ret addBreath(const Params& p, mu::engraving::MasterScore* score, Out& out);

//! Allowed names, for the schemas' enums
const std::vector<std::string>& dynamicNames();
const std::vector<std::string>& hairpinKinds();
const std::vector<std::string>& articulationNames();
const std::vector<std::string>& ornamentNames();
const std::vector<std::string>& fermataNames();
const std::vector<std::string>& breathNames();
}
