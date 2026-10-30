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
#include "textops.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <memory>
#include <regex>
#include <set>

#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/durationtype.h"
#include "engraving/dom/expression.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/fret.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/interval.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/rehearsalmark.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/sig.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/systemtext.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/textbase.h"
#include "engraving/editing/transpose.h"
#include "engraving/style/style.h"
#include "engraving/types/constants.h"
#include "engraving/types/symnames.h"

#include "address/address.h"
#include "core/handles.h"
#include "core/postconditions.h"
#include "notation/writer.h"
#include "read/scoreprinter.h"
#include "registry/errors.h"

using namespace mu::agent;
using namespace mu::engraving;
using muse::Ret;
using muse::Val;
namespace nt = mu::agent::notation;

namespace {
// ------------------------------------------------------------------ small helpers

bool has(const Params& p, const char* key)
{
    return address::has(p, key);
}

std::string str(const Params& p, const char* key, const std::string& def = {})
{
    return has(p, key) ? p.at(key).toString() : def;
}

bool blank(const std::string& s)
{
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool isWhole(const Val& v)
{
    const bool num = v.type() == Val::Type::Double || v.type() == Val::Type::Int || v.type() == Val::Type::Int64;
    return num && std::isfinite(v.toDouble()) && v.toDouble() == std::floor(v.toDouble());
}

std::string plain(const String& s)
{
    return s.toStdString();
}

//! Any write to a TextBase property goes through EngravingObject's public two-argument
//! undoChangeProperty: TempoText declares its three-argument override protected and without a
//! using-declaration (tempotext.h:107), so the call has to be made on the base class.
void undoProp(EngravingObject* o, Pid id, const PropertyValue& v)
{
    o->undoChangeProperty(id, v);
}

Val strList(const std::vector<std::string>& v)
{
    muse::ValList l;
    for (const std::string& s : v) {
        l.emplace_back(Val(s));
    }
    return Val(l);
}

std::string notationOf(MasterScore* score, int fromBar, int toBar, std::vector<int> staves)
{
    std::sort(staves.begin(), staves.end());
    staves.erase(std::unique(staves.begin(), staves.end()), staves.end());
    read::ReadOptions o;
    o.fromBar = fromBar;
    o.toBar = std::max(fromBar, toBar);
    o.staves = staves;
    return nt::write(read::printScore(score, o));
}

bool contains(const std::vector<EngravingItem*>& v, const EngravingItem* e)
{
    return std::find(v.begin(), v.end(), e) != v.end();
}

//! A grace chord stands for its main chord: text, lyrics and chord symbols belong to the beat
//! (Score::addText moves a lyric on a grace note to its parent, edit.cpp:925-929)
ChordRest* mainChordRest(ChordRest* cr)
{
    if (cr->isGrace() && cr->explicitParent() && cr->explicitParent()->isChordRest()) {
        return toChordRest(cr->explicitParent());
    }
    return cr;
}

Ret refuseMeasureRepeat(MasterScore* score, const ChordRest* cr)
{
    if (cr->isMeasureRepeat()) {
        // canAddTextToItem accepts it, but chordOrRest then treats it as a Rest (spec, add_text crash risks)
        return makeErr(Err::InvalidArg, address::where(score, cr) + " is a measure repeat; write the bar out first");
    }
    return muse::make_ok();
}

Ret voiceOf(const Params& p, int& voice)
{
    voice = 1;
    if (has(p, "voice")) {
        if (!isWhole(p.at("voice")) || p.at("voice").toDouble() < 1 || p.at("voice").toDouble() > 4) {
            return makeErr(Err::Range, "voice must be 1-4"); // as address::resolveTarget
        }
        voice = static_cast<int>(p.at("voice").toDouble());
    }
    return muse::make_ok();
}

//! The chord or rest a text, lyric or (with target) chord symbol is anchored to: `target` (a
//! handle; a note means its chord) or staff + at (+ voice), through the shared address module.
//! With anyVoice (system objects, which always land on the top staff's voice 1, edit.cpp:6792)
//! and no voice given, a position where voice 1 has nothing but another voice starts falls back
//! to that voice.
Ret resolveAnchor(MasterScore* score, const Params& p, ChordRest*& cr, bool anyVoice = false)
{
    cr = nullptr;
    if (has(p, "target")) {
        if (has(p, "staff") || has(p, "at") || has(p, "voice")) {
            return makeErr(Err::InvalidArg, "give either target (a handle), or staff + at (+ voice), not both");
        }
        if (p.at("target").type() != Val::Type::String) {
            return makeErr(Err::InvalidArg, "target must be a handle string from score_read");
        }
        return address::resolveTarget(score, p, "target", cr);
    }
    if (!has(p, "staff")) {
        return makeErr(Err::InvalidArg, "missing staff: {\"part\": \"<name>\", \"index\": 1} or {\"number\": N} (or give target)");
    }
    staff_idx_t staff = 0;
    if (Ret r = address::resolveStaff(score, p.at("staff"), staff); !r) {
        return r;
    }
    if (!has(p, "at")) {
        return makeErr(Err::InvalidArg, "missing at: {\"measure\": N, \"beat\": B}");
    }
    int voice = 1;
    if (Ret r = voiceOf(p, voice); !r) {
        return r;
    }
    Measure* m = nullptr;
    Fraction tick;
    if (Ret r = address::resolvePosition(score, p.at("at"), "at", m, tick); !r) {
        return r;
    }
    Ret r = address::chordRestAt(score, m, tick, staff * VOICES + static_cast<track_idx_t>(voice - 1), cr);
    if (!r && anyVoice && !has(p, "voice")) {
        for (track_idx_t v = 1; v < VOICES; ++v) {
            if (address::chordRestAt(score, m, tick, staff * VOICES + v, cr)) {
                return muse::make_ok();
            }
        }
        cr = nullptr;
    }
    return r;
}

// ------------------------------------------------------------------ linked copies

//! How many copies (the element included) Score::undoAddElement makes of a staff annotation
//! (STAFF_TEXT, EXPRESSION, HARMONY, LYRICS) on its staff and the staves linked to it
//! (edit.cpp:6957-7036): one per linked staff with a linked voice, for voice 1 also where that
//! voice is not linked (all four are VOICE1_COPY_TYPES), and only on linked staves that stand
//! for the same staff of the part (rstaff).
size_t expectedStaffCopies(const EngravingItem* e)
{
    const Staff* ostaff = e->staff();
    const track_idx_t strack = e->track();
    size_t n = 0;
    for (const Staff* st : ostaff->staffList()) {
        if (ostaff->getLinkedTrackInStaff(st, strack) == muse::nidx && track2voice(strack) != 0) {
            continue;
        }
        if (st != ostaff && st->rstaff() != ostaff->rstaff()) {
            continue;
        }
        ++n;
    }
    return n;
}

//! System objects (SYSTEM_TEXT, TEMPO_TEXT, REHEARSAL_MARK) get one copy on the top staff of
//! every score (master and parts) plus every system-object staff (edit.cpp:6716-6732)
size_t expectedSystemCopies(MasterScore* score)
{
    size_t n = 0;
    for (Score* s : score->scoreList()) {
        if (s->nstaves() > 0) {
            ++n;
        }
        for (const Staff* st : s->systemObjectStaves()) {
            if (st && st->idx() != muse::nidx) {
                ++n;
            }
        }
    }
    return n;
}

//! Post-condition for linked parts: the expected number of copies exist, each one is placed
//! where it belongs in its own score, and (for text) carries the requested text
Ret linkedCopiesPresent(const EngravingItem* e, size_t want, const std::string& what, const String* xml = nullptr)
{
    const std::list<EngravingObject*> links = e->linkList();
    if (links.size() != want) {
        return core::expect(false, what + " has " + std::to_string(links.size()) + " copies in the score and its parts, expected "
                            + std::to_string(want));
    }
    for (EngravingObject* o : links) {
        if (o == e) {
            continue;
        }
        const EngravingItem* c = toEngravingItem(o);
        bool placed = false;
        if (c->isLyrics()) {
            const EngravingItem* parent = c->parentItem();
            placed = parent && parent->isChordRest()
                     && std::find(toChordRest(parent)->lyrics().begin(), toChordRest(parent)->lyrics().end(), c)
                     != toChordRest(parent)->lyrics().end();
        } else if (c->explicitParent() && c->explicitParent()->isSegment()) {
            placed = contains(toSegment(c->explicitParent())->annotations(), c);
        }
        if (!placed) {
            return core::expect(false, "a linked copy of " + what + " is not in its part");
        }
        if (xml && c->isTextBase() && !c->isHarmony() && toTextBase(c)->xmlText() != *xml) {
            return core::expect(false, "a linked copy of " + what + " has the text \"" + plain(toTextBase(c)->xmlText())
                                + "\" instead of \"" + plain(*xml) + "\"");
        }
    }
    return muse::make_ok();
}

//! Lyrics: Score::undoAddElement parents each linked copy to seg->element(linkedTrack) in the
//! part without a null check (edit.cpp:7096-7110). Refuse before mutating if a linked staff has
//! no chord or rest there.
Ret linkedChordRestsPresent(MasterScore* score, const ChordRest* cr)
{
    const Staff* ostaff = cr->staff();
    const track_idx_t strack = cr->track();
    const Fraction tick = cr->segment()->tick();
    for (const Staff* st : ostaff->staffList()) {
        if (st == ostaff) {
            continue;
        }
        track_idx_t lt = ostaff->getLinkedTrackInStaff(st, strack);
        if (lt == muse::nidx) {
            if (track2voice(strack) != 0) {
                continue;
            }
            lt = staff2track(st->idx());
        }
        if (st->rstaff() != ostaff->rstaff()) {
            continue;
        }
        const Measure* m = st->score()->tick2measure(tick);
        const Segment* seg = m ? m->findSegment(SegmentType::ChordRest, tick) : nullptr;
        const EngravingItem* e = seg ? seg->element(lt) : nullptr;
        if (!e || !e->isChordRest()) {
            return makeErr(Err::Refused, "a linked part has no note or rest at " + address::where(score, cr)
                           + " to carry the lyric (MuseScore would crash adding it); check the part's contents first");
        }
    }
    return muse::make_ok();
}

// ------------------------------------------------------------------ markup (spec Q1)

//! The raw `xml` argument is MuseScore text markup; TextBase::setXmlText stores anything
//! (textbase.cpp:2262-2267), so only a small, well-formed subset is accepted: <sym>Name</sym>
//! with a SMuFL name MuseScore knows, <b> <i> <u> <s> <sub> <sup> properly nested, and the five
//! XML entities or numeric character references.
Ret validateMarkup(const std::string& s)
{
    static const std::set<std::string> simple = { "b", "i", "u", "s", "sub", "sup" };
    auto bad = [](const std::string& why) {
        return makeErr(Err::InvalidArg, "xml: " + why + " (allowed: <sym>Name</sym>, <b>, <i>, <u>, <s>, <sub>, <sup>, "
                       "&amp; &lt; &gt; &quot; &apos;; or give plain text in text)");
    };
    std::vector<std::string> open;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '<') {
            const size_t j = s.find('>', i);
            if (j == std::string::npos) {
                return bad("'<' at character " + std::to_string(i + 1) + " is not closed");
            }
            const std::string tag = s.substr(i + 1, j - i - 1);
            const bool closing = !tag.empty() && tag[0] == '/';
            const std::string name = closing ? tag.substr(1) : tag;
            if (name == "sym") {
                if (closing) {
                    return bad("</sym> without <sym>");
                }
                const size_t k = s.find("</sym>", j + 1);
                if (k == std::string::npos) {
                    return bad("<sym> without </sym>");
                }
                const std::string sym = s.substr(j + 1, k - j - 1);
                if (SymNames::symIdByName(String::fromStdString(sym)) == SymId::noSym) {
                    return bad("unknown symbol \"" + sym + "\"");
                }
                i = k + 6;
                continue;
            }
            if (!simple.count(name)) {
                return bad("the tag <" + tag + "> is not allowed");
            }
            if (closing) {
                if (open.empty() || open.back() != name) {
                    return bad("</" + name + "> does not close the last open tag");
                }
                open.pop_back();
            } else {
                open.push_back(name);
            }
            i = j + 1;
            continue;
        }
        if (c == '>') {
            return bad("a bare '>' must be written &gt;");
        }
        if (c == '&') {
            const size_t j = s.find(';', i);
            const std::string ent = j == std::string::npos ? s.substr(i) : s.substr(i, j - i + 1);
            static const std::regex entity("&(amp|lt|gt|quot|apos|#[0-9]+|#x[0-9a-fA-F]+);");
            if (!std::regex_match(ent, entity)) {
                return bad("\"" + ent + "\" is not an entity; write & as &amp;");
            }
            i += ent.size();
            continue;
        }
        ++i;
    }
    if (!open.empty()) {
        return bad("<" + open.back() + "> is not closed");
    }
    if (blank(std::regex_replace(s, std::regex("<[^>]*>"), ""))) {
        // a symbol alone is fine; tags around nothing are empty text
        if (s.find("<sym>") == std::string::npos) {
            return makeErr(Err::InvalidArg, "xml has no text in it");
        }
    }
    return muse::make_ok();
}

//! The text argument: `text` (plain, escaped with plainToXmlText as setPlainText does,
//! textbase.h:318) or `xml` (markup, validated), never both. Empty text is refused: the UI
//! deletes text that is empty when editing ends (textedit.cpp:190-224).
Ret textArg(const Params& p, bool required, const std::string& what, bool& given, String& xml)
{
    const bool hasText = has(p, "text");
    const bool hasXml = has(p, "xml");
    given = hasText || hasXml;
    if (hasText && hasXml) {
        return makeErr(Err::InvalidArg, "give text (plain) or xml (markup), not both");
    }
    if (!given) {
        return required ? makeErr(Err::InvalidArg, what + " needs text (or xml)") : muse::make_ok();
    }
    const std::string s = p.at(hasText ? "text" : "xml").toString();
    if (blank(s)) {
        return makeErr(Err::InvalidArg, std::string(hasText ? "text" : "xml") + " is empty");
    }
    if (hasXml) {
        if (Ret r = validateMarkup(s); !r) {
            return r;
        }
        xml = String::fromStdString(s);
    } else {
        xml = TextBase::plainToXmlText(String::fromStdString(s));
    }
    return muse::make_ok();
}

// ------------------------------------------------------------------ tempo

struct BeatUnit {
    const char* name;
    DurationType type;
    int dots;
};

const std::vector<BeatUnit>& beatUnitTable()
{
    static const std::vector<BeatUnit> t = {
        { "w", DurationType::V_WHOLE, 0 },
        { "h.", DurationType::V_HALF, 1 },
        { "h", DurationType::V_HALF, 0 },
        { "q.", DurationType::V_QUARTER, 1 },
        { "q", DurationType::V_QUARTER, 0 },
        { "e.", DurationType::V_EIGHTH, 1 },
        { "e", DurationType::V_EIGHTH, 0 },
        { "s.", DurationType::V_16TH, 1 },
        { "s", DurationType::V_16TH, 0 },
        { "t.", DurationType::V_32ND, 1 },
        { "t", DurationType::V_32ND, 0 },
        { "x", DurationType::V_64TH, 0 },
        { "y", DurationType::V_128TH, 0 },
    };
    return t;
}

TDuration durationOf(const BeatUnit& u)
{
    TDuration d(u.type);
    d.setDots(u.dots);
    return d;
}

//! The beat Score::addText's default tempo marking uses for a time signature (edit.cpp:965-1013).
//! One difference: for /128 compound metres upstream writes <sym>metNote64ndUp</sym> (edit.cpp:1006),
//! a symbol that does not exist, and no tempo pattern reads a dotted 64th (tempotext.cpp:113-135,
//! 170-200), so the default there is the 128th note.
std::string defaultBeatUnit(MasterScore* score, const Fraction& tick)
{
    const Fraction f = score->sigmap()->timesig(tick).nominal();
    const bool compound = f.numerator() % 3 == 0;
    switch (f.denominator()) {
    case 1: return "w";
    case 2: return "h";
    case 8: return compound ? "q." : "e";
    case 16: return compound ? "e." : "s";
    case 32: return compound ? "s." : "t";
    case 64: return compound ? "t." : "x";
    case 128: return "y";
    default: return "q";
    }
}

//! Does the text carry a metronome mark ("<note> = <number>") that TempoText::updateTempo would
//! read? The same clean-up as updateTempo (tempotext.cpp:262-270), then its absolute pattern
//! (note, spaces, "=", spaces, a number; :265-280). Looked at on a probe that is never added to the score.
bool hasMetronomeMark(MasterScore* score, const String& xml)
{
    std::unique_ptr<TempoText> probe(Factory::createTempoText(score->dummy()->segment()));
    probe->setXmlText(xml);
    String s = probe->plainText();
    s.replace(u",", u".");
    s.replace(u"<sym>space</sym>", u" ");
    s.replace(u"\u2252", u"="); // approximately equal to or the image of
    s.replace(u"\u2248", u"="); // almost equal to
    s.replace(u"~", u"=");
    s.replace(u"ca.", u"");
    s.replace(u"c.", u"");
    s.replace(u"approx.", u"");
    int len = 0;
    TDuration d;
    const int at = TempoText::findTempoDuration(s, len, d); // a byte offset into the UTF-8 text
    if (at < 0) {
        return false; // no metronome note
    }
    // the note (one SMuFL character, 3 bytes in UTF-8), optional augmentation dots (U+ECB7), then "= <digit>"
    static const std::regex mark("^[\\s\\S]{3}(\\s*\xEE\xB2\xB7)*\\s*=\\s*[0-9]");
    const std::string u8 = s.toStdString();
    return std::regex_search(u8.substr(static_cast<size_t>(at)), mark);
}

// ------------------------------------------------------------------ text kinds

enum class Kind {
    Staff,
    System,
    Expression,
    Tempo,
    Rehearsal
};

bool isSystemKind(Kind k)
{
    return k == Kind::System || k == Kind::Tempo || k == Kind::Rehearsal;
}

ElementType elementTypeOf(Kind k)
{
    switch (k) {
    case Kind::Staff: return ElementType::STAFF_TEXT;
    case Kind::System: return ElementType::SYSTEM_TEXT;
    case Kind::Expression: return ElementType::EXPRESSION;
    case Kind::Tempo: return ElementType::TEMPO_TEXT;
    case Kind::Rehearsal: return ElementType::REHEARSAL_MARK;
    }
    return ElementType::STAFF_TEXT;
}

TextStyleType styleOf(Kind k)
{
    switch (k) {
    case Kind::Staff: return TextStyleType::STAFF;
    case Kind::System: return TextStyleType::SYSTEM;
    case Kind::Expression: return TextStyleType::EXPRESSION;
    case Kind::Tempo: return TextStyleType::TEMPO;
    case Kind::Rehearsal: return TextStyleType::REHEARSAL_MARK;
    }
    return TextStyleType::STAFF;
}

std::string voiceAssignmentName(const EngravingItem* e)
{
    return e->getProperty(Pid::VOICE_ASSIGNMENT).value<VoiceAssignment>() == VoiceAssignment::CURRENT_VOICE_ONLY
           ? "this_voice" : "all_voices";
}

//! The voice_assignment argument applied to an expression not yet in the score, as the UI's
//! setInitialTrackAndVoiceAssignment does after adding (engravingitem.cpp:1317-1330), but before,
//! so the linked copies get the same track and assignment (spec Q4; the same rule as
//! expression-marks' dynamics). The default is all_voices whatever the app setting.
void applyVoiceAssignment(EngravingItem* e, track_idx_t crTrack, const std::string& mode)
{
    if (mode == "this_voice") {
        e->setTrack(crTrack);
        e->setProperty(Pid::VOICE_ASSIGNMENT, VoiceAssignment::CURRENT_VOICE_ONLY);
    } else if (mode == "preference") {
        e->setInitialTrackAndVoiceAssignment(crTrack, false);
    } else {
        e->setTrack(trackZeroVoice(crTrack));
        e->setProperty(Pid::VOICE_ASSIGNMENT, VoiceAssignment::ALL_VOICE_IN_INSTRUMENT);
    }
}

// ------------------------------------------------------------------ chord symbols

HarmonyType harmonyTypeOf(const std::string& n)
{
    if (n == "roman") {
        return HarmonyType::ROMAN;
    }
    if (n == "nashville") {
        return HarmonyType::NASHVILLE;
    }
    return HarmonyType::STANDARD;
}

//! Harmony::endEdit's substitutions of typed accidental symbols (harmony.cpp:787-808), with the
//! double flat and double sharp written as the real code points U+1D12B and U+1D12A (upstream
//! writes u"\u1d12b" (harmony.cpp:790), which is U+1D12 followed by 'b', so it never matches).
String normaliseChordText(String s, HarmonyType type)
{
    if (type != HarmonyType::ROMAN) {
        s.replace(u"\U0001D12B", u"bb");
        s.replace(u"\u266d", u"b");
        s.replace(u"\ue260", u"b");
        s.replace(u"\u266f", u"#");
        s.replace(u"\ue262", u"#");
        s.replace(u"\U0001D12A", u"x");
        s.replace(u"\u0394", u"^");
        s.replace(u"\u00d0", u"o");
        s.replace(u"\u00f8", u"0");
        s.replace(u"\u00d8", u"0");
    } else {
        s.replace(u"\ue260", u"\u266d");
        s.replace(u"\ue261", u"\u266e");
        s.replace(u"\ue262", u"\u266f");
    }
    return s;
}

//! The chord symbol of that type on (segment, track), free or in a fret diagram, as
//! NotationInteraction::findHarmonyInSegment looks for it (notationinteraction.cpp:8128-8150; private)
Harmony* findHarmony(const Segment* seg, track_idx_t track, HarmonyType type)
{
    for (EngravingItem* e : seg->annotations()) {
        Harmony* h = e->isHarmony() ? toHarmony(e) : e->isFretDiagram() ? toFretDiagram(e)->harmony() : nullptr;
        if (h && h->track() == track && h->harmonyType() == type) {
            return h;
        }
    }
    return nullptr;
}

//! Recipe B (spec 0.3): NotationInteraction::createHarmony (:8153-8163) then the parse step of
//! Harmony::endEdit (harmony.cpp:813-814). Parent and track are set before parsing (the parse
//! reads the staff's key for Nashville numbers, harmony.cpp:537), and Pid::TEXT is never set
//! (Harmony::setProperty(TEXT) dereferences the parent unchecked, harmony.cpp:1558).
Harmony* buildHarmony(MasterScore* score, Segment* seg, track_idx_t track, HarmonyType type, const String& text)
{
    Harmony* h = Factory::createHarmony(score->dummy()->segment());
    h->setScore(score);
    h->setParent(seg);
    h->setTrack(track);
    h->setHarmonyType(type);
    h->setHarmony(text);
    h->setPlainText(h->harmonyName());
    return h;
}

bool recognised(const Harmony* h, const String& text)
{
    if (h->harmonyType() == HarmonyType::ROMAN) {
        return true; // roman numerals are stored as text (harmony.cpp:513-518)
    }
    const std::string t = plain(text);
    if (t == "N.C." || t == "NC" || t == "N.C" || t == "No Chord") {
        return true;
    }
    for (const HarmonyInfo* info : h->chords()) {
        if (!tpcIsValid(info->rootTpc())) {
            return false;
        }
    }
    return !h->chords().empty();
}

// ------------------------------------------------------------------ lyrics

const char* syllabicName(LyricsSyllabic s)
{
    switch (s) {
    case LyricsSyllabic::SINGLE: return "single";
    case LyricsSyllabic::BEGIN: return "begin";
    case LyricsSyllabic::MIDDLE: return "middle";
    case LyricsSyllabic::END: return "end";
    }
    return "single";
}

bool parseSyllabic(const std::string& n, LyricsSyllabic& out)
{
    if (n == "single") {
        out = LyricsSyllabic::SINGLE;
    } else if (n == "begin") {
        out = LyricsSyllabic::BEGIN;
    } else if (n == "middle") {
        out = LyricsSyllabic::MIDDLE;
    } else if (n == "end") {
        out = LyricsSyllabic::END;
    } else {
        return false;
    }
    return true;
}

bool continues(LyricsSyllabic s)
{
    return s == LyricsSyllabic::BEGIN || s == LyricsSyllabic::MIDDLE;
}

bool joinedToPrevious(LyricsSyllabic s)
{
    return s == LyricsSyllabic::MIDDLE || s == LyricsSyllabic::END;
}

//! Where the next syllable goes, as the space key finds it (navigateToLyrics,
//! notationinteraction.cpp:6719-6733): the next chord on the track, or a rest that already has a
//! lyric in that verse and placement; other rests are skipped
ChordRest* nextLyricSlot(const ChordRest* after, track_idx_t track, int verse, PlacementV placement)
{
    for (Segment* s = after->segment()->next1(SegmentType::ChordRest); s; s = s->next1(SegmentType::ChordRest)) {
        EngravingItem* e = s->element(track);
        if (!e || !e->isChordRest()) {
            continue;
        }
        ChordRest* cr = toChordRest(e);
        if (cr->isChord() || cr->isMeasureRepeat() || cr->lyrics(verse, placement)) {
            return cr;
        }
    }
    return nullptr;
}

//! The lyric of that verse and placement nearest before / after a chord or rest on the same track
Lyrics* lyricBefore(const ChordRest* cr, track_idx_t track, int verse, PlacementV placement)
{
    for (Segment* s = cr->segment()->prev1(SegmentType::ChordRest); s; s = s->prev1(SegmentType::ChordRest)) {
        EngravingItem* e = s->element(track);
        if (e && e->isChordRest()) {
            if (Lyrics* l = toChordRest(e)->lyrics(verse, placement)) {
                return l;
            }
        }
    }
    return nullptr;
}

Lyrics* lyricAfter(const ChordRest* cr, track_idx_t track, int verse, PlacementV placement)
{
    for (Segment* s = cr->segment()->next1(SegmentType::ChordRest); s; s = s->next1(SegmentType::ChordRest)) {
        EngravingItem* e = s->element(track);
        if (e && e->isChordRest()) {
            if (Lyrics* l = toChordRest(e)->lyrics(verse, placement)) {
                return l;
            }
        }
    }
    return nullptr;
}

std::string lyricWhere(MasterScore* score, const Lyrics* l)
{
    return "\"" + plain(l->plainText()) + "\" at " + address::where(score, l->chordRest());
}
}

// ================================================================== names

const std::vector<std::string>& mu::agent::text::textKinds()
{
    static const std::vector<std::string> n = { "staff", "system", "expression", "tempo", "rehearsal" };
    return n;
}

const std::vector<std::string>& mu::agent::text::beatUnits()
{
    static const std::vector<std::string> n = [] {
        std::vector<std::string> v;
        for (const BeatUnit& u : beatUnitTable()) {
            v.emplace_back(u.name);
        }
        return v;
    }();
    return n;
}

const std::vector<std::string>& mu::agent::text::harmonyNotations()
{
    static const std::vector<std::string> n = { "standard", "roman", "nashville" };
    return n;
}

const std::vector<std::string>& mu::agent::text::syllabicNames()
{
    static const std::vector<std::string> n = { "single", "begin", "middle", "end" };
    return n;
}

// ================================================================== add_text

Ret mu::agent::text::addText(const Params& p, MasterScore* score, Out& out)
{
    // 1. validate (nothing changes before Score::addText)
    const std::string kindName = str(p, "kind");
    Kind kind;
    if (kindName == "staff") {
        kind = Kind::Staff;
    } else if (kindName == "system") {
        kind = Kind::System;
    } else if (kindName == "expression") {
        kind = Kind::Expression;
    } else if (kindName == "tempo") {
        kind = Kind::Tempo;
    } else if (kindName == "rehearsal") {
        kind = Kind::Rehearsal;
    } else {
        return makeErr(Err::InvalidArg, "kind must be staff, system, expression, tempo or rehearsal");
    }
    if (has(p, "tempo") && kind != Kind::Tempo) {
        return makeErr(Err::InvalidArg, "tempo {bpm, beat_unit, follow_text} is for kind tempo only");
    }
    if (has(p, "voice_assignment") && kind != Kind::Expression) {
        return makeErr(Err::InvalidArg, "voice_assignment is for kind expression only");
    }
    const std::string mode = str(p, "voice_assignment", "all_voices");
    if (mode != "all_voices" && mode != "this_voice" && mode != "preference") {
        return makeErr(Err::InvalidArg, "voice_assignment must be all_voices, this_voice or preference");
    }

    // tempo: {bpm, beat_unit, follow_text}
    bool hasBpm = false;
    double bpm = 0.0;
    std::string unitName;
    bool hasFollow = false;
    bool follow = true;
    if (has(p, "tempo")) {
        const Val& tv = p.at("tempo");
        if (tv.type() != Val::Type::Map) {
            return makeErr(Err::InvalidArg, "tempo must be an object: {\"bpm\": 120, \"beat_unit\": \"q\"}");
        }
        const muse::ValMap t = tv.toMap();
        for (const auto& [k, v] : t) {
            if (k != "bpm" && k != "beat_unit" && k != "follow_text") {
                return makeErr(Err::InvalidArg, "tempo has an unknown key '" + k + "' (bpm, beat_unit or follow_text)");
            }
        }
        if (t.count("bpm") && !t.at("bpm").isNull()) {
            const Val& b = t.at("bpm");
            const bool num = b.type() == Val::Type::Double || b.type() == Val::Type::Int || b.type() == Val::Type::Int64;
            if (!num || !std::isfinite(b.toDouble()) || b.toDouble() <= 0.0) {
                return makeErr(Err::InvalidArg, "tempo.bpm must be a number greater than 0");
            }
            hasBpm = true;
            bpm = b.toDouble();
        }
        if (t.count("beat_unit") && !t.at("beat_unit").isNull()) {
            unitName = t.at("beat_unit").toString();
            const auto& units = beatUnits();
            if (std::find(units.begin(), units.end(), unitName) == units.end()) {
                return makeErr(Err::InvalidArg, "tempo.beat_unit must be one of w, h., h, q., q, e., e, s., s");
            }
            if (!hasBpm) {
                return makeErr(Err::InvalidArg, "tempo.beat_unit needs tempo.bpm");
            }
        }
        if (t.count("follow_text") && !t.at("follow_text").isNull()) {
            if (t.at("follow_text").type() != Val::Type::Bool) {
                return makeErr(Err::InvalidArg, "tempo.follow_text must be true or false");
            }
            hasFollow = true;
            follow = t.at("follow_text").toBool();
        }
    }

    bool textGiven = false;
    String xml;
    const bool textRequired = kind == Kind::Staff || kind == Kind::System || kind == Kind::Expression;
    if (Ret r = textArg(p, textRequired, "kind " + kindName, textGiven, xml); !r) {
        return r;
    }
    if (kind == Kind::Tempo && !textGiven && !hasBpm) {
        return makeErr(Err::InvalidArg, "kind tempo needs text (e.g. \"Allegro\") or tempo.bpm (or both)");
    }

    ChordRest* cr = nullptr;
    if (Ret r = resolveAnchor(score, p, cr, isSystemKind(kind)); !r) {
        return r;
    }
    cr = mainChordRest(cr);
    if (Ret r = refuseMeasureRepeat(score, cr); !r) {
        return r;
    }
    Segment* seg = cr->segment();
    const ElementType type = elementTypeOf(kind);

    // tempo: what the marking will say and play
    bool mark = false;
    double bps = 0.0; // quarter beats per second when bpm is given
    if (kind == Kind::Tempo) {
        mark = textGiven && hasMetronomeMark(score, xml);
        if (mark && hasBpm) {
            return makeErr(Err::InvalidArg, "the text already has a metronome mark; give the tempo in the text "
                           "(\"<sym>metNoteQuarterUp</sym> = 120\") or as tempo.bpm, not both");
        }
        if (hasBpm) {
            if (unitName.empty()) {
                unitName = defaultBeatUnit(score, cr->tick());
            }
            const BeatUnit* unit = nullptr;
            for (const BeatUnit& u : beatUnitTable()) {
                if (unitName == u.name) {
                    unit = &u;
                }
            }
            const TDuration d = durationOf(*unit);
            const double quarters = static_cast<double>(d.ticks().ticks()) / Constants::DIVISION;
            const double quarterBpm = bpm * quarters;
            // TempoText::setTempo clamps to 5-999 quarter beats a minute (tempotext.cpp:43-44, 324-332)
            if (quarterBpm < 5.0 || quarterBpm > 999.0) {
                return makeErr(Err::InvalidArg, "tempo " + address::number(bpm) + " to the " + unitName + " is "
                               + address::number(quarterBpm) + " quarter notes a minute; MuseScore plays 5-999");
            }
            bps = quarterBpm / 60.0;
            if (!textGiven) {
                // the metronome marking Score::addText writes (edit.cpp:965-1013), with this number
                xml = TempoText::duration2tempoTextString(d) + u" = " + String::fromStdString(address::number(bpm));
                mark = true;
            }
        }
    }

    // spec Q2: one tempo marking and one rehearsal mark per position; the same staff, system or
    // expression text twice at one place is refused too (a repeated request, not a second text)
    const track_idx_t wantTrack = kind == Kind::Expression ? (mode == "this_voice" ? cr->track() : trackZeroVoice(cr->track()))
                                  : cr->track();
    for (EngravingItem* a : seg->annotations()) {
        if (a->type() != type) {
            continue;
        }
        const bool sameText = toTextBase(a)->xmlText() == xml;
        bool clash = false;
        if (kind == Kind::Tempo || kind == Kind::Rehearsal) {
            clash = true;
        } else if (kind == Kind::System) {
            clash = sameText;
        } else if (mode != "preference") {
            clash = sameText && a->track() == wantTrack;
        } else {
            clash = sameText && a->staffIdx() == cr->staffIdx();
        }
        if (clash) {
            return makeErr(Err::Exists, address::where(score, cr) + " already has " + (kind == Kind::Tempo ? "a tempo marking"
                                                                                       : kind == Kind::Rehearsal ? "a rehearsal mark"
                                                                                       : "that text")
                           + " (\"" + plain(toTextBase(a)->plainText()) + "\", " + core::makeHandle(a) + ")");
        }
    }

    // 2. edit
    std::vector<std::string> messages;
    TextBase* t = nullptr;
    if (kind == Kind::Expression) {
        // Recipe B: built with its text and voice assignment, then added (spec Q4)
        Expression* e = Factory::createExpression(score->dummy()->segment());
        e->setXmlText(xml);
        applyVoiceAssignment(e, cr->track(), mode);
        e->setParent(seg);
        score->undoAddElement(e);
        t = e;
    } else {
        // Recipe A: the UI's creation call, then the property change TextBase::endEdit makes
        // (textedit.cpp:239), which also reaches the linked copies
        t = score->addText(styleOf(kind), cr);
        if (Ret r = core::expect(t != nullptr, "Score::addText made no " + kindName + " text at " + address::where(score, cr)); !r) {
            return r;
        }
        if (!xml.isEmpty() && t->xmlText() != xml) {
            undoProp(t, Pid::TEXT, PropertyValue(xml));
        }
        if (kind == Kind::Tempo) {
            TempoText* tt = toTempoText(t);
            // TempoText::commitText: re-read the text when following it (tempotext.cpp:493-499)
            if (tt->followText()) {
                tt->updateTempo();
            }
            if (hasBpm && !mark) {
                // words only ("Allegro") with a number: play it, and stop the text from being
                // re-read later, which would find no number (spec Q5)
                undoProp(tt, Pid::TEMPO_FOLLOW_TEXT, PropertyValue(false));
                undoProp(tt, Pid::TEMPO, PropertyValue(BeatsPerSecond(bps)));
            }
            if (hasFollow && tt->followText() != follow) {
                undoProp(tt, Pid::TEMPO_FOLLOW_TEXT, PropertyValue(follow)); // true re-reads the text (tempotext.cpp:244-250)
            }
            if (!mark && !hasBpm) {
                messages.push_back("the text has no metronome mark, so playback keeps the " + address::number(tt->tempo().val * 60.0)
                                   + " quarter notes a minute of MuseScore's default marking; pass tempo.bpm to choose");
            }
        }
    }

    // 3. check
    const bool inSegment = contains(seg->annotations(), t);
    const track_idx_t finalTrack = isSystemKind(kind) ? 0 : (kind == Kind::Expression ? t->track() : cr->track());
    if (Ret r = core::expect(inSegment && t->type() == type && t->track() == finalTrack && (xml.isEmpty() || t->xmlText() == xml),
                             "the " + kindName + " text at " + address::where(score, cr) + " is not the one requested"); !r) {
        return r;
    }
    if (kind == Kind::Expression && mode != "preference") {
        const VoiceAssignment va = mode == "this_voice" ? VoiceAssignment::CURRENT_VOICE_ONLY : VoiceAssignment::ALL_VOICE_IN_INSTRUMENT;
        if (Ret r = core::expect(t->getProperty(Pid::VOICE_ASSIGNMENT).value<VoiceAssignment>() == va && t->track() == wantTrack,
                                 "the expression's voice assignment is not " + mode); !r) {
            return r;
        }
    }
    double playedBpm = 0.0;
    if (kind == Kind::Tempo) {
        const TempoText* tt = toTempoText(t);
        if (hasBpm) {
            if (Ret r = core::expect(std::fabs(tt->tempo().val - bps) < 1e-9, "the tempo marking does not play "
                                     + address::number(bps * 60.0) + " quarter notes a minute"); !r) {
                return r;
            }
        }
        playedBpm = tt->tempo().val * 60.0;
    }
    const String finalXml = t->xmlText();
    const size_t copies = isSystemKind(kind) ? expectedSystemCopies(score) : expectedStaffCopies(t);
    if (Ret r = linkedCopiesPresent(t, copies, "the " + kindName + " text at " + address::where(score, cr), &finalXml); !r) {
        return r;
    }

    const int bar = address::barOf(score, cr->measure());
    const int staffNo = static_cast<int>(t->staffIdx()) + 1;
    out["handle"] = Val(core::makeHandle(t));
    out["status"] = Val(std::string("added"));
    out["kind"] = Val(kindName);
    out["text"] = Val(plain(t->plainText()));
    out["staff"] = Val(staffNo);
    out["voice"] = Val(static_cast<int>(t->voice()) + 1);
    if (kind == Kind::Tempo) {
        out["bpm"] = Val(playedBpm);
        out["followText"] = Val(toTempoText(t)->followText());
    }
    if (kind == Kind::Expression) {
        out["voiceAssignment"] = Val(voiceAssignmentName(t));
    }
    if (!messages.empty()) {
        out["messages"] = strList(messages);
    }
    out["notation"] = Val(notationOf(score, bar, bar, { staffNo }));
    return muse::make_ok();
}

// ================================================================== add_chord_symbol

Ret mu::agent::text::addChordSymbol(const Params& p, MasterScore* score, Out& out)
{
    // 1. validate
    if (!has(p, "text") || p.at("text").type() != Val::Type::String) {
        return makeErr(Err::InvalidArg, "missing text: the chord symbol, e.g. \"Cmaj7\", \"F#m7b5\", \"Bb/D\", \"N.C.\"");
    }
    const std::string raw = p.at("text").toString();
    if (blank(raw)) {
        return makeErr(Err::InvalidArg, "text is empty; give the chord symbol, e.g. \"Cmaj7\"");
    }
    const std::string notationName = str(p, "notation", "standard");
    const auto& notations = harmonyNotations();
    if (std::find(notations.begin(), notations.end(), notationName) == notations.end()) {
        return makeErr(Err::InvalidArg, "notation must be standard, roman or nashville");
    }
    const std::string ifExists = str(p, "if_exists", "error");
    if (ifExists != "error" && ifExists != "replace") {
        return makeErr(Err::InvalidArg, "if_exists must be error or replace");
    }
    const HarmonyType htype = harmonyTypeOf(notationName);

    // where: any tick inside a bar on a staff and voice (no note needed, as chord-symbol
    // navigation creates the segment, notationinteraction.cpp:7334), or a handle's chord/rest
    Measure* m = nullptr;
    Fraction tick;
    track_idx_t track = 0;
    if (has(p, "target")) {
        ChordRest* cr = nullptr;
        if (Ret r = resolveAnchor(score, p, cr); !r) {
            return r;
        }
        cr = mainChordRest(cr);
        m = cr->measure();
        tick = cr->tick();
        track = cr->track();
    } else {
        if (!has(p, "staff")) {
            return makeErr(Err::InvalidArg, "missing staff: {\"part\": \"<name>\", \"index\": 1} or {\"number\": N} (or give target)");
        }
        staff_idx_t staff = 0;
        if (Ret r = address::resolveStaff(score, p.at("staff"), staff); !r) {
            return r;
        }
        if (!has(p, "at")) {
            return makeErr(Err::InvalidArg, "missing at: {\"measure\": N, \"beat\": B}");
        }
        int voice = 1;
        if (Ret r = voiceOf(p, voice); !r) {
            return r;
        }
        if (Ret r = address::resolvePosition(score, p.at("at"), "at", m, tick); !r) {
            return r;
        }
        track = staff * VOICES + static_cast<track_idx_t>(voice - 1);
    }
    const std::string place = "m" + std::to_string(address::barOf(score, m)) + " " + address::beatOf(m, tick) + " (staff "
                              + std::to_string(track / VOICES + 1) + ", voice " + std::to_string(track % VOICES + 1) + ")";

    const String text = normaliseChordText(String::fromStdString(raw).trimmed(), htype);
    Segment* seg = m->findSegment(SegmentType::ChordRest, tick);
    Harmony* existing = seg ? findHarmony(seg, track, htype) : nullptr;
    if (existing && ifExists == "error") {
        return makeErr(Err::Exists, place + " already has the chord symbol " + plain(existing->harmonyName()) + " ("
                       + core::makeHandle(existing) + "); pass if_exists \"replace\" to change it");
    }
    if (existing && existing->explicitParent() && existing->explicitParent()->isFretDiagram()) {
        return makeErr(Err::Exists, "the chord symbol at " + place + " belongs to a fret diagram; this command does not "
                       "replace those");
    }
    if (existing) {
        // spec Q6: replace = remove + Recipe B. Unchanged if it would read the same.
        std::unique_ptr<Harmony> probe(buildHarmony(score, seg, track, htype, text));
        if (probe->harmonyName() == existing->harmonyName() && probe->plainText() == existing->plainText()) {
            const int bar = address::barOf(score, m);
            out["handle"] = Val(core::makeHandle(existing));
            out["status"] = Val(std::string("unchanged"));
            out["name"] = Val(plain(existing->harmonyName()));
            out["recognised"] = Val(recognised(existing, text));
            out["notation"] = Val(notationOf(score, bar, bar, { static_cast<int>(track / VOICES) + 1 }));
            return muse::make_ok();
        }
    }

    // 2. edit
    if (existing) {
        // the linked copies go too; an annotation-only segment left empty is removed with it
        // (edit.cpp:7484-7528), so the segment is looked up again below
        score->undoRemoveElement(existing);
    }
    seg = m->undoGetSegment(SegmentType::ChordRest, tick);
    Harmony* h = buildHarmony(score, seg, track, htype, text);
    score->undoAddElement(h); // linked copies, transposed where the part's concert pitch differs (edit.cpp:7174-7187)

    // 3. check
    if (Ret r = core::expect(seg && contains(seg->annotations(), h) && h->track() == track && h->harmonyType() == htype
                             && h->plainText() == h->harmonyName() && findHarmony(seg, track, htype) == h,
                             "the chord symbol at " + place + " is not the one requested"); !r) {
        return r;
    }
    if (Ret r = linkedCopiesPresent(h, expectedStaffCopies(h), "the chord symbol at " + place); !r) {
        return r;
    }
    // a copy in a score whose concert-pitch setting differs is transposed by its staff's interval
    // (edit.cpp:7174-7187, Transpose::undoTransposeHarmony with double sharps and flats allowed)
    const bool concert = score->style().styleB(Sid::concertPitch);
    for (EngravingObject* o : h->linkList()) {
        if (o == h) {
            continue;
        }
        const Harmony* c = toHarmony(o);
        const bool partConcert = c->score()->style().styleB(Sid::concertPitch);
        Interval iv;
        if (partConcert != concert) {
            iv = c->staff()->transpose(tick);
            if (!partConcert) {
                iv.flip();
            }
        }
        const std::vector<HarmonyInfo*> mine = h->chords();
        const std::vector<HarmonyInfo*> theirs = c->chords();
        bool same = mine.size() == theirs.size();
        for (size_t i = 0; same && i < mine.size(); ++i) {
            const int root = iv.isZero() ? mine[i]->rootTpc() : Transpose::transposeTpc(mine[i]->rootTpc(), iv, true);
            const int bass = iv.isZero() ? mine[i]->bassTpc() : Transpose::transposeTpc(mine[i]->bassTpc(), iv, true);
            same = theirs[i]->rootTpc() == root && theirs[i]->bassTpc() == bass;
        }
        if (Ret r = core::expect(same, "the copy of the chord symbol at " + place + " in a part reads "
                                 + plain(c->harmonyName()) + ", not transposed as that part's concert pitch setting needs"); !r) {
            return r;
        }
    }

    const bool ok = recognised(h, text);
    const int bar = address::barOf(score, m);
    out["handle"] = Val(core::makeHandle(h));
    out["status"] = Val(std::string(existing ? "replaced" : "added"));
    out["name"] = Val(plain(h->harmonyName()));
    out["recognised"] = Val(ok);
    if (!ok) {
        out["messages"] = strList({ "MuseScore does not recognise \"" + plain(text) + "\" as a chord; it is shown as written "
                                    "and does not play or transpose" });
    }
    out["notation"] = Val(notationOf(score, bar, bar, { static_cast<int>(track / VOICES) + 1 }));
    return muse::make_ok();
}

// ================================================================== add_lyric

namespace {
struct Syllable {
    std::string text;
    String xml;
    LyricsSyllabic syllabic = LyricsSyllabic::SINGLE;
    enum class Melisma {
        None,
        Self,
        To
    } melisma = Melisma::None;
    Val to;
    ChordRest* cr = nullptr;
    ChordRest* end = nullptr; // last chord under the extender (or cr)
    Fraction ticks { 0, 1 };
    Lyrics* existing = nullptr;
};
}

Ret mu::agent::text::addLyric(const Params& p, MasterScore* score, Out& out)
{
    // 1. validate everything, including where every syllable lands, before any change
    int verse = 1;
    if (has(p, "verse")) {
        if (!isWhole(p.at("verse")) || p.at("verse").toDouble() < 1 || p.at("verse").toDouble() > 99) {
            return makeErr(Err::InvalidArg, "verse must be a whole number from 1");
        }
        verse = static_cast<int>(p.at("verse").toDouble());
    }
    const int verse0 = verse - 1; // Pid::VERSE is 0-based (lyrics.cpp:85)
    const bool hasPlacement = has(p, "placement");
    PlacementV placement = score->style().styleV(Sid::lyricsPlacement).value<PlacementV>();
    if (hasPlacement) {
        const std::string pl = p.at("placement").toString();
        if (pl == "above") {
            placement = PlacementV::ABOVE;
        } else if (pl == "below") {
            placement = PlacementV::BELOW;
        } else {
            return makeErr(Err::InvalidArg, "placement must be above or below");
        }
    }
    const std::string ifExists = str(p, "if_exists", "error");
    if (ifExists != "error" && ifExists != "replace") {
        return makeErr(Err::InvalidArg, "if_exists must be error or replace");
    }
    if (!has(p, "syllables") || p.at("syllables").type() != Val::Type::List || p.at("syllables").toList().empty()) {
        return makeErr(Err::InvalidArg, "syllables must be a list of at least one {\"text\": \"...\"}");
    }

    std::vector<Syllable> syls;
    for (const Val& v : p.at("syllables").toList()) {
        const std::string n = "syllable " + std::to_string(syls.size() + 1);
        if (v.type() != Val::Type::Map) {
            return makeErr(Err::InvalidArg, n + " must be an object {\"text\": \"...\", \"syllabic\": \"single\"}");
        }
        const muse::ValMap s = v.toMap();
        for (const auto& [k, _] : s) {
            if (k != "text" && k != "syllabic" && k != "melisma_to") {
                return makeErr(Err::InvalidArg, n + " has an unknown key '" + k + "' (text, syllabic or melisma_to)");
            }
        }
        Syllable sy;
        if (!s.count("text") || s.at("text").type() != Val::Type::String || blank(s.at("text").toString())) {
            return makeErr(Err::InvalidArg, n + " has no text (an empty syllable is deleted by MuseScore)");
        }
        sy.text = s.at("text").toString();
        const std::string trimmed = String::fromStdString(sy.text).trimmed().toStdString();
        if (!trimmed.empty() && (trimmed.back() == '-' || trimmed.back() == '_')) {
            return makeErr(Err::InvalidArg, n + " (\"" + sy.text + "\") ends in '" + trimmed.back() + "'; say syllabic "
                           "\"begin\"/\"middle\" for a hyphen to the next syllable, or melisma_to for an extender");
        }
        sy.xml = TextBase::plainToXmlText(String::fromStdString(sy.text));
        if (s.count("syllabic") && !s.at("syllabic").isNull()) {
            if (!parseSyllabic(s.at("syllabic").toString(), sy.syllabic)) {
                return makeErr(Err::InvalidArg, n + ": syllabic must be single, begin, middle or end");
            }
        }
        if (s.count("melisma_to") && !s.at("melisma_to").isNull()) {
            const Val& to = s.at("melisma_to");
            if (to.type() == Val::Type::String && to.toString() == "self") {
                sy.melisma = Syllable::Melisma::Self;
            } else if (to.type() == Val::Type::Map) {
                sy.melisma = Syllable::Melisma::To;
                sy.to = to;
            } else {
                return makeErr(Err::InvalidArg, n + ": melisma_to must be {\"measure\": N, \"beat\": B} (the last note under "
                               "the extender) or \"self\"");
            }
            if (continues(sy.syllabic)) {
                return makeErr(Err::InvalidArg, n + " (\"" + sy.text + "\") is " + syllabicName(sy.syllabic) + " but has an "
                               "extender; a syllable held over a melisma ends its word: use single or end");
            }
        }
        if (!syls.empty()) {
            const Syllable& prev = syls.back();
            if (continues(prev.syllabic) != joinedToPrevious(sy.syllabic)) {
                return makeErr(Err::InvalidArg, n + " (\"" + sy.text + "\", " + syllabicName(sy.syllabic) + ") follows \""
                               + prev.text + "\" (" + syllabicName(prev.syllabic) + "), so it must be "
                               + (continues(prev.syllabic) ? "middle or end" : "single or begin"));
            }
        }
        syls.push_back(sy);
    }

    ChordRest* first = nullptr;
    if (Ret r = resolveAnchor(score, p, first); !r) {
        return r;
    }
    first = mainChordRest(first);
    const track_idx_t track = first->track();

    // place each syllable: the first at `at`, each next one on the next chord after the previous
    // one (or after the end of its extender)
    const ChordRest* prevEnd = nullptr;
    for (size_t k = 0; k < syls.size(); ++k) {
        Syllable& sy = syls[k];
        const std::string n = "syllable " + std::to_string(k + 1) + " (\"" + sy.text + "\")";
        if (k == 0) {
            sy.cr = first;
        } else {
            sy.cr = nextLyricSlot(prevEnd, track, verse0, placement);
            if (!sy.cr) {
                return makeErr(Err::Range, n + " has no note to go on: staff " + std::to_string(track / VOICES + 1) + " voice "
                               + std::to_string(track % VOICES + 1) + " has no chord after " + address::where(score, prevEnd));
            }
        }
        if (Ret r = refuseMeasureRepeat(score, sy.cr); !r) {
            return r;
        }
        sy.end = sy.cr;
        if (sy.melisma != Syllable::Melisma::None && !sy.cr->isChord()) {
            return makeErr(Err::InvalidArg, n + " is on a rest (" + address::where(score, sy.cr) + "); an extender starts on a note");
        }
        if (sy.melisma == Syllable::Melisma::Self) {
            // a one-chord extender, as the UI marks it (lyrics.h:49, notationinteraction.cpp:7781)
            sy.ticks = Lyrics::TEMP_MELISMA_TICKS;
        } else if (sy.melisma == Syllable::Melisma::To) {
            Measure* mm = nullptr;
            Fraction t;
            if (Ret r = address::resolvePosition(score, sy.to, "syllable " + std::to_string(k + 1) + ".melisma_to", mm, t); !r) {
                return r;
            }
            ChordRest* ecr = nullptr;
            if (Ret r = address::chordRestAt(score, mm, t, track, ecr); !r) {
                const std::string t = r.text();
                const size_t colon = t.find(": ");
                return makeErr(Err::InvalidArg, n + ": melisma_to: " + (t.rfind("E_", 0) == 0 && colon != std::string::npos
                                                                        ? t.substr(colon + 2) : t));
            }
            if (!ecr->isChord()) {
                return makeErr(Err::InvalidArg, n + ": melisma_to (" + address::where(score, ecr) + ") is a rest; an extender "
                               "ends on the last note sung to the syllable");
            }
            if (ecr->tick() <= sy.cr->tick()) {
                return makeErr(Err::InvalidArg, n + ": melisma_to (" + address::where(score, ecr) + ") is not after the "
                               "syllable's note (" + address::where(score, sy.cr) + "); give the last note under the extender, "
                               "or \"self\" for an extender on this note only");
            }
            // layout rewrites a melisma that runs into another syllable (lyricslayout.cpp:432-468)
            for (Segment* s = sy.cr->segment()->next1(SegmentType::ChordRest); s && s->tick() <= ecr->tick();
                 s = s->next1(SegmentType::ChordRest)) {
                EngravingItem* e = s->element(track);
                if (e && e->isChordRest()) {
                    if (const Lyrics* l = toChordRest(e)->lyrics(verse0, placement)) {
                        return makeErr(Err::InvalidArg, n + ": the extender would run over the lyric " + lyricWhere(score, l));
                    }
                }
            }
            sy.end = ecr;
            sy.ticks = ecr->tick() - sy.cr->tick(); // as addMelisma (notationinteraction.cpp:7908)
        }
        sy.existing = sy.cr->lyrics(verse0, placement);
        if (sy.existing && ifExists == "error") {
            return makeErr(Err::Exists, address::where(score, sy.cr) + " already has the verse " + std::to_string(verse) + " lyric \""
                           + plain(sy.existing->plainText()) + "\" (" + core::makeHandle(sy.existing)
                           + "); pass if_exists \"replace\" to change it");
        }
        if (!sy.existing) {
            if (Ret r = linkedChordRestsPresent(score, sy.cr); !r) {
                return r;
            }
        }
        prevEnd = sy.end;
    }

    // neighbours, repaired the way the UI's space / hyphen keys repair them (spec Q13):
    // the lyric before the first syllable and the one after the last
    struct Fix {
        Lyrics* l;
        LyricsSyllabic syllabic;
        bool clearMelisma;
    };
    std::vector<Fix> fixes;
    Lyrics* before = lyricBefore(first, track, verse0, placement);
    const Fraction beforeTicks = before ? before->ticks() : Fraction(0, 1);
    if (before) {
        const LyricsSyllabic s = before->syllabic();
        if (joinedToPrevious(syls.front().syllabic)) {
            // a hyphen from it (navigateToNextSyllable, notationinteraction.cpp:7040-7064)
            const LyricsSyllabic want = s == LyricsSyllabic::SINGLE ? LyricsSyllabic::BEGIN
                                        : s == LyricsSyllabic::END ? LyricsSyllabic::MIDDLE : s;
            fixes.push_back({ before, want, before->ticks().isNotZero() });
        } else if (continues(s)) {
            // a space after it (navigateToLyrics, :6781-6811)
            fixes.push_back({ before, s == LyricsSyllabic::BEGIN ? LyricsSyllabic::SINGLE : LyricsSyllabic::END, false });
        }
    }
    if (Lyrics* after = lyricAfter(syls.back().end, track, verse0, placement)) {
        const LyricsSyllabic s = after->syllabic();
        if (continues(syls.back().syllabic)) {
            if (!joinedToPrevious(s)) {
                fixes.push_back({ after, s == LyricsSyllabic::SINGLE ? LyricsSyllabic::END : LyricsSyllabic::MIDDLE, false });
            }
        } else if (joinedToPrevious(s)) {
            fixes.push_back({ after, s == LyricsSyllabic::END ? LyricsSyllabic::SINGLE : LyricsSyllabic::BEGIN, false });
        }
    }

    // 2. edit
    std::vector<std::string> adjusted;
    for (const Fix& f : fixes) {
        if (f.l->syllabic() != f.syllabic) {
            adjusted.push_back(lyricWhere(score, f.l) + ": syllabic " + syllabicName(f.l->syllabic()) + " -> " + syllabicName(f.syllabic));
            f.l->undoChangeProperty(Pid::SYLLABIC, int(f.syllabic));
        }
        if (f.clearMelisma) {
            adjusted.push_back(lyricWhere(score, f.l) + ": extender removed (a hyphen follows it now)");
            f.l->undoChangeProperty(Pid::LYRIC_TICKS, Fraction(0, 1));
        }
    }
    std::vector<Lyrics*> made;
    std::vector<std::string> statuses;
    for (Syllable& sy : syls) {
        Lyrics* l = sy.existing;
        if (l) {
            // replace: the text change TextBase::endEdit makes, plus the syllable's shape
            if (l->xmlText() != sy.xml) {
                l->undoChangeProperty(Pid::TEXT, PropertyValue(sy.xml));
            }
            l->undoChangeProperty(Pid::SYLLABIC, int(sy.syllabic));
            l->undoChangeProperty(Pid::LYRIC_TICKS, sy.ticks);
            statuses.push_back("replaced");
        } else {
            // Recipe B: Score::addText's lyric (edit.cpp:950-956) with the requested verse, shape and text
            l = Factory::createLyrics(sy.cr);
            l->setTrack(sy.cr->track());
            l->setParent(sy.cr);
            l->setProperty(Pid::VERSE, verse0); // also picks the odd/even verse style (lyrics.cpp:454-463)
            if (hasPlacement) {
                l->setPlacement(placement);
                l->setPropertyFlags(Pid::PLACEMENT, PropertyFlags::UNSTYLED);
            }
            l->setSyllabic(sy.syllabic);
            l->setTicks(sy.ticks);
            l->setXmlText(sy.xml);
            score->undoAddElement(l);
            statuses.push_back("added");
        }
        // shorten an earlier extender that runs into this syllable, as TextBase::endEdit does (textedit.cpp:250-254)
        l->adjustPrevious();
        made.push_back(l);
    }

    if (before && before->ticks() != beforeTicks && before->ticks().isNotZero()) {
        adjusted.push_back(lyricWhere(score, before) + ": extender shortened to end before \"" + syls.front().text + "\"");
    }

    // 3. check
    for (size_t k = 0; k < syls.size(); ++k) {
        const Syllable& sy = syls[k];
        Lyrics* l = made[k];
        const std::string what = "the lyric \"" + sy.text + "\" at " + address::where(score, sy.cr);
        if (Ret r = core::expect(sy.cr->lyrics(verse0, placement) == l && l->plainText() == String::fromStdString(sy.text)
                                 && l->syllabic() == sy.syllabic && l->ticks() == sy.ticks && l->verse() == verse0,
                                 what + " is not as requested"); !r) {
            return r;
        }
        if (!sy.existing) {
            if (Ret r = linkedCopiesPresent(l, expectedStaffCopies(l), what); !r) {
                return r;
            }
        }
    }

    std::vector<std::string> handles;
    std::vector<std::string> places;
    for (size_t k = 0; k < made.size(); ++k) {
        handles.push_back(core::makeHandle(made[k]));
        places.push_back(address::where(score, syls[k].cr));
    }
    const int bar1 = address::barOf(score, syls.front().cr->measure());
    const int bar2 = address::barOf(score, syls.back().end->measure());
    out["handles"] = strList(handles);
    out["placed"] = strList(places);
    out["statuses"] = strList(statuses);
    out["verse"] = Val(verse);
    if (!adjusted.empty()) {
        out["adjusted"] = strList(adjusted);
    }
    out["notation"] = Val(notationOf(score, bar1, bar2, { static_cast<int>(track / VOICES) + 1 }));
    return muse::make_ok();
}
