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

//! tie, tuplet and grace (specs/notes.md): small edits to existing notes, addressed by a handle or
//! {staff, voice, measure, beat} (address/address.h). Each validates everything first, then makes
//! the UI's engraving call, then checks the effect. They run inside the caller's transaction.
namespace mu::agent::notes {
using Out = std::map<std::string, muse::Val>;

muse::Ret tie(mu::engraving::MasterScore* score, const Params& p, Out& out);
muse::Ret tuplet(mu::engraving::MasterScore* score, const Params& p, Out& out);
muse::Ret grace(mu::engraving::MasterScore* score, const Params& p, Out& out);
}
