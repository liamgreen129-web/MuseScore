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

#include <deque>
#include <string>

#include "modularity/ioc.h"
#include "global/iapplication.h"
#include "context/iglobalcontext.h"
#include "multiwindows/imultiwindowsprovider.h"
#include "multiwindows/internal/multiprocess/imultiprocessprovider.h"

namespace mu::agent {
//! One committed change to the score, as published by the undo stack (ScoreChanges).
//! Ticks, not pointers: nothing here outlives the request (CLAUDE.md rule 2).
struct ChangeRecord {
    long long version = 0;
    int tickFrom = -1;
    int tickTo = -1;
    int staffFrom = -1; // 0-based, -1 = unknown
    int staffTo = -1;
};

//! stateVersion: a counter owned by the agent, bumped on every published change
//! (read and session specs: the undo index repeats after undo + new edit, so it can't be used).
struct ChangeLog {
    long long version = 0;
    std::deque<ChangeRecord> records; // newest last, at most `capacity`
    size_t capacity = 1000;
    void add(ChangeRecord r)
    {
        r.version = ++version;
        records.push_back(r);
        while (records.size() > capacity) {
            records.pop_front();
        }
    }
};

//! What a command handler may use. Handlers get the services through this object only,
//! never by holding pointers between requests (CLAUDE.md rule 2).
class AgentEnv : public muse::Contextable
{
public:
    explicit AgentEnv(const muse::modularity::ContextPtr& ctx)
        : muse::Contextable(ctx) {}

    muse::ContextInject<muse::IApplication> application = { this };
    muse::ContextInject<context::IGlobalContext> globalContext = { this };
    muse::GlobalInject<muse::mi::IMultiWindowsProvider> multiwindowsProvider;
    muse::GlobalInject<muse::mi::IMultiProcessProvider> multiProcessProvider;

    int port = 0;
    long long pid = 0;
    ChangeLog changes;
};
}
