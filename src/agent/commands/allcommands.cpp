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
#include "allcommands.h"

#include "app/appcommands.h"
#include "read/readcommands.h"

//! The one list of command groups. Only the conductor edits this file, at merge time
//! (PLAN.md, build-tree rules), so parallel implementers never conflict here.
void mu::agent::registerAllCommands(CommandRegistry& registry)
{
    registerAppCommands(registry);
    registerReadCommands(registry);
}
