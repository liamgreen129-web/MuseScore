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
#include "discoveryrecords.h"

#include <cerrno>
#include <csignal>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QJsonDocument>
#include <QSaveFile>

#include "log.h"

using namespace mu::agent;

static bool processAlive(long long pid)
{
    if (pid <= 0) {
        return false;
    }
    // kill(pid, 0) checks existence; EPERM still means the process exists
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

DiscoveryRecords::DiscoveryRecords(const QString& agentDir)
    : m_dir(agentDir + "/instances"), m_pid(QCoreApplication::applicationPid())
{
}

QString DiscoveryRecords::selfPath() const
{
    return m_dir + "/" + QString::number(m_pid) + ".json";
}

bool DiscoveryRecords::write(const QJsonObject& record)
{
    if (!QDir().mkpath(m_dir)) {
        LOGE() << "cannot create " << m_dir;
        return false;
    }
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QSaveFile file(selfPath());
    if (!file.open(QIODevice::WriteOnly)) {
        LOGE() << "cannot write discovery record " << selfPath();
        return false;
    }
    file.write(QJsonDocument(record).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        LOGE() << "cannot commit discovery record " << selfPath();
        return false;
    }
    QFile::setPermissions(selfPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

void DiscoveryRecords::remove()
{
    QFile::remove(selfPath());
}

int DiscoveryRecords::pruneStale()
{
    int removed = 0;
    const QDir dir(m_dir);
    for (const QString& name : dir.entryList({ "*.json" }, QDir::Files)) {
        bool ok = false;
        const long long pid = QString(name).chopped(5).toLongLong(&ok);
        if (!ok || (pid != m_pid && !processAlive(pid))) {
            if (QFile::remove(dir.filePath(name))) {
                ++removed;
            }
        }
    }
    return removed;
}

std::vector<QJsonObject> DiscoveryRecords::readAll() const
{
    std::vector<QJsonObject> records;
    const QDir dir(m_dir);
    for (const QString& name : dir.entryList({ "*.json" }, QDir::Files, QDir::Name)) {
        bool ok = false;
        const long long pid = QString(name).chopped(5).toLongLong(&ok);
        if (!ok || !processAlive(pid)) {
            continue;
        }
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject()) {
            records.push_back(doc.object());
        }
    }
    return records;
}
