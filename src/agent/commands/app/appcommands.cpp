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
#include "appcommands.h"

#include "agentenv.h"
#include "registry/commandregistry.h"

#include "engraving/types/constants.h"
#include "notation/imasternotation.h"
#include "notation/inotation.h"
#include "notation/internal/inotationundostack.h"
#include "project/inotationproject.h"

using namespace muse;
using namespace mu::agent;

static Val str(const std::string& s)
{
    return Val(s);
}

static Val str(const char* s)
{
    return Val(std::string(s));
}

static Val str(const QString& s)
{
    return Val(s.toStdString());
}

//! Read-only. Never sets currentNotation: that marks a part open and the project dirty
//! (session spec, hello: globalcontext.cpp:92-102).
static Result hello(const Params&, AgentEnv& env)
{
    Result r;

    ValMap app;
    if (env.application()) {
        app["name"] = str(env.application()->name().toStdString());
        app["title"] = str(env.application()->title().toStdString());
        app["version"] = str(env.application()->fullVersion().toStdString());
        app["build"] = str(env.application()->build().toStdString());
        app["revision"] = str(env.application()->revision().toStdString());
    }
    app["bundleId"] = str(AGENT_BUNDLE_ID);
    app["fileFormat"] = str(mu::engraving::Constants::MSC_VERSION_STR);
    app["pid"] = Val(static_cast<int64_t>(env.pid));
    app["port"] = Val(env.port);
    if (env.multiProcessProvider()) {
        app["instanceId"] = str(env.multiProcessProvider()->selfID());
        app["mainInstance"] = Val(env.multiProcessProvider()->isMainInstance());
    }
    if (env.multiwindowsProvider()) {
        app["windowCount"] = Val(env.multiwindowsProvider()->windowCount());
    }
    r.values["app"] = Val(app);

    const mu::project::INotationProjectPtr project = env.globalContext() ? env.globalContext()->currentProject() : nullptr;
    if (!project) {
        r.values["score"] = Val();
        return r;
    }

    ValMap score;
    score["path"] = str(project->path().toStdString());
    score["displayName"] = str(project->displayName());
    score["newlyCreated"] = Val(project->isNewlyCreated());
    score["imported"] = Val(project->isImported());
    score["cloud"] = Val(project->isCloudProject());
    score["needSave"] = Val(project->needSave().val);

    if (const mu::notation::IMasterNotationPtr master = project->masterNotation()) {
        score["hasParts"] = Val(master->hasParts());
        ValList parts;
        for (const mu::notation::IExcerptNotationPtr& excerpt : master->excerpts()) {
            if (excerpt && excerpt->notation()) {
                parts.emplace_back(str(excerpt->notation()->name()));
            }
        }
        score["parts"] = Val(parts);
    }

    if (const mu::notation::INotationPtr view = env.globalContext()->currentNotation()) {
        score["currentView"] = str(view->name());

        if (const mu::notation::INotationUndoStackPtr undo = view->undoStack()) {
            ValMap u;
            u["canUndo"] = Val(undo->canUndo());
            u["canRedo"] = Val(undo->canRedo());
            u["index"] = Val(static_cast<int64_t>(undo->currentStateIndex()));
            u["undoName"] = str(undo->topMostUndoActionName().translated().toStdString());
            u["redoName"] = str(undo->topMostRedoActionName().translated().toStdString());
            score["undo"] = Val(u);
        }

        if (const mu::notation::INotationInteractionPtr interaction = view->interaction()) {
            ValMap modes;
            modes["noteInput"] = Val(interaction->noteInput() && interaction->noteInput()->isNoteInputMode());
            modes["textEdit"] = Val(interaction->isTextEditingStarted());
            modes["drag"] = Val(interaction->isDragStarted());
            score["modes"] = Val(modes);
        }
    }

    r.values["score"] = Val(score);
    return r;
}

void mu::agent::registerAppCommands(CommandRegistry& registry)
{
    CommandInfo info;
    info.uri = "command://app/hello";
    info.name = "hello";
    info.title = "Hello";
    info.description
        = "Identify this MuseScore Agent instance and its open score. Read-only. Returns `app` (name, version, "
          "revision, bundleId, fileFormat, pid, port, instanceId, mainInstance, windowCount) and `score` "
          "(null when no score is open; otherwise path, displayName, newlyCreated, imported, cloud, needSave, "
          "hasParts, parts, currentView, undo {canUndo, canRedo, index, undoName, redoName}, "
          "modes {noteInput, textEdit, drag}). Each open score runs in its own process with its own port.";
    info.output["app"] = { DataType::Object, "Application and instance identity" };
    info.output["score"] = { DataType::Object, "The open score, or null" };
    info.kind = CommandKind::Read;
    info.handler = hello;
    registry.add(info);
}
