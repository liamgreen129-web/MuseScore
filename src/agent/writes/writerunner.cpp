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
#include "writerunner.h"

#include "agentenv.h"
#include "core/transaction.h"
#include "registry/errors.h"
#include "writes/notationundobackend.h"

#include "engraving/dom/masterscore.h"
#include "notation/imasternotation.h"
#include "notation/inotationinteraction.h"
#include "notation/inotationnoteinput.h"
#include "project/inotationproject.h"

using namespace mu::agent;
using muse::Ret;
using muse::Val;

namespace {
const char* EXPECT_VERSION = "expectStateVersion";

mu::notation::IMasterNotationPtr masterNotation(AgentEnv& env)
{
    const auto project = env.globalContext() ? env.globalContext()->currentProject() : nullptr;
    return project ? project->masterNotation() : nullptr;
}

Ret checkInteraction(const mu::notation::INotationPtr& notation)
{
    if (!notation) {
        return muse::make_ok();
    }
    const mu::notation::INotationInteractionPtr ia = notation->interaction();
    if (!ia) {
        return muse::make_ok();
    }
    if (ia->isTextEditingStarted()) {
        return makeErr(Err::Busy, "you are editing text in MuseScore; finish (Esc) and try again");
    }
    if (ia->isDragStarted() || ia->isGripEditStarted() || ia->isEditingElement()) {
        return makeErr(Err::Busy, "you are dragging or editing an element in MuseScore; finish (Esc) and try again");
    }
    if (ia->noteInput() && ia->noteInput()->isNoteInputMode()) {
        return makeErr(Err::Busy, "MuseScore is in note input mode; leave it (Esc) and try again");
    }
    return muse::make_ok();
}

Params withoutWriteArgs(const Params& params)
{
    Params p = params;
    p.erase(EXPECT_VERSION);
    return p;
}

Result finish(core::Transaction& tx, AgentEnv& env, Result res)
{
    res.values["stateVersion"] = Val(static_cast<int64_t>(env.changes.version));
    res.values["changed"] = Val(tx.changed());
    if (!tx.warnings().empty()) {
        muse::ValList w;
        for (const std::string& s : tx.warnings()) {
            w.emplace_back(Val(s));
        }
        res.values["warnings"] = Val(w);
    }
    return res;
}
}

bool mu::agent::addWriteArgs(CommandInfo& info)
{
    for (const char* reserved : { EXPECT_VERSION, "stateVersion", "changed", "warnings", "results" }) {
        if (info.input.count(reserved) || info.output.count(reserved)) {
            return false;
        }
    }
    Arg a { DataType::Integer, "Refuse with E_STALE unless the score is still at this stateVersion (from your last read)" };
    a.min = Val(0);
    info.input[EXPECT_VERSION] = a;
    info.output["stateVersion"] = { DataType::Integer, "The score's stateVersion after this request" };
    info.output["changed"] = { DataType::Boolean, "Whether the score changed (false: nothing to do, no undo entry)" };
    Arg w { DataType::Array, "MuseScore's warnings, if any (the edit was still made)" };
    w.itemsType = DataType::String;
    info.output["warnings"] = w;
    return true;
}

Ret mu::agent::checkCanEdit(AgentEnv& env)
{
    const mu::notation::IMasterNotationPtr master = masterNotation(env);
    if (!master || !master->notation() || !master->masterScore()) {
        return makeErr(Err::NoScore, "no score is open in this instance");
    }
    if (env.interactive()) {
        const muse::RetVal<bool> dialog = env.interactive()->isCurrentUriDialog();
        if (dialog.ret && dialog.val) {
            return makeErr(Err::ModalOpen, "a dialog is open in MuseScore; close it and try again");
        }
    }
    if (Ret r = checkInteraction(master->notation()); !r) {
        return r;
    }
    if (env.globalContext()->currentNotation() != master->notation()) { // a part is in front
        if (Ret r = checkInteraction(env.globalContext()->currentNotation()); !r) {
            return r;
        }
    }
    return muse::make_ok();
}

void mu::agent::stopPlayback(AgentEnv& env)
{
    if (env.playback() && env.playback()->isPlaying() && env.dispatcher()) {
        env.dispatcher()->dispatch("stop");
    }
}

Ret mu::agent::checkStateVersion(const Params& params, const AgentEnv& env)
{
    auto it = params.find(EXPECT_VERSION);
    if (it == params.end() || it->second.isNull()) {
        return muse::make_ok();
    }
    const long long expected = static_cast<long long>(it->second.toDouble());
    if (expected != env.changes.version) {
        return makeErr(Err::Stale, "the score changed since stateVersion " + std::to_string(expected) + " (now "
                       + std::to_string(env.changes.version) + "); read it again (score_changes lists what changed)");
    }
    return muse::make_ok();
}

Result mu::agent::runWrite(const CommandInfo& info, const Params& params, AgentEnv& env)
{
    WriteOp op { &info, withoutWriteArgs(params) };
    Result res = runWrites(info.undoName.empty() ? info.title : info.undoName, { op }, params, env);
    if (res.ret && res.values.count("results")) { // a single command returns its own values at the top
        const muse::ValList list = res.values["results"].toList();
        res.values.erase("results");
        if (!list.empty()) {
            for (const auto& [k, v] : list.front().toMap()) {
                if (!res.values.count(k)) { // stateVersion, changed, warnings are the runner's (registry refuses the clash)
                    res.values[k] = v;
                }
            }
        }
    }
    return res;
}

Result mu::agent::runWrites(const std::string& undoName, const std::vector<WriteOp>& ops, const Params& requestParams, AgentEnv& env)
{
    if (Ret r = checkCanEdit(env); !r) {
        return { r, {} };
    }
    if (Ret r = checkStateVersion(requestParams, env); !r) {
        return { r, {} };
    }
    stopPlayback(env);
    const mu::notation::IMasterNotationPtr master = masterNotation(env);
    NotationUndoBackend backend(master->notation(), env.globalContext()->currentNotation());
    core::Transaction tx(master->masterScore(), backend);
    if (Ret r = tx.begin("Agent: " + undoName); !r) {
        return { r, {} };
    }

    muse::ValList results;
    for (const WriteOp& op : ops) {
        std::map<std::string, Val> out;
        WriteContext ctx { master->masterScore(), env, out };
        const Ret r = tx.step(op.info->name, [&] { return op.info->write(op.params, ctx); });
        if (!r) {
            return { r, {} }; // already rolled back
        }
        results.emplace_back(Val(out));
    }
    if (Ret r = tx.commit(); !r) {
        return { r, {} };
    }
    Result res;
    res.values["results"] = Val(results);
    return finish(tx, env, res);
}
