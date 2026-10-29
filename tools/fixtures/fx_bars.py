"""Ties, system breaks, pickup bars and irregular bars."""
from fractions import Fraction as F

from harness import fixture, check, require, tie_pairs
from mxl import Score, Part, N, R, CH
from fxutil import pitch_at

# ------------------------------------------------------------------ ties ----


def _ties_bars():
    b = []
    b.append([N('C5', 'h'), N('D5', 'q'), N('E5', 'q', tie='start')])                 # 1
    b.append([N('E5', 'h', tie='stop'), CH(['C5', 'E5', 'G5'], 'h', tie='start')])     # 2 whole chord tied on
    b.append([CH(['C5', 'E5', 'G5'], 'h', tie='stop'), N('A4', 'q'), N('G4', 'q', tie='start')])  # 3
    b.append([N('G4', 'w', tie='both')])                                               # 4 chain
    # 5: only the C of the chord is tied on
    c5 = CH(['C5', 'E5'], 'h')
    c5[0].tie = 'start'
    b.append([N('G4', 'h', tie='stop'), c5])
    # 6: C tied in, dotted tied values inside the bar, B tied on
    c6 = CH(['C5', 'F5'], 'q')
    c6[0].tie = 'stop'
    b.append([c6, N('A4', 'q.', tie='start'), N('A4', 'e', tie='stop'), N('B4', 'q', tie='start')])
    b.append([N('B4', 'h', tie='stop'), N('D5', 'q', tie='start'), R('q')])           # 7 (4/4) -> 8 (3/4)
    b.append([N('D5', 'q.', tie='stop'), N('E5', 'e'), N('F5', 'q')])                  # 8
    return b


@fixture('ties-barlines',
         'Ties across barlines: a single note, a whole chord, a three-bar chain (tie in and out of the same '
         'note), a chord with only one note tied on, dotted tied values inside a bar, and a tie over a '
         'time-signature change.',
         'MusicXML from tools/fixtures/mxl.py (`<tie>` + `<tied>`), `mscore -o`.',
         'Re-exported MusicXML: the tie pairs (start bar, stop bar, pitch) match exactly; mscx has one '
         '`<Tie>` spanner per pair.')
def ties_barlines():
    sc = Score('Ties across barlines')
    p = sc.add(Part('P1', 'Clarinet', sound='wind.reed.clarinet', program=72))
    for i, items in enumerate(_ties_bars()):
        m = p.m(time=(3, 4)) if i == 7 else p.m()
        m.v(1, items)
    return sc


TIE_WANT = [('1', '2', 'E5'), ('2', '3', 'C5'), ('2', '3', 'E5'), ('2', '3', 'G5'), ('3', '4', 'G4'),
            ('4', '5', 'G4'), ('5', '6', 'C5'), ('6', '6', 'A4'), ('6', '7', 'B4'), ('7', '8', 'D5')]


@check('ties-barlines')
def _(c):
    pairs = tie_pairs(c.notes())
    require(sorted(pairs) == sorted(TIE_WANT), 'tie pairs %s' % sorted(pairs))
    ties = [sp for sp in c.mscx.iter('Spanner') if sp.get('type') == 'Tie' and sp.find('next') is not None]
    require(len(ties) == len(TIE_WANT), '%d tie spanners starting in mscx' % len(ties))
    return '%d tie pairs match: %s' % (len(pairs), ' '.join('%s>%s:%s' % p for p in sorted(pairs)))


def _break_bars():
    b = []
    for i in range(12):
        b.append([N(pitch_at(i % 5 + 2, 3), 'q') for _ in range(4)])
    # tie across the first system break (bar 3 -> 4)
    b[2][3].tie = 'start'
    b[3][0] = N(b[2][3].pitch, 'q', tie='stop')
    # chain across the second break (bar 7 -> 8 -> 9)
    b[6] = [N('C3', 'h'), N('E3', 'h', tie='start')]
    b[7] = [N('E3', 'w', tie='both')]
    b[8] = [N('E3', 'q', tie='stop'), N('D3', 'q'), N('C3', 'h')]
    # chord tie across the page break (bar 11 -> 12)
    b[10] = [N('G2', 'h'), CH(['C3', 'E3', 'G3'], 'h', tie='start')]
    b[11] = [CH(['C3', 'E3', 'G3'], 'w', tie='stop')]
    return b


@fixture('ties-system-break',
         'Ties that cross forced system breaks and a forced page break: a single note over a system break, '
         'a three-bar chain over a break, a three-note chord over a page break.',
         'MusicXML from tools/fixtures/mxl.py (`<print new-system>` / `new-page` on the bar after the '
         'tie starts), `mscore -o`.',
         'mscx: LayoutBreak line after bars 3 and 7, page after bar 11 (i.e. new system/page starts bars '
         '4, 8, 12); re-exported MusicXML tie pairs match and the tied bars straddle each break.')
def ties_system_break():
    sc = Score('Ties across system breaks')
    p = sc.add(Part('P1', 'Cello', sound='strings.cello', program=43, clefs=[('F', 4)]))
    for i, items in enumerate(_break_bars()):
        m = p.m(new_system=(i + 1 in (4, 8)), new_page=(i + 1 == 12))
        m.v(1, items)
    return sc


@check('ties-system-break')
def _(c):
    pairs = tie_pairs(c.notes())
    pitch3 = _break_bars()[2][3].pitch
    want = [('3', '4', pitch3), ('7', '8', 'E3'), ('8', '9', 'E3'), ('11', '12', 'C3'), ('11', '12', 'E3'),
            ('11', '12', 'G3')]
    require(sorted(pairs) == sorted(want), 'tie pairs %s' % sorted(pairs))
    breaks = {}
    for i, m in enumerate(c.mscx_staff(1).findall('Measure')):
        for lb in m.iter('LayoutBreak'):
            breaks[i + 1] = lb.findtext('subtype')
    require(breaks == {3: 'line', 7: 'line', 11: 'page'}, 'LayoutBreak by bar: %s' % breaks)
    return 'breaks after bars %s; ties crossing them: %s' % (breaks, ' '.join('%s>%s:%s' % p for p in sorted(pairs)))


# --------------------------------------------------------------- pickups ----

@fixture('pickup-bar',
         'Pickup (anacrusis) bar: a one-quarter bar 0 in 3/4, six full bars, and a short final bar that '
         'completes the pickup.',
         'MusicXML from tools/fixtures/mxl.py (`<measure number="0" implicit="yes">`, short last bar), '
         '`mscore -o`.',
         'mscx: first `<Measure>` has len=1/4 and the irregular (excluded from count) flag, last has '
         'len=2/4; music21 on the export: measure 0 is one quarter long, the next bar starts at offset 1, '
         'numbers run 0..7.')
def pickup_bar():
    sc = Score('Pickup bar')
    p = sc.add(Part('P1', 'Flute', sound='wind.flutes.flute', program=74, time=(3, 4)))
    p.m(number=0, implicit=True, length=F(1, 4)).v(1, N('G4', 'q'))
    for i in range(6):
        p.m().v(1, N(pitch_at(i % 3 * 2 + 2, 4), 'h'), N(pitch_at(i % 3 * 2 + 1, 4), 'q'))
    p.m(length=F(1, 2)).v(1, N('C5', 'h'))
    return sc


@check('pickup-bar')
def _(c):
    meas = c.mscx_staff(1).findall('Measure')
    require(meas[0].get('len') == '1/4', 'first len %s' % meas[0].get('len'))
    require(meas[0].find('irregular') is not None, 'pickup not flagged irregular')
    require(meas[-1].get('len') == '2/4', 'last len %s' % meas[-1].get('len'))
    ms = list(c.m21.parts[0].getElementsByClass('Measure'))
    require(ms[0].number == 0 and ms[0].highestTime == 1.0, 'm21 pickup %s %s' % (ms[0].number, ms[0].highestTime))
    require(ms[1].offset == 1.0, 'bar 1 offset %s' % ms[1].offset)
    nums = [m.number for m in ms]
    require(nums == list(range(0, 8)), 'numbers %s' % nums)
    return 'bar numbers %s; pickup 1 quarter, final bar len 2/4' % nums


@fixture('pickup-bar-compound',
         'Pickup bar in compound time: 6/8 in G with a two-eighth pickup, and a second, mid-piece short bar '
         '(four eighths, excluded from the count) after a double barline.',
         'MusicXML from tools/fixtures/mxl.py, `mscore -o`.',
         'mscx: first Measure len=2/8 (two eighths) with the irregular flag; the mid-piece short bar has '
         'len=4/8 and the irregular flag; no other measure carries a len.')
def pickup_compound():
    sc = Score('Pickup bar, compound time')
    p = sc.add(Part('P1', 'Violin', sound='strings.violin', program=41, time=(6, 8), key=1))
    p.m(number=0, implicit=True, length=F(2, 8)).v(1, N('D5', 'e'), N('E5', 'e'))
    for _ in range(3):
        p.m().v(1, N('F#5', 'q.'), N('E5', 'e'), N('D5', 'e'), N('B4', 'e'))
    p.m(right='<barline location="right"><bar-style>light-light</bar-style></barline>').v(1, N('A4', 'q.'), N('B4', 'q.'))
    p.m(length=F(4, 8), implicit=True, number=5).v(1, N('D5', 'e'), N('E5', 'e'), N('F#5', 'q'))
    for _ in range(2):
        p.m().v(1, N('G5', 'q.'), N('F#5', 'e'), N('E5', 'e'), N('D5', 'e'))
    p.m().v(1, N('D5', 'q.'), R('q.'))
    return sc


@check('pickup-bar-compound')
def _(c):
    meas = c.mscx_staff(1).findall('Measure')
    lens = [m.get('len') for m in meas]
    require(lens[0] == '2/8' and meas[0].find('irregular') is not None, 'first %s' % lens)
    require(lens[5] == '4/8' and meas[5].find('irregular') is not None, 'mid-piece short bar %s' % lens)
    require(sum(1 for l in lens if l) == 2, 'unexpected other irregular bars %s' % lens)
    return 'measure len attributes %s' % lens


# -------------------------------------------------------- irregular bars ----

@fixture('irregular-bars',
         'Irregular (actual-duration) bars in 4/4: a bar of 3/4, a bar of 5/8, an overfull bar of 6/4, a '
         'bar split in two halves (second half excluded from the count), and a 1/8 bar.',
         'MusicXML from tools/fixtures/mxl.py (measures whose content differs from the time signature), '
         '`mscore -o`.',
         'mscx: the `<Measure len=...>` attributes are exactly [-, 3/4, 5/8, 6/4, 2/4, 2/4, 1/8, -] and the '
         'split half carries the irregular flag.')
def irregular_bars():
    sc = Score('Irregular bars')
    p = sc.add(Part('P1', 'Oboe', sound='wind.reed.oboe', program=69))
    p.m().v(1, N('C5', 'q'), N('D5', 'q'), N('E5', 'q'), N('F5', 'q'))                                  # full
    p.m(length=F(3, 4)).v(1, N('G5', 'q'), N('A5', 'q'), N('B5', 'q'))                                  # 3/4
    p.m(length=F(5, 8)).v(1, N('C6', 'q'), N('B5', 'e'), N('A5', 'q'))                                  # 5/8
    p.m(length=F(3, 2)).v(1, N('G5', 'q'), N('F5', 'q'), N('E5', 'q'), N('D5', 'q'), N('C5', 'h'))      # 6/4
    p.m(length=F(1, 2)).v(1, N('E5', 'q'), N('G5', 'q'))                                                # split: first half
    p.m(length=F(1, 2), implicit=True, number=5).v(1, N('C6', 'h'))                                     # split: second half
    p.m(length=F(1, 8)).v(1, N('B5', 'e'))                                                              # 1/8
    p.m().v(1, N('C6', 'w'))                                                                            # full
    return sc


@check('irregular-bars')
def _(c):
    meas = c.mscx_staff(1).findall('Measure')
    lens = [m.get('len') for m in meas]
    want = [None, '3/4', '5/8', '6/4', '2/4', '2/4', '1/8', None]
    require(lens == want, 'lens %s != %s' % (lens, want))
    require(meas[5].find('irregular') is not None, 'split half not flagged')
    return 'measure len attributes %s' % lens
