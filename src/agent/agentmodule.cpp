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
#include "agentmodule.h"

#include <cstdlib>

#include "agentservice.h"

#include "log.h"

using namespace muse;
using namespace mu::agent;

static const std::string mname("agent");

std::string AgentModule::moduleName() const
{
    return mname;
}

modularity::IContextSetup* AgentModule::newContext(const modularity::ContextPtr& ctx) const
{
    return new AgentContext(ctx);
}

// =====================================================
// AgentContext
// =====================================================

AgentContext::AgentContext(const modularity::ContextPtr& ctx)
    : modularity::IContextSetup(ctx)
{
}

AgentContext::~AgentContext() = default;

void AgentContext::onAllInited(const IApplication::RunMode& mode)
{
    if (mode != IApplication::RunMode::GuiApp) {
        return;
    }

    if (const char* off = std::getenv("MUSESCORE_AGENT_DISABLE"); off && std::string(off) == "1") {
        LOGI() << "agent server disabled by MUSESCORE_AGENT_DISABLE=1";
        return;
    }

    m_service = std::make_unique<AgentService>(iocContext());
    if (!m_service->start()) {
        m_service.reset();
    }
}

void AgentContext::onDeinit()
{
    if (m_service) {
        m_service->stop();
        m_service.reset();
    }
}
