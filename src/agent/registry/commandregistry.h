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

#include "commandtypes.h"
#include "server/mcptypes.h"

namespace mu::agent {
class CommandRegistry
{
public:
    //! Returns false (and logs) on a duplicate name or URI, a missing handler, or a bad URI.
    bool add(const CommandInfo& info);

    const std::vector<CommandInfo>& commands() const { return m_commands; }
    const CommandInfo* find(const std::string& name) const;

    std::vector<mcp::Tool> tools() const;

    //! Validates params against the command's input schema, then runs the handler.
    Result call(const std::string& name, const Params& params, AgentEnv& env) const;

    //! Schema check alone. Integers arrive from JSON as doubles and must be whole.
    static muse::Ret validate(const CommandInfo& info, const Params& params);

private:
    std::vector<CommandInfo> m_commands;
    std::map<std::string, size_t> m_byName;
};
}
