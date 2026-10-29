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
//! MUSESCORE_AGENT_BACKGROUND=1 (set by the test harness): run as a background app that
//! never takes focus or covers the user's windows. On macOS: accessory activation policy
//! (no Dock icon, never frontmost) and the app hidden. No-op elsewhere.
bool backgroundModeRequested();
void enterBackgroundMode();
}
