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
#include "textcommands.h"

#include "textops.h"

#include "registry/commandregistry.h"

using namespace mu::agent;
using muse::Val;
namespace tx = mu::agent::text;

namespace {
const char* STAFF_DESC
    = "The staff: its number in the score (1-based, as # sN in score_read), {\"number\": N}, or "
      "{\"part\": \"<part name as in score_overview>\", \"index\": <staff in the part, 1-based, default 1>}";
const char* POSITION_DESC
    = "{\"measure\": <bar by position, as m<N> in score_read; a pickup is 1>, \"beat\": <1-based, in quarter notes "
      "whatever the time signature: 1.5 is an eighth after the downbeat>} or {\"measure\", \"offset\": \"<n>/<d>\" "
      "(whole notes from the start of the bar, for triplet positions)}";
const char* TARGET_DESC
    = "Instead of staff + at (+ voice): a Note, Chord or Rest handle from score_read detail (a note means its chord; a "
      "grace note means its main note)";
const char* VOICE_DESC = "Voice 1-4 (default 1)";
const char* NOTATION_DESC = "The touched bars of that staff in the compact notation, after the edit";

Arg arg(DataType t, const std::string& d, bool required = false)
{
    Arg a;
    a.type = t;
    a.description = d;
    a.required = required;
    return a;
}

Arg voiceArg()
{
    // no schema min/max: a voice outside 1-4 is E_RANGE from the command, as in address/address.h
    return arg(DataType::Integer, VOICE_DESC);
}

Arg enumArg(const std::string& d, const std::vector<std::string>& values, bool required = false)
{
    Arg a = arg(DataType::String, d, required);
    a.enumValues = values;
    return a;
}

Arg listOf(DataType item, const std::string& d, bool required = false)
{
    Arg a = arg(DataType::Array, d, required);
    a.itemsType = item;
    return a;
}
}

void mu::agent::registerTextCommands(CommandRegistry& registry)
{
    {
        CommandInfo c;
        c.uri = "command://notation/add_text";
        c.name = "add_text";
        c.title = "Add text";
        c.description
            = "Adds text to the note or rest that starts at a position, as Add > Text does, with its text set: kind staff "
              "(staff text such as \"pizz.\", on that voice), system (system text: always on the top staff and in every "
              "part), expression (\"dolce\", \"espress.\"; voice_assignment all_voices by default), tempo (a tempo marking on "
              "the top staff that sets playback: text \"Allegro\", and/or tempo.bpm; with bpm alone the text is a "
              "metronome mark like the one MuseScore writes, e.g. q = 132) or rehearsal (a rehearsal mark; without text "
              "it continues the sequence A, B, C... as MuseScore does). text is plain text; xml is MuseScore markup "
              "limited to <sym>Name</sym>, <b>, <i>, <u>, <s>, <sub>, <sup>. A tempo text with a metronome mark plays "
              "that number; words only with tempo.bpm play bpm (and stop following the text); words only without bpm "
              "keep MuseScore's default 80 to the beat (reported in messages). Refuses (E_RANGE) when no note or rest "
              "starts exactly there (system kinds use any voice when voice is not given) or the voice is outside 1-4; "
              "with E_INVALID_ARG a measure repeat, empty text, markup outside that subset, kind tempo with neither "
              "text nor tempo.bpm, tempo given for a kind other than tempo or voice_assignment for a kind other than "
              "expression, a tempo text that already has a metronome mark together with tempo.bpm, and a tempo "
              "outside 5-999 quarter notes a minute (after beat_unit); with E_EXISTS a second tempo marking or "
              "rehearsal mark at the same place, or the same text twice there. Returns the handle, where it went, and the bar's notation (staff text "
              "shows as +\"pizz.\", system text +sys\"...\", expression +exp\"...\", tempo +tempo\"...\"(q=N), "
              "rehearsal marks as mark= on the bar line of the top staff).";
        c.kind = CommandKind::Write;
        c.undoName = "Add text";
        c.input["kind"] = enumArg("staff, system, expression, tempo or rehearsal", tx::textKinds(), true);
        c.input["staff"] = arg(DataType::Undefined, STAFF_DESC);
        c.input["at"] = arg(DataType::Object, std::string("The note or rest it attaches to: ") + POSITION_DESC);
        c.input["voice"] = voiceArg();
        c.input["target"] = arg(DataType::String, TARGET_DESC);
        c.input["text"] = arg(DataType::String, "The text, plain (escaped for you)");
        c.input["xml"] = arg(DataType::String, "Instead of text: MuseScore text markup, e.g. \"<sym>metNoteQuarterUp</sym> = 60\" "
                                               "or \"<i>dolce</i>\"");
        c.input["tempo"] = arg(DataType::Object, std::string("kind tempo only: {\"bpm\": <number>, \"beat_unit\": one of ") + tx::beatUnitList()
                                                 + " (default: the beat of the time signature, as MuseScore's default "
                                                 "marking: q, q. in 6/8, e in 3/8, s or t in /16 and /32 metres), "
                                                 "\"follow_text\": <bool>}");
        c.input["voice_assignment"] = enumArg("kind expression only: all_voices (default), this_voice, or preference (follow this "
                                              "MuseScore's 'dynamics apply to all voices' setting, as the UI does)",
                                              { "all_voices", "this_voice", "preference" });
        c.output["handle"] = arg(DataType::String, "Handle of the text");
        c.output["status"] = arg(DataType::String, "added");
        c.output["kind"] = arg(DataType::String, "The kind");
        c.output["text"] = arg(DataType::String, "The text as plain text");
        c.output["staff"] = arg(DataType::Integer, "The staff it is on (1 for system, tempo and rehearsal)");
        c.output["voice"] = arg(DataType::Integer, "The voice it is on");
        c.output["bpm"] = arg(DataType::Float, "kind tempo: what it plays, in quarter notes a minute");
        c.output["followText"] = arg(DataType::Boolean, "kind tempo: whether MuseScore re-reads the tempo from the text when it is edited");
        c.output["voiceAssignment"] = arg(DataType::String, "kind expression: all_voices or this_voice");
        c.output["messages"] = listOf(DataType::String, "Things worth knowing about the result");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return tx::addText(p, ctx.score, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/add_chord_symbol";
        c.name = "add_chord_symbol";
        c.title = "Add a chord symbol";
        c.description
            = "Adds a chord symbol on one staff at any position in a bar (a note is not needed: a change in the middle of a "
              "held note or on an empty beat is fine), as typing one and moving with the chord-symbol navigation keys "
              "does. text is the symbol as it should read in the score as it is now displayed (concert or written pitch, "
              "whichever the score shows), e.g. Cmaj7, F#m7b5, Bb/D, N.C.; '|' separates polychord parts; the flat and "
              "sharp signs are turned into b and #. notation: standard (default), roman (V7) or nashville (4). Linked "
              "parts get a copy, transposed where the part's concert pitch setting differs. If that position, staff and "
              "voice already has a chord symbol of the same notation, if_exists error (default) refuses with E_EXISTS "
              "and replace swaps it (status unchanged if it already reads the same). A symbol MuseScore cannot parse "
              "is still added, as written, with recognised false. Refuses: E_INVALID_ARG for empty text or bad arguments, E_RANGE "
              "for a staff, bar, beat or voice outside the score, E_EXISTS for an existing chord symbol (with if_exists "
              "error) and for one that belongs to a fret diagram (never replaced). In the notation chord symbols are on the staff's h "
              "line: m3 h | {G7}:h {C}:h. Returns the handle, status (added, replaced, unchanged), name and the bar's notation.";
        c.kind = CommandKind::Write;
        c.undoName = "Add chord symbol";
        c.input["staff"] = arg(DataType::Undefined, STAFF_DESC);
        c.input["at"] = arg(DataType::Object, std::string("Where: ") + POSITION_DESC + "; any position inside the bar");
        c.input["voice"] = voiceArg();
        c.input["target"] = arg(DataType::String, TARGET_DESC);
        c.input["text"] = arg(DataType::String, "The chord symbol, e.g. \"Cmaj7\", \"Dm7b5\", \"Bb/D\", \"N.C.\"", true);
        c.input["notation"] = enumArg("standard (default), roman or nashville", tx::harmonyNotations());
        c.input["if_exists"] = enumArg("When there is already one there: error (default) or replace", { "error", "replace" });
        c.output["handle"] = arg(DataType::String, "Handle of the chord symbol");
        c.output["status"] = arg(DataType::String, "added, replaced or unchanged");
        c.output["name"] = arg(DataType::String, "The chord symbol as MuseScore spells it");
        c.output["recognised"] = arg(DataType::Boolean, "false when MuseScore could not parse it (shown as written, not played)");
        c.output["messages"] = listOf(DataType::String, "Things worth knowing about the result");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return tx::addChordSymbol(p, ctx.score, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/add_lyric";
        c.name = "add_lyric";
        c.title = "Add lyrics";
        c.description
            = "Adds lyric syllables to one voice of a staff, one verse at a time, as entering lyrics in MuseScore does. "
              "The first syllable goes on the note or rest at at (or target); each next one on the next chord of that "
              "voice after the previous syllable, or after the last note of its extender (rests are skipped unless they "
              "already carry a lyric of that verse). Each syllable: text (plain, without hyphens or underscores), "
              "syllabic single (default), begin, middle or end (begin/middle draw a hyphen to the next syllable), and "
              "melisma_to: {measure, beat} of the last note sung to it (an extender line), or \"self\" for an extender on "
              "its own note. Within the list a syllable after begin/middle must be middle/end and vice versa; a syllable "
              "with an extender must be single or end. The lyric just before and just after are repaired the way the "
              "space and hyphen keys repair them (listed in adjusted). verse is 1-based (default 1); placement "
              "above/below (default: the style). A slot that already has a lyric of that verse: if_exists error "
              "(default, E_EXISTS) or replace. Refuses an extender that starts or ends on a rest, before its note, or over "
              "another syllable (E_INVALID_ARG), inconsistent syllabic values, text ending in - or _, a measure repeat "
              "(E_INVALID_ARG), a sequence that runs past the last note or a staff/bar/beat/voice outside the score "
              "(E_RANGE), and a note whose linked part has no note or rest at that tick (E_REFUSED; MuseScore would "
              "crash adding it). No partial lyric lines over "
              "repeats are made. In the notation: +ly\"Sing-\" (hyphen), +ly\"song_\" (extender), +ly2\"...\" (verse 2). "
              "Returns the handles, where each syllable went, and the notation of the bars.";
        c.kind = CommandKind::Write;
        c.undoName = "Add lyrics";
        c.input["staff"] = arg(DataType::Undefined, STAFF_DESC);
        c.input["at"] = arg(DataType::Object, std::string("The note (or rest) of the first syllable: ") + POSITION_DESC);
        c.input["voice"] = voiceArg();
        c.input["target"] = arg(DataType::String, TARGET_DESC);
        c.input["verse"] = arg(DataType::Integer, "Verse number, 1-based (default 1)");
        c.input["verse"].min = Val(1);
        c.input["verse"].max = Val(99);
        c.input["placement"] = enumArg("above or below the staff (default: the score's lyrics style)", { "above", "below" });
        c.input["syllables"] = listOf(DataType::Object, "The syllables in order: {\"text\": \"Glo\", \"syllabic\": "
                                                        "\"single\"|\"begin\"|\"middle\"|\"end\", \"melisma_to\": {\"measure\": N, "
                                                        "\"beat\": B} or \"self\"}", true);
        c.input["if_exists"] = enumArg("When a note already has a lyric of that verse: error (default) or replace", { "error", "replace" });
        c.output["handles"] = listOf(DataType::String, "Handles of the lyrics, in order");
        c.output["placed"] = listOf(DataType::String, "Where each syllable went");
        c.output["statuses"] = listOf(DataType::String, "added or replaced, per syllable");
        c.output["verse"] = arg(DataType::Integer, "The verse");
        c.output["adjusted"] = listOf(DataType::String, "Neighbouring lyrics whose hyphen or extender was repaired");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return tx::addLyric(p, ctx.score, ctx.out); };
        registry.add(c);
    }
}
