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

#include <memory>

#include "async/asyncable.h"
#include "modularity/ioc.h"

#include "agentenv.h"
#include "registry/commandregistry.h"

namespace mu::agent {
namespace mcp {
class McpServer;
class HttpTransport;
}
class DiscoveryRecords;

//! One per process: the token, the MCP server, the command registry and this
//! instance's discovery record.
class AgentService : public muse::async::Asyncable
{
public:
    explicit AgentService(const muse::modularity::ContextPtr& ctx);
    ~AgentService();

    bool start();
    void stop();

private:
    void writeDiscoveryRecord();
    void watchCurrentProject();

    AgentEnv m_env;
    CommandRegistry m_registry;
    std::unique_ptr<mcp::McpServer> m_server;
    mcp::HttpTransport* m_transport = nullptr; // owned by m_server
    std::unique_ptr<DiscoveryRecords> m_discovery;
    QString m_startedAt;
};
}
