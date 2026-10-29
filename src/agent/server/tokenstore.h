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

#include <QByteArray>
#include <QString>

namespace mu::agent {
//! The bearer token shared by every agent instance of this user.
//! Stored as 64 hex characters in <dir>/token, owner read/write only, in a 0700 folder.
//! Created on first use; an unreadable or malformed file is replaced.
class TokenStore
{
public:
    static QByteArray loadOrCreate(const QString& dir);

    static bool isWellFormed(const QByteArray& token);
};
}
