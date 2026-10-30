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

//! The text group (P2.4, docs/agent/specs/text.md): staff, system, expression, tempo and
//! rehearsal text; chord symbols; lyrics. Each operation is one write step: it validates
//! everything first, then edits the master score through the engraving calls the UI ends in
//! (Score::addText + the Pid::TEXT change TextBase::endEdit makes, Recipe A; or the element
//! built with its text and added with Score::undoAddElement, Recipe B, as chord-symbol and lyric
//! navigation do), checks the effect, and fills `out` with handles, a status and the compact
//! notation of the touched bars. They run inside the runner's transaction and never open, lock
//! or commit anything themselves and never enter text-edit mode. No default depends on app
//! preferences: the only read of one is add_text's voice_assignment "preference" for expression
//! text, which follows "dynamics apply to all voices" only when the caller asks for it.
//! Kept free of AgentEnv so the unit tests call them inside a Transaction on a fixture.
namespace mu::agent::text {
using Out = std::map<std::string, muse::Val>;

muse::Ret addText(const Params& p, mu::engraving::MasterScore* score, Out& out);
muse::Ret addChordSymbol(const Params& p, mu::engraving::MasterScore* score, Out& out);
muse::Ret addLyric(const Params& p, mu::engraving::MasterScore* score, Out& out);

//! Allowed values, for the schemas' enums
const std::vector<std::string>& textKinds();      // staff, system, expression, tempo, rehearsal
const std::vector<std::string>& beatUnits();      // w, h., h, q., q, e., e, s., s, t., t, x, y
std::string beatUnitList();                       // the same, "w, h., h, ..."
const std::vector<std::string>& harmonyNotations(); // standard, roman, nashville
const std::vector<std::string>& syllabicNames();  // single, begin, middle, end
}
