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
// P2.4 expression-lines: add_slur and add_line on the engraving model, each run inside a core
// Transaction with the score's own undo stack, as the write runner runs them in the app.

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "engraving/dom/chord.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/part.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/ottava.h"
#include "engraving/dom/pedal.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/textline.h"
#include "engraving/dom/trill.h"
#include "engraving/dom/volta.h"
#include "engraving/editing/undo.h"
#include "engraving/tests/utils/scorerw.h"

#include "core/handles.h"
#include "core/transaction.h"
#include "core/undobackend.h"
#include "registry/errors.h"

#include "commands/expression-lines/lineops.h"
#include "notation/writer.h"
#include "read/scoreprinter.h"

using namespace mu::agent;
using namespace mu::agent::core;
using namespace mu::engraving;
using muse::Ret;
using muse::RetVal;
using muse::Val;
using muse::ValList;
using muse::ValMap;
namespace fs = std::filesystem;
using AErr = mu::agent::Err; // mu::engraving has an Err too

namespace {
MasterScore* loadFixture(const char* name)
{
    const fs::path p = fs::path(agent_tests_DATA_ROOT) / "../../../tests/fixtures/synthetic" / name;
    return ScoreRW::readScore(muse::String::fromStdString(p.string()), true);
}

using Op = std::function<Ret(MasterScore*, const Params&, std::map<std::string, Val>&)>;

struct Outcome {
    Ret ret;
    std::map<std::string, Val> out;
    std::string str(const char* key) const { auto it = out.find(key); return it == out.end() ? "" : it->second.toString(); }
};

Val pos(int measure, double beat, int staff, int voice = 1)
{
    ValMap m;
    m["measure"] = Val(static_cast<double>(measure));
    m["beat"] = Val(beat);
    m["staff"] = Val(static_cast<double>(staff));
    m["voice"] = Val(static_cast<double>(voice));
    return Val(m);
}

Val partPos(int measure, double beat, const char* part, int index = 1)
{
    ValMap staff;
    staff["part"] = Val(part);
    staff["index"] = Val(static_cast<double>(index));
    ValMap m;
    m["measure"] = Val(static_cast<double>(measure));
    m["beat"] = Val(beat);
    m["staff"] = Val(staff);
    m["voice"] = Val(1.0);
    return Val(m);
}

Val bar(int measure)
{
    ValMap m;
    m["measure"] = Val(static_cast<double>(measure));
    return Val(m);
}

Val handleAddr(const std::string& h)
{
    ValMap m;
    m["handle"] = Val(h);
    return Val(m);
}

Val with(Val addr, const char* key, Val v)
{
    ValMap m = addr.toMap();
    m[key] = v;
    return Val(m);
}

Params slurArgs(Val start, Val end)
{
    Params p;
    p["start"] = start;
    p["end"] = end;
    return p;
}

Params lineArgs(const char* kind, Val start, Val end)
{
    Params p;
    p["kind"] = Val(kind);
    p["start"] = start;
    p["end"] = end;
    return p;
}

size_t countSpanners(const Score* score, ElementType type)
{
    size_t n = 0;
    for (const auto& [tick, sp] : score->spanner()) {
        n += sp->type() == type ? 1 : 0;
    }
    return n;
}

size_t countAll(MasterScore* score, ElementType type)
{
    size_t n = 0;
    for (Score* s : score->scoreList()) {
        n += countSpanners(s, type);
    }
    return n;
}

bool contains(const std::string& s, const std::string& part)
{
    return s.find(part) != std::string::npos;
}

ChordRest* crAt(MasterScore* score, int bar, int offsetTicks, int staff, int voice = 1)
{
    int i = 0;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        if (++i == bar) {
            Segment* s = m->findSegment(SegmentType::ChordRest, m->tick() + Fraction::fromTicks(offsetTicks));
            EngravingItem* e = s ? s->element(static_cast<track_idx_t>((staff - 1) * VOICES + voice - 1)) : nullptr;
            return e && e->isChordRest() ? toChordRest(e) : nullptr;
        }
    }
    return nullptr;
}

class Agent_ExpressionLines : public ::testing::Test
{
protected:
    void open(const char* fixture)
    {
        score.reset(loadFixture(fixture));
        ASSERT_TRUE(score);
        backend = std::make_unique<ScoreUndoBackend>(score.get());
        MScore::setError(MsError::MS_NO_ERROR);
    }

    //! Everything a failed request must leave alone: the whole score printed in the notation,
    //! and every spanner of the master and of each part
    std::string snapshot()
    {
        std::string s = mu::agent::notation::write(read::printScore(score.get()));
        for (Score* sc : score->scoreList()) {
            s += "\nspanners " + std::to_string(sc->spanner().size());
            for (const auto& [tick, sp] : sc->spanner()) {
                s += " " + std::string(sp->typeName()) + "@" + std::to_string(tick) + "-" + std::to_string(sp->tick2().ticks());
            }
        }
        return s;
    }

    //! One request: a transaction around the op, as runWrite does. A failed op must leave the
    //! undo stack where it was and the score unchanged.
    Outcome run(const Op& op, const Params& p)
    {
        Outcome o;
        const size_t before = backend->currentIndex();
        const std::string state = snapshot();
        Transaction tx(score.get(), *backend);
        o.ret = tx.begin("Agent: test");
        if (!o.ret) {
            return o;
        }
        o.ret = tx.step("op", [&] { return op(score.get(), p, o.out); });
        if (!o.ret) {
            EXPECT_EQ(backend->currentIndex(), before) << "a failed op left an undo entry";
            EXPECT_FALSE(score->undoStack()->isLocked());
            EXPECT_EQ(snapshot(), state) << "a failed op changed the score: " << o.ret.text();
            return o;
        }
        o.ret = tx.commit();
        EXPECT_EQ(backend->currentIndex(), before + 1) << "a successful op must make exactly one undo entry";
        return o;
    }

    Outcome slur(const Params& p) { return run(lines::addSlur, p); }
    Outcome line(const Params& p) { return run(lines::addLine, p); }

    void expectErr(const Outcome& o, AErr code, const std::string& textPart = {})
    {
        EXPECT_FALSE(o.ret) << "expected " << errName(code);
        EXPECT_EQ(o.ret.code(), static_cast<int>(code)) << o.ret.text();
        EXPECT_TRUE(contains(o.ret.text(), errName(code))) << o.ret.text();
        if (!textPart.empty()) {
            EXPECT_TRUE(contains(o.ret.text(), textPart)) << o.ret.text();
        }
    }

    void undo() { score->undoRedo(true, nullptr); }
    void redo() { score->undoRedo(false, nullptr); }

    std::unique_ptr<MasterScore> score;
    std::unique_ptr<ScoreUndoBackend> backend;
};
}

// ---------------------------------------------------------------------------------------------
// add_slur
// ---------------------------------------------------------------------------------------------

TEST_F(Agent_ExpressionLines, SlurAcrossBarlineThenUndoRedo)
{
    open("spanners-system-break.mscz");
    const size_t slurs = countSpanners(score.get(), ElementType::SLUR);
    const Outcome o = slur(slurArgs(pos(3, 3, 2), pos(4, 1, 2)));
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(countSpanners(score.get(), ElementType::SLUR), slurs + 1);
    EXPECT_TRUE(contains(o.str("bars"), "m3 v1 | F3:h (D3:h")) << o.str("bars");
    EXPECT_TRUE(contains(o.str("bars"), "m4 v1 | C3:h) E3:h")) << o.str("bars");
    EXPECT_EQ(o.out.at("startBar").toInt(), 3);
    EXPECT_EQ(o.out.at("endBar").toInt(), 4);

    RetVal<EngravingItem*> h = resolveHandle(score.get(), o.str("handle"));
    ASSERT_TRUE(h.ret) << h.ret.text() << " " << o.str("handle");
    ASSERT_TRUE(h.val->isSlur());
    const Slur* s = toSlur(h.val);
    EXPECT_EQ(s->startElement(), crAt(score.get(), 3, 960, 2));
    EXPECT_EQ(s->endElement(), crAt(score.get(), 4, 0, 2));
    EXPECT_EQ(s->track(), 4u);
    EXPECT_EQ(s->track2(), 4u);
    EXPECT_EQ(s->slurDirection(), DirectionV::AUTO);

    undo();
    EXPECT_EQ(countSpanners(score.get(), ElementType::SLUR), slurs);
    redo();
    EXPECT_EQ(countSpanners(score.get(), ElementType::SLUR), slurs + 1);
}

TEST_F(Agent_ExpressionLines, SlurByPartNameDirectionAndStyle)
{
    open("spanners-system-break.mscz");
    Params p = slurArgs(partPos(1, 1, "Piano", 1), partPos(1, 3, "Piano", 1));
    p["direction"] = Val("down");
    p["style"] = Val("dashed");
    const Outcome o = slur(p);
    ASSERT_TRUE(o.ret) << o.ret.text();
    const Slur* s = toSlur(resolveHandle(score.get(), o.str("handle")).val);
    EXPECT_EQ(s->slurDirection(), DirectionV::DOWN);
    EXPECT_EQ(s->getProperty(Pid::SLUR_STYLE_TYPE).value<SlurStyleType>(), SlurStyleType::Dashed);
    EXPECT_TRUE(contains(o.str("bars"), "m1 v1 | (F4:q G4:q A4:q) B4:q")) << o.str("bars");
    EXPECT_EQ(countSpanners(score.get(), ElementType::SLUR), 3u); // the fixture has two
    undo();
    EXPECT_EQ(countSpanners(score.get(), ElementType::SLUR), 2u);
}

TEST_F(Agent_ExpressionLines, SlurCrossStaffWithinPart)
{
    open("spanners-system-break.mscz");
    const Outcome o = slur(slurArgs(pos(5, 4, 1), pos(6, 1, 2)));
    ASSERT_TRUE(o.ret) << o.ret.text();
    const Slur* s = toSlur(resolveHandle(score.get(), o.str("handle")).val);
    EXPECT_EQ(s->track(), 0u);
    EXPECT_EQ(s->track2(), 4u);
    EXPECT_TRUE(contains(o.str("bars"), "# s2")) << o.str("bars");
}

TEST_F(Agent_ExpressionLines, SlurCrossVoice)
{
    open("voices-four.mscz");
    const Outcome o = slur(slurArgs(pos(1, 1, 1, 1), pos(1, 2, 1, 2)));
    ASSERT_TRUE(o.ret) << o.ret.text();
    const Slur* s = toSlur(resolveHandle(score.get(), o.str("handle")).val);
    EXPECT_EQ(s->track(), 0u);
    EXPECT_EQ(s->track2(), 1u);
}

TEST_F(Agent_ExpressionLines, SlurFromGraceToMainChord)
{
    open("grace-notes.mscz");
    const Outcome o = slur(slurArgs(with(pos(1, 1, 1), "grace", Val(1.0)), pos(1, 1, 1)));
    ASSERT_TRUE(o.ret) << o.ret.text();
    const Slur* s = toSlur(resolveHandle(score.get(), o.str("handle")).val);
    ASSERT_TRUE(s->startElement()->isChord());
    EXPECT_TRUE(toChord(s->startElement())->isGrace());
    EXPECT_EQ(s->endElement(), crAt(score.get(), 1, 0, 1));
}

TEST_F(Agent_ExpressionLines, SlurByHandles)
{
    open("spanners-system-break.mscz");
    const std::string a = makeHandle(toChord(crAt(score.get(), 7, 0, 1))->upNote()); // a note maps to its chord
    const std::string b = makeHandle(crAt(score.get(), 7, 1440, 1));
    const Outcome o = slur(slurArgs(handleAddr(a), handleAddr(b)));
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_TRUE(contains(o.str("bars"), "m7 v1 | (F4:q G4:q A4:q B4:q)")) << o.str("bars");
}

TEST_F(Agent_ExpressionLines, SlurInLinkedPartsReachesEveryPart)
{
    open("parts-linked.mscz");
    ASSERT_EQ(score->excerpts().size(), 3u);
    const size_t before = countAll(score.get(), ElementType::SLUR);
    const Outcome o = slur(slurArgs(partPos(1, 2, "Flute"), partPos(1, 4, "Flute")));
    ASSERT_TRUE(o.ret) << o.ret.text();
    // master + "Flute" + "Flute and Clarinet"
    EXPECT_EQ(countAll(score.get(), ElementType::SLUR), before + 3);
    undo();
    EXPECT_EQ(countAll(score.get(), ElementType::SLUR), before);
    redo();
    EXPECT_EQ(countAll(score.get(), ElementType::SLUR), before + 3);
}

TEST_F(Agent_ExpressionLines, SlurErrors)
{
    open("parts-linked.mscz");
    const size_t slurs = countAll(score.get(), ElementType::SLUR);

    expectErr(slur(slurArgs(pos(1, 1, 2), pos(1, 2, 2))), AErr::InvalidArg, "rest");            // Clarinet m1 beat 1 is a rest
    expectErr(slur(slurArgs(pos(1, 2, 2), pos(1, 4, 2))), AErr::InvalidArg, "rest");            // ... and beat 4
    expectErr(slur(slurArgs(pos(1, 1, 1), pos(1, 1, 1))), AErr::InvalidArg, "same chord");
    expectErr(slur(slurArgs(pos(1, 2, 1), pos(1, 2, 2))), AErr::InvalidArg, "different parts");
    expectErr(slur(slurArgs(pos(1, 3, 1), pos(1, 2, 1))), AErr::Range, "before start");
    expectErr(slur(slurArgs(pos(3, 2, 1), pos(3, 3, 1))), AErr::Range, "beats 1, 3");           // half notes on 1 and 3
    expectErr(slur(slurArgs(pos(99, 1, 1), pos(1, 2, 1))), AErr::Range, "outside the score");
    expectErr(slur(slurArgs(pos(1, 1, 9), pos(1, 2, 1))), AErr::Range, "is outside 1-");
    expectErr(slur(slurArgs(pos(1, 5, 1), pos(1, 2, 1))), AErr::Range, "past its end");
    expectErr(slur(slurArgs(pos(1, 1, 1, 5), pos(1, 2, 1))), AErr::InvalidArg, "voice");
    expectErr(slur(slurArgs(partPos(1, 1, "Oboe"), pos(1, 2, 1))), AErr::Range, "no part named");
    expectErr(slur(slurArgs(partPos(1, 1, "Flute", 2), pos(1, 2, 1))), AErr::Range, "index 2");
    expectErr(slur(slurArgs(with(pos(1, 1, 1), "bar", Val(1.0)), pos(1, 2, 1))), AErr::InvalidArg, "unknown key");
    expectErr(slur(slurArgs(with(pos(1, 1, 1), "tick", Val(0.0)), pos(1, 2, 1))), AErr::InvalidArg, "one of beat");
    expectErr(slur(slurArgs(with(pos(1, 1, 1), "handle", Val("x")), pos(1, 2, 1))), AErr::InvalidArg, "not both");
    expectErr(slur(slurArgs(handleAddr("Chord:nonsense"), pos(1, 2, 1))), AErr::InvalidArg, "malformed handle");
    expectErr(slur(slurArgs(Val("m1"), pos(1, 2, 1))), AErr::InvalidArg, "must be an object");
    expectErr(slur(slurArgs(with(pos(1, 1, 1), "grace", Val(1.0)), pos(1, 2, 1))), AErr::Range, "grace");
    {
        ValMap noMeasure = pos(1, 1, 1).toMap();
        noMeasure.erase("measure");
        expectErr(slur(slurArgs(Val(noMeasure), pos(1, 2, 1))), AErr::InvalidArg, "measure is required");
        ValMap noStaff = pos(1, 1, 1).toMap();
        noStaff.erase("staff");
        expectErr(slur(slurArgs(Val(noStaff), pos(1, 2, 1))), AErr::InvalidArg, "staff is required");
    }
    {
        Params p = slurArgs(pos(1, 1, 1), pos(1, 2, 1));
        p["direction"] = Val("sideways");
        expectErr(slur(p), AErr::InvalidArg, "direction");
        p.erase("direction");
        p["style"] = Val(3.0);
        expectErr(slur(p), AErr::InvalidArg, "style");
    }
    {
        Params p;
        p["start"] = pos(1, 1, 1);
        expectErr(slur(p), AErr::InvalidArg, "required");
    }
    EXPECT_EQ(countAll(score.get(), ElementType::SLUR), slurs);
}

TEST_F(Agent_ExpressionLines, DuplicateSlurIsRefused)
{
    open("spanners-system-break.mscz");
    const Outcome first = slur(slurArgs(pos(10, 1, 1), pos(10, 4, 1)));
    ASSERT_TRUE(first.ret) << first.ret.text();
    const size_t slurs = countSpanners(score.get(), ElementType::SLUR);
    const Outcome again = slur(slurArgs(pos(10, 1, 1), pos(10, 4, 1)));
    expectErr(again, AErr::Exists, first.str("handle"));
    EXPECT_EQ(countSpanners(score.get(), ElementType::SLUR), slurs);

    // also when the existing slur has a direction (the UI would stack a second one there)
    Params down = slurArgs(pos(12, 1, 1), pos(12, 2, 1));
    down["direction"] = Val("up");
    ASSERT_TRUE(slur(down).ret);
    expectErr(slur(slurArgs(pos(12, 1, 1), pos(12, 2, 1))), AErr::Exists);
    EXPECT_EQ(countSpanners(score.get(), ElementType::SLUR), slurs + 1);
}

TEST_F(Agent_ExpressionLines, HandleThatIsNotAnEventIsRefused)
{
    open("spanners-system-break.mscz");
    const Outcome first = slur(slurArgs(pos(7, 1, 1), pos(7, 4, 1)));
    ASSERT_TRUE(first.ret) << first.ret.text();
    expectErr(slur(slurArgs(handleAddr(first.str("handle")), pos(7, 4, 1))), AErr::InvalidArg, "names a Slur");
}

TEST_F(Agent_ExpressionLines, StaleHandleIsRefused)
{
    open("spanners-system-break.mscz");
    const std::string h = makeHandle(crAt(score.get(), 7, 0, 1));
    std::string stale = h;
    stale.back() = stale.back() == '1' ? '2' : '1'; // the fingerprint no longer matches
    expectErr(slur(slurArgs(handleAddr(stale), pos(7, 4, 1))), AErr::StaleHandle);
}

// ---------------------------------------------------------------------------------------------
// add_line
// ---------------------------------------------------------------------------------------------

TEST_F(Agent_ExpressionLines, PedalOverTwoBarsThenUndoRedo)
{
    open("spanners-system-break.mscz");
    const size_t pedals = countSpanners(score.get(), ElementType::PEDAL);
    const Outcome o = line(lineArgs("pedal", pos(9, 1, 2), pos(10, 3, 2)));
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(countSpanners(score.get(), ElementType::PEDAL), pedals + 1);
    const Spanner* sp = toSpanner(resolveHandle(score.get(), o.str("handle")).val);
    ASSERT_TRUE(sp && sp->isPedal());
    const Measure* m9 = crAt(score.get(), 9, 0, 2)->measure();
    EXPECT_EQ(sp->tick(), m9->tick());
    EXPECT_EQ(sp->tick2(), m9->nextMeasure()->endTick()); // to the end of the half note on beat 3 of m10
    EXPECT_EQ(sp->track(), 4u);
    EXPECT_EQ(toPedal(sp)->endHookType(), HookType::HOOK_90);
    EXPECT_TRUE(contains(o.str("bars"), "m9 v1 | D3:h+ped[ F3:h")) << o.str("bars");
    EXPECT_TRUE(contains(o.str("bars"), "m10 v1 | E3:h C3:h+ped]")) << o.str("bars");
    EXPECT_EQ(o.str("kind"), "pedal");
    undo();
    EXPECT_EQ(countSpanners(score.get(), ElementType::PEDAL), pedals);
    redo();
    EXPECT_EQ(countSpanners(score.get(), ElementType::PEDAL), pedals + 1);
}

TEST_F(Agent_ExpressionLines, PedalAsteriskStyle)
{
    open("spanners-system-break.mscz");
    Params p = lineArgs("pedal", pos(11, 1, 2), pos(11, 1, 2));
    p["pedalStyle"] = Val("asterisk");
    const Outcome o = line(p);
    ASSERT_TRUE(o.ret) << o.ret.text();
    const Pedal* pedal = toPedal(resolveHandle(score.get(), o.str("handle")).val);
    EXPECT_FALSE(pedal->lineVisible());
    EXPECT_EQ(pedal->endHookType(), HookType::ROSETTE);
    EXPECT_TRUE(contains(o.str("bars"), "m11 v1 | F3:h+ped[+ped] D3:h")) << o.str("bars"); // one event: both marks
    EXPECT_EQ(pedal->tick2() - pedal->tick(), Fraction(1, 2));
}

TEST_F(Agent_ExpressionLines, OttavaTypesAndPlacement)
{
    open("spanners-system-break.mscz");
    Params p = lineArgs("ottava", pos(1, 1, 1), pos(1, 4, 1));
    const Outcome a = line(p);
    ASSERT_TRUE(a.ret) << a.ret.text();
    EXPECT_EQ(toOttava(resolveHandle(score.get(), a.str("handle")).val)->ottavaType(), OttavaType::OTTAVA_8VA);
    // sounding pitch: F4 under an 8va reads F5
    EXPECT_TRUE(contains(a.str("bars"), "m1 v1 | F5:q+8va[ G5:q A5:q B5:q+8va]")) << a.str("bars");

    p = lineArgs("ottava", pos(2, 2, 1), pos(3, 1, 1));
    p["ottavaType"] = Val("15mb");
    p["placement"] = Val("above");
    const Outcome b = line(p);
    ASSERT_TRUE(b.ret) << b.ret.text();
    const Ottava* o = toOttava(resolveHandle(score.get(), b.str("handle")).val);
    EXPECT_EQ(o->ottavaType(), OttavaType::OTTAVA_15MB);
    EXPECT_EQ(o->placement(), PlacementV::ABOVE);
    EXPECT_TRUE(contains(b.str("bars"), "m2 v1 | (G4:q+< A2:q+15mb[")) << b.str("bars");
    EXPECT_TRUE(contains(b.str("bars"), "m3 v1 | A2:q+15mb]")) << b.str("bars"); // A4 sounding two octaves down
    undo();
    undo();
    EXPECT_EQ(countSpanners(score.get(), ElementType::OTTAVA), 1u); // the fixture's own
}

TEST_F(Agent_ExpressionLines, TrillInLinkedPartsHasOrnament)
{
    open("parts-linked.mscz");
    const size_t trills = countAll(score.get(), ElementType::TRILL);
    Params p = lineArgs("trill", partPos(3, 1, "Flute"), partPos(3, 1, "Flute"));
    p["trillType"] = Val("prallprall");
    const Outcome o = line(p);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(countAll(score.get(), ElementType::TRILL), trills + 3);
    for (Score* s : score->scoreList()) {
        for (const auto& [tick, sp] : s->spanner()) {
            if (sp->isTrill()) {
                EXPECT_TRUE(toTrill(sp)->ornament());
                EXPECT_EQ(toTrill(sp)->trillType(), TrillType::PRALLPRALL_LINE);
            }
        }
    }
    EXPECT_TRUE(contains(o.str("bars"), "m3 v1 | D5:h+trl[+trl] C5:h")) << o.str("bars");
    undo();
    EXPECT_EQ(countAll(score.get(), ElementType::TRILL), trills);
    redo();
    EXPECT_EQ(countAll(score.get(), ElementType::TRILL), trills + 3);
}

TEST_F(Agent_ExpressionLines, VoltaOverWholeBars)
{
    open("parts-linked.mscz");
    const size_t voltas = countAll(score.get(), ElementType::VOLTA);
    Params p = lineArgs("volta", bar(2), bar(3));
    p["endings"] = Val(ValList { Val(2.0), Val(1.0) });
    const Outcome o = line(p);
    ASSERT_TRUE(o.ret) << o.ret.text();
    const Volta* v = toVolta(resolveHandle(score.get(), o.str("handle")).val);
    ASSERT_TRUE(v);
    EXPECT_EQ(v->endings(), (std::vector<int> { 1, 2 }));
    EXPECT_EQ(v->text(), u"1., 2.");
    EXPECT_EQ(v->voltaType(), Volta::Type::CLOSED);
    EXPECT_EQ(v->track(), 0u);
    const Measure* m2 = crAt(score.get(), 2, 0, 1)->measure();
    EXPECT_EQ(v->tick(), m2->tick());
    EXPECT_EQ(v->tick2(), m2->nextMeasure()->endTick());
    EXPECT_TRUE(v->systemFlag());
    // one on the master's top staff and one in each of the three parts
    EXPECT_EQ(countAll(score.get(), ElementType::VOLTA), voltas + 4);
    EXPECT_TRUE(contains(o.str("bars"), "volta=1,2")) << o.str("bars");
    EXPECT_TRUE(contains(o.str("bars"), "volta=end")) << o.str("bars");
    undo();
    EXPECT_EQ(countAll(score.get(), ElementType::VOLTA), voltas);
    redo();
    EXPECT_EQ(countAll(score.get(), ElementType::VOLTA), voltas + 4);
}

TEST_F(Agent_ExpressionLines, VoltaOnLastBarAndOpen)
{
    open("parts-linked.mscz");
    Params p = lineArgs("volta", bar(4), bar(4));
    p["voltaType"] = Val("open");
    p["text"] = Val("2.");
    p["endings"] = Val(ValList { Val(2.0) });
    const Outcome o = line(p);
    ASSERT_TRUE(o.ret) << o.ret.text();
    const Volta* v = toVolta(resolveHandle(score.get(), o.str("handle")).val);
    EXPECT_EQ(v->voltaType(), Volta::Type::OPEN);
    EXPECT_EQ(v->text(), u"2.");
    EXPECT_EQ(v->tick2(), score->lastMeasure()->endTick());
}

TEST_F(Agent_ExpressionLines, StaffAndSystemTextLines)
{
    open("parts-linked.mscz");
    const size_t lines0 = countAll(score.get(), ElementType::TEXTLINE);
    Params p = lineArgs("textLine", partPos(2, 1, "Bassoon"), partPos(3, 3, "Bassoon"));
    p["text"] = Val("rit.");
    const Outcome a = line(p);
    ASSERT_TRUE(a.ret) << a.ret.text();
    const TextLine* t = toTextLine(resolveHandle(score.get(), a.str("handle")).val);
    EXPECT_FALSE(t->systemFlag());
    EXPECT_EQ(t->beginText(), u"rit.");
    EXPECT_EQ(t->endHookType(), HookType::HOOK_90);
    EXPECT_EQ(t->track(), 8u);
    EXPECT_EQ(countAll(score.get(), ElementType::TEXTLINE), lines0 + 1); // the bassoon has no part
    EXPECT_TRUE(contains(a.str("bars"), "m2 v1 | G2:h+txt[\"rit.\" C3:h")) << a.str("bars");
    EXPECT_TRUE(contains(a.str("bars"), "m3 v1 | B2:h A2:h+txt]")) << a.str("bars");

    p = lineArgs("textLine", partPos(1, 1, "Bassoon"), partPos(1, 1, "Bassoon"));
    p["text"] = Val("a <b> & c");
    p["system"] = Val(true);
    p["endHook"] = Val("none");
    p["endText"] = Val("end");
    const Outcome b = line(p);
    ASSERT_TRUE(b.ret) << b.ret.text();
    const TextLine* s = toTextLine(resolveHandle(score.get(), b.str("handle")).val);
    EXPECT_TRUE(s->systemFlag());
    EXPECT_EQ(s->track(), 0u); // system lines go on the top staff
    EXPECT_EQ(s->beginText(), u"a &lt;b&gt; &amp; c");
    EXPECT_EQ(s->endText(), u"end");
    EXPECT_EQ(s->endHookType(), HookType::NONE);
    EXPECT_EQ(countAll(score.get(), ElementType::TEXTLINE), lines0 + 1 + 4);
    undo();
    EXPECT_EQ(countAll(score.get(), ElementType::TEXTLINE), lines0 + 1);
}

TEST_F(Agent_ExpressionLines, LineErrors)
{
    open("spanners-system-break.mscz");
    const size_t total = score->spanner().size();

    expectErr(line(lineArgs("pedal", pos(1, 1, 1), pos(1, 3, 2))), AErr::InvalidArg, "different staves");
    expectErr(line(lineArgs("pedal", pos(2, 1, 2), pos(1, 1, 2))), AErr::Range, "before start");
    expectErr(line(lineArgs("pedal", pos(2, 2, 2), pos(2, 3, 2))), AErr::Range, "no note or rest");
    expectErr(line(lineArgs("glissando", pos(1, 1, 1), pos(1, 2, 1))), AErr::InvalidArg, "kind");
    {
        Params p = lineArgs("pedal", pos(1, 1, 1), pos(1, 2, 1));
        p.erase("kind");
        expectErr(line(p), AErr::InvalidArg, "kind is required");
        p["kind"] = Val("trill");
        p.erase("end");
        expectErr(line(p), AErr::InvalidArg, "required");
    }
    {
        Params p = lineArgs("pedal", pos(1, 1, 1), pos(1, 2, 1));
        p["ottavaType"] = Val("8vb");
        expectErr(line(p), AErr::InvalidArg, "does not apply to kind pedal");
        p = lineArgs("ottava", pos(1, 1, 1), pos(1, 2, 1));
        p["ottavaType"] = Val("8vx");
        expectErr(line(p), AErr::InvalidArg, "ottavaType");
        p = lineArgs("textLine", pos(1, 1, 1), pos(1, 2, 1));
        p["system"] = Val("yes");
        expectErr(line(p), AErr::InvalidArg, "system");
        p = lineArgs("trill", pos(1, 1, 1), pos(1, 2, 1));
        p["placement"] = Val("left");
        expectErr(line(p), AErr::InvalidArg, "placement");
    }
    {
        Params p = lineArgs("volta", pos(1, 2, 1), bar(2));
        expectErr(line(p), AErr::InvalidArg, "whole bars");
        p = lineArgs("volta", bar(3), bar(2));
        expectErr(line(p), AErr::Range, "before start bar");
        p = lineArgs("volta", bar(1), bar(20));
        expectErr(line(p), AErr::Range, "outside bars");
        p = lineArgs("volta", bar(1), bar(2));
        p["endings"] = Val(ValList {});
        expectErr(line(p), AErr::InvalidArg, "non-empty");
        p["endings"] = Val(ValList { Val(1.0), Val(1.0) });
        expectErr(line(p), AErr::InvalidArg, "twice");
        p["endings"] = Val(ValList { Val(0.0) });
        expectErr(line(p), AErr::InvalidArg, "1-100");
    }
    EXPECT_EQ(score->spanner().size(), total);
}

TEST_F(Agent_ExpressionLines, LineOnGraceNoteIsRefused)
{
    open("grace-notes.mscz");
    expectErr(line(lineArgs("trill", with(pos(1, 1, 1), "grace", Val(1.0)), pos(1, 2, 1))), AErr::InvalidArg, "grace");
}

// Several commands in one transaction (a batch): one undo entry, and a failure in a later step
// takes the earlier ones back too.
TEST_F(Agent_ExpressionLines, BatchIsAllOrNothing)
{
    open("spanners-system-break.mscz");
    const size_t total = score->spanner().size();
    const size_t before = backend->currentIndex();
    {
        Transaction tx(score.get(), *backend);
        ASSERT_TRUE(tx.begin("Agent: batch"));
        std::map<std::string, Val> out;
        EXPECT_TRUE(tx.step("add_slur", [&] { return lines::addSlur(score.get(), slurArgs(pos(10, 1, 1), pos(10, 4, 1)), out); }));
        EXPECT_TRUE(tx.step("add_line", [&] { return lines::addLine(score.get(), lineArgs("pedal", pos(12, 1, 2), pos(12, 3, 2)), out); }));
        const Ret r = tx.step("add_line", [&] { return lines::addLine(score.get(), lineArgs("pedal", pos(12, 1, 2), pos(99, 1, 2)), out); });
        EXPECT_FALSE(r);
        EXPECT_EQ(r.code(), static_cast<int>(AErr::Range));
    }
    EXPECT_EQ(score->spanner().size(), total);
    EXPECT_EQ(backend->currentIndex(), before);

    {
        Transaction tx(score.get(), *backend);
        ASSERT_TRUE(tx.begin("Agent: batch"));
        std::map<std::string, Val> out;
        EXPECT_TRUE(tx.step("add_slur", [&] { return lines::addSlur(score.get(), slurArgs(pos(10, 1, 1), pos(10, 4, 1)), out); }));
        EXPECT_TRUE(tx.step("add_line", [&] { return lines::addLine(score.get(), lineArgs("pedal", pos(12, 1, 2), pos(12, 3, 2)), out); }));
        ASSERT_TRUE(tx.commit());
    }
    EXPECT_EQ(score->spanner().size(), total + 2);
    EXPECT_EQ(backend->currentIndex(), before + 1);
    undo();
    EXPECT_EQ(score->spanner().size(), total);
}

// ---------------------------------------------------------------------------------------------
// Every remaining validation path: the code, and nothing changed (run() compares the whole score
// printed, every spanner of the master and its parts, and the undo index)
// ---------------------------------------------------------------------------------------------

TEST_F(Agent_ExpressionLines, SlurEndsAtTheSameTimeAreRefused)
{
    open("voices-four.mscz");
    expectErr(slur(slurArgs(pos(1, 1, 1, 1), pos(1, 1, 1, 2))), AErr::InvalidArg, "same time");
}

TEST_F(Agent_ExpressionLines, SlurOnTwoLinkedStavesOfOnePartIsRefused)
{
    open("pickup-bar.mscz");
    // add a staff linked to the flute's, as "Add linked staff" does (links_tests.cpp:104-109)
    score->startCmd(muse::TranslatableString::untranslatable("test: linked staff"));
    Staff* original = score->staff(0);
    Staff* linked = Factory::createStaff(original->part());
    linked->setPart(original->part());
    score->undoInsertStaff(linked, 1, false);
    Excerpt::cloneStaff(original, linked);
    score->endCmd();
    ASSERT_EQ(score->nstaves(), 2u);
    ASSERT_TRUE(score->staff(1)->isLinked(score->staff(0)));
    expectErr(slur(slurArgs(pos(2, 1, 1), pos(2, 3, 2))), AErr::InvalidArg, "linked staves");
}

TEST_F(Agent_ExpressionLines, SlurOnTrillCueNoteIsRefused)
{
    open("parts-linked.mscz");
    ChordRest* cr = crAt(score.get(), 1, 0, 1);
    ASSERT_TRUE(cr && cr->isChord());
    toChord(cr)->setIsTrillCueNote(true); // test set-up only
    expectErr(slur(slurArgs(pos(1, 1, 1), pos(1, 2, 1))), AErr::InvalidArg, "trill cue");
    expectErr(slur(slurArgs(pos(1, 2, 1), pos(1, 1, 1))), AErr::InvalidArg, "trill cue");
}

TEST_F(Agent_ExpressionLines, AmbiguousPartNameIsRefused)
{
    open("parts-linked.mscz");
    score->parts().at(1)->setPartName(u"Flute"); // test set-up only: two parts named "Flute"
    expectErr(slur(slurArgs(partPos(1, 2, "Flute"), partPos(1, 3, "Flute"))), AErr::InvalidArg, "2 parts are named");
}

TEST_F(Agent_ExpressionLines, MalformedAddressesAreRefused)
{
    open("parts-linked.mscz");
    const Val ok = pos(1, 3, 1);
    {
        ValMap staff;
        staff["part"] = Val("Flute");
        staff["number"] = Val(1.0);
        expectErr(slur(slurArgs(with(pos(1, 2, 1), "staff", Val(staff)), ok)), AErr::InvalidArg, "not both"); // shared shape: number, or part (+ index)
        staff.erase("number");
        staff["index"] = Val(1.5);
        expectErr(slur(slurArgs(with(pos(1, 2, 1), "staff", Val(staff)), ok)), AErr::InvalidArg, "index must be a whole number");
        staff.erase("index");
        staff["part"] = Val(2.0);
        expectErr(slur(slurArgs(with(pos(1, 2, 1), "staff", Val(staff)), ok)), AErr::InvalidArg, "staff must be");
    }
    expectErr(slur(slurArgs(with(pos(1, 2, 1), "staff", Val(1.5)), ok)), AErr::InvalidArg, "must be a whole number");
    expectErr(slur(slurArgs(with(pos(1, 2, 1), "staff", Val(true)), ok)), AErr::InvalidArg, "staff must be");
    expectErr(slur(slurArgs(with(pos(1, 2, 1), "beat", Val("two")), ok)), AErr::InvalidArg, "beat must be a number");
    {
        ValMap m = pos(1, 2, 1).toMap();
        m.erase("beat");
        m["tick"] = Val(480.5);
        expectErr(slur(slurArgs(Val(m), ok)), AErr::InvalidArg, "tick must be a whole number");
        m["tick"] = Val(480.0); // the same place as beat 2: accepted
        ASSERT_TRUE(slur(slurArgs(Val(m), ok)).ret);
    }
    expectErr(slur(slurArgs(with(pos(1, 2, 1), "measure", Val(1.5)), ok)), AErr::InvalidArg, "measure is required");
    expectErr(slur(slurArgs(with(pos(1, 2, 1), "grace", Val(0.0)), ok)), AErr::InvalidArg, "grace must be a whole number");
    expectErr(slur(slurArgs(with(pos(1, 1, 2), "grace", Val(1.0)), pos(1, 2, 2))), AErr::InvalidArg, "no grace notes");
    expectErr(slur(slurArgs(handleAddr("x"), ok)), AErr::InvalidArg, "malformed handle");
    {
        ValMap m;
        m["handle"] = Val(3.0);
        expectErr(slur(slurArgs(Val(m), ok)), AErr::InvalidArg, "handle must be a string");
    }
}

TEST_F(Agent_ExpressionLines, MalformedVoltaAddressesAreRefused)
{
    open("parts-linked.mscz");
    auto volta = [&](Val start, Val end) { return line(lineArgs("volta", start, end)); };
    expectErr(volta(Val("m1"), bar(2)), AErr::InvalidArg, "must be an object");
    expectErr(volta(bar(1), Val(2.0)), AErr::InvalidArg, "must be an object");
    expectErr(volta(with(bar(1), "bar", Val(2.0)), bar(2)), AErr::InvalidArg, "unknown key 'bar'");
    expectErr(volta(Val(ValMap {}), bar(2)), AErr::InvalidArg, "measure is required");
    expectErr(volta(with(bar(1), "staff", Val(2.0)), bar(2)), AErr::InvalidArg, "no staff");
    expectErr(volta(bar(1), with(bar(2), "voice", Val(1.0))), AErr::InvalidArg, "no voice");
    expectErr(volta(with(bar(1), "tick", Val(0.0)), bar(2)), AErr::InvalidArg, "whole bars");
    expectErr(volta(with(bar(1), "grace", Val(1.0)), bar(2)), AErr::InvalidArg, "whole bars");
    expectErr(volta(handleAddr("Chord:bad"), bar(2)), AErr::InvalidArg, "malformed handle");
    // a handle names its bar
    const std::string h = makeHandle(crAt(score.get(), 2, 0, 1));
    const Outcome o = volta(handleAddr(h), bar(2));
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(o.out.at("startBar").toInt(), 2);
}

TEST_F(Agent_ExpressionLines, MalformedLineOptionsAreRefused)
{
    open("parts-linked.mscz");
    Params p = lineArgs("volta", bar(1), bar(2));
    p["endings"] = Val(1.0);
    expectErr(line(p), AErr::InvalidArg, "non-empty list");
    p["endings"] = Val(ValList { Val(1.5) });
    expectErr(line(p), AErr::InvalidArg, "1-100");
    p["endings"] = Val(ValList { Val(101.0) });
    expectErr(line(p), AErr::InvalidArg, "1-100");
    p["endings"] = Val(ValList { Val("1") });
    expectErr(line(p), AErr::InvalidArg, "1-100");

    p = lineArgs("pedal", pos(1, 1, 3), pos(1, 1, 3));
    p["kind"] = Val(3.0);
    expectErr(line(p), AErr::InvalidArg, "'kind' must be a string");

    p = lineArgs("textLine", pos(1, 1, 3), pos(2, 1, 3));
    p["text"] = Val(true);
    expectErr(line(p), AErr::InvalidArg, "'text' must be a string");
    p["text"] = Val("ok");
    p["endText"] = Val(1.0);
    expectErr(line(p), AErr::InvalidArg, "'endText' must be a string");
    p.erase("endText");
    p["endHook"] = Val("curly");
    expectErr(line(p), AErr::InvalidArg, "endHook");

    p = lineArgs("trill", pos(1, 1, 3), pos(1, 1, 3));
    p["trillType"] = Val("mordent");
    expectErr(line(p), AErr::InvalidArg, "trillType");
    p = lineArgs("pedal", pos(1, 1, 3), pos(1, 1, 3));
    p["pedalStyle"] = Val("curly");
    expectErr(line(p), AErr::InvalidArg, "pedalStyle");
    p = lineArgs("volta", bar(1), bar(1));
    p["voltaType"] = Val("half");
    expectErr(line(p), AErr::InvalidArg, "voltaType");
}

TEST_F(Agent_ExpressionLines, PlacementIsReadBack)
{
    open("pickup-bar.mscz");
    Params p = lineArgs("pedal", pos(2, 1, 1), pos(2, 3, 1));
    p["placement"] = Val("above");
    const Outcome o = line(p);
    ASSERT_TRUE(o.ret) << o.ret.text();
    EXPECT_EQ(toSpanner(resolveHandle(score.get(), o.str("handle")).val)->placement(), PlacementV::ABOVE);
    undo();
    redo();
    EXPECT_EQ(toSpanner(resolveHandle(score.get(), o.str("handle")).val)->placement(), PlacementV::ABOVE);
}

