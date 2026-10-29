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

namespace mu::agent {
class CommandRegistry;

//! Write commands for the core's integration tests only (tests/agent/test_core.py), registered
//! when MUSESCORE_AGENT_TEST_COMMANDS=1. They exercise the transaction machinery over MCP before
//! the real write groups exist; command://test/... so they can never be mistaken for real ones.
void registerTestCommands(CommandRegistry& registry);
}
