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
#include "expressionlines.h"

#include "registry/commandregistry.h"

using namespace mu::agent;
using muse::Val;

namespace {
const char* EVENT_ADDRESS
    = "An event (chord, note or rest), as an object: either {\"handle\": \"<handle from score_read detail or an earlier "
      "command>\"} or a position {\"measure\": <bar number as score_read prints it, m<N>, pickup = 1>, \"beat\": <1-based, in "
      "quarter notes from the start of the bar, e.g. 1, 2.5; default 1>, or instead \"tick\": <ticks from the start of the "
      "bar, 480 per quarter, for tuplet positions>, \"staff\": <staff number as in score_read's s<N>, or {\"part\": \"<part "
      "name>\", \"index\": <1-based staff within the part, default 1>}>, \"voice\": <1-4, default 1>, \"grace\": <optional "
      "1-based index of a grace chord of that chord, in score order>}. A chord or rest must start exactly there.";
}

void mu::agent::registerExpressionLinesCommands(CommandRegistry& registry)
{
    {
        CommandInfo c;
        c.uri = "command://notation/add_slur";
        c.name = "add_slur";
        c.title = "Add a slur";
        c.description
            = "Adds a slur from one chord to another, like selecting the two notes and pressing S. The ends may be in "
              "different bars or systems, different voices, or different staves of the same part (piano RH to LH). "
              "Refused: a rest at either end (E_INVALID_ARG), both ends on the same chord (E_INVALID_ARG), ends in "
              "different parts (E_INVALID_ARG), end before start (E_RANGE), no chord starting at an address (E_RANGE, "
              "the message lists the beats that have events), a slur with the same ends already there (E_EXISTS). "
              "Linked parts get the slur too. Returns the slur's handle and the touched bars in the compact notation, "
              "where the slur shows as ( ... ). Grace-note slurs are added but not shown in the notation.";
        c.kind = CommandKind::Write;
        c.undoName = "Add slur";
        c.input["start"] = { DataType::Object, std::string("First chord of the slur. ") + EVENT_ADDRESS, true };
        c.input["end"] = { DataType::Object, std::string("Last chord of the slur, after start. ") + EVENT_ADDRESS, true };
        c.input["direction"] = { DataType::String, "Slur direction (default auto)" };
        c.input["direction"].enumValues = { "auto", "up", "down" };
        c.input["style"] = { DataType::String, "Line style (default solid)" };
        c.input["style"].enumValues = { "solid", "dashed", "dotted", "wideDashed" };
        c.output["handle"] = { DataType::String, "Handle of the new slur" };
        c.output["startBar"] = { DataType::Integer, "Bar the slur starts in" };
        c.output["endBar"] = { DataType::Integer, "Bar the slur ends in" };
        c.output["bars"] = { DataType::String, "The touched bars on the slur's staves, in the compact notation (as score_read)" };
        c.write = [](const Params& p, WriteContext& ctx) { return lines::addSlur(ctx.score, p, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/add_line";
        c.name = "add_line";
        c.title = "Add a line (pedal, ottava, trill, volta, text line)";
        c.description
            = "Adds a line over a passage, as applying it from the Lines palette to a range does. kind: pedal, ottava "
              "(8va, 8vb, 15ma, 15mb, 22ma, 22mb), trill (trill line or its prall variants), volta (1st/2nd ending "
              "bracket over whole bars), textLine (a line with text, e.g. \"rit.\"; system: true puts it once above "
              "the system and in every part). start and end are the first and last events covered: the line runs to "
              "the end of the end event. Pedal, ottava, trill and staff text lines go on the start event's staff in "
              "voice 1 (as the palette puts them; the addresses only give the times); start and end must be on the "
              "same staff. A volta takes {\"measure\": n} for start and end and only draws the bracket: add the "
              "end-repeat barline separately. An ottava changes the sounding pitch of the notes under it, which "
              "score_read then shows. Overlapping lines are allowed, as in the UI. Refused: end before start "
              "(E_RANGE), no event at an address (E_RANGE), ends on different staves, a grace note as an end, or an "
              "option that does not apply to the kind (E_INVALID_ARG). Returns the line's handle and the touched bars "
              "in the compact notation (marks +8va[ ... +8va], +ped[ ... +ped], +trl[ ... +trl], "
              "+txt[\"rit.\" ... +txt]; a volta as bar attributes volta=1 ... volta=end).";
        c.kind = CommandKind::Write;
        c.undoName = "Add line";
        c.input["kind"] = { DataType::String, "Which line", true };
        c.input["kind"].enumValues = { "pedal", "ottava", "trill", "volta", "textLine" };
        c.input["start"] = { DataType::Object, std::string("First event covered. For a volta: {\"measure\": n}. ") + EVENT_ADDRESS, true };
        c.input["end"] = { DataType::Object, std::string("Last event covered (inclusive). For a volta: {\"measure\": n}, the last bar "
                                                         "of the ending. ") + EVENT_ADDRESS, true };
        c.input["placement"] = { DataType::String, "Above or below the staff (default: the style's placement for the kind)" };
        c.input["placement"].enumValues = { "default", "above", "below" };
        c.input["ottavaType"] = { DataType::String, "kind ottava only (default 8va)" };
        c.input["ottavaType"].enumValues = { "8va", "8vb", "15ma", "15mb", "22ma", "22mb" };
        c.input["trillType"] = { DataType::String, "kind trill only (default trill)" };
        c.input["trillType"].enumValues = { "trill", "upprall", "downprall", "prallprall" };
        c.input["pedalStyle"] = { DataType::String, "kind pedal only, as in the palette: line = Ped. and line with an end "
                                                    "hook (default), asterisk = Ped. and *, straight = straight hooks, "
                                                    "angledEnd / angledBoth / angledStart = angled hooks" };
        c.input["pedalStyle"].enumValues = { "line", "asterisk", "straight", "angledEnd", "angledBoth", "angledStart" };
        c.input["endings"] = { DataType::Array, "kind volta only: the passes that play this ending, e.g. [1] or [1, 2] "
                                                "(default [1])" };
        c.input["endings"].itemsType = DataType::Integer;
        c.input["voltaType"] = { DataType::String, "kind volta only: closed (hook at the end, default) or open" };
        c.input["voltaType"].enumValues = { "closed", "open" };
        c.input["text"] = { DataType::String, "kind volta: the label (default from endings, e.g. \"1.\" or \"1., 2.\"); kind "
                                              "textLine: the text at the start of the line (plain text)" };
        c.input["continueText"] = { DataType::String, "kind textLine only: text repeated at the start of each later system" };
        c.input["endText"] = { DataType::String, "kind textLine only: text at the end of the line" };
        c.input["endHook"] = { DataType::String, "kind textLine only: the line's end (default hook90, as the palette's text line)" };
        c.input["endHook"].enumValues = { "none", "hook90", "hook45", "hook90t", "arrow", "arrowFilled" };
        c.input["system"] = { DataType::Boolean, "kind textLine only: a system text line (top staff, every part) instead of "
                                                 "a staff text line (default false)" };
        c.output["handle"] = { DataType::String, "Handle of the new line" };
        c.output["kind"] = { DataType::String, "The kind added" };
        c.output["startBar"] = { DataType::Integer, "Bar the line starts in" };
        c.output["endBar"] = { DataType::Integer, "Bar the line ends in" };
        c.output["bars"] = { DataType::String, "The touched bars on the line's staff, in the compact notation (as score_read)" };
        c.write = [](const Params& p, WriteContext& ctx) { return lines::addLine(ctx.score, p, ctx.out); };
        registry.add(c);
    }
}
