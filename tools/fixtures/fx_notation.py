"""Chord symbols, lyrics, transposing instruments, key/time changes."""
from fractions import Fraction as F

from harness import fixture, check, require
from mxl import Score, Part, N, R, MR, CH, H, words, tempo, rehearsal
from fxutil import rests, pitch_at, written, midi_of, fill, eighths

# ---------------------------------------------------------- chord symbols ----

_CS = []          # expected (root, kind, bass) in order, filled by the builder


def _h(root, kind, bass=None, degrees=(), offset=None):
    _CS.append((root, kind, bass))
    return H(root, kind, bass=bass, degrees=degrees, offset=offset)


def _cs_bars():
    _CS.clear()
    q = lambda *ps: [N(p, 'q') for p in ps]
    b = []
    b.append([_h('C', 'major'), N('E5', 'q'), N('G5', 'q'), _h('A', 'minor-seventh'), N('A5', 'q'), N('G5', 'q')])
    b.append([_h('D', 'minor-seventh'), N('F5', 'q'), N('A5', 'q'), _h('G', 'dominant'), N('G5', 'q'), N('F5', 'q')])
    b.append([_h('C', 'major-seventh'), N('E5', 'q'), N('D5', 'q'), _h('F', 'major', bass='A'), N('C5', 'q'), N('A4', 'q')])
    b.append([_h('Bb', 'major', degrees=[('add', 9, 0)]), N('D5', 'q'), N('F5', 'q'),
              _h('E', 'half-diminished'), N('G5', 'q'), N('B4', 'q')])
    b.append([_h('A', 'dominant', degrees=[('alter', 9, -1)]), N('C#5', 'q'), N('E5', 'q'),
              _h('D', 'major-minor'), N('F5', 'q'), N('A5', 'q')])
    b.append([_h('G', 'suspended-fourth'), N('G5', 'q'), N('C6', 'q'),
              _h('C', 'major-sixth', degrees=[('add', 9, 0)]), N('E6', 'q'), N('C6', 'q')])
    b.append([_h('C', 'diminished-seventh'), N('Eb5', 'q'), N('Gb5', 'q'), _h('C', 'augmented'), N('E5', 'q'), N('G#5', 'q')])
    # a chord change in the middle of a held note, via <offset>
    b.append([_h('G', 'dominant'), _h('C', 'major', offset=F(1, 2)), N('B4', 'w')])
    b.append([_h('Bb', 'major', bass='D'), N('F5', 'q'), N('D5', 'q'),
              _h('F#', 'half-diminished', bass='C'), N('C5', 'q'), N('A4', 'q')])
    b.append([_h('C', 'none'), R('h'), _h('C', 'major'), N('C5', 'h')])
    return b


@fixture('chord-symbols',
         'Lead sheet with chord symbols: major, minor-7, dominant, maj7, slash chords (A and D bass, flat '
         'root, F#), add9, 7b9, min-maj7, sus4, 6/9, dim7, augmented, half-diminished, "N.C.", and a chord '
         'change in the middle of a held whole note.',
         'MusicXML from tools/fixtures/mxl.py (`<harmony>` with root, kind, bass, degree, offset), `mscore -o`.',
         'Re-exported MusicXML harmony list (root, kind, bass) equals the source in order; mscx holds the '
         'same number of `<Harmony>` elements.')
def chord_symbols():
    sc = Score('Chord symbols')
    p = sc.add(Part('P1', 'Piano', sound='keyboard.piano', program=1))
    for items in _cs_bars():
        p.m().v(1, items)
    return sc


def _xml_harmonies(c):
    out = []
    for m in c.measures():
        for h in m.findall('harmony'):
            r = h.find('root')
            alt = int(float(r.findtext('root-alter', '0')))
            root = r.findtext('root-step') + {0: '', 1: '#', -1: 'b'}[alt]
            bass = None
            if h.find('bass') is not None:
                ba = int(float(h.find('bass').findtext('bass-alter', '0')))
                bass = h.find('bass').findtext('bass-step') + {0: '', 1: '#', -1: 'b'}[ba]
            degrees = [(d.findtext('degree-type'), int(d.findtext('degree-value')), int(float(d.findtext('degree-alter', '0'))))
                       for d in h.findall('degree')]
            out.append((m.get('number'), root, h.findtext('kind'), bass, degrees))
    return out


@check('chord-symbols')
def _(c):
    _cs_bars()
    got = _xml_harmonies(c)
    want = list(_CS)
    require(len(got) == len(want), '%d harmonies exported, %d expected' % (len(got), len(want)))
    bad = [(g, w) for g, w in zip(got, want) if (g[1], g[2], g[3]) != w]
    require(not bad, 'harmonies differ: %s' % bad[:3])
    n = c.mscx_count('Harmony')
    require(n == len(want), '%d Harmony in mscx' % n)
    return '%d chord symbols round-trip (root, kind, bass); degrees seen: %s' % (
        len(got), sorted({(d[0], d[1], d[2]) for g in got for d in g[4]}))


# ---------------------------------------------------------------- lyrics ----

# per note: (pitch, dur, {verse: (text, syllabic[, extender])}, slur)
def _ly_notes():
    S, B, M, E = 'single', 'begin', 'middle', 'end'
    return [
        [('C5', 'q', {1: ('Walk', B), 2: ('Sing', B), 3: ('Hear', S)}, None),
         ('E5', 'q', {1: ('ing', E), 2: ('ing', E), 3: ('the', S)}, None),
         ('G5', 'q', {1: ('in', S), 2: ('through', S), 3: ('bells', S)}, None),
         ('E5', 'q', {1: ('the', S), 2: ('the', S), 3: ('ring', S)}, None)],
        [('A5', 'q', {1: ('sil', B), 2: ('gold', B), 3: ('out', S)}, None),
         ('G5', 'q', {1: ('ver', E), 2: ('en', E), 3: ('of', S)}, None),
         ('E5', 'h', {1: ('light', S, 'start'), 2: ('morn', B), 3: ('sight', S)}, 'start')],
        [('D5', 'q', {1: (None, None, 'continue'), 2: ('ing', E)}, None),
         ('C5', 'q', {1: (None, None, 'stop'), 2: ('flows', S)}, 'stop'),
         ('D5', 'h', {1: ('and', S), 2: ('slow', S)}, None)],
        [('E5', 'q', {1: ('won', B), 2: ('ra', B)}, None),
         ('F5', 'q', {1: ('der', M), 2: ('di', M)}, None),
         ('G5', 'q', {1: ('ful', E), 2: ('ant', E)}, None),
         ('A5', 'q', {1: ('tune', S), 2: ('beams', S)}, None)],
        [('G5', 'q', {1: ('long', S, 'start'), 2: ('the', S)}, 'start'),
         ('F5', 'q', {1: (None, None, 'continue'), 2: ('qui', B)}, None),
         ('E5', 'h', {1: (None, None, 'continue'), 2: ('et', E)}, None)],
        [('D5', 'q', {1: (None, None, 'stop'), 2: ('dawn', S)}, 'stop'),
         ('E5', 'q', {1: ('days', S), 2: ('sings', S)}, None),
         ('C5', 'h', {1: ('end', S), 2: ('on', S)}, None)],
        [('E5', 'q', {1: ('hom', B), 2: ('hom', B)}, None),
         ('D5', 'q', {1: ('ward', E), 2: ('ward', E)}, None),
         ('C5', 'h', {1: ('bound', S), 2: ('bound', S)}, None)],
        [('C5', 'w', {1: ('home', S), 2: ('home', S)}, None)],
    ]


@fixture('lyrics-melismas-verses',
         'Vocal line with three verses (verse 3 only in the first two bars), hyphenated two- and '
         'three-syllable words (begin/middle/end), and melismas drawn as extender lines over several notes, '
         'one of them running across a barline, with a slur over each melisma.',
         'MusicXML from tools/fixtures/mxl.py (`<lyric number>` with syllabic/text/extend), `mscore -o`. The '
         'words are invented filler, not from any song.',
         'Re-exported MusicXML: per verse the (text, syllabic) sequence equals the source and the two melisma '
         'lines start on the right syllables (`<extend/>`); mscx has one `<Lyrics>` per syllable and `<ticks_f>` '
         '3/4 and 4/4 on the two melisma starts.')
def lyrics_melismas():
    sc = Score('Lyrics, melismas, verses')
    p = sc.add(Part('P1', 'Voice', sound='voice.vocals', program=53, key=0))
    for bar in _ly_notes():
        items = []
        for pitch, d, lyr, slur in bar:
            ly = []
            for verse, spec in sorted(lyr.items()):
                text, syl = spec[0], spec[1]
                ext = spec[2] if len(spec) > 2 else None
                ly.append((verse, syl, text, ext))
            sl = [(slur, 1)] if slur else []
            items.append(N(pitch, d, ly=ly, slur=sl))
        p.m().v(1, items)
    return sc


@check('lyrics-melismas-verses')
def _(c):
    want = {}
    ext_want = []
    for bar in _ly_notes():
        for pitch, d, lyr, slur in bar:
            for verse, spec in lyr.items():
                if spec[0] is not None:
                    want.setdefault(str(verse), []).append((spec[0], spec[1]))
                if verse == 1 and len(spec) > 2 and spec[2] == 'start':
                    ext_want.append(spec[0])
    got, ext_got = {}, []
    for n in c.notes():
        for number, syl, text, ext in n['lyrics']:
            if text is not None:
                got.setdefault(number, []).append((text, syl))
            if ext:
                ext_got.append((number, text))
    require(got == want, 'lyrics differ:\n got  %s\n want %s' % (got, want))
    require(ext_got == [('1', t) for t in ext_want], 'melisma starts differ: got %s want %s' % (ext_got, ext_want))
    n_ly = c.mscx_count('Lyrics')
    n_syl = sum(len(v) for v in want.values())
    require(n_ly == n_syl, 'mscx has %d Lyrics for %d syllables' % (n_ly, n_syl))
    ticks = [l.findtext('ticks_f') or l.findtext('ticks') for l in c.mscx.iter('Lyrics')
             if l.find('ticks_f') is not None or l.find('ticks') is not None]
    require(ticks == ['3/4', '4/4'], 'melisma lengths in mscx %s' % ticks)
    return '%s syllables per verse; melisma lines start at %s with lengths %s (exported as <extend/> on the starting note)' % (
        {k: len(v) for k, v in sorted(want.items())}, ext_want, ticks)


# ------------------------------------------------- transposing instruments ----

# (id, name, sound, midi program, MusicXML transpose (diatonic, chromatic, octave-change), written key fifths)
TRANSPOSERS = [
    ('P1', 'Flute', 'wind.flutes.flute', 74, None, 0),
    ('P2', 'Trumpet in Bb', 'brass.trumpet.bflat', 57, (-1, -2, 0), 2),
    ('P3', 'Horn in F', 'brass.french-horn', 61, (-4, -7, 0), 1),
    ('P4', 'Alto Saxophone in Eb', 'wind.reed.saxophone.alto', 66, (-5, -9, 0), 3),
    ('P5', 'Tenor Saxophone in Bb', 'wind.reed.saxophone.tenor', 67, (-1, -2, -1), 2),
    ('P6', 'Clarinet in A', 'wind.reed.clarinet.a', 72, (-2, -3, 0), -3),
]
CONCERT = [['C4', 'D4', 'E4', 'F4'], ['G4', 'A4', 'B4', 'C5'], ['Bb4', 'F#4', 'Eb4', 'Ab4'], ['C4', 'G4']]


def _concert_midi():
    return [midi_of(p) for bar in CONCERT for p in bar]


@fixture('transposing-instruments',
         'Transposing instruments playing the same concert-pitch line (C4 up to C5, then Bb F# Eb Ab, then '
         'C and G): Bb trumpet, F horn, Eb alto sax, Bb tenor sax (sounds a major ninth below written), A clarinet, with a '
         'non-transposing flute as reference. Written pitches and key signatures are the transposed ones.',
         'MusicXML from tools/fixtures/mxl.py (`<transpose>` diatonic/chromatic/octave-change in each part, '
         'written pitches computed by fxutil.written), `mscore -o`.',
         'mscx: every staff\'s notes have the same concert `<pitch>` values; each Instrument has the '
         'expected transposeDiatonic/transposeChromatic; the exported MusicXML carries the same `<transpose>` '
         'totals per part.')
def transposing_instruments():
    sc = Score('Transposing instruments')
    for pid, name, sound, prog, tr, key in TRANSPOSERS:
        p = sc.add(Part(pid, name, sound=sound, program=prog, transpose=tr, key=key, channel=int(pid[1:])))
        dia, chrom = (0, 0) if tr is None else (tr[0] + 7 * tr[2], tr[1] + 12 * tr[2])
        for bar in CONCERT:
            m = p.m()
            notes = [N(written(x, dia, chrom), 'q' if len(bar) == 4 else 'h') for x in bar]
            m.v(1, notes)
    return sc


@check('transposing-instruments')
def _(c):
    want = _concert_midi()
    staves = c.mscx.find('Score').findall('Staff')
    require(len(staves) == 6, '%d staves' % len(staves))
    for st in staves:
        got = [int(n.findtext('pitch')) for n in st.iter('Note')]
        require(got == want, 'staff %s concert pitches %s != %s' % (st.get('id'), got, want))
    insts = []
    for part in c.mscx.find('Score').findall('Part'):
        ins = part.find('Instrument')
        insts.append((ins.findtext('longName') or part.findtext('trackName'),
                      int(ins.findtext('transposeDiatonic') or 0), int(ins.findtext('transposeChromatic') or 0)))
    wantt = [(0, 0), (-1, -2), (-4, -7), (-5, -9), (-8, -14), (-2, -3)]
    require([(d, ch) for _, d, ch in insts] == wantt, 'instrument transposition %s' % insts)
    tx = []
    for part in c.parts():
        t = part.find('measure/attributes/transpose')
        if t is None:
            tx.append((0, 0))
        else:
            o = int(t.findtext('octave-change', '0'))
            tx.append((int(t.findtext('diatonic', '0')) + 7 * o, int(t.findtext('chromatic', '0')) + 12 * o))
    require(tx == wantt, 'exported <transpose> totals %s' % tx)
    first = [[n['pitch'] for n in c.notes(i) if n['m'] == '1'][0] for i in range(6)]
    return 'concert pitches identical on all 6 staves; transposition (diatonic, chromatic) %s; first written pitch per part %s' % (
        wantt, first)


# ------------------------------------------------------- key/time changes ----

# (time, key fifths, mode, violin clef change, cello clef change) per bar; None = unchanged
KT = [
    ((4, 4), 0, 'major'), None, ((4, 4), 1, 'major'), ((3, 4), None, None), None,
    ((6, 8), -3, 'major'), None, ((5, 4), None, None), ((5, 4), 3, 'minor'), (('C', None), None, None),
    (('C/', None), -1, 'minor'), ((7, 8), None, None), ((7, 8), 0, 'major'), ((4, 4), None, None), None,
]
CELLO_CLEFS = {1: ('F', 4), 6: ('C', 4), 11: ('G', 2), 14: ('F', 4)}


def _lens(time):
    n, d = time
    if n == 'C':
        return F(1)
    if n == 'C/':
        return F(1)
    return F(n, d)


@fixture('key-time-changes',
         'Violin and cello through 15 bars of key and time changes: 4/4 C, G major (bar 3), 3/4 (4), Eb major '
         'and 6/8 together (6), 5/4 (8), F# minor (9), common time symbol (10), cut time + D minor (11), '
         '7/8 (12), back to C major (13), 4/4 (14); cello clef changes bass-tenor-treble-bass.',
         'MusicXML from tools/fixtures/mxl.py (`<key>`, `<time>`, `<clef>` in later measures), `mscore -o`.',
         'Re-exported MusicXML: for both parts the (bar, key fifths, mode, beats/beat-type, symbol) changes '
         'and the cello clef changes equal the source exactly.')
def key_time_changes():
    sc = Score('Key and time changes')
    vl = sc.add(Part('P1', 'Violin', sound='strings.violin', program=41))
    vc = sc.add(Part('P2', 'Cello', sound='strings.cello', program=43, clefs=[('F', 4)]))
    cur = (4, 4)
    for i, spec in enumerate(KT):
        bar = i + 1
        kw = {}
        if spec:
            t, k, mode = spec
            if t is not None:
                kw['time'] = t
                cur = t
            if k is not None:
                kw['key'] = k
                kw['mode'] = mode
        length = _lens(cur)
        for part in (vl, vc):
            kk = dict(kw)
            if part is vc and bar in CELLO_CLEFS and bar != 1:
                kk['clefs'] = [CELLO_CLEFS[bar]]
            m = part.m(**kk)
            if part is vl:
                m.v(1, eighths(length, [pitch_at(2 + (bar + j) % 5, 4) for j in range(3)]))
            else:
                base = pitch_at(0 + (bar % 3), 2) if bar not in (6, 7, 8, 9, 10) else pitch_at(bar % 3 + 4, 2)
                m.v(1, fill(length, lambda d: N(base, d)))
    return sc


@check('key-time-changes')
def _(c):
    exp_changes = []
    cur_t, cur_k = None, None
    for i, spec in enumerate(KT):
        if spec:
            t, k, mode = spec
            if t is not None:
                exp_changes.append((str(i + 1), 'time', (t if t[0] in ('C', 'C/') else t)))
            if k is not None:
                exp_changes.append((str(i + 1), 'key', (k, mode)))
    for pi in (0, 1):
        got = []
        for m in c.measures(pi):
            a = m.find('attributes')
            if a is None:
                continue
            k = a.find('key')
            if k is not None:
                got.append((m.get('number'), 'key', (int(k.findtext('fifths')), k.findtext('mode', 'major'))))
            t = a.find('time')
            if t is not None:
                sym = t.get('symbol')
                beats, bt = int(t.findtext('beats')), int(t.findtext('beat-type'))
                if sym == 'common':
                    val = ('C', None)
                elif sym == 'cut':
                    val = ('C/', None)
                else:
                    val = (beats, bt)
                got.append((m.get('number'), 'time', val))
        # order within a bar: key then time in both lists
        norm = lambda lst: sorted(lst, key=lambda x: (int(x[0]), x[1]))
        require(norm(got) == norm(exp_changes), 'part %d changes:\n got  %s\n want %s' % (pi, norm(got), norm(exp_changes)))
    clefs = []
    for m in c.measures(1):
        a = m.find('attributes')
        if a is not None and a.find('clef') is not None:
            cl = a.find('clef')
            clefs.append((int(m.get('number')), (cl.findtext('sign'), int(cl.findtext('line')))))
    require(clefs == sorted(CELLO_CLEFS.items()), 'cello clefs %s' % clefs)
    return '%d key/time changes identical in both parts; cello clefs %s' % (len(exp_changes), clefs)
