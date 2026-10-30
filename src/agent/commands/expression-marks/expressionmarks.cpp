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
#include "expressionmarks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <utility>

#include "engraving/dom/articulation.h"
#include "engraving/dom/barline.h"
#include "engraving/dom/breath.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/fermata.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/ornament.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tapping.h"
#include "engraving/dom/textbase.h"
#include "engraving/editing/editdata.h"
#include "engraving/types/constants.h"
#include "engraving/types/typesconv.h"

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
// ------------------------------------------------------------------ names

const std::vector<std::pair<std::string, SymId> >& articulationTable()
{
    // the master Articulations palette (palettecreator.cpp:722-762), Above forms, then the
    // NOTATION.md section 5 codes as aliases so a mark read as `+st` can be written as `st`
    static const std::vector<std::pair<std::string, SymId> > t = {
        { "accent", SymId::articAccentAbove },
        { "staccato", SymId::articStaccatoAbove },
        { "staccatissimo", SymId::articStaccatissimoAbove },
        { "tenuto", SymId::articTenutoAbove },
        { "tenuto_staccato", SymId::articTenutoStaccatoAbove },
        { "marcato", SymId::articMarcatoAbove },
        { "accent_staccato", SymId::articAccentStaccatoAbove },
        { "marcato_staccato", SymId::articMarcatoStaccatoAbove },
        { "marcato_tenuto", SymId::articMarcatoTenutoAbove },
        { "staccatissimo_stroke", SymId::articStaccatissimoStrokeAbove },
        { "staccatissimo_wedge", SymId::articStaccatissimoWedgeAbove },
        { "stress", SymId::articStressAbove },
        { "tenuto_accent", SymId::articTenutoAccentAbove },
        { "unstress", SymId::articUnstressAbove },
        { "soft_accent", SymId::articSoftAccentAbove },
        { "soft_accent_staccato", SymId::articSoftAccentStaccatoAbove },
        { "soft_accent_tenuto", SymId::articSoftAccentTenutoAbove },
        { "soft_accent_tenuto_staccato", SymId::articSoftAccentTenutoStaccatoAbove },
        { "fade_in", SymId::guitarFadeIn },
        { "fade_out", SymId::guitarFadeOut },
        { "volume_swell", SymId::guitarVolumeSwell },
        { "wiggle_sawtooth", SymId::wiggleSawtooth },
        { "wiggle_sawtooth_wide", SymId::wiggleSawtoothWide },
        { "vibrato_large_faster", SymId::wiggleVibratoLargeFaster },
        { "vibrato_large_slowest", SymId::wiggleVibratoLargeSlowest },
        { "mute_open", SymId::brassMuteOpen },
        { "mute_closed", SymId::brassMuteClosed },
        { "harmonic", SymId::stringsHarmonic },
        { "up_bow", SymId::stringsUpBow },
        { "down_bow", SymId::stringsDownBow },
        { "snap_pizzicato", SymId::pluckedSnapPizzicatoAbove },
        { "half_open", SymId::pictHalfOpen2 },
        { "st", SymId::articStaccatoAbove },
        { "sti", SymId::articStaccatissimoAbove },
        { "ten", SymId::articTenutoAbove },
        { "tst", SymId::articTenutoStaccatoAbove },
        { "ac", SymId::articAccentAbove },
        { "mar", SymId::articMarcatoAbove },
        { "up", SymId::stringsUpBow },
        { "dn", SymId::stringsDownBow },
        { "harm", SymId::stringsHarmonic },
        { "open", SymId::brassMuteOpen },
        { "stop", SymId::brassMuteClosed },
    };
    return t;
}

const std::vector<std::pair<std::string, SymId> >& ornamentTable()
{
    // the master Ornaments palette (palettecreator.cpp:790-814); shake/shake_muffat as the
    // keyboard actions use them (notationactioncontroller.cpp:469-470); NOTATION.md aliases last
    static const std::vector<std::pair<std::string, SymId> > t = {
        { "turn", SymId::ornamentTurn },
        { "turn_inverted", SymId::ornamentTurnInverted },
        { "turn_slash", SymId::ornamentTurnSlash },
        { "turn_up", SymId::ornamentTurnUp },
        { "turn_up_s", SymId::ornamentTurnUpS },
        { "trill", SymId::ornamentTrill },
        { "short_trill", SymId::ornamentShortTrill },
        { "mordent", SymId::ornamentMordent },
        { "haydn", SymId::ornamentHaydn },
        { "tremblement", SymId::ornamentTremblement },
        { "prall_mordent", SymId::ornamentPrallMordent },
        { "up_prall", SymId::ornamentUpPrall },
        { "precomp_mordent_upper_prefix", SymId::ornamentPrecompMordentUpperPrefix },
        { "up_mordent", SymId::ornamentUpMordent },
        { "down_mordent", SymId::ornamentDownMordent },
        { "prall_down", SymId::ornamentPrallDown },
        { "prall_up", SymId::ornamentPrallUp },
        { "line_prall", SymId::ornamentLinePrall },
        { "precomp_slide", SymId::ornamentPrecompSlide },
        { "shake", SymId::ornamentShake3 },
        { "shake_muffat", SymId::ornamentShakeMuffat1 },
        { "tremblement_couperin", SymId::ornamentTremblementCouperin },
        { "pince_couperin", SymId::ornamentPinceCouperin },
        { "tr", SymId::ornamentTrill },
        { "mord", SymId::ornamentMordent },
        { "imord", SymId::ornamentShortTrill },
        { "iturn", SymId::ornamentTurnInverted },
    };
    return t;
}

const std::vector<std::pair<std::string, SymId> >& fermataTable()
{
    // the master Breaths & pauses palette (palettecreator.cpp:966-974); NOTATION.md's vlong alias
    static const std::vector<std::pair<std::string, SymId> > t = {
        { "normal", SymId::fermataAbove },
        { "short", SymId::fermataShortAbove },
        { "long", SymId::fermataLongAbove },
        { "long_henze", SymId::fermataLongHenzeAbove },
        { "short_henze", SymId::fermataShortHenzeAbove },
        { "very_long", SymId::fermataVeryLongAbove },
        { "very_short", SymId::fermataVeryShortAbove },
        { "vlong", SymId::fermataVeryLongAbove },
    };
    return t;
}

const std::vector<std::pair<std::string, SymId> >& breathTable()
{
    // Breath::BREATH_LIST order (breath.cpp:39-50); NOTATION.md's br / cae aliases last
    static const std::vector<std::pair<std::string, SymId> > t = {
        { "comma", SymId::breathMarkComma },
        { "tick", SymId::breathMarkTick },
        { "salzedo", SymId::breathMarkSalzedo },
        { "upbow", SymId::breathMarkUpbow },
        { "caesura_curved", SymId::caesuraCurved },
        { "caesura", SymId::caesura },
        { "caesura_short", SymId::caesuraShort },
        { "caesura_thick", SymId::caesuraThick },
        { "chant_caesura", SymId::chantCaesura },
        { "caesura_single_stroke", SymId::caesuraSingleStroke },
        { "br", SymId::breathMarkComma },
        { "cae", SymId::caesura },
    };
    return t;
}

std::vector<std::string> namesOf(const std::vector<std::pair<std::string, SymId> >& table)
{
    std::vector<std::string> out;
    for (const auto& e : table) {
        out.push_back(e.first);
    }
    return out;
}

SymId symFor(const std::vector<std::pair<std::string, SymId> >& table, const std::string& name)
{
    for (const auto& e : table) {
        if (e.first == name) {
            return e.second;
        }
    }
    return SymId::noSym;
}

// ------------------------------------------------------------------ arguments

bool has(const Params& p, const char* key)
{
    auto it = p.find(key);
    return it != p.end() && !it->second.isNull();
}

std::string str(const Params& p, const char* key, const std::string& def = {})
{
    return has(p, key) ? p.at(key).toString() : def;
}

bool isNumber(const Val& v)
{
    return v.type() == Val::Type::Double || v.type() == Val::Type::Int || v.type() == Val::Type::Int64;
}

bool isWhole(const Val& v)
{
    return isNumber(v) && std::isfinite(v.toDouble()) && v.toDouble() == std::floor(v.toDouble());
}

// ------------------------------------------------------------------ addressing (address/address.h)

using address::barOf;
using address::beatOf;
using address::chordRestAt;
using address::where;

//! `staff`: {part: "<part name>", index: <1-based staff in the part, default 1>} or
//! {number: <1-based staff in the score, as "# sN" in score_read>}
Ret resolveStaff(MasterScore* score, const Params& p, staff_idx_t& out)
{
    if (!has(p, "staff")) {
        return makeErr(Err::InvalidArg, "missing 'staff': give a staff number, {\"part\": \"<name>\", \"index\": 1} or {\"number\": <staff>}");
    }
    return address::resolveStaff(score, p.at("staff"), out);
}

//! A position: {measure: <bar by position, pickup = 1>, beat: <1-based quarter beat>} or
//! {measure, offset: "<n>/<d>" (whole notes from the start of the bar)}
Ret resolvePosition(MasterScore* score, const Params& p, const char* key, Measure*& m, Fraction& tick)
{
    const std::string name(key);
    if (!has(p, key)) {
        return makeErr(Err::InvalidArg, "missing '" + name + "': {\"measure\": N, \"beat\": B}");
    }
    const Val& v = p.at(key);
    if (v.type() != Val::Type::Map) {
        return makeErr(Err::InvalidArg, "'" + name + "' must be an object: {\"measure\": N, \"beat\": B}");
    }
    // stricter than the shared shape, which allows the bar's start without a beat: marks go on an event
    const muse::ValMap pos = v.toMap();
    for (const auto& [k, _] : pos) {
        if (k != "measure" && k != "beat" && k != "offset") {
            return makeErr(Err::InvalidArg, "'" + name + "' has an unknown key '" + k + "' (measure, beat or offset)");
        }
    }
    if (pos.count("beat") == pos.count("offset")) {
        return makeErr(Err::InvalidArg, "'" + name + "' needs exactly one of beat or offset");
    }
    return address::resolvePosition(score, v, name, m, tick);
}

int voiceArg(const Params& p)
{
    return has(p, "voice") ? p.at("voice").toInt() : 1;
}

//! The chord or rest a command acts on: `target` (a Note, Chord or Rest handle) or
//! staff + voice + the position under `posKey`. A note means its chord.
Ret resolveTarget(MasterScore* score, const Params& p, const char* posKey, ChordRest*& cr)
{
    cr = nullptr;
    if (has(p, "target")) {
        if (has(p, "staff") || has(p, posKey) || has(p, "voice")) {
            return makeErr(Err::InvalidArg, std::string("give either target (a handle), or staff + ") + posKey + " (+ voice), not both");
        }
        if (p.at("target").type() != Val::Type::String) {
            return makeErr(Err::InvalidArg, "'target' must be a handle string from score_read");
        }
        muse::RetVal<EngravingItem*> r = core::resolveHandle(score, p.at("target").toString());
        if (!r.ret) {
            return r.ret;
        }
        EngravingItem* e = r.val;
        if (e->isNote()) {
            e = toNote(e)->chord();
        }
        if (!e->isChordRest() || (e->isRest() && toRest(e)->isGap())) {
            return makeErr(Err::InvalidArg, std::string("target must name a note, chord or rest; it names a ") + e->typeName());
        }
        cr = toChordRest(e);
        return muse::make_ok();
    }
    staff_idx_t staff = 0;
    if (Ret r = resolveStaff(score, p, staff); !r) {
        return r;
    }
    Measure* m = nullptr;
    Fraction tick;
    if (Ret r = resolvePosition(score, p, posKey, m, tick); !r) {
        return r;
    }
    const track_idx_t track = staff * VOICES + static_cast<track_idx_t>(voiceArg(p) - 1);
    return chordRestAt(score, m, tick, track, cr);
}

Ret refuseMeasureRepeat(MasterScore* score, const ChordRest* cr)
{
    if (cr->isMeasureRepeat()) {
        return makeErr(Err::InvalidArg, where(score, cr) + " is a measure repeat; write the bar out first");
    }
    return muse::make_ok();
}

//! A grace chord stands for its parent chord where the mark belongs to the beat (dynamics,
//! fermatas): the UI drops them on the CR's segment, which is the parent's.
ChordRest* mainChordRest(ChordRest* cr)
{
    if (cr->isGrace() && cr->explicitParent() && cr->explicitParent()->isChordRest()) {
        return toChordRest(cr->explicitParent());
    }
    return cr;
}

// ------------------------------------------------------------------ results

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

Val handleList(const std::vector<std::string>& hs)
{
    muse::ValList l;
    for (const std::string& h : hs) {
        l.emplace_back(Val(h));
    }
    return Val(l);
}

std::string voiceAssignmentName(const EngravingItem* e)
{
    return e->getProperty(Pid::VOICE_ASSIGNMENT).value<VoiceAssignment>() == VoiceAssignment::CURRENT_VOICE_ONLY
           ? "this_voice" : "all_voices";
}

//! Apply the voice_assignment argument to an element not yet in the score (track and
//! VOICE_ASSIGNMENT), as the UI's setInitialTrackAndVoiceAssignment does after adding
//! (engravingitem.cpp:1317-1330), but before, so linked clones get the same fields (spec M6/Q2).
void applyVoiceAssignment(EngravingItem* e, track_idx_t crTrack, const std::string& mode)
{
    if (mode == "all_voices") {
        e->setTrack(trackZeroVoice(crTrack));
        e->setProperty(Pid::VOICE_ASSIGNMENT, VoiceAssignment::ALL_VOICE_IN_INSTRUMENT);
    } else if (mode == "this_voice") {
        e->setTrack(crTrack);
        e->setProperty(Pid::VOICE_ASSIGNMENT, VoiceAssignment::CURRENT_VOICE_ONLY);
    } else { // "preference": the user's "dynamics apply to all voices" setting
        e->setInitialTrackAndVoiceAssignment(crTrack, false);
    }
}

Dynamic* dynamicAt(Segment* seg, track_idx_t track)
{
    for (EngravingItem* a : seg->annotations()) {
        if (a->isDynamic() && a->track() == track) {
            return toDynamic(a);
        }
    }
    return nullptr;
}

Fermata* fermataAt(Segment* seg, track_idx_t track)
{
    for (EngravingItem* a : seg->annotations()) {
        if (a->isFermata() && a->track() == track) {
            return toFermata(a);
        }
    }
    return nullptr;
}

bool contains(const std::vector<EngravingItem*>& v, const EngravingItem* e)
{
    return std::find(v.begin(), v.end(), e) != v.end();
}

//! Where Score::addHairpin(Hairpin*, cr1, cr2) ends a hairpin with no range selected
//! (edit.cpp:4383-4398): the end of cr2 (or of cr1 and its ties), cut short at the first
//! dynamic on cr1's track inside the last note
Fraction hairpinEnd(const ChordRest* cr1, const ChordRest* cr2)
{
    const track_idx_t track = cr1->track();
    Fraction endTick;
    if (cr2 && cr2 != cr1) {
        endTick = cr2->endTick();
    } else {
        endTick = cr1->isChord() ? toChord(cr1)->endTickIncludingTied() : cr1->endTick();
    }
    const Segment* startSegment = cr2 ? cr2->segment() : cr1->segment();
    for (const Segment* seg = startSegment; seg && seg->tick() < endTick; seg = seg->next1(Segment::CHORD_REST_OR_TIME_TICK_TYPE)) {
        if (seg == startSegment) {
            continue;
        }
        if (seg->findAnnotation(ElementType::DYNAMIC, track, track)) {
            return seg->tick();
        }
    }
    return endTick;
}

//! How many copies (the element included) Score::undoAddElement makes of e on its staff and
//! the staves linked to it (edit.cpp:6957-7036): one per linked staff with a linked voice; for
//! voice-1 dynamics and hairpins also where that voice is not linked; dynamics only on the
//! linked staves that stand for the same staff of the part (rstaff).
size_t expectedCopies(const EngravingItem* e)
{
    const Staff* ostaff = e->staff();
    const track_idx_t strack = e->track();
    size_t n = 0;
    for (const Staff* st : ostaff->staffList()) {
        if (ostaff->getLinkedTrackInStaff(st, strack) == muse::nidx
            && !(track2voice(strack) == 0 && (e->isDynamic() || e->isHairpin()))) {
            continue;
        }
        if (st != ostaff && e->isDynamic() && st->rstaff() != ostaff->rstaff()) {
            continue;
        }
        ++n;
    }
    return n;
}

//! Post-condition for linked parts: the expected number of linked copies exist, and each one
//! is in its own score where it belongs (segment annotation, spanner map, breath slot).
Ret linkedCopiesPresent(const EngravingItem* e, const std::string& what)
{
    const std::list<EngravingObject*> links = e->linkList();
    const size_t want = expectedCopies(e);
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
        if (c->isHairpin()) {
            for (const auto& [tick, sp] : c->score()->spanner()) {
                placed = placed || sp == c;
            }
        } else if (c->explicitParent() && c->explicitParent()->isSegment()) {
            const Segment* seg = toSegment(c->explicitParent());
            placed = c->isBreath() ? seg->element(c->track()) == c : contains(seg->annotations(), c);
        }
        if (!placed) {
            return core::expect(false, "a linked copy of " + what + " is not in its part");
        }
    }
    return muse::make_ok();
}
}

// ================================================================== names

const std::vector<std::string>& mu::agent::expression_marks::dynamicNames()
{
    // the TConv XML names (typesconv.cpp:1362-1431) without "other-dynamics"
    static const std::vector<std::string> n = {
        "pppppp", "ppppp", "pppp", "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff", "ffff", "fffff", "ffffff",
        "fp", "pf", "sf", "sfz", "sff", "sffz", "sfff", "sfffz", "sfp", "sfpp", "rfz", "rf", "fz", "m", "r", "s", "z", "n"
    };
    return n;
}

const std::vector<std::string>& mu::agent::expression_marks::hairpinKinds()
{
    static const std::vector<std::string> n = { "cresc", "dim", "cresc_line", "dim_line" };
    return n;
}

const std::vector<std::string>& mu::agent::expression_marks::articulationNames()
{
    static const std::vector<std::string> n = namesOf(articulationTable());
    return n;
}

const std::vector<std::string>& mu::agent::expression_marks::ornamentNames()
{
    static const std::vector<std::string> n = namesOf(ornamentTable());
    return n;
}

const std::vector<std::string>& mu::agent::expression_marks::fermataNames()
{
    static const std::vector<std::string> n = namesOf(fermataTable());
    return n;
}

const std::vector<std::string>& mu::agent::expression_marks::breathNames()
{
    static const std::vector<std::string> n = namesOf(breathTable());
    return n;
}

// ================================================================== add_dynamic

Ret mu::agent::expression_marks::addDynamic(const Params& p, MasterScore* score, Out& out)
{
    // 1. validate (nothing changes before the drop)
    ChordRest* cr = nullptr;
    if (Ret r = resolveTarget(score, p, "at", cr); !r) {
        return r;
    }
    cr = mainChordRest(cr);
    if (Ret r = refuseMeasureRepeat(score, cr); !r) {
        return r;
    }
    const bool byName = has(p, "dynamic");
    const bool byText = has(p, "text");
    if (byName == byText) {
        return makeErr(Err::InvalidArg, "give either dynamic (a name such as \"mf\") or text (such as \"subito p\")");
    }
    String xml;
    if (byName) {
        const std::string name = p.at("dynamic").toString();
        const auto& names = dynamicNames();
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            return makeErr(Err::InvalidArg, "unknown dynamic \"" + name + "\"");
        }
        xml = String::fromStdString(name);
    } else {
        std::string text = p.at("text").toString();
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
            return makeErr(Err::InvalidArg, "'text' is empty");
        }
        xml = TextBase::plainToXmlText(String::fromStdString(text));
    }
    const std::string mode = str(p, "voice_assignment", "all_voices");
    const std::string ifExists = str(p, "if_exists", "replace");
    const bool hasVelocity = has(p, "velocity");
    const int velocity = hasVelocity ? p.at("velocity").toInt() : -1;

    // the palette element (palettecreator.cpp:259-262): type and xml text from the name or text
    std::unique_ptr<Dynamic> d(Factory::createDynamic(score->dummy()->segment()));
    d->setDynamicType(xml);
    if (d->dynamicType() == DynamicType::OTHER) {
        // spec Q15: such a dynamic has no velocity and does not change playback
        return makeErr(Err::InvalidArg, "\"" + str(p, byText ? "text" : "dynamic") + "\" contains no dynamic (p, mf, sfz, ...); "
                       "for words such as \"dolce\" or \"cresc.\" use add_text with kind expression");
    }
    if (hasVelocity) {
        d->setVelocity(velocity);
    }
    applyVoiceAssignment(d.get(), cr->track(), mode);
    const track_idx_t track = d->track();
    const DynamicType wantType = d->dynamicType();
    const String wantXml = d->xmlText();
    const std::string vaName = voiceAssignmentName(d.get());

    Segment* seg = cr->segment();
    Dynamic* old = dynamicAt(seg, track);
    if (old && ifExists == "error") {
        return makeErr(Err::Exists, where(score, cr) + " already has a dynamic (" + std::string(TConv::toXml(old->dynamicType()).ascii())
                       + ", " + core::makeHandle(old) + "); pass if_exists \"replace\" to change it");
    }

    // 2. edit
    Dynamic* result = nullptr;
    std::string status;
    if (old) {
        const bool sameText = old->dynamicType() == wantType && old->xmlText() == wantXml;
        const bool sameVelocity = !hasVelocity || old->velocity() == velocity;
        if (sameText && sameVelocity) {
            status = "unchanged";
        } else {
            // velocity first: getProperty(VELOCITY) is the effective velocity (dynamic.cpp:450), so
            // after a type change the undo record would hold the new type's default instead
            if (!sameVelocity) {
                old->undoChangeProperty(Pid::VELOCITY, velocity);
            }
            if (!sameText) {
                // the palette dropped on a dynamic (dynamic.cpp:318-325): type, then text; deletes d
                EditData ed;
                ed.dropElement = d.release();
                old->drop(ed);
            }
            status = "replaced";
        }
        result = old;
    } else {
        // ChordRest::drop DYNAMIC (chordrest.cpp:285-290): track, voice-assignment check, segment, undoAddElement
        Dynamic* nd = d.release();
        EditData ed;
        ed.dropElement = nd;
        cr->drop(ed);
        result = nd;
        status = "added";
    }

    // 3. check
    const bool inSegment = contains(seg->annotations(), result);
    if (Ret r = core::expect(inSegment && result->track() == track && result->dynamicType() == wantType
                             && result->xmlText() == wantXml && (!hasVelocity || result->velocity() == velocity),
                             "the dynamic at " + where(score, cr) + " is not the one requested"); !r) {
        return r;
    }
    if (status == "added") {
        if (Ret r = linkedCopiesPresent(result, "the dynamic at " + where(score, cr)); !r) {
            return r;
        }
    }

    const int bar = barOf(score, cr->measure());
    out["handle"] = Val(core::makeHandle(result));
    out["status"] = Val(status);
    out["dynamic"] = Val(std::string(TConv::toXml(result->dynamicType()).ascii()));
    out["voiceAssignment"] = Val(status == "added" ? vaName : voiceAssignmentName(result));
    out["notation"] = Val(notationOf(score, bar, bar, { static_cast<int>(cr->staffIdx()) + 1 }));
    return muse::make_ok();
}

// ================================================================== add_hairpin

Ret mu::agent::expression_marks::addHairpin(const Params& p, MasterScore* score, Out& out)
{
    // 1. validate
    const std::string kind = str(p, "kind");
    HairpinType type;
    if (kind == "cresc") {
        type = HairpinType::CRESC_HAIRPIN;
    } else if (kind == "dim") {
        type = HairpinType::DIM_HAIRPIN;
    } else if (kind == "cresc_line") {
        type = HairpinType::CRESC_LINE;
    } else if (kind == "dim_line") {
        type = HairpinType::DIM_LINE;
    } else {
        return makeErr(Err::InvalidArg, "kind must be cresc, dim, cresc_line or dim_line");
    }
    staff_idx_t staff = 0;
    if (Ret r = resolveStaff(score, p, staff); !r) {
        return r;
    }
    const track_idx_t track = staff * VOICES + static_cast<track_idx_t>(voiceArg(p) - 1);
    Measure* m1 = nullptr;
    Measure* m2 = nullptr;
    Fraction t1;
    Fraction t2;
    if (Ret r = resolvePosition(score, p, "from", m1, t1); !r) {
        return r;
    }
    if (Ret r = resolvePosition(score, p, "to", m2, t2); !r) {
        return r;
    }
    ChordRest* cr1 = nullptr;
    ChordRest* cr2 = nullptr;
    if (Ret r = chordRestAt(score, m1, t1, track, cr1); !r) {
        return r;
    }
    if (Ret r = chordRestAt(score, m2, t2, track, cr2); !r) {
        return r;
    }
    if (Ret r = refuseMeasureRepeat(score, cr1); !r) {
        return r;
    }
    if (Ret r = refuseMeasureRepeat(score, cr2); !r) {
        return r;
    }
    if (cr2->tick() < cr1->tick()) {
        // proposed E_EMPTY_SPAN (spec M10)
        return makeErr(Err::InvalidArg, "'to' (" + where(score, cr2) + ") is before 'from' (" + where(score, cr1)
                       + "); a hairpin runs from its first note to its last");
    }
    const std::string mode = str(p, "voice_assignment", "all_voices");

    // the hairpin, with type, line texts (edit.cpp:4338-4346) and voice assignment set before it
    // is added, so linked clones get them too
    std::unique_ptr<Hairpin> h(Factory::createHairpin(score->dummy()->segment()));
    h->setHairpinType(type);
    if (type == HairpinType::CRESC_LINE) {
        h->setBeginText(u"cresc.");
        h->setContinueText(u"(cresc.)");
    } else if (type == HairpinType::DIM_LINE) {
        h->setBeginText(u"dim.");
        h->setContinueText(u"(dim.)");
    }
    applyVoiceAssignment(h.get(), cr1->track(), mode);
    const VoiceAssignment va = h->voiceAssignment();
    const track_idx_t finalTrack = h->track();

    // spec Q5: overlapping hairpins on one track are refused (with the end the add will use)
    const Fraction start = cr1->tick();
    const Fraction end = hairpinEnd(cr1, cr2);
    for (const auto& iv : score->spannerMap().findOverlapping(start.ticks(), end.ticks())) {
        const Spanner* sp = iv.value;
        if (sp->isHairpin() && sp->track() == finalTrack && sp->tick() < end && sp->tick2() > start) {
            return makeErr(Err::Exists, "a hairpin already covers part of that span on this staff and voice ("
                           + core::makeHandle(sp) + "); remove it first or start this one where it ends");
        }
    }

    // 2. edit: Score::addHairpin(Hairpin*, cr1, cr2) (edit.cpp:4374-4407), as the add-hairpin
    // action does; it reads the selection (a range would replace the end), so clear it first
    score->deselectAll();
    Hairpin* hp = h.release();
    score->addHairpin(hp, cr1, cr2);
    // the add makes the linked copies on cr1's voice: check them before the move below
    if (Ret r = linkedCopiesPresent(hp, "the hairpin from " + where(score, cr1)); !r) {
        return r;
    }
    if (va == VoiceAssignment::ALL_VOICE_IN_INSTRUMENT && hp->voice() != 0) {
        // edit.cpp:4325-4327: an all-voices hairpin moves to voice 1, as in the UI. Only the
        // master moves (a plain setTrack, not undoable): the part copies keep cr1's voice track;
        // only the voice assignment, set before the add, reached them.
        hp->setTrack(trackZeroVoice(hp->track()));
    }

    // 3. check (model fields only: no segments until layout at commit, spec F2)
    bool found = false;
    for (const auto& iv : score->spannerMap().findOverlapping(hp->tick().ticks(), hp->tick2().ticks())) {
        found = found || iv.value == hp;
    }
    if (Ret r = core::expect(found && hp->tick() == start && hp->tick2() == end && hp->tick2() > hp->tick() && hp->track() == finalTrack
                             && hp->hairpinType() == type, "the hairpin was not added from " + where(score, cr1)); !r) {
        return r;
    }

    const int bar1 = barOf(score, cr1->measure());
    const int bar2 = barOf(score, cr2->measure());
    Measure* endBar = score->tick2measure(hp->tick2() - Fraction(1, Constants::DIVISION * 4));
    out["handle"] = Val(core::makeHandle(hp));
    out["start"] = Val(where(score, cr1));
    out["end"] = Val(endBar ? ("m" + std::to_string(barOf(score, endBar)) + " " + beatOf(endBar, hp->tick2())
                               + (hp->tick2() == endBar->endTick() ? " (the end of the bar)" : "")) : std::string());
    out["voiceAssignment"] = Val(std::string(va == VoiceAssignment::CURRENT_VOICE_ONLY ? "this_voice" : "all_voices"));
    out["notation"] = Val(notationOf(score, bar1, std::max(bar2, endBar ? barOf(score, endBar) : bar2),
                                     { static_cast<int>(staff) + 1 }));
    return muse::make_ok();
}

// ================================================================== add_articulation / add_ornament

namespace {
//! Chords for an articulation or ornament: the target, or every chord from `at` through
//! `through` (inclusive, by start tick) on that staff and voice; rests in a span are skipped.
Ret chordsFor(MasterScore* score, const Params& p, const char* what, std::vector<Chord*>& chords)
{
    ChordRest* cr = nullptr;
    if (Ret r = resolveTarget(score, p, "at", cr); !r) {
        return r;
    }
    if (!has(p, "through")) {
        if (Ret r = refuseMeasureRepeat(score, cr); !r) {
            return r;
        }
        if (!cr->isChord()) {
            // proposed E_NOT_A_CHORD (spec M10); every UI path ignores rests
            return makeErr(Err::InvalidArg, where(score, cr) + " is a rest; " + what + " go on notes");
        }
        chords.push_back(toChord(cr));
        return muse::make_ok();
    }
    if (has(p, "target")) {
        return makeErr(Err::InvalidArg, "'through' works with staff + at, not with target");
    }
    Measure* m2 = nullptr;
    Fraction t2;
    if (Ret r = resolvePosition(score, p, "through", m2, t2); !r) {
        return r;
    }
    if (t2 < cr->tick()) {
        return makeErr(Err::InvalidArg, "'through' is before 'at'");
    }
    const track_idx_t track = cr->track();
    for (Segment* s = cr->segment(); s && s->tick() <= t2; s = s->next1(SegmentType::ChordRest)) {
        EngravingItem* e = s->element(track);
        if (e && e->isChord()) {
            chords.push_back(toChord(e));
        }
    }
    if (chords.empty()) {
        return makeErr(Err::InvalidArg, std::string("there are no notes from ") + where(score, cr) + " through that position");
    }
    return muse::make_ok();
}

//! One mark (articulation or ornament) on each chord, with the palette's semantics: add, never
//! remove; a chord that already has it is left alone (spec Q6). Chord::drop (chord.cpp:1473-1525).
Ret addChordMarks(const Params& p, MasterScore* score, mu::agent::expression_marks::Out& out, bool ornament)
{
    const char* what = ornament ? "ornaments" : "articulations";
    const std::string name = str(p, ornament ? "ornament" : "articulation");
    const SymId sym = symFor(ornament ? ornamentTable() : articulationTable(), name);
    if (sym == SymId::noSym) {
        return makeErr(Err::InvalidArg, std::string("unknown ") + (ornament ? "ornament" : "articulation") + " \"" + name + "\"");
    }
    std::vector<Chord*> chords;
    if (Ret r = chordsFor(score, p, what, chords); !r) {
        return r;
    }

    auto make = [&]() -> Articulation* {
        Articulation* a = ornament ? Factory::createOrnament(score->dummy()->chord())
                          : Factory::createArticulation(score->dummy()->chord());
        a->setSymId(sym);
        return a;
    };
    // the parts the mark must leave on the chord: itself, or for a combined mark added to a
    // chord that already has marks, its parts (Score::toggleArticulation splits it, cmd.cpp:2473-2487)
    std::unique_ptr<Articulation> probe(make());
    const bool combined = probe->isDouble();
    const std::set<SymId> parts = combined ? splitArticulations({ sym }) : std::set<SymId> { sym };
    auto hasSym = [&](Chord* c, SymId id) {
        std::unique_ptr<Articulation> a(make());
        a->setSymId(id);
        return c->hasArticulation(a.get()) != nullptr;
    };

    // validate every chord before changing any
    for (Chord* c : chords) {
        if (c->tapping() && !c->hasArticulation(probe.get())) {
            // spec Q8: Score::toggleArticulation removes a tapping when anything else is added (cmd.cpp:2459-2464)
            return makeErr(Err::Refused, where(score, c) + " has a tapping mark, which MuseScore would remove "
                           "when another mark is added; remove the tapping first if that is what you want");
        }
    }

    std::vector<std::string> handles;
    int added = 0;
    int unchanged = 0;
    for (Chord* c : chords) {
        if (Articulation* existing = c->hasArticulation(probe.get())) {
            ++unchanged;
            handles.push_back(core::makeHandle(existing));
            continue;
        }
        const size_t before = c->articulations().size();
        Articulation* a = make();
        EditData ed;
        ed.dropElement = a;
        c->drop(ed);
        const bool inChord = std::find(c->articulations().begin(), c->articulations().end(), a) != c->articulations().end();
        if (!inChord) {
            delete a; // split into parts: the combined original is neither added nor freed upstream
            a = nullptr;
        }
        // check: the mark (or every part of a split combined mark) is on the chord now
        bool ok = inChord;
        if (!ok) {
            ok = true;
            for (SymId id : parts) {
                ok = ok && hasSym(c, id);
            }
        }
        if (Ret r = core::expect(ok, std::string(ornament ? "the ornament" : "the articulation") + " was not added at "
                                 + where(score, c)); !r) {
            return r;
        }
        if (c->articulations().size() > before) {
            ++added;
        } else {
            ++unchanged;
        }
        if (a) {
            handles.push_back(core::makeHandle(a));
        } else {
            for (SymId id : parts) {
                std::unique_ptr<Articulation> q(make());
                q->setSymId(id);
                handles.push_back(core::makeHandle(c->hasArticulation(q.get())));
            }
        }
    }

    const int bar1 = barOf(score, chords.front()->measure());
    const int bar2 = barOf(score, chords.back()->measure());
    out["handles"] = handleList(handles);
    out["added"] = Val(added);
    out["unchanged"] = Val(unchanged);
    out["notation"] = Val(notationOf(score, bar1, bar2, { static_cast<int>(chords.front()->staffIdx()) + 1 }));
    return muse::make_ok();
}
}

Ret mu::agent::expression_marks::addArticulation(const Params& p, MasterScore* score, Out& out)
{
    return addChordMarks(p, score, out, false);
}

Ret mu::agent::expression_marks::addOrnament(const Params& p, MasterScore* score, Out& out)
{
    return addChordMarks(p, score, out, true);
}

// ================================================================== add_fermata

Ret mu::agent::expression_marks::addFermata(const Params& p, MasterScore* score, Out& out)
{
    // 1. validate
    const std::string name = str(p, "fermata", "normal");
    const SymId sym = symFor(fermataTable(), name);
    if (sym == SymId::noSym) {
        return makeErr(Err::InvalidArg, "unknown fermata \"" + name + "\"");
    }
    const bool hasStretch = has(p, "time_stretch");
    const double stretch = hasStretch ? p.at("time_stretch").toDouble() : 0.0;
    if (hasStretch && !(stretch > 0.0)) {
        return makeErr(Err::InvalidArg, "time_stretch must be greater than 0");
    }
    const bool allStaves = has(p, "all_staves") && p.at("all_staves").toBool();
    const bool onBarline = has(p, "barline");

    // where each fermata goes: a chord/rest, or an end barline; with the staff's track
    struct Place {
        EngravingItem* target = nullptr;
        Segment* segment = nullptr;
        track_idx_t track = 0;
    };
    std::vector<Place> places;
    std::vector<int> skipped;
    int bar = 0;

    if (onBarline) {
        if (has(p, "at") || has(p, "target") || has(p, "voice")) {
            return makeErr(Err::InvalidArg, "give barline (a bar number) or at/target, not both");
        }
        if (!isWhole(p.at("barline"))) {
            return makeErr(Err::InvalidArg, "'barline' must be the number of the bar whose end barline gets the fermata");
        }
        bar = p.at("barline").toInt();
        const std::vector<Measure*> bars = allMeasures(score);
        if (bar < 1 || bar > static_cast<int>(bars.size())) {
            return makeErr(Err::Range, "bar " + std::to_string(bar) + " is outside the score (1-" + std::to_string(bars.size()) + ")");
        }
        Measure* m = bars.at(bar - 1);
        Segment* seg = m->findSegment(SegmentType::EndBarLine, m->endTick());
        std::vector<staff_idx_t> staves;
        if (allStaves) {
            if (has(p, "staff")) {
                return makeErr(Err::InvalidArg, "all_staves puts one on every staff; leave out staff");
            }
            for (staff_idx_t s = 0; s < score->nstaves(); ++s) {
                staves.push_back(s);
            }
        } else {
            staff_idx_t s = 0;
            if (Ret r = resolveStaff(score, p, s); !r) {
                return r;
            }
            staves.push_back(s);
        }
        for (staff_idx_t s : staves) {
            EngravingItem* bl = seg ? seg->element(s * VOICES) : nullptr;
            if (!bl || !bl->isBarLine()) {
                // proposed E_NO_BARLINE (spec M10); BarLine::acceptDrop takes fermatas on end barlines only (barline.cpp:379-380)
                return makeErr(Err::Range, "m" + std::to_string(bar) + " has no end barline on staff " + std::to_string(s + 1));
            }
            places.push_back({ bl, seg, s * VOICES });
        }
    } else if (allStaves) {
        if (has(p, "target") || has(p, "staff")) {
            return makeErr(Err::InvalidArg, "all_staves takes at (+ voice), not staff or target");
        }
        Measure* m = nullptr;
        Fraction tick;
        if (Ret r = resolvePosition(score, p, "at", m, tick); !r) {
            return r;
        }
        bar = barOf(score, m);
        const track_idx_t voice = static_cast<track_idx_t>(voiceArg(p) - 1);
        for (staff_idx_t s = 0; s < score->nstaves(); ++s) {
            ChordRest* cr = nullptr;
            if (!chordRestAt(score, m, tick, s * VOICES + voice, cr) || cr->isMeasureRepeat()) {
                skipped.push_back(static_cast<int>(s) + 1); // nothing starts there on that staff (a held note, say)
                continue;
            }
            places.push_back({ cr, cr->segment(), cr->track() });
        }
        if (places.empty()) {
            return makeErr(Err::Range, "no staff has a note or rest starting at m" + std::to_string(bar) + " " + beatOf(m, tick)
                           + " in voice " + std::to_string(voice + 1));
        }
    } else {
        ChordRest* cr = nullptr;
        if (Ret r = resolveTarget(score, p, "at", cr); !r) {
            return r;
        }
        cr = mainChordRest(cr);
        if (Ret r = refuseMeasureRepeat(score, cr); !r) {
            return r;
        }
        bar = barOf(score, cr->measure());
        places.push_back({ cr, cr->segment(), cr->track() });
    }

    // 2. edit: ChordRest::drop FERMATA (chordrest.cpp:257-283) / BarLine::drop (barline.cpp:458-477)
    std::unique_ptr<Fermata> probe(Factory::createFermata(score->dummy()->segment()));
    probe->setSymIdAndTimeStretch(sym);
    const int wantSubtype = probe->subtype();

    std::vector<std::string> handles;
    std::vector<int> staves;
    std::set<std::string> kinds; // what happened on each staff: added, replaced, changed (stretch only)
    auto note = [&](const std::string& s) { kinds.insert(s); };
    for (const Place& pl : places) {
        staves.push_back(static_cast<int>(pl.track / VOICES) + 1);
        Fermata* existing = fermataAt(pl.segment, pl.track);
        if (existing && existing->subtype() == wantSubtype) {
            if (hasStretch && existing->timeStretch() != stretch) {
                existing->undoChangeProperty(Pid::TIME_STRETCH, stretch);
                note("changed");
            }
        } else {
            Fermata* f = Factory::createFermata(score->dummy()->segment());
            f->setSymIdAndTimeStretch(sym); // once only: a second call resets the stretch to -1 (fermata.cpp:264)
            if (hasStretch) {
                f->setTimeStretch(stretch);
            }
            EditData ed;
            ed.dropElement = f;
            pl.target->drop(ed);
            note(existing ? "replaced" : "added");
        }
        // 3. check
        Fermata* now = fermataAt(pl.segment, pl.track);
        if (Ret r = core::expect(now && now->subtype() == wantSubtype && now->timeStretch() > 0
                                 && (!hasStretch || now->timeStretch() == stretch),
                                 "no " + name + " fermata on staff " + std::to_string(pl.track / VOICES + 1) + " afterwards"); !r) {
            return r;
        }
        handles.push_back(core::makeHandle(now));
    }

    out["handles"] = handleList(handles);
    out["status"] = Val(kinds.empty() ? std::string("unchanged") : kinds.size() == 1 ? *kinds.begin() : std::string("changed"));
    if (!skipped.empty()) {
        muse::ValList l;
        for (int s : skipped) {
            l.emplace_back(Val(s));
        }
        out["skippedStaves"] = Val(l);
    }
    out["notation"] = Val(notationOf(score, bar, bar, staves));
    return muse::make_ok();
}

// ================================================================== add_breath

Ret mu::agent::expression_marks::addBreath(const Params& p, MasterScore* score, Out& out)
{
    // 1. validate
    const std::string name = str(p, "breath", "comma");
    const SymId sym = symFor(breathTable(), name);
    if (sym == SymId::noSym) {
        return makeErr(Err::InvalidArg, "unknown breath mark \"" + name + "\"");
    }
    double pause = 0.0;
    for (const BreathType& bt : Breath::BREATH_LIST) {
        if (bt.id == sym) {
            pause = bt.pause; // the palette sets it explicitly (palettecreator.cpp:989)
        }
    }
    if (has(p, "pause")) {
        pause = p.at("pause").toDouble();
        if (!std::isfinite(pause) || pause < 0.0) {
            return makeErr(Err::InvalidArg, "pause must be 0 or more seconds");
        }
    }
    ChordRest* cr = nullptr;
    if (Ret r = resolveTarget(score, p, "after", cr); !r) {
        return r;
    }
    if (cr->isGrace()) {
        // proposed E_GRACE (spec M10, Q9): a grace note's end tick is not "after" it musically
        return makeErr(Err::InvalidArg, "a breath mark cannot follow a grace note; put it after the main note");
    }
    if (Ret r = refuseMeasureRepeat(score, cr); !r) {
        return r;
    }
    const track_idx_t track = cr->track();
    Measure* m = cr->measure();
    const Fraction endTick = cr->endTick();

    // 2. edit
    std::string status;
    Segment* seg = m->findSegment(SegmentType::Breath, endTick);
    EngravingItem* existing = seg ? seg->element(track) : nullptr;
    if (existing && existing->isBreath()) {
        // spec Q9: never a second breath in the slot (Segment::add would orphan the first,
        // segment.cpp:830-837); change the existing one's symbol and pause instead
        Breath* b = toBreath(existing);
        status = "unchanged";
        if (b->symId() != sym) {
            b->undoChangeProperty(Pid::SYMBOL, sym);
            status = "replaced";
        }
        if (b->pause() != pause) {
            b->undoChangeProperty(Pid::PAUSE, pause);
            status = "replaced";
        }
    } else {
        // ChordRest::drop BREATH (chordrest.cpp:197-211): at the CR's end tick, in its measure
        Breath* b = Factory::createBreath(score->dummy()->segment());
        b->setSymId(sym);
        b->setPause(pause);
        EditData ed;
        ed.dropElement = b;
        cr->drop(ed);
        status = "added";
    }

    // 3. check
    seg = m->findSegment(SegmentType::Breath, endTick);
    EngravingItem* now = seg ? seg->element(track) : nullptr;
    if (Ret r = core::expect(now && now->isBreath() && toBreath(now)->symId() == sym && toBreath(now)->pause() == pause,
                             "no breath mark after " + where(score, cr) + " afterwards"); !r) {
        return r;
    }
    if (status == "added") {
        if (Ret r = linkedCopiesPresent(now, "the breath mark after " + where(score, cr)); !r) {
            return r;
        }
    }

    const int bar = barOf(score, m);
    out["handle"] = Val(core::makeHandle(now));
    out["status"] = Val(status);
    out["notation"] = Val(notationOf(score, bar, bar, { static_cast<int>(cr->staffIdx()) + 1 }));
    return muse::make_ok();
}
