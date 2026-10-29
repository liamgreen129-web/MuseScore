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
#include "background.h"

#include <cstdlib>
#include <string>

#import <AppKit/AppKit.h>

bool mu::agent::backgroundModeRequested()
{
    const char* v = std::getenv("MUSESCORE_AGENT_BACKGROUND");
    return v && std::string(v) == "1";
}

void mu::agent::enterBackgroundMode()
{
    // Accessory apps have no Dock icon and are never made frontmost by the system; hiding
    // gives focus back to whatever the user was using.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp hide:nil];
}
