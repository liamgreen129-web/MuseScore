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

#include "engraving/dom/masterscore.h"
#include "engraving/dom/mscore.h"
#include "engraving/editing/undo.h"
#include "engraving/tests/utils/scorerw.h"

#include "commands/notes/clearrange.h"
#include "commands/notes/writevoice.h"
#include "engraving/dom/selectionfilter.h"
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
