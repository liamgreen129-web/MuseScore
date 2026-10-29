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
// read group (spec: docs/agent/specs/read.md): score_overview, score_read, score_validate,
// score_changes. All read-only: no selection, no view change, no layout calls.

#include "readcommands.h"

#include <algorithm>

#include "agentenv.h"
#include "registry/commandregistry.h"
#include "registry/errors.h"

#include "engraving/dom/chordrest.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rehearsalmark.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/timesig.h"
#include "notation/imasternotation.h"
#include "project/inotationproject.h"

#include "notation/validate.h"
#include "notation/writer.h"
#include "read/scoreprinter.h"

using namespace muse;
using namespace mu::agent;
using mu::engraving::Fraction;
using mu::engraving::Measure;
using mu::engraving::MasterScore;
using mu::engraving::Segment;
using mu::engraving::SegmentType;
namespace nt = mu::agent::notation;

namespace {
const size_t MAX_READ_CHARS = 40000; // ~10k tokens: where Claude Code starts warning about large tool output

Val str(const std::string& s) { return Val(s); }

MasterScore* currentScore(AgentEnv& env)
{
    if (!env.globalContext()) {
        return nullptr;
    }
    // through the project, as hello does: currentMasterNotation() can be empty while the
    // project is current (seen when the score is opened at launch)
    const mu::project::INotationProjectPtr project = env.globalContext()->currentProject();
    const mu::notation::IMasterNotationPtr master = project ? project->masterNotation() : nullptr;
    return master ? master->masterScore() : nullptr;
}

std::vector<const Measure*> allMeasures(const MasterScore* score)
{
    std::vector<const Measure*> out;
    for (const Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        out.push_back(m);
    }
    return out;
}

int barOfTick(const std::vector<const Measure*>& measures, int tick)
{
    for (size_t i = 0; i < measures.size(); ++i) {
        const Measure* m = measures[i];
        if (tick >= m->tick().ticks() && tick < (m->tick() + m->ticks()).ticks()) {
            return static_cast<int>(i) + 1;
        }
    }
    return tick < 0 ? 0 : static_cast<int>(measures.size());
}

//! "1-4,7,9-12" from sorted bar numbers
std::string ranges(const std::vector<int>& bars)
{
    std::string out;
    for (size_t i = 0; i < bars.size();) {
        size_t j = i;
        while (j + 1 < bars.size() && bars[j + 1] == bars[j] + 1) {
            ++j;
        }
        if (!out.empty()) {
            out += ",";
        }
        out += std::to_string(bars[i]) + (j > i ? "-" + std::to_string(bars[j]) : "");
        i = j + 1;
    }
    return out;
}

std::vector<int> intList(const Params& p, const char* key)
{
    std::vector<int> out;
    auto it = p.find(key);
    if (it != p.end() && !it->second.isNull()) {
        for (const Val& v : it->second.toList()) {
            out.push_back(static_cast<int>(v.toDouble()));
        }
    }
    return out;
}

int intArg(const Params& p, const char* key, int def)
{
    auto it = p.find(key);
    return (it == p.end() || it->second.isNull()) ? def : static_cast<int>(it->second.toDouble());
}

//! Resolves from/to against the score; returns an error Ret when out of range.
Ret resolveRange(const Params& p, int nbars, int& from, int& to)
{
    from = intArg(p, "from", 1);
    to = intArg(p, "to", nbars);
    if (nbars == 0) {
        return makeErr(Err::Range, "the score has no bars");
    }
    if (from < 1 || to > nbars || from > to) {
        return makeErr(Err::Range, "bars " + std::to_string(from) + "-" + std::to_string(to) + " are outside 1-"
                       + std::to_string(nbars));
    }
    return make_ok();
}

// ------------------------------------------------------------------ score_overview

Result scoreOverview(const Params&, AgentEnv& env)
{
    Result r;
    MasterScore* score = currentScore(env);
    if (!score) {
        return { makeErr(Err::NoScore, "no score is open in this instance"), {} };
    }
    const std::vector<const Measure*> measures = allMeasures(score);

    ValMap info;
    info["title"] = str(score->metaTag(u"workTitle").toStdString());
    info["bars"] = Val(static_cast<int>(measures.size()));
    info["staves"] = Val(static_cast<int>(score->nstaves()));
    info["stateVersion"] = Val(static_cast<int64_t>(env.changes.version));
    if (const auto project = env.globalContext()->currentProject()) {
        info["path"] = str(project->path().toStdString());
        info["needSave"] = Val(project->needSave().val);
    }
    r.values["score"] = Val(info);

    ValList parts;
    for (const mu::engraving::Part* part : score->parts()) {
        ValMap pm;
        pm["name"] = str(part->partName().toStdString());
        const mu::engraving::Instrument* inst = part->instrument();
        pm["instrument"] = str(inst ? inst->id().toStdString() : "");
        ValList staves;
        for (const mu::engraving::Staff* st : part->staves()) {
            ValMap sm;
            const size_t idx = st->idx();
            sm["staff"] = Val(static_cast<int>(idx) + 1);
            std::vector<int> used;
            for (size_t i = 0; i < measures.size(); ++i) {
                if (!measures[i]->isEmpty(idx)) {
                    used.push_back(static_cast<int>(i) + 1);
                }
            }
            sm["bars_with_music"] = str(ranges(used));
            staves.emplace_back(Val(sm));
        }
        pm["staves"] = Val(staves);
        parts.emplace_back(Val(pm));
    }
    r.values["parts"] = Val(parts);

    // maps and marks, by bar (from the printed header/attributes of staff 1, so the
    // names match score_read exactly)
    read::ReadOptions opt;
    opt.staves = { 1 };
    opt.voices = { 1 };
    const nt::Document doc = read::printScore(score, opt);
    ValList changes;
    for (const nt::Line& line : doc.lines) {
        if (const nt::StaffHeader* h = std::get_if<nt::StaffHeader>(&line)) {
            ValMap c;
            c["bar"] = Val(1);
            for (const nt::KeyValue& kv : h->attrs) {
                if (kv.key == "key" || kv.key == "time") {
                    c[kv.key] = str(kv.value);
                }
            }
            changes.emplace_back(Val(c));
        } else if (const nt::BarAttrs* a = std::get_if<nt::BarAttrs>(&line)) {
            ValMap c;
            for (const nt::KeyValue& kv : a->attrs) {
                if (kv.key != "len" && kv.key != "clef") {
                    c[kv.key] = str(kv.value);
                }
            }
            if (!c.empty()) {
                c["bar"] = Val(a->bar.index);
                changes.emplace_back(Val(c));
            }
        }
    }
    r.values["bar_attributes"] = Val(changes);

    ValList tempos;
    for (size_t i = 0; i < measures.size(); ++i) {
        for (const Segment* s = measures[i]->first(); s; s = s->next()) {
            for (const mu::engraving::EngravingItem* a : s->annotations()) {
                if (a->isTempoText()) {
                    const mu::engraving::TempoText* t = mu::engraving::toTempoText(a);
                    ValMap tm;
                    tm["bar"] = Val(static_cast<int>(i) + 1);
                    tm["text"] = str(t->plainText().toStdString());
                    tm["bpm"] = Val(static_cast<int>(t->tempo().val * 60.0 + 0.5));
                    tempos.emplace_back(Val(tm));
                }
            }
        }
    }
    r.values["tempo"] = Val(tempos);
    return r;
}

// ------------------------------------------------------------------ score_read

Result scoreRead(const Params& p, AgentEnv& env)
{
    MasterScore* score = currentScore(env);
    if (!score) {
        return { makeErr(Err::NoScore, "no score is open in this instance"), {} };
    }
    const int nbars = static_cast<int>(allMeasures(score).size());
    int from = 0;
    int to = 0;
    if (Ret ret = resolveRange(p, nbars, from, to); !ret) {
        return { ret, {} };
    }

    read::ReadOptions opt;
    opt.fromBar = from;
    opt.toBar = to;
    opt.staves = intList(p, "staves");
    opt.voices = intList(p, "voices");
    for (int s : opt.staves) {
        if (s < 1 || s > static_cast<int>(score->nstaves())) {
            return { makeErr(Err::Range, "staff " + std::to_string(s) + " is outside 1-" + std::to_string(score->nstaves())), {} };
        }
    }
    auto pit = p.find("pitch");
    opt.writtenPitch = pit != p.end() && !pit->second.isNull() && pit->second.toString() == "written";

    const std::string text = nt::write(read::printScore(score, opt));
    if (text.size() > MAX_READ_CHARS) {
        const int bars = to - from + 1;
        const int fits = std::max(1, static_cast<int>(bars * (static_cast<double>(MAX_READ_CHARS) / text.size()) * 0.9));
        return { makeErr(Err::Range, "bars " + std::to_string(from) + "-" + std::to_string(to) + " are too long to return at once ("
                         + std::to_string(text.size()) + " characters); read about " + std::to_string(fits)
                         + " bars at a time, or fewer staves"), {} };
    }

    Result r;
    r.values["text"] = str(text);
    r.values["from"] = Val(from);
    r.values["to"] = Val(to);
    r.values["stateVersion"] = Val(static_cast<int64_t>(env.changes.version));
    return r;
}

// ------------------------------------------------------------------ score_validate

Result scoreValidate(const Params& p, AgentEnv& env)
{
    MasterScore* score = currentScore(env);
    if (!score) {
        return { makeErr(Err::NoScore, "no score is open in this instance"), {} };
    }
    const std::vector<const Measure*> measures = allMeasures(score);
    int from = 0;
    int to = 0;
    if (Ret ret = resolveRange(p, static_cast<int>(measures.size()), from, to); !ret) {
        return { ret, {} };
    }

    ValList problems;
    // voice 1 must fill every bar in the model itself (the printer turns holes into rests,
    // so check the model directly; sanityCheck would repair, which a read must not do)
    for (int idx = from; idx <= to; ++idx) {
        const Measure* m = measures[idx - 1];
        for (size_t staff = 0; staff < score->nstaves(); ++staff) {
            Fraction sum(0, 1);
            for (const Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
                if (const mu::engraving::EngravingItem* el = s->element(staff * mu::engraving::VOICES)) {
                    sum += mu::engraving::toChordRest(el)->actualTicks();
                }
            }
            if (sum != m->ticks()) {
                ValMap pm;
                pm["code"] = str("E_BAR_SUM");
                pm["message"] = str("m" + std::to_string(idx) + " v1 (s" + std::to_string(staff + 1) + ") holds "
                                    + sum.toString().toStdString() + " in a bar of " + m->ticks().toString().toStdString());
                problems.emplace_back(Val(pm));
            }
        }
    }

    read::ReadOptions opt;
    opt.fromBar = from;
    opt.toBar = to;
    const nt::Document doc = read::printScore(score, opt);
    for (const nt::Problem& pr : nt::validate(doc, nt::barLengthsFromDocument(doc))) {
        ValMap pm;
        pm["code"] = str(pr.code);
        pm["message"] = str(pr.message);
        problems.emplace_back(Val(pm));
    }
    ValList details;
    for (const nt::Line& line : doc.lines) {
        if (const nt::Comment* c = std::get_if<nt::Comment>(&line)) {
            details.emplace_back(str(c->text.substr(c->text.find_first_not_of(' '))));
        }
    }

    Result r;
    r.values["ok"] = Val(problems.empty());
    r.values["problems"] = Val(problems);
    r.values["details"] = Val(details);
    r.values["from"] = Val(from);
    r.values["to"] = Val(to);
    return r;
}

// ------------------------------------------------------------------ score_changes

Result scoreChanges(const Params& p, AgentEnv& env)
{
    const long long since = intArg(p, "since", 0);
    MasterScore* score = currentScore(env);
    const std::vector<const Measure*> measures = score ? allMeasures(score) : std::vector<const Measure*> {};

    Result r;
    r.values["stateVersion"] = Val(static_cast<int64_t>(env.changes.version));
    const bool complete = env.changes.records.empty() || env.changes.records.front().version <= since + 1;
    r.values["complete"] = Val(complete);

    std::vector<int> bars;
    std::vector<int> staves;
    ValList list;
    for (const ChangeRecord& c : env.changes.records) {
        if (c.version <= since) {
            continue;
        }
        ValMap cm;
        cm["version"] = Val(static_cast<int64_t>(c.version));
        if (c.tickFrom >= 0 && !measures.empty()) {
            const int b0 = barOfTick(measures, c.tickFrom);
            const int b1 = barOfTick(measures, std::max(c.tickFrom, c.tickTo - 1));
            cm["bars"] = str(std::to_string(b0) + (b1 > b0 ? "-" + std::to_string(b1) : ""));
            for (int b = b0; b <= b1; ++b) {
                bars.push_back(b);
            }
        } else {
            cm["bars"] = str("all");
        }
        if (c.staffFrom >= 0) {
            cm["staves"] = str(std::to_string(c.staffFrom + 1) + (c.staffTo > c.staffFrom ? "-" + std::to_string(c.staffTo + 1) : ""));
            for (int s = c.staffFrom; s <= std::max(c.staffFrom, c.staffTo); ++s) {
                staves.push_back(s + 1);
            }
        }
        list.emplace_back(Val(cm));
    }
    std::sort(bars.begin(), bars.end());
    bars.erase(std::unique(bars.begin(), bars.end()), bars.end());
    std::sort(staves.begin(), staves.end());
    staves.erase(std::unique(staves.begin(), staves.end()), staves.end());
    r.values["changes"] = Val(list);
    r.values["bars"] = str(ranges(bars));
    r.values["staves"] = str(ranges(staves));
    return r;
}

Arg rangeArg(const char* desc)
{
    Arg a { DataType::Integer, desc };
    a.min = Val(1);
    return a;
}
}

void mu::agent::registerReadCommands(CommandRegistry& registry)
{
    {
        CommandInfo c;
        c.uri = "command://notation/score_overview";
        c.name = "score_overview";
        c.title = "Score overview";
        c.description = "A map of the open score in a few hundred tokens: title, bar count, path, needSave, stateVersion; "
                        "parts with their staves and which bars each staff has music in; time/key/tempo changes, "
                        "rehearsal marks, repeats, barlines and breaks by bar; tempo markings. Start here, then use "
                        "score_read for the music itself. Bars are numbered by position (m1 is the first bar, pickup "
                        "included). Read-only.";
        c.output["score"] = { DataType::Object, "title, bars, staves, path, needSave, stateVersion" };
        c.output["parts"] = { DataType::Array, "name, instrument, staves [{staff, bars_with_music}]" };
        c.output["bar_attributes"] = { DataType::Array, "{bar, time?, key?, mark?, repeat?, barline?, break?}" };
        c.output["tempo"] = { DataType::Array, "{bar, text, bpm}" };
        c.handler = scoreOverview;
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/score_read";
        c.name = "score_read";
        c.title = "Read music";
        c.description = "Return bars from..to of the open score in the MuseScore Agent compact notation (NOTATION.md): "
                        "one line per bar per voice, e.g. `m5 v1 | F5:q~+< (F5:e G5:s F5:s E5:e+st+!) F5:e+st`. "
                        "Pitch is sounding pitch (C4 = middle C) unless pitch=\"written\"; durations w h q e s t x with dots; "
                        "~ tie; ( ) slur; [..] chord; {3:2 ...} tuplet; g(..) grace notes; r rest; R full-bar rest; "
                        "_ empty space in voices 2-4; +codes are marks (st staccato, ac accent, mf, < > ! hairpins, "
                        "ly\"..\" lyrics...); `m12 @ ...` lines are bar attributes; `m12 h | {Cm7}:h` chord symbols. "
                        "Large ranges are refused with the number of bars that fit. Read-only.";
        c.input["from"] = rangeArg("First bar (default 1)");
        c.input["to"] = rangeArg("Last bar, inclusive (default the last bar)");
        c.input["staves"] = { DataType::Array, "Staff numbers to include (default all)" };
        c.input["staves"].itemsType = DataType::Integer;
        c.input["voices"] = { DataType::Array, "Voices 1-4 to include (default all)" };
        c.input["voices"].itemsType = DataType::Integer;
        c.input["pitch"] = { DataType::String, "\"sounding\" (default) or \"written\" (as notated for transposing instruments)" };
        c.input["pitch"].enumValues = { "sounding", "written" };
        c.output["text"] = { DataType::String, "The bars in compact notation" };
        c.output["from"] = { DataType::Integer, "First bar returned" };
        c.output["to"] = { DataType::Integer, "Last bar returned" };
        c.output["stateVersion"] = { DataType::Integer, "Change counter at the time of reading (see score_changes)" };
        c.handler = scoreRead;
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/score_validate";
        c.name = "score_validate";
        c.title = "Check the score";
        c.description = "Check bars from..to for problems without changing anything: voice 1 not filling a bar "
                        "(E_BAR_SUM), ties that don't land (E_TIE), malformed tuplets (E_TUPLET); `details` lists "
                        "things the notation can't show (cross-voice ties, ...). Read-only; never repairs.";
        c.input["from"] = rangeArg("First bar (default 1)");
        c.input["to"] = rangeArg("Last bar (default the last bar)");
        c.output["ok"] = { DataType::Boolean, "No problems found" };
        c.output["problems"] = { DataType::Array, "{code, message}" };
        c.output["details"] = { DataType::Array, "Notes about things only visible in detail" };
        c.handler = scoreValidate;
        registry.add(c);
    }
    {
        CommandInfo c;
        c.uri = "command://notation/score_changes";
        c.name = "score_changes";
        c.title = "What changed";
        c.description = "Bars and staves changed since stateVersion `since` (from an earlier score_read/score_overview), "
                        "by anyone: the user's edits, undo and redo included. Re-read only those bars. `complete` is "
                        "false when `since` is older than the kept history; then re-read everything.";
        c.input["since"] = { DataType::Integer, "A stateVersion from an earlier call (0 = since the score opened)" };
        c.input["since"].min = Val(0);
        c.output["stateVersion"] = { DataType::Integer, "The current version" };
        c.output["complete"] = { DataType::Boolean, "False if history before `since` was dropped" };
        c.output["bars"] = { DataType::String, "Changed bars, e.g. \"3-5,12\"" };
        c.output["staves"] = { DataType::String, "Changed staves" };
        c.output["changes"] = { DataType::Array, "{version, bars, staves} per change" };
        c.handler = scoreChanges;
        registry.add(c);
    }
}
