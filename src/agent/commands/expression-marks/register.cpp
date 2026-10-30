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
#include "expressionmarkscommands.h"

#include "expressionmarks.h"

#include "registry/commandregistry.h"

using namespace mu::agent;
using muse::Val;
namespace em = mu::agent::expression_marks;

namespace {
const char* STAFF_DESC
    = "The staff: its number in the score (1-based, as # sN in score_read), {\"number\": N}, or "
      "{\"part\": \"<part name as in score_overview>\", \"index\": <staff in the part, 1-based, default 1>}";
const char* POSITION_DESC
    = "{\"measure\": <bar by position, as m<N> in score_read; a pickup is 1>, \"beat\": <1-based, in quarter notes "
      "whatever the time signature: 1.5 is an eighth after the downbeat>} or {\"measure\", \"offset\": \"<n>/<d>\" "
      "(whole notes from the start of the bar, for triplet positions)}. A note or rest must start exactly there";
const char* TARGET_DESC
    = "Instead of staff + position (+ voice): a Note, Chord or Rest handle from score_read detail (a note means its chord)";
const char* VOICE_DESC = "Voice 1-4 (default 1)";
const char* VA_DESC
    = "Which voices it applies to: all_voices (default; MuseScore's factory setting), this_voice, or preference "
      "(follow this MuseScore's 'dynamics apply to all voices' setting, as the palette does)";
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
    Arg a = arg(DataType::Integer, VOICE_DESC);
    a.min = Val(1);
    a.max = Val(4);
    return a;
}

Arg enumArg(const std::string& d, const std::vector<std::string>& values, bool required = false)
{
    Arg a = arg(DataType::String, d, required);
    a.enumValues = values;
    return a;
}

Arg listOf(DataType item, const std::string& d)
{
    Arg a = arg(DataType::Array, d);
    a.itemsType = item;
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
}

void mu::agent::registerExpressionMarksCommands(CommandRegistry& registry)
{
    {
        CommandInfo c;
        c.uri = "command://notation/add_dynamic";
        c.name = "add_dynamic";
        c.title = "Add a dynamic";
        c.description
            = "Adds a dynamic (p, mf, sfz...) to the note, chord or rest that starts at a position on one staff, as the "
              "Dynamics palette does. Address it with staff + at (+ voice), or with target (a handle). Give dynamic (a "
              "name) or text (e.g. \"subito p\": plain text that must contain a dynamic; words alone such as \"dolce\" "
              "are refused, use add_text kind expression). If that slot already has a dynamic, if_exists replace "
              "(default) changes its type and text in place (keeping its voice assignment: voice_assignment is "
              "ignored then), error refuses with E_EXISTS. The slot is the voice the dynamic lands in: voice 1 for "
              "all_voices (the default), the given voice for this_voice. A grace note means its main "
              "note. Refuses (E_RANGE) when no note or rest starts exactly at the position. Returns the dynamic's "
              "handle, status (added, replaced or unchanged) and the bar's notation.";
        c.kind = CommandKind::Write;
        c.undoName = "Add dynamic";
        c.input["staff"] = arg(DataType::Undefined, STAFF_DESC);
        c.input["at"] = arg(DataType::Object, std::string("Where: ") + POSITION_DESC);
        c.input["voice"] = voiceArg();
        c.input["target"] = arg(DataType::String, TARGET_DESC);
        c.input["dynamic"] = enumArg("The dynamic, by name", em::dynamicNames());
        c.input["text"] = arg(DataType::String, "Custom text containing a dynamic, e.g. \"subito p\" or \"p dolce\" (plain text)");
        c.input["velocity"] = arg(DataType::Integer, "MIDI velocity for playback (default: the dynamic's own)");
        c.input["velocity"].min = Val(1);
        c.input["velocity"].max = Val(127);
        c.input["voice_assignment"] = enumArg(VA_DESC, { "preference", "all_voices", "this_voice" });
        c.input["if_exists"] = enumArg("When that slot already has a dynamic: replace (default) or error", { "replace", "error" });
        c.output["handle"] = arg(DataType::String, "Handle of the dynamic");
        c.output["status"] = arg(DataType::String, "added, replaced or unchanged");
        c.output["dynamic"] = arg(DataType::String, "The dynamic's type name");
        c.output["voiceAssignment"] = arg(DataType::String, "all_voices or this_voice");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return em::addDynamic(p, ctx.score, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/add_hairpin";
        c.name = "add_hairpin";
        c.title = "Add a hairpin";
        c.description
            = "Adds a crescendo or diminuendo hairpin (or a cresc./dim. text line) on one staff and voice, from the note "
              "at from to the end of the note at to (inclusive), as the add-hairpin action does with those notes "
              "selected. If a dynamic sits inside the last note's duration, the hairpin stops there. Refuses when to is "
              "before from (E_INVALID_ARG), when no note or rest starts at either position (E_RANGE), and when a "
              "hairpin already covers part of the span on that staff and voice (E_EXISTS). In the notation the hairpin "
              "shows as < or > on its first note and ! on the note where it ends. Returns its handle, where it starts "
              "and ends, and the notation of the bars it spans.";
        c.kind = CommandKind::Write;
        c.undoName = "Add hairpin";
        c.input["kind"] = enumArg("cresc, dim (hairpins), cresc_line, dim_line (\"cresc.\"/\"dim.\" text lines)", em::hairpinKinds(), true);
        c.input["staff"] = arg(DataType::Undefined, STAFF_DESC, true);
        c.input["voice"] = voiceArg();
        c.input["from"] = arg(DataType::Object, std::string("The first note under the hairpin: ") + POSITION_DESC, true);
        c.input["to"] = arg(DataType::Object, std::string("The last note under the hairpin (inclusive): ") + POSITION_DESC, true);
        c.input["voice_assignment"] = enumArg(VA_DESC, { "preference", "all_voices", "this_voice" });
        c.output["handle"] = arg(DataType::String, "Handle of the hairpin");
        c.output["start"] = arg(DataType::String, "Where it starts");
        c.output["end"] = arg(DataType::String, "Where it ends (bar and beat of its end point)");
        c.output["voiceAssignment"] = arg(DataType::String, "all_voices or this_voice");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return em::addHairpin(p, ctx.score, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/add_articulation";
        c.name = "add_articulation";
        c.title = "Add an articulation";
        c.description
            = "Adds an articulation (staccato, accent, tenuto, up_bow...; the notation codes st, ac, ten, ... work too) to "
              "a chord, as the Articulations palette does: it adds and never removes, and a chord that already has that "
              "articulation is left alone (counted as unchanged). With through, every chord from at through that "
              "position on the staff and voice gets one (rests are skipped). A combined mark such as accent_staccato is "
              "stored as one symbol on a chord with no marks, and as its parts on a chord that has some. Refuses a rest "
              "(E_INVALID_ARG) and a chord with a tapping mark, which MuseScore would remove (E_REFUSED). Returns handles, counts "
              "and the notation of the bars.";
        c.kind = CommandKind::Write;
        c.undoName = "Add articulation";
        c.input["articulation"] = enumArg("Which articulation", em::articulationNames(), true);
        c.input["staff"] = arg(DataType::Undefined, STAFF_DESC);
        c.input["at"] = arg(DataType::Object, std::string("The chord: ") + POSITION_DESC);
        c.input["through"] = arg(DataType::Object, "Optional end of a span (inclusive, same form as at)");
        c.input["voice"] = voiceArg();
        c.input["target"] = arg(DataType::String, TARGET_DESC);
        c.output["handles"] = listOf(DataType::String, "Handles of the articulations on the chords");
        c.output["added"] = arg(DataType::Integer, "Chords that got it");
        c.output["unchanged"] = arg(DataType::Integer, "Chords that already had it");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return em::addArticulation(p, ctx.score, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/add_ornament";
        c.name = "add_ornament";
        c.title = "Add an ornament";
        c.description
            = "Adds an ornament symbol (trill, mordent, turn, ...; the notation codes tr, mord, imord, turn, iturn work "
              "too) to a chord, as the Ornaments palette does: it adds and never removes; a chord that already has it is "
              "left alone. trill is the tr symbol on one note, not a trill line (that is add_line). With through, every "
              "chord from at through that position gets one. Refuses a rest (E_INVALID_ARG) and a chord with a tapping "
              "mark (E_REFUSED). Returns handles, counts and the notation of the bars.";
        c.kind = CommandKind::Write;
        c.undoName = "Add ornament";
        c.input["ornament"] = enumArg("Which ornament", em::ornamentNames(), true);
        c.input["staff"] = arg(DataType::Undefined, STAFF_DESC);
        c.input["at"] = arg(DataType::Object, std::string("The chord: ") + POSITION_DESC);
        c.input["through"] = arg(DataType::Object, "Optional end of a span (inclusive, same form as at)");
        c.input["voice"] = voiceArg();
        c.input["target"] = arg(DataType::String, TARGET_DESC);
        c.output["handles"] = listOf(DataType::String, "Handles of the ornaments on the chords");
        c.output["added"] = arg(DataType::Integer, "Chords that got it");
        c.output["unchanged"] = arg(DataType::Integer, "Chords that already had it");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return em::addOrnament(p, ctx.score, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/add_fermata";
        c.name = "add_fermata";
        c.title = "Add a fermata";
        c.description
            = "Adds a fermata, as the Breaths & pauses palette does, to the note or rest at a position (staff + at, or "
              "target), or to a bar's end barline (barline: bar number). all_staves: true puts one on every staff at "
              "that position (staves where no note or rest starts there are skipped and listed). A slot with the same "
              "fermata is left alone (time_stretch is updated if given); a different fermata there is replaced. "
              "Barline fermatas do not lengthen playback. Default stretch: normal 2, short 1.5, long 3, very_long 4, "
              "very_short 1.25. Returns handles, status and the bar's notation.";
        c.kind = CommandKind::Write;
        c.undoName = "Add fermata";
        c.input["fermata"] = enumArg("Which fermata (default normal)", em::fermataNames());
        c.input["staff"] = arg(DataType::Undefined, STAFF_DESC);
        c.input["at"] = arg(DataType::Object, std::string("The note or rest: ") + POSITION_DESC);
        c.input["voice"] = voiceArg();
        c.input["target"] = arg(DataType::String, TARGET_DESC);
        c.input["barline"] = arg(DataType::Integer, "Instead of a note: the bar (by position) whose end barline gets the fermata");
        c.input["barline"].min = Val(1);
        c.input["all_staves"] = arg(DataType::Boolean, "One fermata on every staff (with at or barline; leave out staff)");
        c.input["time_stretch"] = arg(DataType::Float, "How much longer it plays, as a factor (> 0)");
        c.output["handles"] = listOf(DataType::String, "Handles of the fermatas");
        c.output["status"] = arg(DataType::String, "added, replaced, changed (stretch only, or mixed) or unchanged");
        c.output["skippedStaves"] = listOf(DataType::Integer, "With all_staves: staves where nothing starts at that position");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return em::addFermata(p, ctx.score, ctx.out); };
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/add_breath";
        c.name = "add_breath";
        c.title = "Add a breath mark or caesura";
        c.description
            = "Adds a breath mark or caesura after a note or rest, as the Breaths & pauses palette does: it sits at the "
              "end of that note (in the notation it shows as +br or +cae on the note). Address the note with staff + "
              "after (+ voice), or target. Caesuras pause playback by 2 seconds by default; breath marks by 0. If there "
              "is already one after that note it is changed to this one (never a second). Refuses grace notes. "
              "Returns its handle, status (added, replaced or unchanged) and the bar's notation. Breath types: "
              + joined(em::breathNames()) + ".";
        c.kind = CommandKind::Write;
        c.undoName = "Add breath mark";
        c.input["breath"] = enumArg("Which mark (default comma)", em::breathNames());
        c.input["staff"] = arg(DataType::Undefined, STAFF_DESC);
        c.input["after"] = arg(DataType::Object, std::string("The note or rest it follows: ") + POSITION_DESC);
        c.input["voice"] = voiceArg();
        c.input["target"] = arg(DataType::String, TARGET_DESC);
        c.input["pause"] = arg(DataType::Float, "Playback pause in seconds (default 0 for breath marks, 2 for caesuras)");
        c.input["pause"].min = Val(0.0);
        c.output["handle"] = arg(DataType::String, "Handle of the mark");
        c.output["status"] = arg(DataType::String, "added, replaced or unchanged");
        c.output["notation"] = arg(DataType::String, NOTATION_DESC);
        c.write = [](const Params& p, WriteContext& ctx) { return em::addBreath(p, ctx.score, ctx.out); };
        registry.add(c);
    }
}
