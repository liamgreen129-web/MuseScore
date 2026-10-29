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

#include <string>
#include <vector>

#include "global/types/ret.h"
#include "registry/commandtypes.h"

namespace mu::agent {
//! Adds the arguments and outputs every write command has (expectStateVersion in;
//! stateVersion, changed, warnings out). False if the command defines one of those names
//! itself. Called by the registry.
bool addWriteArgs(CommandInfo& info);

//! Can the agent change the score right now? E_NO_SCORE; E_MODAL_OPEN when a dialog is open
//! (requests still arrive while a modal runs its own event loop); E_BUSY while the user is
//! dragging, editing text or an element, in note input, or another edit holds the undo stack.
//! Also used by undo and redo.
muse::Ret checkCanEdit(AgentEnv& env);

//! An edit during playback stops playback first (not refused). Call once the request has
//! passed every check, so a refused request leaves playback alone.
void stopPlayback(AgentEnv& env);

//! E_STALE when expectStateVersion is given and the score has changed since
muse::Ret checkStateVersion(const Params& params, const AgentEnv& env);

//! One write command = one transaction = one undo entry
Result runWrite(const CommandInfo& info, const Params& params, AgentEnv& env);

struct WriteOp {
    const CommandInfo* info = nullptr;
    Params params;
};

//! Several write commands in one transaction (the batch command): all or nothing, one undo entry
Result runWrites(const std::string& undoName, const std::vector<WriteOp>& ops, const Params& requestParams, AgentEnv& env);
}
