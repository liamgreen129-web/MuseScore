#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Reference printer: music21 score -> MuseScore Agent compact notation (docs/agent/NOTATION.md).
#
# Used to produce NOTATION.md's worked examples from real scores, and later as an
# independent oracle for the C++ printer (P1.4): both read the same MusicXML, so their
# outputs can be compared. Run with the MuseClaude venv (music21):
#   ~/.venvs/MuseClaude/bin/python -I tools/notation_ref/m21_print.py <file|corpus:name> [--bars 1-4] [--staves 1,2]
import argparse
import sys
from fractions import Fraction

from music21 import (articulations, bar, chord, clef, converter, corpus, dynamics, expressions,
                     harmony, key, meter, note, repeat, spanner, stream, tempo)

DUR = {"breve": "b", "whole": "w", "half": "h", "quarter": "q", "eighth": "e", "16th": "s",
       "32nd": "t", "64th": "x", "128th": "y"}

ARTIC = [  # (music21 class, code) in the canonical order of NOTATION.md section 5
    (articulations.Staccatissimo, "sti"), (articulations.Staccato, "st"), (articulations.Tenuto, "ten"),
    (articulations.StrongAccent, "mar"), (articulations.Accent, "ac"), (articulations.UpBow, "up"),
    (articulations.DownBow, "dn"), (articulations.Harmonic, "harm"), (articulations.OpenString, "open"),
    (articulations.Stopped, "stop"),
]


def pitch_name(p):
    acc = ""
    if p.accidental is not None:
        acc = {"sharp": "#", "double-sharp": "##", "flat": "b", "double-flat": "bb", "natural": ""}.get(p.accidental.name, "")
    return f"{p.step}{acc}{p.octave}"


def dur_code(d):
    base = DUR.get(d.type)
    if base is None:
        raise ValueError(f"unsupported duration type {d.type}")
    return base + "." * d.dots


def clef_name(c):
    return {"TrebleClef": "treble", "BassClef": "bass", "AltoClef": "alto", "TenorClef": "tenor",
            "Treble8vbClef": "treble8vb", "PercussionClef": "perc", "TabClef": "tab"}.get(type(c).__name__, type(c).__name__)


def key_name(k):
    if k is None:
        return None
    if isinstance(k, key.Key):
        t = k.tonic.name.replace("-", "b")
        return t if k.mode == "major" else t.lower()
    return key.KeySignature(k.sharps).asKey("major").tonic.name.replace("-", "b")


def time_name(ts):
    if ts.symbol == "common":
        return "C"
    if ts.symbol == "cut":
        return "C/"
    return ts.ratioString


class Spanners:
    """Start/stop marks for slurs, hairpins and lines, keyed by element id."""

    def __init__(self, part):
        self.open_paren, self.close_paren = {}, {}
        self.start_marks, self.end_marks = {}, {}
        for sp in part.recurse().getElementsByClass(spanner.Spanner):
            first, last = sp.getFirst(), sp.getLast()
            if first is None or last is None:
                continue
            # MusicXML import can pair a spanner's start and stop across the piece (reused
            # slur numbers); a spanner that runs backwards is an import artefact, not music.
            fm, lm = first.getContextByClass("Measure"), last.getContextByClass("Measure")
            if fm is None or lm is None or (fm.number, first.offset) > (lm.number, last.offset):
                continue
            if isinstance(sp, spanner.Slur):
                self.open_paren[id(first)] = self.open_paren.get(id(first), 0) + 1
                self.close_paren[id(last)] = self.close_paren.get(id(last), 0) + 1
            elif isinstance(sp, dynamics.Crescendo):
                self.start_marks.setdefault(id(first), []).append("<")
                self.end_marks.setdefault(id(last), []).append("!")
            elif isinstance(sp, dynamics.Diminuendo):
                self.start_marks.setdefault(id(first), []).append(">")
                self.end_marks.setdefault(id(last), []).append("!")
            elif isinstance(sp, spanner.Ottava):
                code = {"8va": "8va", "8vb": "8vb", "15ma": "15ma", "15mb": "15mb"}.get(sp.type, "8va")
                self.start_marks.setdefault(id(first), []).append(code + "[")
                self.end_marks.setdefault(id(last), []).append(code + "]")
            elif isinstance(sp, expressions.TrillExtension):
                self.start_marks.setdefault(id(first), []).append("trl[")
                self.end_marks.setdefault(id(last), []).append("trl]")


def marks_for(n, sp, pending_dyn):
    out = []
    for cls, code in ARTIC:
        if any(isinstance(a, cls) for a in n.articulations):
            if code == "st" and any(isinstance(a, articulations.Tenuto) for a in n.articulations):
                continue  # emitted as tst below
            if code == "ten" and any(isinstance(a, articulations.Staccato) for a in n.articulations):
                out.append("tst")
                continue
            out.append(code)
    for e in n.expressions:
        if isinstance(e, expressions.Fermata):
            out.append("ferm")
        elif isinstance(e, expressions.InvertedMordent):
            out.append("imord")
        elif isinstance(e, expressions.Mordent):
            out.append("mord")
        elif isinstance(e, expressions.InvertedTurn):
            out.append("iturn")
        elif isinstance(e, expressions.Turn):
            out.append("turn")
        elif isinstance(e, expressions.Trill):
            out.append("tr")
    out += pending_dyn
    out += sp.start_marks.get(id(n), [])
    out += sp.end_marks.get(id(n), [])
    for ly in n.lyrics:
        syl = ly.text or ""
        if ly.syllabic in ("begin", "middle"):
            syl += "-"
        num = "" if ly.number in (None, 1) else str(ly.number)
        out.append(f'ly{num}"{syl}"')
    return "".join("+" + m for m in out)


def event_text(n, sp, pending_dyn):
    if isinstance(n, note.Rest):
        body = "r:" + dur_code(n.duration)
    elif isinstance(n, chord.Chord):
        ps = []
        for p_note in sorted(n.notes, key=lambda x: x.pitch.ps):
            t = pitch_name(p_note.pitch)
            if p_note.tie is not None and p_note.tie.type in ("start", "continue"):
                t += "~"
            ps.append(t)
        body = "[" + ",".join(ps) + "]:" + dur_code(n.duration)
    else:
        body = pitch_name(n.pitch) + ":" + dur_code(n.duration)
        if n.tie is not None and n.tie.type in ("start", "continue"):
            body += "~"
    return "(" * sp.open_paren.get(id(n), 0) + body + marks_for(n, sp, pending_dyn) + ")" * sp.close_paren.get(id(n), 0)


def voice_items(container, sp, bar_len):
    """Tokens for one voice of one bar, with tuplets, grace notes and gaps."""
    tokens, grace_buf, tuplet_stack = [], [], []
    dyn_by_offset = {}
    for d in container.getElementsByClass(dynamics.Dynamic):
        dyn_by_offset.setdefault(Fraction(d.offset).limit_denominator(1024), []).append(d.value)
    pos = Fraction(0)

    def emit(tok):
        if tuplet_stack:
            tuplet_stack[-1][1].append(tok)
        else:
            tokens.append(tok)

    for n in container.notesAndRests:
        off = Fraction(n.offset).limit_denominator(1024)
        if n.duration.isGrace:
            slash = isinstance(n.duration, type(n.duration)) and getattr(n.duration, "slash", False)
            grace_buf.append((slash, event_text(n, sp, [])))
            continue
        if off > pos and not tuplet_stack:
            emit("_:" + gap_code(off - pos))
        pending = dyn_by_offset.pop(off, [])
        tok = event_text(n, sp, pending)
        if grace_buf:
            slash = grace_buf[0][0]
            tok = ("g/(" if slash else "g(") + " ".join(t for _, t in grace_buf) + ") " + tok
            grace_buf = []
        tups = n.duration.tuplets
        if tups and tups[0].type == "start":
            tuplet_stack.append((tups[0], []))
        emit(tok)
        if tups and tups[0].type == "stop" and tuplet_stack:
            t, inner = tuplet_stack.pop()
            emit("{%d:%d %s}" % (t.numberNotesActual, t.numberNotesNormal, " ".join(inner)))
        pos = off + Fraction(n.duration.quarterLength).limit_denominator(1024)
    if pos < bar_len:
        emit("_:" + gap_code(bar_len - pos))
    return tokens


def gap_code(ql):
    table = {Fraction(8): "b", Fraction(4): "w", Fraction(3): "h.", Fraction(2): "h", Fraction(3, 2): "q.",
             Fraction(1): "q", Fraction(3, 4): "e.", Fraction(1, 2): "e", Fraction(1, 4): "s", Fraction(1, 8): "t"}
    if ql in table:
        return table[ql]
    # split into the largest standard values
    for v in sorted(table, reverse=True):
        if v < ql:
            return table[v] + " _:" + gap_code(ql - v)
    raise ValueError(f"cannot express gap {ql}")


def is_full_bar_rest(m, voice):
    nr = list(voice.notesAndRests)
    return len(nr) == 1 and isinstance(nr[0], note.Rest) and (nr[0].fullMeasure in (True, "always") or
                                                               Fraction(nr[0].duration.quarterLength) == Fraction(m.barDuration.quarterLength))


def bar_attrs(m, prev, first):
    a = []
    ts = m.timeSignature
    if ts is not None and (first or prev.get("time") != time_name(ts)):
        a.append("time=" + time_name(ts)); prev["time"] = time_name(ts)
    ks = m.keySignature
    if ks is not None and (first or prev.get("key") != key_name(ks)):
        a.append("key=" + key_name(ks)); prev["key"] = key_name(ks)
    cl = m.clef
    if cl is not None and not first and prev.get("clef") != clef_name(cl):
        a.append("clef=" + clef_name(cl)); prev["clef"] = clef_name(cl)
    for mm in m.getElementsByClass(tempo.MetronomeMark):
        txt = f'"{mm.text}"' if mm.text else ""
        if mm.number:
            a.append(f"tempo={txt}(q={int(mm.number)})")
        elif txt:
            a.append(f"tempo={txt}")
    for rm in m.getElementsByClass(expressions.RehearsalMark):
        a.append("mark=" + rm.content)
    lb, rb = m.leftBarline, m.rightBarline
    if isinstance(lb, bar.Repeat) and lb.direction == "start":
        a.append("repeat=start")
    if isinstance(rb, bar.Repeat) and rb.direction == "end":
        a.append("repeat=end" + (f":{rb.times}" if rb.times and rb.times != 2 else ""))
    elif rb is not None and rb.type in ("double", "final", "dashed", "dotted", "none", "light-light", "light-heavy"):
        a.append("barline=" + {"light-light": "double", "light-heavy": "final"}.get(rb.type, rb.type))
    real_len = Fraction(m.duration.quarterLength).limit_denominator(1024)
    nominal = Fraction(m.barDuration.quarterLength).limit_denominator(1024)
    if real_len and real_len != nominal:
        a.append(f"len={real_len / 4}")
    return a


def print_score(s, bars=None, staves=None, header_comment=None, names=None):
    s = s.toSoundingPitch()  # NOTATION.md section 2: pitch is what sounds
    out = []
    if header_comment:
        out.append(f"% {header_comment}")
    parts = list(s.parts)
    for si, part in enumerate(parts, 1):
        if staves and si not in staves:
            continue
        sp = Spanners(part)
        measures = list(part.getElementsByClass(stream.Measure))
        inst = part.getInstrument(returnDefault=False)
        name = (part.partName or (inst.instrumentName if inst else None) or f"Staff {si}").strip()
        if names and si <= len(names):
            name = names[si - 1]
        first_clef = measures[0].clef if measures and measures[0].clef else part.recurse().getElementsByClass(clef.Clef).first()
        first_key = part.recurse().getElementsByClass(key.KeySignature).first()
        first_time = part.recurse().getElementsByClass(meter.TimeSignature).first()
        hdr = [f'# s{si} "{name}"', "clef=" + (clef_name(first_clef) if first_clef else "treble")]
        if inst is not None and inst.transposition is not None and inst.transposition.semitones != 0:
            dn = inst.transposition.directedName  # music21 writes "M-2"; NOTATION.md writes "-M2"
            hdr.append("transposes=" + ("-" + dn.replace("-", "") if "-" in dn else dn))
        if first_key is not None:
            hdr.append("key=" + key_name(first_key))
        if first_time is not None:
            hdr.append("time=" + time_name(first_time))
        out.append(" ".join(hdr))
        prev = {"time": hdr[-1].split("=", 1)[1] if first_time else None, "key": key_name(first_key) if first_key else None,
                "clef": clef_name(first_clef) if first_clef else None}
        for idx, m in enumerate(measures, 1):
            if bars and not (bars[0] <= idx <= bars[1]):
                continue
            attrs = bar_attrs(m, prev, first=False)
            label = f"m{idx}" + (f"({m.number})" if m.number != idx else "")
            if attrs:
                out.append(f"{label} @ " + " ".join(attrs))
            bar_len = Fraction(m.duration.quarterLength).limit_denominator(1024) or Fraction(m.barDuration.quarterLength)
            voices = list(m.voices) or [m]
            for vi, v in enumerate(voices, 1):
                if vi == 1 and is_full_bar_rest(m, v):
                    out.append(f"{label} v1 | R")
                    continue
                toks = voice_items(v, sp, bar_len)
                if vi > 1 and all(t.startswith("_:") for t in toks):
                    continue
                out.append(f"{label} v{vi} | " + " ".join(toks))
            hs = [h for h in m.recurse().getElementsByClass(harmony.ChordSymbol)]
            if hs:
                items, pos = [], Fraction(0)
                hs.sort(key=lambda h: h.offset)
                for i, h in enumerate(hs):
                    off = Fraction(h.offset).limit_denominator(1024)
                    nxt = Fraction(hs[i + 1].offset).limit_denominator(1024) if i + 1 < len(hs) else bar_len
                    if off > pos:
                        items.append("_:" + gap_code(off - pos))
                    figure = h.figure.replace("-", "b")
                    items.append("{" + figure + "}:" + gap_code(nxt - off).split(" ")[0])
                    pos = nxt
                out.append(f"{label} h | " + " ".join(items))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="file path or corpus:<name>")
    ap.add_argument("--bars", help="e.g. 1-4")
    ap.add_argument("--staves", help="e.g. 1,2")
    ap.add_argument("--names", help="staff names to print, comma-separated, e.g. 'Piano RH,Piano LH'")
    a = ap.parse_args()
    s = corpus.parse(a.source[7:]) if a.source.startswith("corpus:") else converter.parse(a.source)
    bars = tuple(int(x) for x in a.bars.split("-")) if a.bars else None
    staves = {int(x) for x in a.staves.split(",")} if a.staves else None
    names = a.names.split(",") if a.names else None
    print(print_score(s, bars, staves, names=names))


if __name__ == "__main__":
    sys.exit(main())
