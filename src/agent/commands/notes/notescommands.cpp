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
#include "notescommands.h"

#include "registry/commandregistry.h"
#include "registry/errors.h"
#include "writevoice.h"

using namespace mu::agent;
using muse::Ret;
using muse::Val;
using muse::ValList;

namespace {
ValList strings(const std::vector<std::string>& v)
{
    ValList out;
    for (const std::string& s : v) {
        out.emplace_back(Val(s));
    }
    return out;
}

Ret writeVoice(const Params& p, WriteContext& ctx)
{
    notes::WriteVoiceArgs a;
    a.staff = static_cast<int>(p.at("staff").toDouble());
    a.content = p.at("content").toString();
    if (auto it = p.find("pitch"); it != p.end() && !it->second.isNull()) {
        a.pitch = it->second.toString() == "written" ? notes::PitchMode::Written : notes::PitchMode::Sounding;
    }
    if (auto it = p.find("keepAnnotations"); it != p.end() && !it->second.isNull()) {
        a.keepAnnotations = it->second.toBool();
    }
    notes::WriteVoiceResult r;
    Ret ret = notes::writeVoice(ctx.score, a, r);
    if (!ret) {
        return ret;
    }
    ctx.out["text"] = Val(r.text);
    ctx.out["bars"] = Val(r.fromBar == r.toBar ? std::to_string(r.fromBar) : std::to_string(r.fromBar) + "-" + std::to_string(r.toBar));
    ctx.out["annotationsRemoved"] = Val(strings(r.annotationsRemoved));
    ctx.out["tiesRemoved"] = Val(strings(r.tiesRemoved));
    return ret;
}
}

void mu::agent::registerNotesCommands(CommandRegistry& registry)
{
    CommandInfo c;
    c.uri = "command://notation/write_voice";
    c.name = "write_voice";
    c.title = "Write notes and rhythms";
    c.description
        = "Replace whole voices of one staff, bar by bar, with notes, chords, rests, tuplets, ties and grace notes "
          "in the compact notation that score_read prints. Each line \"m<N> v<V> | ...\" replaces voice V of bar N "
          "(bars by position, 1-based); other bars and voices are untouched. Copy lines from score_read, edit them, "
          "and send them back: a staff header and bar-attribute or chord-symbol lines that match the score as it is "
          "are accepted and ignored. Pitches are sounding unless pitch is \"written\" (or the header says "
          "pitch=written). Voice 1 must fill its bar; voices 2-4 use '_' for space without a rest, and a voice 2-4 "
          "line of only '_' removes that voice from the bar. A tie on the last event of the last bar written ties "
          "into the next bar, whose first event must be that pitch. Ties coming into the first bar from the bar "
          "before are kept when the new first event has that pitch, otherwise listed in tiesRemoved. "
          "Refused: marks (+...) and slurs in the content (add them with the expression and text commands), "
          "changed bar attributes or chord symbols, tablature and percussion staves, measure repeats, staves with "
          "a local time signature, and anything that does not add up (each problem is named). With "
          "keepAnnotations (default true) dynamics, hairpins, chord symbols, staff text and lines stay; "
          "articulations, lyrics and slurs on the old notes go with them. annotationsRemoved lists anything "
          "MuseScore removed while writing (an annotation between the new notes, a mid-bar clef). "
          "Returns the written bars of the staff as score_read prints them. One undo entry.";
    c.kind = CommandKind::Write;
    c.undoName = "Write voice";
    c.input["staff"] = { DataType::Integer, "Staff number, 1-based, as score_read numbers them", true };
    c.input["staff"].min = Val(1);
    c.input["content"] = { DataType::String, "Voice lines \"m<N> v<V> | ...\" in the compact notation (NOTATION.md)", true };
    c.input["pitch"] = { DataType::String, "sounding (default) or written, for transposing instruments" };
    c.input["pitch"].enumValues = { "sounding", "written" };
    c.input["keepAnnotations"] = { DataType::Boolean, "Keep dynamics, hairpins, chord symbols, text and lines (default true)" };
    c.output["text"] = { DataType::String, "The written bars of that staff, as score_read prints them" };
    c.output["bars"] = { DataType::String, "The bars written, \"3\" or \"3-6\"" };
    c.output["annotationsRemoved"] = { DataType::Array, "Annotations, clefs and breaths MuseScore removed while writing" };
    c.output["annotationsRemoved"].itemsType = DataType::String;
    c.output["tiesRemoved"] = { DataType::Array, "Ties into the first bar that no longer land" };
    c.output["tiesRemoved"].itemsType = DataType::String;
    c.write = writeVoice;
    registry.add(c);
}
