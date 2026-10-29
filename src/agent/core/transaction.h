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

#include <functional>
#include <string>
#include <vector>

#include "global/types/ret.h"

#include "core/selectionguard.h"
#include "core/undobackend.h"

namespace mu::engraving {
class MasterScore;
class UndoCommand;
}

namespace mu::agent::core {
//! One agent request = one transaction = one undo entry (CLAUDE.md rule 3).
//!
//!   Transaction tx(score, backend);
//!   if (Ret r = tx.begin("Add dynamic"); !r) return r;     // refuses if a macro is open or locked
//!   if (Ret r = tx.step("add_dynamic", [&] { ...; return make_ok(); }); !r) return r; // rolled back
//!   return tx.commit();
//!
//! begin: refuse when the undo stack is locked or a macro is active (a user drag, a plugin, the
//! text editor, another agent request) because prepareChanges would then be a no-op and the edit
//! would land in someone else's macro; clear MScore's error; prepareChanges(name); lock(); save
//! and clear the user's selection (SelectionGuard), so selection-reading engraving paths see
//! only what the step selects.
//!
//! step: run one operation. Afterwards check MScore::_error (paths that fail by setting it
//! rather than returning: a non-warning error fails the step, a warning is recorded and
//! cleared), that the macro is still open, ours and locked, and that it was not unwound
//! (cmdAddTimeSig and friends can unwind(); a sentinel first child makes that visible even in
//! step 1). A failed step, or an exception, rolls the whole transaction back at once and
//! returns "step N (label): <error>". A step that committed the macro itself is undone and
//! dropped. Steps never lock/unlock/commit themselves.
//!
//! commit: take the sentinel out, restore the user's selection and input state, unlock,
//! commitChanges, notify. The destructor rolls back anything not committed.
class Transaction
{
public:
    Transaction(mu::engraving::MasterScore* score, IUndoBackend& backend);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    muse::Ret begin(const std::string& actionName);
    muse::Ret step(const std::string& label, const std::function<muse::Ret()>& op);
    muse::Ret commit();
    void rollback();

    bool isOpen() const { return m_open; }
    //! After commit: true when the undo stack gained an entry (false for a no-op request)
    bool changed() const { return m_changed; }
    int stepsRun() const { return m_steps; }
    const std::vector<std::string>& warnings() const { return m_warnings; }
    mu::engraving::MasterScore* score() const { return m_score; }

private:
    void finish(bool rollback);
    void removeSentinel();
    void recoverClosedMacro();

    mu::engraving::MasterScore* m_score = nullptr;
    IUndoBackend& m_backend;
    SelectionGuard m_selection;
    bool m_open = false;
    bool m_changed = false;
    int m_steps = 0;
    size_t m_indexBefore = 0;
    const void* m_macro = nullptr; // compared, never dereferenced
    mu::engraving::UndoCommand* m_sentinel = nullptr;
    std::vector<std::string> m_warnings;
};
}
