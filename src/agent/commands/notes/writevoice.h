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

#include "global/types/ret.h"

namespace mu::engraving {
class MasterScore;
}

namespace mu::agent::notes {
enum class PitchMode {
    Default, // sounding, unless the content's staff header says pitch=written
    Sounding,
    Written
};

struct WriteVoiceArgs {
    int staff = 1;              // 1-based, as score_read numbers staves
    std::string content;        // voice lines "m<N> v<V> | ..." (NOTATION.md)
    PitchMode pitch = PitchMode::Default;
    bool keepAnnotations = true; // dynamics, chord symbols, staff text, lines on the staff stay
};

struct WriteVoiceResult {
    int fromBar = 0;
    int toBar = 0;
    std::string text;                           // the written bars as score_read prints them (that staff)
    std::vector<std::string> annotationsRemoved; // annotations MuseScore removed while writing
    std::vector<std::string> tiesRemoved;        // ties into the range that no longer land
};

//! write_voice (P2.3, specs/notes.md): replace whole voices of one staff, bar by bar, with the
//! content. Runs inside an open transaction (one step); validates everything before changing
//! anything; checks the result by printing the bars back and comparing them with the content.
//! Engraving calls only (CLAUDE.md rule 4): Score::deleteRange to clear each voice, then the
//! note-input engine with a private InputState (addPitch, enterRest, addTuplet, setGraceNote),
//! ties between the created notes, a gap property for '_' in voices 2-4.
muse::Ret writeVoice(mu::engraving::MasterScore* score, const WriteVoiceArgs& args, WriteVoiceResult& result);
}
