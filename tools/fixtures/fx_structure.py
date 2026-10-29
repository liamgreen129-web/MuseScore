"""Repeats and voltas, jumps and markers, spanners crossing system breaks."""
from fractions import Fraction as F

from harness import fixture, check, require
from mxl import (Score, Part, N, R, CH, Raw, words, dyn, wedge, octshift, pedal, bracket, segno, coda, jump,
                 rehearsal, tempo)
from fxutil import pitch_at, rests

# ------------------------------------------------------ repeats and voltas ----


def bl(loc, style=None, ending=None, rep=None, times=None):
    """One <barline>: ending=(number, start|stop|discontinue), rep=forward|backward."""
    s = '<barline location="%s">' % loc
    if style:
        s += '<bar-style>%s</bar-style>' % style
    if ending:
        s += '<ending number="%s" type="%s"/>' % ending
    if rep:
        s += '<repeat direction="%s"%s/>' % (rep, ' times="%d"' % times if times else '')
    return s + '</barline>'


REPEAT_BARS = {
    1: dict(left=bl('left', 'heavy-light', rep='forward')),
    3: dict(left=bl('left', ending=('1', 'start')), right=bl('right', 'light-heavy', ending=('1', 'stop'), rep='backward')),
    4: dict(left=bl('left', ending=('2', 'start')), right=bl('right', 'light-light', ending=('2', 'discontinue'))),
    5: dict(left=bl('left', 'heavy-light', rep='forward')),
    6: dict(right=bl('right', 'light-heavy', rep='backward', times=3)),
    7: dict(left=bl('left', 'heavy-light', rep='forward')),
    8: dict(left=bl('left', ending=('1, 2', 'start')), right=bl('right', 'light-heavy', ending=('1, 2', 'stop'), rep='backward')),
    9: dict(left=bl('left', ending=('3', 'start')), right=bl('right', ending=('3', 'stop'))),
    10: dict(right=bl('right', 'light-heavy')),
}


@fixture('repeats-voltas',
         'Repeats and voltas in ten 4/4 bars: a plain repeat with first and second endings (bars 1-4), a '
         'repeat played three times (5-6) running straight into another repeat (7-9, an end-start repeat '
         'barline), an ending labelled "1, 2" and a third ending, then a final barline.',
         'MusicXML from tools/fixtures/mxl.py (`<barline>` with `<repeat>`/`<ending>`, `times="3"`), `mscore -o`.',
         'mscx: start-repeat flags on bars 1/5/7, end-repeat counts (2, 3, 2) on bars 3/6/8, and Volta '
         'spanners with endings "1", "2", "1,2", "3" starting on bars 3/4/8/9; re-exported MusicXML has the '
         'same repeat directions and ending numbers (1, 2, "1, 2", 3).')
def repeats_voltas():
    sc = Score('Repeats and voltas')
    p = sc.add(Part('P1', 'Piano', sound='keyboard.piano', program=1))
    for bar in range(1, 11):
        kw = REPEAT_BARS.get(bar, {})
        m = p.m(**kw)
        m.v(1, [N(pitch_at((bar + i) % 5 + 2, 4), 'q') for i in range(4)])
    return sc


def _volta_info(c):
    out = []
    for i, m in enumerate(c.mscx_staff(1).findall('Measure')):
        for sp in m.iter('Spanner'):
            if sp.get('type') == 'Volta':
                v = sp.find('Volta')
                if v is not None:
                    out.append((i + 1, v.findtext('endings')))
    return out


@check('repeats-voltas')
def _(c):
    meas = c.mscx_staff(1).findall('Measure')
    starts = [i + 1 for i, m in enumerate(meas) if m.find('startRepeat') is not None]
    ends = {i + 1: int(m.findtext('endRepeat')) for i, m in enumerate(meas) if m.find('endRepeat') is not None}
    voltas = _volta_info(c)
    require(starts == [1, 5, 7], 'start repeats %s' % starts)
    require(ends == {3: 2, 6: 3, 8: 2}, 'end repeats %s' % ends)
    require(voltas == [(3, '1'), (4, '2'), (8, '1,2'), (9, '3')], 'voltas %s' % voltas)
    x_rep = [(m.get('number'), r.get('direction')) for m in c.measures() for r in m.iter('repeat')]
    x_end = [(m.get('number'), e.get('number'), e.get('type')) for m in c.measures() for e in m.iter('ending')]
    require([r for r in x_rep if r[1] == 'forward'] == [('1', 'forward'), ('5', 'forward'), ('7', 'forward')],
            'exported forward repeats %s' % x_rep)
    require([r[0] for r in x_rep if r[1] == 'backward'] == ['3', '6', '8'], 'exported backward repeats %s' % x_rep)
    require([e[1] for e in x_end if e[2] == 'start'] == ['1', '2', '1, 2', '3'], 'exported ending numbers %s' % x_end)
    return 'start repeats %s, end repeat counts %s, voltas %s' % (starts, ends, voltas)


# ------------------------------------------------------- jumps and markers ----


MARKERS = {   # bar -> xml of a Marker/Jump measure element, as MuseScore writes them
    3: ('Marker', 'segno', '<Marker><style>repeat_left</style><text><sym>segno</sym></text>'
                           '<label>segno</label><markerType>segno</markerType></Marker>'),
    5: ('Marker', 'coda', '<Marker><style>repeat_right</style><text>To Coda</text>'
                          '<label>coda</label><markerType>coda</markerType></Marker>'),
    6: ('Jump', 'D.S. al Coda', '<Jump><style>repeat_right</style><text>D.S. al Coda</text><jumpTo>segno</jumpTo>'
                                '<playUntil>coda</playUntil><continueAt>codab</continueAt></Jump>'),
    7: ('Marker', 'codab', '<Marker><style>repeat_left</style><text><sym>coda</sym></text>'
                           '<label>codab</label><markerType>codab</markerType></Marker>'),
    11: ('Marker', 'fine', '<Marker><style>repeat_right</style><text>Fine</text>'
                           '<label>fine</label><markerType>fine</markerType></Marker>'),
    12: ('Jump', 'D.C. al Fine', '<Jump><style>repeat_right</style><text>D.C. al Fine</text><jumpTo>start</jumpTo>'
                                 '<playUntil>fine</playUntil><continueAt></continueAt></Jump>'),
}


@fixture('jumps-markers',
         'Navigation markers and jumps: segno (bar 3), "To Coda" (5), "D.S. al Coda" (6), coda sign (7), '
         '"Fine" (11) and "D.C. al Fine" (12), with a rehearsal mark and a tempo mark in bar 1.',
         'MusicXML skeleton from tools/fixtures/mxl.py through `mscore -o`; the Marker and Jump measure '
         'elements are then inserted into the .mscx by tools/fixtures/mscxedit.py (the MusicXML importer turns '
         '`<segno/>`, `<coda/>` and `<sound dalsegno=...>` into plain staff text, not real markers) and the '
         'app re-saves the file.',
         'mscx: Marker labels segno/coda/codab/fine and Jump texts sit on exactly the intended bars, and the '
         'jump targets are segno->coda->codab and start->fine.')
def jumps_markers():
    sc = Score('Jumps and markers')
    p = sc.add(Part('P1', 'Piano', sound='keyboard.piano', program=1))
    for bar in range(1, 13):
        pre = [rehearsal('A'), tempo(100, 'Allegro')] if bar == 1 else []
        m = p.m(pre=pre)
        m.v(1, [N(pitch_at((bar + i) % 5 + 2, 4), 'q') for i in range(4)])
    extras = {(1, bar): [x[2]] for bar, x in MARKERS.items()}
    return sc, None, extras


@check('jumps-markers')
def _(c):
    got = {}
    for i, m in enumerate(c.mscx_staff(1).findall('Measure')):
        for el in m:
            if el.tag == 'Marker':
                got[i + 1] = ('Marker', el.findtext('label'))
            elif el.tag == 'Jump':
                got[i + 1] = ('Jump', el.findtext('text'))
    want = {bar: (x[0], x[1]) for bar, x in MARKERS.items()}
    require(got == want, 'markers/jumps by bar:\n got  %s\n want %s' % (got, want))
    jumps = {j.findtext('text'): (j.findtext('jumpTo'), j.findtext('playUntil'), j.findtext('continueAt'))
             for j in c.mscx.iter('Jump')}
    require(jumps == {'D.S. al Coda': ('segno', 'coda', 'codab'), 'D.C. al Fine': ('start', 'fine', None)} or
            jumps == {'D.S. al Coda': ('segno', 'coda', 'codab'), 'D.C. al Fine': ('start', 'fine', '')}, 'jump targets %s' % jumps)
    return 'markers/jumps by bar %s; jump targets %s' % (got, jumps)


# ------------------------------------------- spanners across system breaks ----

BREAKS = (4, 7, 10)           # bars that start a new system
# kind -> list of ((start bar, beat), (stop bar, beat))
SPANS = {
    'slur': [((3, 1), (5, 0)), ((2, 0), (11, 3))],
    'wedge': [((2, 0), (4, 2)), ((8, 0), (11, 1))],
    'octave-shift': [((5, 0), (8, 1))],
    'pedal': [((1, 0), (6, 2))],
    'trill': [((6, 1), (8, 1))],
    'bracket': [((8, 2), (11, 2))],
    'glissando': [((3, 3), (4, 0))],
}
ENDINGS = {9: ('1', 'start'), 10: ('1', 'stop'), 11: ('2', 'start'), 12: ('2', 'discontinue')}


def _span_events():
    """(bar, beat) -> {'pre': [Raw], 'slur': [...], 'nt': [...]} for the right hand."""
    ev = {}

    def at(key):
        return ev.setdefault(key, {'pre': [], 'slur': [], 'nt': []})
    for i, (a, b) in enumerate(SPANS['slur']):
        at(a)['slur'].append(('start', i + 1))
        at(b)['slur'].append(('stop', i + 1))
    for i, (a, b) in enumerate(SPANS['wedge']):
        at(a)['pre'].append(wedge('crescendo' if i == 0 else 'diminuendo', i + 1))
        at(b)['pre'].append(wedge('stop', i + 1))
    a, b = SPANS['octave-shift'][0]
    at(a)['pre'].append(octshift('down', 8, 1))      # MusicXML "down" = 8va (written an octave lower)
    at(b)['pre'].append(octshift('stop', 8, 1))
    a, b = SPANS['trill'][0]
    at(a)['nt'].append('<ornaments><trill-mark/><wavy-line type="start" number="1"/></ornaments>')
    at(b)['nt'].append('<ornaments><wavy-line type="stop" number="1"/></ornaments>')
    a, b = SPANS['bracket'][0]
    at(a)['pre'].append(bracket('start', 1, text='rit.'))
    at(b)['pre'].append(bracket('stop', 1))
    a, b = SPANS['glissando'][0]
    at(a)['nt'].append('<glissando type="start" line-type="wavy" number="1"/>')
    at(b)['nt'].append('<glissando type="stop" number="1"/>')
    return ev


@fixture('spanners-system-break',
         'Spanners that cross forced system breaks (new system at bars 4, 7 and 10 of 12): two slurs (one '
         'running over three breaks), crescendo and diminuendo hairpins, an 8va line, a pedal line on the '
         'left-hand staff, a trill line, a "rit." text line, a glissando between bars 3 and 4, and two '
         'voltas, the first split by a break.',
         'MusicXML from tools/fixtures/mxl.py (slur, wedge, octave-shift, pedal, wavy-line, bracket, glissando, '
         'ending, `<print new-system>`), `mscore -o`.',
         'Re-exported MusicXML: every spanner starts and stops in the bars given; each one (except the second volta) straddles at '
         'least one system break; mscx has LayoutBreak on bars 3/6/9 and lists the spanner types present.')
def spanners_system_break():
    sc = Score('Spanners across system breaks')
    p = sc.add(Part('P1', 'Piano', sound='keyboard.piano', program=1, staves=2))
    ev = _span_events()
    pedal_pre = {SPANS['pedal'][0][0]: pedal('start', staff=2), SPANS['pedal'][0][1]: pedal('stop', staff=2)}
    for bar in range(1, 13):
        kw = {}
        if bar in BREAKS:
            kw['new_system'] = True
        if bar == 7:
            kw['left'] = bl('left', 'heavy-light', rep='forward')
        if bar in ENDINGS:
            num, typ = ENDINGS[bar]
            if typ == 'start':
                kw['left'] = bl('left', ending=(num, 'start'))
            else:
                kw['right'] = bl('right', 'light-heavy' if typ == 'stop' else None, ending=(num, typ),
                                 rep='backward' if typ == 'stop' else None)
        m = p.m(**kw)
        rh, lh = [], []
        for beat in range(4):
            e = ev.get((bar, beat), {'pre': [], 'slur': [], 'nt': []})
            rh += e['pre']
            rh.append(N(pitch_at((bar + beat) % 6 + 2, 4), 'q', slur=e['slur'], nt=e['nt']))
        for beat in (0, 2):
            if (bar, beat) in pedal_pre:
                lh.append(pedal_pre[(bar, beat)])
            lh.append(N(pitch_at((bar + beat) % 4, 3), 'h'))
        m.v(1, rh, staff=1)
        m.v(5, lh, staff=2)
    return sc


def _xml_spans(c):
    """kind -> list of (start bar, stop bar) from the re-exported MusicXML (paired by number)."""
    open_ = {}
    out = {}

    def feed(kind, num, typ, bar):
        key = (kind, num)
        if typ in ('start', 'crescendo', 'diminuendo', 'up', 'down'):
            open_[key] = bar
        elif typ in ('stop', 'discontinue') and key in open_:
            out.setdefault(kind, []).append((open_.pop(key), bar))
    for m in c.measures():
        bar = int(m.get('number'))
        for n in m.findall('note'):
            for s in n.iter('slur'):
                feed('slur', s.get('number'), s.get('type'), bar)
            for s in n.iter('wavy-line'):
                feed('trill', s.get('number'), s.get('type'), bar)
            for s in n.iter('glissando'):
                feed('glissando', s.get('number'), s.get('type'), bar)
            for s in n.iter('slide'):
                feed('glissando', s.get('number'), s.get('type'), bar)
        for d in m.findall('direction'):
            for s in d.iter('wedge'):
                feed('wedge', s.get('number'), s.get('type'), bar)
            for s in d.iter('octave-shift'):
                feed('octave-shift', s.get('number'), s.get('type'), bar)
            for s in d.iter('pedal'):
                feed('pedal', s.get('number', '1'), s.get('type'), bar)
            for s in d.iter('bracket'):
                feed('bracket', s.get('number'), s.get('type'), bar)
        for e in m.iter('ending'):
            feed('ending', e.get('number'), e.get('type'), bar)
    return out


@check('spanners-system-break')
def _(c):
    got = _xml_spans(c)
    want = {k: sorted((a[0], b[0]) for a, b in v) for k, v in SPANS.items()}
    want['ending'] = [(9, 10), (11, 12)]
    got = {k: sorted(v) for k, v in got.items()}
    require(got == want, 'spanner bars differ:\n got  %s\n want %s' % (got, want))
    for kind, spans in got.items():
        for a, b in spans:
            if (kind, a, b) == ('ending', 11, 12):      # the second volta sits inside one system
                continue
            require(any(a < br <= b for br in BREAKS), '%s %s-%s does not cross a system break' % (kind, a, b))
    breaks = {}
    for i, m in enumerate(c.mscx_staff(1).findall('Measure')):
        for lb in m.iter('LayoutBreak'):
            breaks[i + 1] = lb.findtext('subtype')
    require(breaks == {3: 'line', 6: 'line', 9: 'line'}, 'LayoutBreak by bar: %s' % breaks)
    kinds = {}
    for sp in c.mscx.iter('Spanner'):
        kinds[sp.get('type')] = kinds.get(sp.get('type'), 0) + 1
    return 'spanner (start bar, stop bar) pairs %s all cross a break; mscx spanner types %s' % (got, kinds)
