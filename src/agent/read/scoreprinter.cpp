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
#include "scoreprinter.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "engraving/dom/articulation.h"
#include "engraving/dom/barline.h"
#include "engraving/dom/breath.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/fermata.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/jump.h"
#include "engraving/dom/marker.h"
#include "engraving/dom/key.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/ottava.h"
#include "engraving/dom/part.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/rehearsalmark.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/textline.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/volta.h"
#include "engraving/types/symnames.h"
#include "engraving/types/typesconv.h"

#include "notation/writer.h"

using namespace mu::engraving;
using namespace mu::agent;
namespace nt = mu::agent::notation;
using read::ReadOptions;
using read::splitDuration;

namespace {
nt::Rational toRational(const Fraction& f)
{
    return nt::Rational(f.numerator(), f.denominator());
}

std::optional<nt::BaseDuration> toBase(DurationType t)
{
    switch (t) {
    case DurationType::V_BREVE: return nt::BaseDuration::Breve;
    case DurationType::V_WHOLE: return nt::BaseDuration::Whole;
    case DurationType::V_HALF: return nt::BaseDuration::Half;
    case DurationType::V_QUARTER: return nt::BaseDuration::Quarter;
    case DurationType::V_EIGHTH: return nt::BaseDuration::Eighth;
    case DurationType::V_16TH: return nt::BaseDuration::Sixteenth;
    case DurationType::V_32ND: return nt::BaseDuration::ThirtySecond;
    case DurationType::V_64TH: return nt::BaseDuration::SixtyFourth;
    case DurationType::V_128TH: return nt::BaseDuration::OneTwentyEighth;
    default: return std::nullopt;
    }
}

const char* KEY_MAJOR[] = { "Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#" };
const char* KEY_MINOR[] = { "ab", "eb", "bb", "f", "c", "g", "d", "a", "e", "b", "f#", "c#", "g#", "d#", "a#" };

std::string keyName(const KeySigEvent& ev, bool concert)
{
    if (ev.isAtonal() || ev.custom()) {
        return "none";
    }
    const int k = static_cast<int>(concert ? ev.concertKey() : ev.key());
    if (k < -7 || k > 7) {
        return "none";
    }
    return ev.mode() == KeyMode::MINOR ? KEY_MINOR[k + 7] : KEY_MAJOR[k + 7];
}

std::string timeName(const TimeSig* ts)
{
    if (!ts) {
        return {};
    }
    if (ts->timeSigType() == TimeSigType::FOUR_FOUR) {
        return "C";
    }
    if (ts->timeSigType() == TimeSigType::ALLA_BREVE) {
        return "C/";
    }
    return std::to_string(ts->sig().numerator()) + "/" + std::to_string(ts->sig().denominator());
}

std::string clefName(ClefType c)
{
    switch (c) {
    case ClefType::G: return "treble";
    case ClefType::G8_VB: return "treble8vb";
    case ClefType::G8_VA: return "treble8va";
    case ClefType::F: return "bass";
    case ClefType::F8_VB: return "bass8vb";
    case ClefType::C3: return "alto";
    case ClefType::C4: return "tenor";
    case ClefType::C1: return "soprano";
    case ClefType::C2: return "mezzo";
    case ClefType::PERC: return "perc";
    case ClefType::TAB: return "tab";
    default: return std::string(TConv::toXml(c).ascii());
    }
}

//! "-M2" style, from a staff transposition (concert = written + interval).
std::string intervalName(const Interval& iv)
{
    if (iv.isZero()) {
        return {};
    }
    const int chrom = std::abs(iv.chromatic);
    const int diat = std::abs(iv.diatonic);
    const int octaves = diat / 7;
    const int simpleDiat = diat % 7;
    const int simpleChrom = chrom - octaves * 12;
    static const int perfectish[] = { 0, 2, 4, 5, 7, 9, 11 }; // major/perfect semitones by diatonic step
    const bool perfectClass = simpleDiat == 0 || simpleDiat == 3 || simpleDiat == 4;
    const int delta = simpleChrom - perfectish[simpleDiat];
    std::string quality;
    if (perfectClass) {
        quality = delta == 0 ? "P" : delta > 0 ? "A" : "d";
    } else {
        quality = delta == 0 ? "M" : delta == -1 ? "m" : delta > 0 ? "A" : "d";
    }
    const std::string sign = (iv.chromatic < 0 || iv.diatonic < 0) ? "-" : "";
    return sign + quality + std::to_string(diat + 1);
}

std::string symName(SymId id)
{
    return std::string(SymNames::nameForSymId(id).ascii());
}

std::optional<std::string> articulationCode(SymId id)
{
    static const std::map<SymId, std::string> codes = {
        { SymId::articStaccatoAbove, "st" }, { SymId::articStaccatoBelow, "st" },
        { SymId::articStaccatissimoAbove, "sti" }, { SymId::articStaccatissimoBelow, "sti" },
        { SymId::articTenutoAbove, "ten" }, { SymId::articTenutoBelow, "ten" },
        { SymId::articTenutoStaccatoAbove, "tst" }, { SymId::articTenutoStaccatoBelow, "tst" },
        { SymId::articAccentAbove, "ac" }, { SymId::articAccentBelow, "ac" },
        { SymId::articMarcatoAbove, "mar" }, { SymId::articMarcatoBelow, "mar" },
        { SymId::stringsUpBow, "up" }, { SymId::stringsDownBow, "dn" },
        { SymId::stringsHarmonic, "harm" }, { SymId::brassMuteOpen, "open" }, { SymId::brassMuteClosed, "stop" },
        { SymId::ornamentTrill, "tr" }, { SymId::ornamentMordent, "mord" }, { SymId::ornamentShortTrill, "imord" },
        { SymId::ornamentTurn, "turn" }, { SymId::ornamentTurnInverted, "iturn" },
    };
    auto it = codes.find(id);
    return it == codes.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::string fermataArg(SymId id)
{
    switch (id) {
    case SymId::fermataShortAbove:
    case SymId::fermataShortBelow: return "short";
    case SymId::fermataLongAbove:
    case SymId::fermataLongBelow: return "long";
    case SymId::fermataVeryLongAbove:
    case SymId::fermataVeryLongBelow: return "vlong";
    default: return {};
    }
}

std::string plain(const String& s)
{
    return s.toStdString();
}

//! The text without SMuFL symbols (the private use area, U+E000-U+F8FF)
std::string withoutSymbols(const String& s)
{
    String out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char16_t c = s.at(i).unicode();
        if (c < 0xE000 || c > 0xF8FF) {
            out.append(s.at(i));
        }
    }
    return out.toStdString();
}

//! Marks gathered from spanners, keyed by the ChordRest they start/end on.
struct SpannerMarks {
    std::map<const ChordRest*, std::vector<nt::Mark> > start;
    std::map<const ChordRest*, std::vector<nt::Mark> > end;
    std::map<const ChordRest*, int> slurOpen;
    std::map<const ChordRest*, int> slurClose;
};

//! The chord/rest a line starts on when it has none of its own: a system line (system flag) is
//! anchored to a Segment (Spanner::doComputeStartElement, spanner.cpp:741), so startCR() is null.
//! Prefers the line's own voice, then any voice of its staff, starting at tick; else the event
//! sounding at tick on that staff.
const ChordRest* chordRestAtTickInStaff(const Score* score, const Fraction& tick, track_idx_t lineTrack)
{
    const staff_idx_t si = track2staff(lineTrack);
    const track_idx_t strack = si * VOICES;
    if (const Segment* seg = score->tick2segment(tick, true, SegmentType::ChordRest)) {
        if (const EngravingItem* e = seg->element(lineTrack); e && e->isChordRest()) {
            return toChordRest(e);
        }
        for (track_idx_t t = strack; t < strack + VOICES; ++t) {
            if (const EngravingItem* e = seg->element(t); e && e->isChordRest()) {
                return toChordRest(e);
            }
        }
    }
    const Measure* m = score->tick2measure(tick);
    if (!m) {
        return nullptr;
    }
    for (const Segment* seg = m->first(SegmentType::ChordRest); seg; seg = seg->next(SegmentType::ChordRest)) {
        for (track_idx_t t = strack; t < strack + VOICES; ++t) {
            const EngravingItem* e = seg->element(t);
            if (e && e->isChordRest()) {
                const ChordRest* cr = toChordRest(e);
                if (cr->tick() <= tick && tick < cr->tick() + cr->actualTicks()) {
                    return cr;
                }
            }
        }
    }
    return nullptr;
}

SpannerMarks collectSpanners(const Score* score)
{
    SpannerMarks out;
    for (const auto& [tick, sp] : score->spanner()) {
        Spanner* s = sp;
        const EngravingItem* se = s->startElement();
        const EngravingItem* ee = s->endElement();
        const ChordRest* scr = (se && se->isChordRest()) ? toChordRest(se) : s->startCR();
        if (!scr && !s->isSlur() && s->anchor() == Spanner::Anchor::SEGMENT) {
            scr = chordRestAtTickInStaff(score, s->tick(), s->track());
        }
        const ChordRest* ecr = (ee && ee->isChordRest()) ? toChordRest(ee) : s->endCR();
        if (!s->isSlur()) {
            // NOTATION.md: an end mark goes on the last event the line covers. A line's endElement
            // is the event starting at tick2 (Spanner::doComputeEndElement, spanner.cpp:791), one
            // too far, and missing when the line runs to the end of the score; Ottava finds its
            // end the way used here (ottava.cpp:464-466): the event ending at or before tick2.
            const staff_idx_t si = track2staff(s->track());
            const ChordRest* last = score->findChordRestEndingBeforeTickInStaffAndVoice(s->tick2(), si, track2voice(s->track()));
            if (!last || last->tick() < s->tick()) {
                last = score->findChordRestEndingBeforeTickInStaff(s->tick2(), si);
            }
            if (last && last->tick() >= s->tick()) {
                ecr = last;
            }
        }
        if (!scr || !ecr) {
            continue;
        }
        if (s->isSlur()) {
            if (scr->isGrace() || ecr->isGrace()) {
                continue; // grace slurs are not representable in the notation (detail only)
            }
            out.slurOpen[scr] += 1;
            out.slurClose[ecr] += 1;
        } else if (s->isHairpin()) {
            const Hairpin* h = toHairpin(s);
            nt::Mark m;
            const HairpinType ht = h->hairpinType();
            m.code = (ht == HairpinType::CRESC_HAIRPIN || ht == HairpinType::CRESC_LINE) ? "<" : ">";
            if (ht == HairpinType::CRESC_LINE || ht == HairpinType::DIM_LINE) {
                const std::string txt = plain(h->beginText());
                if (!txt.empty()) {
                    m.text = txt;
                }
            }
            out.start[scr].push_back(m);
            nt::Mark e;
            e.code = "!";
            out.end[ecr].push_back(e);
        } else {
            std::string code;
            std::optional<std::string> text;
            if (s->isOttava()) {
                switch (toOttava(s)->ottavaType()) {
                case OttavaType::OTTAVA_8VA: code = "8va"; break;
                case OttavaType::OTTAVA_8VB: code = "8vb"; break;
                case OttavaType::OTTAVA_15MA: code = "15ma"; break;
                case OttavaType::OTTAVA_15MB: code = "15mb"; break;
                default: code = "8va"; break; // 22ma/22mb: detail only (see NOTATION.md section 8)
                }
            } else if (s->isPedal()) {
                code = "ped";
            } else if (s->isTrill()) {
                code = "trl";
            } else if (s->isLetRing()) {
                code = "lr";
            } else if (s->isTextLine()) {
                code = "txt";
                text = plain(toTextLine(s)->beginText());
            } else {
                continue;
            }
            nt::Mark st;
            st.code = code;
            st.bracket = '[';
            st.text = (code == "txt") ? std::optional<std::string>(text.value_or(" ")) : std::nullopt;
            out.start[scr].push_back(st);
            nt::Mark en;
            en.code = code;
            en.bracket = ']';
            out.end[ecr].push_back(en);
        }
    }
    return out;
}

class Printer
{
public:
    Printer(const Score* score, const ReadOptions& opt)
        : m_score(score), m_opt(opt), m_spanners(collectSpanners(score)) {}

    nt::Document run()
    {
        nt::Document doc;
        std::vector<const Measure*> measures;
        for (const Measure* m = m_score->firstMeasure(); m; m = m->nextMeasure()) {
            measures.push_back(m);
        }
        if (measures.empty()) {
            return doc;
        }
        collectVoltas(measures);
        const int from = std::max(1, m_opt.fromBar);
        const int to = m_opt.toBar > 0 ? std::min<int>(m_opt.toBar, static_cast<int>(measures.size())) : static_cast<int>(measures.size());

        for (staff_idx_t si = 0; si < m_score->nstaves(); ++si) {
            const int staffNo = static_cast<int>(si) + 1;
            if (!m_opt.staves.empty() && std::find(m_opt.staves.begin(), m_opt.staves.end(), staffNo) == m_opt.staves.end()) {
                continue;
            }
            printStaff(doc, si, measures, from, to);
        }
        return doc;
    }

private:
    nt::Pitch spell(const Note* n, const Fraction& tick) const
    {
        const Staff* staff = n->staff();
        int pitch;
        int tpc;
        if (m_opt.writtenPitch) {
            pitch = n->pitch() - staff->transpose(tick).chromatic;
            tpc = n->tpc2();
        } else {
            pitch = n->pitch() + staff->pitchOffset(tick); // concert + ottava = what sounds
            tpc = n->tpc1();
        }
        nt::Pitch p;
        p.step = static_cast<char>(tpc2stepName(tpc).unicode());
        p.alter = static_cast<int>(tpc2alter(tpc));
        const int base = pitch - p.alter;
        p.octave = static_cast<int>(std::floor(base / 12.0)) - 1;
        return p;
    }

    nt::Duration durationOf(const ChordRest* cr) const
    {
        const TDuration d = cr->durationType();
        nt::Duration out;
        if (auto b = toBase(d.type())) {
            out.base = *b;
            out.dots = d.dots();
        }
        return out;
    }

    void annotationMarks(const ChordRest* cr, std::vector<nt::Mark>& marks) const
    {
        const Segment* seg = cr->segment();
        const track_idx_t track = cr->track();
        const bool firstStaffVoice1 = track == 0;
        for (const EngravingItem* a : seg->annotations()) {
            const bool sameTrack = a->track() == track;
            const bool systemItem = a->systemFlag() && a->track() == 0 && firstStaffVoice1;
            if (!sameTrack && !systemItem) {
                continue;
            }
            nt::Mark m;
            if (a->isFermata()) {
                m.code = "ferm";
                m.arg = fermataArg(toFermata(a)->symId());
            } else if (a->isDynamic()) {
                const DynamicType dt = toDynamic(a)->dynamicType();
                const std::string name(TConv::toXml(dt).ascii());
                static const std::set<std::string> known = { "pppp", "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff", "ffff",
                                                             "fp", "sf", "sfz", "sffz", "sfp", "rfz", "fz" };
                if (known.count(name)) {
                    m.code = name;
                } else {
                    m.code = "dyn";
                    m.text = plain(toDynamic(a)->plainText());
                }
            } else if (a->isTempoText()) {
                const TempoText* t = toTempoText(a);
                m.code = "tempo";
                // plainText() keeps a <sym> as its SMuFL code point (textbase.cpp:1727-1734): the
                // metronome's note. Without it a metronome-only marking reads " = 96", which is the
                // paren part, not text; "Allegro <note> = 120" keeps "Allegro"
                // The words are what comes before the mark's "=", after TempoText::updateTempo's own
                // clean-up (tempotext.cpp:265-273: approximately-equal signs and "~" read as "=",
                // "ca." / "c." / "approx." dropped): "Allegro (<note> = ca. 132)" keeps "Allegro"
                String clean = t->plainText();
                clean.replace(u"\u2252", u"=");
                clean.replace(u"\u2248", u"=");
                clean.replace(u"~", u"=");
                std::string txt = withoutSymbols(clean);
                const size_t eq = txt.find('=');
                std::string words = eq == std::string::npos ? txt : txt.substr(0, eq);
                words.erase(0, words.find_first_not_of(' '));
                words.erase(words.find_last_not_of(" (") + 1);
                if (!words.empty()) {
                    m.text = words;
                }
                const double bpm = t->tempo().val * 60.0;
                m.paren = "q=" + std::to_string(static_cast<int>(std::lround(bpm)));
            } else if (a->isStaffText()) {
                m.code = "";
                m.text = plain(toTextBase(a)->plainText());
            } else if (a->isSystemText()) {
                m.code = "sys";
                m.text = plain(toTextBase(a)->plainText());
            } else if (a->isExpression()) {
                m.code = "exp";
                m.text = plain(toTextBase(a)->plainText());
            } else {
                continue; // harmony and rehearsal marks are printed elsewhere; others are detail
            }
            marks.push_back(m);
        }
    }

    //! A tie is printed only when it ends on the same pitch in the very next event of the
    //! same voice (NOTATION.md section 7 rule 3). Imports can leave ties between different
    //! pitches, across voices or over a repeat; those become "% detail:" comments.
    bool tieLands(const Note* n, const ChordRest* cr) const
    {
        const Tie* tie = n->tieFor();
        if (!tie) {
            return false;
        }
        const Note* end = tie->endNote();
        std::string why;
        if (!end || !end->chord()) {
            why = "is not attached to an end note";
        } else if (end->pitch() != n->pitch()) {
            why = "ends on a different pitch";
        } else if (end->chord()->track() != cr->track()) {
            why = "ends in another voice or staff";
        } else if (end->chord()->tick() != cr->tick() + cr->actualTicks()) {
            why = "does not end on the next event";
        }
        if (why.empty()) {
            return true;
        }
        if (!cr->isGrace()) {
            m_details.push_back("detail: m" + std::to_string(m_barIndex) + " v" + std::to_string(cr->voice() + 1)
                                + ": the tie from " + nt::writePitch(spell(n, cr->tick())) + " " + why);
        }
        return false;
    }

    nt::Event makeEvent(const ChordRest* cr, bool withMarks) const
    {
        nt::Event e;
        e.duration = durationOf(cr);
        if (cr->isMeasureRepeat()) {
            // a MeasureRepeat is a Rest subclass with its own type: isRest() is false, and
            // toChord() on it reads past the object. The notation has no measure repeat yet.
            e.kind = nt::EventKind::FullBarRest;
            m_details.push_back("detail: m" + std::to_string(m_barIndex) + " v" + std::to_string(cr->voice() + 1)
                                + " is a measure repeat");
        } else if (cr->isRest()) {
            const Rest* r = toRest(cr);
            e.kind = r->isGap() ? nt::EventKind::Gap : (r->isFullMeasureRest() ? nt::EventKind::FullBarRest : nt::EventKind::Rest);
            if (e.kind == nt::EventKind::Gap && cr->voice() == 0) {
                e.kind = nt::EventKind::Rest; // NOTATION.md: voice 1 has no gaps
            }
        } else {
            const Chord* c = toChord(cr);
            std::vector<const Note*> notes(c->notes().begin(), c->notes().end());
            std::sort(notes.begin(), notes.end(), [](const Note* a, const Note* b) { return a->pitch() < b->pitch(); });
            e.kind = notes.size() > 1 ? nt::EventKind::Chord : nt::EventKind::Note;
            for (const Note* n : notes) {
                nt::NoteHead h;
                h.pitch = spell(n, c->tick());
                h.tie = tieLands(n, cr);
                e.heads.push_back(h);
            }
            if (withMarks) {
                for (const Articulation* a : c->articulations()) {
                    nt::Mark m;
                    if (auto code = articulationCode(a->symId())) {
                        m.code = *code;
                    } else {
                        m.code = "sym";
                        m.arg = symName(a->symId());
                    }
                    e.marks.push_back(m);
                }
            }
        }

        if (!withMarks) {
            return e;
        }

        annotationMarks(cr, e.marks);

        // breath after the event: a Breath segment at the event's end tick, same track
        if (const Measure* m = cr->measure()) {
            const Fraction endTick = cr->tick() + cr->actualTicks();
            for (const Segment* s = m->first(SegmentType::Breath); s; s = s->next(SegmentType::Breath)) {
                if (s->tick() != endTick) {
                    continue;
                }
                if (const EngravingItem* b = s->element(cr->track())) {
                    nt::Mark bm;
                    bm.code = toBreath(b)->isCaesura() ? "cae" : "br";
                    e.marks.push_back(bm);
                }
            }
        }

        if (auto it = m_spanners.start.find(cr); it != m_spanners.start.end()) {
            e.marks.insert(e.marks.end(), it->second.begin(), it->second.end());
        }
        if (auto it = m_spanners.end.find(cr); it != m_spanners.end.end()) {
            e.marks.insert(e.marks.end(), it->second.begin(), it->second.end());
        }

        for (const Lyrics* l : cr->lyrics()) {
            nt::Mark lm;
            lm.code = l->verse() == 0 ? "ly" : "ly" + std::to_string(l->verse() + 1);
            std::string syl = plain(l->plainText());
            if (l->syllabic() == LyricsSyllabic::BEGIN || l->syllabic() == LyricsSyllabic::MIDDLE) {
                syl += "-";
            } else if (l->ticks().isNotZero()) {
                syl += "_";
            }
            lm.text = syl;
            e.marks.push_back(lm);
        }

        if (auto it = m_spanners.slurOpen.find(cr); it != m_spanners.slurOpen.end()) {
            e.slurOpen = it->second;
        }
        if (auto it = m_spanners.slurClose.find(cr); it != m_spanners.slurClose.end()) {
            e.slurClose = it->second;
        }

        std::stable_sort(e.marks.begin(), e.marks.end(),
                         [](const nt::Mark& a, const nt::Mark& b) { return nt::markRank(a) < nt::markRank(b); });

        if (cr->isChord()) {
            const Chord* c = toChord(cr);
            for (const Chord* g : c->graceNotesBefore()) {
                if (e.graceBefore.empty()) {
                    e.graceBeforeKind = g->noteType() == NoteType::ACCIACCATURA ? nt::GraceKind::Acciaccatura : nt::GraceKind::Appoggiatura;
                }
                e.graceBefore.push_back(makeEvent(g, false));
            }
            for (const Chord* g : c->graceNotesAfter()) {
                e.graceAfter.push_back(makeEvent(g, false));
            }
        }
        return e;
    }

    //! Items of one voice in one bar, with nested tuplets rebuilt from each ChordRest's chain.
    std::vector<nt::Item> voiceItems(const Measure* m, track_idx_t track, const nt::Rational& barLen, bool& anyContent) const
    {
        struct Open {
            const mu::engraving::Tuplet* tuplet;
            nt::Tuplet node;
        };
        std::vector<nt::Item> top;
        std::vector<Open> stack;
        nt::Rational pos;

        auto sink = [&](nt::Item item) {
            if (stack.empty()) {
                top.push_back(std::move(item));
            } else {
                stack.back().node.items.push_back(std::move(item));
            }
        };
        auto closeTo = [&](size_t depth) {
            while (stack.size() > depth) {
                Open o = std::move(stack.back());
                stack.pop_back();
                sink(std::move(o.node));
            }
        };
        auto gap = [&](const nt::Rational& len) {
            for (const nt::Duration& d : splitDuration(len)) {
                nt::Event g;
                g.kind = track % VOICES == 0 ? nt::EventKind::Rest : nt::EventKind::Gap;
                g.duration = d;
                sink(g);
            }
        };

        for (const Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
            const EngravingItem* el = s->element(track);
            if (!el || !el->isChordRest()) {
                continue;
            }
            const ChordRest* cr = toChordRest(el);
            const nt::Rational at = toRational(s->rtick());

            std::vector<const mu::engraving::Tuplet*> chain;
            for (const mu::engraving::Tuplet* t = cr->tuplet(); t; t = t->tuplet()) {
                chain.insert(chain.begin(), t);
            }
            size_t common = 0;
            while (common < chain.size() && common < stack.size() && stack[common].tuplet == chain[common]) {
                ++common;
            }
            closeTo(common);
            if (stack.empty() && pos < at) {
                gap(at - pos); // a hole in the voice (corrupt in voice 1, normal in 2-4)
            }
            for (size_t i = common; i < chain.size(); ++i) {
                Open o;
                o.tuplet = chain[i];
                o.node.actual = chain[i]->ratio().numerator();
                o.node.normal = chain[i]->ratio().denominator();
                stack.push_back(std::move(o));
            }

            nt::Event e = makeEvent(cr, true);
            if (!(e.kind == nt::EventKind::Gap)) {
                anyContent = true;
            }
            sink(e);
            if (stack.empty()) {
                pos = at + toRational(cr->actualTicks());
            } else {
                // position after the outermost tuplet is known once it closes
                const mu::engraving::Tuplet* outer = stack.front().tuplet;
                pos = toRational(outer->rtick()) + toRational(outer->ticks());
            }
        }
        closeTo(0);
        if (pos < barLen) {
            gap(barLen - pos);
        }
        return top;
    }

    void printStaff(nt::Document& doc, staff_idx_t si, const std::vector<const Measure*>& measures, int from, int to)
    {
        const Staff* staff = m_score->staff(si);
        const Part* part = staff->part();
        const Measure* first = measures[from - 1];
        const Fraction t0 = first->tick();

        nt::StaffHeader h;
        h.staff = static_cast<int>(si) + 1;
        h.name = plain(part->partName());
        if (part->nstaves() > 1) {
            const size_t local = si - part->staff(0)->idx() + 1;
            h.name += " (" + std::to_string(local) + ")";
        }
        h.attrs.push_back({ "clef", clefName(staff->clef(t0)) });
        const std::string tr = intervalName(staff->transpose(t0));
        if (!tr.empty()) {
            h.attrs.push_back({ "transposes", tr });
        }
        if (m_opt.writtenPitch && !tr.empty()) {
            h.attrs.push_back({ "pitch", "written" });
        }
        std::string prevKey = keyName(staff->keySigEvent(t0), !m_opt.writtenPitch);
        h.attrs.push_back({ "key", prevKey });
        std::string prevTime = timeName(staff->timeSig(t0));
        if (!prevTime.empty()) {
            h.attrs.push_back({ "time", prevTime });
        }
        ClefType prevClef = staff->clef(t0);
        doc.lines.push_back(h);

        for (int idx = from; idx <= to; ++idx) {
            m_barIndex = idx;
            const Measure* m = measures[idx - 1];
            const Fraction tick = m->tick();
            nt::BarRef ref;
            ref.index = idx;
            const int displayed = m->no() + 1;
            if (displayed != idx) {
                ref.display = std::to_string(displayed);
            }

            nt::BarAttrs attrs;
            attrs.bar = ref;
            if (idx > from) {
                const std::string t = timeName(staff->timeSig(tick));
                if (!t.empty() && t != prevTime) {
                    attrs.attrs.push_back({ "time", t });
                    prevTime = t;
                }
                const std::string k = keyName(staff->keySigEvent(tick), !m_opt.writtenPitch);
                if (k != prevKey) {
                    attrs.attrs.push_back({ "key", k });
                    prevKey = k;
                }
                const ClefType c = staff->clef(tick);
                if (c != prevClef) {
                    attrs.attrs.push_back({ "clef", clefName(c) });
                    prevClef = c;
                }
            }
            for (const Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
                for (const EngravingItem* a : s->annotations()) {
                    if (a->isRehearsalMark() && (a->track() / VOICES == si || (a->systemFlag() && si == 0))) {
                        attrs.attrs.push_back({ "mark", plain(toTextBase(a)->plainText()) });
                    }
                }
            }
            if (m->repeatStart()) {
                attrs.attrs.push_back({ "repeat", "start" });
            }
            if (auto vit = m_voltaAttrs.find(idx); vit != m_voltaAttrs.end()) {
                attrs.attrs.insert(attrs.attrs.end(), vit->second.begin(), vit->second.end());
            }
            for (const EngravingItem* el : m->el()) {
                if (el->isMarker()) {
                    attrs.attrs.push_back({ "marker", markerName(toMarker(el)) });
                } else if (el->isJump()) {
                    attrs.attrs.push_back({ "jump", "\"" + plain(toJump(el)->plainText()) + "\"" });
                }
            }
            if (m->repeatEnd()) {
                attrs.attrs.push_back({ "repeat", m->repeatCount() == 2 ? std::string("end") : "end:" + std::to_string(m->repeatCount()) });
            } else {
                switch (m->endBarLineType()) {
                case BarLineType::DOUBLE: attrs.attrs.push_back({ "barline", "double" }); break;
                case BarLineType::END: attrs.attrs.push_back({ "barline", "final" }); break;
                case BarLineType::BROKEN: attrs.attrs.push_back({ "barline", "dashed" }); break;
                case BarLineType::DOTTED: attrs.attrs.push_back({ "barline", "dotted" }); break;
                default: break;
                }
            }
            if (m->lineBreak()) {
                attrs.attrs.push_back({ "break", "line" });
            }
            if (m->pageBreak()) {
                attrs.attrs.push_back({ "break", "page" });
            }
            if (m->sectionBreak()) {
                attrs.attrs.push_back({ "break", "section" });
            }
            const Fraction len = m->ticks();
            if (len != m->timesig()) {
                attrs.attrs.push_back({ "len", toRational(len).toString() });
            }
            if (!attrs.attrs.empty()) {
                doc.lines.push_back(attrs);
            }

            const nt::Rational barLen = toRational(len);
            for (voice_idx_t v = 0; v < VOICES; ++v) {
                const int voiceNo = static_cast<int>(v) + 1;
                if (!m_opt.voices.empty() && std::find(m_opt.voices.begin(), m_opt.voices.end(), voiceNo) == m_opt.voices.end()) {
                    continue;
                }
                bool anyContent = false;
                nt::VoiceLine vl;
                vl.bar = ref;
                vl.voice = voiceNo;
                vl.items = voiceItems(m, si * VOICES + v, barLen, anyContent);
                if (v > 0 && !anyContent) {
                    m_details.clear();
                    continue; // empty voices 2-4 are not printed
                }
                doc.lines.push_back(vl);
                for (const std::string& d : m_details) {
                    doc.lines.push_back(nt::Comment { " " + d });
                }
                m_details.clear();
            }

            printHarmony(doc, m, si, ref, barLen);
        }
    }

    //! Voltas are system spanners; NOTATION.md section 6 prints them as bar attributes:
    //! volta=1,2 on the first bar, volta=end on the last bar they cover.
    void collectVoltas(const std::vector<const Measure*>& measures)
    {
        auto barOf = [&](const Fraction& tick) {
            for (size_t i = 0; i < measures.size(); ++i) {
                if (tick >= measures[i]->tick() && tick < measures[i]->tick() + measures[i]->ticks()) {
                    return static_cast<int>(i) + 1;
                }
            }
            return 0;
        };
        for (const auto& [tick, sp] : m_score->spanner()) {
            if (!sp->isVolta()) {
                continue;
            }
            const Volta* v = toVolta(sp);
            std::string endings;
            for (int e : v->endings()) {
                endings += (endings.empty() ? "" : ",") + std::to_string(e);
            }
            const int first = barOf(v->tick());
            const int last = barOf(v->tick2() - Fraction(1, 1920));
            if (first) {
                m_voltaAttrs[first].push_back({ "volta", endings.empty() ? "?" : endings });
            }
            if (last) {
                m_voltaAttrs[last].push_back({ "volta", "end" });
            }
        }
    }

    static std::string markerName(const Marker* mk)
    {
        switch (mk->markerType()) {
        case MarkerType::SEGNO:
        case MarkerType::VARSEGNO: return "segno";
        case MarkerType::CODA:
        case MarkerType::VARCODA: return "coda";
        case MarkerType::CODETTA: return "codetta";
        case MarkerType::FINE: return "fine";
        case MarkerType::TOCODA:
        case MarkerType::TOCODASYM: return "tocoda";
        default: return "\"" + plain(mk->plainText()) + "\"";
        }
    }

    void printHarmony(nt::Document& doc, const Measure* m, staff_idx_t si, const nt::BarRef& ref, const nt::Rational& barLen) const
    {
        std::vector<std::pair<nt::Rational, std::string> > chords;
        for (const Segment* s = m->first(); s; s = s->next()) {
            for (const EngravingItem* a : s->annotations()) {
                if (a->isHarmony() && a->track() / VOICES == si) {
                    std::string name = plain(toHarmony(a)->harmonyName());
                    if (name.empty()) {
                        name = plain(toHarmony(a)->plainText()); // e.g. "N.C." has no harmony name
                    }
                    if (!name.empty()) {
                        chords.emplace_back(toRational(s->rtick()), name);
                    }
                }
            }
        }
        if (chords.empty()) {
            return;
        }
        std::stable_sort(chords.begin(), chords.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        nt::HarmonyLine hl;
        hl.bar = ref;
        nt::Rational pos;
        for (size_t i = 0; i < chords.size(); ++i) {
            const nt::Rational at = chords[i].first;
            const nt::Rational next = i + 1 < chords.size() ? chords[i + 1].first : barLen;
            for (const nt::Duration& d : splitDuration(at - pos)) {
                hl.items.push_back({ std::nullopt, d });
            }
            const std::vector<nt::Duration> parts = splitDuration(next - at);
            for (size_t p = 0; p < parts.size(); ++p) {
                hl.items.push_back({ p == 0 ? std::optional<std::string>(chords[i].second) : std::nullopt, parts[p] });
            }
            pos = next;
        }
        doc.lines.push_back(hl);
    }

    const Score* m_score;
    ReadOptions m_opt;
    SpannerMarks m_spanners;
    std::map<int, std::vector<nt::KeyValue> > m_voltaAttrs;
    mutable std::vector<std::string> m_details; // "% detail:" lines for the voice line being built
    int m_barIndex = 0;
};
}

std::vector<nt::Duration> mu::agent::read::splitDuration(const nt::Rational& value)
{
    std::vector<nt::Duration> out;
    nt::Rational left = value;
    static const nt::BaseDuration bases[] = { nt::BaseDuration::Breve, nt::BaseDuration::Whole, nt::BaseDuration::Half, nt::BaseDuration::Quarter,
                                          nt::BaseDuration::Eighth, nt::BaseDuration::Sixteenth, nt::BaseDuration::ThirtySecond,
                                          nt::BaseDuration::SixtyFourth, nt::BaseDuration::OneTwentyEighth };
    int guard = 0;
    while (nt::Rational(0) < left && guard++ < 64) {
        bool placed = false;
        for (nt::BaseDuration b : bases) {
            for (int dots = 2; dots >= 0; --dots) {
                nt::Duration d { b, dots };
                if (!(left < d.value())) {
                    out.push_back(d);
                    left = left - d.value();
                    placed = true;
                    break;
                }
            }
            if (placed) {
                break;
            }
        }
        if (!placed) {
            break; // shorter than a 128th: cannot be expressed
        }
    }
    return out;
}

nt::Document mu::agent::read::printScore(const Score* score, const ReadOptions& options)
{
    if (!score) {
        return {};
    }
    return Printer(score, options).run();
}
