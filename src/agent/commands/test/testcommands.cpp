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
#include "testcommands.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

#include "agentenv.h"
#include "core/postconditions.h"
#include "registry/commandregistry.h"
#include "registry/errors.h"

#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/editing/undo.h"
#include "notation/imasternotation.h"
#include "notation/inotationinteraction.h"
#include "notation/inotationnoteinput.h"
#include "project/inotationproject.h"

using namespace mu::agent;
using namespace mu::engraving;
using muse::Ret;
using muse::Val;

namespace {
Measure* bar(MasterScore* score, int n)
{
    int i = 0;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        if (++i == n) {
            return m;
        }
    }
    return nullptr;
}

Ret setStretch(const Params& p, WriteContext& ctx)
{
    const int n = static_cast<int>(p.at("bar").toDouble());
    Measure* m = bar(ctx.score, n);
    if (!m) {
        return makeErr(Err::Range, "bar " + std::to_string(n) + " is outside the score");
    }
    const double v = p.at("value").toDouble();
    m->undoChangeProperty(Pid::USER_STRETCH, v);
    ctx.out["bar"] = Val(n);
    ctx.out["stretch"] = Val(m->getProperty(Pid::USER_STRETCH).toDouble());
    return core::expect(m->getProperty(Pid::USER_STRETCH).toDouble() == v, "stretch was not set");
}

Ret fail(const Params& p, WriteContext& ctx)
{
    bar(ctx.score, 1)->undoChangeProperty(Pid::USER_STRETCH, 9.0); // a change that must be rolled back
    const std::string how = p.at("how").toString();
    if (how == "error") {
        return makeErr(Err::Range, "deliberate failure");
    }
    if (how == "refuse") {
        MScore::setError(MsError::NO_STAFF_SELECTED);
        return muse::make_ok();
    }
    if (how == "throw") {
        throw std::runtime_error("deliberate exception");
    }
    if (how == "unwind") {
        ctx.score->undoStack()->activeCommand()->unwind();
        return muse::make_ok();
    }
    if (how == "warn") {
        MScore::setError(MsError::TRANSPOSE_NO_FRET_DIAGRAM, true);
        return muse::make_ok();
    }
    return makeErr(Err::InvalidArg, "unknown how");
}
}

void mu::agent::registerTestCommands(CommandRegistry& registry)
{
    const char* on = std::getenv("MUSESCORE_AGENT_TEST_COMMANDS");
    if (!on || std::string(on) != "1") {
        return;
    }
    {
        CommandInfo c;
        c.uri = "command://test/test_set_stretch";
        c.name = "test_set_stretch";
        c.title = "TEST: set a bar's stretch";
        c.description = "Test only. Sets a bar's layout stretch (an undoable property change).";
        c.kind = CommandKind::Write;
        c.undoName = "Test stretch";
        c.input["bar"] = { DataType::Integer, "Bar, 1-based", true };
        c.input["bar"].min = Val(1);
        c.input["value"] = { DataType::Float, "Stretch factor", true };
        c.input["value"].min = Val(0.1);
        c.input["value"].max = Val(10.0);
        c.output["bar"] = { DataType::Integer, "The bar" };
        c.output["stretch"] = { DataType::Float, "Its stretch now" };
        c.write = setStretch;
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://test/test_dispatch";
        c.name = "test_dispatch";
        c.title = "TEST: dispatch a MuseScore action";
        c.description = "Test only. Dispatches a UI action (note-input, escape, play, stop), to put MuseScore into the "
                        "states the write guards must notice. Reports note input and playback afterwards.";
        c.input["action"] = { DataType::String, "Action code", true };
        c.input["action"].enumValues = { "note-input", "escape", "play", "stop" };
        c.kind = CommandKind::Control;
        c.output["noteInput"] = { DataType::Boolean, "In note input mode afterwards" };
        c.output["playing"] = { DataType::Boolean, "Playing afterwards" };
        c.handler = [](const Params& p, AgentEnv& env) {
            Result r;
            if (!env.dispatcher()) {
                r.ret = makeErr(Err::Internal, "no dispatcher");
                return r;
            }
            const std::string action = p.at("action").toString();
            auto project = env.globalContext()->currentProject();
            auto notation = project ? project->masterNotation()->notation() : nullptr;
            // note input directly: the action is refused while the (hidden) window has no focus
            if (action == "note-input" && notation) {
                notation->interaction()->noteInput()->startNoteInput(mu::notation::NoteInputMethod::BY_NOTE_NAME, false);
            } else if (action == "escape" && notation && notation->interaction()->noteInput()->isNoteInputMode()) {
                notation->interaction()->noteInput()->endNoteInput();
            } else {
                env.dispatcher()->dispatch(action);
            }
            r.values["noteInput"] = Val(notation && notation->interaction()->noteInput()->isNoteInputMode());
            r.values["playing"] = Val(env.playback() && env.playback()->isPlaying());
            return r;
        };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://test/test_fail";
        c.name = "test_fail";
        c.title = "TEST: change bar 1, then fail";
        c.description = "Test only. Sets bar 1's stretch to 9, then fails the chosen way (or warns).";
        c.kind = CommandKind::Write;
        c.undoName = "Test fail";
        c.input["how"] = { DataType::String, "error, refuse, throw, unwind or warn", true };
        c.input["how"].enumValues = { "error", "refuse", "throw", "unwind", "warn" };
        c.write = fail;
        registry.add(c);
    }
}
