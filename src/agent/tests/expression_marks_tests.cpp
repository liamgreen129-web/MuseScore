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
// P2.4 expression-marks: add_dynamic, add_hairpin, add_articulation, add_ornament, add_fermata,
// add_breath. Each operation runs inside a core Transaction on a synthetic fixture with the
// score's own undo stack (as core_tests.cpp), so every validation error is checked to leave
// the score and the undo stack untouched, and every edit is undone and redone.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engraving/dom/articulation.h"
#include "engraving/dom/barline.h"
#include "engraving/dom/breath.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/fermata.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/select.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tapping.h"
#include "engraving/editing/undo.h"
#include "engraving/tests/utils/scorerw.h"

#include "commands/expression-marks/expressionmarks.h"
#include "core/handles.h"
#include "core/transaction.h"
#include "core/undobackend.h"
#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::agent::core;
using namespace mu::engraving;
using muse::Ret;
using muse::Val;
using muse::ValMap;
namespace em = mu::agent::expression_marks;
namespace fs = std::filesystem;
using AErr = mu::agent::Err;

namespace {
using Op = std::function<Ret(const Params&, MasterScore*, em::Out&)>;

MasterScore* load(const char* name)
{
    const fs::path p = fs::path(agent_tests_DATA_ROOT) / "../../../tests/fixtures/synthetic" / name;
    return ScoreRW::readScore(muse::String::fromStdString(p.string()), true);
}

int barOf(MasterScore* score, const Measure* target)
{
    int i = 0;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        ++i;
        if (m == target) {
            return i;
        }
    }
    return 0;
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

//! The first chord or rest on a track that matches
ChordRest* find(MasterScore* score, track_idx_t track, const std::function<bool(ChordRest*)>& pred)
{
    for (Segment* s = score->firstSegment(SegmentType::ChordRest); s; s = s->next1(SegmentType::ChordRest)) {
        EngravingItem* e = s->element(track);
        if (e && e->isChordRest() && pred(toChordRest(e))) {
            return toChordRest(e);
        }
    }
    return nullptr;
}

ChordRest* chordAt(MasterScore* score, track_idx_t track, int skip = 0)
{
    return find(score, track, [&](ChordRest* cr) { return cr->isChord() && skip-- == 0; });
}

ChordRest* restAt(MasterScore* score, track_idx_t track)
{
    return find(score, track, [](ChordRest* cr) { return cr->isRest() && !toRest(cr)->isGap() && !cr->isMeasureRepeat(); });
}

Val pos(MasterScore* score, const ChordRest* cr)
{
    ValMap m;
    m["measure"] = Val(barOf(score, cr->measure()));
    m["offset"] = Val((cr->tick() - cr->measure()->tick()).reduced().toString().toStdString());
    return Val(m);
}

Val posBeat(int measure, double beat)
{
    ValMap m;
    m["measure"] = Val(measure);
    m["beat"] = Val(beat);
    return Val(m);
}

Val staffNo(int n)
{
    ValMap m;
    m["number"] = Val(n);
    return Val(m);
}

Val staffPart(const std::string& part, int index = 0)
{
    ValMap m;
    m["part"] = Val(part);
    if (index) {
        m["index"] = Val(index);
    }
    return Val(m);
}

//! staff + at (+ voice) addressing a chord or rest
Params at(MasterScore* score, const ChordRest* cr, const char* key = "at")
{
    Params p;
    p["staff"] = staffNo(static_cast<int>(cr->staffIdx()) + 1);
    p[key] = pos(score, cr);
    p["voice"] = Val(static_cast<int>(cr->voice()) + 1);
    return p;
}

Params with(Params p, const std::string& k, const Val& v)
{
    p[k] = v;
    return p;
}

int code(const Ret& r)
{
    return r.code();
}

int err(AErr e)
{
    return static_cast<int>(e);
}

int dynamicsAt(Segment* seg, track_idx_t track)
{
    int n = 0;
    for (EngravingItem* a : seg->annotations()) {
        n += a->isDynamic() && a->track() == track;
    }
    return n;
}

int fermatasAt(Segment* seg, track_idx_t track)
{
    int n = 0;
    for (EngravingItem* a : seg->annotations()) {
        n += a->isFermata() && a->track() == track;
    }
    return n;
}

int hairpins(MasterScore* score)
{
    int n = 0;
    for (const auto& [tick, sp] : score->spanner()) {
        n += sp->isHairpin();
    }
    return n;
}

class Agent_ExpressionMarks : public ::testing::Test
{
protected:
    void open(const char* name)
    {
        score.reset(load(name));
        ASSERT_TRUE(score);
        backend = std::make_unique<ScoreUndoBackend>(score.get());
        MScore::setError(MsError::MS_NO_ERROR);
    }

    void SetUp() override { open("ties-barlines.mscz"); }

    //! One request: a transaction around the op. Returns the op's (or commit's) result.
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
        return r;
    }

    //! The score as saved (.mscx text): what "nothing changed" is compared on
    std::string snapshot()
    {
        const fs::path f = fs::temp_directory_path() / "agent_expression_marks_snapshot.mscx";
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
        EXPECT_EQ(snapshot(), saved) << "a refused request changed the score";
        EXPECT_FALSE(r) << "expected " << errName(e);
        EXPECT_EQ(code(r), err(e)) << r.text();
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

    std::unique_ptr<MasterScore> score;
    std::unique_ptr<ScoreUndoBackend> backend;
    em::Out out;
    bool changed = false;
};

const Op DYN = em::addDynamic;
const Op HP = em::addHairpin;
const Op ART = em::addArticulation;
const Op ORN = em::addOrnament;
const Op FER = em::addFermata;
const Op BR = em::addBreath;
}

// ================================================================== addressing (shared by all six)

TEST_F(Agent_ExpressionMarks, StaffErrors)
{
    ChordRest* cr = chordAt(score.get(), 0);
    ASSERT_TRUE(cr);
    const Params base = with(at(score.get(), cr), "dynamic", Val("p"));
    Params noStaff = base;
    noStaff.erase("staff");
    refused(DYN, noStaff, AErr::InvalidArg, "staff");
    refused(DYN, with(base, "staff", Val(1)), AErr::InvalidArg, "object");
    ValMap junk;
    junk["name"] = Val("x");
    refused(DYN, with(base, "staff", Val(junk)), AErr::InvalidArg, "unknown key");
    refused(DYN, with(base, "staff", staffNo(99)), AErr::Range, "outside");
    refused(DYN, with(base, "staff", staffNo(0)), AErr::Range);
    refused(DYN, with(base, "staff", staffPart("No Such Part")), AErr::Range, "no part named");
    const std::string partName = score->parts().front()->partName().toStdString();
    refused(DYN, with(base, "staff", staffPart(partName, 9)), AErr::Range, "staff(s)");
    ValMap both;
    both["number"] = Val(1);
    both["part"] = Val(partName);
    refused(DYN, with(base, "staff", Val(both)), AErr::InvalidArg, "either");
    ValMap badIndex;
    badIndex["part"] = Val(partName);
    badIndex["index"] = Val(0);
    refused(DYN, with(base, "staff", Val(badIndex)), AErr::InvalidArg, "index");
    // by part name (case-insensitive) works
    std::string upper = partName;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    EXPECT_TRUE(run(DYN, with(base, "staff", staffPart(upper)))) << partName;
}

TEST_F(Agent_ExpressionMarks, PositionErrors)
{
    Params base;
    base["staff"] = staffNo(1);
    base["dynamic"] = Val("p");
    refused(DYN, base, AErr::InvalidArg, "missing 'at'");
    refused(DYN, with(base, "at", Val(3)), AErr::InvalidArg, "object");
    ValMap extra;
    extra["measure"] = Val(1);
    extra["beat"] = Val(1);
    extra["tick"] = Val(0);
    refused(DYN, with(base, "at", Val(extra)), AErr::InvalidArg, "unknown key");
    ValMap noMeasure;
    noMeasure["beat"] = Val(1);
    refused(DYN, with(base, "at", Val(noMeasure)), AErr::InvalidArg, "measure");
    ValMap both;
    both["measure"] = Val(1);
    both["beat"] = Val(1);
    both["offset"] = Val("0/1");
    refused(DYN, with(base, "at", Val(both)), AErr::InvalidArg, "exactly one");
    ValMap neither;
    neither["measure"] = Val(1);
    refused(DYN, with(base, "at", Val(neither)), AErr::InvalidArg, "exactly one");
    refused(DYN, with(base, "at", posBeat(999, 1)), AErr::Range, "outside the score");
    refused(DYN, with(base, "at", posBeat(0, 1)), AErr::Range, "outside the score");
    refused(DYN, with(base, "at", posBeat(1, 0.5)), AErr::InvalidArg, "from 1");
    refused(DYN, with(base, "at", posBeat(1, 1.3333)), AErr::InvalidArg, "offset");
    refused(DYN, with(base, "at", posBeat(1, 99)), AErr::Range, "past its end");
    ValMap badOffset;
    badOffset["measure"] = Val(1);
    badOffset["offset"] = Val("three eighths");
    refused(DYN, with(base, "at", Val(badOffset)), AErr::InvalidArg, "fraction");
    // a beat where nothing starts (between the first two events of bar 1), named E_NO_CHORDREST in the spec
    Measure* m1 = bar(score.get(), 1);
    Segment* s1 = m1->first(SegmentType::ChordRest);
    ChordRest* first = toChordRest(s1->element(0));
    ASSERT_TRUE(first);
    ValMap between;
    between["measure"] = Val(1);
    between["offset"] = Val((first->ticks() / Fraction(2, 1)).reduced().toString().toStdString());
    refused(DYN, with(base, "at", Val(between)), AErr::Range, "no note or rest starts");
    // voice 3 is empty in bar 1
    refused(DYN, with(with(base, "at", posBeat(1, 1)), "voice", Val(3)), AErr::Range, "voice 3");
}

TEST_F(Agent_ExpressionMarks, TargetErrors)
{
    ChordRest* cr = chordAt(score.get(), 0);
    Params p;
    p["dynamic"] = Val("p");
    p["target"] = Val(makeHandle(cr));
    refused(DYN, with(p, "staff", staffNo(1)), AErr::InvalidArg, "not both");
    refused(DYN, with(p, "target", Val("garbage")), AErr::InvalidArg, "malformed");
    refused(DYN, with(p, "target", Val(3)), AErr::InvalidArg, "handle");
    // a handle to the bar itself is not a chord or rest
    refused(DYN, with(p, "target", Val(makeHandle(cr->measure()))), AErr::InvalidArg, "names a");
    // a stale handle: right place and type, wrong fingerprint
    std::string stale = makeHandle(cr);
    stale = stale.substr(0, stale.find('#')) + "#1/64";
    refused(DYN, with(p, "target", Val(stale)), AErr::StaleHandle);
    // a note handle means its chord
    p["target"] = Val(makeHandle(toChord(cr)->notes().front()));
    p["voice_assignment"] = Val("this_voice");
    ASSERT_TRUE(run(DYN, p));
    EXPECT_EQ(dynamicsAt(cr->segment(), cr->track()), 1);
}

// ================================================================== add_dynamic

TEST_F(Agent_ExpressionMarks, DynamicValidation)
{
    ChordRest* cr = chordAt(score.get(), 0);
    const Params base = at(score.get(), cr);
    refused(DYN, base, AErr::InvalidArg, "either dynamic");
    refused(DYN, with(with(base, "dynamic", Val("p")), "text", Val("p")), AErr::InvalidArg, "either dynamic");
    refused(DYN, with(base, "dynamic", Val("mezzo")), AErr::InvalidArg, "unknown dynamic");
    refused(DYN, with(base, "text", Val("  ")), AErr::InvalidArg, "empty");
    refused(DYN, with(base, "text", Val("dolce")), AErr::InvalidArg, "add_text");
    ASSERT_TRUE(run(DYN, with(base, "dynamic", Val("mf"))));
    refused(DYN, with(with(base, "dynamic", Val("p")), "if_exists", Val("error")), AErr::Exists, "already has a dynamic");
}

TEST_F(Agent_ExpressionMarks, DynamicAddUndoRedo)
{
    ChordRest* cr = chordAt(score.get(), 0, 1);
    Segment* seg = cr->segment();
    const Params p = with(with(at(score.get(), cr), "dynamic", Val("mf")), "voice_assignment", Val("this_voice"));
    addsOneEntry(DYN, p,
                 [&] {
        ASSERT_EQ(dynamicsAt(seg, cr->track()), 1);
        Dynamic* d = nullptr;
        for (EngravingItem* a : seg->annotations()) {
            if (a->isDynamic()) {
                d = toDynamic(a);
            }
        }
        ASSERT_TRUE(d);
        EXPECT_EQ(d->dynamicType(), DynamicType::MF);
        EXPECT_EQ(d->xmlText(), Dynamic::dynamicText(DynamicType::MF));
        EXPECT_EQ(d->getProperty(Pid::VOICE_ASSIGNMENT).value<VoiceAssignment>(), VoiceAssignment::CURRENT_VOICE_ONLY);
    },
                 [&] { EXPECT_EQ(dynamicsAt(seg, cr->track()), 0); });
    EXPECT_EQ(outStr("status"), "added");
    EXPECT_EQ(outStr("dynamic"), "mf");
    EXPECT_EQ(outStr("voiceAssignment"), "this_voice");
    EXPECT_NE(outStr("notation").find("+mf"), std::string::npos) << outStr("notation");
    // the handle resolves to the dynamic
    muse::RetVal<EngravingItem*> h = resolveHandle(score.get(), outStr("handle"));
    ASSERT_TRUE(h.ret) << h.ret.text();
    EXPECT_TRUE(h.val->isDynamic());
}

TEST_F(Agent_ExpressionMarks, DynamicReplaceAndUnchanged)
{
    ChordRest* cr = chordAt(score.get(), 0);
    Segment* seg = cr->segment();
    const Params base = with(at(score.get(), cr), "voice_assignment", Val("this_voice"));
    ASSERT_TRUE(run(DYN, with(base, "dynamic", Val("mf"))));
    const size_t idx = backend->currentIndex();
    // same dynamic again: no change, no undo entry
    ASSERT_TRUE(run(DYN, with(base, "dynamic", Val("mf"))));
    EXPECT_EQ(outStr("status"), "unchanged");
    EXPECT_FALSE(changed);
    EXPECT_EQ(backend->currentIndex(), idx);
    // replace (default): one dynamic, new type; one undo restores mf
    addsOneEntry(DYN, with(with(base, "dynamic", Val("f")), "velocity", Val(100)),
                 [&] {
        ASSERT_EQ(dynamicsAt(seg, cr->track()), 1);
        Dynamic* d = toDynamic(seg->findAnnotation(ElementType::DYNAMIC, cr->track(), cr->track()));
        EXPECT_EQ(d->dynamicType(), DynamicType::F);
        EXPECT_EQ(d->velocity(), 100);
    },
                 [&] {
        ASSERT_EQ(dynamicsAt(seg, cr->track()), 1);
        Dynamic* d = toDynamic(seg->findAnnotation(ElementType::DYNAMIC, cr->track(), cr->track()));
        EXPECT_EQ(d->dynamicType(), DynamicType::MF);
        EXPECT_EQ(d->velocity(), 80);
    });
    EXPECT_EQ(outStr("status"), "replaced");
}

TEST_F(Agent_ExpressionMarks, DynamicCustomTextAndRest)
{
    ChordRest* cr = chordAt(score.get(), 0);
    ASSERT_TRUE(run(DYN, with(with(at(score.get(), cr), "text", Val("subito p")), "voice_assignment", Val("this_voice"))));
    Dynamic* d = toDynamic(cr->segment()->findAnnotation(ElementType::DYNAMIC, cr->track(), cr->track()));
    ASSERT_TRUE(d);
    EXPECT_EQ(d->dynamicType(), DynamicType::P);
    EXPECT_EQ(d->xmlText(), u"subito <sym>dynamicPiano</sym>");

    open("voices-four.mscz");
    ChordRest* rest = restAt(score.get(), 3);
    ASSERT_TRUE(rest) << "voices-four has rests in voice 4";
    ASSERT_TRUE(run(DYN, with(at(score.get(), rest), "dynamic", Val("pp"))));
    EXPECT_EQ(outStr("status"), "added");
}

TEST_F(Agent_ExpressionMarks, DynamicVoiceAssignment)
{
    open("voices-four.mscz");
    ChordRest* v2 = chordAt(score.get(), 1);
    ASSERT_TRUE(v2);
    Segment* seg = v2->segment();
    ASSERT_TRUE(run(DYN, with(with(at(score.get(), v2), "dynamic", Val("p")), "voice_assignment", Val("all_voices"))));
    EXPECT_EQ(dynamicsAt(seg, 0), 1); // moved to voice 1 of the staff, as the palette does
    EXPECT_EQ(dynamicsAt(seg, 1), 0);
    EXPECT_EQ(outStr("voiceAssignment"), "all_voices");
    ASSERT_TRUE(run(DYN, with(with(at(score.get(), v2), "dynamic", Val("f")), "voice_assignment", Val("this_voice"))));
    EXPECT_EQ(dynamicsAt(seg, 1), 1);
    EXPECT_EQ(dynamicsAt(seg, 0), 1);
    // "preference" follows the configuration (the test mock returns false: this voice only)
    ChordRest* v3 = chordAt(score.get(), 2);
    ASSERT_TRUE(run(DYN, with(with(at(score.get(), v3), "dynamic", Val("mp")), "voice_assignment", Val("preference"))));
    EXPECT_EQ(outStr("voiceAssignment"), "this_voice");
    EXPECT_EQ(dynamicsAt(v3->segment(), v3->track()), 1);
    // the default is all_voices whatever the app setting (MuseScore's factory default)
    ChordRest* v4 = find(score.get(), 3, [](ChordRest* c) {
        return c->isChord() && !c->segment()->findAnnotation(ElementType::DYNAMIC, 0, 3);
    });
    ASSERT_TRUE(v4);
    ASSERT_TRUE(run(DYN, with(at(score.get(), v4), "dynamic", Val("pp"))));
    EXPECT_EQ(outStr("voiceAssignment"), "all_voices");
    EXPECT_EQ(dynamicsAt(v4->segment(), 0), 1);
    EXPECT_EQ(dynamicsAt(v4->segment(), v4->track()), 0);
}

TEST_F(Agent_ExpressionMarks, DynamicOnGraceMeansMainNote)
{
    open("grace-notes.mscz");
    ChordRest* main = find(score.get(), 0, [](ChordRest* cr) { return cr->isChord() && !toChord(cr)->graceNotes().empty(); });
    ASSERT_TRUE(main);
    Chord* grace = toChord(main)->graceNotes().front();
    Params p;
    p["target"] = Val(makeHandle(grace));
    p["dynamic"] = Val("sfz");
    p["voice_assignment"] = Val("this_voice");
    ASSERT_TRUE(run(DYN, p)) << out.size();
    EXPECT_EQ(dynamicsAt(main->segment(), main->track()), 1);
}

TEST_F(Agent_ExpressionMarks, DynamicReachesLinkedParts)
{
    open("parts-linked.mscz");
    ASSERT_FALSE(score->excerpts().empty());
    ChordRest* cr = find(score.get(), 0, [](ChordRest* c) {
        return c->isChord() && c->tick().isNotZero() && !c->segment()->findAnnotation(ElementType::DYNAMIC, 0, 3);
    });
    ASSERT_TRUE(cr);
    ASSERT_TRUE(run(DYN, with(at(score.get(), cr), "dynamic", Val("ff"))));
    Dynamic* d = toDynamic(cr->segment()->findAnnotation(ElementType::DYNAMIC, 0, 3));
    ASSERT_TRUE(d);
    EXPECT_GE(d->linkList().size(), 2u) << "a clone in the flute part";
    score->undoRedo(true, nullptr);
    EXPECT_FALSE(cr->segment()->findAnnotation(ElementType::DYNAMIC, 0, 3));
}

// ================================================================== add_hairpin

TEST_F(Agent_ExpressionMarks, HairpinValidation)
{
    open("voices-four.mscz");
    Params p;
    p["kind"] = Val("swell");
    p["staff"] = staffNo(1);
    p["from"] = posBeat(1, 1);
    p["to"] = posBeat(1, 3);
    refused(HP, p, AErr::InvalidArg, "kind");
    p["kind"] = Val("cresc");
    refused(HP, with(p, "to", posBeat(99, 1)), AErr::Range, "outside");
    Params noFrom = p;
    noFrom.erase("from");
    refused(HP, noFrom, AErr::InvalidArg, "missing 'from'");
    refused(HP, with(p, "staff", staffNo(9)), AErr::Range);
    refused(HP, with(with(p, "from", posBeat(2, 1)), "to", posBeat(1, 1)), AErr::InvalidArg, "before");
    ASSERT_TRUE(run(HP, p)) << "cresc m1";
    refused(HP, with(p, "kind", Val("dim")), AErr::Exists, "already covers");
}

TEST_F(Agent_ExpressionMarks, HairpinAddUndoRedo)
{
    ChordRest* a = chordAt(score.get(), 0);
    ChordRest* b = chordAt(score.get(), 0, 2);
    ASSERT_TRUE(a && b);
    Params p;
    p["kind"] = Val("cresc_line");
    p["staff"] = staffNo(1);
    p["from"] = pos(score.get(), a);
    p["to"] = pos(score.get(), b);
    p["voice_assignment"] = Val("this_voice");
    const int before = hairpins(score.get());
    addsOneEntry(HP, p,
                 [&] {
        ASSERT_EQ(hairpins(score.get()), before + 1);
        Hairpin* h = nullptr;
        for (const auto& [tick, sp] : score->spanner()) {
            if (sp->isHairpin() && sp->tick() == a->tick()) {
                h = toHairpin(sp);
            }
        }
        ASSERT_TRUE(h);
        EXPECT_EQ(h->tick2(), b->isChord() ? b->endTick() : b->endTick());
        EXPECT_EQ(h->hairpinType(), HairpinType::CRESC_LINE);
        EXPECT_EQ(h->beginText(), u"cresc.");
        EXPECT_EQ(h->track(), a->track());
    },
                 [&] { EXPECT_EQ(hairpins(score.get()), before); });
    EXPECT_NE(outStr("notation").find("<\"cresc.\""), std::string::npos) << outStr("notation");
    EXPECT_FALSE(outStr("handle").empty());
}

TEST_F(Agent_ExpressionMarks, HairpinIgnoresRangeSelectionFromEarlierStep)
{
    // in a batch an earlier step may leave a range selected; Score::addHairpin would then end the
    // hairpin at the range's end (edit.cpp:4380)
    ChordRest* a = chordAt(score.get(), 0);
    ChordRest* b = chordAt(score.get(), 0, 1);
    Params p;
    p["kind"] = Val("dim");
    p["staff"] = staffNo(1);
    p["from"] = pos(score.get(), a);
    p["to"] = pos(score.get(), b);
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: test"));
    ASSERT_TRUE(tx.step("select", [&] {
        score->selection().setRange(score->firstSegment(SegmentType::ChordRest), score->lastSegment(), 0, 1);
        return muse::make_ok();
    }));
    ASSERT_TRUE(score->selection().isRange());
    ASSERT_TRUE(tx.step("hairpin", [&] { return em::addHairpin(p, score.get(), out); }));
    ASSERT_TRUE(tx.commit());
    bool found = false;
    for (const auto& [tick, sp] : score->spanner()) {
        if (sp->isHairpin() && sp->tick() == a->tick()) {
            EXPECT_EQ(sp->tick2(), b->endTick());
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(Agent_ExpressionMarks, HairpinSingleNoteAndAdjacent)
{
    open("voices-four.mscz");
    Params p;
    p["kind"] = Val("cresc");
    p["staff"] = staffNo(1);
    p["from"] = posBeat(1, 1);
    p["to"] = posBeat(1, 1);
    ASSERT_TRUE(run(HP, p));
    // a dim starting where the cresc ends is not an overlap
    ChordRest* first = toChordRest(bar(score.get(), 1)->first(SegmentType::ChordRest)->element(0));
    ChordRest* next = toChordRest(first->segment()->next(SegmentType::ChordRest)->element(0));
    ASSERT_TRUE(next);
    p["kind"] = Val("dim");
    p["from"] = pos(score.get(), next);
    p["to"] = pos(score.get(), next);
    EXPECT_TRUE(run(HP, p));
}

// ================================================================== add_articulation / add_ornament

TEST_F(Agent_ExpressionMarks, ArticulationValidation)
{
    open("voices-four.mscz");
    ChordRest* c = chordAt(score.get(), 0);
    ChordRest* rest = restAt(score.get(), 3);
    ASSERT_TRUE(c && rest);
    refused(ART, with(at(score.get(), c), "articulation", Val("wobble")), AErr::InvalidArg, "unknown articulation");
    refused(ART, with(at(score.get(), rest), "articulation", Val("staccato")), AErr::InvalidArg, "is a rest");
    ChordRest* c2 = chordAt(score.get(), 0, 1);
    Params span = with(at(score.get(), c2), "articulation", Val("staccato"));
    refused(ART, with(span, "through", pos(score.get(), c)), AErr::InvalidArg, "before");
    refused(ART, with(span, "through", posBeat(99, 1)), AErr::Range, "outside");
}

TEST_F(Agent_ExpressionMarks, ArticulationAddDuplicateUndo)
{
    ChordRest* c = chordAt(score.get(), 0);
    Chord* ch = toChord(c);
    const size_t n0 = ch->articulations().size();
    const Params p = with(at(score.get(), c), "articulation", Val("st"));
    addsOneEntry(ART, p, [&] { EXPECT_EQ(ch->articulations().size(), n0 + 1); }, [&] { EXPECT_EQ(ch->articulations().size(), n0); });
    EXPECT_EQ(out.at("added").toInt(), 1);
    EXPECT_NE(outStr("notation").find("+st"), std::string::npos) << outStr("notation");
    // the same again: left alone, no undo entry (palette semantics, spec Q6)
    const size_t idx = backend->currentIndex();
    ASSERT_TRUE(run(ART, with(p, "articulation", Val("staccato"))));
    EXPECT_EQ(out.at("unchanged").toInt(), 1);
    EXPECT_FALSE(changed);
    EXPECT_EQ(backend->currentIndex(), idx);
    EXPECT_EQ(ch->articulations().size(), n0 + 1);
}

TEST_F(Agent_ExpressionMarks, ArticulationCombinedSplitsOnMarkedChord)
{
    ChordRest* c = chordAt(score.get(), 0);
    Chord* ch = toChord(c);
    ASSERT_TRUE(ch->articulations().empty());
    ASSERT_TRUE(run(ART, with(at(score.get(), c), "articulation", Val("tenuto"))));
    ASSERT_TRUE(run(ART, with(at(score.get(), c), "articulation", Val("accent_staccato"))));
    // tenuto + accent + staccato as separate marks (Score::toggleArticulation split, cmd.cpp:2473-2487)
    EXPECT_EQ(ch->articulations().size(), 3u);
    ASSERT_TRUE(out.at("handles").toList().size() == 2u);
    // on an empty chord the combined symbol is stored whole
    ChordRest* c2 = chordAt(score.get(), 0, 1);
    ASSERT_TRUE(toChord(c2)->articulations().empty());
    ASSERT_TRUE(run(ART, with(at(score.get(), c2), "articulation", Val("accent_staccato"))));
    ASSERT_EQ(toChord(c2)->articulations().size(), 1u);
    EXPECT_EQ(toChord(c2)->articulations().front()->symId(), SymId::articAccentStaccatoAbove);
}

TEST_F(Agent_ExpressionMarks, ArticulationThroughSpan)
{
    ChordRest* a = chordAt(score.get(), 0);
    ChordRest* b = chordAt(score.get(), 0, 3);
    Params p = with(at(score.get(), a), "articulation", Val("tenuto"));
    p["through"] = pos(score.get(), b);
    ASSERT_TRUE(run(ART, p));
    EXPECT_EQ(out.at("added").toInt(), 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(toChord(chordAt(score.get(), 0, i))->articulations().size(), 1u) << i;
    }
    EXPECT_EQ(toChord(chordAt(score.get(), 0, 4))->articulations().size(), 0u);
    // through with a target, and through before at
    Params t;
    t["articulation"] = Val("tenuto");
    t["target"] = Val(makeHandle(a));
    t["through"] = pos(score.get(), b);
    refused(ART, t, AErr::InvalidArg, "through");
    Params back = with(at(score.get(), b), "articulation", Val("accent"));
    back["through"] = pos(score.get(), a);
    refused(ART, back, AErr::InvalidArg, "before");
}

TEST_F(Agent_ExpressionMarks, ArticulationRefusesTappingChord)
{
    ChordRest* c = chordAt(score.get(), 0);
    Chord* ch = toChord(c);
    {
        Transaction tx(score.get(), *backend);
        ASSERT_TRUE(tx.begin("setup"));
        ASSERT_TRUE(tx.step("tap", [&] {
            Tapping* tap = Factory::createTapping(ch);
            tap->setParent(ch);
            tap->setTrack(ch->track());
            score->undoAddElement(tap);
            return muse::make_ok();
        }));
        ASSERT_TRUE(tx.commit());
    }
    ASSERT_TRUE(ch->tapping());
    refused(ART, with(at(score.get(), c), "articulation", Val("up_bow")), AErr::Refused, "tapping");
    refused(ORN, with(at(score.get(), c), "ornament", Val("mordent")), AErr::Refused, "tapping");
    EXPECT_TRUE(ch->tapping());
}

TEST_F(Agent_ExpressionMarks, OrnamentValidationAndAdd)
{
    open("voices-four.mscz");
    ChordRest* c = chordAt(score.get(), 0);
    ChordRest* rest = restAt(score.get(), 3);
    refused(ORN, with(at(score.get(), c), "ornament", Val("wobble")), AErr::InvalidArg, "unknown ornament");
    refused(ORN, with(at(score.get(), rest), "ornament", Val("trill")), AErr::InvalidArg, "is a rest");
    Chord* ch = toChord(c);
    const int spanners = static_cast<int>(score->spanner().size());
    addsOneEntry(ORN, with(at(score.get(), c), "ornament", Val("tr")),
                 [&] {
        ASSERT_EQ(ch->articulations().size(), 1u);
        EXPECT_TRUE(ch->articulations().front()->isOrnament());
        EXPECT_EQ(ch->articulations().front()->symId(), SymId::ornamentTrill);
    },
                 [&] { EXPECT_TRUE(ch->articulations().empty()); });
    EXPECT_EQ(static_cast<int>(score->spanner().size()), spanners) << "a trill symbol, not a trill line";
    EXPECT_NE(outStr("notation").find("+tr"), std::string::npos) << outStr("notation");
    ASSERT_TRUE(run(ORN, with(at(score.get(), c), "ornament", Val("trill"))));
    EXPECT_EQ(out.at("unchanged").toInt(), 1);
    EXPECT_EQ(ch->articulations().size(), 1u);
}

TEST_F(Agent_ExpressionMarks, OrnamentOnGraceChord)
{
    open("grace-notes.mscz");
    ChordRest* main = find(score.get(), 0, [](ChordRest* cr) { return cr->isChord() && !toChord(cr)->graceNotes().empty(); });
    ASSERT_TRUE(main);
    Chord* grace = toChord(main)->graceNotes().front();
    Params p;
    p["target"] = Val(makeHandle(grace));
    p["ornament"] = Val("mordent");
    ASSERT_TRUE(run(ORN, p));
    EXPECT_EQ(grace->articulations().size(), 1u);
    EXPECT_TRUE(toChord(main)->articulations().empty());
}

// ================================================================== add_fermata

TEST_F(Agent_ExpressionMarks, FermataValidation)
{
    ChordRest* c = chordAt(score.get(), 0);
    const Params base = at(score.get(), c);
    refused(FER, with(base, "fermata", Val("eternal")), AErr::InvalidArg, "unknown fermata");
    refused(FER, with(base, "time_stretch", Val(0.0)), AErr::InvalidArg, "greater than 0");
    refused(FER, with(base, "barline", Val(1)), AErr::InvalidArg, "not both");
    Params bl;
    bl["staff"] = staffNo(1);
    bl["barline"] = Val(999);
    refused(FER, bl, AErr::Range, "outside");
    bl["barline"] = Val(1.5);
    refused(FER, bl, AErr::InvalidArg, "number of the bar");
    Params all = with(base, "all_staves", Val(true));
    refused(FER, all, AErr::InvalidArg, "all_staves");
}

TEST_F(Agent_ExpressionMarks, FermataAddReplaceUnchanged)
{
    ChordRest* c = chordAt(score.get(), 0, 2);
    Segment* seg = c->segment();
    addsOneEntry(FER, at(score.get(), c),
                 [&] {
        ASSERT_EQ(fermatasAt(seg, c->track()), 1);
        Fermata* f = toFermata(seg->findAnnotation(ElementType::FERMATA, c->track(), c->track()));
        EXPECT_EQ(f->symId(), SymId::fermataAbove);
        EXPECT_DOUBLE_EQ(f->timeStretch(), 2.0);
        EXPECT_EQ(f->placement(), PlacementV::ABOVE);
    },
                 [&] { EXPECT_EQ(fermatasAt(seg, c->track()), 0); });
    EXPECT_EQ(outStr("status"), "added");
    EXPECT_NE(outStr("notation").find("+ferm"), std::string::npos) << outStr("notation");
    const size_t idx = backend->currentIndex();
    ASSERT_TRUE(run(FER, at(score.get(), c)));
    EXPECT_EQ(outStr("status"), "unchanged");
    EXPECT_EQ(backend->currentIndex(), idx);
    addsOneEntry(FER, with(at(score.get(), c), "fermata", Val("short")),
                 [&] {
        ASSERT_EQ(fermatasAt(seg, c->track()), 1);
        Fermata* f = toFermata(seg->findAnnotation(ElementType::FERMATA, c->track(), c->track()));
        EXPECT_EQ(f->fermataType(), FermataType::Short);
        EXPECT_DOUBLE_EQ(f->timeStretch(), 1.5);
    },
                 [&] {
        ASSERT_EQ(fermatasAt(seg, c->track()), 1);
        EXPECT_EQ(toFermata(seg->findAnnotation(ElementType::FERMATA, c->track(), c->track()))->fermataType(), FermataType::Normal);
    });
    EXPECT_EQ(outStr("status"), "replaced");
    ASSERT_TRUE(run(FER, with(with(at(score.get(), c), "fermata", Val("short")), "time_stretch", Val(2.5))));
    EXPECT_EQ(outStr("status"), "changed");
    EXPECT_DOUBLE_EQ(toFermata(seg->findAnnotation(ElementType::FERMATA, c->track(), c->track()))->timeStretch(), 2.5);
}

TEST_F(Agent_ExpressionMarks, FermataBarlineAllStavesAndVoice2)
{
    open("voices-four.mscz");
    Params bl;
    bl["barline"] = Val(1);
    bl["staff"] = staffNo(1);
    ASSERT_TRUE(run(FER, bl)) << "end barline of bar 1";
    Measure* m1 = bar(score.get(), 1);
    Segment* end = m1->findSegment(SegmentType::EndBarLine, m1->endTick());
    ASSERT_TRUE(end);
    EXPECT_EQ(fermatasAt(end, 0), 1);
    // voice 2 rest or chord: placed below
    ChordRest* v2 = chordAt(score.get(), 1);
    ASSERT_TRUE(run(FER, at(score.get(), v2)));
    Fermata* f = toFermata(v2->segment()->findAnnotation(ElementType::FERMATA, 1, 1));
    ASSERT_TRUE(f);
    EXPECT_EQ(f->placement(), PlacementV::BELOW);
    // all staves at bar 2 beat 1
    Params all;
    all["at"] = posBeat(2, 1);
    all["all_staves"] = Val(true);
    ASSERT_TRUE(run(FER, all));
    EXPECT_EQ(out.at("handles").toList().size() + (out.count("skippedStaves") ? out.at("skippedStaves").toList().size() : 0),
              score->nstaves());
}

// ================================================================== add_breath

TEST_F(Agent_ExpressionMarks, BreathValidation)
{
    ChordRest* c = chordAt(score.get(), 0);
    Params base = at(score.get(), c, "after");
    refused(BR, with(base, "breath", Val("gasp")), AErr::InvalidArg, "unknown breath");
    refused(BR, with(base, "pause", Val(-1.0)), AErr::InvalidArg, "pause");
    Params wrongKey = at(score.get(), c, "at");
    refused(BR, wrongKey, AErr::InvalidArg, "missing 'after'"); // the registry also rejects the unknown 'at'
}

TEST_F(Agent_ExpressionMarks, BreathAddReplaceUnchanged)
{
    ChordRest* c = chordAt(score.get(), 0);
    Measure* m = c->measure();
    const Fraction endTick = c->endTick();
    auto breathHere = [&]() -> Breath* {
        Segment* s = m->findSegment(SegmentType::Breath, endTick);
        EngravingItem* e = s ? s->element(c->track()) : nullptr;
        return e && e->isBreath() ? toBreath(e) : nullptr;
    };
    addsOneEntry(BR, at(score.get(), c, "after"),
                 [&] {
        ASSERT_TRUE(breathHere());
        EXPECT_EQ(breathHere()->symId(), SymId::breathMarkComma);
        EXPECT_DOUBLE_EQ(breathHere()->pause(), 0.0);
    },
                 [&] { EXPECT_FALSE(breathHere()); });
    EXPECT_EQ(outStr("status"), "added");
    EXPECT_NE(outStr("notation").find("+br"), std::string::npos) << outStr("notation");
    const size_t idx = backend->currentIndex();
    ASSERT_TRUE(run(BR, at(score.get(), c, "after")));
    EXPECT_EQ(outStr("status"), "unchanged");
    EXPECT_EQ(backend->currentIndex(), idx);
    addsOneEntry(BR, with(at(score.get(), c, "after"), "breath", Val("caesura")),
                 [&] {
        ASSERT_TRUE(breathHere());
        EXPECT_EQ(breathHere()->symId(), SymId::caesura);
        EXPECT_DOUBLE_EQ(breathHere()->pause(), 2.0);
    },
                 [&] {
        ASSERT_TRUE(breathHere());
        EXPECT_EQ(breathHere()->symId(), SymId::breathMarkComma);
    });
    EXPECT_EQ(outStr("status"), "replaced");
}

TEST_F(Agent_ExpressionMarks, BreathAfterLastNoteOfBarAndGrace)
{
    // the last event of bar 1 ends at the bar's end: the Breath segment stays in bar 1
    Measure* m1 = bar(score.get(), 1);
    ChordRest* last = nullptr;
    for (Segment* s = m1->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
        if (s->element(0)) {
            last = toChordRest(s->element(0));
        }
    }
    ASSERT_TRUE(last);
    ASSERT_TRUE(run(BR, with(at(score.get(), last, "after"), "breath", Val("cae"))));
    Segment* s = m1->findSegment(SegmentType::Breath, m1->endTick());
    ASSERT_TRUE(s);
    EXPECT_TRUE(s->element(0) && s->element(0)->isBreath());

    open("grace-notes.mscz");
    ChordRest* main = find(score.get(), 0, [](ChordRest* cr) { return cr->isChord() && !toChord(cr)->graceNotes().empty(); });
    Params p;
    p["target"] = Val(makeHandle(toChord(main)->graceNotes().front()));
    refused(BR, p, AErr::InvalidArg, "grace");
}

// ================================================================== additions from the P2.4 review

namespace {
int hairpinsIn(const Score* s)
{
    int n = 0;
    for (const auto& [tick, sp] : s->spanner()) {
        n += sp->isHairpin();
    }
    return n;
}

int breathsIn(const Score* s)
{
    int n = 0;
    for (const Segment* seg = s->firstSegment(SegmentType::Breath); seg; seg = seg->next1(SegmentType::Breath)) {
        for (track_idx_t t = 0; t < s->ntracks(); ++t) {
            n += seg->element(t) && seg->element(t)->isBreath();
        }
    }
    return n;
}

//! hairpins / breaths in each part score, in excerpt order
std::vector<int> perPart(MasterScore* score, int (*count)(const Score*))
{
    std::vector<int> v;
    for (Excerpt* ex : score->excerpts()) {
        v.push_back(ex->excerptScore() ? count(ex->excerptScore()) : -1);
    }
    return v;
}
}

TEST_F(Agent_ExpressionMarks, MeasureRepeatRefused)
{
    {
        Transaction tx(score.get(), *backend);
        ASSERT_TRUE(tx.begin("setup"));
        ASSERT_TRUE(tx.step("repeat", [&] {
            // empty the bar first, or cmdAddMeasureRepeat asks with a MessageBox (cmd.cpp:3877)
            Measure* m2 = bar(score.get(), 2);
            score->deleteRange(m2->first(SegmentType::ChordRest), m2->nextMeasure()->first(SegmentType::ChordRest), 0, VOICES,
                               SelectionFilter(), false);
            score->cmdAddMeasureRepeat(m2, 1, 0);
            return muse::make_ok();
        }));
        ASSERT_TRUE(tx.commit());
    }
    ChordRest* mr = toChordRest(bar(score.get(), 2)->first(SegmentType::ChordRest)->element(0));
    ASSERT_TRUE(mr && mr->isMeasureRepeat());
    Params p;
    p["staff"] = staffNo(1);
    p["at"] = posBeat(2, 1);
    refused(DYN, with(p, "dynamic", Val("p")), AErr::InvalidArg, "measure repeat");
    refused(ART, with(p, "articulation", Val("accent")), AErr::InvalidArg, "measure repeat");
    refused(FER, p, AErr::InvalidArg, "measure repeat");
    Params b;
    b["staff"] = staffNo(1);
    b["after"] = posBeat(2, 1);
    refused(BR, b, AErr::InvalidArg, "measure repeat");
    Params h;
    h["kind"] = Val("cresc");
    h["staff"] = staffNo(1);
    h["from"] = posBeat(1, 1);
    h["to"] = posBeat(2, 1);
    refused(HP, h, AErr::InvalidArg, "measure repeat");
}

TEST_F(Agent_ExpressionMarks, SpanWithOnlyRestsRefused)
{
    open("voices-four.mscz");
    ChordRest* rest = restAt(score.get(), 3);
    ASSERT_TRUE(rest);
    Params p = with(at(score.get(), rest), "articulation", Val("staccato"));
    p["through"] = pos(score.get(), rest);
    refused(ART, p, AErr::InvalidArg, "there are no notes from");
    Params o = with(at(score.get(), rest), "ornament", Val("mordent"));
    o["through"] = pos(score.get(), rest);
    refused(ORN, o, AErr::InvalidArg, "there are no notes from");
}

TEST_F(Agent_ExpressionMarks, FermataNoEndBarline)
{
    // a bar without its end-barline segment (removed directly, as fixture set-up)
    Measure* m = bar(score.get(), 1);
    Segment* s = m->findSegment(SegmentType::EndBarLine, m->endTick());
    ASSERT_TRUE(s);
    m->segments().remove(s);
    Params bl;
    bl["staff"] = staffNo(1);
    bl["barline"] = Val(1);
    refused(FER, bl, AErr::Range, "has no end barline");
}

TEST_F(Agent_ExpressionMarks, FermataAllStavesErrors)
{
    open("voices-four.mscz");
    // voice 4 holds a whole note in bar 1: nothing starts at beat 2 in that voice on any staff
    Params all;
    all["all_staves"] = Val(true);
    all["at"] = posBeat(1, 2);
    all["voice"] = Val(4);
    refused(FER, all, AErr::Range, "no staff has");
    Params bl;
    bl["barline"] = Val(1);
    bl["all_staves"] = Val(true);
    bl["staff"] = staffNo(1);
    refused(FER, bl, AErr::InvalidArg, "leave out staff");
}

TEST_F(Agent_ExpressionMarks, HairpinToWithoutNote)
{
    open("voices-four.mscz");
    Params h;
    h["kind"] = Val("dim");
    h["staff"] = staffNo(1);
    h["voice"] = Val(4);
    h["from"] = posBeat(1, 1);
    h["to"] = posBeat(1, 2);
    refused(HP, h, AErr::Range, "no note or rest starts");
}

TEST_F(Agent_ExpressionMarks, HairpinEndStopsAtDynamic)
{
    open("voices-four.mscz");
    // voice 4 holds a whole note in bar 1; a voice-4 dynamic inside it (at voice 1's second
    // event) cuts a hairpin over that note short there (edit.cpp:4389-4396)
    ChordRest* whole = toChordRest(bar(score.get(), 1)->first(SegmentType::ChordRest)->element(3));
    ASSERT_TRUE(whole && whole->isChord());
    ChordRest* inside = chordAt(score.get(), 0, 1);
    ASSERT_TRUE(inside && inside->tick() > whole->tick() && inside->tick() < whole->endTick());
    {
        Transaction tx(score.get(), *backend);
        ASSERT_TRUE(tx.begin("setup"));
        ASSERT_TRUE(tx.step("dynamic", [&] {
            Dynamic* d = Factory::createDynamic(score->dummy()->segment());
            d->setDynamicType(u"f");
            d->setTrack(3);
            d->setProperty(Pid::VOICE_ASSIGNMENT, VoiceAssignment::CURRENT_VOICE_ONLY);
            d->setParent(inside->segment());
            score->undoAddElement(d);
            return muse::make_ok();
        }));
        ASSERT_TRUE(tx.commit());
    }
    Params h;
    h["kind"] = Val("cresc");
    h["staff"] = staffNo(1);
    h["voice"] = Val(4);
    h["from"] = pos(score.get(), whole);
    h["to"] = pos(score.get(), whole);
    h["voice_assignment"] = Val("this_voice");
    ASSERT_TRUE(run(HP, h));
    bool found = false;
    for (const auto& [tick, sp] : score->spanner()) {
        if (sp->isHairpin() && sp->track() == 3) {
            EXPECT_EQ(sp->tick2(), inside->tick());
            found = true;
        }
    }
    EXPECT_TRUE(found);
    // a second hairpin over the same note overlaps the (shortened) first one
    refused(HP, with(h, "kind", Val("dim")), AErr::Exists, "already covers");
}

TEST_F(Agent_ExpressionMarks, ArticulationSpanUndoRedo)
{
    ChordRest* a = chordAt(score.get(), 0);
    ChordRest* b = chordAt(score.get(), 0, 2);
    Params p = with(at(score.get(), a), "articulation", Val("marcato"));
    p["through"] = pos(score.get(), b);
    auto count = [&] {
        int n = 0;
        for (int i = 0; i < 3; ++i) {
            n += static_cast<int>(toChord(chordAt(score.get(), 0, i))->articulations().size());
        }
        return n;
    };
    addsOneEntry(ART, p, [&] { EXPECT_EQ(count(), 3); }, [&] { EXPECT_EQ(count(), 0); });
    EXPECT_EQ(out.at("added").toInt(), 3);
}

TEST_F(Agent_ExpressionMarks, OrnamentOnGraceUndoRedo)
{
    open("grace-notes.mscz");
    ChordRest* main = find(score.get(), 0, [](ChordRest* cr) { return cr->isChord() && !toChord(cr)->graceNotes().empty(); });
    ASSERT_TRUE(main);
    Chord* grace = toChord(main)->graceNotes().front();
    Params p;
    p["target"] = Val(makeHandle(grace));
    p["ornament"] = Val("turn");
    addsOneEntry(ORN, p, [&] {
        ASSERT_EQ(grace->articulations().size(), 1u);
        EXPECT_EQ(grace->articulations().front()->symId(), SymId::ornamentTurn);
    }, [&] { EXPECT_TRUE(grace->articulations().empty()); });
}

TEST_F(Agent_ExpressionMarks, HairpinInLinkedParts)
{
    open("parts-linked.mscz");
    ASSERT_FALSE(score->excerpts().empty());
    ChordRest* a = chordAt(score.get(), 0, 1);
    ChordRest* b = chordAt(score.get(), 0, 2);
    const std::vector<int> before = perPart(score.get(), hairpinsIn);
    const int master = hairpinsIn(score.get());
    Params h;
    h["kind"] = Val("dim");
    h["staff"] = staffNo(1);
    h["from"] = pos(score.get(), a);
    h["to"] = pos(score.get(), b);
    addsOneEntry(HP, h,
                 [&] {
        EXPECT_EQ(hairpinsIn(score.get()), master + 1);
        const std::vector<int> now = perPart(score.get(), hairpinsIn);
        int added = 0;
        for (size_t i = 0; i < now.size(); ++i) {
            EXPECT_GE(now[i], before[i]);
            added += now[i] - before[i];
        }
        EXPECT_EQ(added, 2) << "a copy in the Flute part and in the Flute and Clarinet part";
    },
                 [&] {
        EXPECT_EQ(hairpinsIn(score.get()), master);
        EXPECT_EQ(perPart(score.get(), hairpinsIn), before);
    });
}

TEST_F(Agent_ExpressionMarks, BreathInLinkedParts)
{
    open("parts-linked.mscz");
    ChordRest* a = chordAt(score.get(), 0, 1);
    const std::vector<int> before = perPart(score.get(), breathsIn);
    const int master = breathsIn(score.get());
    addsOneEntry(BR, at(score.get(), a, "after"),
                 [&] {
        EXPECT_EQ(breathsIn(score.get()), master + 1);
        const std::vector<int> now = perPart(score.get(), breathsIn);
        int added = 0;
        for (size_t i = 0; i < now.size(); ++i) {
            added += now[i] - before[i];
        }
        EXPECT_EQ(added, 2) << "a copy in the Flute part and in the Flute and Clarinet part";
    },
                 [&] {
        EXPECT_EQ(breathsIn(score.get()), master);
        EXPECT_EQ(perPart(score.get(), breathsIn), before);
    });
}
