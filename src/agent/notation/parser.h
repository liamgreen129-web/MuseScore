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
#include <vector>

#include "ast.h"

namespace mu::agent::notation {
struct ParseError {
    int line = 0; // 1-based
    int col = 0;  // 1-based
    std::string message;

    std::string toString() const; // "line 3, col 14: expected ':' after pitch"
};

struct ParseResult {
    Document doc;
    std::vector<ParseError> errors;
    bool ok() const { return errors.empty(); }
};

//! Text -> Document. A line with an error is reported and skipped; parsing continues so
//! one call reports every bad line. Mark codes are checked against NOTATION.md section 5.
ParseResult parse(const std::string& text);

//! True if `code` (with bracket/arg/text as given) is a mark NOTATION.md defines.
bool isKnownMark(const Mark& mark, std::string* why = nullptr);
}
