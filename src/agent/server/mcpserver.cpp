/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2026 MuseScore/Audacity and others
 * Ported from muse_framework framework/rcontrol/mcp/mcpserver.cpp @ ecdc6e7 for MuseScore Agent.
 * Changes from upstream are marked AGENT.
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
#include "mcpserver.h"

#include "serialization/json.h"

#include "log.h"

using namespace muse;
using namespace mu::agent::mcp;

static const char* PROTOCOL_VERSION = "2025-11-25";
static const char* SERVER_NAME = "MuseScoreAgent"; // AGENT: upstream is "MuseMCP"

static JsonValue toMcpJsonValue(const Val& val)
{
    switch (val.type()) {
    case Val::Type::Undefined: return JsonValue();
    case Val::Type::Bool: return JsonValue(val.toBool());
    case Val::Type::Int: return JsonValue(val.toInt());
    case Val::Type::Int64: return JsonValue(static_cast<double>(val.toInt64()));
    case Val::Type::Double: return JsonValue(val.toDouble());
    case Val::Type::String: return JsonValue(val.toString());
    case Val::Type::List: {
        JsonArray array;
        for (const auto& item : val.toList()) {
            array.append(toMcpJsonValue(item));
        }
        JsonValue result; // AGENT: 4.7.6 has no JsonArray -> JsonValue conversion, only assignment
        result = array;
        return result;
    }
    case Val::Type::Map: {
        JsonObject object;
        for (const auto& [key, value] : val.toMap()) {
            object[key] = toMcpJsonValue(value);
        }
        JsonValue result;
        result = object;
        return result;
    }
    default: {
        NOT_SUPPORTED << "data type: " << (int)val.type();
    }
    }

    return JsonValue();
}

static Val toVal(const JsonValue& val)
{
    if (val.isNull()) {
        return Val();
    }
    if (val.isBool()) {
        return Val(val.toBool());
    }
    if (val.isNumber()) {
        return Val(val.toDouble());
    }
    if (val.isString()) {
        return Val(val.toString().toStdString());
    }
    if (val.isArray()) {
        ValList list;
        const JsonArray array = val.toArray();
        for (size_t i = 0; i < array.size(); ++i) {
            list.emplace_back(toVal(array.at(i)));
        }
        return Val(list);
    }
    if (val.isObject()) {
        ValMap map;
        const JsonObject object = val.toObject();
        for (const auto& key : object.keys()) {
            map[key] = toVal(object[key]);
        }
        return Val(map);
    }

    return Val();
}

static std::map<std::string, Val> toArgs(const JsonObject& obj)
{
    std::map<std::string, Val> args;
    for (const auto& key : obj.keys()) {
        args[key] = toVal(obj[key]);
    }
    return args;
}

// AGENT: shared by input and output schemas; adds enum and items over upstream
static JsonObject propertyToJson(const Property& property, bool withLimits)
{
    JsonObject propertyObj;
    if (property.type != DataType::Undefined) { // any type (e.g. a handle string or an address object): no "type"
        propertyObj["type"] = to_string(property.type);
    }
    propertyObj["description"] = property.description;
    if (withLimits) {
        if (!property.minimum.isNull()) {
            propertyObj["minimum"] = toMcpJsonValue(property.minimum);
        }
        if (!property.maximum.isNull()) {
            propertyObj["maximum"] = toMcpJsonValue(property.maximum);
        }
    }
    if (!property.enumValues.empty()) {
        JsonArray values;
        for (const std::string& v : property.enumValues) {
            values.append(v);
        }
        propertyObj["enum"] = values;
    }
    if (property.type == DataType::Array && property.itemsType != DataType::Undefined) {
        JsonObject items;
        items["type"] = to_string(property.itemsType);
        propertyObj["items"] = items;
    }
    return propertyObj;
}

McpServer::McpServer(const std::string& version, std::unique_ptr<ITransport> transport)
    : m_version(version),
    m_transport(std::move(transport))
{
}

McpServer::~McpServer()
{
    deinit();
}

void McpServer::init()
{
    IF_ASSERT_FAILED(m_transport) {
        return;
    }

    m_transport->onRequest([this](const ByteArray& request, const ITransport::ResponseHandler& onResponse) {
        handle(request, onResponse);
    });

    m_transport->start();
}

void McpServer::deinit()
{
    if (m_transport) {
        m_transport->stop();
    }
}

void McpServer::handle(const ByteArray& request, const ITransport::ResponseHandler& onResponse)
{
    auto respond = [onResponse](const JsonObject& res) {
        if (res.empty()) {
            onResponse(ByteArray());
            return;
        }
        JsonDocument doc(res);
        onResponse(doc.toJson(JsonDocument::Format::Compact));
    };

    // AGENT: upstream passes a parse failure on as an empty request
    std::string err;
    const JsonDocument doc = JsonDocument::fromJson(request, &err);
    if (!err.empty() || !doc.isObject()) {
        replyError(JsonValue(), -32700, "Parse error", respond);
        return;
    }

    onRequest(doc.rootObject(), respond);
}

void McpServer::onRequest(const JsonObject& request, const std::function<void(const JsonObject&)>& onResponse)
{
    const String method = request.value("method").toString();
    if (method == u"initialize") {
        onInitialize(request, onResponse);
    } else if (method == u"ping") { // AGENT: MCP ping, not handled upstream
        JsonObject response;
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id");
        response["result"] = JsonObject();
        onResponse(response);
    } else if (method == u"tools/list") {
        onToolsList(request, onResponse);
    } else if (method == u"tools/call") {
        onToolsCall(request, onResponse);
    } else if (method.startsWith(u"notifications/")) {
        onNotifications(method);
        onResponse(JsonObject());
    } else {
        LOGE() << "Unknown method: " << method;
        replyError(request.value("id"), -32601, "Method not found", onResponse);
    }
}

void McpServer::onInitialize(const JsonObject& request, const std::function<void(const JsonObject&)>& onResponse)
{
    JsonObject response;
    response["jsonrpc"] = "2.0";
    response["id"] = request.value("id");

    JsonObject result;
    result["protocolVersion"] = PROTOCOL_VERSION;

    JsonObject toolCapabilities;
    toolCapabilities["listChanged"] = false;

    JsonObject capabilities;
    capabilities["tools"] = toolCapabilities;
    result["capabilities"] = capabilities;

    JsonObject serverInfo;
    serverInfo["name"] = SERVER_NAME;
    serverInfo["version"] = m_version;
    result["serverInfo"] = serverInfo;

    response["result"] = result;

    onResponse(response);
}

void McpServer::onToolsListRequest(const ToolsListHandler& onToolsList)
{
    m_onToolsList = onToolsList;
}

void McpServer::onToolsList(const JsonObject& request, const std::function<void(const JsonObject&)>& onResponse)
{
    if (!m_onToolsList) {
        replyError(request.value("id"), -32603, "No tools list handler", onResponse);
        return;
    }

    const JsonValue id = request.value("id");
    m_onToolsList([id, onResponse](const std::vector<Tool>& tools) {
        JsonObject response;
        response["jsonrpc"] = "2.0";
        response["id"] = id;

        JsonArray arr;
        for (const Tool& tool : tools) {
            JsonObject obj;
            obj["name"] = tool.name;
            obj["title"] = tool.title;
            obj["description"] = tool.description;

            // input
            {
                const InputSchema& schema = tool.inputSchema;
                JsonObject schemaObj;
                schemaObj["type"] = schema.type;
                JsonObject properties;
                for (const auto& [name, property] : schema.properties) {
                    properties[name] = propertyToJson(property, true);
                }
                schemaObj["properties"] = properties;
                if (!schema.required.empty()) {
                    JsonArray required;
                    for (const std::string& name : schema.required) {
                        required.append(name);
                    }
                    schemaObj["required"] = required;
                }
                obj["inputSchema"] = schemaObj;
            }

            // output
            {
                const OutputSchema& schema = tool.outputSchema;
                JsonObject schemaObj;
                schemaObj["type"] = schema.type;
                JsonObject properties;
                for (const auto& [name, property] : schema.properties) {
                    properties[name] = propertyToJson(property, false);
                }
                schemaObj["properties"] = properties;
                obj["outputSchema"] = schemaObj;
            }

            arr << obj;
        }

        JsonObject result;
        result["tools"] = arr;

        response["result"] = result;

        onResponse(response);
    });
}

void McpServer::onToolsCallRequest(const ToolsCallHandler& onToolsCall)
{
    m_onToolsCall = onToolsCall;
}

void McpServer::onToolsCall(const JsonObject& request, const std::function<void(const JsonObject&)>& onResponse)
{
    if (!m_onToolsCall) {
        replyError(request.value("id"), -32603, "No tools call handler", onResponse);
        return;
    }

    const JsonValue id = request.value("id");
    const JsonObject params = request.value("params").toObject();
    const std::string name = params.value("name").toString().toStdString();
    const JsonObject arguments = params.value("arguments").toObject();

    // AGENT: a missing tool name is a protocol error, not a tool error
    if (name.empty()) {
        replyError(id, -32602, "Invalid params: missing tool name", onResponse);
        return;
    }

    m_onToolsCall(name, toArgs(arguments), [id, onResponse](const ToolResult& toolResult) {
        JsonObject response;
        response["jsonrpc"] = "2.0";
        response["id"] = id;

        JsonObject resObj;
        JsonArray content;
        bool isError = false;

        if (toolResult.ret) {
            JsonObject structuredContent;
            for (const auto& [name, val] : toolResult.result) {
                structuredContent[name] = toMcpJsonValue(val);
            }
            resObj["structuredContent"] = structuredContent;

            JsonObject contentObj;
            contentObj["type"] = "text";
            contentObj["text"] = JsonDocument(structuredContent).toJson(JsonDocument::Format::Compact).constChar();
            content.append(contentObj);
        } else {
            isError = true;
            JsonObject errText;
            errText["type"] = "text";
            // AGENT: the message alone ("E_CODE: ..."), not upstream's "[code] message"
            errText["text"] = toolResult.ret.text().empty() ? toolResult.ret.toString() : toolResult.ret.text();
            content.append(errText);
        }

        resObj["content"] = content;
        resObj["isError"] = isError;

        response["result"] = resObj;
        onResponse(response);
    });
}

void McpServer::onNotifications(const String& method)
{
    LOGI() << "Notification: " << method;
}

void McpServer::replyError(const JsonValue& id, int code, const std::string& message,
                           const std::function<void(const JsonObject&)>& onResponse)
{
    JsonObject error;
    error["code"] = code;
    error["message"] = message;

    JsonObject response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = error;

    onResponse(response);
}
