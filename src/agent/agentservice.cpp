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
#include "agentservice.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonObject>

#include "muse_framework_config.h"
#include "global/iglobalconfiguration.h"
#include "project/inotationproject.h"
#include "notation/imasternotation.h"
#include "notation/internal/inotationundostack.h"

#include "commands/allcommands.h"
#include "discovery/discoveryrecords.h"
#include "server/httptransport.h"
#include "server/mcpserver.h"
#include "server/tokenstore.h"

#include "log.h"

using namespace muse;
using namespace mu::agent;

AgentService::AgentService(const modularity::ContextPtr& ctx)
    : m_env(ctx)
{
}

AgentService::~AgentService()
{
    stop();
}

bool AgentService::start()
{
    GlobalInject<IGlobalConfiguration> globalConfiguration;
    const QString agentDir = globalConfiguration()->userAppDataPath().toQString() + "/agent";

    mcp::HttpTransportConfig config;
    config.token = TokenStore::loadOrCreate(agentDir);
    if (config.token.isEmpty()) {
        LOGE() << "agent server not started: no token";
        return false;
    }

    registerAllCommands(m_registry);

    std::string version = std::string(MUSE_APP_VERSION) + "-agent";
#ifdef MUSE_APP_REVISION
    version += "+" + std::string(MUSE_APP_REVISION);
#endif

    auto transport = std::make_unique<mcp::HttpTransport>(config);
    m_transport = transport.get();
    m_server = std::make_unique<mcp::McpServer>(version, std::move(transport));

    m_server->onToolsListRequest([this](const mcp::McpServer::ToolsListResultHandler& onResult) {
        onResult(m_registry.tools());
    });

    m_server->onToolsCallRequest([this](const std::string& name, const std::map<std::string, Val>& args,
                                        const mcp::McpServer::ToolsCallResultHandler& onResult) {
        const Result result = m_registry.call(name, args, m_env);
        mcp::ToolResult toolResult;
        toolResult.ret = result.ret;
        toolResult.result = result.values;
        onResult(toolResult);
    });

    m_server->init();

    m_env.port = m_transport->port();
    m_env.pid = QCoreApplication::applicationPid();
    if (m_env.port == 0) {
        LOGE() << "agent server not listening";
        return false;
    }

    m_startedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    m_discovery = std::make_unique<DiscoveryRecords>(agentDir);
    const int pruned = m_discovery->pruneStale();
    if (pruned > 0) {
        LOGI() << "removed " << pruned << " stale discovery records";
    }
    writeDiscoveryRecord();
    watchCurrentProject();

    return true;
}

void AgentService::stop()
{
    if (m_discovery) {
        m_discovery->remove();
        m_discovery.reset();
    }
    if (m_server) {
        m_server->deinit();
        m_server.reset();
        m_transport = nullptr;
    }
}

void AgentService::watchCurrentProject()
{
    if (!m_env.globalContext()) {
        return;
    }

    auto subscribe = [this]() {
        writeDiscoveryRecord();
        const mu::project::INotationProjectPtr project = m_env.globalContext()->currentProject();
        if (!project) {
            return;
        }
        project->pathChanged().onNotify(this, [this]() { writeDiscoveryRecord(); });
        project->displayNameChanged().onNotify(this, [this]() { writeDiscoveryRecord(); });
        if (const mu::notation::IMasterNotationPtr master = project->masterNotation()) {
            if (const mu::notation::INotationUndoStackPtr undo = master->notation() ? master->notation()->undoStack() : nullptr) {
                undo->changesChannel().onReceive(this, [this](const mu::engraving::ScoreChanges& c) {
                    // copy ticks and staff indices only; the record's pointers die with the callback
                    ChangeRecord r;
                    r.tickFrom = c.tickFrom;
                    r.tickTo = c.tickTo;
                    r.staffFrom = c.staffIdxFrom == muse::nidx ? -1 : static_cast<int>(c.staffIdxFrom);
                    r.staffTo = c.staffIdxTo == muse::nidx ? -1 : static_cast<int>(c.staffIdxTo);
                    m_env.changes.add(r);
                });
            }
        }
    };
    m_env.globalContext()->currentProjectChanged().onNotify(this, subscribe);
    subscribe(); // a score may already be open (opened with the app)
}

void AgentService::writeDiscoveryRecord()
{
    if (!m_discovery) {
        return;
    }

    QJsonObject record;
    record["pid"] = static_cast<qint64>(m_env.pid);
    record["port"] = m_env.port;
    record["url"] = QString("http://127.0.0.1:%1/mcp").arg(m_env.port);
    record["version"] = QString(MUSE_APP_VERSION);
    record["startedAt"] = m_startedAt;
    if (m_env.multiProcessProvider()) {
        record["instanceId"] = QString::fromStdString(m_env.multiProcessProvider()->selfID());
    }

    const mu::project::INotationProjectPtr project = m_env.globalContext() ? m_env.globalContext()->currentProject() : nullptr;
    if (project) {
        QJsonObject score;
        score["path"] = project->path().toQString();
        score["displayName"] = project->displayName();
        record["score"] = score;
    } else {
        record["score"] = QJsonValue::Null;
    }

    m_discovery->write(record);
}
