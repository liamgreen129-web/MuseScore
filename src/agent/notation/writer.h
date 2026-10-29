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

#include "ast.h"

namespace mu::agent::notation {
//! Document -> canonical text (NOTATION.md section 7 rule 5): one space between items,
//! marks in the order of section 5's table, chords low to high, '\n' after every line.
std::string write(const Document& doc);

std::string writePitch(const Pitch& p);
std::string writeDuration(const Duration& d);
std::string writeMark(const Mark& m);
std::string writeItems(const std::vector<Item>& items);

//! Rank of a mark in the canonical order (section 5's table).
int markRank(const Mark& m);
}
