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
#include "commandregistry.h"

#include "writes/writerunner.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <regex>

#include "errors.h"

#include "log.h"

using namespace muse;
using namespace mu::agent;

static mcp::DataType toMcp(DataType t)
{
    switch (t) {
    case DataType::Undefined: return mcp::DataType::Undefined;
    case DataType::String: return mcp::DataType::String;
    case DataType::Integer: return mcp::DataType::Integer;
    case DataType::Float: return mcp::DataType::Float;
    case DataType::Boolean: return mcp::DataType::Boolean;
    case DataType::Object: return mcp::DataType::Object;
    case DataType::Array: return mcp::DataType::Array;
    case DataType::Null: return mcp::DataType::Null;
    }
    return mcp::DataType::Undefined;
}

static const char* typeName(DataType t)
{
    switch (t) {
    case DataType::Undefined: return "any";
    case DataType::String: return "string";
    case DataType::Integer: return "integer";
    case DataType::Float: return "number";
    case DataType::Boolean: return "boolean";
    case DataType::Object: return "object";
    case DataType::Array: return "array";
    case DataType::Null: return "null";
    }
    return "any";
}

static bool isNumber(const Val& v)
{
    return v.type() == Val::Type::Double || v.type() == Val::Type::Int || v.type() == Val::Type::Int64;
}

static bool matchesType(const Val& v, DataType t)
{
    switch (t) {
    case DataType::Undefined: return true;
    case DataType::String: return v.type() == Val::Type::String;
    case DataType::Integer: {
        if (!isNumber(v)) {
            return false;
        }
        const double d = v.toDouble();
        return std::isfinite(d) && d == std::floor(d);
    }
    case DataType::Float: return isNumber(v) && std::isfinite(v.toDouble());
    case DataType::Boolean: return v.type() == Val::Type::Bool;
    case DataType::Object: return v.type() == Val::Type::Map;
    case DataType::Array: return v.type() == Val::Type::List;
    case DataType::Null: return v.isNull();
    }
    return false;
}

bool CommandRegistry::add(const CommandInfo& info)
{
    static const std::regex uriRe("^command://[a-z]+/[a-z][a-z0-9_]*$");
    static const std::regex nameRe("^[a-z][a-z0-9_]{0,63}$");

    if (!std::regex_match(info.uri, uriRe) || !std::regex_match(info.name, nameRe)) {
        LOGE() << "bad command uri or name: " << info.uri << " / " << info.name;
        return false;
    }
    if (info.uri.substr(info.uri.rfind('/') + 1) != info.name) {
        LOGE() << "command name must be the last URI segment: " << info.uri << " / " << info.name;
        return false;
    }
    if (info.kind == CommandKind::Write ? !info.write : !info.handler) {
        LOGE() << "command without handler: " << info.name;
        return false;
    }
    if (m_byName.count(info.name)) {
        LOGE() << "duplicate command name: " << info.name;
        return false;
    }
    for (const CommandInfo& c : m_commands) {
        if (c.uri == info.uri) {
            LOGE() << "duplicate command uri: " << info.uri;
            return false;
        }
    }

    CommandInfo c = info;
    if (c.kind == CommandKind::Write && !addWriteArgs(c)) {
        LOGE() << "write command uses a reserved argument or output name: " << info.name;
        return false;
    }
    m_byName[c.name] = m_commands.size();
    m_commands.push_back(std::move(c));
    return true;
}

const CommandInfo* CommandRegistry::find(const std::string& name) const
{
    auto it = m_byName.find(name);
    return it == m_byName.end() ? nullptr : &m_commands.at(it->second);
}

std::vector<mcp::Tool> CommandRegistry::tools() const
{
    std::vector<mcp::Tool> tools;
    tools.reserve(m_commands.size());
    for (const CommandInfo& info : m_commands) {
        mcp::Tool tool;
        tool.name = info.name;
        tool.title = info.title;
        tool.description = info.description;
        for (const auto& [name, arg] : info.input) {
            mcp::Property p;
            p.type = toMcp(arg.type);
            p.description = arg.description;
            p.minimum = arg.min;
            p.maximum = arg.max;
            p.enumValues = arg.enumValues;
            p.itemsType = toMcp(arg.itemsType);
            tool.inputSchema.properties[name] = p;
            if (arg.required) {
                tool.inputSchema.required.push_back(name);
            }
        }
        for (const auto& [name, arg] : info.output) {
            mcp::Property p;
            p.type = toMcp(arg.type);
            p.description = arg.description;
            p.itemsType = toMcp(arg.itemsType);
            tool.outputSchema.properties[name] = p;
        }
        tools.push_back(std::move(tool));
    }
    return tools;
}

Ret CommandRegistry::validate(const CommandInfo& info, const Params& params)
{
    for (const auto& [name, value] : params) {
        if (!info.input.count(name)) {
            return makeErr(Err::InvalidArg, "unknown argument '" + name + "' for " + info.name);
        }
    }

    for (const auto& [name, arg] : info.input) {
        auto it = params.find(name);
        if (it == params.end() || it->second.isNull()) {
            if (arg.required) {
                return makeErr(Err::InvalidArg, "missing required argument '" + name + "'");
            }
            continue;
        }

        const Val& v = it->second;
        if (!matchesType(v, arg.type)) {
            return makeErr(Err::InvalidArg, "argument '" + name + "' must be " + typeName(arg.type));
        }

        if (isNumber(v)) {
            if (!arg.min.isNull() && v.toDouble() < arg.min.toDouble()) {
                return makeErr(Err::InvalidArg, "argument '" + name + "' is below its minimum " + arg.min.toString());
            }
            if (!arg.max.isNull() && v.toDouble() > arg.max.toDouble()) {
                return makeErr(Err::InvalidArg, "argument '" + name + "' is above its maximum " + arg.max.toString());
            }
        }

        if (!arg.enumValues.empty()) {
            const std::string s = v.toString();
            if (std::find(arg.enumValues.begin(), arg.enumValues.end(), s) == arg.enumValues.end()) {
                return makeErr(Err::InvalidArg, "argument '" + name + "' has value '" + s + "', not one of the allowed values");
            }
        }

        if (arg.type == DataType::Array && arg.itemsType != DataType::Undefined) {
            for (const Val& item : v.toList()) {
                if (!matchesType(item, arg.itemsType)) {
                    return makeErr(Err::InvalidArg, "items of '" + name + "' must be " + typeName(arg.itemsType));
                }
            }
        }
    }

    return make_ok();
}

Result CommandRegistry::call(const std::string& name, const Params& params, AgentEnv& env) const
{
    const CommandInfo* info = find(name);
    if (!info) {
        return { makeErr(Err::UnknownCommand, "no command named '" + name + "'"), {} };
    }

    const Ret valid = validate(*info, params);
    if (!valid) {
        return { valid, {} };
    }

    try {
        if (info->kind == CommandKind::Write) {
            return runWrite(*info, params, env);
        }
        return info->handler(params, env);
    } catch (const std::exception& e) {
        LOGE() << "command " << name << " threw: " << e.what();
        return { makeErr(Err::Internal, std::string("command threw: ") + e.what()), {} };
    }
}
