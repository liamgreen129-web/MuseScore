/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2026 MuseScore/Audacity and others
 * Ported from muse_framework framework/rcontrol/mcp/mcpserver.h @ ecdc6e7 for MuseScore Agent.
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

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "itransport.h"
#include "serialization/json.h"
#include "mcptypes.h"

namespace mu::agent::mcp {
class McpServer
{
public:

    //! AGENT: unlike upstream, the transport is required and owned by the caller's unique_ptr.
    McpServer(const std::string& version, std::unique_ptr<ITransport> transport);
    ~McpServer();

    void init();
    void deinit();

    using ToolsListResultHandler = std::function<void (std::vector<Tool>)>;
    using ToolsListHandler = std::function<void (ToolsListResultHandler)>;
    using ToolsCallResultHandler = std::function<void (const ToolResult&)>;
    using ToolsCallHandler = std::function<void (const std::string& name,
                                                 const std::map<std::string, muse::Val>& args,
                                                 ToolsCallResultHandler)>;

    void onToolsListRequest(const ToolsListHandler& onToolsList);
    void onToolsCallRequest(const ToolsCallHandler& onToolsCall);

    //! Exposed for unit tests: the JSON-RPC layer without a transport.
    void handle(const muse::ByteArray& request, const ITransport::ResponseHandler& onResponse);

private:

    void onRequest(const muse::JsonObject& request, const std::function<void(const muse::JsonObject&)>& onResponse);

    void onInitialize(const muse::JsonObject& request, const std::function<void(const muse::JsonObject&)>& onResponse);
    void onToolsList(const muse::JsonObject& request, const std::function<void(const muse::JsonObject&)>& onResponse);
    void onToolsCall(const muse::JsonObject& request, const std::function<void(const muse::JsonObject&)>& onResponse);
    void onNotifications(const muse::String& method);

    void replyError(const muse::JsonValue& id, int code, const std::string& message,
                    const std::function<void(const muse::JsonObject&)>& onResponse);

    std::string m_version;
    std::unique_ptr<ITransport> m_transport;
    ToolsListHandler m_onToolsList;
    ToolsCallHandler m_onToolsCall;
};
}
