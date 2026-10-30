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
#include "pitchcommands.h"

#include "pitchops.h"

#include "registry/commandregistry.h"

using namespace mu::agent;
using muse::Val;
namespace pt = mu::agent::pitch;

//! Every argument has one JSON type: the registry's Arg has a single DataType, and
//! DataType::Undefined is rendered as "type": "undefined" in tools/list (server/mcptypes.h:43),
//! which is not valid JSON Schema. So the spec's oneOf arguments are split: target (handle) or
//! staff + at; pitch (spelled) or midi (number); note (pitch) or note_index; range is always an
//! object ({score: true} for the whole score).
namespace {
Arg arg(DataType t, const std::string& d, bool required = false)
{
    Arg a;
    a.type = t;
    a.description = d;
    a.required = required;
    return a;
}

Arg enumArg(const std::string& d, const std::vector<std::string>& values, bool required = false)
{
    Arg a = arg(DataType::String, d, required);
    a.enumValues = values;
    return a;
}

Arg intArg(const std::string& d, int min, int max)
{
    Arg a = arg(DataType::Integer, d);
    a.min = Val(min);
    a.max = Val(max);
    return a;
}

std::string joined(const std::vector<std::string>& v)
{
    std::string s;
    for (const std::string& x : v) {
        s += (s.empty() ? "" : ", ") + x;
    }
    return s;
}

const char* STAFF_DESC
    = "The staff: {\"part\": \"<part name as in score_overview>\", \"index\": <staff in the part, 1-based, default 1>} "
      "or {\"number\": <staff in the score, 1-based, as # sN in score_read>}";
const char* POSITION_DESC
    = "{\"measure\": <bar by position, as m<N> in score_read; a pickup is 1>, \"beat\": <1-based, in quarter notes>} or "
      "{\"measure\", \"offset\": \"<n>/<d>\" (whole notes from the start of the bar)}";
const char* NOTATION_DESC = "The touched bars of the staff (or staves) in the compact notation, after the edit (at most 32 bars)";
}

void mu::agent::registerPitchCommands(CommandRegistry& registry)
{
    {
        CommandInfo c;
        c.uri = "command://notation/set_pitch";
        c.name = "set_pitch";
        c.title = "Set a note's pitch";
        c.description
            = "Changes the pitch (and spelling) of one existing note, keeping the note itself (its element id, "
              "articulations, lyrics...), as the arrow keys end up doing: the whole tie chain the note belongs to changes "
              "with it, the old accidental is removed from every linked copy (layout puts the right one back), and linked "
              "staves and parts follow. Address the note with target (a Note handle, or a Chord handle) or with staff + at "
              "(+ voice); in a chord pick the tone with note (its pitch, e.g. \"E4\", in the call's space) or note_index "
              "(1 = the lowest); grace (1-based) picks a grace note before it. Give pitch, a spelled pitch whose spelling is "
              "kept (\"F#4\", \"Bbb3\"; C4 = middle C; accidentals #, ##, b, bb), or midi, a number spelled by spelling "
              "(auto = nearest in the key, the default; sharps; flats). space: sounding (default; what score_read prints, "
              "transposition and any 8va included) or written. On a TAB staff the note is refretted on the highest string "
              "that plays it, or on tab {string, fret} (string 0 = the top line). On a drum staff give midi, the drum "
              "sound. Refuses (E_INVALID_ARG): target with staff/at; a rest; a chord of several notes without note or "
              "note_index, or no such tone; both or neither of pitch and midi; a pitch that is not spelled as above or "
              "outside MIDI 0-127, or that would be stored outside 0-127; spelling with pitch; a spelling that would need "
              "a triple accidental in the other view; tab on a non-TAB staff, a string or fret outside the instrument, a "
              "tab that does not play the pitch, a pitch no string can play; on a drum staff pitch, space or spelling, or "
              "a midi that is not in the drumset; a note in a guitar bend. E_RANGE: a staff, bar or voice outside the "
              "score, a position where no note or rest starts, note_index or grace past the chord's. E_STALE_HANDLE: a "
              "handle that no longer resolves. Returns the note's new handle, its sounding and written pitch, how many "
              "tied notes changed with it, status (changed or unchanged) and the bars' notation.";
        c.kind = CommandKind::Write;
        c.undoName = "Set pitch";
        c.input["target"] = arg(DataType::String, "A Note or Chord handle from score_read detail (instead of staff + at)");
        c.input["staff"] = arg(DataType::Object, STAFF_DESC);
        c.input["at"] = arg(DataType::Object, std::string("Where the note starts: ") + POSITION_DESC);
        c.input["voice"] = intArg("Voice 1-4 (default 1)", 1, 4);
        c.input["note"] = arg(DataType::String, "In a chord: the tone to change, by its current pitch (\"E4\")");
        c.input["note_index"] = intArg("In a chord: the tone to change, 1 = the lowest", 1, 32);
        c.input["grace"] = intArg("A grace note before the note: 1 = the first", 1, 32);
        c.input["pitch"] = arg(DataType::String, "The new pitch, spelled: \"F#4\", \"Bbb3\" (C4 = middle C)");
        c.input["midi"] = intArg("The new pitch as a MIDI number (60 = middle C), spelled by spelling", 0, 127);
        c.input["space"] = enumArg("sounding (default, as score_read prints) or written (as the player reads it)",
                                   { "sounding", "written" });
        c.input["spelling"] = enumArg("With midi only: auto (default, nearest in the key), sharps or flats",
                                      { "auto", "sharps", "flats" });
        c.input["tab"] = arg(DataType::Object, "TAB staves only: {\"string\": S (0 = the top line), \"fret\": F}");
        c.output["handle"] = arg(DataType::String, "The note's handle after the edit (the fingerprint holds the pitch)");
        c.output["status"] = arg(DataType::String, "changed, or unchanged when it already had that pitch and spelling");
        c.output["pitch"] = arg(DataType::String, "Its sounding pitch, as score_read prints it");
        c.output["writtenPitch"] = arg(DataType::String, "Its written pitch");
        c.output["tiedNotes"] = arg(DataType::Integer, "Notes in its tie chain, itself included, that now have this pitch");
        c.output["string"] = arg(DataType::Integer, "TAB staves: the string (0 = the top line)");
        c.output["fret"] = arg(DataType::Integer, "TAB staves: the fret");
        c.output["bars"] = arg(DataType::String, "The bars the tie chain covers, e.g. m3-m5");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return pt::setPitch(p, ctx.score, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/transpose";
        c.name = "transpose";
        c.title = "Transpose";
        c.description
            = "Transposes notes, and optionally key signatures and chord symbols, as Tools > Transpose does. range: "
              "{score: true, staves?} for the whole score; {start, end, staves?} where start and end are {measure, "
              "beat|offset}, end inclusive (the note or rest starting there is transposed; give only end.measure for the "
              "whole bar); staves lists staves ({\"number\": 3} or {\"part\": \"Flute\", \"index\": 1}; default all); or "
              "{handles: [...]} with Note, Chord, chord-symbol or fret-diagram handles. by: {interval: one of "
            + joined(pt::intervalNames())
            + ", direction: up|down, octaves: 0-3 (compound intervals: a ninth is M2 + 1 octave)}; {key: -7..7 (the "
              "concert key as sharps/flats; a minor key uses its relative major's count), direction: closest (default) | "
              "up | down}; or {degrees: 1-6 (1 = a second, diatonic, within the key), direction, keep_alterations "
              "(default true)}. Spelling follows the interval exactly (C up a d2 is Db); double_accidentals false "
              "respells double sharps and flats. key_signatures (default true, ranges only) changes the key changes inside "
              "the range and the one at its start; none is added at its edges except at the start of the score. "
              "chord_symbols (default true) moves chord symbols and fret diagrams. Percussion staves are skipped. The "
              "user's selection filter is ignored (everything is transposed). A tie that leaves what is transposed is "
              "refused unless ties is extend, which widens the range over the whole tie chain. Refuses (E_INVALID_ARG): "
              "a malformed range or by, an interval not in the list, a P1 with no octaves, by.key or key_signatures with "
              "handles, key_signatures with degrees, end before start, a note that would leave MIDI 0-127 (diatonic too, "
              "where MuseScore would wrap it by octaves), a tie leaving the range, chord-symbol handles with "
              "chord_symbols false, nothing pitched to transpose (only percussion, only rests). E_RANGE: a bar or staff "
              "outside the score, or a start/end where no note or rest starts on those staves. E_STALE_HANDLE: a handle "
              "that no longer resolves. To the key already in force with direction closest it changes nothing (status "
              "unchanged). A fret diagram whose new shape is unknown is cleared and reported in warnings. Returns the "
              "interval used, counts of notes, chord symbols and key signatures, and the notation of the bars.";
        c.kind = CommandKind::Write;
        c.undoName = "Transpose";
        c.input["range"] = arg(DataType::Object, "{\"score\": true}, {\"start\": {\"measure\": M, \"beat\": B}, \"end\": {...}, "
                                                 "\"staves\": [...]} or {\"handles\": [...]}", true);
        c.input["by"] = arg(DataType::Object, "{\"interval\": \"M2\", \"direction\": \"up\", \"octaves\": 0}, {\"key\": -2, "
                                              "\"direction\": \"closest\"} or {\"degrees\": 2, \"direction\": \"up\", "
                                              "\"keep_alterations\": true}", true);
        c.input["key_signatures"] = arg(DataType::Boolean, "Transpose key signatures in the range (default true; interval and key only)");
        c.input["chord_symbols"] = arg(DataType::Boolean, "Transpose chord symbols and fret diagrams (default true)");
        c.input["double_accidentals"] = arg(DataType::Boolean, "Allow double sharps and flats in the result (default true)");
        c.input["ties"] = enumArg("A tie leaving what is transposed: reject (default) or extend the range over the chain",
                                  { "reject", "extend" });
        c.output["status"] = arg(DataType::String, "transposed, or unchanged (already in that key)");
        c.output["interval"] = arg(DataType::String, "The interval applied, e.g. \"M2 up\" or \"M3 + 1 octave(s) down\"");
        c.output["notes"] = arg(DataType::Integer, "Notes transposed (linked copies not counted)");
        c.output["chordSymbols"] = arg(DataType::Integer, "Chord symbols transposed");
        c.output["keySignatures"] = arg(DataType::Integer, "Key signatures changed");
        c.output["extendedForTies"] = arg(DataType::Boolean, "ties extend widened the range");
        c.output["bars"] = arg(DataType::String, "The bars transposed, e.g. m9-m16");
        c.output["notationBars"] = arg(DataType::String, "When the range is longer than 32 bars: the bars notation holds");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return pt::transpose(p, ctx.score, ctx.out); };
        registry.add(c);
    }
}
