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
#include "notationundobackend.h"

#include "engraving/dom/masterscore.h"
#include "engraving/editing/undo.h"
#include "notation/inotationinteraction.h"
#include "notation/internal/inotationundostack.h"

using namespace mu::agent;

bool NotationUndoBackend::isLocked() const
{
    return m_notation->undoStack()->isLocked();
}

bool NotationUndoBackend::hasActiveCommand() const
{
    const mu::engraving::Score* score = m_notation->elements()->msScore();
    return score && score->masterScore()->undoStack()->hasActiveCommand();
}

size_t NotationUndoBackend::currentIndex() const
{
    return m_notation->undoStack()->currentStateIndex();
}

std::string NotationUndoBackend::topUndoName() const
{
    return m_notation->undoStack()->topMostUndoActionName().str.toStdString();
}

void NotationUndoBackend::prepareChanges(const std::string& name)
{
    m_notation->undoStack()->prepareChanges(muse::TranslatableString::untranslatable(muse::String::fromStdString(name)));
}

void NotationUndoBackend::lock()
{
    m_notation->undoStack()->lock();
}

void NotationUndoBackend::unlock()
{
    m_notation->undoStack()->unlock();
}

void NotationUndoBackend::commitChanges()
{
    m_notation->undoStack()->commitChanges();
}

void NotationUndoBackend::rollbackChanges()
{
    m_notation->undoStack()->rollbackChanges();
}

void NotationUndoBackend::notifyChanged()
{
    m_notation->notationChanged().notify();
    m_notation->interaction()->selectionChanged().notify();
    if (m_inFront && m_inFront != m_notation) {
        m_inFront->notationChanged().notify();
        m_inFront->interaction()->selectionChanged().notify();
    }
}
