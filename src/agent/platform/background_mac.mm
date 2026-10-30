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
    // Prohibited apps have no Dock icon and cannot be activated at all: an accessory app is
    // still activated by QWindow::requestActivate (a score opened, a dialog shown), which
    // flashes its window over the user's and takes them out of a full-screen space. Hiding
    // gives focus back to whatever the user was using.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    [NSApp hide:nil];

    // Belt and braces: if anything still activates or unhides the app, hide it again.
    static bool observing = false;
    if (!observing) {
        observing = true;
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        for (NSNotificationName name : { NSApplicationDidBecomeActiveNotification, NSApplicationDidUnhideNotification }) {
            [nc addObserverForName:name object:NSApp queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification*) {
                [NSApp hide:nil];
            }];
        }
    }
}
