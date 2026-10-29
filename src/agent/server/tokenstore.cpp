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
#include "tokenstore.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QRandomGenerator>
#include <QSaveFile>

#include "log.h"

using namespace mu::agent;

static const int TOKEN_BYTES = 32;

bool TokenStore::isWellFormed(const QByteArray& token)
{
    if (token.size() != TOKEN_BYTES * 2) {
        return false;
    }
    for (const char ch : token) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

QByteArray TokenStore::loadOrCreate(const QString& dir)
{
    if (!QDir().mkpath(dir)) {
        LOGE() << "cannot create token folder: " << dir;
        return QByteArray();
    }
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    const QString path = dir + "/token";

    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly)) {
        const QByteArray token = existing.readAll().trimmed();
        existing.close();
        if (isWellFormed(token)) {
            return token;
        }
        LOGW() << "token file is malformed, replacing it";
    }

    QByteArray raw(TOKEN_BYTES, '\0');
    QRandomGenerator* rng = QRandomGenerator::system();
    for (int i = 0; i < TOKEN_BYTES; ++i) {
        raw[i] = static_cast<char>(rng->bounded(256));
    }
    const QByteArray token = raw.toHex();

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        LOGE() << "cannot write token file: " << path;
        return QByteArray();
    }
    file.write(token);
    file.write("\n");
    if (!file.commit()) {
        LOGE() << "cannot commit token file: " << path;
        return QByteArray();
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    LOGI() << "created agent token at " << path;
    return token;
}
