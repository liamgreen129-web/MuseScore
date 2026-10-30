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
// P2.3 write_voice on the engraving model, inside the core's Transaction with the score's own
// undo stack. Scenario numbers are specs/notes.md NOTES-WV-xx.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>

#include "engraving/dom/accidental.h"
#include "engraving/dom/beam.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/part.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftype.h"
#include "engraving/dom/mscore.h"
#include "engraving/editing/undo.h"
#include "engraving/types/constants.h"
#include "engraving/tests/utils/scorerw.h"

#include "address/address.h"
#include "commands/notes/clearrange.h"
#include "commands/notes/noteops.h"
#include "commands/notes/writevoice.h"
#include "engraving/dom/selectionfilter.h"
#include "core/handles.h"
#include "core/transaction.h"
#include "core/undobackend.h"
#include "notation/parser.h"
#include "notation/writer.h"
#include "read/scoreprinter.h"

using namespace mu::agent;
using namespace mu::agent::notes;
using namespace mu::engraving;
using muse::Ret;
namespace fs = std::filesystem;
namespace nt = mu::agent::notation;

namespace {
fs::path fixtureDir()
{
    return fs::path(agent_tests_DATA_ROOT) / "../../../tests/fixtures/synthetic";
}

MasterScore* load(const std::string& name)
{
    return ScoreRW::readScore(muse::String::fromStdString((fixtureDir() / name).string()), true);
}

struct Outcome {
    Ret ret;
    WriteVoiceResult r;
    bool changed = false;
};

Outcome runWrite(MasterScore* score, int staff, const std::string& content, PitchMode pitch = PitchMode::Default,
              bool keepAnnotations = true)
{
    MScore::setError(MsError::MS_NO_ERROR);
    core::ScoreUndoBackend backend(score);
    core::Transaction tx(score, backend);
    Outcome o;
    o.ret = tx.begin("Agent: Write voice");
    if (!o.ret) {
        return o;
    }
    WriteVoiceArgs a;
    a.staff = staff;
    a.content = content;
    a.pitch = pitch;
    a.keepAnnotations = keepAnnotations;
    o.ret = tx.step("write_voice", [&] { return writeVoice(score, a, o.r); });
    if (o.ret) {
        o.ret = tx.commit();
        o.changed = tx.changed();
    }
    return o;
}

std::string print(MasterScore* score, int staff, int from = 1, int to = 0, bool written = false)
{
    read::ReadOptions opt;
    opt.fromBar = from;
    opt.toBar = to;
    opt.staves = { staff };
    opt.writtenPitch = written;
    return nt::write(read::printScore(score, opt));
}

void stripMarks(std::vector<nt::Item>& items)
{
    for (nt::Item& item : items) {
        if (nt::Event* e = std::get_if<nt::Event>(&item)) {
            e->marks.clear();
            e->slurOpen = e->slurClose = 0;
        } else {
            nt::Tuplet& t = std::get<nt::Tuplet>(item);
            t.slurOpen = t.slurClose = 0;
            stripMarks(t.items);
        }
    }
}

//! The text with every +mark and slur taken out (write_voice writes notes and rhythms only)
std::string withoutMarks(const std::string& text)
{
    nt::ParseResult r = nt::parse(text);
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.errors.front().toString());
    for (nt::Line& l : r.doc.lines) {
        if (nt::VoiceLine* vl = std::get_if<nt::VoiceLine>(&l)) {
            stripMarks(vl->items);
        }
    }
    return nt::write(r.doc);
}

//! Only the "m<N> v<V> | ..." lines (headers, attributes, harmony and comments dropped).
std::string voiceLines(const std::string& text)
{
    std::istringstream in(text);
    std::string out;
    for (std::string line; std::getline(in, line);) {
        if (line.size() > 1 && line[0] == 'm' && line.find(" v") != std::string::npos && line.find(" | ") != std::string::npos) {
            out += line + "\n";
        }
    }
    return out;
}

bool has(const std::string& s, const std::string& part)
{
    return s.find(part) != std::string::npos;
}

void undo(MasterScore* score)
{
    score->undoRedo(true, nullptr);
}

class Agent_WriteVoice : public ::testing::Test
{
protected:
    std::unique_ptr<MasterScore> open(const std::string& name)
    {
        std::unique_ptr<MasterScore> s(load(name));
        EXPECT_TRUE(s) << name;
        return s;
    }
};
}

// WV-01: rewrite a melody; one undo entry; one undo restores it
TEST_F(Agent_WriteVoice, RewritesBarsAndUndoesInOneStep)
{
    auto s = open("pickup-bar.mscz");
    const std::string before = print(s.get(), 1);
    const size_t idx = s->undoStack()->currentIndex();
    Outcome o = runWrite(s.get(), 1, "m2 v1 | C5:q D5:q E5:q\nm3 v1 | F5:h.");
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(o.changed);
    EXPECT_EQ(s->undoStack()->currentIndex(), idx + 1);
    EXPECT_EQ(o.r.fromBar, 2);
    EXPECT_EQ(o.r.toBar, 3);
    const std::string after = print(s.get(), 1);
    EXPECT_TRUE(has(after, "m2(1) v1 | C5:q D5:q E5:q\n")) << after;
    EXPECT_TRUE(has(after, "m3(2) v1 | F5:h.\n")) << after;
    EXPECT_TRUE(has(after, "m4(3) v1 | B4:h A4:q\n")) << after; // untouched
    EXPECT_TRUE(has(o.r.text, "m2(1) v1 | C5:q D5:q E5:q")) << o.r.text;
    undo(s.get());
    EXPECT_EQ(print(s.get(), 1), before);
}

//! Every staff of every fixture in `dir`, written back as it reads, reads the same. Returns
//! the number of staves checked; staves write_voice v1 refuses (marks, tablature...) are skipped.
int roundTrip(const fs::path& dir, std::map<std::string, int>& skipped)
{
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".mscz") {
            continue;
        }
        const std::string name = entry.path().filename().string();
        for (bool written : { false, true }) {
            std::unique_ptr<MasterScore> s(ScoreRW::readScore(muse::String::fromStdString(entry.path().string()), true));
            if (!s) {
                ADD_FAILURE() << name;
                continue;
            }
            for (int staff = 1; staff <= static_cast<int>(s->nstaves()); ++staff) {
                const std::string text = withoutMarks(print(s.get(), staff, 1, 0, written));
                Outcome o = runWrite(s.get(), staff, text, written ? PitchMode::Written : PitchMode::Sounding);
                bool skip = false;
                for (const char* why : { "marks and slurs", "tablature", "measure repeat", "local time signature",
                                         "cross-staff" }) {
                    if (!o.ret && has(o.ret.text(), why)) {
                        ++skipped[why];
                        skip = true;
                    }
                }
                if (skip) {
                    continue; // not expressible in write_voice v1, refused before anything changed
                }
                EXPECT_TRUE(o.ret) << name << " staff " << staff << (written ? " written" : " sounding") << ": "
                                   << o.ret.text();
                EXPECT_EQ(voiceLines(withoutMarks(print(s.get(), staff, 1, 0, written))), voiceLines(text))
                    << name << " staff " << staff;
                for (const std::string& gone : o.r.annotationsRemoved) {
                    // the same rhythm is written back, so every annotation has its segment again
                    ADD_FAILURE() << name << " staff " << staff << ": removed " << gone;
                }
                ++checked;
            }
            // Deliberately not destroyed: MuseScore 4.7.6 can double-delete in ~UndoStack
            // (RemoveElement::cleanup) when a score whose history holds a large range delete is
            // destroyed, even after undo; startCmd/deleteRange/endCmd alone reproduces it on
            // corpus/beethoven-op74 (DECISIONS 2026-10-29). Not an agent bug; leaking keeps the run stable.
            s.release();
        }
    }
    return checked;
}

TEST_F(Agent_WriteVoice, WritingWhatWasReadChangesNothing)
{
    std::map<std::string, int> skipped;
    const int checked = roundTrip(fixtureDir(), skipped);
    EXPECT_GT(checked, 20);
    std::cout << "synthetic: " << checked << " staff passes checked";
    for (const auto& [why, n] : skipped) {
        std::cout << "; " << n << " skipped (" << why << ")";
    }
    std::cout << std::endl;
}

// the same over the public-domain corpus (git-ignored; made by tools/fixtures/make_corpus_fixtures.py)
TEST_F(Agent_WriteVoice, WritingWhatWasReadChangesNothingCorpus)
{
    const fs::path dir = fixtureDir() / "../corpus";
    if (!fs::exists(dir)) {
        GTEST_SKIP() << "no corpus fixtures";
    }
    std::map<std::string, int> skipped;
    const int checked = roundTrip(dir, skipped);
    std::cout << "corpus: " << checked << " staff passes checked";
    for (const auto& [why, n] : skipped) {
        std::cout << "; " << n << " skipped (" << why << ")";
    }
    std::cout << std::endl;
}

// WV-02/03: a second voice under voice 1, with a gap; a voice of only gaps is removed
TEST_F(Agent_WriteVoice, VoicesTwoToFourAndGaps)
{
    auto s = open("pickup-bar.mscz");
    Outcome o = runWrite(s.get(), 1, "m3 v2 | C4:h _:q");
    ASSERT_TRUE(o.ret) << o.ret.text();
    std::string t = print(s.get(), 1, 3, 3);
    EXPECT_TRUE(has(t, "m3(2) v1 | G4:h F4:q\n")) << t;
    EXPECT_TRUE(has(t, "m3(2) v2 | C4:h _:q\n")) << t;

    o = runWrite(s.get(), 1, "m3 v2 | _:h _:q");
    ASSERT_TRUE(o.ret) << o.ret.text();
    t = print(s.get(), 1, 3, 3);
    EXPECT_FALSE(has(t, " v2 |")) << t;

    auto v = open("voices-four.mscz");
    o = runWrite(v.get(), 1, "m2 v4 | R\nm1 v2 | _:h A5:h");
    ASSERT_TRUE(o.ret) << o.ret.text();
    t = print(v.get(), 1, 1, 2);
    EXPECT_TRUE(has(t, "m1 v2 | _:h A5:h\n")) << t;
    EXPECT_TRUE(has(t, "m1 v3 | E5:h D5:h\n")) << t;
    EXPECT_TRUE(has(t, "m2 v4 | R\n")) << t;
}

// WV-04/05: tuplets, nested tuplets
TEST_F(Agent_WriteVoice, Tuplets)
{
    auto s = open("pickup-bar.mscz");
    Outcome o = runWrite(s.get(), 1, "m2 v1 | {3:2 C5:e D5:e E5:e} {5:4 F5:s G5:s A5:s B5:s C6:s} r:q\n"
                                  "m3 v1 | {3:2 C5:q {3:2 D5:e E5:e F5:e} G5:q} r:q");
    ASSERT_TRUE(o.ret) << o.ret.text();
    const std::string t = print(s.get(), 1, 2, 3);
    EXPECT_TRUE(has(t, "v1 | {3:2 C5:e D5:e E5:e} {5:4 F5:s G5:s A5:s B5:s C6:s} r:q\n")) << t;
    EXPECT_TRUE(has(t, "v1 | {3:2 C5:q {3:2 D5:e E5:e F5:e} G5:q} r:q\n")) << t;
}

// WV-06: ties inside the content, across the bar line, and out of the last bar written
TEST_F(Agent_WriteVoice, Ties)
{
    auto s = open("pickup-bar.mscz");
    Outcome o = runWrite(s.get(), 1, "m2 v1 | C5:h D5:q~\nm3 v1 | D5:q E5:q~ E5:q");
    ASSERT_TRUE(o.ret) << o.ret.text();
    std::string t = print(s.get(), 1, 2, 3);
    EXPECT_TRUE(has(t, "v1 | C5:h D5:q~\n")) << t;
    EXPECT_TRUE(has(t, "v1 | D5:q E5:q~ E5:q\n")) << t;

    o = runWrite(s.get(), 1, "m4 v1 | B4:h E4:q~"); // m5 starts with E4
    ASSERT_TRUE(o.ret) << o.ret.text();
    t = print(s.get(), 1, 4, 4);
    EXPECT_TRUE(has(t, "v1 | B4:h E4:q~\n")) << t;

    const std::string before = print(s.get(), 1);
    o = runWrite(s.get(), 1, "m5 v1 | E4:h D4:q~"); // m6 starts with G4
    EXPECT_FALSE(o.ret);
    EXPECT_TRUE(has(o.ret.text(), "E_INVALID_ARG")) << o.ret.text();
    EXPECT_TRUE(has(o.ret.text(), "next bar")) << o.ret.text();
    EXPECT_EQ(print(s.get(), 1), before);
}

// a tie coming into the first bar written is kept when the new first event has that pitch
TEST_F(Agent_WriteVoice, IncomingTies)
{
    auto s = open("ties-barlines.mscz");
    Outcome o = runWrite(s.get(), 1, "m2 v1 | E5:q F5:q [C5,E5,G5]:h~");
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(o.r.tiesRemoved.empty());
    std::string t = print(s.get(), 1, 1, 2);
    EXPECT_TRUE(has(t, "m1 v1 | C5:h D5:q E5:q~\n")) << t;

    o = runWrite(s.get(), 1, "m2 v1 | F5:h [C5,E5,G5]:h~");
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(o.r.tiesRemoved.size(), 1u);
    t = print(s.get(), 1, 1, 1);
    EXPECT_TRUE(has(t, "m1 v1 | C5:h D5:q E5:q\n")) << t;
}

// WV-07: written and sounding pitch on a transposing staff
TEST_F(Agent_WriteVoice, PitchModes)
{
    auto s = open("transposing-instruments.mscz");
    Outcome o = runWrite(s.get(), 2, "m1 v1 | C4:q D4:q F#4:q Gb4:q", PitchMode::Written);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 2, 1, 1, true), "m1 v1 | C4:q D4:q F#4:q Gb4:q\n")) << print(s.get(), 2, 1, 1, true);
    EXPECT_TRUE(has(print(s.get(), 2, 1, 1, false), "m1 v1 | Bb3:q C4:q E4:q Fb4:q\n")) << print(s.get(), 2, 1, 1, false);

    o = runWrite(s.get(), 2, "m2 v1 | Bb3:q C4:q E4:q Fb4:q");
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 2, 2, 2, true), "m2 v1 | C4:q D4:q F#4:q Gb4:q\n")) << print(s.get(), 2, 2, 2, true);

    // a header saying pitch=written switches the default; contradicting it is refused
    o = runWrite(s.get(), 2, "# s2 \"Trumpet in B♭\" clef=treble transposes=-M2 pitch=written key=D time=4/4\nm3 v1 | D4:w");
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 2, 3, 3, false), "m3 v1 | C4:w\n")) << print(s.get(), 2, 3, 3, false);
    o = runWrite(s.get(), 2, "# s2 \"Trumpet\" pitch=written\nm3 v1 | D4:w", PitchMode::Sounding);
    EXPECT_FALSE(o.ret);
}

// WV-08: grace notes before (with and without slash) and after, chords, order kept
TEST_F(Agent_WriteVoice, GraceNotes)
{
    auto s = open("pickup-bar.mscz");
    Outcome o = runWrite(s.get(), 1, "m2 v1 | g(B4:s C5:s) D5:h g/(F5:e) E5:q\nm3 v1 | g([A4,C5]:s) B4:h C5:q ga(D5:s E5:s)");
    ASSERT_TRUE(o.ret) << o.ret.text();
    const std::string t = print(s.get(), 1, 2, 3);
    EXPECT_TRUE(has(t, "v1 | g(B4:s C5:s) D5:h g/(F5:e) E5:q\n")) << t;
    EXPECT_TRUE(has(t, "v1 | g([A4,C5]:s) B4:h C5:q ga(D5:s E5:s)\n")) << t;
}

// WV-09: pickup and irregular bars sum to their own length; problems are named
TEST_F(Agent_WriteVoice, RefusesBeforeChangingAnything)
{
    auto s = open("pickup-bar.mscz");
    const std::string before = print(s.get(), 1);
    const size_t idx = s->undoStack()->currentIndex();
    struct Case {
        std::string content;
        std::string expect;
    };
    const Case cases[] = {
        { "m1 v1 | G4:h", "m1 v1 is 1/4 too long" },
        { "m2 v1 | C5:h", "m2 v1 is 1/4 short" },
        { "m2 v1 | C5:h _:q", "voices 2-4" },
        { "m2 v1 | C5:h+p D5:q", "marks and slurs" },
        { "m2 v1 | C5:h (D5:q)", "marks and slurs" },
        { "m2 v1 | r:h g(C5:e) r:q", "not a rest" },
        { "m2 v1 | {3:2 C5:q D5:q E5:q} r:q r:q r:q", "too long" },
        { "m2 v1 | {3:5 C5:q D5:q E5:q}", "not one note value" },
        { "m2 v1 | C5:h D5:q\nm2 v1 | C5:h D5:q", "appears twice" },
        { "m99 v1 | C5:h D5:q", "outside the score" },
        { "m2 v1 | C5:h D5:q~", "next bar" },
        { "m2 v1 | C5:h~ D5:q", "same pitch" },
        { "# s2 \"Oboe\"\nm2 v1 | C5:h D5:q", "header is for staff 2" },
        { "m2 @ time=2/4\nm2 v1 | C5:h D5:q", "bar attributes" },
        { "m2 v1 | C5:h D5:q\nm2 h | {C}:h.", "chord-symbol" },
        { "m2 v1 | A9:h D5:q", "0-127" },
        { "m2 v1 | C5:h g(D5:q.) D5:q", "undotted" },
        { "m2 v1 | C5:q g/(D5:q) D5:q E5:q", "8, 16 or 32" },
        { "this is not notation", "does not parse" },
        { "", "no voice lines" },
    };
    for (const Case& c : cases) {
        Outcome o = runWrite(s.get(), 1, c.content);
        EXPECT_FALSE(o.ret) << c.content;
        EXPECT_TRUE(has(o.ret.text(), c.expect)) << c.content << " -> " << o.ret.text();
        EXPECT_EQ(s->undoStack()->currentIndex(), idx) << c.content;
        EXPECT_EQ(print(s.get(), 1), before) << c.content;
    }
    Outcome o = runWrite(s.get(), 2, "m2 v1 | C5:h D5:q");
    EXPECT_TRUE(has(o.ret.text(), "E_RANGE")) << o.ret.text();
    // the pickup bar takes exactly its own length
    o = runWrite(s.get(), 1, "m1 v1 | A4:e B4:e");
    EXPECT_TRUE(o.ret) << o.ret.text();
}

// WV-12: dynamics and chord symbols stay unless keepAnnotations is false
TEST_F(Agent_WriteVoice, KeepsAnnotations)
{
    auto s = open("chord-symbols.mscz");
    Outcome o = runWrite(s.get(), 1, "m1 v1 | C5:h B4:h");
    ASSERT_TRUE(o.ret) << o.ret.text();
    std::string t = print(s.get(), 1, 1, 1);
    EXPECT_TRUE(has(t, "m1 h | {C}:h {Am7}:h\n")) << t;
    EXPECT_TRUE(o.r.annotationsRemoved.empty());

    // a chord symbol on beat 3 has nothing to sit on under a whole note: MuseScore drops it, and it is reported
    o = runWrite(s.get(), 1, "m2 v1 | C5:w");
    ASSERT_TRUE(o.ret) << o.ret.text();
    t = print(s.get(), 1, 2, 2);
    EXPECT_TRUE(has(t, "m2 v1 | C5:w\n")) << t;
    if (!has(t, "{G7}")) {
        ASSERT_FALSE(o.r.annotationsRemoved.empty()) << t;
        EXPECT_TRUE(has(o.r.annotationsRemoved.front(), "m2+1/2")) << o.r.annotationsRemoved.front();
    }

    o = runWrite(s.get(), 1, "m3 v1 | C5:h B4:h", PitchMode::Default, false);
    ASSERT_TRUE(o.ret) << o.ret.text();
    t = print(s.get(), 1, 3, 3);
    EXPECT_FALSE(has(t, "m3 h |")) << t;
}

// --- clear_range (NOTES-CR-xx)

namespace {
Outcome runClear(MasterScore* score, const ClearRangeArgs& a, ClearRangeResult* out = nullptr)
{
    MScore::setError(MsError::MS_NO_ERROR);
    core::ScoreUndoBackend backend(score);
    core::Transaction tx(score, backend);
    Outcome o;
    o.ret = tx.begin("Agent: Clear range");
    ClearRangeResult r;
    if (o.ret) {
        o.ret = tx.step("clear_range", [&] { return clearRange(score, a, r); });
    }
    if (o.ret) {
        o.ret = tx.commit();
        o.changed = tx.changed();
    }
    if (out) {
        *out = r;
    }
    return o;
}

ClearRangeArgs bars(int s1, int s2, int from, int to)
{
    ClearRangeArgs a;
    a.staffFrom = s1;
    a.staffTo = s2;
    a.from.bar = from;
    a.to.bar = to;
    return a;
}
}

// CR-01: clear voice 2 in two bars: it disappears, voice 1 untouched; one undo restores
TEST_F(Agent_WriteVoice, ClearVoiceTwo)
{
    auto s = open("voices-four.mscz");
    const std::string before = print(s.get(), 1);
    ClearRangeArgs a = bars(1, 1, 1, 2);
    a.voices = { 2 };
    ClearRangeResult r;
    Outcome o = runClear(s.get(), a, &r);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(o.changed);
    const std::string t = print(s.get(), 1, 1, 2);
    EXPECT_FALSE(has(t, " v2 |")) << t;
    EXPECT_TRUE(has(t, "m1 v1 | E6:e D6:e C6:e D6:e E6:e G6:e E6:e D6:e\n")) << t;
    EXPECT_TRUE(has(t, "m1 v3 | E5:h D5:h\n")) << t;
    EXPECT_EQ(r.fromBar, 1);
    EXPECT_EQ(r.toBar, 2);
    undo(s.get());
    EXPECT_EQ(print(s.get(), 1), before);
}

// CR-02: clear voice 1 keeping chord symbols; the user's own selection filter is neither used nor changed
TEST_F(Agent_WriteVoice, ClearKeepsExcludedTypesAndIgnoresUserFilter)
{
    auto s = open("chord-symbols.mscz");
    s->selectionFilter().setFiltered(VoicesSelectionFilterTypes::FIRST_VOICE, false); // the user's filter
    const SelectionFilter userFilter = s->selectionFilter();
    ClearRangeArgs a = bars(1, 1, 1, 2);
    a.include = { "slurs", "ties", "articulations", "lyrics" };
    Outcome o = runClear(s.get(), a);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(s->selectionFilter() == userFilter);
    const std::string t = print(s.get(), 1, 1, 2);
    EXPECT_TRUE(has(t, "m1 v1 | R\n")) << t;
    EXPECT_TRUE(has(t, "m1 h | {C}:h {Am7}:h\n")) << t;
    EXPECT_TRUE(has(t, "m2 h | {Dm7}:h {G7}:h\n")) << t;

    o = runClear(s.get(), bars(1, 1, 3, 3)); // default: everything goes
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_FALSE(has(print(s.get(), 1, 3, 3), "m3 h |")) << print(s.get(), 1, 3, 3);
}

// CR-03: beats 2-3 of one bar across two staves
TEST_F(Agent_WriteVoice, ClearBeatsAcrossStaves)
{
    auto s = open("transposing-instruments.mscz");
    ClearRangeArgs a = bars(1, 2, 1, 1);
    a.from.beat = 2;
    a.from.hasBeat = true;
    a.to.beat = 4;
    a.to.hasBeat = true;
    Outcome o = runClear(s.get(), a);
    ASSERT_TRUE(o.ret) << o.ret.text();
    for (int staff : { 1, 2 }) {
        const std::string t = print(s.get(), staff, 1, 1);
        EXPECT_TRUE(has(t, "m1 v1 | C4:q r:") && has(t, " F4:q\n")) << t;
        EXPECT_FALSE(has(t, "D4") || has(t, "E4")) << t;
    }
    EXPECT_TRUE(has(print(s.get(), 3, 1, 1), "m1 v1 | C4:q D4:q E4:q F4:q\n")) << print(s.get(), 3, 1, 1);
}

// CR-04: a range cutting through a nested tuplet is cleared the UI's way and still adds up
TEST_F(Agent_WriteVoice, ClearCutsTuplet)
{
    auto s = open("tuplets-nested.mscz");
    ClearRangeArgs a = bars(1, 1, 1, 1);
    a.to.beat = 3;
    a.to.hasBeat = true;
    Outcome o = runClear(s.get(), a);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_FALSE(has(print(s.get(), 1, 1, 1), "C5") || has(print(s.get(), 1, 1, 1), "D5")) << print(s.get(), 1, 1, 1);
}

TEST_F(Agent_WriteVoice, ClearRefusesBeforeChangingAnything)
{
    auto s = open("pickup-bar.mscz");
    const std::string before = print(s.get(), 1);
    const size_t idx = s->undoStack()->currentIndex();
    struct Case {
        ClearRangeArgs a;
        std::string expect;
    };
    std::vector<Case> cases;
    cases.push_back({ bars(1, 2, 1, 1), "within 1-1" });
    cases.push_back({ bars(1, 1, 3, 2), "after" });
    cases.push_back({ bars(1, 1, 1, 99), "outside the score" });
    { ClearRangeArgs a = bars(1, 1, 2, 2); a.voices = { 5 }; cases.push_back({ a, "not 1-4" }); }
    { ClearRangeArgs a = bars(1, 1, 2, 2); a.voices = {}; cases.push_back({ a, "empty" }); }
    { ClearRangeArgs a = bars(1, 1, 2, 2); a.include = { "noise" }; cases.push_back({ a, "unknown element type" }); }
    { ClearRangeArgs a = bars(1, 1, 2, 2); a.from.beat = 1.5; a.from.hasBeat = true; cases.push_back({ a, "nothing starts at \"from\"" }); }
    { ClearRangeArgs a = bars(1, 1, 2, 2); a.to.beat = 2.5; a.to.hasBeat = true; cases.push_back({ a, "nothing starts at \"to\"" }); }
    { ClearRangeArgs a = bars(1, 1, 2, 2); a.from.beat = 4; a.from.hasBeat = true; cases.push_back({ a, "past the end" }); }
    { ClearRangeArgs a = bars(1, 1, 2, 2); a.from.beat = 1.0001; a.from.hasBeat = true; cases.push_back({ a, "not a position" }); }
    { ClearRangeArgs a = bars(1, 1, 2, 2); a.to.beat = 1; a.to.hasBeat = true; cases.push_back({ a, "empty" }); }
    for (const Case& c : cases) {
        Outcome o = runClear(s.get(), c.a);
        EXPECT_FALSE(o.ret) << c.expect;
        EXPECT_TRUE(has(o.ret.text(), c.expect)) << c.expect << " -> " << o.ret.text();
        EXPECT_EQ(s->undoStack()->currentIndex(), idx) << c.expect;
        EXPECT_EQ(print(s.get(), 1), before) << c.expect;
    }
}

// --- tie, tuplet, grace (NOTES-TIE, NOTES-TU, NOTES-GR)

namespace {
using muse::Val;
using muse::ValMap;

Outcome runOp(MasterScore* score, muse::Ret (*op)(MasterScore*, const Params&, notes::Out&), const Params& p,
              notes::Out* out = nullptr)
{
    MScore::setError(MsError::MS_NO_ERROR);
    core::ScoreUndoBackend backend(score);
    core::Transaction tx(score, backend);
    Outcome o;
    notes::Out res;
    o.ret = tx.begin("Agent: op");
    if (o.ret) {
        o.ret = tx.step("op", [&] { return op(score, p, res); });
    }
    if (o.ret) {
        o.ret = tx.commit();
        o.changed = tx.changed();
    }
    if (out) {
        *out = res;
    }
    return o;
}

Val at(int staff, int bar, double beat, int voice = 1)
{
    ValMap m;
    m["staff"] = Val(staff);
    m["measure"] = Val(bar);
    m["beat"] = Val(beat);
    m["voice"] = Val(voice);
    return Val(m);
}

void expectRefused(MasterScore* s, muse::Ret (*op)(MasterScore*, const Params&, notes::Out&), const Params& p,
                   const std::string& expect)
{
    const std::string before = print(s, 1);
    const size_t idx = s->undoStack()->currentIndex();
    Outcome o = runOp(s, op, p);
    EXPECT_FALSE(o.ret) << expect;
    EXPECT_TRUE(has(o.ret.text(), expect)) << expect << " -> " << o.ret.text();
    EXPECT_EQ(s->undoStack()->currentIndex(), idx) << expect;
    EXPECT_EQ(print(s, 1), before) << expect;
}
}

// TIE-01/02/05: tie neighbouring same pitches, across the bar line, then remove a tie
TEST_F(Agent_WriteVoice, TieAddAndRemove)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | C5:h C5:q\nm3 v1 | C5:h. ").ret);
    notes::Out out;
    Outcome o = runOp(s.get(), notes::tie, { { "at", at(1, 2, 1) } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | C5:h~ C5:q\n")) << print(s.get(), 1, 2, 2);
    o = runOp(s.get(), notes::tie, { { "at", at(1, 2, 3) }, { "note", Val("C5") } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(out["bars"].toString(), "2-3");
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | C5:h~ C5:q~\n")) << print(s.get(), 1, 2, 2);
    o = runOp(s.get(), notes::tie, { { "at", at(1, 2, 1) }, { "action", Val("remove") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | C5:h C5:q~\n")) << print(s.get(), 1, 2, 2);
    undo(s.get());
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | C5:h~ C5:q~\n")) << print(s.get(), 1, 2, 2);
}

// TIE-03/04 and the other refusals: nothing changes, no notes are added
TEST_F(Agent_WriteVoice, TieRefusals)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | [C5,E5]:h C5:q\nm3 v1 | D5:h r:q").ret);
    ASSERT_TRUE(runWrite(s.get(), 1, "m4 v1 | B4:h A4:q\nm4 v2 | _:h B4:q").ret);
    expectRefused(s.get(), notes::tie, { { "at", at(1, 2, 1) } }, "E5");                          // chord: E5 has no partner
    expectRefused(s.get(), notes::tie, { { "at", at(1, 2, 3) } }, "has that pitch");             // next bar has no C5
    expectRefused(s.get(), notes::tie, { { "at", at(1, 3, 3) } }, "is a rest");
    expectRefused(s.get(), notes::tie, { { "at", at(1, 2, 1) }, { "note", Val("G5") } }, "has no G5");
    expectRefused(s.get(), notes::tie, { { "at", at(1, 2, 1) }, { "note", Val("C5") }, { "action", Val("remove") } }, "not tied");
    expectRefused(s.get(), notes::tie, { { "at", at(1, 2, 1) }, { "action", Val("toggle") } }, "add or remove");
    expectRefused(s.get(), notes::tie, { { "at", at(1, 4, 1) } }, "another voice");             // next B4 only in voice 2
    expectRefused(s.get(), notes::tie, { { "at", at(1, 2, 2) } }, "no note or rest starts");
    expectRefused(s.get(), notes::tie, { { "at", at(9, 2, 1) } }, "outside 1-1");
    expectRefused(s.get(), notes::tie, {}, "missing at");
    ASSERT_TRUE(runOp(s.get(), notes::tie, { { "at", at(1, 2, 1) }, { "note", Val("C5") } }).ret);
    expectRefused(s.get(), notes::tie, { { "at", at(1, 2, 1) }, { "note", Val("C5") } }, "already tied");
}

// TU-01/02/03: a quarter becomes a triplet, a half becomes 5:4 keeping its pitch; an impossible unit is refused
TEST_F(Agent_WriteVoice, TupletFromNote)
{
    auto s = open("pickup-bar.mscz");
    notes::Out out;
    Outcome o = runOp(s.get(), notes::tuplet, { { "at", at(1, 2, 3) }, { "ratio", Val("3:2") } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | E4:h {3:2 D4:e r:e r:e}\n")) << print(s.get(), 1, 2, 2);
    EXPECT_TRUE(has(out["handle"].toString(), "Chord:")) << out["handle"].toString();
    o = runOp(s.get(), notes::tuplet, { { "at", at(1, 3, 1) }, { "ratio", Val("5:4") }, { "numberType", Val("ratio") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 3, 3), "m3(2) v1 | {5:4 G4:e r:e r:e r:e r:e} F4:q\n")) << print(s.get(), 1, 3, 3);
    undo(s.get());
    EXPECT_TRUE(has(print(s.get(), 1, 3, 3), "m3(2) v1 | G4:h F4:q\n")) << print(s.get(), 1, 3, 3);

    ASSERT_TRUE(runWrite(s.get(), 1, "m4 v1 | B4:q. A4:e r:q").ret);
    expectRefused(s.get(), notes::tuplet, { { "at", at(1, 4, 3) }, { "ratio", Val("5:3") } }, "not a note value");
    expectRefused(s.get(), notes::tuplet, { { "at", at(1, 4, 3) }, { "ratio", Val("3") } }, "actual:normal");
    expectRefused(s.get(), notes::tuplet, { { "at", at(1, 4, 3) }, { "ratio", Val("1:2") } }, "actual:normal");
    expectRefused(s.get(), notes::tuplet, { { "at", at(1, 4, 3) }, { "ratio", Val("3:2") }, { "bracketType", Val("x") } },
                  "bracketType");
    expectRefused(s.get(), notes::tuplet, { { "at", at(1, 4, 3) }, { "ratio", Val("3:2") }, { "numberType", Val("x") } },
                  "numberType");
}

// GR-01/02/03/04: graces before (order: newest in front) and after, spelling as given, refusals
TEST_F(Agent_WriteVoice, GraceNotesOneAtATime)
{
    auto s = open("pickup-bar.mscz");
    notes::Out out;
    Outcome o = runOp(s.get(), notes::grace, { { "at", at(1, 2, 1) }, { "type", Val("acciaccatura") }, { "note", Val("Fb4") } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(out["handle"].toString(), "Note:")) << out["handle"].toString();
    o = runOp(s.get(), notes::grace, { { "at", at(1, 2, 1) }, { "type", Val("acciaccatura") }, { "note", Val("D4") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    o = runOp(s.get(), notes::grace, { { "at", at(1, 2, 3) }, { "type", Val("grace16after") }, { "note", Val("C#4") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | g/(D4:e Fb4:e) E4:h D4:q ga(C#4:s)\n")) << print(s.get(), 1, 2, 2);

    auto t = open("transposing-instruments.mscz");
    o = runOp(t.get(), notes::grace, { { "at", at(2, 1, 1) }, { "type", Val("grace16") }, { "note", Val("B3") }, { "pitch", Val("written") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(t.get(), 2, 1, 1, true), "m1 v1 | g(B3:s) D4:q")) << print(t.get(), 2, 1, 1, true);
    EXPECT_TRUE(has(print(t.get(), 2, 1, 1, false), "m1 v1 | g(A3:s) C4:q")) << print(t.get(), 2, 1, 1, false);

    ASSERT_TRUE(runWrite(s.get(), 1, "m4 v1 | B4:h r:q").ret);
    expectRefused(s.get(), notes::grace, { { "at", at(1, 4, 3) }, { "type", Val("grace16") }, { "note", Val("C5") } }, "is a rest");
    expectRefused(s.get(), notes::grace, { { "at", at(1, 4, 1) }, { "type", Val("grace64") }, { "note", Val("C5") } }, "type must be");
    expectRefused(s.get(), notes::grace, { { "at", at(1, 4, 1) }, { "type", Val("grace16") }, { "note", Val("H5") } }, "note must be");
    expectRefused(s.get(), notes::grace, { { "at", at(1, 4, 1) }, { "type", Val("grace16") }, { "note", Val("A9") } }, "0-127");
}

// addresses: staff by part name, unknown keys, handles that went stale
TEST_F(Agent_WriteVoice, SharedAddresses)
{
    auto s = open("transposing-instruments.mscz");
    ValMap byPart;
    byPart["part"] = Val("horn in f");
    ValMap a;
    a["staff"] = Val(byPart);
    a["measure"] = Val(1);
    a["beat"] = Val(2);
    notes::Out out;
    Outcome o = runOp(s.get(), notes::tuplet, { { "at", Val(a) }, { "ratio", Val("3:2") } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 3, 1, 1), "m1 v1 | C4:q {3:2 D4:e r:e r:e} E4:q F4:q\n")) << print(s.get(), 3, 1, 1);
    const std::string handle = out["handle"].toString();
    o = runOp(s.get(), notes::grace, { { "at", Val(handle) }, { "type", Val("grace16") }, { "note", Val("C4") } });
    EXPECT_TRUE(o.ret) << o.ret.text();

    a["colour"] = Val("red");
    expectRefused(s.get(), notes::tuplet, { { "at", Val(a) }, { "ratio", Val("3:2") } }, "unknown key 'colour'");
    byPart["part"] = Val("Tuba");
    ValMap b;
    b["staff"] = Val(byPart);
    b["measure"] = Val(1);
    b["beat"] = Val(1);
    expectRefused(s.get(), notes::tuplet, { { "at", Val(b) }, { "ratio", Val("3:2") } }, "no part named");
    expectRefused(s.get(), notes::tuplet, { { "at", at(1, 1, 1.0001) }, { "ratio", Val("3:2") } }, "does not fall on a tick");
    expectRefused(s.get(), notes::tuplet, { { "at", at(1, 1, 9) }, { "ratio", Val("3:2") } }, "past its end");
    expectRefused(s.get(), notes::tuplet, { { "at", at(1, 1, 1, 5) }, { "ratio", Val("3:2") } }, "voice must be 1-4");
    ASSERT_TRUE(runWrite(s.get(), 3, "m1 v1 | C4:w").ret);
    expectRefused(s.get(), notes::grace, { { "at", Val(handle) }, { "type", Val("grace16") }, { "note", Val("C4") } }, "E_STALE_HANDLE");
}

// --- add_note_to_chord, remove_note, set_duration, exchange_voices (specs/notes.md)

namespace {
Val atOffset(int staff, int bar, const std::string& offset, int voice = 1)
{
    ValMap m;
    m["staff"] = Val(staff);
    m["measure"] = Val(bar);
    m["offset"] = Val(offset);
    m["voice"] = Val(voice);
    return Val(m);
}

Val staffList(std::initializer_list<int> staves)
{
    muse::ValList l;
    for (int n : staves) {
        l.emplace_back(Val(n));
    }
    return Val(l);
}

Params xv(Val staves, int from, int to, int a, int b)
{
    return { { "staves", staves }, { "fromMeasure", Val(from) }, { "toMeasure", Val(to) }, { "voiceA", Val(a) },
             { "voiceB", Val(b) } };
}

//! The first grace chord of bar `bar` on staff 1, voice 1
Chord* firstGrace(MasterScore* s, int bar)
{
    Measure* m = s->firstMeasure();
    for (int i = 1; i < bar; ++i) {
        m = m->nextMeasure();
    }
    for (Segment* seg = m->first(SegmentType::ChordRest); seg; seg = seg->next(SegmentType::ChordRest)) {
        EngravingItem* e = seg->element(0);
        if (e && e->isChord() && !toChord(e)->graceNotes().empty()) {
            return toChord(e)->graceNotes().front();
        }
    }
    return nullptr;
}

Score* excerptNamed(MasterScore* s, const std::string& name)
{
    for (Excerpt* ex : s->excerpts()) {
        if (ex->name().toStdString() == name) {
            return ex->excerptScore();
        }
    }
    return nullptr;
}

std::string printOf(const Score* score, int staff, int from, int to)
{
    read::ReadOptions opt;
    opt.fromBar = from;
    opt.toBar = to;
    opt.staves = { staff };
    return nt::write(read::printScore(score, opt));
}
}

// ANC-01: a third above a single note, spelled as asked; one undo entry
TEST_F(Agent_WriteVoice, AddNoteToChord)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | E4:h D4:q").ret);
    const size_t idx = s->undoStack()->currentIndex();
    notes::Out out;
    Outcome o = runOp(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 1) }, { "note", Val("G4") } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(s->undoStack()->currentIndex(), idx + 1);
    EXPECT_EQ(out["chords"].toInt(), 1);
    EXPECT_TRUE(has(out["handle"].toString(), "Note:")) << out["handle"].toString();
    o = runOp(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 3) }, { "note", Val("F#4") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | [E4,G4]:h [D4,F#4]:q\n")) << print(s.get(), 1, 2, 2);
    // Gb4 is the pitch of F#4: one key, one note
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 3) }, { "note", Val("Gb4") } }, "already has F#4");
    undo(s.get());
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | [E4,G4]:h D4:q\n")) << print(s.get(), 1, 2, 2);
}

// ANC-02: added to a chord in a tie chain, every chord of the chain gets it, tied; or only the target
TEST_F(Agent_WriteVoice, AddNoteToChordThroughTies)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m3 v1 | C5:h~ C5:q~\nm4 v1 | C5:h.").ret);
    notes::Out out;
    Outcome o = runOp(s.get(), notes::addNoteToChord, { { "at", at(1, 3, 3) }, { "note", Val("E5") } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(out["chords"].toInt(), 3);
    EXPECT_EQ(out["bars"].toString(), "3-4");
    EXPECT_TRUE(has(print(s.get(), 1, 3, 4), "m3(2) v1 | [C5,E5]:h~ [C5,E5]:q~\nm4(3) v1 | [C5,E5]:h.\n")) << print(s.get(), 1, 3, 4);
    o = runOp(s.get(), notes::addNoteToChord, { { "at", at(1, 4, 1) }, { "note", Val("G5") }, { "extendThroughTies", Val(false) } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 3, 4), "m3(2) v1 | [C5,E5]:h~ [C5,E5]:q~\nm4(3) v1 | [C5,E5,G5]:h.\n")) << print(s.get(), 1, 3, 4);
    // the chain already has G5 in its last chord
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 3, 1) }, { "note", Val("G5") } }, "tied to the target");
    undo(s.get());
    undo(s.get());
    EXPECT_TRUE(has(print(s.get(), 1, 3, 4), "m3(2) v1 | C5:h~ C5:q~\nm4(3) v1 | C5:h.\n")) << print(s.get(), 1, 3, 4);
}

// ANC-03/04 and the other refusals: nothing changes
TEST_F(Agent_WriteVoice, AddNoteToChordRefusals)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | E4:h r:q").ret);
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 1) }, { "note", Val("E4") } }, "E_EXISTS");
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 1) }, { "note", Val("Fb4") } }, "already has E4");
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 3) }, { "note", Val("C5") } }, "is a rest");
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 1) }, { "note", Val("H4") } }, "note must be");
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 1) } }, "note must be");
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 1) }, { "note", Val("A9") } }, "0-127");
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 2) }, { "note", Val("C5") } }, "no note or rest starts");
    expectRefused(s.get(), notes::addNoteToChord, { { "note", Val("C5") } }, "missing at");
}

// ANC: a note added in the master reaches the linked parts
TEST_F(Agent_WriteVoice, AddNoteToChordReachesParts)
{
    auto s = open("parts-linked.mscz");
    ASSERT_EQ(s->excerpts().size(), 3u);
    Score* flute = excerptNamed(s.get(), "Flute");
    ASSERT_TRUE(flute);
    ValMap staff;
    staff["part"] = Val("Flute");
    ValMap a;
    a["staff"] = Val(staff);
    a["measure"] = Val(4);
    a["beat"] = Val(1);
    Outcome o = runOp(s.get(), notes::addNoteToChord, { { "at", Val(a) }, { "note", Val("D5") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 4, 4), "m4 v1 | [B4,D5]:w")) << print(s.get(), 1, 4, 4);
    EXPECT_TRUE(has(printOf(flute, 1, 4, 4), "m4 v1 | [B4,D5]:w")) << printOf(flute, 1, 4, 4);
    undo(s.get());
    EXPECT_TRUE(has(printOf(flute, 1, 4, 4), "m4 v1 | B4:w")) << printOf(flute, 1, 4, 4);
}

// RN-01/02/03: a note of a chord; the only note (inside a triplet) becomes a rest; a tied note
TEST_F(Agent_WriteVoice, RemoveNote)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | [C4,E4,G4]:h D4:q\nm3 v1 | {3:2 C5:e D5:e E5:e} F5:h\nm4 v1 | A4:h~ A4:q").ret);
    notes::Out out;
    Outcome o = runOp(s.get(), notes::removeNote, { { "at", at(1, 2, 1) }, { "note", Val("E4") } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_FALSE(out["leftRest"].toBool());
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | [C4,G4]:h D4:q\n")) << print(s.get(), 1, 2, 2);
    // by the chord's handle plus a pitch
    o = runOp(s.get(), notes::removeNote, { { "at", out["handle"] }, { "note", Val("G4") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | C4:h D4:q\n")) << print(s.get(), 1, 2, 2);

    o = runOp(s.get(), notes::removeNote, { { "at", atOffset(1, 3, "1/12") } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(out["leftRest"].toBool());
    EXPECT_TRUE(has(out["handle"].toString(), "Rest:")) << out["handle"].toString();
    EXPECT_TRUE(has(print(s.get(), 1, 3, 3), "m3(2) v1 | {3:2 C5:e r:e E5:e} F5:h\n")) << print(s.get(), 1, 3, 3);

    o = runOp(s.get(), notes::removeNote, { { "at", at(1, 4, 3) } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 4, 4), "m4(3) v1 | A4:h r:q\n")) << print(s.get(), 1, 4, 4);
    undo(s.get());
    EXPECT_TRUE(has(print(s.get(), 1, 4, 4), "m4(3) v1 | A4:h~ A4:q\n")) << print(s.get(), 1, 4, 4);
}

// RN: a grace note by its note handle; the refusals
TEST_F(Agent_WriteVoice, RemoveNoteGraceAndRefusals)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | g(D5:s) [C5,E5]:h r:q\nm3 v1 | G4:h. ").ret);
    Chord* g = firstGrace(s.get(), 2);
    ASSERT_TRUE(g);
    const Val noteHandle(core::makeHandle(g->notes().front()));
    expectRefused(s.get(), notes::removeNote, { { "at", noteHandle }, { "note", Val("D5") } }, "leave note out");
    expectRefused(s.get(), notes::removeNote, { { "at", at(1, 2, 1) } }, "say which");
    expectRefused(s.get(), notes::removeNote, { { "at", at(1, 2, 1) }, { "note", Val("D5") } }, "has no D5");
    expectRefused(s.get(), notes::removeNote, { { "at", at(1, 2, 1) }, { "note", Val("Fb5") } }, "spelling counts");
    expectRefused(s.get(), notes::removeNote, { { "at", at(1, 2, 3) } }, "is a rest");
    expectRefused(s.get(), notes::removeNote, { { "at", at(1, 3, 1) }, { "whenLastNote", Val("error") } }, "only note");
    expectRefused(s.get(), notes::removeNote, { { "at", at(1, 3, 1) }, { "whenLastNote", Val("keep") } }, "rest or error");
    expectRefused(s.get(), notes::removeNote, { { "at", at(1, 3, 1) }, { "note", Val("H4") } }, "note must be");

    notes::Out out;
    Outcome o = runOp(s.get(), notes::removeNote, { { "at", noteHandle } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_FALSE(out["leftRest"].toBool());
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | [C5,E5]:h r:q\n")) << print(s.get(), 1, 2, 2);
    undo(s.get());
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | g(D5:s) [C5,E5]:h r:q\n")) << print(s.get(), 1, 2, 2);
}

// SD-01/02/03: shorter fills with rests; longer needs allowOverwrite over notes; across the bar line ties
TEST_F(Agent_WriteVoice, SetDuration)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | C5:q D5:e E5:e F5:q\nm3 v1 | G5:h A5:q").ret);
    Outcome o = runOp(s.get(), notes::setDuration, { { "at", at(1, 3, 1) }, { "duration", Val("q") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 3, 3), "m3(2) v1 | G5:q r:q A5:q\n")) << print(s.get(), 1, 3, 3);

    expectRefused(s.get(), notes::setDuration, { { "at", at(1, 2, 1) }, { "duration", Val("q.") } }, "allowOverwrite");
    o = runOp(s.get(), notes::setDuration, { { "at", at(1, 2, 1) }, { "duration", Val("q.") }, { "allowOverwrite", Val(true) } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | C5:q. E5:e F5:q\n")) << print(s.get(), 1, 2, 2);

    notes::Out out;
    o = runOp(s.get(), notes::setDuration, { { "at", at(1, 2, 3) }, { "duration", Val("h") }, { "allowOverwrite", Val(true) } }, &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(out["bars"].toString(), "2-3");
    EXPECT_TRUE(has(print(s.get(), 1, 2, 3), "m2(1) v1 | C5:q. E5:e F5:q~\nm3(2) v1 | F5:q r:q A5:q\n")) << print(s.get(), 1, 2, 3);
    undo(s.get());
    EXPECT_TRUE(has(print(s.get(), 1, 2, 3), "m2(1) v1 | C5:q. E5:e F5:q\nm3(2) v1 | G5:q r:q A5:q\n")) << print(s.get(), 1, 2, 3);

    // a rest grows over rests without allowOverwrite
    expectRefused(s.get(), notes::setDuration, { { "at", at(1, 3, 2) }, { "duration", Val("h") } }, "allowOverwrite"); // A5
    ASSERT_TRUE(runWrite(s.get(), 1, "m4 v1 | r:q r:q B4:q").ret);
    o = runOp(s.get(), notes::setDuration, { { "at", at(1, 4, 1) }, { "duration", Val("h") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 4, 4), "m4(3) v1 | r:h B4:q\n")) << print(s.get(), 1, 4, 4);
}

// SD-04/05 and the other refusals: tuplets, the end of the score, a grace note's value
TEST_F(Agent_WriteVoice, SetDurationTupletsGraceAndRefusals)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | {3:2 C5:e D5:e E5:e} F5:h\nm3 v1 | C5:q {3:2 D5:e E5:e F5:e} G5:q\n"
                                      "m4 v1 | g(D5:s) C5:h.").ret);
    expectRefused(s.get(), notes::setDuration, { { "at", at(1, 2, 1) }, { "duration", Val("h") }, { "allowOverwrite", Val(true) } },
                  "tuplet's end");
    expectRefused(s.get(), notes::setDuration, { { "at", at(1, 3, 1) }, { "duration", Val("q.") }, { "allowOverwrite", Val(true) } },
                  "inside the tuplet");
    expectRefused(s.get(), notes::setDuration, { { "at", at(1, 2, 1) }, { "duration", Val("z") } }, "duration must be");
    expectRefused(s.get(), notes::setDuration, { { "at", at(1, 2, 1) } }, "duration must be");
    expectRefused(s.get(), notes::setDuration, { { "at", atOffset(1, 2, "1/24") }, { "duration", Val("q") } }, "no note or rest starts");
    expectRefused(s.get(), notes::setDuration, { { "at", at(1, 2, 1) }, { "duration", Val("q~") } }, "duration must be");

    Outcome o = runOp(s.get(), notes::setDuration, { { "at", at(1, 2, 1) }, { "duration", Val("q") }, { "allowOverwrite", Val(true) } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 2, 2), "m2(1) v1 | {3:2 C5:q E5:e} F5:h\n")) << print(s.get(), 1, 2, 2);

    Chord* g = firstGrace(s.get(), 4);
    ASSERT_TRUE(g);
    o = runOp(s.get(), notes::setDuration, { { "at", Val(core::makeHandle(g)) }, { "duration", Val("e") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 4, 4), "m4(3) v1 | g(D5:e) C5:h.\n")) << print(s.get(), 1, 4, 4);

    // the last bar (2/4) is the end of the score
    const int last = static_cast<int>(address::allMeasures(s.get()).size());
    ASSERT_TRUE(runWrite(s.get(), 1, "m" + std::to_string(last) + " v1 | C5:q D5:q").ret);
    expectRefused(s.get(), notes::setDuration, { { "at", at(1, last, 2) }, { "duration", Val("h") } }, "past the end of the score");
}

// XV-01/02: swap voices 1 and 2; an empty voice 2 leaves rests in voice 1; one undo restores both
TEST_F(Agent_WriteVoice, ExchangeVoices)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | C5:h D5:q\nm2 v2 | E4:q F4:q G4:q\nm3 v1 | G5:h A5:q").ret);
    const std::string before = print(s.get(), 1, 2, 3);
    EXPECT_FALSE(has(before, "m3(2) v2 |")) << before;
    const size_t idx = s->undoStack()->currentIndex();
    notes::Out out;
    Outcome o = runOp(s.get(), notes::exchangeVoices, xv(staffList({ 1 }), 2, 3, 1, 2), &out);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(s->undoStack()->currentIndex(), idx + 1);
    EXPECT_EQ(out["bars"].toString(), "2-3");
    const std::string after = print(s.get(), 1, 2, 3);
    EXPECT_TRUE(has(after, "m2(1) v1 | E4:q F4:q G4:q\n")) << after;
    EXPECT_TRUE(has(after, "m2(1) v2 | C5:h D5:q\n")) << after;
    EXPECT_TRUE(has(after, "m3(2) v2 | G5:h A5:q\n")) << after;
    // voice 1 of m3 is rests only (MuseScore fills it, edit.cpp:6428-6456)
    const size_t at3 = after.find("m3(2) v1 | ");
    ASSERT_NE(at3, std::string::npos) << after;
    const std::string line3 = after.substr(at3, after.find('\n', at3) - at3);
    EXPECT_EQ(line3.find_first_of("ABCDEFG"), std::string::npos) << line3;
    EXPECT_TRUE(has(o.ret ? out["text"].toString() : "", "m2(1) v2 | C5:h D5:q")) << out["text"].toString();
    undo(s.get());
    EXPECT_EQ(print(s.get(), 1, 2, 3), before);
}

TEST_F(Agent_WriteVoice, ExchangeVoicesRefusals)
{
    auto s = open("transposing-instruments.mscz");
    expectRefused(s.get(), notes::exchangeVoices, xv(staffList({ 1 }), 1, 1, 2, 2), "two different voices");
    expectRefused(s.get(), notes::exchangeVoices, xv(staffList({ 1 }), 2, 1, 1, 2), "before fromMeasure");
    expectRefused(s.get(), notes::exchangeVoices, xv(staffList({ 1, 3 }), 1, 1, 1, 2), "next to each other");
    expectRefused(s.get(), notes::exchangeVoices, xv(staffList({ 1 }), 1, 1, 1, 5), "voiceB 5 is outside 1-4");
    expectRefused(s.get(), notes::exchangeVoices, xv(staffList({ 1 }), 1, 99, 1, 2), "toMeasure 99 is outside");
    expectRefused(s.get(), notes::exchangeVoices, xv(staffList({ 99 }), 1, 1, 1, 2), "staff 99 is outside");
    expectRefused(s.get(), notes::exchangeVoices, xv(Val(muse::ValList()), 1, 1, 1, 2), "at least one staff");
    expectRefused(s.get(), notes::exchangeVoices, { { "staves", staffList({ 1 }) }, { "fromMeasure", Val(1) } }, "missing toMeasure");
}

// XV-03: in a score with linked parts the parts follow
TEST_F(Agent_WriteVoice, ExchangeVoicesReachesParts)
{
    auto s = open("parts-linked.mscz");
    Score* flute = excerptNamed(s.get(), "Flute");
    ASSERT_TRUE(flute);
    ASSERT_TRUE(runWrite(s.get(), 1, "m4 v1 | B4:w\nm4 v2 | G4:h G4:h").ret);
    Outcome o = runOp(s.get(), notes::exchangeVoices, xv(staffList({ 1 }), 4, 4, 1, 2));
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(s.get(), 1, 4, 4), "m4 v1 | G4:h G4:h\n")) << print(s.get(), 1, 4, 4);
    EXPECT_TRUE(has(printOf(flute, 1, 4, 4), "m4 v1 | G4:h G4:h\n")) << printOf(flute, 1, 4, 4);
    EXPECT_TRUE(has(printOf(flute, 1, 4, 4), "m4 v2 | B4:w\n")) << printOf(flute, 1, 4, 4);
}

// --- review follow-ups: the remaining refusals, forceAccidental, parts and redo

namespace {
Chord* chordAt(MasterScore* s, int bar, int beat, int staff = 1)
{
    Measure* m = address::allMeasures(s).at(bar - 1);
    ChordRest* cr = nullptr;
    const Fraction tick = m->tick() + Fraction::fromTicks((beat - 1) * Constants::DIVISION);
    if (!address::chordRestAt(s, m, tick, (staff - 1) * VOICES, cr) || !cr->isChord()) {
        return nullptr;
    }
    return toChord(cr);
}

void redo(MasterScore* score)
{
    score->undoRedo(false, nullptr);
}

//! Bar `bar` of staff 1 becomes a one-bar measure repeat (it is emptied first, so MuseScore asks nothing)
void makeMeasureRepeat(MasterScore* s, int bar)
{
    ASSERT_TRUE(runWrite(s, 1, "m" + std::to_string(bar) + " v1 | R").ret);
    s->startCmd(muse::TranslatableString::untranslatable("test: measure repeat"));
    s->cmdAddMeasureRepeat(address::allMeasures(s).at(bar - 1), 1, 0);
    s->endCmd();
}
}

// forceAccidental: the accidental shows in both pitch modes (needs both spellings in the NoteVal)
TEST_F(Agent_WriteVoice, AddNoteToChordForceAccidental)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | E4:h D4:q").ret);
    Outcome o = runOp(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 1) }, { "note", Val("G4") }, { "forceAccidental", Val(true) } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    const Note* n = chordAt(s.get(), 2, 1)->findNote(67);
    ASSERT_TRUE(n);
    ASSERT_TRUE(n->accidental());
    EXPECT_EQ(n->accidental()->accidentalType(), AccidentalType::NATURAL);

    auto t = open("transposing-instruments.mscz");
    o = runOp(t.get(), notes::addNoteToChord, { { "at", at(2, 1, 1) }, { "note", Val("F#4") }, { "pitch", Val("written") },
                  { "forceAccidental", Val(true) } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(print(t.get(), 2, 1, 1, true), "m1 v1 | [D4,F#4]:q")) << print(t.get(), 2, 1, 1, true);
    const Note* w = chordAt(t.get(), 1, 1, 2)->findNote(64); // written F#4 on a B-flat staff sounds E4
    ASSERT_TRUE(w);
    ASSERT_TRUE(w->accidental());
    EXPECT_EQ(w->accidental()->accidentalType(), AccidentalType::SHARP);
    undo(t.get());
    redo(t.get());
    EXPECT_TRUE(has(print(t.get(), 2, 1, 1, true), "m1 v1 | [D4,F#4]:q")) << print(t.get(), 2, 1, 1, true);
}

// trill cue notes, tablature and percussion staves, generated notes, stale handles
TEST_F(Agent_WriteVoice, NoteEditsRefuseStavesAndSpecialNotes)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | [C5,E5]:h D5:q").ret);
    Chord* c = chordAt(s.get(), 2, 1);
    ASSERT_TRUE(c);
    c->setIsTrillCueNote(true);
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 1) }, { "note", Val("G5") } }, "trill");
    expectRefused(s.get(), notes::removeNote, { { "at", at(1, 2, 1) }, { "note", Val("C5") } }, "trill");
    expectRefused(s.get(), notes::setDuration, { { "at", at(1, 2, 1) }, { "duration", Val("q") } }, "trill");
    c->setIsTrillCueNote(false);

    Note* gen = c->findNote(72);
    ASSERT_TRUE(gen);
    gen->setGenerated(true);
    expectRefused(s.get(), notes::removeNote, { { "at", at(1, 2, 1) }, { "note", Val("C5") } }, "generated");
    gen->setGenerated(false);

    const Val handle(core::makeHandle(c->findNote(76)));
    ASSERT_TRUE(runOp(s.get(), notes::removeNote, { { "at", handle } }).ret);
    expectRefused(s.get(), notes::removeNote, { { "at", handle } }, "E_STALE_HANDLE");

    const StaffType* original = s->staff(0)->staffType(Fraction(0, 1));
    const StaffType saved = *original;
    s->staff(0)->setStaffType(Fraction(0, 1), *StaffType::preset(StaffTypes::TAB_6SIMPLE));
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 1) }, { "note", Val("G5") } }, "tablature");
    expectRefused(s.get(), notes::removeNote, { { "at", at(1, 2, 1) } }, "tablature");
    s->staff(0)->setStaffType(Fraction(0, 1), saved);
    // a percussion staff is one whose instrument uses a drumset (Staff::isDrumStaff, staff.cpp:1879)
    s->staff(0)->part()->instrument()->setUseDrumset(true);
    expectRefused(s.get(), notes::addNoteToChord, { { "at", at(1, 2, 1) }, { "note", Val("G5") } }, "percussion");
    s->staff(0)->part()->instrument()->setUseDrumset(false);
}

// measure repeats: set_duration and exchange_voices refuse them, also when a value would grow over one
TEST_F(Agent_WriteVoice, NoteEditsRefuseMeasureRepeats)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m4 v1 | B4:h A4:q").ret);
    makeMeasureRepeat(s.get(), 5);
    expectRefused(s.get(), notes::setDuration, { { "at", at(1, 5, 1) }, { "duration", Val("q") } }, "measure repeat");
    expectRefused(s.get(), notes::setDuration, { { "at", at(1, 4, 3) }, { "duration", Val("h") }, { "allowOverwrite", Val(true) } },
                  "overwrite the measure repeat");
    expectRefused(s.get(), notes::exchangeVoices, xv(staffList({ 1 }), 4, 5, 1, 2), "measure repeat");
}

TEST_F(Agent_WriteVoice, ExchangeVoicesMoreRefusals)
{
    auto s = open("transposing-instruments.mscz");
    expectRefused(s.get(), notes::exchangeVoices, xv(Val(1), 1, 1, 1, 2), "at least one staff");
    expectRefused(s.get(), notes::exchangeVoices, xv(staffList({ 1 }), 99, 99, 1, 2), "fromMeasure 99 is outside");
    expectRefused(s.get(), notes::exchangeVoices, xv(staffList({ 1 }), 1, 1, 0, 2), "voiceA 0 is outside 1-4");
    muse::ValList half;
    half.emplace_back(Val(1.5));
    expectRefused(s.get(), notes::exchangeVoices, xv(Val(half), 1, 1, 1, 2), "whole number");
    Params p = xv(staffList({ 1 }), 1, 1, 1, 2);
    p["toMeasure"] = Val(1.5);
    expectRefused(s.get(), notes::exchangeVoices, p, "toMeasure must be a whole number");
}

// RN-04 and set_duration in a score with linked parts: the part follows, and undo/redo too
TEST_F(Agent_WriteVoice, RemoveNoteAndSetDurationReachParts)
{
    auto s = open("parts-linked.mscz");
    Score* flute = excerptNamed(s.get(), "Flute");
    ASSERT_TRUE(flute);
    ASSERT_TRUE(runWrite(s.get(), 1, "m4 v1 | [B4,D5]:h C5:h").ret);
    EXPECT_TRUE(has(printOf(flute, 1, 4, 4), "m4 v1 | [B4,D5]:h C5:h\n")) << printOf(flute, 1, 4, 4);
    Outcome o = runOp(s.get(), notes::removeNote, { { "at", at(1, 4, 1) }, { "note", Val("D5") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(printOf(flute, 1, 4, 4), "m4 v1 | B4:h C5:h\n")) << printOf(flute, 1, 4, 4);
    o = runOp(s.get(), notes::setDuration, { { "at", at(1, 4, 3) }, { "duration", Val("q") } });
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(has(printOf(flute, 1, 4, 4), "m4 v1 | B4:h C5:q r:q\n")) << printOf(flute, 1, 4, 4);
    undo(s.get());
    EXPECT_TRUE(has(printOf(flute, 1, 4, 4), "m4 v1 | B4:h C5:h\n")) << printOf(flute, 1, 4, 4);
    undo(s.get());
    EXPECT_TRUE(has(printOf(flute, 1, 4, 4), "m4 v1 | [B4,D5]:h C5:h\n")) << printOf(flute, 1, 4, 4);
    redo(s.get());
    redo(s.get());
    EXPECT_TRUE(has(printOf(flute, 1, 4, 4), "m4 v1 | B4:h C5:q r:q\n")) << printOf(flute, 1, 4, 4);
    EXPECT_TRUE(has(print(s.get(), 1, 4, 4), "m4 v1 | B4:h C5:q r:q\n")) << print(s.get(), 1, 4, 4);
}

// redo after undo gives the same bars, for each of the four
TEST_F(Agent_WriteVoice, NoteEditsRedo)
{
    auto s = open("pickup-bar.mscz");
    ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | E4:h D4:q\nm2 v2 | C4:q C4:q C4:q\nm3 v1 | C5:h~ C5:q~\nm4 v1 | C5:h.").ret);
    struct Step {
        muse::Ret (*op)(MasterScore*, const Params&, notes::Out&);
        Params p;
    };
    const std::vector<Step> steps = {
        { notes::addNoteToChord, { { "at", at(1, 3, 1) }, { "note", Val("E5") } } },
        { notes::removeNote, { { "at", at(1, 2, 3) } } },
        { notes::setDuration, { { "at", at(1, 2, 1) }, { "duration", Val("q") } } },
        { notes::exchangeVoices, xv(staffList({ 1 }), 2, 2, 1, 2) },
    };
    for (const Step& st : steps) {
        const std::string before = print(s.get(), 1, 2, 4);
        Outcome o = runOp(s.get(), st.op, st.p);
        ASSERT_TRUE(o.ret) << o.ret.text();
        const std::string after = print(s.get(), 1, 2, 4);
        EXPECT_NE(after, before);
        undo(s.get());
        EXPECT_EQ(print(s.get(), 1, 2, 4), before);
        redo(s.get());
        EXPECT_EQ(print(s.get(), 1, 2, 4), after);
    }
}

// S14: a beam's own properties (here its stem direction) should come back on undo of write_voice and
// of the UI's range delete. Upstream bug, not fixed yet: Score::removeElement takes the chord out of
// its beam (score.cpp:1686-1689) and undo does not put it back, so layout makes a new default beam.
// A fix must keep the old beam's lifetime safe (beams are deleted directly in several places), see
// DECISIONS 2026-10-30. Enable when fixed.
TEST_F(Agent_WriteVoice, DISABLED_UndoKeepsBeamProperties)
{
    for (int path = 0; path < 2; ++path) {
        SCOPED_TRACE(path == 0 ? "write_voice" : "deleteRange");
        auto s = open("pickup-bar.mscz");
        ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | C5:e D5:e E5:q F5:q").ret);
        s->doLayout();
        Beam* b = chordAt(s.get(), 2, 1)->beam();
        ASSERT_TRUE(b);
        s->startCmd(muse::TranslatableString::untranslatable("test: beam up"));
        b->undoChangeProperty(Pid::STEM_DIRECTION, PropertyValue::fromValue(DirectionV::DOWN));
        s->endCmd();
        ASSERT_EQ(chordAt(s.get(), 2, 1)->beam()->getProperty(Pid::STEM_DIRECTION).value<DirectionV>(), DirectionV::DOWN);
        if (path == 0) {
            ASSERT_TRUE(runWrite(s.get(), 1, "m2 v1 | G4:q A4:q B4:q").ret);
        } else {
            Measure* m = address::allMeasures(s.get()).at(1);
            s->startCmd(muse::TranslatableString::untranslatable("test: delete"));
            s->deleteRange(m->first(SegmentType::ChordRest), m->nextMeasure()->first(SegmentType::ChordRest), 0, VOICES, SelectionFilter(), false);
            s->endCmd();
        }
        undo(s.get());
        s->doLayout();
        Beam* after = chordAt(s.get(), 2, 1)->beam();
        ASSERT_TRUE(after);
        EXPECT_EQ(after->getProperty(Pid::STEM_DIRECTION).value<DirectionV>(), DirectionV::DOWN);
        EXPECT_EQ(after, b);
    }
}
