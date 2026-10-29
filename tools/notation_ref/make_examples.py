#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Builds the worked examples for docs/agent/NOTATION.md section 9 with m21_print.py.
# Public-domain scores from the music21 corpus, plus one short original example written
# here to cover what the corpus excerpts don't (lyrics, a Bb instrument, two voices on one
# staff, tuplets, ties across the barline, chord symbols).
#   ~/.venvs/MuseClaude/bin/python -I tools/notation_ref/make_examples.py > /tmp/examples.md
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from m21_print import print_score  # noqa: E402

from music21 import (chord, corpus, dynamics, expressions, harmony, instrument, interval, key,  # noqa: E402
                     meter, metadata, note, spanner, stream, duration)


def corpus_tune(path, title):
    opus = corpus.parse(path)
    for sc in opus.scores:
        if sc.metadata and sc.metadata.title == title:
            return sc
    raise KeyError(title)


def original_example():
    """'Lantern Song', 4 bars, written for this document (no source)."""
    sc = stream.Score()
    sc.metadata = metadata.Metadata(title="Lantern Song")

    # Voice: lyrics, a hyphenated word, a melisma with extender, a tie across the barline
    v = stream.Part(); v.partName = "Voice"
    v.insert(0, instrument.Vocalist())
    m1 = stream.Measure(number=1); m1.append([meter.TimeSignature("4/4"), key.Key("G")])
    words = [("D5", 1, "Light", "single"), ("B4", 0.5, "the", "single"), ("C5", 0.5, "lan", "begin"),
             ("D5", 1, "tern", "end"), ("G5", 1, "high", "single")]
    for p, ql, w, syl in words:
        n = note.Note(p, quarterLength=ql); n.addLyric(w); n.lyrics[0].syllabic = syl; m1.append(n)
    m1.notes[-1].tie = None
    g_tied = m1.notes[-1]; g_tied.tie = __import__("music21").tie.Tie("start")
    m2 = stream.Measure(number=2)
    n = note.Note("G5", quarterLength=1); n.tie = __import__("music21").tie.Tie("stop"); m2.append(n)
    a = note.Note("F#5", quarterLength=0.5); a.addLyric("glow"); a.lyrics[0].syllabic = "single"; m2.append(a)
    m2.append(note.Note("E5", quarterLength=0.5))
    m2.append(note.Note("D5", quarterLength=2))
    v.append([m1, m2])
    sl = spanner.Slur([a, m2.notes[2]]); v.insert(0, sl)

    # Trumpet in Bb: written pitches; printed at sounding pitch with transposes=-M2
    t = stream.Part(); t.partName = "Trumpet in Bb"
    tp = instrument.Trumpet(); tp.transposition = interval.Interval("M-2"); t.insert(0, tp)
    t.atSoundingPitch = False
    tm1 = stream.Measure(number=1); tm1.append([meter.TimeSignature("4/4"), key.Key("A")])
    tm1.append(note.Rest(quarterLength=2))
    trip = [note.Note(p, quarterLength=1 / 3) for p in ("E5", "F#5", "G#5")]
    for i, n in enumerate(trip):
        n.duration.appendTuplet(duration.Tuplet(3, 2, "eighth"))
        n.duration.tuplets[0].type = "start" if i == 0 else ("stop" if i == 2 else None)
    tm1.append(trip)
    last = note.Note("A5", quarterLength=1); last.articulations = [__import__("music21").articulations.Accent()]
    tm1.append(last)
    tm1.insert(2, dynamics.Dynamic("mf"))
    tm2 = stream.Measure(number=2)
    gr = note.Note("B5", quarterLength=0.5).getGrace(); gr.duration.slash = True
    tm2.append(gr)
    tm2.append(note.Note("A5", quarterLength=2))
    tm2.append(note.Rest(quarterLength=2))
    t.append([tm1, tm2])

    # Piano right hand: two voices on one staff, chord symbols on the staff
    pr = stream.Part(); pr.partName = "Piano"
    pm1 = stream.Measure(number=1); pm1.append([meter.TimeSignature("4/4"), key.Key("G")])
    va, vb = stream.Voice(id="1"), stream.Voice(id="2")
    va.append([note.Note("B4", quarterLength=2), note.Note("C5", quarterLength=2)])
    vb.append([note.Note("G4", quarterLength=1), note.Note("F#4", quarterLength=1),
               chord.Chord(["E4", "G4"], quarterLength=2)])
    pm1.insert(0, va); pm1.insert(0, vb)
    pm1.insert(0, harmony.ChordSymbol("G")); pm1.insert(2, harmony.ChordSymbol("Cmaj7"))
    pm2 = stream.Measure(number=2)
    wa, wb = stream.Voice(id="1"), stream.Voice(id="2")
    wa.append([chord.Chord(["B4", "D5"], quarterLength=2), note.Note("A4", quarterLength=2)])
    wb.append([note.Note("G4", quarterLength=2)])
    pm2.insert(0, wa); pm2.insert(0, wb)
    pm2.insert(0, harmony.ChordSymbol("G")); pm2.insert(2, harmony.ChordSymbol("D7"))
    pr.append([pm1, pm2])

    sc.insert(0, v); sc.insert(0, t); sc.insert(0, pr)
    return sc


EXAMPLES = [
    ("9.1 Four-part chorale (pickup bar, fermatas, minor key)",
     "J. S. Bach, four-part chorale BWV 66.6, bars 1–4 (music21 corpus `bach/bwv66.6`).",
     lambda: print_score(corpus.parse("bach/bwv66.6"), bars=(1, 4))),
    ("9.2 Piano, two staves (slurs, trill, Alberti bass)",
     "W. A. Mozart, Piano Sonata in C, K. 545, i, bars 1–4 (music21 corpus `mozart/k545/movement1_exposition`).",
     lambda: print_score(corpus.parse("mozart/k545/movement1_exposition"), bars=(1, 4),
                         names=["Piano (right hand)", "Piano (left hand)"])),
    ("9.3 String quartet (dynamics, hairpin, slurs across the bar, staccato, ties)",
     "L. van Beethoven, String Quartet Op. 18 No. 1, i, bars 3–6, Violin I and Cello "
     "(music21 corpus `beethoven/opus18no1/movement1.mxl`).",
     lambda: print_score(corpus.parse("beethoven/opus18no1/movement1.mxl"), bars=(3, 6), staves={1, 4})),
    ("9.4 Folk tune (repeats, grace notes)",
     "*Highland Laddie*, from Aird's *Selection of Scotch, English, Irish and Foreign Airs* (c. 1782), "
     "first strain (music21 corpus `airdsAirs/book1.abc`).",
     lambda: print_score(corpus_tune("airdsAirs/book1.abc", "Highland Laddie."), bars=(1, 9), names=["Fiddle"])),
    ("9.5 Everything else (lyrics, Bb trumpet, two voices, triplet, chord symbols)",
     "*Lantern Song*, 2 bars written for this document.",
     lambda: print_score(original_example())),
]


def main():
    out = []
    for title, source, make in EXAMPLES:
        out.append(f"### {title}\n\n{source}\n\n```\n{make()}\n```\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
