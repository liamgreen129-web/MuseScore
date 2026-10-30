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
// P2.4 pitch: set_pitch and transpose. Each operation runs inside a core Transaction on a
// synthetic fixture with the score's own undo stack (as expression_marks_tests.cpp), so every
// validation error is checked to leave the saved score and the undo stack untouched, and every
// edit is undone and redone.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "engraving/dom/chord.h"
#include "engraving/dom/drumset.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/fret.h"
#include "engraving/dom/guitarbend.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/interval.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/selectionfilter.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftype.h"
#include "engraving/dom/stringdata.h"
#include "engraving/dom/tie.h"
#include "engraving/editing/transpose.h"
#include "engraving/editing/undo.h"
#include "engraving/tests/utils/scorerw.h"

#include "commands/pitch/pitchops.h"
#include "core/handles.h"
#include "core/transaction.h"
#include "core/undobackend.h"
#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::agent::core;
using namespace mu::engraving;
using muse::Ret;
using muse::Val;
using muse::ValList;
using muse::ValMap;
namespace pt = mu::agent::pitch;
namespace fs = std::filesystem;
using AErr = mu::agent::Err;

namespace {
using Op = std::function<Ret(const Params&, MasterScore*, pt::Out&)>;

const Op SET = pt::setPitch;
const Op TR = pt::transpose;

MasterScore* load(const char* name)
{
    const fs::path p = fs::path(agent_tests_DATA_ROOT) / "../../../tests/fixtures/synthetic" / name;
    return ScoreRW::readScore(muse::String::fromStdString(p.string()), true);
}

Measure* bar(MasterScore* score, int n)
{
    int i = 0;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        if (++i == n) {
            return m;
        }
    }
    return nullptr;
}

//! The chord or rest at bar/beat (1-based quarter beats) on a staff (1-based) and voice (1-based)
ChordRest* crAt(MasterScore* score, int staff, int measure, double beat, int voice = 1)
{
    Measure* m = bar(score, measure);
    if (!m) {
        return nullptr;
    }
    const Fraction tick = m->tick() + Fraction::fromTicks(static_cast<int>((beat - 1.0) * Constants::DIVISION));
    Segment* s = m->findSegment(SegmentType::ChordRest, tick);
    if (!s) {
        return nullptr;
    }
    EngravingItem* e = s->element(static_cast<track_idx_t>((staff - 1) * VOICES + (voice - 1)));
    return e && e->isChordRest() ? toChordRest(e) : nullptr;
}

Note* noteAt(MasterScore* score, int staff, int measure, double beat, int index = 0, int voice = 1)
{
    ChordRest* cr = crAt(score, staff, measure, beat, voice);
    if (!cr || !cr->isChord() || static_cast<int>(toChord(cr)->notes().size()) <= index) {
        return nullptr;
    }
    return toChord(cr)->notes().at(static_cast<size_t>(index));
}

Val where(int staff, int measure, double beat, int voice = 0)
{
    ValMap m;
    m["staff"] = Val(staff);
    m["measure"] = Val(measure);
    m["beat"] = Val(beat);
    if (voice) {
        m["voice"] = Val(voice);
    }
    return Val(m);
}

Val withKey(Val v, const std::string& k, const Val& x)
{
    ValMap m = v.toMap();
    m[k] = x;
    return Val(m);
}

//! set_pitch arguments. A where() map is spread over the command's arguments (staff, at, voice;
//! note / note_index / grace; anything else goes into at, to test its key check); a string target
//! is a handle. A string pitch is `pitch`, a number is `midi`, anything else goes in `pitch`.
Params setArgs(const Val& target, const Val& pitch)
{
    Params p;
    if (target.type() == Val::Type::Map) {
        ValMap at;
        for (const auto& [k, v] : target.toMap()) {
            if (k == "staff") {
                ValMap s;
                s["number"] = v;
                p["staff"] = Val(s);
            } else if (k == "voice" || k == "grace") {
                p[k] = v;
            } else if (k == "note") {
                p[v.type() == Val::Type::Int || v.type() == Val::Type::Int64 || v.type() == Val::Type::Double ? "note_index" : "note"] = v;
            } else {
                at[k] = v;
            }
        }
        p["at"] = Val(at);
    } else {
        p["target"] = target;
    }
    const bool number = pitch.type() == Val::Type::Int || pitch.type() == Val::Type::Int64 || pitch.type() == Val::Type::Double;
    p[number ? "midi" : "pitch"] = pitch;
    return p;
}

Params with(Params p, const std::string& k, const Val& v)
{
    p[k] = v;
    return p;
}

Val pos(int measure, double beat = 0)
{
    ValMap m;
    m["measure"] = Val(measure);
    if (beat > 0) {
        m["beat"] = Val(beat);
    }
    return Val(m);
}

Val rangeOf(const Val& start, const Val& end, const std::vector<int>& staves = {})
{
    ValMap r;
    r["start"] = start;
    r["end"] = end;
    if (!staves.empty()) {
        ValList l;
        for (int s : staves) {
            l.emplace_back(Val(s));
        }
        r["staves"] = Val(l);
    }
    return Val(r);
}

Val handles(const std::vector<std::string>& hs)
{
    ValList l;
    for (const std::string& h : hs) {
        l.emplace_back(Val(h));
    }
    ValMap r;
    r["handles"] = Val(l);
    return Val(r);
}

Val interval(const std::string& name, const std::string& dir, int octaves = -1)
{
    ValMap m;
    m["interval"] = Val(name);
    m["direction"] = Val(dir);
    if (octaves >= 0) {
        m["octaves"] = Val(octaves);
    }
    return Val(m);
}

Val toKey(int key, const std::string& dir = "")
{
    ValMap m;
    m["key"] = Val(key);
    if (!dir.empty()) {
        m["direction"] = Val(dir);
    }
    return Val(m);
}

Val degrees(int n, const std::string& dir, int keep = -1)
{
    ValMap m;
    m["degrees"] = Val(n);
    m["direction"] = Val(dir);
    if (keep >= 0) {
        m["keep_alterations"] = Val(keep == 1);
    }
    return Val(m);
}

Params trArgs(const Val& range, const Val& by)
{
    Params p;
    p["range"] = range;
    p["by"] = by;
    return p;
}

Interval ivl(const std::string& name)
{
    const int i = pt::intervalIndex(name);
    return i < 0 ? Interval(99, 99) : Interval::allIntervals[static_cast<size_t>(i)];
}

Val scoreRange()
{
    ValMap r;
    r["score"] = Val(true);
    return Val(r);
}

Harmony* firstHarmony(MasterScore* score)
{
    for (Segment* s = score->firstSegment(SegmentType::ChordRest); s; s = s->next1(SegmentType::ChordRest)) {
        for (EngravingItem* a : s->annotations()) {
            if (a->isHarmony()) {
                return toHarmony(a);
            }
        }
    }
    return nullptr;
}

std::vector<int> pitchesOf(MasterScore* score, int staff, int measure, const std::vector<double>& beats)
{
    std::vector<int> out;
    for (double b : beats) {
        Note* n = noteAt(score, staff, measure, b);
        out.push_back(n ? n->pitch() : -1);
    }
    return out;
}

class Agent_Pitch : public ::testing::Test
{
protected:
    void open(const char* name)
    {
        score.reset(load(name));
        ASSERT_TRUE(score);
        backend = std::make_unique<ScoreUndoBackend>(score.get());
        MScore::setError(MsError::MS_NO_ERROR);
    }

    void SetUp() override { open("transposing-instruments.mscz"); }

    Ret run(const Op& op, const Params& p)
    {
        out.clear();
        Transaction tx(score.get(), *backend);
        if (Ret r = tx.begin("Agent: test"); !r) {
            return r;
        }
        if (Ret r = tx.step("op", [&] { return op(p, score.get(), out); }); !r) {
            return r;
        }
        Ret r = tx.commit();
        changed = tx.changed();
        warnings = tx.warnings();
        return r;
    }

    std::string snapshot()
    {
        const fs::path f = fs::temp_directory_path() / "agent_pitch_snapshot.mscx";
        EXPECT_TRUE(ScoreRW::saveScore(score.get(), muse::String::fromStdString(f.string())));
        std::ifstream in(f, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        fs::remove(f);
        return s;
    }

    //! The op must fail with `e`, leave no undo entry and leave the saved score byte-identical
    void refused(const Op& op, const Params& p, AErr e, const std::string& contains = {})
    {
        const std::string saved = snapshot();
        const size_t before = backend->currentIndex();
        const Ret r = run(op, p);
        EXPECT_EQ(snapshot(), saved) << "a refused request changed the score: " << r.text();
        EXPECT_FALSE(r) << "expected " << errName(e);
        EXPECT_EQ(r.code(), static_cast<int>(e)) << r.text();
        if (!contains.empty()) {
            EXPECT_NE(r.text().find(contains), std::string::npos) << r.text();
        }
        EXPECT_EQ(backend->currentIndex(), before);
        EXPECT_FALSE(score->undoStack()->hasActiveCommand());
    }

    //! The op must succeed with one undo entry; undo then redo
    void addsOneEntry(const Op& op, const Params& p, const std::function<void()>& checkDone, const std::function<void()>& checkUndone)
    {
        const size_t before = backend->currentIndex();
        const Ret r = run(op, p);
        ASSERT_TRUE(r) << r.text();
        EXPECT_TRUE(changed);
        EXPECT_EQ(backend->currentIndex(), before + 1);
        checkDone();
        score->undoRedo(true, nullptr);
        checkUndone();
        score->undoRedo(false, nullptr);
        checkDone();
    }

    std::string outStr(const char* k) const { return out.count(k) ? out.at(k).toString() : std::string(); }
    int outInt(const char* k) const { return out.count(k) ? out.at(k).toInt() : -999; }

    std::unique_ptr<MasterScore> score;
    std::unique_ptr<ScoreUndoBackend> backend;
    pt::Out out;
    bool changed = false;
    std::vector<std::string> warnings;
};
}

// ================================================================== helpers

TEST_F(Agent_Pitch, IntervalNamesMatchAllIntervals)
{
    const std::vector<std::string>& n = pt::intervalNames();
    ASSERT_EQ(n.size(), Interval::allIntervals.size());
    EXPECT_EQ(pt::intervalIndex("P1"), 0);
    EXPECT_EQ(ivl("A1"), Interval(0, 1));
    EXPECT_EQ(ivl("d2"), Interval(1, 0));
    EXPECT_EQ(ivl("M2"), Interval(1, 2));
    EXPECT_EQ(ivl("m3"), Interval(2, 3));
    EXPECT_EQ(ivl("P4"), Interval(3, 5));
    EXPECT_EQ(ivl("A4"), Interval(3, 6));
    EXPECT_EQ(ivl("d5"), Interval(4, 6));
    EXPECT_EQ(ivl("P5"), Interval(4, 7));
    EXPECT_EQ(ivl("M6"), Interval(5, 9));
    EXPECT_EQ(ivl("m7"), Interval(6, 10));
    EXPECT_EQ(ivl("d8"), Interval(7, 11));
    EXPECT_EQ(ivl("P8"), Interval(7, 12));
    EXPECT_EQ(pt::intervalIndex("M9"), -1);
}

TEST_F(Agent_Pitch, PitchNamesUseMuseScoreOctaves)
{
    mu::agent::notation::Pitch p;
    ASSERT_TRUE(pt::parsePitchName("C4", p));
    EXPECT_EQ(p.midi(), 60);
    ASSERT_TRUE(pt::parsePitchName("B#3", p));
    EXPECT_EQ(p.midi(), 60);
    // tpc2pitch agrees with the octave rule: B#3 and C4 are the same key
    EXPECT_EQ(tpc2pitch(step2tpc(6, AccidentalVal::SHARP)) % 12, 0);
    ASSERT_TRUE(pt::parsePitchName("Cb4", p));
    EXPECT_EQ(p.midi(), 59);
    ASSERT_TRUE(pt::parsePitchName("Bbb3", p));
    EXPECT_EQ(p.alter, -2);
    EXPECT_EQ(p.midi(), 57);
    ASSERT_TRUE(pt::parsePitchName("F##5", p));
    EXPECT_EQ(p.midi(), 79);
    ASSERT_TRUE(pt::parsePitchName("C-1", p));
    EXPECT_EQ(p.midi(), 0);
    ASSERT_TRUE(pt::parsePitchName("G9", p));
    EXPECT_EQ(p.midi(), 127);
    std::string why;
    EXPECT_FALSE(pt::parsePitchName("G#9", p, &why));
    EXPECT_NE(why.find("outside 0-127"), std::string::npos) << why;
    EXPECT_FALSE(pt::parsePitchName("Cb-1", p));
    EXPECT_FALSE(pt::parsePitchName("", p));
    EXPECT_FALSE(pt::parsePitchName("c4", p));
    EXPECT_FALSE(pt::parsePitchName("H4", p));
    EXPECT_FALSE(pt::parsePitchName("Cx4", p));
    EXPECT_FALSE(pt::parsePitchName("C10", p));
    EXPECT_FALSE(pt::parsePitchName("C#", p));
    EXPECT_FALSE(pt::parsePitchName("C###4", p));
}

// ================================================================== set_pitch

TEST_F(Agent_Pitch, SetPitchSpelledNonTransposing)
{
    Note* n = noteAt(score.get(), 1, 1, 1);
    ASSERT_TRUE(n);
    ASSERT_EQ(n->pitch(), 60);
    addsOneEntry(SET, setArgs(where(1, 1, 1), Val("F#4")), [&] {
        EXPECT_EQ(n->pitch(), 66);
        EXPECT_EQ(n->tpc1(), Tpc::TPC_F_S);
        EXPECT_EQ(n->tpc2(), Tpc::TPC_F_S);
    }, [&] {
        EXPECT_EQ(n->pitch(), 60);
        EXPECT_EQ(n->tpc1(), Tpc::TPC_C);
    });
    EXPECT_EQ(outStr("status"), "changed");
    EXPECT_EQ(outStr("pitch"), "F#4");
    EXPECT_EQ(outInt("tiedNotes"), 1);
    EXPECT_EQ(outStr("bars"), "m1");
    EXPECT_NE(outStr("notation").find("m1 v1 | F#4:q D4:q"), std::string::npos) << outStr("notation");
    EXPECT_EQ(outStr("handle"), makeHandle(n)) << "the result names the same note";
}

TEST_F(Agent_Pitch, SetPitchRespellOnly)
{
    Note* n = noteAt(score.get(), 1, 3, 2); // F#4
    ASSERT_TRUE(n);
    ASSERT_EQ(n->pitch(), 66);
    addsOneEntry(SET, setArgs(where(1, 3, 2), Val("Gb4")), [&] {
        EXPECT_EQ(n->pitch(), 66);
        EXPECT_EQ(n->tpc1(), Tpc::TPC_G_B);
        EXPECT_EQ(n->tpc2(), Tpc::TPC_G_B);
    }, [&] {
        EXPECT_EQ(n->tpc1(), Tpc::TPC_F_S);
    });
}

TEST_F(Agent_Pitch, SetPitchSameValueIsUnchanged)
{
    const size_t before = backend->currentIndex();
    ASSERT_TRUE(run(SET, setArgs(where(1, 1, 1), Val("C4"))));
    EXPECT_FALSE(changed);
    EXPECT_EQ(outStr("status"), "unchanged");
    EXPECT_EQ(backend->currentIndex(), before);
}

TEST_F(Agent_Pitch, SetPitchMidiNumberSpellings)
{
    Note* n = noteAt(score.get(), 1, 1, 1);
    ASSERT_TRUE(run(SET, setArgs(where(1, 1, 1), Val(66))));
    EXPECT_EQ(n->pitch(), 66);
    EXPECT_EQ(n->tpc1(), pitch2tpc(66, Key::C, Prefer::NEAREST));
    ASSERT_TRUE(run(SET, with(setArgs(where(1, 1, 1), Val(68)), "spelling", Val("sharps"))));
    EXPECT_EQ(n->tpc1(), Tpc::TPC_G_S);
    ASSERT_TRUE(run(SET, with(setArgs(where(1, 1, 1), Val(68)), "spelling", Val("flats"))));
    EXPECT_EQ(n->pitch(), 68);
    EXPECT_EQ(n->tpc1(), Tpc::TPC_A_B);
}

TEST_F(Agent_Pitch, SetPitchFollowsTheWholeTieChain)
{
    open("ties-barlines.mscz");
    // bars 3-5: G4 tied m3 beat 4 -> m4 -> m5 beat 1; address the middle note
    Note* a = noteAt(score.get(), 1, 3, 4);
    Note* b = noteAt(score.get(), 1, 4, 1);
    Note* c = noteAt(score.get(), 1, 5, 1);
    ASSERT_TRUE(a && b && c);
    ASSERT_EQ(b->tiedNotes().size(), 3u);
    addsOneEntry(SET, setArgs(where(1, 4, 1), Val("A4")), [&] {
        EXPECT_EQ(a->pitch(), 69);
        EXPECT_EQ(b->pitch(), 69);
        EXPECT_EQ(c->pitch(), 69);
        ASSERT_TRUE(a->tieFor());
        EXPECT_EQ(a->tieFor()->endNote(), b);
        ASSERT_TRUE(b->tieFor());
        EXPECT_EQ(b->tieFor()->endNote(), c);
    }, [&] {
        EXPECT_EQ(a->pitch(), 67);
        EXPECT_EQ(b->pitch(), 67);
        EXPECT_EQ(c->pitch(), 67);
    });
    EXPECT_EQ(outInt("tiedNotes"), 3);
    EXPECT_EQ(outStr("bars"), "m3-m5");
}

TEST_F(Agent_Pitch, SetPitchWrittenOnTransposingInstrument)
{
    // staff 2: Bb trumpet; m1 beat 1 sounds C4, written D4
    Note* n = noteAt(score.get(), 2, 1, 1);
    ASSERT_TRUE(n);
    ASSERT_EQ(n->tpc2(), Tpc::TPC_D);
    addsOneEntry(SET, with(setArgs(where(2, 1, 1), Val("E4")), "space", Val("written")), [&] {
        EXPECT_EQ(n->pitch(), 62);
        EXPECT_EQ(n->tpc1(), Tpc::TPC_D);
        EXPECT_EQ(n->tpc2(), Tpc::TPC_E);
    }, [&] {
        EXPECT_EQ(n->pitch(), 60);
        EXPECT_EQ(n->tpc2(), Tpc::TPC_D);
    });
    EXPECT_EQ(outStr("pitch"), "D4");
    EXPECT_EQ(outStr("writtenPitch"), "E4");
}

TEST_F(Agent_Pitch, SetPitchSoundingOnTransposingInstrument)
{
    Note* n = noteAt(score.get(), 2, 1, 1);
    ASSERT_TRUE(run(SET, setArgs(where(2, 1, 1), Val("Bb4"))));
    EXPECT_EQ(n->pitch(), 70);
    EXPECT_EQ(n->tpc1(), Tpc::TPC_B_B);
    EXPECT_EQ(n->tpc2(), Tpc::TPC_C);
    EXPECT_EQ(outStr("writtenPitch"), "C5");
}

TEST_F(Agent_Pitch, SetPitchWrittenMidiUsesTheWrittenKey)
{
    // the trumpet is written in D major: written MIDI 66 is F#, sounding E4
    Note* n = noteAt(score.get(), 2, 1, 1);
    ASSERT_TRUE(run(SET, with(setArgs(where(2, 1, 1), Val(66)), "space", Val("written"))));
    EXPECT_EQ(n->pitch(), 64);
    EXPECT_EQ(n->tpc2(), Tpc::TPC_F_S);
    EXPECT_EQ(n->tpc1(), Tpc::TPC_E);
}

TEST_F(Agent_Pitch, SetPitchRemovesTheOldAccidental)
{
    // m3 beat 1 is Bb4 with a flat
    Note* n = noteAt(score.get(), 1, 3, 1);
    ASSERT_TRUE(n);
    ASSERT_TRUE(n->accidental());
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: test"));
    ASSERT_TRUE(tx.step("op", [&] {
        Ret r = SET(setArgs(where(1, 3, 1), Val("B4")), score.get(), out);
        EXPECT_FALSE(n->accidental()) << "upDown removes the accidental before the pitch change (cmd.cpp:2078-2086)";
        return r;
    }));
    ASSERT_TRUE(tx.commit());
    EXPECT_EQ(n->pitch(), 71);
}

TEST_F(Agent_Pitch, SetPitchLinkedPartsFollow)
{
    open("parts-linked.mscz");
    ASSERT_FALSE(score->excerpts().empty());
    Note* n = noteAt(score.get(), 1, 1, 3); // flute G5
    ASSERT_TRUE(n);
    ASSERT_GE(n->linkList().size(), 2u) << "a copy in the Flute part";
    auto all = [&](int pitch, int tpc) {
        for (EngravingObject* e : n->linkList()) {
            EXPECT_EQ(toNote(e)->pitch(), pitch);
            EXPECT_EQ(toNote(e)->tpc1(), tpc);
        }
    };
    addsOneEntry(SET, setArgs(where(1, 1, 3), Val("Bb5")), [&] { all(82, Tpc::TPC_B_B); }, [&] { all(79, Tpc::TPC_G); });
}

TEST_F(Agent_Pitch, SetPitchLinkedTransposingPart)
{
    open("parts-linked.mscz");
    Note* n = noteAt(score.get(), 2, 1, 2); // clarinet: sounds E5, written F#5
    ASSERT_TRUE(n);
    ASSERT_EQ(n->tpc2(), Tpc::TPC_F_S);
    ASSERT_TRUE(run(SET, with(setArgs(where(2, 1, 2), Val("G5")), "space", Val("written"))));
    for (EngravingObject* e : n->linkList()) {
        EXPECT_EQ(toNote(e)->pitch(), 77);
        EXPECT_EQ(toNote(e)->tpc1(), Tpc::TPC_F);
        EXPECT_EQ(toNote(e)->tpc2(), Tpc::TPC_G);
    }
}

TEST_F(Agent_Pitch, SetPitchChordToneByPitchOrIndex)
{
    open("parts-linked.mscz");
    ChordRest* cr = crAt(score.get(), 1, 2, 3); // [C5, E5]
    ASSERT_TRUE(cr && cr->isChord());
    ASSERT_EQ(toChord(cr)->notes().size(), 2u);
    Note* low = toChord(cr)->notes().at(0);
    Note* high = toChord(cr)->notes().at(1);
    refused(SET, setArgs(where(1, 2, 3), Val("F5")), AErr::InvalidArg, "has 2 notes");
    ASSERT_TRUE(run(SET, setArgs(withKey(where(1, 2, 3), "note", Val("E5")), Val("F5"))));
    EXPECT_EQ(high->pitch(), 77);
    EXPECT_EQ(low->pitch(), 72);
    ASSERT_TRUE(run(SET, setArgs(withKey(where(1, 2, 3), "note", Val(1)), Val("D5"))));
    EXPECT_EQ(low->pitch(), 74);
    refused(SET, setArgs(withKey(where(1, 2, 3), "note", Val("G5")), Val("A5")), AErr::InvalidArg, "no G5");
    refused(SET, setArgs(withKey(where(1, 2, 3), "note", Val("X")), Val("A5")), AErr::InvalidArg, "note \"X\"");
    refused(SET, setArgs(withKey(where(1, 2, 3), "note", Val(3)), Val("A5")), AErr::Range, "2 note(s)");
    refused(SET, setArgs(withKey(where(1, 2, 3), "note", Val(0)), Val("A5")), AErr::InvalidArg, "from 1");
    refused(SET, setArgs(withKey(where(1, 2, 3), "note", Val(true)), Val("A5")), AErr::InvalidArg, "note must be");
    Params both = setArgs(withKey(where(1, 2, 3), "note", Val("E5")), Val("A5"));
    both["note_index"] = Val(1);
    refused(SET, both, AErr::InvalidArg, "not both");
}

TEST_F(Agent_Pitch, SetPitchGraceNote)
{
    open("grace-notes.mscz");
    ChordRest* cr = crAt(score.get(), 1, 1, 1);
    ASSERT_TRUE(cr && cr->isChord());
    ASSERT_EQ(toChord(cr)->graceNotesBefore().size(), 1u);
    Note* grace = toChord(cr)->graceNotesBefore().at(0)->notes().front();
    Note* main = toChord(cr)->notes().front();
    const int mainPitch = main->pitch();
    ASSERT_TRUE(run(SET, setArgs(withKey(where(1, 1, 1), "grace", Val(1)), Val("E5"))));
    EXPECT_EQ(grace->pitch(), 76);
    EXPECT_EQ(main->pitch(), mainPitch);
    refused(SET, setArgs(withKey(where(1, 1, 1), "grace", Val(2)), Val("E5")), AErr::Range, "grace note(s)");
    refused(SET, setArgs(withKey(where(1, 1, 1), "grace", Val(0)), Val("E5")), AErr::InvalidArg, "grace");
}

TEST_F(Agent_Pitch, SetPitchHandleTargets)
{
    open("parts-linked.mscz");
    Note* n = noteAt(score.get(), 1, 1, 2);
    ASSERT_TRUE(n);
    Params p = setArgs(Val(makeHandle(n)), Val("G5"));
    ASSERT_TRUE(run(SET, p));
    EXPECT_EQ(n->pitch(), 79);
    // the old handle's fingerprint was the old pitch: it is stale now
    refused(SET, p, AErr::StaleHandle);
    // the returned handle works
    ASSERT_TRUE(run(SET, setArgs(Val(outStr("handle")), Val("F5"))));
    EXPECT_EQ(n->pitch(), 77);
    // a single-note chord handle works
    ASSERT_TRUE(run(SET, setArgs(Val(makeHandle(n->chord())), Val("E5"))));
    EXPECT_EQ(n->pitch(), 76);
    // a chord of two notes, a rest, a bar and garbage are refused
    refused(SET, setArgs(Val(makeHandle(crAt(score.get(), 1, 2, 3))), Val("E5")), AErr::InvalidArg, "has 2 notes");
    // a chord handle with a tone selector works; a note handle with one is refused
    Params tone = setArgs(Val(makeHandle(crAt(score.get(), 1, 2, 3))), Val("G5"));
    tone["note"] = Val("E5");
    ASSERT_TRUE(run(SET, tone));
    EXPECT_EQ(toChord(crAt(score.get(), 1, 2, 3))->notes().at(1)->pitch(), 79);
    Params noteAndTone = setArgs(Val(makeHandle(n)), Val("G5"));
    noteAndTone["note_index"] = Val(1);
    refused(SET, noteAndTone, AErr::InvalidArg, "names one note already");
    refused(SET, setArgs(Val(makeHandle(bar(score.get(), 1))), Val("E5")), AErr::InvalidArg, "must name a note");
    refused(SET, setArgs(Val("garbage"), Val("E5")), AErr::InvalidArg);
    ChordRest* rest = crAt(score.get(), 2, 1, 1); // clarinet bar 1 starts with a rest
    ASSERT_TRUE(rest && rest->isRest());
    refused(SET, setArgs(Val(makeHandle(rest)), Val("E5")), AErr::InvalidArg, "rest");
}

TEST_F(Agent_Pitch, SetPitchTargetErrors)
{
    Params noTarget;
    noTarget["pitch"] = Val("C4");
    refused(SET, noTarget, AErr::InvalidArg, "missing target");
    refused(SET, setArgs(Val(5), Val("C4")), AErr::InvalidArg, "target must be");
    refused(SET, setArgs(withKey(where(1, 1, 1), "chord", Val(1)), Val("C4")), AErr::InvalidArg, "at has an unknown key 'chord'");
    Params both = setArgs(where(1, 1, 1), Val("C4"));
    both["target"] = Val(makeHandle(noteAt(score.get(), 1, 1, 1)));
    refused(SET, both, AErr::InvalidArg, "not both");
    Params noAt = setArgs(where(1, 1, 1), Val("C4"));
    noAt.erase("at");
    refused(SET, noAt, AErr::InvalidArg, "missing target");
    refused(SET, with(setArgs(where(1, 1, 1), Val("C4")), "voice", Val(5)), AErr::Range, "voice must be 1-4");
    refused(SET, setArgs(where(99, 1, 1), Val("C4")), AErr::Range, "outside");
    refused(SET, setArgs(where(1, 99, 1), Val("C4")), AErr::Range, "outside the score");
    refused(SET, setArgs(where(1, 1, 1.5), Val("C4")), AErr::Range, "no note or rest starts");
    refused(SET, setArgs(where(1, 1, 1, 2), Val("C4")), AErr::Range, "voice 2");
    open("ties-barlines.mscz");
    refused(SET, setArgs(where(1, 7, 4), Val("C4")), AErr::InvalidArg, "rest"); // m7 beat 4 is a rest
}

TEST_F(Agent_Pitch, SetPitchPitchErrors)
{
    const Val t = where(1, 1, 1);
    Params noPitch = setArgs(t, Val("C4"));
    noPitch.erase("pitch");
    refused(SET, noPitch, AErr::InvalidArg, "missing pitch");
    refused(SET, setArgs(t, Val("H4")), AErr::InvalidArg, "step letter");
    refused(SET, setArgs(t, Val("c4")), AErr::InvalidArg, "step letter");
    refused(SET, setArgs(t, Val("Cx4")), AErr::InvalidArg, "octave");
    refused(SET, setArgs(t, Val("C10")), AErr::InvalidArg, "octave");
    refused(SET, setArgs(t, Val("G#9")), AErr::InvalidArg, "outside 0-127");
    refused(SET, setArgs(t, Val(128)), AErr::InvalidArg, "outside 0-127");
    refused(SET, setArgs(t, Val(-1)), AErr::InvalidArg, "outside 0-127");
    refused(SET, setArgs(t, Val(60.5)), AErr::InvalidArg, "whole number");
    refused(SET, setArgs(t, Val(true)), AErr::InvalidArg, "spelled pitch");
    refused(SET, with(setArgs(t, Val("C4")), "midi", Val(60)), AErr::InvalidArg, "not both");
    refused(SET, with(setArgs(t, Val("C4")), "spelling", Val("sharps")), AErr::InvalidArg, "midi only");
    refused(SET, with(setArgs(t, Val(61)), "spelling", Val("naturals")), AErr::InvalidArg, "auto, sharps or flats");
    refused(SET, with(setArgs(t, Val("C4")), "space", Val("concert")), AErr::InvalidArg, "sounding");
    refused(SET, with(setArgs(t, Val("C4")), "tab", Val(3)), AErr::InvalidArg, "tab must be");
}

TEST_F(Agent_Pitch, SetPitchStoredOutOfRangeIsRefused)
{
    // staff 5: tenor sax, sounds a major ninth (14 semitones) below written: written C-1 would be MIDI -14
    refused(SET, with(setArgs(where(5, 1, 1), Val("C-1")), "space", Val("written")), AErr::InvalidArg, "outside 0-127");
    refused(SET, with(setArgs(where(5, 1, 1), Val(0)), "space", Val("written")), AErr::InvalidArg, "outside 0-127");
}

TEST_F(Agent_Pitch, SetPitchTripleAccidentalIsRefused)
{
    // written Cbb5 on the Bb trumpet would sound Bbbb4
    refused(SET, with(setArgs(where(2, 1, 1), Val("Cbb5")), "space", Val("written")), AErr::InvalidArg, "triple accidental");
}

TEST_F(Agent_Pitch, SetPitchUnderAnOttava)
{
    open("spanners-system-break.mscz");
    Note* n = nullptr;
    for (const auto& [tick, sp] : score->spanner()) {
        if (sp->isOttava() && sp->startCR() && sp->startCR()->isChord()) {
            n = toChord(sp->startCR())->notes().front();
            break;
        }
    }
    ASSERT_TRUE(n) << "the fixture has an 8va line";
    ASSERT_EQ(n->staff()->pitchOffset(n->chord()->tick()), 12);
    Params p;
    p["target"] = Val(makeHandle(n));
    p["pitch"] = Val("C6");
    ASSERT_TRUE(run(SET, p)) << "sounding C6 under 8va";
    EXPECT_EQ(n->pitch(), 72) << "stored without the ottava (NOTATION.md section 2)";
    EXPECT_EQ(outStr("pitch"), "C6");
    p["target"] = Val(outStr("handle"));
    p["pitch"] = Val("D5");
    p["space"] = Val("written");
    ASSERT_TRUE(run(SET, p));
    EXPECT_EQ(n->pitch(), 74);
    EXPECT_EQ(outStr("pitch"), "D6");
}

TEST_F(Agent_Pitch, SetPitchGuitarBendIsRefused)
{
    open("ties-barlines.mscz");
    Note* n = noteAt(score.get(), 1, 1, 1);
    ASSERT_TRUE(n);
    score->startCmd(muse::TranslatableString::untranslatable("test: bend"));
    GuitarBend* bend = score->addGuitarBend(GuitarBendType::BEND, n);
    score->endCmd();
    ASSERT_TRUE(bend) << "addGuitarBend(BEND) made no bend from m1 beat 1";
    ASSERT_TRUE(n->bendFor());
    Note* end = toNote(bend->endElement());
    ASSERT_TRUE(end && end->bendBack());
    refused(SET, setArgs(where(1, 1, 1), Val("D5")), AErr::InvalidArg, "guitar bend");
    refused(SET, setArgs(Val(makeHandle(end)), Val("F5")), AErr::InvalidArg, "guitar bend");
}

TEST_F(Agent_Pitch, SetPitchTabOnStandardStaffIsRefused)
{
    ValMap tab;
    tab["string"] = Val(0);
    tab["fret"] = Val(3);
    refused(SET, with(setArgs(where(1, 1, 1), Val("C4")), "tab", Val(tab)), AErr::InvalidArg, "TAB staves");
}

TEST_F(Agent_Pitch, SetPitchOnTabStaff)
{
    open("ties-barlines.mscz");
    // test set-up only: make staff 1 a six-string guitar TAB staff
    Staff* staff = score->staff(0);
    int strings[6] = { 40, 45, 50, 55, 59, 64 };
    staff->part()->instrument()->setStringData(StringData(24, 6, strings));
    staff->setStaffType(Fraction(0, 1), *StaffType::preset(StaffTypes::TAB_6COMMON));
    ASSERT_TRUE(staff->isTabStaff(Fraction(0, 1)));
    Note* n = noteAt(score.get(), 1, 1, 1);
    ASSERT_TRUE(n);

    // no tab: the highest string that plays it
    ASSERT_TRUE(run(SET, setArgs(where(1, 1, 1), Val("A4"))));
    EXPECT_EQ(n->pitch(), 69);
    const StringData* sd = staff->part()->stringData(n->chord()->tick(), staff->idx());
    ASSERT_TRUE(sd);
    EXPECT_EQ(sd->getPitch(n->string(), n->fret(), staff, n->chord()->tick()), 69);
    EXPECT_EQ(outInt("string"), n->string());

    // an explicit fretting that plays it: B string (1) fret 10 = A4
    ValMap tab;
    tab["string"] = Val(1);
    tab["fret"] = Val(10);
    ASSERT_TRUE(run(SET, with(setArgs(Val(outStr("handle")), Val("A4")), "tab", Val(tab))));
    EXPECT_EQ(n->string(), 1);
    EXPECT_EQ(n->fret(), 10);

    // the same pitch without tab keeps the note's fretting (not convertPitch's string 0 fret 5):
    // unchanged, and a respelling stays on string 1 fret 10 too
    ASSERT_TRUE(run(SET, setArgs(Val(outStr("handle")), Val("A4"))));
    EXPECT_EQ(outStr("status"), "unchanged");
    EXPECT_FALSE(changed);
    EXPECT_EQ(n->string(), 1);
    EXPECT_EQ(n->fret(), 10);
    ASSERT_TRUE(run(SET, setArgs(Val(outStr("handle")), Val("Bbb4"))));
    EXPECT_EQ(n->tpc1(), Tpc::TPC_B_BB);
    EXPECT_EQ(n->string(), 1);
    EXPECT_EQ(n->fret(), 10);

    // refusals
    refused(SET, with(setArgs(where(1, 1, 1), Val("C5")), "tab", Val(tab)), AErr::InvalidArg, "plays MIDI");
    ValMap badString = tab;
    badString["string"] = Val(6);
    refused(SET, with(setArgs(where(1, 1, 1), Val("A4")), "tab", Val(badString)), AErr::InvalidArg, "tab.string");
    ValMap badFret = tab;
    badFret["fret"] = Val(25);
    refused(SET, with(setArgs(where(1, 1, 1), Val("A4")), "tab", Val(badFret)), AErr::InvalidArg, "tab.fret");
    ValMap junk = tab;
    junk["capo"] = Val(1);
    refused(SET, with(setArgs(where(1, 1, 1), Val("A4")), "tab", Val(junk)), AErr::InvalidArg, "unknown key");
    ValMap half;
    half["string"] = Val(1);
    refused(SET, with(setArgs(where(1, 1, 1), Val("A4")), "tab", Val(half)), AErr::InvalidArg, "whole numbers");
    refused(SET, setArgs(where(1, 1, 1), Val("C2")), AErr::InvalidArg, "cannot be fretted"); // below the low E
}

TEST_F(Agent_Pitch, SetPitchOnTabStaffWithoutStrings)
{
    open("ties-barlines.mscz");
    // test set-up only: a TAB staff type on an instrument with no strings
    Staff* staff = score->staff(0);
    staff->part()->instrument()->setStringData(StringData());
    staff->setStaffType(Fraction(0, 1), *StaffType::preset(StaffTypes::TAB_6COMMON));
    refused(SET, setArgs(where(1, 1, 1), Val("A4")), AErr::Refused, "without strings");
}

TEST_F(Agent_Pitch, SetPitchGraceOnAGraceChordIsRefused)
{
    open("grace-notes.mscz");
    ChordRest* cr = crAt(score.get(), 1, 1, 1);
    ASSERT_TRUE(cr && cr->isChord() && !toChord(cr)->graceNotesBefore().empty());
    Chord* grace = toChord(cr)->graceNotesBefore().at(0);
    Params p = setArgs(Val(makeHandle(grace)), Val("E5"));
    p["grace"] = Val(1);
    refused(SET, p, AErr::InvalidArg, "grace note already");
    // the grace chord's handle alone works
    p.erase("grace");
    ASSERT_TRUE(run(SET, p));
    EXPECT_EQ(grace->notes().front()->pitch(), 76);
}

TEST_F(Agent_Pitch, SetPitchOnDrumStaff)
{
    open("ties-barlines.mscz");
    if (!smDrumset) {
        GTEST_SKIP() << "no standard drumset in this environment";
    }
    // test set-up only: the instrument uses the standard drumset
    score->staff(0)->part()->instrument()->setDrumset(smDrumset);
    ASSERT_TRUE(score->staff(0)->part()->instrument()->useDrumset());
    Note* n = noteAt(score.get(), 1, 1, 1);
    ASSERT_TRUE(smDrumset->isValid(38));
    ASSERT_TRUE(run(SET, setArgs(where(1, 1, 1), Val(38))));
    EXPECT_EQ(n->pitch(), 38);
    EXPECT_EQ(n->tpc1(), pitch2tpc(38, Key::C, Prefer::NEAREST));
    EXPECT_EQ(n->tpc2(), n->tpc1());
    refused(SET, setArgs(where(1, 1, 1), Val("D2")), AErr::InvalidArg, "drum staff");
    refused(SET, with(setArgs(where(1, 1, 1), Val(36)), "space", Val("written")), AErr::InvalidArg, "drum staff");
    refused(SET, with(setArgs(where(1, 1, 1), Val(36)), "spelling", Val("flats")), AErr::InvalidArg, "drum staff");
    int invalid = -1;
    for (int i = 0; i < 128 && invalid < 0; ++i) {
        if (!smDrumset->isValid(i)) {
            invalid = i;
        }
    }
    ASSERT_GE(invalid, 0);
    refused(SET, setArgs(where(1, 1, 1), Val(invalid)), AErr::InvalidArg, "not a sound in this staff's drumset");
}

// ================================================================== transpose

TEST_F(Agent_Pitch, TransposeBarByIntervalKeepsSpelling) // S08
{
    ValMap flute;
    flute["part"] = Val("Flute");
    flute["index"] = Val(1);
    ValMap r;
    r["start"] = pos(3, 1);
    r["end"] = pos(3, 4);
    r["staves"] = Val(ValList { Val(flute) });
    Params p = trArgs(Val(r), interval("M2", "up"));
    p["key_signatures"] = Val(false);
    const std::vector<int> trumpet = pitchesOf(score.get(), 2, 3, { 1, 2, 3, 4 });
    addsOneEntry(TR, p, [&] {
        EXPECT_EQ(pitchesOf(score.get(), 1, 3, { 1, 2, 3, 4 }), (std::vector<int> { 72, 68, 65, 70 }));
        EXPECT_EQ(noteAt(score.get(), 1, 3, 1)->tpc1(), Tpc::TPC_C);
        EXPECT_EQ(noteAt(score.get(), 1, 3, 2)->tpc1(), Tpc::TPC_G_S);
        EXPECT_EQ(noteAt(score.get(), 1, 3, 3)->tpc1(), Tpc::TPC_F);
        EXPECT_EQ(noteAt(score.get(), 1, 3, 4)->tpc1(), Tpc::TPC_B_B);
        EXPECT_EQ(pitchesOf(score.get(), 2, 3, { 1, 2, 3, 4 }), trumpet);
        EXPECT_EQ(pitchesOf(score.get(), 1, 4, { 1, 3 }), (std::vector<int> { 60, 67 })) << "bar 4 untouched";
    }, [&] {
        EXPECT_EQ(pitchesOf(score.get(), 1, 3, { 1, 2, 3, 4 }), (std::vector<int> { 70, 66, 63, 68 }));
    });
    EXPECT_EQ(outStr("status"), "transposed");
    EXPECT_EQ(outStr("interval"), "M2 up");
    EXPECT_EQ(outInt("notes"), 4);
    EXPECT_EQ(outStr("bars"), "m3");
    EXPECT_NE(outStr("notation").find("m3 v1 | C5:q G#4:q F4:q Bb4:q"), std::string::npos) << outStr("notation");
}

TEST_F(Agent_Pitch, TransposeCompoundIntervalIsOneEntry)
{
    Params p = trArgs(rangeOf(pos(1), pos(1), { 1 }), interval("M3", "up", 1));
    addsOneEntry(TR, p, [&] {
        EXPECT_EQ(pitchesOf(score.get(), 1, 1, { 1, 2, 3, 4 }), (std::vector<int> { 76, 78, 80, 81 }));
        EXPECT_EQ(noteAt(score.get(), 1, 1, 1)->tpc1(), Tpc::TPC_E);
    }, [&] {
        EXPECT_EQ(pitchesOf(score.get(), 1, 1, { 1, 2, 3, 4 }), (std::vector<int> { 60, 62, 64, 65 }));
    });
    EXPECT_EQ(outStr("interval"), "M3 + 1 octave(s) up");
}

TEST_F(Agent_Pitch, TransposeDown)
{
    ASSERT_TRUE(run(TR, trArgs(rangeOf(pos(2, 1), pos(2, 2), { 1 }), interval("m3", "down"))));
    EXPECT_EQ(pitchesOf(score.get(), 1, 2, { 1, 2, 3 }), (std::vector<int> { 64, 66, 71 }));
    EXPECT_EQ(noteAt(score.get(), 1, 2, 2)->tpc1(), Tpc::TPC_F_S);
    EXPECT_EQ(outStr("interval"), "m3 down");
}

TEST_F(Agent_Pitch, TransposeToKeyWholeScore)
{
    Params p = trArgs(scoreRange(), toKey(2));
    addsOneEntry(TR, p, [&] {
        EXPECT_EQ(noteAt(score.get(), 1, 1, 1)->pitch(), 62);
        EXPECT_EQ(noteAt(score.get(), 1, 1, 1)->tpc1(), Tpc::TPC_D);
        Note* tp = noteAt(score.get(), 2, 1, 1);
        EXPECT_EQ(tp->pitch(), 62);
        EXPECT_EQ(tp->tpc2(), Tpc::TPC_E);
        for (staff_idx_t s = 0; s < score->nstaves(); ++s) {
            EXPECT_EQ(int(score->staff(s)->concertKey(Fraction(0, 1))), 2) << "staff " << s + 1;
        }
        EXPECT_EQ(int(score->staff(1)->key(Fraction(0, 1))), 4) << "the Bb trumpet is written in E";
    }, [&] {
        EXPECT_EQ(noteAt(score.get(), 1, 1, 1)->pitch(), 60);
        EXPECT_EQ(int(score->staff(1)->concertKey(Fraction(0, 1))), 0);
    });
    EXPECT_EQ(outStr("interval"), "M2 up");
    EXPECT_GE(outInt("keySignatures"), 5);
}

TEST_F(Agent_Pitch, TransposeToTheSameKeyClosestDoesNothing)
{
    const std::string saved = snapshot();
    const size_t before = backend->currentIndex();
    ASSERT_TRUE(run(TR, trArgs(scoreRange(), toKey(0, "closest"))));
    EXPECT_FALSE(changed);
    EXPECT_EQ(outStr("status"), "unchanged");
    EXPECT_EQ(backend->currentIndex(), before);
    EXPECT_EQ(snapshot(), saved);
}

TEST_F(Agent_Pitch, TransposeToTheSameKeyUpIsAnOctave)
{
    ASSERT_TRUE(run(TR, trArgs(scoreRange(), toKey(0, "up"))));
    EXPECT_EQ(noteAt(score.get(), 1, 1, 1)->pitch(), 72);
}

TEST_F(Agent_Pitch, TransposeToKeyOnSeparateStaves)
{
    // staves 1 and 3 (not 2): two engine calls, one per block
    const std::vector<int> trumpet = pitchesOf(score.get(), 2, 1, { 1, 2 });
    ASSERT_TRUE(run(TR, trArgs(rangeOf(pos(1), pos(1), { 1, 3 }), toKey(2))));
    EXPECT_EQ(noteAt(score.get(), 1, 1, 1)->pitch(), 62);
    EXPECT_EQ(noteAt(score.get(), 3, 1, 1)->pitch(), 62);
    EXPECT_EQ(pitchesOf(score.get(), 2, 1, { 1, 2 }), trumpet);
}

TEST_F(Agent_Pitch, TransposeDiatonicKeepsAlterations)
{
    const std::vector<int> trumpet = pitchesOf(score.get(), 2, 3, { 1, 2, 3, 4 });
    addsOneEntry(TR, trArgs(rangeOf(pos(3), pos(3), { 1 }), degrees(2, "up")), [&] {
        // Bb F# Eb Ab up a third in C, alterations kept: Db A# Gb Cb
        EXPECT_EQ(pitchesOf(score.get(), 1, 3, { 1, 2, 3, 4 }), (std::vector<int> { 73, 70, 66, 71 }));
        EXPECT_EQ(noteAt(score.get(), 1, 3, 1)->tpc1(), Tpc::TPC_D_B);
        EXPECT_EQ(noteAt(score.get(), 1, 3, 4)->tpc1(), Tpc::TPC_C_B);
        EXPECT_EQ(pitchesOf(score.get(), 2, 3, { 1, 2, 3, 4 }), trumpet);
        EXPECT_EQ(int(score->staff(0)->concertKey(bar(score.get(), 3)->tick())), 0) << "key unchanged";
    }, [&] {
        EXPECT_EQ(pitchesOf(score.get(), 1, 3, { 1, 2, 3, 4 }), (std::vector<int> { 70, 66, 63, 68 }));
    });
}

TEST_F(Agent_Pitch, TransposeDiatonicWithoutAlterations)
{
    ASSERT_TRUE(run(TR, trArgs(rangeOf(pos(3, 1), pos(3, 1), { 1 }), degrees(2, "up", 0))));
    EXPECT_EQ(noteAt(score.get(), 1, 3, 1)->pitch(), 74); // Bb -> D
}

TEST_F(Agent_Pitch, TransposeDoubleAccidentals)
{
    ASSERT_TRUE(run(TR, trArgs(rangeOf(pos(3, 2), pos(3, 2), { 1 }), interval("A1", "up"))));
    EXPECT_EQ(noteAt(score.get(), 1, 3, 2)->tpc1(), Tpc::TPC_F_SS);
    score->undoRedo(true, nullptr);
    Params p = trArgs(rangeOf(pos(3, 2), pos(3, 2), { 1 }), interval("A1", "up"));
    p["double_accidentals"] = Val(false);
    ASSERT_TRUE(run(TR, p));
    EXPECT_EQ(noteAt(score.get(), 1, 3, 2)->pitch(), 67);
    EXPECT_EQ(noteAt(score.get(), 1, 3, 2)->tpc1(), Tpc::TPC_G);
}

TEST_F(Agent_Pitch, TransposeOutOfRangeIsRefused)
{
    ASSERT_TRUE(run(SET, setArgs(where(1, 1, 1), Val("G9"))));
    ASSERT_EQ(noteAt(score.get(), 1, 1, 1)->pitch(), 127);
    refused(TR, trArgs(rangeOf(pos(1), pos(1), { 1 }), interval("m2", "up")), AErr::InvalidArg, "outside MIDI 0-127");
    refused(TR, trArgs(rangeOf(pos(1), pos(1), { 1 }), degrees(1, "up")), AErr::InvalidArg, "outside MIDI 0-127");
    refused(TR, trArgs(rangeOf(pos(1), pos(1), { 1 }), interval("P1", "up", 1)), AErr::InvalidArg, "outside MIDI 0-127");
}

TEST_F(Agent_Pitch, TransposeTieLeavingTheRange)
{
    open("ties-barlines.mscz");
    // m3 beat 3 A4, beat 4 G4 tied into m4 and m5
    const Params p = trArgs(rangeOf(pos(3, 3), pos(3, 4)), interval("M2", "up"));
    refused(TR, p, AErr::InvalidArg, "is tied to");
    refused(TR, trArgs(rangeOf(pos(3), pos(3)), interval("M2", "up")), AErr::InvalidArg, "is tied to"); // tied in from m2
    addsOneEntry(TR, with(p, "ties", Val("extend")), [&] {
        EXPECT_EQ(noteAt(score.get(), 1, 3, 3)->pitch(), 71);
        EXPECT_EQ(noteAt(score.get(), 1, 3, 4)->pitch(), 69);
        EXPECT_EQ(noteAt(score.get(), 1, 4, 1)->pitch(), 69);
        EXPECT_EQ(noteAt(score.get(), 1, 5, 1)->pitch(), 69);
        EXPECT_EQ(noteAt(score.get(), 1, 5, 3)->pitch(), 72) << "after the chain: untouched";
    }, [&] {
        EXPECT_EQ(noteAt(score.get(), 1, 5, 1)->pitch(), 67);
    });
    EXPECT_TRUE(out.at("extendedForTies").toBool());
    EXPECT_EQ(outStr("bars"), "m3-m5");
}

TEST_F(Agent_Pitch, TransposeTiedNoteInAList)
{
    open("ties-barlines.mscz");
    Note* g = noteAt(score.get(), 1, 4, 1); // the middle of the G4 chain
    ASSERT_TRUE(g);
    const Params p = trArgs(handles({ makeHandle(g) }), interval("m2", "down"));
    refused(TR, p, AErr::InvalidArg, "is tied to");
    ASSERT_TRUE(run(TR, with(p, "ties", Val("extend"))));
    EXPECT_EQ(noteAt(score.get(), 1, 3, 4)->pitch(), 66);
    EXPECT_EQ(g->pitch(), 66);
    EXPECT_EQ(noteAt(score.get(), 1, 5, 1)->pitch(), 66);
    EXPECT_EQ(outInt("notes"), 3);
}

TEST_F(Agent_Pitch, TransposeRangeErrors)
{
    const Val by = interval("M2", "up");
    Params noRange;
    noRange["by"] = by;
    refused(TR, noRange, AErr::InvalidArg, "missing range");
    refused(TR, trArgs(Val("score"), by), AErr::InvalidArg, "range must be an object");
    refused(TR, trArgs(Val(5), by), AErr::InvalidArg, "range must be");
    ValMap onlyEnd;
    onlyEnd["end"] = pos(1);
    refused(TR, trArgs(Val(onlyEnd), by), AErr::InvalidArg, "range.start is required");
    ValMap notObj;
    notObj["start"] = Val(1);
    notObj["end"] = pos(1);
    refused(TR, trArgs(Val(notObj), by), AErr::InvalidArg, "must be an object");
    refused(TR, trArgs(rangeOf(withKey(pos(1), "staff", Val(1)), pos(1)), by), AErr::InvalidArg, "unknown key");
    ValMap junk;
    junk["start"] = pos(1);
    junk["end"] = pos(1);
    junk["voices"] = Val(1);
    refused(TR, trArgs(Val(junk), by), AErr::InvalidArg, "unknown key 'voices'");
    refused(TR, trArgs(rangeOf(pos(3, 3), pos(3, 1)), by), AErr::InvalidArg, "before range.start");
    refused(TR, trArgs(rangeOf(pos(3, 1.5), pos(3, 4)), by), AErr::Range, "range.start: no note or rest");
    refused(TR, trArgs(rangeOf(pos(3, 1), pos(3, 3.5)), by), AErr::Range, "range.end: no note or rest");
    refused(TR, trArgs(rangeOf(pos(99), pos(99)), by), AErr::Range, "outside the score");
    refused(TR, trArgs(rangeOf(pos(1), pos(1), { 99 }), by), AErr::Range, "outside");
    ValMap emptyStaves;
    emptyStaves["start"] = pos(1);
    emptyStaves["end"] = pos(1);
    emptyStaves["staves"] = Val(ValList {});
    refused(TR, trArgs(Val(emptyStaves), by), AErr::InvalidArg, "non-empty list");
    ValMap both;
    both["start"] = pos(1);
    both["handles"] = Val(ValList { Val(makeHandle(noteAt(score.get(), 1, 1, 1))) });
    refused(TR, trArgs(Val(both), by), AErr::InvalidArg, "handles alone");
    ValMap notScore;
    notScore["score"] = Val(false);
    refused(TR, trArgs(Val(notScore), by), AErr::InvalidArg, "range.score must be true");
    ValMap scoreAndStart;
    scoreAndStart["score"] = Val(true);
    scoreAndStart["start"] = pos(1);
    refused(TR, trArgs(Val(scoreAndStart), by), AErr::InvalidArg, "not both");
    refused(TR, trArgs(handles({}), by), AErr::InvalidArg, "non-empty list");
    ValMap numbers;
    numbers["handles"] = Val(ValList { Val(1) });
    refused(TR, trArgs(Val(numbers), by), AErr::InvalidArg, "handle strings");
    refused(TR, trArgs(handles({ "garbage" }), by), AErr::InvalidArg);
    refused(TR, trArgs(handles({ makeHandle(bar(score.get(), 1)) }), by), AErr::InvalidArg, "transpose takes");
}

TEST_F(Agent_Pitch, TransposeByErrors)
{
    const Val r = rangeOf(pos(1), pos(1), { 1 });
    Params noBy;
    noBy["range"] = r;
    refused(TR, noBy, AErr::InvalidArg, "missing by");
    refused(TR, trArgs(r, Val("M2")), AErr::InvalidArg, "by must be");
    refused(TR, trArgs(r, Val(ValMap {})), AErr::InvalidArg, "exactly one");
    ValMap two;
    two["interval"] = Val("M2");
    two["key"] = Val(2);
    refused(TR, trArgs(r, Val(two)), AErr::InvalidArg, "exactly one");
    refused(TR, trArgs(r, interval("M9", "up")), AErr::InvalidArg, "octaves");
    ValMap noDir;
    noDir["interval"] = Val("M2");
    refused(TR, trArgs(r, Val(noDir)), AErr::InvalidArg, "direction is required");
    refused(TR, trArgs(r, interval("M2", "sideways")), AErr::InvalidArg, "not one of");
    refused(TR, trArgs(r, interval("M2", "closest")), AErr::InvalidArg, "not one of");
    refused(TR, trArgs(r, interval("M2", "up", 4)), AErr::InvalidArg, "0-3");
    refused(TR, trArgs(r, interval("P1", "up")), AErr::InvalidArg, "changes nothing");
    refused(TR, trArgs(r, withKey(interval("M2", "up"), "keep_alterations", Val(true))), AErr::InvalidArg, "does not go with");
    refused(TR, trArgs(r, toKey(8)), AErr::InvalidArg, "-7 to 7");
    refused(TR, trArgs(r, withKey(toKey(1), "key", Val(1.5))), AErr::InvalidArg, "-7 to 7");
    refused(TR, trArgs(r, toKey(1, "sideways")), AErr::InvalidArg, "not one of");
    refused(TR, trArgs(r, withKey(toKey(1), "octaves", Val(1))), AErr::InvalidArg, "does not go with");
    refused(TR, trArgs(r, degrees(0, "up")), AErr::InvalidArg, "1-6");
    refused(TR, trArgs(r, degrees(7, "up")), AErr::InvalidArg, "1-6");
    refused(TR, trArgs(r, withKey(degrees(2, "up"), "keep_alterations", Val("yes"))), AErr::InvalidArg, "true or false");
    Params p = trArgs(r, interval("M2", "up"));
    refused(TR, with(p, "key_signatures", Val("yes")), AErr::InvalidArg, "true or false");
    refused(TR, with(p, "chord_symbols", Val(1)), AErr::InvalidArg, "true or false");
    refused(TR, with(p, "double_accidentals", Val("no")), AErr::InvalidArg, "true or false");
    refused(TR, with(p, "ties", Val("maybe")), AErr::InvalidArg, "reject");
    refused(TR, with(trArgs(r, degrees(2, "up")), "key_signatures", Val(true)), AErr::InvalidArg, "keep_alterations");
}

TEST_F(Agent_Pitch, TransposeListErrors)
{
    const Val list = handles({ makeHandle(noteAt(score.get(), 1, 1, 1)) });
    refused(TR, trArgs(list, toKey(2)), AErr::InvalidArg, "needs a range");
    refused(TR, with(trArgs(list, interval("M2", "up")), "key_signatures", Val(true)), AErr::InvalidArg, "needs a range");
    open("chord-symbols.mscz");
    Harmony* h = firstHarmony(score.get());
    ASSERT_TRUE(h);
    Params p = trArgs(handles({ makeHandle(h) }), interval("M2", "up"));
    p["chord_symbols"] = Val(false);
    refused(TR, p, AErr::InvalidArg, "chord_symbols is false");
}

TEST_F(Agent_Pitch, TransposeListOfNotes)
{
    Note* a = noteAt(score.get(), 1, 1, 1);
    Note* b = noteAt(score.get(), 1, 1, 2);
    Note* c = noteAt(score.get(), 1, 1, 3);
    addsOneEntry(TR, trArgs(handles({ makeHandle(a), makeHandle(b->chord()) }), interval("M2", "up")), [&] {
        EXPECT_EQ(a->pitch(), 62);
        EXPECT_EQ(b->pitch(), 64);
        EXPECT_EQ(c->pitch(), 64);
    }, [&] {
        EXPECT_EQ(a->pitch(), 60);
        EXPECT_EQ(b->pitch(), 62);
    });
    EXPECT_EQ(outInt("notes"), 2);
}

TEST_F(Agent_Pitch, TransposeLinkedParts)
{
    open("parts-linked.mscz");
    Note* n = noteAt(score.get(), 1, 1, 1);
    ASSERT_GE(n->linkList().size(), 2u);
    Params p = trArgs(rangeOf(pos(1), pos(1), { 1 }), interval("M2", "up"));
    p["key_signatures"] = Val(false);
    addsOneEntry(TR, p, [&] {
        EXPECT_EQ(pitchesOf(score.get(), 1, 1, { 1, 2, 3, 4 }), (std::vector<int> { 78, 79, 81, 83 }));
        for (EngravingObject* e : n->linkList()) {
            EXPECT_EQ(toNote(e)->pitch(), 78);
            EXPECT_EQ(toNote(e)->tpc1(), Tpc::TPC_F_S);
        }
    }, [&] {
        for (EngravingObject* e : n->linkList()) {
            EXPECT_EQ(toNote(e)->pitch(), 76);
        }
    });
}

TEST_F(Agent_Pitch, TransposeLinkedTransposingPart)
{
    open("parts-linked.mscz");
    Note* n = noteAt(score.get(), 2, 1, 2); // clarinet: sounds E5, written F#5
    ASSERT_TRUE(run(TR, trArgs(rangeOf(pos(1), pos(1), { 2 }), interval("M2", "up"))));
    for (EngravingObject* e : n->linkList()) {
        EXPECT_EQ(toNote(e)->pitch(), 78);
        EXPECT_EQ(toNote(e)->tpc1(), Tpc::TPC_F_S);
        EXPECT_EQ(toNote(e)->tpc2(), Tpc::TPC_G_S);
    }
}

TEST_F(Agent_Pitch, TransposeIgnoresAndRestoresTheSelectionFilter)
{
    open("voices-four.mscz");
    score->selectionFilter().setFiltered(VoicesSelectionFilterTypes::SECOND_VOICE, false);
    const SelectionFilter userFilter = score->selectionFilter();
    Note* v2 = noteAt(score.get(), 1, 1, 1, 0, 2);
    ASSERT_TRUE(v2);
    ASSERT_EQ(v2->pitch(), 81);
    ASSERT_TRUE(run(TR, trArgs(rangeOf(pos(1), pos(1)), interval("M2", "up"))));
    EXPECT_EQ(v2->pitch(), 83) << "voice 2 transposed although the user's filter leaves it out";
    EXPECT_TRUE(score->selectionFilter() == userFilter) << "the user's filter is back";
}

TEST_F(Agent_Pitch, TransposeChordSymbols)
{
    open("chord-symbols.mscz");
    Harmony* h = firstHarmony(score.get());
    ASSERT_TRUE(h);
    const int root = h->rootTpc();
    ASSERT_EQ(root, Tpc::TPC_C);
    Params p = trArgs(scoreRange(), interval("M2", "up"));
    addsOneEntry(TR, p, [&] { EXPECT_EQ(h->rootTpc(), Tpc::TPC_D); }, [&] { EXPECT_EQ(h->rootTpc(), Tpc::TPC_C); });
    EXPECT_GE(outInt("chordSymbols"), 1);
    score->undoRedo(true, nullptr);
    p["chord_symbols"] = Val(false);
    ASSERT_TRUE(run(TR, p));
    EXPECT_EQ(h->rootTpc(), Tpc::TPC_C) << "chord_symbols false leaves them";
    EXPECT_EQ(outInt("chordSymbols"), 0);
}

TEST_F(Agent_Pitch, TransposeChordSymbolHandlesOnly)
{
    open("chord-symbols.mscz");
    Harmony* h = firstHarmony(score.get());
    ASSERT_TRUE(h);
    Note* n = noteAt(score.get(), 1, 1, 1);
    const int pitch = n->pitch();
    ASSERT_TRUE(run(TR, trArgs(handles({ makeHandle(h) }), interval("M2", "up"))));
    EXPECT_EQ(h->rootTpc(), Tpc::TPC_D);
    EXPECT_EQ(n->pitch(), pitch);
    EXPECT_EQ(outInt("notes"), 0);
    EXPECT_EQ(outInt("chordSymbols"), 1);
}

TEST_F(Agent_Pitch, TransposeSkipsPercussionStaves)
{
    // test set-up only: staff 6 (clarinet in A) becomes a percussion staff
    score->staff(5)->setStaffType(Fraction(0, 1), *StaffType::preset(StaffTypes::PERC_DEFAULT));
    const std::vector<int> perc = pitchesOf(score.get(), 6, 1, { 1, 2, 3, 4 });
    ASSERT_TRUE(run(TR, trArgs(scoreRange(), interval("M2", "up"))));
    EXPECT_EQ(noteAt(score.get(), 1, 1, 1)->pitch(), 62);
    EXPECT_EQ(pitchesOf(score.get(), 6, 1, { 1, 2, 3, 4 }), perc);
    score->undoRedo(true, nullptr);
    refused(TR, trArgs(rangeOf(pos(1), pos(1), { 6 }), interval("M2", "up")), AErr::InvalidArg, "percussion");
}

TEST_F(Agent_Pitch, TransposeNothingPitched)
{
    open("parts-linked.mscz");
    // clarinet m1 beat 1 is a rest. With key signatures on, the bar's key signature would be
    // transposed (the range starts at the bar start, transpose.cpp:195-200), so turn them off.
    Params p = trArgs(rangeOf(pos(1, 1), pos(1, 1), { 2 }), interval("M2", "up"));
    p["key_signatures"] = Val(false);
    refused(TR, p, AErr::InvalidArg, "nothing to transpose");
}

// ================================================================== review additions

namespace {
struct NoteBefore {
    Note* note = nullptr;
    int pitch = 0;
    int tpc1 = 0;
};

std::vector<NoteBefore> allNotes(MasterScore* score)
{
    std::vector<NoteBefore> v;
    for (Segment* s = score->firstSegment(SegmentType::ChordRest); s; s = s->next1(SegmentType::ChordRest)) {
        for (track_idx_t t = 0; t < score->ntracks(); ++t) {
            EngravingItem* e = s->element(t);
            if (e && e->isChord()) {
                for (Note* n : toChord(e)->notes()) {
                    v.push_back({ n, n->pitch(), n->tpc1() });
                }
            }
        }
    }
    return v;
}
}

//! Keys with many accidentals: Staff::transpose respells the written key of a transposing staff
//! (staff.cpp:861-891) and undoChangeKeySig respells tpc2 through transpositionChanged
//! (edit.cpp:6078-6084). The post-condition must accept that written spelling.
TEST_F(Agent_Pitch, TransposeToFarKeysOnTransposingInstruments)
{
    struct Case {
        int key;
        Interval interval;
    };
    for (const Case& c : { Case { 4, Interval(2, 4) }, Case { 5, Interval(-1, -1) } }) {
        open("transposing-instruments.mscz");
        const std::vector<NoteBefore> before = allNotes(score.get());
        ASSERT_FALSE(before.empty());
        const Ret r = run(TR, trArgs(scoreRange(), toKey(c.key)));
        ASSERT_TRUE(r) << "key " << c.key << ": " << r.text();
        for (staff_idx_t s = 0; s < score->nstaves(); ++s) {
            EXPECT_EQ(int(score->staff(s)->concertKey(Fraction(0, 1))), c.key) << "staff " << s + 1;
        }
        for (const NoteBefore& b : before) {
            Note* n = b.note;
            EXPECT_EQ(n->pitch(), b.pitch + c.interval.chromatic);
            EXPECT_EQ(n->tpc1(), Transpose::transposeTpc(b.tpc1, c.interval, true));
            // the written spelling names the written pitch, with at most a double accidental
            const int written = n->pitch() - n->staff()->transpose(n->tick()).chromatic;
            EXPECT_EQ(((tpc2pitch(n->tpc2()) - written) % 12 + 12) % 12, 0) << "staff " << n->staffIdx() + 1;
            EXPECT_LE(std::abs(static_cast<int>(tpc2alter(n->tpc2()))), 2);
        }
        // the Eb alto sax (staff 4) reads in another key than the concert one
        EXPECT_NE(int(score->staff(3)->key(Fraction(0, 1))), c.key);
        score->undoRedo(true, nullptr);
        for (const NoteBefore& b : before) {
            EXPECT_EQ(b.note->pitch(), b.pitch);
            EXPECT_EQ(b.note->tpc1(), b.tpc1);
        }
    }
}

TEST_F(Agent_Pitch, TransposeKeyChangeInsideTheRange)
{
    open("key-time-changes.mscz");
    // bar 3 changes to G major; bar 6 to Eb major
    const Fraction t1 = bar(score.get(), 1)->tick();
    const Fraction t3 = bar(score.get(), 3)->tick();
    const Fraction t6 = bar(score.get(), 6)->tick();
    ASSERT_EQ(int(score->staff(0)->concertKey(t3)), 1);
    ASSERT_EQ(int(score->staff(0)->concertKey(t6)), -3);
    addsOneEntry(TR, trArgs(rangeOf(pos(2), pos(4)), interval("M2", "up")), [&] {
        for (staff_idx_t s = 0; s < 2; ++s) {
            EXPECT_EQ(int(score->staff(s)->concertKey(t3)), 3) << "G -> A at bar 3, staff " << s + 1;
            EXPECT_EQ(int(score->staff(s)->concertKey(t1)), 0) << "bar 1 is outside the range";
            EXPECT_EQ(int(score->staff(s)->concertKey(t6)), -3) << "the key change at bar 6 is outside the range";
        }
        EXPECT_EQ(noteAt(score.get(), 1, 3, 1)->pitch(), 71);
        EXPECT_EQ(noteAt(score.get(), 1, 5, 1)->pitch(), 64) << "bar 5 untouched";
    }, [&] {
        EXPECT_EQ(int(score->staff(0)->concertKey(t3)), 1);
        EXPECT_EQ(noteAt(score.get(), 1, 3, 1)->pitch(), 69);
    });
    EXPECT_GE(outInt("keySignatures"), 2);
}

TEST_F(Agent_Pitch, LinkedTabStaffIsRefretted)
{
    open("ties-barlines.mscz");
    // test set-up only: a guitar with a TAB staff linked to its standard staff ("Add linked staff",
    // as expression_lines_tests.cpp does); the set-up command's layout frets the TAB notes
    Staff* original = score->staff(0);
    int strings[6] = { 40, 45, 50, 55, 59, 64 };
    original->part()->instrument()->setStringData(StringData(24, 6, strings));
    score->startCmd(muse::TranslatableString::untranslatable("test: linked TAB staff"));
    Staff* tabStaff = Factory::createStaff(original->part());
    tabStaff->setPart(original->part());
    score->undoInsertStaff(tabStaff, 1, false);
    Excerpt::cloneStaff(original, tabStaff);
    tabStaff->setStaffType(Fraction(0, 1), *StaffType::preset(StaffTypes::TAB_6COMMON));
    score->endCmd();
    ASSERT_EQ(score->nstaves(), 2u);
    ASSERT_TRUE(tabStaff->isTabStaff(Fraction(0, 1)));
    Note* tabNote = noteAt(score.get(), 2, 1, 1);
    ASSERT_TRUE(tabNote);
    auto fretted = [&](int pitch) {
        EXPECT_EQ(tabNote->pitch(), pitch);
        const Fraction tick = tabNote->chord()->tick();
        const StringData* sd = tabStaff->part()->stringData(tick, tabStaff->idx());
        ASSERT_TRUE(sd);
        EXPECT_EQ(sd->getPitch(tabNote->string(), tabNote->fret(), tabStaff, tick), pitch)
            << "string " << tabNote->string() << " fret " << tabNote->fret();
        EXPECT_FALSE(tabNote->fretConflict());
    };
    fretted(72);
    addsOneEntry(SET, setArgs(where(1, 1, 1), Val("A4")), [&] { fretted(69); }, [&] { fretted(72); });
    addsOneEntry(TR, trArgs(rangeOf(pos(1, 1), pos(1, 1), { 1 }), interval("M2", "up")), [&] { fretted(71); },
                 [&] { fretted(69); });
}

TEST_F(Agent_Pitch, FretDiagramWithUnknownShapeLeavesNoStrayChordSymbol)
{
    open("ties-barlines.mscz");
    // test set-up only: an E major fret diagram with no chord symbol on bar 1. Up an augmented
    // unison it would be E#, which harmony_to_diagram.xml has no shape for: the engine's
    // no-chord-symbol branch then returns with its temporary Harmony attached (transpose.cpp:626-629)
    Segment* seg = bar(score.get(), 1)->first(SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    score->startCmd(muse::TranslatableString::untranslatable("test: fret diagram"));
    FretDiagram* fd = Factory::createFretDiagram(seg);
    fd->setTrack(0);
    fd->setParent(seg);
    fd->updateDiagram(u"E");
    score->undoAddElement(fd);
    score->endCmd();
    ASSERT_FALSE(fd->isClear()) << "the E shape was found";
    ASSERT_FALSE(fd->harmony());
    ASSERT_TRUE(fd->patternsFromHarmony(u"E#").empty()) << "the premise: no E# shape";
    const muse::String shape = fd->patternFromDiagram();

    Params p = trArgs(rangeOf(pos(1), pos(1), { 1 }), interval("A1", "up"));
    p["key_signatures"] = Val(false);
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: test"));
    const Ret r = tx.step("op", [&] { return TR(p, score.get(), out); });
    ASSERT_TRUE(r) << r.text();
    bool warned = false;
    for (const std::string& w : tx.warnings()) {
        warned = warned || w.find("TRANSPOSE_NO_FRET_DIAGRAM") != std::string::npos;
    }
    EXPECT_TRUE(warned) << "the unknown shape is reported as a warning";
    EXPECT_FALSE(fd->harmony()) << "the engine's temporary chord symbol was taken off";
    tx.rollback();
    EXPECT_FALSE(fd->harmony()) << "no stray chord symbol after rollback";
    EXPECT_EQ(fd->patternFromDiagram(), shape) << "rollback restores the diagram";
    EXPECT_EQ(noteAt(score.get(), 1, 1, 1)->pitch(), 72);
}

TEST_F(Agent_Pitch, TieExtendThroughARepeatIsRefused)
{
    // an upstream fixture with partial ties into voltas and a coda (partialtie_tests.cpp)
    const fs::path f = fs::path(agent_tests_DATA_ROOT) / "../../engraving/tests/partialtie_data/volta_coda-ref.mscx";
    score.reset(ScoreRW::readScore(muse::String::fromStdString(f.string()), true));
    ASSERT_TRUE(score);
    backend = std::make_unique<ScoreUndoBackend>(score.get());
    Note* member = nullptr;
    for (const NoteBefore& b : allNotes(score.get())) {
        const std::vector<Note*> chain = b.note->tiedNotes();
        bool partial = false;
        for (Note* x : chain) {
            partial = partial || (x->tieFor() && x->tieFor()->isPartialTie()) || (x->tieBack() && x->tieBack()->isPartialTie());
        }
        if (partial && chain.size() > 1) {
            member = b.note;
            break;
        }
    }
    ASSERT_TRUE(member) << "the fixture has a tie chain through a jump";
    const Params p = trArgs(handles({ makeHandle(member) }), interval("M2", "up"));
    refused(TR, p, AErr::InvalidArg, "is tied to");
    refused(TR, with(p, "ties", Val("extend")), AErr::InvalidArg, "partial ties");
}
