/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2026 MuseScore/Audacity and others
 * Ported from muse_framework framework/rcontrol/mcp/itransport.h @ ecdc6e7 for MuseScore Agent.
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

#include "global/types/bytearray.h"

namespace mu::agent::mcp {
class ITransport
{
public:
    virtual ~ITransport() = default;

    using ResponseHandler = std::function<void (const muse::ByteArray&)>;
    using RequestHandler = std::function<void (const muse::ByteArray& request, const ResponseHandler& onResponse)>;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual void onRequest(const RequestHandler& onRequest) = 0;
};
}
