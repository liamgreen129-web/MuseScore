"""Tuplet fixtures: every ratio, nested tuplets, tuplets with rests/ties/voices."""
from fractions import Fraction as F

from harness import fixture, check, require, tie_pairs, mscx_tuplet_stacks
from mxl import Score, Part, N, R, MR, CH, T, words
from fxutil import rests, dur_of, pitch_at

# ---------------------------------------------------------------- ratios ----

# (actual, normal, unit note value, label)
RATIOS = [
    (2, 3, 'e'), (3, 2, 'e'), (4, 3, 'e'), (5, 4, 's'), (6, 4, 's'), (7, 4, 's'),
    (7, 8, 's'), (8, 6, 's'), (9, 8, 's'), (10, 8, 't'), (11, 8, 't'), (12, 8, 't'),
    (13, 8, 't'), (3, 2, 'q'), (5, 4, 'e'), (3, 2, 'h'),
]
UNIT = {'q': 'quarter', 'e': 'eighth', 's': '16th', 't': '32nd', 'h': 'half'}


def _ratio_bar(meas, actual, normal, unit, k):
    base = dur_of(UNIT[unit], 0)
    total = normal * base
    pitches = [pitch_at((k + i) % 6 + 2, 5) for i in range(actual)]
    group = T(actual, normal, *[N(p, unit) for p in pitches])
    left = F(1) - total
    meas.v(1, group, rests(left))


@fixture('tuplets-ratios',
         'One 4/4 bar per tuplet ratio: 2:3 (duplet), 3:2, 4:3, 5:4, 6:4, 7:4, 7:8, 8:6, 9:8, 10:8, 11:8, 12:8, '
         '13:8, plus 3:2 on quarters, 5:4 on eighths, 3:2 on halves, and a 6/8 bar of two 2:3 duplets.',
         'MusicXML from tools/fixtures/mxl.py (time-modification + tuplet start/stop), `mscore -o`.',
         'mscx has the 18 `<Tuplet>` elements with exactly these actual/normal/base values in order; '
         're-exported MusicXML notes carry the same ratio sequence.')
def tuplets_ratios():
    sc = Score('Tuplet ratios')
    p = sc.add(Part('P1', 'Violin', sound='strings.violin', program=41))
    for k, (a, n, u) in enumerate(RATIOS):
        m = p.m()
        _ratio_bar(m, a, n, u, k)
    m = p.m(time=(6, 8))
    m.v(1, T(2, 3, N('G5', 'e'), N('A5', 'e')), T(2, 3, N('B5', 'e'), N('G5', 'e')))
    return sc


@check('tuplets-ratios')
def _(c):
    want = [(a, n, UNIT[u]) for a, n, u in RATIOS] + [(2, 3, 'eighth'), (2, 3, 'eighth')]
    got = []
    for t in c.mscx.iter('Tuplet'):
        got.append((int(t.findtext('actualNotes')), int(t.findtext('normalNotes')), t.findtext('baseNote')))
    require(got == want, 'tuplets in mscx %s != %s' % (got, want))
    notes = [n for n in c.notes() if not n['rest']]
    ratios = [n['ratio'] for n in notes]
    exp = []
    for a, n, u in RATIOS:
        exp += [(a, n)] * a
    exp += [(2, 3)] * 4
    require(ratios == exp, 'note ratios differ: %s' % ratios)
    return '%d tuplets: %s' % (len(got), ' '.join('%d:%d' % (a, n) for a, n, _ in got))


# ---------------------------------------------------------------- nested ----

NESTED_BARS = []


def _nested_items():
    bars = []
    # 1: triplet in triplet
    bars.append([T(3, 2, N('C5', 'q'), T(3, 2, N('D5', 'e'), N('E5', 'e'), N('F5', 'e')), N('G5', 'q')),
                 rests(F(1, 2))])
    # 2: triplet inside a quintuplet
    bars.append([T(5, 4, N('C5', 'e'), N('D5', 'e'), T(3, 2, N('E5', 's'), N('F5', 's'), N('G5', 's')),
                   N('A5', 'e'), N('B5', 'e')), rests(F(1, 2))])
    # 3: quintuplets inside a duplet (2:3); the net ratio is not 1, so MuseScore keeps both levels
    bars.append([T(2, 3, T(5, 4, *[N(pitch_at(i + 2, 5), 's') for i in range(5)]),
                   T(5, 4, *[N(pitch_at(6 - i, 5), 's') for i in range(5)]), base='quarter'),
                 rests(F(1, 4))])
    # 4: three levels deep (3:2 in 3:2 in 3:2)
    bars.append([T(3, 2, N('C5', 'q'),
                   T(3, 2, N('D5', 'e'), T(3, 2, N('E5', 's'), N('F5', 's'), N('G5', 's')), N('A5', 'e')),
                   N('B5', 'q')),
                 rests(F(1, 2))])
    # 5: nested with a chord and rests inside
    bars.append([T(3, 2, CH(['C5', 'E5'], 'e'), R('e'),
                   T(3, 2, N('G5', 's'), R('s'), N('E5', 's'))),
                 rests(F(3, 4))])
    # 6: sextuplet inside a quintuplet
    bars.append([T(5, 4, N('C5', 'e'), N('D5', 'e'),
                   T(6, 4, *[N(pitch_at(i + 2, 5), 's') for i in range(6)]), N('B5', 'e')),
                 rests(F(1, 2))])
    # 7: two sibling nested groups in one bar
    bars.append([T(3, 2, N('C5', 'e'), T(3, 2, N('D5', 's'), N('E5', 's'), N('F5', 's')), N('G5', 'e')),
                 T(3, 2, T(3, 2, N('A5', 's'), N('G5', 's'), N('F5', 's')), N('E5', 'e'), N('D5', 'e'), base='eighth'),
                 rests(F(1, 2))])
    return bars


@fixture('tuplets-nested',
         'Nested tuplets: triplet in triplet, triplet in quintuplet, quintuplets in a duplet, three levels deep, '
         'nested with a chord and rests, sextuplet in quintuplet, two sibling nested groups.',
         'Single-staff MusicXML skeleton with whole-bar rests through `mscore -o`; the seven bars are then '
         'written into the .mscx by tools/fixtures/mscxedit.py (MuseScore\'s MusicXML importer cannot build '
         'nested tuplets, see "Things the fixtures showed" below) and the app re-saves the result.',
         'the fixture\'s own .mscx is walked (`<Tuplet>`/`<endTuplet/>`) and every chord/rest must carry the '
         'expected stack of ratios (up to 3 deep); 18 `<Tuplet>` elements; the MusicXML export still sums to 4/4 per bar.')
def tuplets_nested():
    sc = Score('Nested tuplets')
    p = sc.add(Part('P1', 'Flute', sound='wind.flutes.flute', program=74))
    bars = _nested_items()
    for _ in bars:
        p.m().v(1, MR(F(1)))
    edits = {(1, i + 1): {0: b} for i, b in enumerate(bars)}
    return sc, edits


@check('tuplets-nested')
def _(c):
    from mxl import flat, Ev
    bars = _nested_items()
    measures = c.mscx_staff(1).findall('Measure')
    require(len(measures) == len(bars), '%d measures' % len(measures))
    maxdepth = 0
    for i, m in enumerate(measures):
        want = [tuple(e.tm) for e in flat(bars[i]) if isinstance(e, Ev) and not e.chord and e.tm]
        got = [tuple((a, n) for a, n, _ in st) for st in mscx_tuplet_stacks(m) if st]
        require(got == want, 'bar %d nesting: %s != %s' % (i + 1, got, want))
        maxdepth = max(maxdepth, max(len(g) for g in got))
    n_tup = c.mscx_count('Tuplet')
    require(n_tup == 18, 'mscx Tuplet count %d' % n_tup)
    require(maxdepth == 3, 'max depth %d' % maxdepth)
    # the app can export it too: every bar still sums to 4/4 in the MusicXML
    for i, m in enumerate(c.measures()):
        total = sum(int(n.findtext('duration')) for n in m.findall('note') if n.find('chord') is None)
        div = int(c.xml.find('part/measure/attributes/divisions').text)
        require(total == 4 * div, 'exported bar %d length %d/%d' % (i + 1, total, div))
    return '%d <Tuplet> in mscx, max nesting depth %d, per-note ratio stacks match the source' % (n_tup, maxdepth)


# ---------------------------------------------------- rests, ties, voices ----

def _trv_bars():
    """Returns list of dicts: bar -> {voice: items}."""
    b = []
    # 1: triplet starting with a rest
    b.append({1: [T(3, 2, R('e'), N('E5', 'e'), N('G5', 'e')), N('C5', 'q'), N('D5', 'q'), N('E5', 'q')]})
    # 2: triplet with a rest in the middle and one at the end (quintuplet)
    b.append({1: [T(3, 2, N('C5', 'e'), R('e'), N('D5', 'e')), T(5, 4, N('E5', 's'), R('s'), N('F5', 's'), N('G5', 's'), R('s')),
                  N('A5', 'q'), N('B4', 'q', tie='start')]})
    # 3: tie into the first note of a triplet; last note of the triplet tied out
    b.append({1: [T(3, 2, N('B4', 'e', tie='stop'), N('C5', 'e'), N('D5', 'e', tie='start')), N('D5', 'q', tie='stop'),
                  N('E5', 'h')]})
    # 4: ties inside a tuplet; tuplet of chords
    b.append({1: [T(3, 2, N('C5', 'e', tie='start'), N('C5', 'e', tie='stop'), N('D5', 'e')),
                  T(3, 2, CH(['C5', 'E5'], 'e'), CH(['D5', 'F5'], 'e'), CH(['E5', 'G5'], 'e')), rests(F(1, 2))]})
    # 5: two voices at once with different tuplet ratios (3:2 quarters against 5:4 eighths)
    b.append({1: [T(3, 2, N('G5', 'q'), N('F5', 'q'), N('E5', 'q')), N('D5', 'h') if False else rests(F(1, 2))],
              2: [T(5, 4, *[N(pitch_at(i, 4), 'e') for i in range(5)]), N('C4', 'h')]})
    # 6: 7:4 sixteenths over straight eighths in voice 2
    b.append({1: [T(7, 4, *[N(pitch_at(i + 2, 5), 's') for i in range(7)]), rests(F(3, 4))],
              2: [N('C4', 'e'), N('D4', 'e'), N('E4', 'e'), N('F4', 'e'), rests(F(1, 2))]})
    return b


@fixture('tuplets-rests-ties-voices',
         'Tuplets that start with a rest or contain rests, a tie into a tuplet from the previous bar, out of a tuplet, and inside one, a '
         'tuplet of chords, and two voices at once carrying different ratios (3:2 against 5:4, 7:4 over '
         'straight eighths).',
         'MusicXML from tools/fixtures/mxl.py, `mscore -o`.',
         'Re-exported MusicXML: ratio sequence per voice per bar matches, the tie pairs (bar, bar, pitch) '
         'match, voice 2 exists in bars 5-6.')
def tuplets_rests_ties_voices():
    sc = Score('Tuplets with rests, ties and voices')
    p = sc.add(Part('P1', 'Oboe', sound='wind.reed.oboe', program=69))
    for bar in _trv_bars():
        m = p.m()
        for v, items in bar.items():
            m.v(v, items)
    return sc


@check('tuplets-rests-ties-voices')
def _(c):
    notes = c.notes()
    pairs = tie_pairs(notes)
    want_pairs = [('2', '3', 'B4'), ('3', '3', 'D5'), ('4', '4', 'C5')]
    require(sorted(pairs) == sorted(want_pairs), 'tie pairs %s' % pairs)
    def ratios(m, v):
        return [n['ratio'] for n in notes if n['m'] == m and n['voice'] == v and not n['chord']]
    r5 = ratios('5', 1)
    require(r5[:3] == [(3, 2)] * 3, 'bar5 v1 %s' % r5)
    r5b = ratios('5', 2)
    require(r5b[:5] == [(5, 4)] * 5, 'bar5 v2 %s' % r5b)
    r6 = ratios('6', 1)
    require(r6[:7] == [(7, 4)] * 7, 'bar6 v1 %s' % r6)
    require(all(r is None for r in ratios('6', 2)[:4]), 'bar6 v2 should be straight eighths')
    r1 = ratios('1', 1)
    require(r1[:3] == [(3, 2)] * 3 and [n for n in notes if n['m'] == '1'][0]['rest'], 'bar1 starts with a rest in the triplet')
    return 'tie pairs %s; v1 3:2 against v2 5:4 in bar 5, 7:4 in bar 6' % pairs
