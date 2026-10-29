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
// P2.1 transaction core: Transaction, SelectionGuard, handles, post-conditions, on the
// engraving model with the score's own undo stack (ScoreUndoBackend).

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>

#include "engraving/dom/chord.h"
#include "engraving/dom/input.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/segment.h"
#include "engraving/editing/undo.h"
#include "engraving/tests/utils/scorerw.h"

#include "core/handles.h"
#include "core/postconditions.h"
#include "core/transaction.h"
#include "core/undobackend.h"
#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::agent::core;
using namespace mu::engraving;
using muse::Ret;
namespace fs = std::filesystem;

namespace {
MasterScore* load(const char* name)
{
    const fs::path p = fs::path(agent_tests_DATA_ROOT) / "../../../tests/fixtures/synthetic" / name;
    return ScoreRW::readScore(muse::String::fromStdString(p.string()), true);
}

Chord* firstChord(MasterScore* score, int skip = 0)
{
    for (Segment* s = score->firstSegment(SegmentType::ChordRest); s; s = s->next1(SegmentType::ChordRest)) {
        if (EngravingItem* e = s->element(0); e && e->isChord() && skip-- == 0) {
            return toChord(e);
        }
    }
    return nullptr;
}

double stretch(Measure* m)
{
    return m->getProperty(Pid::USER_STRETCH).toDouble();
}

Ret setStretch(Measure* m, double v)
{
    m->undoChangeProperty(Pid::USER_STRETCH, v);
    return muse::make_ok();
}

bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.rfind(prefix, 0) == 0;
}

class Agent_Core : public ::testing::Test
{
protected:
    void SetUp() override
    {
        score.reset(load("pickup-bar.mscz"));
        ASSERT_TRUE(score);
        backend = std::make_unique<ScoreUndoBackend>(score.get());
        MScore::setError(MsError::MS_NO_ERROR);
    }

    std::unique_ptr<MasterScore> score;
    std::unique_ptr<ScoreUndoBackend> backend;
};
}

TEST_F(Agent_Core, OneRequestIsOneUndoEntry)
{
    Measure* m1 = score->firstMeasure();
    Measure* m2 = m1->nextMeasure();
    const size_t before = backend->currentIndex();
    {
        Transaction tx(score.get(), *backend);
        ASSERT_TRUE(tx.begin("Agent: stretch"));
        EXPECT_TRUE(score->undoStack()->isLocked());
        EXPECT_TRUE(tx.step("a", [&] { return setStretch(m1, 2.0); }));
        EXPECT_TRUE(tx.step("b", [&] { return setStretch(m2, 3.0); }));
        EXPECT_TRUE(tx.step("c", [&] { return setStretch(m1, 1.5); }));
        ASSERT_TRUE(tx.commit());
        EXPECT_TRUE(tx.changed());
        EXPECT_EQ(tx.stepsRun(), 3);
    }
    EXPECT_FALSE(score->undoStack()->isLocked());
    EXPECT_FALSE(score->undoStack()->hasActiveCommand());
    EXPECT_EQ(backend->currentIndex(), before + 1);
    EXPECT_EQ(backend->topUndoName(), "Agent: stretch");
    EXPECT_DOUBLE_EQ(stretch(m1), 1.5);
    EXPECT_DOUBLE_EQ(stretch(m2), 3.0);

    score->undoRedo(true, nullptr); // one undo reverts all three steps
    EXPECT_DOUBLE_EQ(stretch(m1), 1.0);
    EXPECT_DOUBLE_EQ(stretch(m2), 1.0);
    score->undoRedo(false, nullptr);
    EXPECT_DOUBLE_EQ(stretch(m1), 1.5);
}

TEST_F(Agent_Core, NoChangeLeavesNoUndoEntry)
{
    const size_t before = backend->currentIndex();
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: nothing"));
    EXPECT_TRUE(tx.step("noop", [] { return muse::make_ok(); }));
    ASSERT_TRUE(tx.commit());
    EXPECT_FALSE(tx.changed());
    EXPECT_EQ(backend->currentIndex(), before);
}

TEST_F(Agent_Core, FailedStepRollsEverythingBack)
{
    Measure* m1 = score->firstMeasure();
    const size_t before = backend->currentIndex();
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: fails"));
    EXPECT_TRUE(tx.step("ok", [&] { return setStretch(m1, 2.0); }));
    const Ret r = tx.step("bad", [&] {
        setStretch(m1, 4.0);
        return mu::agent::makeErr(mu::agent::Err::Range, "bar 99 is outside 1-8");
    });
    EXPECT_FALSE(r);
    EXPECT_EQ(r.text(), "step 2 (bad): E_RANGE: bar 99 is outside 1-8");
    EXPECT_FALSE(tx.isOpen());
    EXPECT_DOUBLE_EQ(stretch(m1), 1.0);
    EXPECT_EQ(backend->currentIndex(), before);
    EXPECT_FALSE(score->undoStack()->isLocked());
    EXPECT_FALSE(score->undoStack()->hasActiveCommand());
    // a further step on a rolled-back transaction is refused, not run
    bool ran = false;
    EXPECT_FALSE(tx.step("late", [&] { ran = true; return muse::make_ok(); }));
    EXPECT_FALSE(ran);
}

TEST_F(Agent_Core, MScoreErrorIsARefusalAndRollsBack)
{
    Measure* m1 = score->firstMeasure();
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: refused"));
    const Ret r = tx.step("exchange", [&] {
        setStretch(m1, 2.0);
        MScore::setError(MsError::NO_STAFF_SELECTED);
        return muse::make_ok();
    });
    EXPECT_FALSE(r);
    EXPECT_TRUE(startsWith(r.text(), "step 1 (exchange): E_REFUSED: ")) << r.text();
    EXPECT_DOUBLE_EQ(stretch(m1), 1.0);
    EXPECT_EQ(MScore::_error, MsError::MS_NO_ERROR);
}

TEST_F(Agent_Core, MScoreWarningIsReportedAndCommitted)
{
    Measure* m1 = score->firstMeasure();
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: warns"));
    EXPECT_TRUE(tx.step("transpose", [&] {
        setStretch(m1, 2.0);
        MScore::setError(MsError::TRANSPOSE_NO_FRET_DIAGRAM, true);
        return muse::make_ok();
    }));
    ASSERT_TRUE(tx.commit());
    EXPECT_TRUE(tx.changed());
    ASSERT_EQ(tx.warnings().size(), 1u);
    EXPECT_DOUBLE_EQ(stretch(m1), 2.0);
}

TEST_F(Agent_Core, ExceptionRollsBack)
{
    Measure* m1 = score->firstMeasure();
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: throws"));
    const Ret r = tx.step("boom", [&]() -> Ret {
        setStretch(m1, 2.0);
        throw std::runtime_error("oops");
    });
    EXPECT_TRUE(startsWith(r.text(), "step 1 (boom): E_INTERNAL: exception: oops")) << r.text();
    EXPECT_DOUBLE_EQ(stretch(m1), 1.0);
}

TEST_F(Agent_Core, DestructorRollsBackAnUncommittedTransaction)
{
    Measure* m1 = score->firstMeasure();
    const size_t before = backend->currentIndex();
    {
        Transaction tx(score.get(), *backend);
        ASSERT_TRUE(tx.begin("Agent: abandoned"));
        EXPECT_TRUE(tx.step("a", [&] { return setStretch(m1, 2.0); }));
    }
    EXPECT_DOUBLE_EQ(stretch(m1), 1.0);
    EXPECT_EQ(backend->currentIndex(), before);
    EXPECT_FALSE(score->undoStack()->isLocked());
}

TEST_F(Agent_Core, RefusesWhileSomeoneElseHasAnOpenEditOrTheLock)
{
    // a user drag or plugin holds an open macro
    score->startCmd(muse::TranslatableString::untranslatable("User drag"));
    {
        Transaction tx(score.get(), *backend);
        const Ret r = tx.begin("Agent: x");
        EXPECT_TRUE(startsWith(r.text(), "E_BUSY")) << r.text();
    }
    EXPECT_TRUE(score->undoStack()->hasActiveCommand()); // the user's edit is untouched
    score->endCmd();

    score->undoStack()->setLocked(true);
    {
        Transaction tx(score.get(), *backend);
        EXPECT_TRUE(startsWith(tx.begin("Agent: y").text(), "E_BUSY"));
    }
    score->undoStack()->setLocked(false);
}

TEST_F(Agent_Core, UnwindInsideAStepIsDetected)
{
    Measure* m1 = score->firstMeasure();
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: unwinds"));
    EXPECT_TRUE(tx.step("a", [&] { return setStretch(m1, 2.0); }));
    const Ret r = tx.step("timesig", [&] {
        score->undoStack()->activeCommand()->unwind(); // what cmdAddTimeSig does on some paths
        return muse::make_ok();
    });
    EXPECT_FALSE(r);
    EXPECT_TRUE(startsWith(r.text(), "step 2 (timesig): E_REFUSED")) << r.text();
    EXPECT_DOUBLE_EQ(stretch(m1), 1.0);
    EXPECT_FALSE(score->undoStack()->hasActiveCommand());
}

TEST_F(Agent_Core, UnwindInTheFirstStepIsDetectedToo)
{
    Measure* m1 = score->firstMeasure();
    const size_t before = backend->currentIndex();
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: unwinds first"));
    const Ret r = tx.step("timesig", [&] {
        setStretch(m1, 2.0);
        score->undoStack()->activeCommand()->unwind();
        return muse::make_ok();
    });
    EXPECT_TRUE(startsWith(r.text(), "step 1 (timesig): E_REFUSED")) << r.text();
    EXPECT_DOUBLE_EQ(stretch(m1), 1.0);
    EXPECT_EQ(backend->currentIndex(), before);
    EXPECT_FALSE(score->undoStack()->hasActiveCommand());
    EXPECT_FALSE(score->undoStack()->isLocked());
}

TEST_F(Agent_Core, AStepThatReleasesTheLockFails)
{
    Measure* m1 = score->firstMeasure();
    const size_t before = backend->currentIndex();
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: unlocks"));
    const Ret r = tx.step("parts", [&] {
        setStretch(m1, 2.0);
        score->undoStack()->setLocked(false); // what MasterNotationParts::endGlobalEdit does
        return muse::make_ok();
    });
    EXPECT_TRUE(startsWith(r.text(), "step 1 (parts): E_INTERNAL")) << r.text();
    EXPECT_DOUBLE_EQ(stretch(m1), 1.0);
    EXPECT_EQ(backend->currentIndex(), before);
    EXPECT_FALSE(score->undoStack()->hasActiveCommand());
}

TEST_F(Agent_Core, AStepThatCommitsTheMacroItselfIsUndoneAndDropped)
{
    Measure* m1 = score->firstMeasure();
    const size_t before = backend->currentIndex();
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: commits early"));
    EXPECT_TRUE(tx.step("a", [&] { return setStretch(m1, 2.0); }));
    const Ret r = tx.step("apply", [&] {
        setStretch(m1, 3.0);
        score->undoStack()->setLocked(false); // unlock + apply(), as the UI wrappers do
        score->endCmd();
        return muse::make_ok();
    });
    EXPECT_TRUE(startsWith(r.text(), "step 2 (apply): E_INTERNAL")) << r.text();
    EXPECT_FALSE(tx.isOpen());
    EXPECT_DOUBLE_EQ(stretch(m1), 1.0);
    EXPECT_EQ(backend->currentIndex(), before);
    EXPECT_FALSE(score->undoStack()->canRedo()); // no half-done entry left to redo either
    EXPECT_FALSE(score->undoStack()->hasActiveCommand());
    EXPECT_FALSE(score->undoStack()->isLocked());
}

TEST_F(Agent_Core, RollbackRestoresSelectionAndInputState)
{
    Note* userNote = firstChord(score.get())->upNote();
    score->select(userNote, SelectType::SINGLE, 0);
    InputState& is = score->inputState();
    is.setDuration(DurationType::V_EIGHTH);
    is.setTrack(0);
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: fails after moving things"));
    const Ret r = tx.step("moves", [&] {
        Chord* other = firstChord(score.get(), 2);
        score->select(other, SelectType::SINGLE, 0);
        score->inputState().setDuration(DurationType::V_WHOLE);
        score->inputState().setTrack(3);
        score->inputState().setSegment(other->segment());
        return mu::agent::makeErr(mu::agent::Err::Range, "no");
    });
    EXPECT_FALSE(r);
    ASSERT_TRUE(score->selection().isSingle());
    EXPECT_EQ(score->selection().element(), userNote);
    EXPECT_EQ(score->inputState().duration(), TDuration(DurationType::V_EIGHTH));
    EXPECT_EQ(score->inputState().track(), 0u);
}

TEST_F(Agent_Core, UserListSelectionSurvivesTheEdit)
{
    Note* userNote = firstChord(score.get())->upNote();
    score->select(userNote, SelectType::SINGLE, 0);
    Note* other = firstChord(score.get(), 1)->upNote();

    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: selects"));
    EXPECT_TRUE(score->selection().isNone()); // cleared for the steps
    EXPECT_TRUE(tx.step("select", [&] {
        score->select(other, SelectType::SINGLE, 0);
        return setStretch(score->firstMeasure(), 2.0);
    }));
    ASSERT_TRUE(tx.commit());
    ASSERT_TRUE(score->selection().isSingle());
    EXPECT_EQ(score->selection().element(), userNote);
}

TEST_F(Agent_Core, UserSelectionOfADeletedElementIsDropped)
{
    Chord* chord = firstChord(score.get(), 1);
    score->select(chord->upNote(), SelectType::SINGLE, 0);
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: deletes"));
    EXPECT_TRUE(tx.step("remove", [&] {
        score->undoRemoveElement(chord);
        return muse::make_ok();
    }));
    ASSERT_TRUE(tx.commit());
    EXPECT_TRUE(score->selection().isNone());
}

TEST_F(Agent_Core, UserRangeSelectionSurvivesTheEdit)
{
    Measure* m2 = score->firstMeasure()->nextMeasure();
    score->selection().setRangeTicks(m2->tick(), m2->endTick(), 0, 1);
    score->selection().updateSelectedElements();
    ASSERT_TRUE(score->selection().isRange());

    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: range"));
    EXPECT_TRUE(tx.step("a", [&] { return setStretch(m2, 2.0); }));
    ASSERT_TRUE(tx.commit());
    ASSERT_TRUE(score->selection().isRange());
    EXPECT_EQ(score->selection().tickStart(), m2->tick());
    EXPECT_EQ(score->selection().staffStart(), 0u);
    EXPECT_EQ(score->selection().staffEnd(), 1u);
}

TEST_F(Agent_Core, BarSumPostConditionCatchesAHoleAndRollsBack)
{
    EXPECT_TRUE(barsAddUp(score.get()));
    Chord* chord = firstChord(score.get(), 1);
    const size_t before = backend->currentIndex();
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: hole"));
    const Ret r = tx.step("remove", [&]() -> Ret {
        score->undoRemoveElement(chord); // leaves a gap in voice 1
        return barsAddUp(score.get());
    });
    EXPECT_TRUE(startsWith(r.text(), "step 1 (remove): E_POSTCONDITION: bars no longer add up: m")) << r.text();
    EXPECT_EQ(backend->currentIndex(), before);
    EXPECT_TRUE(barsAddUp(score.get()));
}

// ------------------------------------------------------------------ handles

TEST(Agent_Handles, FormatAndParseRoundTrip)
{
    Handle h;
    h.type = "Note";
    h.eid = "Jx3kQ0aB1cd_Pq9+S2t/vwx";
    h.bar = 12;
    h.offset = Fraction(2, 8);
    h.staff = 2;
    h.voice = 3;
    h.key = "60";
    const std::string s = formatHandle(h);
    EXPECT_EQ(s, "Note:Jx3kQ0aB1cd_Pq9+S2t/vwx@m12+1/4.s2.v3#60");
    const auto back = parseHandle(s);
    ASSERT_TRUE(back.ret) << back.ret.text();
    EXPECT_EQ(back.val.eid, h.eid);
    EXPECT_EQ(back.val.offset, Fraction(1, 4));
    EXPECT_EQ(back.val.staff, 2);
    EXPECT_EQ(back.val.voice, 3);
    EXPECT_EQ(back.val.key, "60");

    h.offset = Fraction(0, 1);
    h.key.clear();
    EXPECT_EQ(formatHandle(h), "Note:Jx3kQ0aB1cd_Pq9+S2t/vwx@m12.s2.v3");
}

TEST(Agent_Handles, MalformedHandlesAreInvalidArgsNotAsserts)
{
    for (const char* bad : { "", "Note", "Note:abc", "Note:a_b@x1.s1.v1", "Note:a_b@m0.s1.v1", "Note:a-b_c@m1.s1.v1",
                             "Note:ab@m1.s1.v1", "Note:a_b_c@m1.s1.v1", "Note:a_b@m1+1/0.s1.v1", "Note:a_b@m1.s1.v5",
                             "Note:a_b@m1.s1", "Note:a_b@m1.s1.v1x", "Note:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_b@m1.s1.v1" }) {
        const auto r = parseHandle(bad);
        EXPECT_FALSE(r.ret) << bad;
        EXPECT_TRUE(startsWith(r.ret.text(), "E_INVALID_ARG")) << bad << " -> " << r.ret.text();
    }
}

TEST_F(Agent_Core, HandleResolvesToTheSameElementAndGoesStaleWhenRemoved)
{
    Chord* chord = firstChord(score.get(), 1);
    Note* note = chord->upNote();
    const std::string hn = makeHandle(note);
    const std::string hc = makeHandle(chord);
    EXPECT_TRUE(startsWith(hn, "Note:")) << hn;
    EXPECT_NE(hn.find("#" + std::to_string(note->pitch())), std::string::npos) << hn;

    auto r = resolveHandle(score.get(), hn);
    ASSERT_TRUE(r.ret) << r.ret.text();
    EXPECT_EQ(r.val, note);
    EXPECT_EQ(resolveHandle(score.get(), hc).val, chord);

    // removed: stale (the EID register still maps the EID, but the walk does not find it)
    Transaction tx(score.get(), *backend);
    ASSERT_TRUE(tx.begin("Agent: remove"));
    EXPECT_TRUE(tx.step("remove", [&] { score->undoRemoveElement(chord); return muse::make_ok(); }));
    ASSERT_TRUE(tx.commit());
    r = resolveHandle(score.get(), hn);
    EXPECT_TRUE(startsWith(r.ret.text(), "E_STALE_HANDLE")) << r.ret.text();

    // undo puts the same object back, so the handle resolves again
    score->undoRedo(true, nullptr);
    r = resolveHandle(score.get(), hn);
    ASSERT_TRUE(r.ret) << r.ret.text();
    EXPECT_EQ(r.val, note);
}

TEST_F(Agent_Core, HandleWithAChangedFingerprintIsStale)
{
    Note* note = firstChord(score.get(), 1)->upNote();
    const std::string h = makeHandle(note);
    std::string other = h.substr(0, h.find('#')) + "#" + std::to_string(note->pitch() + 1);
    EXPECT_TRUE(startsWith(resolveHandle(score.get(), other).ret.text(), "E_STALE_HANDLE"));
    // right place, unknown id
    const std::string unknown = "Note:AAAAAAAAAAA_AAAAAAAAAAA" + h.substr(h.find('@'));
    EXPECT_TRUE(startsWith(resolveHandle(score.get(), unknown).ret.text(), "E_STALE_HANDLE"));
    // outside the score
    EXPECT_TRUE(startsWith(resolveHandle(score.get(), "Note:A_B@m999.s1.v1").ret.text(), "E_RANGE"));
}

// Every element the write groups will name gets a handle that resolves back to it, on every
// synthetic fixture: notes, chords, rests, measures, end barlines, clefs, key and time
// signatures, annotations, lyrics, articulations, ties, spanners, grace chords.
TEST(Agent_Handles, EveryNameableElementInEveryFixtureRoundTrips)
{
    const fs::path dir = fs::path(agent_tests_DATA_ROOT) / "../../../tests/fixtures/synthetic";
    std::map<std::string, int> counts;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".mscz") {
            continue;
        }
        SCOPED_TRACE(entry.path().filename().string());
        std::unique_ptr<MasterScore> score(ScoreRW::readScore(muse::String::fromStdString(entry.path().string()), true));
        ASSERT_TRUE(score);

        std::vector<EngravingItem*> items;
        for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
            items.push_back(m);
            for (EngravingItem* e : m->el()) {
                items.push_back(e);
            }
            for (Segment* s = m->first(); s; s = s->next()) {
                for (EngravingItem* a : s->annotations()) {
                    items.push_back(a);
                }
                for (size_t t = 0; t < score->ntracks(); ++t) {
                    EngravingItem* e = s->element(t);
                    if (!e) {
                        continue;
                    }
                    items.push_back(e);
                    if (!e->isChord()) {
                        continue;
                    }
                    Chord* c = toChord(e);
                    for (Chord* g : c->graceNotes()) {
                        items.push_back(g);
                        for (Note* n : g->notes()) {
                            items.push_back(n);
                        }
                    }
                    for (Lyrics* l : c->lyrics()) {
                        items.push_back(l);
                    }
                    for (Articulation* a : c->articulations()) {
                        items.push_back(a);
                    }
                    for (Note* n : c->notes()) {
                        items.push_back(n);
                        if (n->tieFor()) {
                            items.push_back(n->tieFor());
                        }
                    }
                }
            }
        }
        for (const auto& [tick, sp] : score->spanner()) {
            items.push_back(sp);
        }

        for (EngravingItem* e : items) {
            const std::string h = makeHandle(e);
            const auto r = resolveHandle(score.get(), h);
            EXPECT_TRUE(r.ret && r.val == e) << e->typeName() << " " << h << " -> " << r.ret.text();
            ++counts[e->typeName()];
        }
    }
    std::string seen;
    for (const auto& [type, n] : counts) {
        seen += type + "=" + std::to_string(n) + " ";
    }
    RecordProperty("types", seen);
    EXPECT_GT(counts["Note"], 100) << seen;
    EXPECT_GT(counts["Tie"], 0) << seen;
    EXPECT_GT(counts["Slur"], 0) << seen;
    EXPECT_GT(counts["Lyrics"], 0) << seen;
    EXPECT_GT(counts["BarLine"], 0) << seen;
    EXPECT_GT(counts["Measure"], 0) << seen;
}
