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
// P2.4 text: add_text (staff, system, expression, tempo, rehearsal), add_chord_symbol, add_lyric.
// Each operation runs inside a core Transaction on a fixture with the score's own undo stack
// (as expression_marks_tests.cpp), so every validation error is checked to leave the score and
// the undo stack untouched, and every edit is undone and redone.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "engraving/dom/chord.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/expression.h"
#include "engraving/dom/fret.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/rehearsalmark.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/select.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/systemtext.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/textbase.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/editing/transpose.h"
#include "engraving/editing/undo.h"
#include "engraving/style/style.h"
#include "engraving/tests/mocks/engravingconfigurationmock.h"
#include "engraving/tests/utils/scorerw.h"
#include "engraving/types/constants.h"

#include "commands/text/textops.h"
#include "core/handles.h"
#include "core/transaction.h"
#include "core/undobackend.h"
#include "registry/errors.h"

#include "modularity/ioc.h"

using namespace mu::agent;
using namespace mu::agent::core;
using namespace mu::engraving;
using muse::Ret;
using muse::Val;
using muse::ValList;
using muse::ValMap;
namespace tx = mu::agent::text;
namespace fs = std::filesystem;
using AErr = mu::agent::Err;

namespace {
using Op = std::function<Ret(const Params&, MasterScore*, tx::Out&)>;

const Op TXT = tx::addText;
const Op CHS = tx::addChordSymbol;
const Op LYR = tx::addLyric;

MasterScore* loadPath(const fs::path& p)
{
    return ScoreRW::readScore(muse::String::fromStdString(p.string()), true);
}

MasterScore* load(const char* name)
{
    return loadPath(fs::path(agent_tests_DATA_ROOT) / "../../../tests/fixtures/synthetic" / name);
}

Measure* bar(Score* score, int n)
{
    int i = 0;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        if (++i == n) {
            return m;
        }
    }
    return nullptr;
}

int barOf(Score* score, const Measure* target)
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

Fraction tickAt(Score* score, int measure, double beat)
{
    return bar(score, measure)->tick() + Fraction::fromTicks(static_cast<int>(std::lround((beat - 1.0) * Constants::DIVISION)));
}

//! The chord or rest starting at bar/beat on a track (nullptr if none)
ChordRest* crAt(Score* score, int measure, double beat, track_idx_t track = 0)
{
    Measure* m = bar(score, measure);
    Segment* s = m ? m->findSegment(SegmentType::ChordRest, tickAt(score, measure, beat)) : nullptr;
    EngravingItem* e = s ? s->element(track) : nullptr;
    return e && e->isChordRest() ? toChordRest(e) : nullptr;
}

ChordRest* find(Score* score, track_idx_t track, const std::function<bool(ChordRest*)>& pred)
{
    for (Segment* s = score->firstSegment(SegmentType::ChordRest); s; s = s->next1(SegmentType::ChordRest)) {
        EngravingItem* e = s->element(track);
        if (e && e->isChordRest() && pred(toChordRest(e))) {
            return toChordRest(e);
        }
    }
    return nullptr;
}

Val posBeat(int measure, double beat)
{
    ValMap m;
    m["measure"] = Val(measure);
    m["beat"] = Val(beat);
    return Val(m);
}

Val posOffset(int measure, const std::string& offset)
{
    ValMap m;
    m["measure"] = Val(measure);
    m["offset"] = Val(offset);
    return Val(m);
}

Val pos(Score* score, const ChordRest* cr)
{
    return posOffset(barOf(score, cr->measure()), (cr->tick() - cr->measure()->tick()).reduced().toString().toStdString());
}

Val staffNo(int n)
{
    ValMap m;
    m["number"] = Val(n);
    return Val(m);
}

Val staffPart(const std::string& part)
{
    ValMap m;
    m["part"] = Val(part);
    return Val(m);
}

Params where(int staff, int measure, double beat, int voice = 0)
{
    Params p;
    p["staff"] = staffNo(staff);
    p["at"] = posBeat(measure, beat);
    if (voice) {
        p["voice"] = Val(voice);
    }
    return p;
}

Params at(Score* score, const ChordRest* cr)
{
    Params p;
    p["staff"] = staffNo(static_cast<int>(cr->staffIdx()) + 1);
    p["at"] = pos(score, cr);
    p["voice"] = Val(static_cast<int>(cr->voice()) + 1);
    return p;
}

Params with(Params p, const std::string& k, const Val& v)
{
    p[k] = v;
    return p;
}

Params without(Params p, const std::string& k)
{
    p.erase(k);
    return p;
}

Val tempoArg(double bpm, const std::string& unit = {})
{
    ValMap t;
    t["bpm"] = Val(bpm);
    if (!unit.empty()) {
        t["beat_unit"] = Val(unit);
    }
    return Val(t);
}

Val syl(const std::string& text, const std::string& syllabic = {}, const Val& melismaTo = Val())
{
    ValMap s;
    s["text"] = Val(text);
    if (!syllabic.empty()) {
        s["syllabic"] = Val(syllabic);
    }
    if (!melismaTo.isNull()) {
        s["melisma_to"] = melismaTo;
    }
    return Val(s);
}

Val syls(const std::vector<Val>& v)
{
    return Val(ValList(v.begin(), v.end()));
}

int err(AErr e)
{
    return static_cast<int>(e);
}

//! Annotations of a type anywhere in a score (master or part)
int countAnnotations(const Score* s, ElementType t)
{
    int n = 0;
    for (const Segment* seg = s->firstSegment(SegmentType::All); seg; seg = seg->next1()) {
        for (const EngravingItem* a : seg->annotations()) {
            n += a->type() == t;
        }
    }
    return n;
}

int countLyrics(const Score* s)
{
    int n = 0;
    for (const Segment* seg = s->firstSegment(SegmentType::ChordRest); seg; seg = seg->next1(SegmentType::ChordRest)) {
        for (track_idx_t t = 0; t < s->ntracks(); ++t) {
            const EngravingItem* e = seg->element(t);
            if (e && e->isChordRest()) {
                n += static_cast<int>(toChordRest(e)->lyrics().size());
            }
        }
    }
    return n;
}

std::vector<int> perPart(MasterScore* score, const std::function<int(const Score*)>& count)
{
    std::vector<int> v;
    for (Excerpt* ex : score->excerpts()) {
        v.push_back(ex->excerptScore() ? count(ex->excerptScore()) : -1);
    }
    return v;
}

int sum(const std::vector<int>& v)
{
    int n = 0;
    for (int x : v) {
        n += x;
    }
    return n;
}

std::string plainOf(const TextBase* t)
{
    return t ? t->plainText().toStdString() : std::string("<none>");
}

EngravingItem* annotation(Segment* seg, ElementType t, track_idx_t track)
{
    for (EngravingItem* a : seg->annotations()) {
        if (a->type() == t && a->track() == track) {
            return a;
        }
    }
    return nullptr;
}

std::vector<Harmony*> harmoniesAt(Segment* seg, track_idx_t track)
{
    std::vector<Harmony*> v;
    for (EngravingItem* a : seg->annotations()) {
        if (a->isHarmony() && a->track() == track) {
            v.push_back(toHarmony(a));
        }
    }
    return v;
}

class Agent_Text : public ::testing::Test
{
protected:
    void open(const char* name) { adopt(load(name)); }

    void adopt(MasterScore* s)
    {
        score.reset(s);
        ASSERT_TRUE(score);
        backend = std::make_unique<ScoreUndoBackend>(score.get());
        MScore::setError(MsError::MS_NO_ERROR);
    }

    void SetUp() override { open("chord-symbols.mscz"); }

    Ret run(const Op& op, const Params& p)
    {
        out.clear();
        Transaction t(score.get(), *backend);
        if (Ret r = t.begin("Agent: test"); !r) {
            return r;
        }
        if (Ret r = t.step("op", [&] { return op(p, score.get(), out); }); !r) {
            return r;
        }
        Ret r = t.commit();
        changed = t.changed();
        return r;
    }

    //! A set-up edit inside its own transaction (not under test)
    void setup(const std::function<void()>& f)
    {
        Transaction t(score.get(), *backend);
        ASSERT_TRUE(t.begin("setup"));
        ASSERT_TRUE(t.step("setup", [&] { f(); return muse::make_ok(); }));
        ASSERT_TRUE(t.commit());
    }

    std::string snapshot()
    {
        const fs::path f = fs::temp_directory_path() / "agent_text_snapshot.mscx";
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
        EXPECT_EQ(r.code(), err(e)) << r.text();
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
    bool outHas(const std::string& k, const std::string& needle) const { return outStr(k.c_str()).find(needle) != std::string::npos; }

    std::unique_ptr<MasterScore> score;
    std::unique_ptr<ScoreUndoBackend> backend;
    tx::Out out;
    bool changed = false;
};

Params textArgs(const std::string& kind, const std::string& text, int staff = 1, int measure = 1, double beat = 1)
{
    Params p = where(staff, measure, beat);
    p["kind"] = Val(kind);
    if (!text.empty()) {
        p["text"] = Val(text);
    }
    return p;
}

Params chordArgs(const std::string& text, int measure = 1, double beat = 1, int staff = 1)
{
    Params p = where(staff, measure, beat);
    p["text"] = Val(text);
    return p;
}

Params lyricArgs(const std::vector<Val>& s, int measure = 1, double beat = 1, int staff = 1)
{
    Params p = where(staff, measure, beat);
    p["syllables"] = syls(s);
    return p;
}

//! Bar 2 becomes a one-bar measure repeat (as expression_marks_tests.cpp sets it up)
void makeMeasureRepeat(MasterScore* score)
{
    Measure* m2 = bar(score, 2);
    score->deleteRange(m2->first(SegmentType::ChordRest), m2->nextMeasure()->first(SegmentType::ChordRest), 0, VOICES,
                       SelectionFilter(), false);
    score->cmdAddMeasureRepeat(m2, 1, 0);
}
}

// ================================================================== shared addressing

TEST_F(Agent_Text, AddressErrors)
{
    const Params base = textArgs("staff", "pizz.");
    refused(TXT, without(base, "staff"), AErr::InvalidArg, "missing staff");
    refused(TXT, with(base, "staff", staffNo(9)), AErr::Range, "outside");
    refused(TXT, with(base, "staff", staffPart("Tuba")), AErr::Range, "no part named");
    refused(TXT, without(base, "at"), AErr::InvalidArg, "missing at");
    refused(TXT, with(base, "at", Val(3)), AErr::InvalidArg, "object");
    refused(TXT, with(base, "voice", Val(5)), AErr::Range, "voice must be 1-4");
    refused(TXT, with(base, "at", posBeat(99, 1)), AErr::Range, "outside the score");
    refused(TXT, with(base, "at", posBeat(1, 9)), AErr::Range, "past its end");
    refused(TXT, with(base, "at", posBeat(1, 1.5)), AErr::Range, "no note or rest starts");
    refused(TXT, with(base, "voice", Val(2)), AErr::Range, "voice 2");
    ChordRest* cr = crAt(score.get(), 1, 1);
    ASSERT_TRUE(cr);
    refused(TXT, with(base, "target", Val(makeHandle(cr))), AErr::InvalidArg, "not both");
    Params t;
    t["kind"] = Val("staff");
    t["text"] = Val("pizz.");
    refused(TXT, with(t, "target", Val(3)), AErr::InvalidArg, "handle string");
    refused(TXT, with(t, "target", Val("garbage")), AErr::InvalidArg);
    refused(TXT, with(t, "target", Val(makeHandle(cr->measure()))), AErr::InvalidArg, "names a");
    // by part name and by a note handle both work
    EXPECT_TRUE(run(TXT, with(textArgs("staff", "arco"), "staff", staffPart("piano"))));
    ASSERT_TRUE(cr->isChord());
    EXPECT_TRUE(run(TXT, with(t, "target", Val(makeHandle(toChord(cr)->notes().front())))));
}

TEST_F(Agent_Text, MeasureRepeatRefused)
{
    setup([&] { makeMeasureRepeat(score.get()); });
    ChordRest* mr = crAt(score.get(), 2, 1);
    ASSERT_TRUE(mr && mr->isMeasureRepeat());
    refused(TXT, textArgs("staff", "pizz.", 1, 2, 1), AErr::InvalidArg, "measure repeat");
    refused(LYR, lyricArgs({ syl("la") }, 2, 1), AErr::InvalidArg, "measure repeat");
    // a sequence that runs into the repeat bar
    refused(LYR, lyricArgs({ syl("a"), syl("b") }, 1, 4), AErr::InvalidArg, "measure repeat");
}

// ================================================================== add_text: validation

TEST_F(Agent_Text, TextValidation)
{
    const Params base = textArgs("staff", "pizz.");
    refused(TXT, with(base, "kind", Val("poem")), AErr::InvalidArg, "kind must be");
    refused(TXT, without(base, "text"), AErr::InvalidArg, "needs text");
    refused(TXT, with(base, "text", Val("   ")), AErr::InvalidArg, "empty");
    refused(TXT, with(base, "xml", Val("<b>x</b>")), AErr::InvalidArg, "not both");
    refused(TXT, with(base, "tempo", tempoArg(120)), AErr::InvalidArg, "kind tempo only");
    refused(TXT, with(base, "voice_assignment", Val("this_voice")), AErr::InvalidArg, "expression only");
    refused(TXT, with(with(base, "kind", Val("expression")), "voice_assignment", Val("sometimes")), AErr::InvalidArg,
            "voice_assignment must");
    refused(TXT, with(without(base, "text"), "xml", Val("  ")), AErr::InvalidArg, "empty");
}

TEST_F(Agent_Text, MarkupValidation)
{
    const Params base = without(textArgs("staff", ""), "text");
    const std::vector<std::pair<std::string, std::string> > bad = {
        { "<b>dolce", "is not closed" },
        { "dol<ce", "is not closed" },
        { "<font size=\"12\">x</font>", "not allowed" },
        { "<sym>noSuchSymbol</sym>", "unknown symbol" },
        { "x</sym>", "without <sym>" },
        { "<sym>metNoteQuarterUp", "without </sym>" },
        { "<b><i>x</b></i>", "does not close" },
        { "</b>", "does not close" },
        { "a > b", "bare '>'" },
        { "Tom & Jerry", "not an entity" },
        { "&nbsp;", "not an entity" },
        { "<b> </b>", "no text" },
    };
    for (const auto& [xml, why] : bad) {
        refused(TXT, with(base, "xml", Val(xml)), AErr::InvalidArg, why);
    }
    // good markup is stored as given
    ASSERT_TRUE(run(TXT, with(base, "xml", Val("<i>dolce</i> &amp; <sym>metNoteQuarterUp</sym>"))));
    Segment* seg = crAt(score.get(), 1, 1)->segment();
    StaffText* t = toStaffText(annotation(seg, ElementType::STAFF_TEXT, 0));
    ASSERT_TRUE(t);
    EXPECT_EQ(t->xmlText(), u"<i>dolce</i> &amp; <sym>metNoteQuarterUp</sym>");
}

// ================================================================== add_text: kinds

TEST_F(Agent_Text, StaffTextAddUndoRedo)
{
    Segment* seg = crAt(score.get(), 3, 1)->segment();
    const int before = countAnnotations(score.get(), ElementType::STAFF_TEXT);
    addsOneEntry(TXT, textArgs("staff", "pizz.", 1, 3, 1),
                 [&] {
        StaffText* t = toStaffText(annotation(seg, ElementType::STAFF_TEXT, 0));
        ASSERT_TRUE(t);
        EXPECT_EQ(t->xmlText(), u"pizz.");
        EXPECT_EQ(countAnnotations(score.get(), ElementType::STAFF_TEXT), before + 1);
    },
                 [&] { EXPECT_EQ(countAnnotations(score.get(), ElementType::STAFF_TEXT), before); });
    EXPECT_EQ(outStr("status"), "added");
    EXPECT_EQ(outStr("text"), "pizz.");
    EXPECT_TRUE(outHas("notation", "+\"pizz.\"")) << outStr("notation");
    muse::RetVal<EngravingItem*> h = resolveHandle(score.get(), outStr("handle"));
    ASSERT_TRUE(h.ret) << h.ret.text();
    EXPECT_TRUE(h.val->isStaffText());
    // plain text is escaped
    ASSERT_TRUE(run(TXT, textArgs("staff", "A & B", 1, 3, 2)));
    StaffText* amp = toStaffText(annotation(crAt(score.get(), 3, 2)->segment(), ElementType::STAFF_TEXT, 0));
    ASSERT_TRUE(amp);
    EXPECT_EQ(amp->xmlText(), u"A &amp; B");
    EXPECT_EQ(amp->plainText(), u"A & B");
    // the same text at the same place again is refused; another text there is fine
    refused(TXT, textArgs("staff", "pizz.", 1, 3, 1), AErr::Exists, "already has that text");
    EXPECT_TRUE(run(TXT, textArgs("staff", "sul pont.", 1, 3, 1)));
}

TEST_F(Agent_Text, StaffTextKeepsVoice)
{
    open("voices-four.mscz");
    ChordRest* v2 = find(score.get(), 1, [](ChordRest* c) { return c->isChord(); });
    ASSERT_TRUE(v2);
    ASSERT_TRUE(run(TXT, with(with(at(score.get(), v2), "kind", Val("staff")), "text", Val("solo"))));
    EngravingItem* t = annotation(v2->segment(), ElementType::STAFF_TEXT, 1);
    ASSERT_TRUE(t) << "staff text keeps the voice-2 track (chordrest.cpp:1292)";
    EXPECT_EQ(out.at("voice").toInt(), 2);
}

TEST_F(Agent_Text, StaffTextReachesLinkedParts)
{
    open("parts-linked.mscz");
    ASSERT_FALSE(score->excerpts().empty());
    ChordRest* cr = find(score.get(), 0, [](ChordRest* c) { return c->isChord() && c->tick().isNotZero(); });
    ASSERT_TRUE(cr);
    auto count = [](const Score* s) { return countAnnotations(s, ElementType::STAFF_TEXT); };
    const std::vector<int> before = perPart(score.get(), count);
    addsOneEntry(TXT, with(with(at(score.get(), cr), "kind", Val("staff")), "text", Val("con sord.")),
                 [&] {
        StaffText* t = toStaffText(annotation(cr->segment(), ElementType::STAFF_TEXT, cr->track()));
        ASSERT_TRUE(t);
        EXPECT_EQ(t->linkList().size(), 3u) << "the master, the Flute part and the Flute and Clarinet part";
        for (EngravingObject* o : t->linkList()) {
            EXPECT_EQ(toTextBase(o)->xmlText(), u"con sord.");
        }
        EXPECT_EQ(sum(perPart(score.get(), count)), sum(before) + 2);
    },
                 [&] { EXPECT_EQ(perPart(score.get(), count), before); });
}

TEST_F(Agent_Text, SystemTextGoesToTopStaffAndEveryPart)
{
    open("parts-linked.mscz");
    ChordRest* low = crAt(score.get(), 1, 1, 2 * VOICES);
    ASSERT_TRUE(low) << "the bassoon starts bar 1";
    auto count = [](const Score* s) { return countAnnotations(s, ElementType::SYSTEM_TEXT); };
    const std::vector<int> before = perPart(score.get(), count);
    addsOneEntry(TXT, textArgs("system", "Tutti", 3, 1, 1),
                 [&] {
        SystemText* t = toSystemText(annotation(low->segment(), ElementType::SYSTEM_TEXT, 0));
        ASSERT_TRUE(t) << "on the top staff, voice 1 (edit.cpp:6716-6792)";
        EXPECT_EQ(t->xmlText(), u"Tutti");
        const std::vector<int> now = perPart(score.get(), count);
        for (size_t i = 0; i < now.size(); ++i) {
            EXPECT_EQ(now[i], before[i] + 1) << "part " << i;
        }
    },
                 [&] { EXPECT_EQ(perPart(score.get(), count), before); });
    EXPECT_EQ(out.at("staff").toInt(), 1);
    EXPECT_TRUE(outHas("notation", "+sys\"Tutti\"")) << outStr("notation");
    refused(TXT, textArgs("system", "Tutti", 1, 1, 1), AErr::Exists);
}

TEST_F(Agent_Text, ExpressionVoiceAssignment)
{
    open("voices-four.mscz");
    ChordRest* v2 = find(score.get(), 1, [](ChordRest* c) { return c->isChord(); });
    ASSERT_TRUE(v2);
    const Params base = with(with(at(score.get(), v2), "kind", Val("expression")), "text", Val("dolce"));
    addsOneEntry(TXT, base,
                 [&] {
        EngravingItem* e = annotation(v2->segment(), ElementType::EXPRESSION, 0);
        ASSERT_TRUE(e) << "all_voices (the default) moves it to voice 1";
        EXPECT_EQ(e->getProperty(Pid::VOICE_ASSIGNMENT).value<VoiceAssignment>(), VoiceAssignment::ALL_VOICE_IN_INSTRUMENT);
        EXPECT_EQ(toTextBase(e)->xmlText(), u"dolce");
    },
                 [&] { EXPECT_FALSE(annotation(v2->segment(), ElementType::EXPRESSION, 0)); });
    EXPECT_EQ(outStr("voiceAssignment"), "all_voices");
    ASSERT_TRUE(run(TXT, with(with(base, "text", Val("espress.")), "voice_assignment", Val("this_voice"))));
    EngravingItem* e = annotation(v2->segment(), ElementType::EXPRESSION, 1);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->getProperty(Pid::VOICE_ASSIGNMENT).value<VoiceAssignment>(), VoiceAssignment::CURRENT_VOICE_ONLY);
    EXPECT_EQ(outStr("voiceAssignment"), "this_voice");
    refused(TXT, base, AErr::Exists, "already has that text");
}

// ------------------------------------------------------------------ tempo

TEST_F(Agent_Text, TempoValidation)
{
    const Params base = without(textArgs("tempo", ""), "text");
    refused(TXT, base, AErr::InvalidArg, "needs text");
    refused(TXT, with(base, "tempo", Val(120)), AErr::InvalidArg, "must be an object");
    ValMap unknown;
    unknown["speed"] = Val(120);
    refused(TXT, with(base, "tempo", Val(unknown)), AErr::InvalidArg, "unknown key");
    refused(TXT, with(base, "tempo", tempoArg(0)), AErr::InvalidArg, "greater than 0");
    ValMap notNum;
    notNum["bpm"] = Val("fast");
    refused(TXT, with(base, "tempo", Val(notNum)), AErr::InvalidArg, "greater than 0");
    refused(TXT, with(base, "tempo", tempoArg(120, "z")), AErr::InvalidArg, "beat_unit must"); // x is a 64th
    ValMap unitOnly;
    unitOnly["beat_unit"] = Val("q");
    refused(TXT, with(base, "tempo", Val(unitOnly)), AErr::InvalidArg, "needs tempo.bpm");
    ValMap follow;
    follow["bpm"] = Val(100);
    follow["follow_text"] = Val("yes");
    refused(TXT, with(base, "tempo", Val(follow)), AErr::InvalidArg, "true or false");
    refused(TXT, with(base, "tempo", tempoArg(2000)), AErr::InvalidArg, "5-999");
    refused(TXT, with(base, "tempo", tempoArg(4, "q")), AErr::InvalidArg, "5-999");
    refused(TXT, with(base, "tempo", tempoArg(600, "w")), AErr::InvalidArg, "5-999"); // 2400 quarters a minute
    refused(TXT, with(with(base, "xml", Val("<sym>metNoteQuarterUp</sym> = 96")), "tempo", tempoArg(120)), AErr::InvalidArg,
            "not both");
    ASSERT_TRUE(run(TXT, with(base, "tempo", tempoArg(100))));
    refused(TXT, with(base, "text", Val("Presto")), AErr::Exists, "already has a tempo marking");
}

TEST_F(Agent_Text, TempoFromBpmSetsTheTempoMap)
{
    const Fraction tick = tickAt(score.get(), 1, 1);
    const double before = score->tempo(tick).val;
    addsOneEntry(TXT, with(without(textArgs("tempo", ""), "text"), "tempo", tempoArg(132)),
                 [&] {
        TempoText* t = toTempoText(annotation(crAt(score.get(), 1, 1)->segment(), ElementType::TEMPO_TEXT, 0));
        ASSERT_TRUE(t);
        EXPECT_EQ(t->xmlText(), u"<sym>metNoteQuarterUp</sym> = 132");
        EXPECT_TRUE(t->followText());
        EXPECT_NEAR(t->tempo().val, 2.2, 1e-9);
        EXPECT_NEAR(score->tempo(tick).val, 2.2, 1e-9);
    },
                 [&] { EXPECT_NEAR(score->tempo(tick).val, before, 1e-9); });
    EXPECT_NEAR(out.at("bpm").toDouble(), 132.0, 1e-6);
    EXPECT_TRUE(outHas("notation", "+tempo(q=132)")) << outStr("notation");
}

TEST_F(Agent_Text, TempoWordsWithBpm)
{
    const Fraction tick = tickAt(score.get(), 2, 1);
    ASSERT_TRUE(run(TXT, with(textArgs("tempo", "Allegro", 1, 2, 1), "tempo", tempoArg(144))));
    TempoText* t = toTempoText(annotation(crAt(score.get(), 2, 1)->segment(), ElementType::TEMPO_TEXT, 0));
    ASSERT_TRUE(t);
    EXPECT_EQ(t->xmlText(), u"Allegro");
    EXPECT_FALSE(t->followText()) << "spec Q5: the words alone would not give the number back";
    EXPECT_NEAR(t->tempo().val, 2.4, 1e-9);
    EXPECT_NEAR(score->tempo(tick).val, 2.4, 1e-9);
    EXPECT_FALSE(out.at("followText").toBool());
    EXPECT_TRUE(outHas("notation", "+tempo\"Allegro\"(q=144)")) << outStr("notation");
}

TEST_F(Agent_Text, TempoWordsOnlyKeepsTheDefault)
{
    ASSERT_TRUE(run(TXT, textArgs("tempo", "Andante", 1, 3, 1)));
    TempoText* t = toTempoText(annotation(crAt(score.get(), 3, 1)->segment(), ElementType::TEMPO_TEXT, 0));
    ASSERT_TRUE(t);
    EXPECT_EQ(t->xmlText(), u"Andante");
    EXPECT_NEAR(out.at("bpm").toDouble(), 80.0, 1e-6) << "UI parity (spec TX-08)";
    ASSERT_TRUE(out.count("messages"));
    EXPECT_NE(out.at("messages").toList().front().toString().find("no metronome mark"), std::string::npos);
}

TEST_F(Agent_Text, TempoMetronomeMarkInMarkup)
{
    Params p = without(textArgs("tempo", "", 1, 4, 1), "text");
    p["xml"] = Val("Lento <sym>metNoteQuarterUp</sym> = 56");
    ASSERT_TRUE(run(TXT, p));
    EXPECT_NEAR(out.at("bpm").toDouble(), 56.0, 1e-6);
    EXPECT_TRUE(out.at("followText").toBool());
}

TEST_F(Agent_Text, TempoDefaultBeatInCompoundTime)
{
    open("pickup-bar-compound.mscz");
    ChordRest* cr = crAt(score.get(), 2, 1);
    ASSERT_TRUE(cr) << "bar 2 of the 6/8 fixture starts with a note or rest";
    Params p = at(score.get(), cr);
    p["kind"] = Val("tempo");
    p["tempo"] = tempoArg(60);
    ASSERT_TRUE(run(TXT, p));
    TempoText* t = toTempoText(annotation(cr->segment(), ElementType::TEMPO_TEXT, 0));
    ASSERT_TRUE(t);
    EXPECT_EQ(t->xmlText(), u"<sym>metNoteQuarterUp</sym><sym>space</sym><sym>metAugmentationDot</sym> = 60");
    EXPECT_NEAR(t->tempo().val, 1.5, 1e-9) << "60 dotted quarters a minute = 90 quarters";
}

// ------------------------------------------------------------------ rehearsal

TEST_F(Agent_Text, RehearsalMarksSequence)
{
    const Params p = without(textArgs("rehearsal", ""), "text");
    addsOneEntry(TXT, p,
                 [&] {
        EngravingItem* r = annotation(crAt(score.get(), 1, 1)->segment(), ElementType::REHEARSAL_MARK, 0);
        ASSERT_TRUE(r);
        EXPECT_EQ(toTextBase(r)->xmlText(), u"A");
    },
                 [&] { EXPECT_FALSE(annotation(crAt(score.get(), 1, 1)->segment(), ElementType::REHEARSAL_MARK, 0)); });
    EXPECT_TRUE(outHas("notation", "mark=A")) << outStr("notation");
    ASSERT_TRUE(run(TXT, with(p, "at", posBeat(3, 1))));
    EXPECT_EQ(outStr("text"), "B");
    ASSERT_TRUE(run(TXT, textArgs("rehearsal", "Intro", 1, 5, 1)));
    EXPECT_EQ(outStr("text"), "Intro");
    refused(TXT, p, AErr::Exists, "already has a rehearsal mark");
}

// ================================================================== add_chord_symbol

TEST_F(Agent_Text, ChordSymbolValidation)
{
    const Params base = chordArgs("Dm7", 1, 2);
    refused(CHS, without(base, "text"), AErr::InvalidArg, "missing text");
    refused(CHS, with(base, "text", Val(7)), AErr::InvalidArg, "missing text");
    refused(CHS, with(base, "text", Val(" ")), AErr::InvalidArg, "empty");
    refused(CHS, with(base, "notation", Val("jazz")), AErr::InvalidArg, "notation must");
    refused(CHS, with(base, "if_exists", Val("merge")), AErr::InvalidArg, "if_exists must");
    refused(CHS, without(base, "staff"), AErr::InvalidArg, "missing staff");
    refused(CHS, with(base, "staff", staffNo(4)), AErr::Range, "outside");
    refused(CHS, without(base, "at"), AErr::InvalidArg, "missing at");
    refused(CHS, with(base, "voice", Val(0)), AErr::Range, "voice must be 1-4");
    refused(CHS, with(base, "at", posBeat(1, 5)), AErr::Range, "past its end");
    refused(CHS, with(base, "at", posBeat(11, 1)), AErr::Range, "outside the score");
    refused(CHS, with(base, "target", Val(makeHandle(crAt(score.get(), 1, 2)))), AErr::InvalidArg, "not both");
    // bar 1 beat 1 already has C: if_exists defaults to error
    refused(CHS, chordArgs("C6"), AErr::Exists, "already has the chord symbol");
}

TEST_F(Agent_Text, ChordSymbolOnNoteUndoRedo)
{
    Segment* seg = crAt(score.get(), 1, 2)->segment();
    ASSERT_TRUE(harmoniesAt(seg, 0).empty());
    addsOneEntry(CHS, chordArgs("Dm7", 1, 2),
                 [&] {
        const std::vector<Harmony*> hs = harmoniesAt(seg, 0);
        ASSERT_EQ(hs.size(), 1u);
        EXPECT_EQ(hs.front()->harmonyName(), u"Dm7");
        EXPECT_EQ(hs.front()->plainText(), u"Dm7");
        EXPECT_EQ(hs.front()->harmonyType(), HarmonyType::STANDARD);
        EXPECT_TRUE(tpcIsValid(hs.front()->rootTpc()));
    },
                 [&] { EXPECT_TRUE(harmoniesAt(seg, 0).empty()); });
    EXPECT_EQ(outStr("status"), "added");
    EXPECT_EQ(outStr("name"), "Dm7");
    EXPECT_TRUE(out.at("recognised").toBool());
    EXPECT_TRUE(outHas("notation", "m1 h | {C}:q {Dm7}:q {Am7}:h")) << outStr("notation");
}

TEST_F(Agent_Text, ChordSymbolBetweenNotesMakesASegment)
{
    Measure* m2 = bar(score.get(), 2);
    const Fraction tick = m2->tick() + Fraction(1, 8);
    ASSERT_FALSE(m2->findSegment(SegmentType::ChordRest, tick));
    Params p = chordArgs("G7/B", 2, 1);
    p["at"] = posOffset(2, "1/8");
    addsOneEntry(CHS, p,
                 [&] {
        Segment* s = m2->findSegment(SegmentType::ChordRest, tick);
        ASSERT_TRUE(s) << "annotation-only segment (navigateToHarmony, notationinteraction.cpp:7334)";
        ASSERT_EQ(harmoniesAt(s, 0).size(), 1u);
        EXPECT_EQ(harmoniesAt(s, 0).front()->harmonyName(), u"G7/B");
    },
                 [&] { EXPECT_FALSE(m2->findSegment(SegmentType::ChordRest, tick)); });
    EXPECT_TRUE(outHas("notation", "{G7/B}")) << outStr("notation");
}

TEST_F(Agent_Text, ChordSymbolReplaceAndUnchanged)
{
    Segment* seg = crAt(score.get(), 1, 1)->segment();
    ASSERT_EQ(harmoniesAt(seg, 0).size(), 1u);
    ASSERT_EQ(harmoniesAt(seg, 0).front()->harmonyName(), u"C");
    // the same chord again: no change, no undo entry
    const size_t idx = backend->currentIndex();
    ASSERT_TRUE(run(CHS, with(chordArgs("C"), "if_exists", Val("replace"))));
    EXPECT_EQ(outStr("status"), "unchanged");
    EXPECT_FALSE(changed);
    EXPECT_EQ(backend->currentIndex(), idx);
    addsOneEntry(CHS, with(chordArgs("C6"), "if_exists", Val("replace")),
                 [&] {
        ASSERT_EQ(harmoniesAt(seg, 0).size(), 1u);
        EXPECT_EQ(harmoniesAt(seg, 0).front()->harmonyName(), u"C6");
    },
                 [&] {
        ASSERT_EQ(harmoniesAt(seg, 0).size(), 1u);
        EXPECT_EQ(harmoniesAt(seg, 0).front()->harmonyName(), u"C");
    });
    EXPECT_EQ(outStr("status"), "replaced");
    // a mid-note chord symbol replaced: its annotation-only segment goes and comes back
    Measure* m8 = bar(score.get(), 8);
    Segment* mid = m8->findSegment(SegmentType::ChordRest, m8->tick() + Fraction(1, 2));
    ASSERT_TRUE(mid && !harmoniesAt(mid, 0).empty()) << "the fixture changes chord in the middle of bar 8";
    ASSERT_TRUE(run(CHS, with(chordArgs("F", 8, 3), "if_exists", Val("replace"))));
    mid = m8->findSegment(SegmentType::ChordRest, m8->tick() + Fraction(1, 2));
    ASSERT_TRUE(mid);
    ASSERT_EQ(harmoniesAt(mid, 0).size(), 1u);
    EXPECT_EQ(harmoniesAt(mid, 0).front()->harmonyName(), u"F");
}

TEST_F(Agent_Text, ChordSymbolUnrecognisedAndAccidentals)
{
    ASSERT_TRUE(run(CHS, chordArgs("xyz", 3, 2)));
    EXPECT_FALSE(out.at("recognised").toBool());
    EXPECT_TRUE(out.count("messages"));
    EXPECT_EQ(outStr("name"), "xyz");
    ASSERT_TRUE(run(CHS, chordArgs("B\u266d7", 4, 2)));
    EXPECT_EQ(outStr("name"), "Bb7");
    EXPECT_TRUE(out.at("recognised").toBool());
    ASSERT_TRUE(run(CHS, chordArgs("N.C.", 5, 2)));
    EXPECT_TRUE(out.at("recognised").toBool());
}

TEST_F(Agent_Text, ChordSymbolRomanBesideStandard)
{
    Segment* seg = crAt(score.get(), 1, 1)->segment();
    ASSERT_TRUE(run(CHS, with(chordArgs("I"), "notation", Val("roman"))));
    const std::vector<Harmony*> hs = harmoniesAt(seg, 0);
    ASSERT_EQ(hs.size(), 2u) << "a roman numeral is a different slot from the chord symbol (findHarmonyInSegment)";
    refused(CHS, with(chordArgs("V"), "notation", Val("roman")), AErr::Exists);
}

TEST_F(Agent_Text, ChordSymbolReachesLinkedParts)
{
    open("parts-linked.mscz");
    auto count = [](const Score* s) { return countAnnotations(s, ElementType::HARMONY); };
    const std::vector<int> before = perPart(score.get(), count);
    Segment* seg = crAt(score.get(), 1, 1)->segment();
    addsOneEntry(CHS, chordArgs("F"),
                 [&] {
        const std::vector<Harmony*> hs = harmoniesAt(seg, 0);
        ASSERT_EQ(hs.size(), 1u);
        EXPECT_EQ(hs.front()->linkList().size(), 3u) << "the master, the Flute part and the Flute and Clarinet part";
        EXPECT_EQ(sum(perPart(score.get(), count)), sum(before) + 2);
    },
                 [&] { EXPECT_EQ(perPart(score.get(), count), before); });
}

TEST_F(Agent_Text, ChordSymbolInFretDiagramNotReplaced)
{
    adopt(loadPath(fs::path(agent_tests_DATA_ROOT) / "../../engraving/tests/chordsymbol_data/add-to-fret.mscz"));
    Segment* seg = score->firstMeasure()->findFirstR(SegmentType::ChordRest, Fraction(0, 1));
    ASSERT_TRUE(seg);
    FretDiagram* fd = toFretDiagram(seg->findAnnotation(ElementType::FRET_DIAGRAM, 0, 0));
    ASSERT_TRUE(fd);
    setup([&] { score->addText(TextStyleType::HARMONY_A, fd); });
    ASSERT_TRUE(fd->harmony());
    refused(CHS, chordArgs("G"), AErr::Exists, "fret diagram");
    refused(CHS, with(chordArgs("G"), "if_exists", Val("replace")), AErr::Exists, "fret diagram");
}

// ================================================================== add_lyric

TEST_F(Agent_Text, LyricArgumentValidation)
{
    const Params base = lyricArgs({ syl("love") });
    refused(LYR, without(base, "syllables"), AErr::InvalidArg, "syllables must");
    refused(LYR, with(base, "syllables", Val(ValList())), AErr::InvalidArg, "syllables must");
    refused(LYR, with(base, "syllables", Val("love")), AErr::InvalidArg, "syllables must");
    refused(LYR, with(base, "syllables", syls({ Val(3) })), AErr::InvalidArg, "must be an object");
    ValMap extra;
    extra["text"] = Val("x");
    extra["hyphen"] = Val(true);
    refused(LYR, with(base, "syllables", syls({ Val(extra) })), AErr::InvalidArg, "unknown key");
    ValMap noText;
    noText["syllabic"] = Val("single");
    refused(LYR, with(base, "syllables", syls({ Val(noText) })), AErr::InvalidArg, "has no text");
    refused(LYR, with(base, "syllables", syls({ syl("  ") })), AErr::InvalidArg, "has no text");
    refused(LYR, with(base, "syllables", syls({ syl("Sing-") })), AErr::InvalidArg, "ends in");
    refused(LYR, with(base, "syllables", syls({ syl("song_") })), AErr::InvalidArg, "ends in");
    refused(LYR, with(base, "syllables", syls({ syl("x", "first") })), AErr::InvalidArg, "syllabic must");
    refused(LYR, with(base, "syllables", syls({ syl("x", "", Val(3)) })), AErr::InvalidArg, "melisma_to must");
    refused(LYR, with(base, "syllables", syls({ syl("x", "", Val("long")) })), AErr::InvalidArg, "melisma_to must");
    refused(LYR, with(base, "syllables", syls({ syl("Glo", "begin", Val("self")) })), AErr::InvalidArg, "ends its word");
    refused(LYR, with(base, "syllables", syls({ syl("Sing", "begin"), syl("x") })), AErr::InvalidArg, "must be middle or end");
    refused(LYR, with(base, "syllables", syls({ syl("a"), syl("ing", "end") })), AErr::InvalidArg, "must be single or begin");
    refused(LYR, with(base, "verse", Val(0)), AErr::InvalidArg, "verse must");
    refused(LYR, with(base, "verse", Val(1.5)), AErr::InvalidArg, "verse must");
    refused(LYR, with(base, "placement", Val("left")), AErr::InvalidArg, "placement must");
    refused(LYR, with(base, "if_exists", Val("append")), AErr::InvalidArg, "if_exists must");
    refused(LYR, without(base, "staff"), AErr::InvalidArg, "missing staff");
    refused(LYR, with(base, "at", posBeat(1, 2.5)), AErr::Range, "no note or rest starts");
}

TEST_F(Agent_Text, LyricPlacementValidation)
{
    // bar 10: a half rest, then C5 for the second half; the last note of the score
    ASSERT_TRUE(crAt(score.get(), 10, 1) && crAt(score.get(), 10, 1)->isRest());
    refused(LYR, lyricArgs({ syl("a"), syl("men") }, 10, 3), AErr::Range, "has no note to go on");
    refused(LYR, lyricArgs({ syl("oh", "", Val("self")) }, 10, 1), AErr::InvalidArg, "on a rest");
    refused(LYR, lyricArgs({ syl("oh", "", posBeat(10, 1)) }, 9, 1), AErr::InvalidArg, "is a rest");
    refused(LYR, lyricArgs({ syl("a", "", posBeat(2, 1)) }, 3, 1), AErr::InvalidArg, "is not after");
    refused(LYR, lyricArgs({ syl("a", "", posBeat(1, 1)) }, 1, 1), AErr::InvalidArg, "is not after");
    refused(LYR, lyricArgs({ syl("a", "", posBeat(1, 1.5)) }, 1, 1), AErr::InvalidArg, "melisma_to");
    {
        const Ret r = run(LYR, lyricArgs({ syl("a", "", posBeat(1, 1.5)) }, 1, 1));
        EXPECT_EQ(r.text().find("E_RANGE"), std::string::npos) << "one code per message: " << r.text();
    }
    refused(LYR, lyricArgs({ syl("a", "", posBeat(99, 1)) }, 1, 1), AErr::Range, "outside the score");
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("in") }, 1, 3)));
    refused(LYR, lyricArgs({ syl("a", "", posBeat(1, 4)) }, 1, 1), AErr::InvalidArg, "run over the lyric");
    refused(LYR, lyricArgs({ syl("x") }, 1, 3), AErr::Exists, "already has the verse 1 lyric");
    // a sequence whose second syllable lands on the existing one
    refused(LYR, lyricArgs({ syl("a"), syl("b") }, 1, 2), AErr::Exists, "already has");
}

TEST_F(Agent_Text, LyricS10SequenceWithHyphensAndMelisma)
{
    // tests/agent/scenarios/S10_lyrics_melismas.json, step 1
    Params p = lyricArgs({ syl("Sing", "begin"), syl("ing", "end"), syl("a"), syl("song", "", posBeat(2, 2)), syl("of"),
                           syl("joy") });
    p["staff"] = staffPart("Piano");
    p["verse"] = Val(1);
    addsOneEntry(LYR, p,
                 [&] {
        auto ly = [&](int m, double b) { ChordRest* cr = crAt(score.get(), m, b); return cr ? cr->lyrics(0, PlacementV::BELOW) : nullptr; };
        ASSERT_TRUE(ly(1, 1) && ly(1, 2) && ly(1, 3) && ly(1, 4) && ly(2, 3) && ly(2, 4));
        EXPECT_EQ(ly(1, 1)->syllabic(), LyricsSyllabic::BEGIN);
        EXPECT_EQ(ly(1, 2)->syllabic(), LyricsSyllabic::END);
        EXPECT_EQ(ly(1, 3)->syllabic(), LyricsSyllabic::SINGLE);
        EXPECT_EQ(ly(1, 4)->ticks(), tickAt(score.get(), 2, 2) - tickAt(score.get(), 1, 4));
        EXPECT_FALSE(ly(2, 1)) << "under the extender";
        EXPECT_FALSE(ly(2, 2)) << "under the extender";
        EXPECT_EQ(ly(2, 3)->plainText(), u"of");
        EXPECT_EQ(ly(2, 4)->plainText(), u"joy");
    },
                 [&] { EXPECT_EQ(countLyrics(score.get()), 0); });
    EXPECT_EQ(out.at("handles").toList().size(), 6u);
    EXPECT_TRUE(outHas("notation", "m1 v1 | E5:q+ly\"Sing-\" G5:q+ly\"ing\" A5:q+ly\"a\" G5:q+ly\"song_\"")) << outStr("notation");
    EXPECT_TRUE(outHas("notation", "m2 v1 | F5:q A5:q G5:q+ly\"of\" F5:q+ly\"joy\"")) << outStr("notation");
}

TEST_F(Agent_Text, LyricSelfMelismaVerse2AndPlacement)
{
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("Ah", "", Val("self")) }, 1, 1)));
    Lyrics* l = crAt(score.get(), 1, 1)->lyrics(0, PlacementV::BELOW);
    ASSERT_TRUE(l);
    EXPECT_EQ(l->ticks(), Lyrics::TEMP_MELISMA_TICKS);
    ASSERT_TRUE(run(LYR, with(lyricArgs({ syl("Twas") }, 1, 1), "verse", Val(2))));
    Lyrics* l2 = crAt(score.get(), 1, 1)->lyrics(1, PlacementV::BELOW);
    ASSERT_TRUE(l2);
    EXPECT_EQ(l2->textStyleType(), TextStyleType::LYRICS_EVEN) << "Pid::VERSE picks the even style (lyrics.cpp:454-463)";
    EXPECT_TRUE(outHas("notation", "+ly2\"Twas\"")) << outStr("notation");
    ASSERT_TRUE(run(LYR, with(lyricArgs({ syl("up") }, 1, 1), "placement", Val("above"))));
    Lyrics* above = crAt(score.get(), 1, 1)->lyrics(0, PlacementV::ABOVE);
    ASSERT_TRUE(above);
    EXPECT_EQ(above->plainText(), u"up");
    EXPECT_EQ(crAt(score.get(), 1, 1)->lyrics().size(), 3u);
}

TEST_F(Agent_Text, LyricReplace)
{
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("love") }, 1, 1)));
    ChordRest* cr = crAt(score.get(), 1, 1);
    addsOneEntry(LYR, with(lyricArgs({ syl("hope", "", Val("self")) }, 1, 1), "if_exists", Val("replace")),
                 [&] {
        ASSERT_EQ(cr->lyrics().size(), 1u);
        EXPECT_EQ(cr->lyrics().front()->plainText(), u"hope");
        EXPECT_EQ(cr->lyrics().front()->ticks(), Lyrics::TEMP_MELISMA_TICKS);
    },
                 [&] {
        ASSERT_EQ(cr->lyrics().size(), 1u);
        EXPECT_EQ(cr->lyrics().front()->plainText(), u"love");
        EXPECT_TRUE(cr->lyrics().front()->ticks().isZero());
    });
    EXPECT_EQ(out.at("statuses").toList().front().toString(), "replaced");
}

TEST_F(Agent_Text, LyricNeighboursRepairedAsTheUIDoes)
{
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("love") }, 1, 1)));
    Lyrics* first = crAt(score.get(), 1, 1)->lyrics(0, PlacementV::BELOW);
    ASSERT_TRUE(first);
    // "-ly" joined to it: the previous syllable becomes begin (navigateToNextSyllable)
    addsOneEntry(LYR, lyricArgs({ syl("ly", "end") }, 1, 2),
                 [&] { EXPECT_EQ(first->syllabic(), LyricsSyllabic::BEGIN); },
                 [&] { EXPECT_EQ(first->syllabic(), LyricsSyllabic::SINGLE); });
    ASSERT_TRUE(out.count("adjusted"));
    EXPECT_NE(out.at("adjusted").toList().front().toString().find("single -> begin"), std::string::npos);
    // a begin syllable before an existing single word: that word becomes its end (navigateToNextSyllable)
    open("chord-symbols.mscz");
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("go") }, 1, 3)));
    Lyrics* next = crAt(score.get(), 1, 3)->lyrics(0, PlacementV::BELOW);
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("Al", "begin") }, 1, 2)));
    EXPECT_EQ(next->syllabic(), LyricsSyllabic::END);
    // a single word after a begin: the begin becomes single (navigateToLyrics, space)
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("Hey", "begin") }, 2, 1)));
    Lyrics* hey = crAt(score.get(), 2, 1)->lyrics(0, PlacementV::BELOW);
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("you") }, 2, 2)));
    EXPECT_EQ(hey->syllabic(), LyricsSyllabic::SINGLE);
}

TEST_F(Agent_Text, LyricOnRestAndGrace)
{
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("(hum)") }, 10, 1)));
    EXPECT_EQ(crAt(score.get(), 10, 1)->lyrics().size(), 1u) << "lyrics on rests are allowed (edit.cpp:932-933)";
    open("grace-notes.mscz");
    ChordRest* main = find(score.get(), 0, [](ChordRest* c) { return c->isChord() && !toChord(c)->graceNotes().empty(); });
    ASSERT_TRUE(main);
    Params p;
    p["target"] = Val(makeHandle(toChord(main)->graceNotes().front()));
    p["syllables"] = syls({ syl("la") });
    ASSERT_TRUE(run(LYR, p));
    EXPECT_EQ(main->lyrics().size(), 1u) << "a grace note's lyric goes on its main chord (edit.cpp:925-929)";
}

TEST_F(Agent_Text, LyricReachesLinkedParts)
{
    open("parts-linked.mscz");
    ChordRest* cr = find(score.get(), 0, [](ChordRest* c) { return c->isChord() && c->tick().isNotZero(); });
    ASSERT_TRUE(cr);
    const std::vector<int> before = perPart(score.get(), countLyrics);
    Params p = at(score.get(), cr);
    p["syllables"] = syls({ syl("Hal", "begin"), syl("le", "end") });
    addsOneEntry(LYR, p,
                 [&] {
        ASSERT_EQ(cr->lyrics().size(), 1u);
        EXPECT_EQ(cr->lyrics().front()->linkList().size(), 3u);
        EXPECT_EQ(sum(perPart(score.get(), countLyrics)), sum(before) + 4) << "two syllables in two parts";
    },
                 [&] { EXPECT_EQ(perPart(score.get(), countLyrics), before); });
}

TEST_F(Agent_Text, LyricRefusedWhenALinkedPartHasNoNoteThere)
{
    open("parts-linked.mscz");
    ChordRest* cr = find(score.get(), 0, [](ChordRest* c) { return c->isChord() && c->tick().isNotZero(); });
    ASSERT_TRUE(cr);
    // break one part on purpose: take the linked chord at that tick out of its segment
    Staff* linked = nullptr;
    for (Staff* st : cr->staff()->staffList()) {
        if (st->score() != score.get()) {
            linked = st;
            break;
        }
    }
    ASSERT_TRUE(linked) << "the flute staff is linked to a part";
    const track_idx_t lt = linked->idx() * VOICES;
    Segment* ps = linked->score()->tick2measure(cr->tick())->findSegment(SegmentType::ChordRest, cr->tick());
    ASSERT_TRUE(ps && ps->element(lt));
    ps->remove(ps->element(lt));
    Params p = at(score.get(), cr);
    p["syllables"] = syls({ syl("la") });
    refused(LYR, p, AErr::Refused, "linked part has no note");
}

// ================================================================== additions from the text review

TEST_F(Agent_Text, TempoMarkSpelledAsUpdateTempoReadsIt)
{
    // TempoText::updateTempo turns the almost-equal sign (U+2248), U+2252 and "~" into "=" and drops "c." / "ca." / "approx."
    // before matching (tempotext.cpp:262-270), so these are metronome marks too
    const std::vector<std::pair<std::string, double> > marks = {
        { "<sym>metNoteQuarterUp</sym> \u2248 120", 120.0 },
        { "<sym>metNoteQuarterUp</sym> = c. 120", 120.0 },
        { "<sym>metNoteQuarterUp</sym> ~ 108", 108.0 },
        { "Allegro (<sym>metNoteQuarterUp</sym> = ca. 132)", 132.0 },
    };
    int measure = 1;
    for (const auto& [xml, bpm] : marks) {
        Params p = without(textArgs("tempo", "", 1, measure, 1), "text");
        p["xml"] = Val(xml);
        refused(TXT, with(p, "tempo", tempoArg(100)), AErr::InvalidArg, "already has a metronome mark");
        ASSERT_TRUE(run(TXT, p)) << xml;
        EXPECT_NEAR(out.at("bpm").toDouble(), bpm, 1e-6) << xml;
        EXPECT_TRUE(out.at("followText").toBool()) << xml;
        ++measure;
    }
    // every note pattern of updateTempo counts, and a relative mark ("<note> = <note>") too
    for (const char* xml : { "<sym>metNoteQuarterUp</sym> <sym>metNoteQuarterUp</sym> = 120",
                             "<sym>metNoteQuarterUp</sym> = <sym>metNote8thUp</sym>" }) {
        Params p = without(textArgs("tempo", "", 1, measure, 1), "text");
        p["xml"] = Val(std::string(xml));
        refused(TXT, with(p, "tempo", tempoArg(100)), AErr::InvalidArg, "already has a metronome mark");
    }
    // a dotted 64th is no pattern of updateTempo (it reads nothing), so bpm is allowed
    {
        Params p = without(textArgs("tempo", "", 1, measure, 1), "text");
        p["xml"] = Val("<sym>metNote64thUp</sym><sym>metAugmentationDot</sym> = 100");
        ASSERT_TRUE(run(TXT, with(p, "tempo", tempoArg(90))));
        EXPECT_NEAR(out.at("bpm").toDouble(), 90.0, 1e-6);
        ++measure;
    }
    // a note symbol with no number after it is not a mark: bpm is allowed and plays
    Params words = without(textArgs("tempo", "", 1, measure, 1), "text");
    words["xml"] = Val("<sym>metNoteQuarterUp</sym> swing");
    ASSERT_TRUE(run(TXT, with(words, "tempo", tempoArg(150))));
    EXPECT_NEAR(out.at("bpm").toDouble(), 150.0, 1e-6);
    EXPECT_FALSE(out.at("followText").toBool());
}

TEST_F(Agent_Text, SystemTextFallsBackToAnotherVoice)
{
    // voices-four bar 2 beat 4.5: only voice 2 starts there (voice 1 holds a half note from beat 3)
    open("voices-four.mscz");
    ASSERT_FALSE(crAt(score.get(), 2, 4.5, 0));
    ChordRest* v2 = crAt(score.get(), 2, 4.5, 1);
    ASSERT_TRUE(v2);
    refused(TXT, textArgs("staff", "solo", 1, 2, 4.5), AErr::Range, "voice 1");
    refused(TXT, with(textArgs("system", "Tutti", 1, 2, 4.5), "voice", Val(1)), AErr::Range, "voice 1");
    addsOneEntry(TXT, textArgs("system", "Tutti", 1, 2, 4.5),
                 [&] {
        EngravingItem* t = annotation(v2->segment(), ElementType::SYSTEM_TEXT, 0);
        ASSERT_TRUE(t) << "anchored on voice 2's note, stored on the top staff's voice 1";
        EXPECT_EQ(toTextBase(t)->xmlText(), u"Tutti");
    },
                 [&] { EXPECT_FALSE(annotation(v2->segment(), ElementType::SYSTEM_TEXT, 0)); });
    EXPECT_EQ(out.at("voice").toInt(), 1);
}

TEST_F(Agent_Text, ExpressionPreferenceFollowsTheConfiguration)
{
    // "preference" is the only path that reads the app setting; the unit-test configuration is a
    // NiceMock registered by environment.cpp, so it can be switched both ways here
    auto cfg = muse::modularity::globalIoc()->resolve<IEngravingConfiguration>("utests");
    auto* mock = dynamic_cast<EngravingConfigurationMock*>(cfg.get());
    ASSERT_TRUE(mock);
    // put the setting back however this test ends, so later tests see the shared mock unchanged
    struct Restore {
        EngravingConfigurationMock* m;
        bool was;
        ~Restore() { ON_CALL(*m, dynamicsApplyToAllVoices()).WillByDefault(::testing::Return(was)); }
    } restore { mock, mock->dynamicsApplyToAllVoices() };
    open("voices-four.mscz");
    ChordRest* v2 = find(score.get(), 1, [](ChordRest* c) { return c->isChord(); });
    ASSERT_TRUE(v2);
    const Params base = with(with(at(score.get(), v2), "kind", Val("expression")), "voice_assignment", Val("preference"));

    ON_CALL(*mock, dynamicsApplyToAllVoices()).WillByDefault(::testing::Return(true));
    ASSERT_TRUE(run(TXT, with(base, "text", Val("dolce"))));
    EXPECT_EQ(outStr("voiceAssignment"), "all_voices");
    EXPECT_TRUE(annotation(v2->segment(), ElementType::EXPRESSION, 0));

    ON_CALL(*mock, dynamicsApplyToAllVoices()).WillByDefault(::testing::Return(false));
    ASSERT_TRUE(run(TXT, with(base, "text", Val("espress."))));
    EXPECT_EQ(outStr("voiceAssignment"), "this_voice");
    EXPECT_TRUE(annotation(v2->segment(), ElementType::EXPRESSION, 1));

    // the default never asks: all_voices with the setting off
    ASSERT_TRUE(run(TXT, with(without(base, "voice_assignment"), "text", Val("cantabile"))));
    EXPECT_EQ(outStr("voiceAssignment"), "all_voices");
}

TEST_F(Agent_Text, ChordSymbolTransposedInAPartWithOtherConcertPitch)
{
    // parts-linked: the Bb clarinet (staff 2) has its own part. Show that part in concert pitch
    // (fixture set-up, not under test); the master stays at written pitch, so a written C in the
    // master must read Bb in that part (edit.cpp:7174-7187) and C in the Flute and Clarinet part
    open("parts-linked.mscz");
    Score* clarinetPart = nullptr;
    for (Excerpt* ex : score->excerpts()) {
        if (ex->excerptScore() && ex->name().contains(u"Clarinet") && !ex->name().contains(u"Flute")) {
            clarinetPart = ex->excerptScore();
        }
    }
    ASSERT_TRUE(clarinetPart);
    ASSERT_FALSE(score->style().styleB(Sid::concertPitch));
    clarinetPart->style().set(Sid::concertPitch, true);
    const Interval iv = score->staff(1)->transpose(Fraction(0, 1));
    ASSERT_FALSE(iv.isZero()) << "the clarinet transposes";

    Segment* seg = crAt(score.get(), 1, 1, VOICES)->segment();
    auto copies = [&]() {
        std::vector<Harmony*> v;
        const std::vector<Harmony*> hs = harmoniesAt(seg, VOICES);
        if (hs.size() == 1) {
            for (EngravingObject* o : hs.front()->linkList()) {
                v.push_back(toHarmony(o));
            }
        }
        return v;
    };
    auto check = [&]() {
        const std::vector<Harmony*> hs = harmoniesAt(seg, VOICES);
        ASSERT_EQ(hs.size(), 1u);
        const int written = hs.front()->rootTpc();
        ASSERT_TRUE(tpcIsValid(written));
        const std::vector<Harmony*> all = copies();
        ASSERT_EQ(all.size(), 3u) << "the master, the Clarinet part and the Flute and Clarinet part";
        for (Harmony* h : all) {
            if (h->score() == clarinetPart) {
                EXPECT_EQ(h->rootTpc(), Transpose::transposeTpc(written, iv, true)) << "concert pitch in the part";
                EXPECT_EQ(h->rootTpc(), static_cast<int>(Tpc::TPC_B_B));
            } else {
                EXPECT_EQ(h->rootTpc(), written) << "same concert-pitch setting as the master";
            }
        }
    };
    Params p = chordArgs("C", 1, 1, 2);
    addsOneEntry(CHS, p, check, [&] { EXPECT_TRUE(harmoniesAt(seg, VOICES).empty()); });
    // replace keeps the transposition right too
    addsOneEntry(CHS, with(with(p, "text", Val("F7")), "if_exists", Val("replace")),
                 [&] {
        const std::vector<Harmony*> all = copies();
        ASSERT_EQ(all.size(), 3u);
        for (Harmony* h : all) {
            EXPECT_EQ(h->harmonyName(), h->score() == clarinetPart ? u"Eb7" : u"F7");
        }
    },
                 [&] { check(); });
}

TEST_F(Agent_Text, LyricsInTheMelismaFixture)
{
    open("lyrics-melismas-verses.mscz");
    // verse 1 "light" (bar 2 beat 3, a half note) holds an extender to bar 3 beat 2
    Lyrics* light = crAt(score.get(), 2, 3)->lyrics(0, PlacementV::BELOW);
    ASSERT_TRUE(light);
    ASSERT_EQ(plainOf(light), "light");
    const Fraction lightTicks = light->ticks();
    ASSERT_EQ(lightTicks, Fraction(3, 4));

    // verse 3 stops after bar 2; carrying it on into bar 3 leaves verse 1's extender alone
    // (adjustPrevious only looks at the same verse and placement, lyrics.cpp:300-318)
    ASSERT_TRUE(run(LYR, with(lyricArgs({ syl("and"), syl("far") }, 3, 1), "verse", Val(3))));
    EXPECT_EQ(light->ticks(), lightTicks);
    EXPECT_FALSE(out.count("adjusted"));
    ASSERT_TRUE(crAt(score.get(), 3, 2)->lyrics(2, PlacementV::BELOW));

    // a verse-1 syllable under the extender: no chord is left for it, so it goes. adjustPrevious sets
    // eps (lyrics.cpp:310-315) and layout drops that to 0 without undo (lyrics.cpp:588-602), as in
    // the UI; the command makes the 0 part of its own undoable edit
    addsOneEntry(LYR, lyricArgs({ syl("oh") }, 3, 1),
                 [&] { EXPECT_EQ(light->ticks(), Fraction(0, 1)); },
                 [&] { EXPECT_EQ(light->ticks(), lightTicks); });
    ASSERT_TRUE(out.count("adjusted"));
    EXPECT_NE(out.at("adjusted").toList().front().toString().find("extender removed"), std::string::npos)
        << out.at("adjusted").toList().front().toString();

    // replace the middle of "won-der-ful" (bar 4) with a single word: both neighbours are repaired
    Lyrics* won = crAt(score.get(), 4, 1)->lyrics(0, PlacementV::BELOW);
    Lyrics* ful = crAt(score.get(), 4, 3)->lyrics(0, PlacementV::BELOW);
    ASSERT_TRUE(won && ful);
    ASSERT_EQ(won->syllabic(), LyricsSyllabic::BEGIN);
    ASSERT_EQ(ful->syllabic(), LyricsSyllabic::END);
    addsOneEntry(LYR, with(lyricArgs({ syl("dah") }, 4, 2), "if_exists", Val("replace")),
                 [&] {
        EXPECT_EQ(won->syllabic(), LyricsSyllabic::SINGLE);
        EXPECT_EQ(ful->syllabic(), LyricsSyllabic::SINGLE);
        EXPECT_EQ(plainOf(crAt(score.get(), 4, 2)->lyrics(0, PlacementV::BELOW)), "dah");
    },
                 [&] {
        EXPECT_EQ(won->syllabic(), LyricsSyllabic::BEGIN);
        EXPECT_EQ(ful->syllabic(), LyricsSyllabic::END);
        EXPECT_EQ(plainOf(crAt(score.get(), 4, 2)->lyrics(0, PlacementV::BELOW)), "der");
    });
    EXPECT_EQ(out.at("adjusted").toList().size(), 2u);
    // replacing it with a middle syllable joins both neighbours into the word again
    ASSERT_TRUE(run(LYR, with(lyricArgs({ syl("dah", "middle") }, 4, 2), "if_exists", Val("replace"))));
    EXPECT_EQ(won->syllabic(), LyricsSyllabic::BEGIN);
    EXPECT_EQ(ful->syllabic(), LyricsSyllabic::END);
    EXPECT_EQ(out.at("adjusted").toList().size(), 2u);
}

// score_read's tempo mark: the words before the metronome mark, without the note symbol, whatever
// spelling of "=" updateTempo accepts
TEST_F(Agent_Text, TempoMarkPrintsTheWordsOnly)
{
    const std::vector<std::pair<std::string, std::string> > cases = {
        { "<sym>metNoteQuarterUp</sym> = 132", "+tempo(q=132)" },
        { "Allegro <sym>metNoteQuarterUp</sym> = 120", "+tempo\"Allegro\"(q=120)" },
        { "Allegro <sym>metNoteQuarterUp</sym>=120", "+tempo\"Allegro\"(q=120)" },
        { "Allegro (<sym>metNoteQuarterUp</sym> = ca. 132)", "+tempo\"Allegro\"(q=132)" },
        { "<sym>metNoteQuarterUp</sym> \u2248 108", "+tempo(q=108)" },
    };
    int measure = 1;
    for (const auto& [xml, printed] : cases) {
        Params p = without(textArgs("tempo", "", 1, measure, 1), "text");
        p["xml"] = Val(xml);
        ASSERT_TRUE(run(TXT, p)) << xml;
        EXPECT_TRUE(outHas("notation", printed)) << xml << " -> " << outStr("notation");
        ++measure;
    }
}

// TX05: a one-chord extender survives undo of the edit that removed it (joining the next syllable
// with a hyphen). Upstream layout used to drop it to 0 without undo (AGENT: fix in lyrics.cpp)
TEST_F(Agent_Text, OneChordExtenderSurvivesUndo)
{
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("Ah", "", Val("self")) }, 3, 1)));
    Lyrics* ah = crAt(score.get(), 3, 1)->lyrics(0, PlacementV::BELOW);
    ASSERT_TRUE(ah);
    ASSERT_EQ(ah->ticks(), Lyrics::TEMP_MELISMA_TICKS);
    const std::string saved = snapshot();
    ASSERT_TRUE(run(LYR, lyricArgs({ syl("men", "end") }, 3, 2)));
    EXPECT_EQ(ah->ticks(), Fraction(0, 1));
    score->undoRedo(true, nullptr);
    EXPECT_EQ(ah->ticks(), Lyrics::TEMP_MELISMA_TICKS);
    EXPECT_EQ(snapshot(), saved);
    score->undoRedo(false, nullptr);
    score->undoRedo(true, nullptr);
    EXPECT_EQ(ah->ticks(), Lyrics::TEMP_MELISMA_TICKS);
    EXPECT_EQ(snapshot(), saved);
}
