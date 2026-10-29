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
// session group, P2.1 part: batch, undo, redo (specs: notes.md "batch", session.md "undo"/"redo").

#include "sessioncommands.h"

#include "agentenv.h"
#include "registry/commandregistry.h"
#include "registry/errors.h"
#include "writes/writerunner.h"

#include "engraving/dom/masterscore.h"
#include "engraving/editing/undo.h"
#include "notation/imasternotation.h"
#include "notation/inotationinteraction.h"
#include "notation/internal/inotationundostack.h"
#include "project/inotationproject.h"

using namespace mu::agent;
using muse::Ret;
using muse::Val;

namespace {
const size_t MAX_OPERATIONS = 500;

Result batch(const Params& p, AgentEnv& env)
{
    if (!env.registry) {
        return { makeErr(Err::Internal, "no registry"), {} };
    }
    const muse::ValList list = p.at("operations").toList();
    if (list.empty()) {
        return { makeErr(Err::InvalidArg, "operations is empty"), {} };
    }
    if (list.size() > MAX_OPERATIONS) {
        return { makeErr(Err::InvalidArg, "at most " + std::to_string(MAX_OPERATIONS) + " operations per batch"), {} };
    }

    // validate every operation before touching the score
    std::vector<WriteOp> ops;
    for (size_t i = 0; i < list.size(); ++i) {
        const std::string where = "operation " + std::to_string(i + 1) + ": ";
        if (list[i].type() != Val::Type::Map) {
            return { makeErr(Err::InvalidArg, where + "must be an object {command, args}"), {} };
        }
        const muse::ValMap op = list[i].toMap();
        auto c = op.find("command");
        if (c == op.end() || c->second.type() != Val::Type::String) {
            return { makeErr(Err::InvalidArg, where + "needs a command name"), {} };
        }
        for (const auto& [k, v] : op) {
            if (k != "command" && k != "args") {
                return { makeErr(Err::InvalidArg, where + "unknown field '" + k + "'"), {} };
            }
        }
        const CommandInfo* info = env.registry->find(c->second.toString());
        if (!info) {
            return { makeErr(Err::UnknownCommand, where + "no command named '" + c->second.toString() + "'"), {} };
        }
        if (info->kind != CommandKind::Write) {
            return { makeErr(Err::InvalidArg, where + info->name + " is not a write command and cannot be batched"), {} };
        }
        Params args;
        if (auto a = op.find("args"); a != op.end() && !a->second.isNull()) {
            if (a->second.type() != Val::Type::Map) {
                return { makeErr(Err::InvalidArg, where + "args must be an object"), {} };
            }
            for (const auto& [k, v] : a->second.toMap()) {
                args[k] = v;
            }
        }
        if (args.count("expectStateVersion")) {
            return { makeErr(Err::InvalidArg, where + "put expectStateVersion on the batch, not on an operation"), {} };
        }
        if (Ret r = CommandRegistry::validate(*info, args); !r) {
            return { muse::Ret(r.code(), where + info->name + ": " + r.text()), {} };
        }
        ops.push_back({ info, args });
    }

    std::string name = p.count("name") && !p.at("name").isNull() ? p.at("name").toString() : std::string();
    if (name.empty()) {
        name = ops.size() == 1 ? (ops[0].info->undoName.empty() ? ops[0].info->title : ops[0].info->undoName)
               : std::to_string(ops.size()) + " edits";
    }
    return runWrites(name, ops, p, env);
}

mu::notation::INotationPtr viewNotation(AgentEnv& env)
{
    // the UI undoes through the notation in front (a part or the score) so that view repaints;
    // the undo stack itself is the master score's, shared by all of them
    if (auto n = env.globalContext()->currentNotation()) {
        return n;
    }
    auto project = env.globalContext()->currentProject();
    return project && project->masterNotation() ? project->masterNotation()->notation() : nullptr;
}

Result undoRedo(const Params& p, AgentEnv& env, bool undo)
{
    const char* verb = undo ? "undo" : "redo";
    if (Ret r = checkCanEdit(env); !r) {
        return { r, {} };
    }
    if (Ret r = checkStateVersion(p, env); !r) {
        return { r, {} };
    }
    const mu::notation::INotationPtr notation = viewNotation(env);
    const mu::notation::INotationUndoStackPtr stack = notation ? notation->undoStack() : nullptr;
    if (!stack) {
        return { makeErr(Err::NoScore, "no score is open in this instance"), {} };
    }
    // undo inside an open macro reverts the previous entry and the next commit then deletes it (session S5)
    auto master = env.globalContext()->currentProject()->masterNotation();
    if (stack->isLocked() || master->masterScore()->undoStack()->hasActiveCommand()) {
        return { makeErr(Err::Busy, "MuseScore is in the middle of another edit; try again when it has finished"), {} };
    }

    const size_t steps = p.count("steps") && !p.at("steps").isNull() ? static_cast<size_t>(p.at("steps").toDouble()) : 1;
    const size_t cur = stack->currentStateIndex();
    const size_t available = undo ? cur : stack->undoRedoActionCount() - cur;
    if (available == 0) {
        return { makeErr(undo ? Err::NothingToUndo : Err::NothingToRedo, std::string("nothing to ") + verb), {} };
    }
    if (steps > available) {
        return { makeErr(undo ? Err::NothingToUndo : Err::NothingToRedo, "only " + std::to_string(available) + " entr"
                         + (available == 1 ? "y" : "ies") + " to " + verb), {} };
    }

    // the names of the entries this will revert or restore, in order, all checked before acting
    std::vector<std::string> names;
    for (size_t i = 0; i < steps; ++i) {
        names.push_back(stack->lastActionNameAtIdx(undo ? cur - i : cur + i + 1).str.toStdString());
    }
    std::vector<std::string> expected;
    const bool hasOne = p.count("expectActionName") && !p.at("expectActionName").isNull();
    const bool hasMany = p.count("expectActionNames") && !p.at("expectActionNames").isNull();
    if (hasOne && hasMany) {
        return { makeErr(Err::InvalidArg, "give expectActionName or expectActionNames, not both"), {} };
    }
    if (hasOne) {
        if (steps != 1) {
            return { makeErr(Err::InvalidArg, "with steps > 1, use expectActionNames (one name per entry)"), {} };
        }
        expected.push_back(p.at("expectActionName").toString());
    } else if (hasMany) {
        for (const Val& v : p.at("expectActionNames").toList()) {
            expected.push_back(v.toString());
        }
        if (expected.size() != steps) {
            return { makeErr(Err::InvalidArg, "expectActionNames has " + std::to_string(expected.size()) + " names for "
                             + std::to_string(steps) + " steps"), {} };
        }
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != names[i]) {
            return { makeErr(Err::Stale, std::string(verb) + " entry " + std::to_string(i + 1) + " is \"" + names[i] + "\", not \""
                             + expected[i] + "\"; nothing was " + (undo ? "undone" : "redone")), {} };
        }
    }

    stopPlayback(env);
    muse::ValList done;
    for (size_t i = 0; i < steps; ++i) {
        const size_t before = stack->currentStateIndex();
        if (undo) {
            notation->interaction()->undo();
        } else {
            notation->interaction()->redo();
        }
        // post-condition: exactly one entry each time (a guard inside MuseScore can make it a no-op)
        const size_t after = stack->currentStateIndex();
        if (after != (undo ? before - 1 : before + 1)) {
            return { makeErr(Err::Refused, "MuseScore did not " + std::string(verb) + " \"" + names[i] + "\" (" + std::to_string(i)
                             + " of " + std::to_string(steps) + " done)"), {} };
        }
        done.emplace_back(Val(names[i]));
    }

    Result res;
    res.values[undo ? "undone" : "redone"] = Val(done);
    res.values["nextUndo"] = Val(stack->topMostUndoActionName().str.toStdString());
    res.values["nextRedo"] = Val(stack->topMostRedoActionName().str.toStdString());
    res.values["stateVersion"] = Val(static_cast<int64_t>(env.changes.version));
    return res;
}

void addUndoArgs(CommandInfo& c, bool undo)
{
    c.input["steps"] = { DataType::Integer, std::string("How many entries to ") + (undo ? "undo" : "redo") + " (default 1)" };
    c.input["steps"].min = Val(1);
    c.input["steps"].max = Val(50);
    c.input["expectStateVersion"] = { DataType::Integer, "Refuse with E_STALE unless the score is still at this stateVersion" };
    c.input["expectActionName"] = { DataType::String, std::string("With steps 1: refuse with E_STALE unless the ") + (undo ? "undo" : "redo")
                                    + " entry has this name, e.g. \"Agent: Add dynamic\". Use it: a request that changed "
                                      "nothing leaves no entry, so a blind undo could revert the user's own edit." };
    c.input["expectActionNames"] = { DataType::Array, "One expected name per step, in order; every one is checked before anything is "
                                     + std::string(undo ? "undone" : "redone") };
    c.input["expectActionNames"].itemsType = DataType::String;
    c.output[undo ? "undone" : "redone"] = { DataType::Array, "Names of the entries, in order" };
    c.output[undo ? "undone" : "redone"].itemsType = DataType::String;
    c.output["nextUndo"] = { DataType::String, "Name of the entry undo would revert now (empty: none)" };
    c.output["nextRedo"] = { DataType::String, "Name of the entry redo would restore now (empty: none)" };
    c.output["stateVersion"] = { DataType::Integer, "The score's stateVersion afterwards" };
}
}

void mu::agent::registerSessionCommands(CommandRegistry& registry)
{
    {
        CommandInfo c;
        c.uri = "command://notation/batch";
        c.name = "batch";
        c.title = "Run several edits as one";
        c.description = "Run write commands in order as one edit: one undo entry, all or nothing. If any operation "
                        "fails, nothing is changed and the error names the operation (\"step 7 (add_dynamic): ...\"). "
                        "Every operation is checked against its command's arguments before anything runs.";
        c.input["operations"] = { DataType::Array, "[{\"command\": \"<write command name>\", \"args\": {...}}, ...] (1-500)", true };
        c.input["operations"].itemsType = DataType::Object;
        c.input["name"] = { DataType::String, "Name of the undo entry (MuseScore shows \"Agent: <name>\")" };
        c.input["expectStateVersion"] = { DataType::Integer, "Refuse with E_STALE unless the score is still at this stateVersion" };
        c.input["expectStateVersion"].min = Val(0);
        c.output["results"] = { DataType::Array, "Each operation's own results, in order" };
        c.output["results"].itemsType = DataType::Object;
        c.output["stateVersion"] = { DataType::Integer, "The score's stateVersion afterwards" };
        c.output["changed"] = { DataType::Boolean, "Whether the score changed" };
        c.kind = CommandKind::Control;
        c.handler = batch;
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/undo";
        c.name = "undo";
        c.title = "Undo";
        c.description = "Undo the last edit(s), like Edit > Undo. Refused while you are editing in MuseScore. "
                        "Pass expectActionName so it cannot undo the wrong thing.";
        c.kind = CommandKind::Control;
        addUndoArgs(c, true);
        c.handler = [](const Params& p, AgentEnv& env) { return undoRedo(p, env, true); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/redo";
        c.name = "redo";
        c.title = "Redo";
        c.description = "Redo what undo reverted, like Edit > Redo.";
        c.kind = CommandKind::Control;
        addUndoArgs(c, false);
        c.handler = [](const Params& p, AgentEnv& env) { return undoRedo(p, env, false); };
        registry.add(c);
    }
}
