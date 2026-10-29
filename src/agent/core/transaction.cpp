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
#include "transaction.h"

#include <exception>

#include "engraving/dom/masterscore.h"
#include "engraving/dom/mscore.h"
#include "engraving/editing/undo.h"

#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::agent::core;
using namespace mu::engraving;
using muse::Ret;

namespace {
//! First child of every agent macro. It does nothing; unwind() deletes it with everything else,
//! so a step that unwinds is caught even when it is the first, and it is taken out before commit
//! so an agent request that changed nothing still leaves no undo entry.
class Sentinel : public UndoCommand
{
public:
    UNDO_NAME("AgentTransactionSentinel")
};

bool sentinelFirst(const UndoMacro* macro, const UndoCommand* sentinel)
{
    return macro && sentinel && !macro->commands().empty() && macro->commands().front() == sentinel;
}
}

Transaction::Transaction(MasterScore* score, IUndoBackend& backend)
    : m_score(score), m_backend(backend)
{
}

Transaction::~Transaction()
{
    if (m_open) {
        finish(true);
    }
}

Ret Transaction::begin(const std::string& actionName)
{
    if (!m_score) {
        return makeErr(Err::NoScore, "no score is open in this instance");
    }
    if (m_open) {
        return makeErr(Err::Internal, "transaction already open");
    }
    // prepareChanges is a silent no-op in both cases, and the edit would join someone else's macro
    if (m_backend.isLocked() || m_backend.hasActiveCommand()) {
        return makeErr(Err::Busy, "MuseScore is in the middle of another edit (a drag, text edit, plugin or another request); "
                                  "try again when it has finished");
    }

    m_indexBefore = m_backend.currentIndex();
    m_backend.prepareChanges(actionName);
    if (!m_backend.hasActiveCommand()) {
        return makeErr(Err::Internal, "could not open an undo entry");
    }
    MScore::setError(MsError::MS_NO_ERROR); // startCmd clears it too; be explicit
    m_backend.lock();
    m_macro = m_score->undoStack()->activeCommand();
    m_sentinel = new Sentinel();
    m_score->undoStack()->activeCommand()->appendChild(m_sentinel);
    m_open = true;
    m_steps = 0;
    m_changed = false;
    m_warnings.clear();

    // after prepareChanges, so the undo entry's own snapshot holds the user's selection
    m_selection.save(m_score);
    m_selection.clear();
    return muse::make_ok();
}

Ret Transaction::step(const std::string& label, const std::function<Ret()>& op)
{
    if (!m_open) {
        return makeErr(Err::Internal, "no open transaction for step " + label);
    }
    ++m_steps;
    const std::string where = "step " + std::to_string(m_steps) + " (" + label + "): ";
    UndoStack* stack = m_score->undoStack();

    Ret ret = muse::make_ok();
    try {
        ret = op();
    } catch (const std::exception& e) {
        ret = makeErr(Err::Internal, std::string("exception: ") + e.what());
    } catch (...) {
        ret = makeErr(Err::Internal, "unknown exception");
    }

    // the macro must still be ours, still locked, and nothing may have unwound it
    if (stack->activeCommand() != m_macro) {
        recoverClosedMacro();
        return muse::Ret(static_cast<int>(Err::Internal), where + "E_INTERNAL: the operation closed the transaction's undo entry; "
                         "its changes were undone");
    }
    if (!sentinelFirst(stack->activeCommand(), m_sentinel)) {
        m_sentinel = nullptr; // deleted by the unwind
        ret = makeErr(Err::Refused, "MuseScore undid the edit's changes (unwind)");
    } else if (!m_backend.isLocked()) {
        ret = makeErr(Err::Internal, "the operation released the transaction's undo lock");
    }

    if (MScore::_error != MsError::MS_NO_ERROR) {
        const std::string msg = MScore::errorToString(MScore::_error);
        if (MScore::_errorIsWarning) {
            m_warnings.push_back(where + msg);
        } else if (ret) {
            ret = makeErr(Err::Refused, msg);
        }
        MScore::setError(MsError::MS_NO_ERROR);
    }

    if (!ret) {
        finish(true);
        return muse::Ret(ret.code(), where + ret.text());
    }
    return muse::make_ok();
}

Ret Transaction::commit()
{
    if (!m_open) {
        return makeErr(Err::Internal, "no open transaction to commit");
    }
    finish(false);
    // endCmd rolls back by itself when an error is pending; step() clears them, so this means a bug
    if (!m_changed && m_backend.currentIndex() != m_indexBefore) {
        return makeErr(Err::Internal, "the undo stack moved unexpectedly at commit");
    }
    return muse::make_ok();
}

void Transaction::rollback()
{
    if (m_open) {
        finish(true);
    }
}

void Transaction::removeSentinel()
{
    UndoMacro* macro = m_score->undoStack()->activeCommand();
    if (!sentinelFirst(macro, m_sentinel)) {
        m_sentinel = nullptr;
        return;
    }
    // removeChild takes the last child: lift the others off, drop the sentinel, put them back
    std::vector<UndoCommand*> rest;
    while (macro->childCount() > 1) {
        rest.push_back(macro->removeChild());
    }
    delete macro->removeChild();
    m_sentinel = nullptr;
    for (auto it = rest.rbegin(); it != rest.rend(); ++it) {
        macro->appendChild(*it);
    }
}

void Transaction::recoverClosedMacro()
{
    // An operation unlocked and committed (or rolled back) our macro. If it was committed, it is
    // the top undo entry now: undo it and drop it, so nothing half-done stays in the score.
    UndoStack* stack = m_score->undoStack();
    m_open = false;
    m_sentinel = nullptr; // went with the macro
    if (stack->hasActiveCommand()) {
        stack->setLocked(false);
        m_backend.rollbackChanges(); // someone else's macro now; never ours to keep
    }
    if (stack->last() == m_macro && stack->currentIndex() == m_indexBefore + 1) {
        m_score->undoRedo(true, nullptr);
        stack->cleanRedoStack();
    }
    m_macro = nullptr;
    m_backend.unlock();
    MScore::setError(MsError::MS_NO_ERROR);
    m_selection.restore();
    m_backend.notifyChanged();
}

void Transaction::finish(bool rollback)
{
    m_open = false;
    if (!rollback) {
        removeSentinel();
        // inside the macro, so the undo entry's redo state and the live score agree on it
        m_selection.restore();
    }
    m_backend.unlock(); // commit and rollback are no-ops while locked
    if (rollback) {
        MScore::setError(MsError::MS_NO_ERROR);
        m_backend.rollbackChanges();
        m_selection.restore(); // after unwind, so removed-then-restored elements are back in the tree
    } else {
        m_backend.commitChanges();
    }
    m_changed = !rollback && m_backend.currentIndex() == m_indexBefore + 1;
    m_macro = nullptr;
    m_sentinel = nullptr; // removed before commit, or unwound and deleted by the rollback
    MScore::setError(MsError::MS_NO_ERROR);
    m_backend.notifyChanged();
}
