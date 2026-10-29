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

#include "core/undobackend.h"
#include "notation/inotation.h"

namespace mu::agent {
//! The transaction's undo operations on the master notation's INotationUndoStack, so the UI's
//! undo/redo buttons, the window's dirty mark and the change listeners all follow agent edits.
class NotationUndoBackend : public core::IUndoBackend
{
public:
    //! \a inFront: the notation the user is looking at (a part, or the master itself); it is
    //! notified too, so a part view repaints (each view listens to its own notation)
    NotationUndoBackend(notation::INotationPtr masterNotation, notation::INotationPtr inFront)
        : m_notation(std::move(masterNotation)), m_inFront(std::move(inFront)) {}

    bool isLocked() const override;
    bool hasActiveCommand() const override;
    size_t currentIndex() const override;
    std::string topUndoName() const override;

    void prepareChanges(const std::string& name) override;
    void lock() override;
    void unlock() override;
    void commitChanges() override;
    void rollbackChanges() override;
    void notifyChanged() override;

private:
    notation::INotationPtr m_notation;
    notation::INotationPtr m_inFront;
};
}
