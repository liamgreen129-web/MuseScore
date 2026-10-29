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

#include <QJsonObject>
#include <QString>
#include <vector>

namespace mu::agent {
//! One JSON file per running agent instance: <agentDir>/instances/<pid>.json (0600).
//! Lets a client (or instances_list, P3.3) find every open score's port without probing.
//! Never under the official app's folder (DECISIONS 2026-10-29).
class DiscoveryRecords
{
public:
    explicit DiscoveryRecords(const QString& agentDir);

    //! Writes or rewrites this process's record.
    bool write(const QJsonObject& record);

    //! Deletes this process's record.
    void remove();

    //! Deletes records whose process is gone (crash leftovers). Returns how many.
    int pruneStale();

    //! Records of live processes.
    std::vector<QJsonObject> readAll() const;

    QString selfPath() const;

private:
    QString m_dir;
    long long m_pid = 0;
};
}
