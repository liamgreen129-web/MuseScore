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
#include <optional>
#include <string>
#include <vector>

#include "ast.h"

namespace mu::agent::notation {
struct Problem {
    std::string code;    // "E_BAR_SUM", "E_TIE", "E_TUPLET"
    std::string message; // "m12 v1 is 3/8 short"
};

//! Length of bar `index` (1-based) on `staff` in whole notes, or nullopt when unknown
//! (the bar is outside the score). Comes from the score in write_voice, from the
//! document's own `time=`/`len=` attributes when checking text alone.
using BarLengthFn = std::function<std::optional<Rational>(int staff, int index)>;

//! NOTATION.md section 7, rules 1, 3 and 4.
std::vector<Problem> validate(const Document& doc, const BarLengthFn& barLength);

//! Bar lengths from the document's own `time=` and `len=` attributes (and staff headers).
BarLengthFn barLengthsFromDocument(const Document& doc);
}
