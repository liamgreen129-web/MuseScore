#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# P1.6: real-score fixtures from public-domain works in the music21 corpus.
#
# Each source -> MusicXML (music21) -> .mscz (the agent build's own converter) -> back to
# MusicXML -> note counts compared. Output goes to tests/fixtures/corpus/ (git-ignored: the
# encodings are music21's; this script regenerates them).
#   ~/.venvs/MuseClaude/bin/python -I tools/fixtures/make_corpus_fixtures.py [fixture-name ...]
import json
import pathlib
import subprocess
import sys
import tempfile

from music21 import converter, corpus

ROOT = pathlib.Path(__file__).resolve().parents[2]
MSCORE = ROOT / "agent.install/mscore.app/Contents/MacOS/mscore"
OUT = ROOT / "tests/fixtures/corpus"

# (fixture name, corpus path, opus title or None, why it is in the set)
SOURCES = [
    ("bach-bwv66-6", "bach/bwv66.6", None, "SATB chorale, pickup, fermatas"),
    ("bach-bwv4-8", "bach/bwv4.8", None, "chorale, longer, minor"),
    ("mozart-k545-i", "mozart/k545/movement1_exposition", None, "piano 2 staves, trills, Alberti bass, clef change"),
    ("mozart-k458-i", "mozart/k458/movement1", None, "string quartet, 6/8, dynamics"),
    ("beethoven-op18-1-i", "beethoven/opus18no1/movement1.mxl", None, "long quartet movement (stress), turns, hairpins"),
    ("beethoven-op74", "beethoven/opus74.mxl", None, "whole quartet, over 30 pages (G1 read time)"),
    ("haydn-op74-1-i", "haydn/opus74no1/movement1", None, "quartet, repeats"),
    ("schumann-dichterliebe-2", "schumann_robert/dichterliebe_no2.xml", None, "song: voice + piano, lyrics"),
    ("clara-schumann-polonaise-1", "schumann_clara/polonaise_op1n1.mxl", None, "piano, dense rhythms, ornaments"),
    ("chopin-mazurka-6-2", "chopin/mazurka06-2.krn", None, "piano, 3/4, from Humdrum"),
    ("schoenberg-op19-2", "schoenberg/opus19/movement2.mxl", None, "atonal piano, no key"),
    ("handel-lascia", "handel/rinaldo/Lascia_chio_pianga.mxl", None, "aria, voice + continuo, lyrics"),
    ("corelli-op3-1-grave", "corelli/opus3no1/1grave.xml", None, "trio sonata, figured continuo"),
    ("cpebach-h186", "cpebach/h186.mxl", None, "keyboard, ornaments"),
    ("beach-prayer", "beach/prayer_of_a_tired_child.musicxml", None, "song, lyrics, piano"),
    ("berlin-alexanders-ragtime", "leadSheet/berlinAlexandersRagtime.mxl", None, "lead sheet: chord symbols, lyrics"),
    ("foster-brown-hair", "leadSheet/fosterBrownHair.mxl", None, "lead sheet: chord symbols"),
    ("monteverdi-madrigal-3-1", "monteverdi/madrigal.3.1.mxl", None, "5-voice madrigal, lyrics, old clefs"),
    ("airds-highland-laddie", "airdsAirs/book1.abc", "Highland Laddie.", "folk tune, grace notes, repeats"),
]


def load(path, title):
    s = corpus.parse(path)
    if title is not None:
        for sc in s.scores:
            if sc.metadata and sc.metadata.title == title:
                return sc
        raise KeyError(title)
    return s


def count_notes(s):
    return sum(1 for n in s.recurse().notes if not n.duration.isGrace)


def main():
    if not MSCORE.exists():
        print(f"error: build the agent app first ({MSCORE})", file=sys.stderr)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    only = set(sys.argv[1:])
    old = json.loads((OUT / "report.json").read_text()) if only and (OUT / "report.json").exists() else []
    report = [e for e in old if e["name"] not in only]
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        for name, path, title, why in SOURCES:
            if only and name not in only:
                continue
            entry = {"name": name, "source": path, "title": title, "why": why}
            try:
                s = load(path, title)
                xml = tmp / f"{name}.musicxml"
                s.write("musicxml", fp=str(xml))
                mscz = OUT / f"{name}.mscz"
                r = subprocess.run([str(MSCORE), "-o", str(mscz), str(xml)], capture_output=True, text=True, timeout=300)
                back = tmp / f"{name}.back.musicxml"
                r2 = subprocess.run([str(MSCORE), "-o", str(back), str(mscz)], capture_output=True, text=True, timeout=300)
                ok = mscz.exists() and back.exists()
                n_src = count_notes(s)
                n_back = count_notes(converter.parse(str(back))) if back.exists() else None
                entry.update(converted=ok, notes_source=n_src, notes_roundtrip=n_back,
                             bytes=mscz.stat().st_size if mscz.exists() else 0,
                             same_notes=(n_src == n_back), exit=[r.returncode, r2.returncode])
            except Exception as e:  # keep going; the report says what failed
                entry.update(converted=False, error=f"{type(e).__name__}: {e}")
            report.append(entry)
            status = "ok " if entry.get("converted") and entry.get("same_notes") else "!! "
            print(status, name, {k: entry.get(k) for k in ("notes_source", "notes_roundtrip", "error") if entry.get(k) is not None})
    order = [src[0] for src in SOURCES]
    report.sort(key=lambda e: order.index(e["name"]) if e["name"] in order else len(order))
    (OUT / "report.json").write_text(json.dumps(report, indent=2))
    return 0 if all(e.get("converted") for e in report) else 1


if __name__ == "__main__":
    sys.exit(main())
