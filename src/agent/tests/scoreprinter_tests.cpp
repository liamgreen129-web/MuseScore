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
// G1-style checks on every fixture: print the score, re-enter the text through the parser,
// and compare pitch and rhythm with an independent walk of the engraving model.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "engraving/dom/chord.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/tests/utils/scorerw.h"

#include "notation/parser.h"
#include "notation/validate.h"
#include "notation/writer.h"
#include "read/scoreprinter.h"

using namespace mu::engraving;
using namespace mu::agent;
namespace nt = mu::agent::notation;
using nt::Rational;
using nt::Document;
using nt::ParseResult;
using nt::BarLengthFn;
using nt::Problem;
using nt::parse;
using nt::validate;
using nt::barLengthsFromDocument;

namespace {
namespace fs = std::filesystem;

// One sounding event: position in whole notes from the start of the score, length, pitches.
struct Sound {
    Rational at;
    Rational len;
    std::vector<int> midi; // empty for rests
    bool operator==(const Sound& o) const { return at == o.at && len == o.len && midi == o.midi; }
};

std::string show(const Sound& s)
{
    std::string m;
    for (int p : s.midi) {
        m += std::to_string(p) + " ";
    }
    return "@" + s.at.toString() + " len " + s.len.toString() + " [" + m + "]";
}

Rational R(const Fraction& f) { return Rational(f.numerator(), f.denominator()); }

// The model side, walked as simply as possible (no tuplet structure, no notation types).
std::map<std::pair<int, int>, std::vector<Sound> > modelSounds(const Score* score)
{
    std::map<std::pair<int, int>, std::vector<Sound> > out;
    for (const Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (const Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
            for (track_idx_t t = 0; t < score->ntracks(); ++t) {
                const EngravingItem* el = s->element(t);
                if (!el || !el->isChordRest()) {
                    continue;
                }
                const ChordRest* cr = toChordRest(el);
                Sound snd { R(s->tick()), R(cr->actualTicks()), {} };
                if (cr->isChord()) {
                    for (const Note* n : toChord(cr)->notes()) {
                        snd.midi.push_back(n->pitch() + n->staff()->pitchOffset(s->tick()));
                    }
                    std::sort(snd.midi.begin(), snd.midi.end());
                } else if (toRest(cr)->isGap()) {
                    continue;
                }
                out[{ static_cast<int>(t / VOICES) + 1, static_cast<int>(t % VOICES) + 1 }].push_back(snd);
            }
        }
    }
    return out;
}

void collect(const std::vector<nt::Item>& items, Rational& pos, const Rational& scale, const Rational& barLen, std::vector<Sound>& out)
{
    for (const nt::Item& item : items) {
        if (const nt::Event* e = std::get_if<nt::Event>(&item)) {
            const Rational len = (e->kind == nt::EventKind::FullBarRest ? barLen : e->duration.value()) * scale;
            if (e->kind != nt::EventKind::Gap) {
                Sound s { pos, len, {} };
                for (const nt::NoteHead& h : e->heads) {
                    s.midi.push_back(h.pitch.midi());
                }
                out.push_back(s);
            }
            pos = pos + len;
        } else {
            const nt::Tuplet& t = std::get<nt::Tuplet>(item);
            collect(t.items, pos, scale * Rational(t.normal, t.actual), barLen, out);
        }
    }
}

// The notation side: positions rebuilt from the parsed text and the score's bar starts.
std::map<std::pair<int, int>, std::vector<Sound> > textSounds(const Document& doc, const std::vector<Rational>& barStart,
                                                               const BarLengthFn& lengths)
{
    std::map<std::pair<int, int>, std::vector<Sound> > out;
    int staff = 0;
    for (const nt::Line& line : doc.lines) {
        if (const nt::StaffHeader* h = std::get_if<nt::StaffHeader>(&line)) {
            staff = h->staff;
        } else if (const nt::VoiceLine* v = std::get_if<nt::VoiceLine>(&line)) {
            Rational pos = barStart.at(v->bar.index - 1);
            collect(v->items, pos, Rational(1), lengths(staff, v->bar.index).value_or(Rational(1)), out[{ staff, v->voice }]);
        }
    }
    return out;
}

std::vector<fs::path> fixtures()
{
    std::vector<fs::path> files;
    const fs::path root = fs::path(agent_tests_DATA_ROOT) / "../../../tests/fixtures";
    for (const char* sub : { "synthetic", "corpus" }) {
        const fs::path dir = root / sub;
        if (!fs::exists(dir)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".mscz") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}
}

TEST(Agent_ScorePrinter, SplitDuration)
{
    using read::splitDuration;
    EXPECT_EQ(splitDuration(Rational(3, 8)).size(), 1u);  // dotted quarter
    EXPECT_EQ(splitDuration(Rational(5, 8)).size(), 2u);  // half + eighth
    EXPECT_EQ(splitDuration(Rational(7, 8)).size(), 1u);  // double-dotted half
    EXPECT_EQ(splitDuration(Rational(0)).size(), 0u);
}

TEST(Agent_ScorePrinter, EveryFixtureRoundTripsPitchAndRhythm)
{
    const std::vector<fs::path> files = fixtures();
    if (files.empty()) {
        GTEST_SKIP() << "no fixtures: run tools/fixtures/make_corpus_fixtures.py";
    }

    for (const fs::path& file : files) {
        SCOPED_TRACE(file.filename().string());
        MasterScore* score = ScoreRW::readScore(muse::String::fromStdString(file.string()), true);
        ASSERT_TRUE(score);

        const auto t0 = std::chrono::steady_clock::now();
        const Document doc = read::printScore(score);
        const std::string text = nt::write(doc);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_LT(ms, 2000.0) << "whole-score read took " << ms << " ms";

        if (const char* dump = std::getenv("AGENT_DUMP_DIR")) { // for eyeballing: AGENT_DUMP_DIR=/tmp/x
            std::ofstream(fs::path(dump) / (file.stem().string() + ".txt")) << text;
        }

        const ParseResult parsed = parse(text);
        if (!parsed.ok()) {
            ADD_FAILURE() << "printed text does not parse: " << parsed.errors[0].toString();
            delete score;
            continue;
        }
        if (!(parsed.doc == doc)) {
            for (size_t i = 0; i < std::min(doc.lines.size(), parsed.doc.lines.size()); ++i) {
                if (!(doc.lines[i] == parsed.doc.lines[i])) {
                    nt::Document a, b;
                    a.lines = { doc.lines[i] };
                    b.lines = { parsed.doc.lines[i] };
                    ADD_FAILURE() << "line " << i + 1 << " differs after parsing:\n printed: " << nt::write(a) << " parsed:  " << nt::write(b);
                    break;
                }
            }
        }
        EXPECT_EQ(nt::write(parsed.doc), text);

        const BarLengthFn lengths = barLengthsFromDocument(parsed.doc);
        int shown = 0;
        for (const Problem& p : validate(parsed.doc, lengths)) {
            std::string context;
            const std::string bar = p.message.substr(0, p.message.find(' ')) + " ";
            if (shown++ < 3) { // the bar and the next one, so ties can be seen
                std::istringstream in(text);
                std::string line, nextBar = "m" + std::to_string(std::stoi(bar.substr(1)) + 1) + " ";
                while (std::getline(in, line)) {
                    if (line.rfind(bar, 0) == 0 || line.rfind(nextBar, 0) == 0
                        || line.rfind(bar.substr(0, bar.size() - 1) + "(", 0) == 0 || line.rfind(nextBar.substr(0, nextBar.size() - 1) + "(", 0) == 0) {
                        context += "\n    " + line;
                    }
                }
            }
            ADD_FAILURE() << p.code << " " << p.message << context;
        }

        std::vector<Rational> barStart;
        for (const Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
            barStart.push_back(R(m->tick()));
        }
        const auto model = modelSounds(score);
        const auto fromText = textSounds(parsed.doc, barStart, lengths);
        for (const auto& [key, sounds] : model) {
            auto it = fromText.find(key);
            if (it == fromText.end()) {
                ADD_FAILURE() << "staff " << key.first << " voice " << key.second << " missing from the text";
                continue;
            }
            EXPECT_EQ(it->second.size(), sounds.size()) << "staff " << key.first << " voice " << key.second;
            for (size_t i = 0; i < std::min(sounds.size(), it->second.size()); ++i) {
                if (it->second[i] == sounds[i]) {
                    continue;
                }
                ADD_FAILURE() << "staff " << key.first << " voice " << key.second << " event " << i
                              << ": text " << show(it->second[i]) << " model " << show(sounds[i]);
                break; // first mismatch per voice is enough
            }
        }
        delete score;
    }
}
