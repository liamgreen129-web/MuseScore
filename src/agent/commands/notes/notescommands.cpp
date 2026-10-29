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

#include <algorithm>
#include <set>

#include "registry/commandregistry.h"
#include "registry/errors.h"
#include "clearrange.h"
#include "noteops.h"
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

//! {measure, beat?} -> BarBeat, or an error naming the argument
Ret barBeat(const Params& p, const std::string& key, notes::BarBeat& out)
{
    auto it = p.find(key);
    if (it == p.end() || it->second.type() != Val::Type::Map) {
        return makeErr(Err::InvalidArg, key + " must be {measure, beat?}");
    }
    for (const auto& [k, v] : it->second.toMap()) {
        if (k == "measure") {
            const double m = v.toDouble();
            if (m != static_cast<int>(m)) {
                return makeErr(Err::InvalidArg, key + ".measure must be a whole number");
            }
            out.bar = static_cast<int>(m);
        } else if (k == "beat") {
            out.beat = v.toDouble();
            out.hasBeat = true;
        } else {
            return makeErr(Err::InvalidArg, key + " has an unknown key \"" + k + "\" (use measure and beat)");
        }
    }
    if (!it->second.toMap().count("measure")) {
        return makeErr(Err::InvalidArg, key + ".measure is required");
    }
    return muse::make_ok();
}

Ret intList(const Params& p, const std::string& key, std::vector<int>& out)
{
    auto it = p.find(key);
    if (it == p.end() || it->second.isNull()) {
        return muse::make_ok();
    }
    for (const Val& v : it->second.toList()) {
        const double d = v.toDouble();
        if (d != static_cast<int>(d)) {
            return makeErr(Err::InvalidArg, key + " must hold whole numbers");
        }
        out.push_back(static_cast<int>(d));
    }
    return muse::make_ok();
}

Ret clearRange(const Params& p, WriteContext& ctx)
{
    notes::ClearRangeArgs a;
    std::vector<int> staves;
    if (Ret r = intList(p, "staves", staves); !r) {
        return r;
    }
    if (staves.empty()) {
        return makeErr(Err::InvalidArg, "staves must list at least one staff");
    }
    std::sort(staves.begin(), staves.end());
    for (size_t i = 1; i < staves.size(); ++i) {
        if (staves[i] != staves[i - 1] + 1) {
            return makeErr(Err::Range, "staves must be next to each other (a range selection), e.g. [2, 3, 4]");
        }
    }
    a.staffFrom = staves.front();
    a.staffTo = staves.back();
    if (Ret r = barBeat(p, "from", a.from); !r) {
        return r;
    }
    if (Ret r = barBeat(p, "to", a.to); !r) {
        return r;
    }
    std::vector<int> voices;
    if (Ret r = intList(p, "voices", voices); !r) {
        return r;
    }
    if (!voices.empty()) {
        a.voices = std::set<int>(voices.begin(), voices.end());
    }
    if (auto it = p.find("include"); it != p.end() && !it->second.isNull()) {
        for (const Val& v : it->second.toList()) {
            a.include.push_back(v.toString());
        }
        if (a.include.empty()) {
            return makeErr(Err::InvalidArg, "include is empty; leave it out to delete every element type with the notes");
        }
    }
    notes::ClearRangeResult r;
    Ret ret = notes::clearRange(ctx.score, a, r);
    if (!ret) {
        return ret;
    }
    ctx.out["text"] = Val(r.text);
    ctx.out["bars"] = Val(r.fromBar == r.toBar ? std::to_string(r.fromBar) : std::to_string(r.fromBar) + "-" + std::to_string(r.toBar));
    return ret;
}

const char* TARGET_DOC = "a handle, or {staff, voice?, measure, beat|offset}: staff is the staff number (as # sN in "
                         "score_read) or {part, index}; measure the bar by position; beat counts quarter notes from 1; "
                         "offset is whole notes from the bar's start (\"1/12\") for triplet positions; voice 1-4, default 1";

void addTarget(CommandInfo& c, const std::string& what)
{
    c.input["at"] = { DataType::Object, what + ": " + TARGET_DOC, true };
    c.input["pitch"] = { DataType::String, "sounding (default) or written: how the note names are meant" };
    c.input["pitch"].enumValues = { "sounding", "written" };
    c.output["text"] = { DataType::String, "The touched bars of that staff, as score_read prints them" };
    c.output["bars"] = { DataType::String, "The bars touched, \"3\" or \"3-4\"" };
}
}

void mu::agent::registerNotesCommands(CommandRegistry& registry)
{
    {
        CommandInfo c;
        c.uri = "command://notation/tie";
        c.name = "tie";
        c.title = "Tie or untie notes";
        c.description
            = "Tie a note (or every note of a chord) to the same pitch in the next event of its voice, as selecting it "
              "and pressing the tie key does, or remove its tie. note picks one note of a chord by pitch (\"E5\"). "
              "Refused (nothing changes): a rest; a note already tied (add) or not tied (remove); no next note of that "
              "pitch, or one that is in another voice or not the next event (write the next note first with "
              "write_voice; the UI would add a note instead). One undo entry.";
        c.kind = CommandKind::Write;
        c.undoName = "Tie";
        addTarget(c, "The chord whose notes start the tie");
        c.input["note"] = { DataType::String, "One note of the chord by pitch, e.g. \"E5\" (default: every note)" };
        c.input["action"] = { DataType::String, "add (default) or remove" };
        c.input["action"].enumValues = { "add", "remove" };
        c.output["notes"] = { DataType::Integer, "How many notes were tied or untied" };
        c.write = [](const Params& p, WriteContext& ctx) { return notes::tie(ctx.score, p, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/tuplet";
        c.name = "tuplet";
        c.title = "Make a tuplet";
        c.description
            = "Turn a note, chord or rest into a tuplet of the same total length, as the tuplet keys do: the first "
              "element keeps the pitches, the others are rests to fill with write_voice. ratio \"3:2\" is three in "
              "the time of two (a quarter becomes an eighth-note triplet). Refused: grace notes, measure repeats, a "
              "ratio whose unit is not a note value for that length (e.g. 5:3 on a quarter). To write whole "
              "tuplet passages at once, use write_voice ({3:2 C5:e D5:e E5:e}). One undo entry.";
        c.kind = CommandKind::Write;
        c.undoName = "Add tuplet";
        addTarget(c, "The note, chord or rest that becomes the tuplet");
        c.input["ratio"] = { DataType::String, "actual:normal, e.g. \"3:2\" or \"5:4\"", true };
        c.input["numberType"] = { DataType::String, "number (default), ratio or none" };
        c.input["numberType"].enumValues = { "number", "ratio", "none" };
        c.input["bracketType"] = { DataType::String, "auto (default), bracket or none" };
        c.input["bracketType"].enumValues = { "auto", "bracket", "none" };
        c.output["handle"] = { DataType::String, "Handle of the tuplet's first element" };
        c.write = [](const Params& p, WriteContext& ctx) { return notes::tuplet(ctx.score, p, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/grace";
        c.name = "grace";
        c.title = "Add a grace note";
        c.description
            = "Add one grace note to a note or chord, as the grace-note palette does, at the pitch and spelling "
              "given. Grace notes before the note are added in front of any already there. type: acciaccatura "
              "(slashed eighth), appoggiatura (eighth), grace4, grace16, grace32 before the note; grace8after, "
              "grace16after, grace32after after it. Refused: a rest, a grace note as the main note, a pitch outside "
              "MIDI 0-127. Several grace notes at once: write_voice (g(B4:s C5:s) D5:h). One undo entry.";
        c.kind = CommandKind::Write;
        c.undoName = "Add grace note";
        addTarget(c, "The main note or chord");
        c.input["type"] = { DataType::String, "The kind of grace note", true };
        c.input["type"].enumValues = { "acciaccatura", "appoggiatura", "grace4", "grace16", "grace32", "grace8after",
                                       "grace16after", "grace32after" };
        c.input["note"] = { DataType::String, "The grace note's pitch, e.g. \"D5\" or \"F#4\"", true };
        c.output["handle"] = { DataType::String, "Handle of the grace note" };
        c.write = [](const Params& p, WriteContext& ctx) { return notes::grace(ctx.score, p, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/clear_range";
        c.name = "clear_range";
        c.title = "Clear a range";
        std::string types;
        for (const std::string& t : notes::clearRangeTypes()) {
            types += (types.empty() ? "" : ", ") + t;
        }
        c.description
            = "Delete the notes in a range, as selecting it and pressing Delete does: voice 1 becomes rests (whole-bar "
              "rests where a bar is cleared whole), voices 2-4 disappear from bars cleared whole and leave gaps elsewhere. "
              "staves: staff numbers (1-based, next to each other). from/to: {measure, beat}; measure is the bar by "
              "position (1-based), beat counts quarter notes from 1 (1.5 = the second eighth). Without a beat, from "
              "is the bar's start and to is the end of that bar; to with a beat stops before that beat. Both ends must "
              "be where a note or rest starts on those staves. voices (default all) limits the clear. include lists "
              "the element types deleted with the notes (default all): " + types + ". Types left out stay, "
              "e.g. include [\"slurs\",\"ties\",\"articulations\",\"lyrics\"] keeps dynamics and chord symbols. "
              "Your selection filter in MuseScore is not used or changed. Returns the cleared bars as score_read "
              "prints them. One undo entry.";
        c.kind = CommandKind::Write;
        c.undoName = "Clear range";
        c.input["staves"] = { DataType::Array, "Staff numbers, 1-based, next to each other", true };
        c.input["staves"].itemsType = DataType::Integer;
        c.input["from"] = { DataType::Object, "{measure, beat?}: where the range starts", true };
        c.input["to"] = { DataType::Object, "{measure, beat?}: the end of that bar, or up to (not including) the beat", true };
        c.input["voices"] = { DataType::Array, "Voices 1-4 to clear (default all)" };
        c.input["voices"].itemsType = DataType::Integer;
        c.input["include"] = { DataType::Array, "Element types deleted with the notes (default all)" };
        c.input["include"].itemsType = DataType::String;
        c.output["text"] = { DataType::String, "The cleared bars of those staves, as score_read prints them" };
        c.output["bars"] = { DataType::String, "The bars touched, \"3\" or \"3-6\"" };
        c.write = clearRange;
        registry.add(c);
    }

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
