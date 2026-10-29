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
#include "undobackend.h"

#include "engraving/dom/masterscore.h"
#include "engraving/editing/undo.h"

using namespace mu::agent::core;
using namespace mu::engraving;

bool ScoreUndoBackend::isLocked() const
{
    return m_score->undoStack()->isLocked();
}

bool ScoreUndoBackend::hasActiveCommand() const
{
    return m_score->undoStack()->hasActiveCommand();
}

size_t ScoreUndoBackend::currentIndex() const
{
    return m_score->undoStack()->currentIndex();
}

std::string ScoreUndoBackend::topUndoName() const
{
    const UndoMacro* last = m_score->undoStack()->last();
    return last ? last->actionName().str.toStdString() : std::string();
}

void ScoreUndoBackend::prepareChanges(const std::string& name)
{
    m_score->startCmd(muse::TranslatableString::untranslatable(muse::String::fromStdString(name)));
}

void ScoreUndoBackend::lock()
{
    m_score->undoStack()->setLocked(true);
}

void ScoreUndoBackend::unlock()
{
    m_score->undoStack()->setLocked(false);
}

void ScoreUndoBackend::commitChanges()
{
    m_score->endCmd();
}

void ScoreUndoBackend::rollbackChanges()
{
    m_score->endCmd(true);
}
