/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2026 MuseScore/Audacity and others
 * Ported from muse_framework framework/rcontrol/mcp/mcptypes.h @ ecdc6e7 for MuseScore Agent.
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

namespace mu::agent::mcp {
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

inline std::string to_string(DataType type)
{
    switch (type) {
    case DataType::Undefined: return "undefined";
    case DataType::String: return "string";
    case DataType::Integer: return "integer";
    case DataType::Float: return "number";
    case DataType::Boolean: return "boolean";
    case DataType::Object: return "object";
    case DataType::Array: return "array";
    case DataType::Null: return "null";
    default: return "undefined";
    }
}

struct Property {
    DataType type;
    std::string description;
    muse::Val minimum;
    muse::Val maximum;
    // AGENT: additions over upstream rcontrol, so schemas can say what the
    // registry validates (P1.2). Empty means "not stated".
    std::vector<std::string> enumValues;
    DataType itemsType = DataType::Undefined;
};

struct InputSchema {
    std::string type = "object";
    std::map<std::string, Property> properties;
    std::vector<std::string> required; // AGENT: addition over upstream
};

struct OutputSchema {
    std::string type = "object";
    std::map<std::string, Property> properties;
};

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    InputSchema inputSchema;
    OutputSchema outputSchema;
};

struct ToolResult {
    muse::Ret ret;
    std::map<std::string, muse::Val> result;
};
}
