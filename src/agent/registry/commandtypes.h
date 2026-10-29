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

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "global/types/ret.h"
#include "global/types/val.h"

//! Command descriptions shaped like MuseScore 5's muse::rcommand (CommandInfo, Arg,
//! InputSchema, Availability) so the set can move onto rcommand when rebased onto 5.0.
//! Differences: arguments can be required, have an enum, and arrays name their item type.

namespace mu::agent {
class AgentEnv;

enum class DataType {
    Undefined = 0,
    String,
    Integer,
    Float,
    Boolean,
    Object,
    Array,
    Null
};

struct Arg {
    DataType type = DataType::Undefined;
    std::string description;
    bool required = false;
    muse::Val min;
    muse::Val max;
    std::vector<std::string> enumValues;
    DataType itemsType = DataType::Undefined;
};

using Params = std::map<std::string, muse::Val>;

struct Result {
    muse::Ret ret = muse::make_ok();
    std::map<std::string, muse::Val> values;
};

enum class CommandKind {
    Read,   // never mutates the score
    Write,  // runs inside one transaction (P2.1)
};

using Handler = std::function<Result (const Params& params, AgentEnv& env)>;

struct CommandInfo {
    std::string uri;          // command://<area>/<name>, as in MuseScore 5
    std::string name;         // MCP tool name; unique across all groups
    std::string title;
    std::string description;
    std::map<std::string, Arg> input;
    std::map<std::string, Arg> output;
    CommandKind kind = CommandKind::Read;
    Handler handler;
};
}
