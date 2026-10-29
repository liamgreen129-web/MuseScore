/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2026 MuseScore/Audacity and others
 * Ported from muse_framework framework/rcontrol/mcp/transport/httptransport.h @ ecdc6e7 for MuseScore Agent.
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
#pragma once

#include <QHash>
#include <QObject>
#include <QByteArray>

#include "itransport.h"

class QTcpServer;
class QTcpSocket;

//! NOTE Streamable HTTP endpoint: POST http://127.0.0.1:<port>/mcp
//! AGENT: every POST needs "Authorization: Bearer <token>"; Host must be loopback.

namespace mu::agent::mcp {
struct HttpTransportConfig {
    int portBase = 2412;     // AGENT: not upstream's 2212, so a future official 5.x can run beside us
    int portSpan = 20;       // one process per open score: try portBase .. portBase + portSpan - 1
    QByteArray token;        // required; empty token refuses every request
};

class HttpConnection : public QObject
{
    Q_OBJECT
public:
    explicit HttpConnection(QTcpSocket* socket, const QByteArray& token, const ITransport::RequestHandler& onRequest,
                            QObject* parent = nullptr);

private slots:
    void onReadyRead();

private:
    struct HttpMessage {
        QByteArray method;
        QByteArray path;
        QByteArray version;
        QHash<QByteArray, QByteArray> headers;
        QByteArray body;
    };

    enum class TakeStatus {
        NeedMore,
        Ready,
        Error
    };

    void processAvailable();
    TakeStatus takeMessage(HttpMessage& message, int& errorStatus, const char*& errorReason);
    bool dispatch(const HttpMessage& message);
    bool isAuthorized(const HttpMessage& message) const;
    void reply(int status, const char* reason, const QByteArray& contentType, const QByteArray& body, bool closeConnection);
    void fail(int status, const char* reason);
    void finishRpc(const muse::ByteArray& response);

    QTcpSocket* m_socket = nullptr;
    QByteArray m_buffer;
    QByteArray m_origin;
    QByteArray m_token;
    bool m_busy = false;
    bool m_closeAfterResponse = false;
    bool m_sse = false;
    ITransport::RequestHandler m_onRequest = nullptr;
};

class HttpTransport : public ITransport
{
public:
    explicit HttpTransport(const HttpTransportConfig& config);
    ~HttpTransport() override;

    bool start() override;
    void stop() override;

    void onRequest(const RequestHandler& onRequest) override;

    //! AGENT: the port actually bound, or 0 when not listening
    int port() const;

private:
    HttpTransportConfig m_config;
    QTcpServer* m_server = nullptr;
    RequestHandler m_onRequest = nullptr;
};

//! AGENT: helpers exposed for unit tests
bool isAllowedOrigin(const QByteArray& origin);
bool isAllowedHost(const QByteArray& host);
bool tokensEqual(const QByteArray& a, const QByteArray& b);
}
