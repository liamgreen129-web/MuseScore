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

#include "modularity/ioc.h"
#include "global/iapplication.h"
#include "context/iglobalcontext.h"
#include "multiwindows/imultiwindowsprovider.h"
#include "multiwindows/internal/multiprocess/imultiprocessprovider.h"

namespace mu::agent {
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
};
}
