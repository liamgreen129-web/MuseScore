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

#include <string>

#include "global/types/ret.h"

namespace mu::agent {
//! Error codes shared by every command group. Group-specific codes (E_TUPLET, E_GRACE...)
//! are added here by the conductor when a group is merged, so the list stays in one place.
//! A failed command's Ret text is "<NAME>: <message>"; clients match on the name.
enum class Err {
    InvalidArg = 9000,      // argument missing, unknown, wrong type or out of its schema range
    UnknownCommand,
    NoScore,                // the command needs an open score and there is none
    Busy,                   // the user is mid-edit (drag, text edit, note input) or a modal is open
    ModalOpen,
    Range,                  // an address (measure, staff, voice, tick) is outside the score
    StaleHandle,            // a handle no longer resolves to the element it named
    Exists,
    PostCondition,          // the edit ran but its effect could not be verified; rolled back
    Internal,
};

inline const char* errName(Err e)
{
    switch (e) {
    case Err::InvalidArg: return "E_INVALID_ARG";
    case Err::UnknownCommand: return "E_UNKNOWN_COMMAND";
    case Err::NoScore: return "E_NO_SCORE";
    case Err::Busy: return "E_BUSY";
    case Err::ModalOpen: return "E_MODAL_OPEN";
    case Err::Range: return "E_RANGE";
    case Err::StaleHandle: return "E_STALE_HANDLE";
    case Err::Exists: return "E_EXISTS";
    case Err::PostCondition: return "E_POSTCONDITION";
    case Err::Internal: return "E_INTERNAL";
    }
    return "E_INTERNAL";
}

inline muse::Ret makeErr(Err e, const std::string& message)
{
    return muse::Ret(static_cast<int>(e), std::string(errName(e)) + ": " + message);
}
}
