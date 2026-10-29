"""Post-processing of an .mscz written by MuseScore.

MuseScore's MusicXML importer cannot build nested tuplets (it reads the
combined time-modification as one flat tuplet and pads with rests), so the
nested-tuplet bars are written straight into the score's .mscx, then the file
is re-saved by the app so the fixture is always the app's own output.
"""
import re
import zipfile
import xml.etree.ElementTree as ET

from mxl import Ev, flat

FIFTHS = {'F': -1, 'C': 0, 'G': 1, 'D': 2, 'A': 3, 'E': 4, 'B': 5}
SEMI = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
ALT = {'': 0, '#': 1, 'b': -1, '##': 2, 'bb': -2}


def parse_pitch(p):
    m = re.match(r'^([A-G])(##|bb|#|b)?(-?\d)$', p)
    return m.group(1), ALT[m.group(2) or ''], int(m.group(3))


def midi(p):
    step, alt, octv = parse_pitch(p)
    return 12 * (octv + 1) + SEMI[step] + alt


def tpc(p):
    step, alt, _ = parse_pitch(p)
    return 14 + FIFTHS[step] + 7 * alt


def _dur(e):
    return '%s<durationType>%s</durationType>' % (
        '<dots>%d</dots>' % e.dots if e.dots else '', e.type)


def voice_xml(items):
    """Ev list (with T() marks) -> list of mscx child elements as strings."""
    evs = [i for i in flat(items) if isinstance(i, Ev)]
    out = []
    i = 0
    while i < len(evs):
        e = evs[i]
        group = [e]
        j = i + 1
        while j < len(evs) and evs[j].chord:
            group.append(evs[j])
            j += 1
        for kind, num, brk, base in e.tup:
            if kind == 'start':
                act, norm = e.tm[num - 1]
                out.append('<Tuplet><normalNotes>%d</normalNotes><actualNotes>%d</actualNotes>'
                           '<baseNote>%s</baseNote><Number><style>tuplet</style><text>%d</text></Number></Tuplet>'
                           % (norm, act, base, act))
        if e.pitch is None:
            out.append('<Rest>%s</Rest>' % _dur(e))
        else:
            notes = ''.join('<Note><pitch>%d</pitch><tpc>%d</tpc></Note>' % (midi(g.pitch), tpc(g.pitch)) for g in group)
            out.append('<Chord>%s%s</Chord>' % (_dur(e), notes))
        for t in e.tup:
            if t[0] == 'stop':
                out.append('<endTuplet/>')
        i = j
    return out


CR_TAGS = {'Rest', 'Chord', 'Tuplet', 'endTuplet', 'Beam'}


def inject(src, dst, edits, extras=None):
    """edits: {(staff_id, measure_number_1based): {voice_index: items}}
    extras: {(staff_id, measure_number_1based): [xml of measure-level elements, e.g. Marker, Jump]}"""
    zin = zipfile.ZipFile(src)
    names = zin.namelist()
    mscx = [n for n in names if n.endswith('.mscx') and '/' not in n][0]
    root = ET.fromstring(zin.read(mscx))
    score = root.find('Score')
    staves = {s.get('id'): s for s in score.findall('Staff')}
    for (sid, mno), voices in (edits or {}).items():
        measures = staves[str(sid)].findall('Measure')
        meas = measures[mno - 1]
        vels = meas.findall('voice')
        for vi, items in voices.items():
            while len(vels) <= vi:
                vels.append(ET.SubElement(meas, 'voice'))
            v = vels[vi]
            for ch in list(v):
                if ch.tag in CR_TAGS:
                    v.remove(ch)
            for frag in voice_xml(items):
                v.append(ET.fromstring(frag))
    for (sid, mno), frags in (extras or {}).items():
        meas = staves[str(sid)].findall('Measure')[mno - 1]
        at = list(meas).index(meas.find('voice'))
        for k, frag in enumerate(frags):
            meas.insert(at + k, ET.fromstring(frag))
    ET.indent(root, space='  ')
    data = b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding='utf-8')
    with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as zout:
        for n in names:
            zout.writestr(n, data if n == mscx else zin.read(n))
