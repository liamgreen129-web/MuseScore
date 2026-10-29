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

#include "global/types/ret.h"
#include "global/types/val.h"

#include "registry/commandtypes.h"

namespace mu::engraving {
class MasterScore;
}

namespace mu::agent::lines {
//! The write handlers of add_slur and add_line, on the engraving model only (no app services),
//! so the unit tests run them inside a core Transaction on a fixture. They validate everything
//! before changing the score, edit through the same Score calls the UI ends in, check the
//! effect, and put their results in `out`. They never open, lock, commit or roll back anything.
muse::Ret addSlur(mu::engraving::MasterScore* score, const Params& params, std::map<std::string, muse::Val>& out);
muse::Ret addLine(mu::engraving::MasterScore* score, const Params& params, std::map<std::string, muse::Val>& out);
}
