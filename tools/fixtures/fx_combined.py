"""Stress score 1: a ~40-bar, five-part piece that mixes every feature of the corpus."""
from collections import Counter
from fractions import Fraction as F

from harness import fixture, check, require, tie_pairs, mscx_tuplet_stacks
from mxl import (Score, Part, N, R, MR, CH, T, H, Ev, flat, dyn, wedge, octshift, pedal, tempo, rehearsal, words)
from fxutil import pitch_at, written, fill, LETTERS
import fx_tuplets
import fx_structure
from fx_structure import bl

TRUMPET_T = (-1, -2, 0)
ALTO_T = (-5, -9, 0)
SHIFT = {'P2': 2, 'P3': 3}          # written key = concert key + this many sharps
LAST = 40
NEW_SYSTEM = {5, 13, 21, 29, 37}
IRREGULAR = {0: F(1, 4), 16: F(2, 4), 32: F(3, 4)}
NESTED_CELLO = 13                  # bar whose cello line is a nested tuplet (written into the mscx)

_SRC = {}


def ts_at(b):
    if b < 9:
        return (4, 4)
    if b < 17:
        return (3, 4)
    if b < 25:
        return (6, 8)
    if b < 27:
        return (5, 4)
    return (4, 4)


def key_at(b):
    if b < 9:
        return (0, 'major')
    if b < 17:
        return (1, 'major')
    if b < 25:
        return (-3, 'major')
    if b < 33:
        return (-1, 'minor')
    return (0, 'major')


def length_at(b):
    if b in IRREGULAR:
        return IRREGULAR[b]
    n, d = ts_at(b)
    return F(n, d)


def scale(fifths, mode, octave, n=22):
    start = (fifths * 4) % 7
    if mode == 'minor':
        start = (start - 2) % 7
    out = []
    sharps, flats = 'FCGDAEB', 'BEADGCF'
    for k in range(n):
        idx = (start + k) % 7
        letter = LETTERS[idx]
        o = octave + (start + k) // 7
        alt = ''
        if fifths > 0 and letter in sharps[:fifths]:
            alt = '#'
        if fifths < 0 and letter in flats[:-fifths]:
            alt = 'b'
        out.append(letter + alt + str(o))
    return out


def beats(b, make):
    n, d = ts_at(b)
    length = length_at(b)
    if d == 8 and n % 3 == 0 and n > 3 and b not in IRREGULAR:
        return [make('q.', i) for i in range(n // 3)]
    return [make('q', i) for i in range(int(length / F(1, 4)))]


def eighth_run(b, pitches):
    length = length_at(b)
    n = int(length / F(1, 8))
    return [N(pitches[i % len(pitches)], 'e') for i in range(n)]


# lyric syllables (invented filler text)
V1 = [('Walk', 'begin'), ('ing', 'end'), ('down', 'single'), ('the', 'single'), ('long', 'single'),
      ('and', 'single'), ('wind', 'begin'), ('ing', 'end'), ('road', 'single'), ('home', 'single')]
V2 = [('Sing', 'begin'), ('ing', 'end'), ('through', 'single'), ('the', 'single'), ('gold', 'begin'),
      ('en', 'end'), ('air', 'single'), ('a', 'begin'), ('far', 'end')]


class Lyr:
    def __init__(self):
        self.i1 = self.i2 = 0

    def next(self):
        a, b = V1[self.i1 % len(V1)], V2[self.i2 % len(V2)]
        self.i1 += 1
        self.i2 += 1
        return [(1, a[1], a[0], None), (2, b[1], b[0], None)]


def degree_chord(sc, deg, mode):
    root = sc[deg][:-1]
    if mode == 'minor':
        kind = {0: 'minor', 3: 'minor', 4: 'dominant', 5: 'major', 1: 'half-diminished'}[deg]
    else:
        kind = {0: 'major', 3: 'major', 4: 'dominant', 5: 'minor', 1: 'minor'}[deg]
    return root, kind


PROG = [0, 5, 3, 4]       # I vi IV V


def common_kw(b):
    kw = {}
    if b == 0:
        kw.update(number=0, implicit=True, length=IRREGULAR[0])
    elif b in IRREGULAR:
        kw['length'] = IRREGULAR[b]
    if b == 9:
        kw['time'] = (3, 4)
    if b == 17:
        kw['time'] = (6, 8)
    if b == 25:
        kw['time'] = (5, 4)
    if b == 27:
        kw['time'] = (4, 4)
    if b in (9, 17, 25, 33):
        kw['key'], kw['mode'] = key_at(b)
    if b in NEW_SYSTEM:
        kw['new_system'] = True
    left = right = ''
    if b == 1:
        left = bl('left', 'heavy-light', rep='forward')
    if b == 7:
        left = bl('left', ending=('1', 'start'))
        right = bl('right', 'light-heavy', ending=('1', 'stop'), rep='backward')
    if b == 8:
        left = bl('left', ending=('2', 'start'))
        right = bl('right', 'light-light', ending=('2', 'discontinue'))
    if b == LAST:
        right = bl('right', 'light-heavy')
    if left:
        kw['left'] = left
    if right:
        kw['right'] = right
    return kw


def mk(part, b, shift=0):
    kw = common_kw(b)
    if shift and 'key' in kw:
        kw['key'] += shift
    return part.m(**kw)


def to_written(items, tr):
    dia, chrom = tr[0] + 7 * tr[2], tr[1] + 12 * tr[2]
    for e in flat(items):
        if isinstance(e, Ev) and e.pitch:
            e.pitch = written(e.pitch, dia, chrom)
    return items


def voice_line(b, sc):
    L = _SRC['lyr']
    if 25 <= b <= 32:
        return [MR(length_at(b))]
    if b == 0:
        return [N(sc[11], 'q', ly=L.next())]
    if b == 4:
        a = L.next()
        s1 = N(sc[12], 'h', ly=[(1, 'single', 'ahh', 'start'), a[1]], slur=[('start', 1)])
        s2 = N(sc[11], 'q', ly=[(1, None, None, 'continue'), (2, 'single', 'oo', None)])
        s3 = N(sc[10], 'q', ly=[(1, None, None, 'stop'), (2, 'single', 'ee', None)], slur=[('stop', 1)])
        return [s1, s2, s3]
    if b == 18:
        def duplet(i):
            return T(2, 3, N(sc[9 + i], 'e', ly=L.next()), N(sc[10 + i], 'e', ly=L.next()))
        return [duplet(0), duplet(2)]
    if b == LAST:
        return [N(sc[7], 'w', nt=['<fermata type="upright"/>'])]
    use_ly = b <= 8 or 17 <= b <= 24
    items = beats(b, lambda d, i: N(sc[7 + (b * 3 + i * 2) % 6], d, ly=L.next() if use_ly else []))
    if b in (1, 9, 17, 33):
        items.insert(0, dyn({1: 'mp', 9: 'mf', 17: 'p', 33: 'f'}[b]))
    return items


def trumpet_line(b, sc):
    if b == 0:
        return [R('q')]
    if b in (1, 2, 13):
        return [MR(length_at(b))]
    p = lambda i: sc[7 + i]
    if b == 3:
        return [T(3, 2, N(p(0), 'e'), N(p(1), 'e'), N(p(2), 'e')), T(3, 2, N(p(2), 'e'), N(p(3), 'e'), N(p(4), 'e')),
                N(p(3), 'q'), N(p(1), 'q')]
    if b == 4:
        return [T(5, 4, *[N(p(i), 's', slur=[('start', 1)] if i == 0 else []) for i in range(5)]), N(p(2), 'h.')]
    if b == 6:
        return [N(p(2), 'q', slur=[('stop', 1)]), N(p(3), 'q'), N(p(4), 'q'), N(p(5), 'q', tie='start')]
    if b == 7:
        return [N(p(5), 'h', tie='stop'), N(p(3), 'h')]
    if b == 14:
        return [T(7, 8, *[N(p(i % 5), 's') for i in range(7)]), N(p(2), 'q')]
    if b == 22:
        return [T(9, 8, *[N(p(i % 6), 's') for i in range(9)]), N(p(3), 'q')]
    if b == 27:
        return [N(p(0), 'q'), N(p(1), 'q', nt=['<ornaments><trill-mark/><wavy-line type="start" number="1"/></ornaments>']),
                N(p(2), 'q'), N(p(3), 'q')]
    if b == 28:
        return [N(p(3), 'q'), N(p(4), 'q', nt=['<ornaments><wavy-line type="stop" number="1"/></ornaments>']),
                N(p(2), 'h')]
    if b == 36:
        return [T(13, 8, *[N(p(i % 7), 't') for i in range(13)]), N(p(2), 'h.')]
    if b == LAST:
        return [N(p(0), 'w', nt=['<fermata type="upright"/>'])]
    return eighth_run(b, [p(0), p(2), p(4), p(2)])


def sax_line(b, sc):
    if b == 0:
        return [R('q')]
    if b <= 4 or 25 <= b <= 26:
        return [MR(length_at(b))]
    p = lambda i: sc[7 + i]
    if b == 5:
        return [N(p(3), 'e', grace='slash'), N(p(2), 'q'), N(p(4), 'q'), N(p(2), 'q'), N(p(0), 'q')]
    if b == 6:
        return [N(p(1), 'e', grace='plain'), N(p(2), 'h'), N(p(1), 'q'), N(p(0), 'q'),
                N(p(1), 's', grace='plain'), N(p(0), 's', grace='plain')]
    if b == 10:
        return [N(p(0), 't', grace='plain'), N(p(1), 't', grace='plain'), N(p(2), 't', grace='plain'),
                N(p(3), 'h'), N(p(2), 'q')]
    if b == 20:
        return [N(p(2), 'q.'), N(p(4), 'q.', tie='start')]
    if b == 21:
        return [N(p(4), 'q.', tie='stop'), N(p(2), 'q.')]
    if b == LAST:
        return [N(p(2), 'w', nt=['<fermata type="upright"/>'])]
    return beats(b, lambda d, i: R(d) if (b + i) % 4 == 3 else N(p((b + 2 * i) % 6), d))


def cello_line(b, sc):
    if b == 0:
        return [R('q')]
    q = lambda i: sc[i]
    if b == 2:
        return [N(q(4), 'q'), N(q(0), 'q'), N(q(4), 'q'), N(q(2), 'q', tie='start')]
    if b == 3:
        return [N(q(2), 'q', tie='stop'), N(q(0), 'q'), N(q(3), 'h')]
    if b == NESTED_CELLO:
        return [MR(length_at(b))]
    if b == LAST:
        return [N(q(0), 'w', nt=['<fermata type="upright"/>'])]
    return beats(b, lambda d, i: N(q((b + i * 2) % 5), d))


def piano_lines(b, sc, mode):
    """(right-hand voice lines [(voice, items)], left-hand items, harmony count)."""
    n_h = 0
    p = lambda i: sc[7 + i]
    if b == 0:
        return [(1, [R('q')])], [R('q')], 0
    length = length_at(b)
    # chord symbols: every bar, twice in 4/4 and 5/4
    rh_voices = []
    if b == 4:
        v1 = [N(p(i), 'e', stem='up') for i in (4, 5, 4, 3, 2, 3, 4, 2)]
        v2 = [N(p(i), 'q', stem='down') for i in (2, 3, 2, 1)]
        v3 = [N(p(1), 'h', stem='up'), N(p(0), 'h', stem='up')]
        v4 = [N(sc[7], 'w', stem='down')]
        rh_voices = [(1, v1), (2, v2), (3, v3), (4, v4)]
    elif 9 <= b <= 12:
        pat = [(sc[3 + (b % 3)], 2), (sc[7 + (b % 3)], 2), (sc[9 + (b % 3)], 1), (sc[11 + (b % 3)], 1),
               (sc[9 + (b % 3)], 1), (sc[7 + (b % 3)], 2)]
        beam = 'bcebce'
        v1 = [N(pp, 'e', staff=s, beam=bm) for (pp, s), bm in zip(pat, beam)]
        rh_voices = [(1, v1)]
        _SRC['xs'] = _SRC.get('xs', 0) + sum(1 for _, s in pat if s == 2)
    else:
        rh_voices = [(1, eighth_run(b, [p(0), p(2), p(4), p(2)]))]
    if b == LAST:
        rh_voices = [(1, [CH([p(0), p(2), p(4)], 'w', nt=['<fermata type="upright"/>'])])]
    # harmony in front of the first RH voice
    v1 = rh_voices[0][1]
    deg = PROG[b % 4]
    root, kind = degree_chord(sc, deg, mode)
    v1.insert(0, H(root, kind))
    n_h += 1
    evs = [i for i, e in enumerate(v1) if isinstance(e, Ev)]
    if length >= F(1, 1) and len(evs) > 1:
        r2, k2 = degree_chord(sc, PROG[(b + 1) % 4], mode)
        v1.insert(evs[len(evs) // 2], H(r2, k2))
        n_h += 1
    # spanners in the right hand
    firsts = [i for i, e in enumerate(v1) if isinstance(e, Ev)]
    if b == 3:
        v1.insert(firsts[0], wedge('crescendo', 1))
    if b == 5:
        v1.insert(firsts[1], wedge('stop', 1))
    if b == 11:
        v1.insert(firsts[0], octshift('down', 8, 1))
    if b == 14:
        v1.insert(firsts[1], octshift('stop', 8, 1))
    # left hand: one note per beat unit, an octave or two below
    lh = beats(b, lambda d, i: N(sc[(deg * 1 + i * 2) % 5], d))
    if b == 17:
        lh.insert(0, pedal('start', staff=2))
    if b == 20:
        lh.insert(1, pedal('stop', staff=2))
    if b == 28:
        lh[-1].tie = 'start'
    if b == 29:
        lh[0].tie = 'stop'
        lh[0].pitch = _SRC['lh28']
    if b == 28:
        _SRC['lh28'] = lh[-1].pitch
    if b == LAST:
        lh = [CH([sc[0], sc[4]], 'w', nt=['<fermata type="upright"/>'])]
    return rh_voices, lh, n_h


MARKERS = {
    25: ('Marker', 'segno', '<Marker><style>repeat_left</style><text><sym>segno</sym></text>'
                            '<label>segno</label><markerType>segno</markerType></Marker>'),
    30: ('Marker', 'coda', '<Marker><style>repeat_right</style><text>To Coda</text>'
                           '<label>coda</label><markerType>coda</markerType></Marker>'),
    32: ('Jump', 'D.S. al Coda', '<Jump><style>repeat_right</style><text>D.S. al Coda</text><jumpTo>segno</jumpTo>'
                                 '<playUntil>coda</playUntil><continueAt>codab</continueAt></Jump>'),
    33: ('Marker', 'codab', '<Marker><style>repeat_left</style><text><sym>coda</sym></text>'
                            '<label>codab</label><markerType>codab</markerType></Marker>'),
}


@fixture('stress-combined',
         'Stress score 1: 41 bars (pickup + 40) for voice, Bb trumpet, Eb alto sax, piano and cello mixing '
         'everything: pickup bar and two irregular bars (2/4 in 3/4, 3/4 in 4/4); time changes 4/4, 3/4, 6/8, '
         '5/4, 4/4 and key changes C, G, Eb, D minor, C; repeat with first/second endings; segno, To Coda, '
         'D.S. al Coda, coda; tuplets 3:2, 5:4, 2:3, 7:8, 9:8, 13:8 and a nested triplet; four voices on a '
         'piano staff; cross-staff beamed arpeggios; two lyric verses with a melisma; chord symbols; '
         'acciaccatura, appoggiatura, 32nd graces and graces after a note; ties over barlines and over '
         'system breaks; slur, hairpin, 8va, pedal and trill lines (slur, hairpin, 8va and a tie cross forced '
         'system breaks at bars 5, 13, 21, 29, 37); transposing trumpet and sax; cello clef changes.',
         'tools/fixtures/fx_combined.py: MusicXML via mxl.py -> `mscore -o`; the nested-tuplet bar (cello, '
         'bar 13) and the Marker/Jump elements are written into the .mscx by mscxedit.py; app re-save.',
         'Counts taken from the source and compared with the file / the MusicXML export: bars and bar lengths, '
         'time and key changes, tuplet ratios and nesting, voices, cross-staff chords, lyrics per verse, '
         'harmony count, grace kinds, repeats/voltas, markers/jumps, tie pairs, spanner bars straddling '
         'breaks, transposition, clef changes.')
def stress_combined():
    _SRC.clear()
    _SRC['lyr'] = Lyr()
    sc = Score('Stress score: combined')
    voice = sc.add(Part('P1', 'Voice', sound='voice.vocals', program=53))
    trp = sc.add(Part('P2', 'Trumpet in Bb', sound='brass.trumpet.bflat', program=57, transpose=TRUMPET_T,
                      key=SHIFT['P2']))
    sax = sc.add(Part('P3', 'Alto Saxophone', sound='wind.reed.saxophone.alto', program=66, transpose=ALTO_T,
                      key=SHIFT['P3'], channel=3))
    pno = sc.add(Part('P4', 'Piano', sound='keyboard.piano', program=1, staves=2, channel=4))
    vc = sc.add(Part('P5', 'Cello', sound='strings.cello', program=43, clefs=[('F', 4)], channel=5))
    n_harm = 0
    for b in range(0, LAST + 1):
        f, mode = key_at(b)
        sc4 = scale(f, mode, 3)
        sc2 = scale(f, mode, 2)
        pre = []
        if b in (1, 9, 17, 25, 33):
            pre.append(rehearsal('ABCDE'[(b - 1) // 8]))
            pre.append(tempo({1: 96, 9: 120, 17: 72, 25: 96, 33: 60}[b], {1: 'Allegretto', 17: 'Andante', 33: 'Largo'}.get(b)))
        m = mk(voice, b)
        m.pre = pre
        m.v(1, voice_line(b, sc4))
        m = mk(trp, b, SHIFT['P2'])
        m.v(1, to_written(trumpet_line(b, sc4), TRUMPET_T))
        m = mk(sax, b, SHIFT['P3'])
        m.v(1, to_written(sax_line(b, sc4), ALTO_T))
        m = mk(pno, b)
        rh, lh, nh = piano_lines(b, sc4, mode)
        n_harm += nh
        for voice_no, items in rh:
            m.v(voice_no, items, staff=1)
        m.v(5, lh, staff=2)
        kw_clef = {}
        if b == 17:
            kw_clef['clefs'] = [('C', 4)]
        if b == 25:
            kw_clef['clefs'] = [('F', 4)]
        kwc = common_kw(b)
        kwc.update(kw_clef)
        m = vc.m(**kwc)
        m.v(1, cello_line(b, sc2 if b < 17 or b > 24 else scale(f, mode, 3)))
    _SRC['n_harm'] = n_harm
    # nested bar is 3/4: triplet-in-triplet (1/2) + a quarter rest
    nested_bar = [T(3, 2, N('C3', 'q'), T(3, 2, N('D3', 'e'), N('E3', 'e'), N('F3', 'e')), N('G3', 'q')), R('q')]
    _SRC['nested_bar'] = nested_bar
    edits = {(6, NESTED_CELLO + 1): {0: nested_bar}}
    extras = {(1, bar + 1): [x[2]] for bar, x in MARKERS.items()}
    return sc, edits, extras


def _xml_spans_part(c, part):
    class Shim:
        def __init__(self, c, part):
            self.c, self.part = c, part

        def measures(self):
            return self.c.measures(self.part)
    return fx_structure._xml_spans(Shim(c, part))


@check('stress-combined')
def _(c):
    lines = []
    parts = c.parts()
    require(len(parts) == 5, '%d parts' % len(parts))
    for i, p in enumerate(parts):
        require(len(p.findall('measure')) == 41, 'part %d has %d measures' % (i, len(p.findall('measure'))))
    # time and key changes (voice part, concert)
    got = []
    for m in c.measures(0):
        a = m.find('attributes')
        if a is None:
            continue
        if a.find('key') is not None:
            got.append((int(m.get('number')), 'key', int(a.find('key/fifths').text), a.findtext('key/mode', 'major')))
        if a.find('time') is not None:
            got.append((int(m.get('number')), 'time', int(a.find('time/beats').text), int(a.find('time/beat-type').text)))
    want = [(0, 'key', 0, 'major'), (0, 'time', 4, 4), (9, 'key', 1, 'major'), (9, 'time', 3, 4),
            (17, 'key', -3, 'major'), (17, 'time', 6, 8), (25, 'key', -1, 'minor'), (25, 'time', 5, 4),
            (27, 'time', 4, 4), (33, 'key', 0, 'major')]
    require(sorted(got) == sorted(want), 'voice part changes:\n got  %s\n want %s' % (sorted(got), sorted(want)))
    # transposing parts: written key follows
    for pi, shift in ((1, 2), (2, 3)):
        keys = [(int(m.get('number')), int(m.find('attributes/key/fifths').text)) for m in c.measures(pi)
                if m.find('attributes/key') is not None]
        exp = [(0, shift), (9, 1 + shift), (17, -3 + shift), (25, -1 + shift), (33, shift)]
        require(keys == exp, 'part %d written keys %s != %s' % (pi, keys, exp))
    # irregular bars and pickup
    meas = c.mscx_staff(1).findall('Measure')
    lens = {i: m.get('len') for i, m in enumerate(meas) if m.get('len')}
    require(lens == {0: '1/4', 16: '2/4', 32: '3/4'}, 'measure lens %s' % lens)
    require(meas[0].find('irregular') is not None, 'pickup flag')
    lines.append('41 bars x 5 parts, lens %s' % lens)
    # layout breaks
    breaks = sorted(i for i, m in enumerate(meas) if m.find('LayoutBreak') is not None)
    require(breaks == sorted(b - 1 for b in NEW_SYSTEM), 'layout breaks on mscx measures %s' % breaks)   # a break sits on the bar before the new system
    # tuplets: ratios in the mscx (all staves) and one nested stack
    ratios = Counter()
    for t in c.mscx.iter('Tuplet'):
        ratios[(int(t.findtext('actualNotes')), int(t.findtext('normalNotes')))] += 1
    for need in ((3, 2), (5, 4), (2, 3), (7, 8), (9, 8), (13, 8)):
        require(ratios[need] > 0, 'tuplet %s missing: %s' % (need, dict(ratios)))
    cello = [s for s in c.mscx.find('Score').findall('Staff') if s.get('id') == '6'][0]
    stacks = [st for st in mscx_tuplet_stacks(cello.findall('Measure')[NESTED_CELLO]) if st]
    require(max(len(s) for s in stacks) == 2, 'nested stack %s' % stacks)
    lines.append('tuplets %s' % sorted(ratios))
    # four voices on the piano staff in bar 4
    pno_staff = [s for s in c.mscx.find('Score').findall('Staff') if s.get('id') == '4'][0]
    nv = len(pno_staff.findall('Measure')[4].findall('voice'))
    require(nv == 4, 'bar 4 piano staff has %d voices' % nv)
    # cross-staff
    xs = sum(1 for ch in c.mscx.iter('Chord') if ch.find('staffMove') is not None)
    require(xs == _SRC['xs'], 'cross-staff chords %d != %d' % (xs, _SRC['xs']))
    # lyrics: two verses, melisma line
    verses = Counter()
    for n in c.notes(0):
        for number, syl, text, ext in n['lyrics']:
            if text:
                verses[number] += 1
    require(set(verses) == {'1', '2'}, 'verses %s' % verses)
    require(any(l.find('ticks_f') is not None for l in c.mscx.iter('Lyrics')), 'no melisma line')
    # harmony
    nh = c.mscx_count('Harmony')
    require(nh == _SRC['n_harm'], '%d harmonies, source %d' % (nh, _SRC['n_harm']))
    # grace kinds
    kinds = Counter()
    for ch in c.mscx.iter('Chord'):
        for k in ('acciaccatura', 'appoggiatura', 'grace32', 'grace16after', 'grace8after'):
            if ch.find(k) is not None:
                kinds[k] += 1
    require(kinds['acciaccatura'] >= 1 and kinds['appoggiatura'] >= 1 and kinds['grace32'] == 3 and kinds['grace16after'] == 2,
            'graces %s' % dict(kinds))
    # repeats, voltas, markers
    starts = [i for i, m in enumerate(meas) if m.find('startRepeat') is not None]
    ends = [i for i, m in enumerate(meas) if m.find('endRepeat') is not None]
    require(starts == [1] and ends == [7], 'repeat bars %s %s' % (starts, ends))
    voltas = fx_structure._volta_info(c)
    require(voltas == [(8, '1'), (9, '2')], 'voltas %s' % voltas)
    mj = {i: (el.tag, el.findtext('label') or el.findtext('text')) for i, m in enumerate(meas) for el in m
          if el.tag in ('Marker', 'Jump')}
    require(mj == {b: (x[0], x[1]) for b, x in MARKERS.items()}, 'markers %s' % mj)
    # ties
    ties = {}
    for pi in range(5):
        ties[pi] = sorted(tie_pairs(c.notes(pi)))
    require(any(t[0] == '6' and t[1] == '7' for t in ties[1]), 'trumpet tie 6-7: %s' % ties[1])
    require(any(t[0] == '20' and t[1] == '21' for t in ties[2]), 'sax tie 20-21 across the system break: %s' % ties[2])
    require(any(t[0] == '2' and t[1] == '3' for t in ties[4]), 'cello tie 2-3: %s' % ties[4])
    require(any(t[0] == '28' and t[1] == '29' for t in ties[3]), 'piano tie 28-29 across the system break: %s' % ties[3])
    # spanners crossing breaks
    tsp = _xml_spans_part(c, 1)
    psp = _xml_spans_part(c, 3)
    require((4, 6) in tsp.get('slur', []), 'trumpet slur %s' % tsp)
    require((3, 5) in psp.get('wedge', []), 'piano hairpin %s' % psp)
    require((11, 14) in psp.get('octave-shift', []), 'piano 8va %s' % psp)
    require((17, 20) in psp.get('pedal', []), 'piano pedal %s' % psp)
    require((27, 28) in tsp.get('trill', []), 'trumpet trill line %s' % tsp)
    lines.append('spanners straddling breaks: slur 4-6, hairpin 3-5, 8va 11-14')
    # transposition and clefs
    insts = []
    for part in c.mscx.find('Score').findall('Part'):
        ins = part.find('Instrument')
        insts.append(int(ins.findtext('transposeChromatic') or 0))
    require(insts == [0, -2, -9, 0, 0], 'transposition %s' % insts)
    clefs = [(int(m.get('number')), m.find('attributes/clef/sign').text) for m in c.measures(4)
             if m.find('attributes/clef') is not None]
    require(clefs == [(0, 'F'), (17, 'C'), (25, 'F')], 'cello clefs %s' % clefs)
    lines.append('voices/cross-staff %d/%d, harmonies %d, graces %s, ties %s' % (
        nv, xs, nh, dict(kinds), {k: len(v) for k, v in ties.items()}))
    return '; '.join(lines)
