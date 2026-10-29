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

#include "muse_framework_config.h"
#include "global/iglobalconfiguration.h"
#include "modularity/ioc.h"

#include "server/httptransport.h"
#include "server/mcpserver.h"
#include "server/tokenstore.h"

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

void AgentContext::onInit(const IApplication::RunMode& mode)
{
    if (mode != IApplication::RunMode::GuiApp) {
        return;
    }

    if (const char* off = std::getenv("MUSESCORE_AGENT_DISABLE"); off && std::string(off) == "1") {
        LOGI() << "agent server disabled by MUSESCORE_AGENT_DISABLE=1";
        return;
    }

    GlobalInject<IGlobalConfiguration> globalConfiguration;
    const QString agentDir = globalConfiguration()->userAppDataPath().toQString() + "/agent";

    mcp::HttpTransportConfig config;
    config.token = TokenStore::loadOrCreate(agentDir);
    if (config.token.isEmpty()) {
        LOGE() << "agent server not started: no token";
        return;
    }

    std::string version = std::string(MUSE_APP_VERSION) + "-agent";
#ifdef MUSE_APP_REVISION
    version += "+" + std::string(MUSE_APP_REVISION);
#endif

    m_server = std::make_unique<mcp::McpServer>(version, std::make_unique<mcp::HttpTransport>(config));

    // P1.1: no tools yet; the registry (P1.2) replaces these handlers.
    m_server->onToolsListRequest([](const mcp::McpServer::ToolsListResultHandler& onResult) {
        onResult({});
    });
    m_server->onToolsCallRequest([](const std::string& name, const std::map<std::string, Val>&,
                                    const mcp::McpServer::ToolsCallResultHandler& onResult) {
        mcp::ToolResult result;
        result.ret = make_ret(Ret::Code::NotSupported, "unknown tool: " + name);
        onResult(result);
    });

    m_server->init();
}

void AgentContext::onDeinit()
{
    if (m_server) {
        m_server->deinit();
        m_server.reset();
    }
}
