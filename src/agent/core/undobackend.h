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

#include <cstddef>
#include <string>

namespace mu::engraving {
class MasterScore;
}

namespace mu::agent::core {
//! The undo operations a transaction needs. In the app this is the master notation's
//! INotationUndoStack (so the UI's undo buttons and dirty state follow); in unit tests it is
//! the score's own UndoStack (ScoreUndoBackend), with the same semantics underneath:
//! prepareChanges = Score::startCmd, commit/rollback = Score::endCmd, lock = one shared bool.
class IUndoBackend
{
public:
    virtual ~IUndoBackend() = default;

    virtual bool isLocked() const = 0;
    virtual bool hasActiveCommand() const = 0;
    virtual size_t currentIndex() const = 0;
    virtual std::string topUndoName() const = 0;

    virtual void prepareChanges(const std::string& name) = 0;
    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual void commitChanges() = 0;
    virtual void rollbackChanges() = 0;

    //! After commit or rollback: tell the views the score changed (notationChanged)
    virtual void notifyChanged() = 0;
};

//! Engraving-only backend, for tests and for anything that has a score but no notation.
class ScoreUndoBackend : public IUndoBackend
{
public:
    explicit ScoreUndoBackend(mu::engraving::MasterScore* score)
        : m_score(score) {}

    bool isLocked() const override;
    bool hasActiveCommand() const override;
    size_t currentIndex() const override;
    std::string topUndoName() const override;

    void prepareChanges(const std::string& name) override;
    void lock() override;
    void unlock() override;
    void commitChanges() override;
    void rollbackChanges() override;
    void notifyChanged() override {}

private:
    mu::engraving::MasterScore* m_score = nullptr;
};
}
