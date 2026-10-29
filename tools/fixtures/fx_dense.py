"""Stress score 2: one staff of dense rhythms (deterministic, seeded)."""
import random
from collections import Counter
from fractions import Fraction as F

from harness import fixture, check, require, tie_pairs
from mxl import Score, Part, N, R, MR, CH, T, Ev, flat, dur_of
from fxutil import pitch_at, rests, fill
import fx_tuplets

SEED = 20261029
TS_PLAN = ([(4, 4)] * 4 + [(3, 4)] * 3 + [(7, 8)] * 3 + [(5, 8)] * 2 + [(9, 16)] * 2 + [(4, 4)] * 3 +
           [(12, 8)] * 2 + [(3, 2)] * 2 + [(2, 2)] * 2 + [(6, 8)] * 2 + [(5, 4)] * 2 + [(4, 4)] * 5)
NESTED_BARS = {2: 0, 16: 3, 30: 6}          # bar -> index into fx_tuplets._nested_items()
V2_BARS = {4, 9, 12, 17, 22, 25, 29, 31}      # bars that also get a second voice
NOTE_DURS = ['h', 'q.', 'q', 'q..', 'e.', 'e', 'e..', 's', 's.', 't', 't.', 'x', 'y']
RUN_UNITS = ['s', 't', 'x', 'y']
TUPS = [(3, 2, 'e'), (3, 2, 's'), (5, 4, 's'), (6, 4, 's'), (7, 4, 's'), (7, 8, 's'), (9, 8, 't'), (11, 8, 't'),
        (2, 3, 'e'), (4, 3, 'e'), (5, 4, 'e'), (13, 8, 't'), (10, 8, 't'), (3, 2, 'q')]
UNIT = {'q': 'quarter', 'e': 'eighth', 's': '16th', 't': '32nd', 'x': '64th', 'y': '128th'}

_SOURCE = {}


def d_of(code):
    base = code.rstrip('.')
    dots = len(code) - len(base)
    names = {'h': 'half', 'q': 'quarter', 'e': 'eighth', 's': '16th', 't': '32nd', 'x': '64th', 'y': '128th'}
    return dur_of(names[base], dots)


class Gen:
    def __init__(self, seed, low=0):
        self.r = random.Random(seed)
        self.pos = 7 + low          # index into the C-major scale starting at C3-ish
        self.low = low

    def pitch(self, repeat=0.3):
        if self.r.random() >= repeat:
            self.pos = max(0, min(18, self.pos + self.r.choice([-3, -2, -1, -1, 1, 1, 2, 3])))
        return pitch_at(self.pos, 3 if self.low == 0 else 2)

    def cell(self, remaining):
        r = self.r
        kinds = ['note'] * 6 + ['rest'] * 2 + ['run'] * 3 + ['tup'] * 4 + ['chord'] + ['grace']
        for _ in range(60):
            k = r.choice(kinds)
            if k == 'note':
                code = r.choice(NOTE_DURS)
                if d_of(code) <= remaining:
                    return [N(self.pitch(), code)]
            elif k == 'rest':
                code = r.choice(['h', 'q', 'e', 's', 'q.', 'e.', 't'])
                if d_of(code) <= remaining:
                    return [R(code)]
            elif k == 'run':
                u = r.choice(RUN_UNITS)
                n = r.randint(2, 8)
                if n * d_of(u) <= remaining:
                    return [N(self.pitch(0.15), u) for _ in range(n)]
            elif k == 'tup':
                a, n, u = r.choice(TUPS)
                if n * d_of(u) <= remaining:
                    items = []
                    for _ in range(a):
                        items.append(R(u) if r.random() < 0.12 else N(self.pitch(0.2), u))
                    if all(e.pitch is None for e in items):
                        items[0] = N(self.pitch(), u)
                    return T(a, n, *items)
            elif k == 'chord':
                code = r.choice(['q', 'e', 'h', 's'])
                if d_of(code) <= remaining:
                    p0 = self.pitch(0.1)
                    base = self.pos
                    return CH([p0, pitch_at(base + 2, 3 if self.low == 0 else 2), pitch_at(base + 4, 3 if self.low == 0 else 2)][:r.choice([2, 3])], code)
            elif k == 'grace':
                code = r.choice(['q', 'e', 's'])
                if d_of(code) <= remaining:
                    g = N(pitch_at(self.pos + r.choice([-1, 1]), 3 if self.low == 0 else 2), 's', grace='slash')
                    return [g, N(self.pitch(0.1), code)]
        return fill(remaining, lambda c: N(self.pitch(), c))

    def bar(self, length):
        out, rem = [], F(length)
        while rem > 0:
            cell = self.cell(rem)
            out.extend(cell)
            rem -= sum(e.dur for e in flat(cell) if isinstance(e, Ev) and not e.chord and not e.grace)
        return out


def _link_ties(bars, r):
    """Tie identical consecutive pitches (including across barlines) with some probability."""
    seq = []
    for bi, items in enumerate(bars):
        for e in flat(items):
            if isinstance(e, Ev) and not e.chord and not e.grace:
                seq.append((bi, e, items))
    # chords break tie chains
    chord_leads = set()
    for items in bars:
        fl = flat(items)
        for i, e in enumerate(fl):
            if isinstance(e, Ev) and i + 1 < len(fl) and fl[i + 1].chord:
                chord_leads.add(id(e))
    after_grace = set()
    for items in bars:
        fl = flat(items)
        for i, e in enumerate(fl):
            if isinstance(e, Ev) and e.grace and i + 1 < len(fl):
                after_grace.add(id(fl[i + 1]))
    n = 0
    for (b1, a, _), (b2, b, _) in zip(seq, seq[1:]):
        if (a.pitch and b.pitch and a.pitch == b.pitch and id(a) not in chord_leads and id(b) not in chord_leads
                and id(b) not in after_grace and r.random() < 0.8):
            a.tie = 'both' if a.tie == 'stop' else 'start'
            b.tie = 'stop' if b.tie is None else 'both'
            n += 1
    return n


def _build():
    g1 = Gen(SEED)
    g2 = Gen(SEED + 1, low=1)
    bars, v2 = [], {}
    for i, (n, d) in enumerate(TS_PLAN):
        bar = i + 1
        length = F(n, d)
        if bar in NESTED_BARS:
            bars.append([MR(length)])
        else:
            bars.append(g1.bar(length))
        if bar in V2_BARS:
            v2[bar] = g2.bar(length)
    ties = _link_ties(bars, g1.r)
    return bars, v2, ties


@fixture('stress-dense-rhythms',
         'Stress score, one staff, 32 bars of dense rhythm: 4/4, 3/4, 7/8, 5/8, 9/16, 12/8, 3/2, 2/2, 6/8 '
         'and 5/4; 128th to half notes with up to double dots; 11 tuplet ratios (2:3 to 13:8) with rests '
         'inside; nested tuplets in three bars; chords; grace notes; ties across barlines; a second voice in '
         'eight bars.',
         'tools/fixtures/fx_dense.py: seeded random cells (MusicXML via mxl.py) -> `mscore -o`; the three '
         'nested-tuplet bars are written into the .mscx by mscxedit.py; app re-save.',
         'Source counts compared with the file: chord+rest count, the multiset of tuplet (actual, normal, '
         'base) in the mscx, ties, and the set of note values; every exported bar and voice sums to its time '
         'signature; 8 bars carry a second voice.')
def stress_dense():
    bars, v2, ties = _build()
    sc = Score('Dense rhythms')
    p = sc.add(Part('P1', 'Piano', sound='keyboard.piano', program=1))
    prev = None
    nested_items = fx_tuplets._nested_items()
    edits = {}
    for i, (ts, items) in enumerate(zip(TS_PLAN, bars)):
        bar = i + 1
        m = p.m(time=ts if ts != prev else None)
        prev = ts
        if bar in NESTED_BARS:
            m.v(1, items)
            edits[(1, bar)] = {0: nested_items[NESTED_BARS[bar]]}
        else:
            m.v(1, items)
        if bar in V2_BARS:
            m.v(2, v2[bar])
    _SOURCE['bars'], _SOURCE['v2'], _SOURCE['ties'], _SOURCE['nested'] = bars, v2, ties, nested_items
    return sc, edits


@check('stress-dense-rhythms')
def _(c):
    bars, v2, ties = _SOURCE['bars'], _SOURCE['v2'], _SOURCE['ties']
    nested = _SOURCE['nested']
    # source tuplet multiset and chord/rest count (voice 1 and 2, with nested bars from their own items)
    want_tup, want_cr = Counter(), 0
    all_items = []
    for i, items in enumerate(bars):
        bar = i + 1
        all_items.append(nested[NESTED_BARS[bar]] if bar in NESTED_BARS else items)
    all_items += list(v2.values())
    for items in all_items:
        for e in flat(items):
            if isinstance(e, Ev) and not e.chord:
                want_cr += 1                      # grace notes are Chords in the mscx too
                for t in e.tup:
                    if t[0] == 'start':
                        a, n = e.tm[t[1] - 1]
                        want_tup[(a, n, t[3])] += 1
    got_tup = Counter()
    for t in c.mscx.iter('Tuplet'):
        got_tup[(int(t.findtext('actualNotes')), int(t.findtext('normalNotes')), t.findtext('baseNote'))] += 1
    require(got_tup == want_tup, 'tuplets differ:\n got  %s\n want %s' % (sorted(got_tup.items()), sorted(want_tup.items())))
    cr = c.mscx_count('Chord') + c.mscx_count('Rest')
    require(cr == want_cr, 'chord+rest %d != source %d' % (cr, want_cr))
    # note values present
    types = Counter(d.text for d in c.mscx.iter('durationType'))
    for need in ('128th', '64th', '32nd', '16th', 'eighth', 'quarter', 'half'):
        require(types[need] > 0, 'no %s in file' % need)
    dots = Counter(d.text for d in c.mscx.iter('dots'))
    require(dots.get('2', 0) > 0, 'no double-dotted values')
    tp = tie_pairs(c.notes())
    require(len(tp) == ties, '%d tie pairs exported, %d in source' % (len(tp), ties))
    # bar sums per voice from the export
    div = int(c.xml.find('part/measure/attributes/divisions').text)
    cur = None
    nv2 = set()
    for m in c.measures():
        a = m.find('attributes')
        if a is not None and a.find('time') is not None:
            cur = (int(a.find('time/beats').text), int(a.find('time/beat-type').text))
        want_len = F(cur[0], cur[1]) * 4 * div
        by_voice = Counter()
        for n in m.findall('note'):
            if n.find('chord') is None and n.find('grace') is None:
                by_voice[n.findtext('voice')] += int(n.findtext('duration'))
        for v, tot in by_voice.items():
            require(tot == want_len, 'bar %s voice %s sums to %s, want %s' % (m.get('number'), v, tot, want_len))
        if '2' in by_voice:
            nv2.add(int(m.get('number')))
    require(nv2 == V2_BARS, 'voice 2 in bars %s' % sorted(nv2))
    return '%d chords+rests, %d tuplets (%d types), %d tie pairs, note values %s, double dots %d' % (
        cr, sum(got_tup.values()), len({(a, n) for a, n, _ in got_tup}), len(tp),
        sorted(types), dots.get('2', 0))
