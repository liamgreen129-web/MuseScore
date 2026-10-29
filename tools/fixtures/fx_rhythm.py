"""Voices, cross-staff beaming, grace notes, ties, pickups, irregular bars."""
from fractions import Fraction as F

from harness import fixture, check, require, tie_pairs
from mxl import Score, Part, N, R, MR, CH, T, words, dyn
from fxutil import rests, pitch_at

# ---------------------------------------------------------- four voices ----


def _voices_bars():
    bars = []
    # 1: four different rhythms at once
    bars.append({
        1: [N(p, 'e', stem='up') for p in ('E6', 'D6', 'C6', 'D6', 'E6', 'G6', 'E6', 'D6')],
        2: [N(p, 'q', stem='down') for p in ('A5', 'G5', 'F5', 'G5')],
        3: [N('E5', 'h', stem='up'), N('D5', 'h', stem='up')],
        4: [N('C5', 'w', stem='down')],
    })
    # 2: rests, a whole-bar rest and dotted rhythms
    bars.append({
        1: [CH(['C6', 'E6'], 'q', stem='up'), R('q'), CH(['D6', 'F6'], 'h', stem='up')],
        2: [R('h'), N('G5', 'q.', stem='down'), N('F5', 'e', stem='down')],
        3: [N('E5', 'q.', stem='up'), N('D5', 'e', stem='up'), R('h')],
        4: [MR(F(1))],
    })
    # 3: triplets, ties and chords in different voices
    bars.append({
        1: [T(3, 2, N('G6', 'e', stem='up'), N('F6', 'e', stem='up'), N('E6', 'e', stem='up')) for _ in range(4)],
        2: [N('B5', 'h', stem='down', tie='start'), N('B5', 'q', stem='down', tie='stop'), R('q')],
        3: [N('F5', 'q', stem='up'), R('q'), CH(['E5', 'G5'], 'h', stem='up')],
        4: [N('C5', 'q', stem='down'), N('D5', 'q', stem='down'), N('C5', 'q', stem='down'), N('B4', 'q', stem='down')],
    })
    # 4: all four voices in the same rhythm
    bars.append({
        1: [N('G6', 'w', stem='up')],
        2: [N('E6', 'w', stem='down')],
        3: [N('C6', 'w', stem='up')],
        4: [N('G5', 'w', stem='down')],
    })
    return bars


@fixture('voices-four',
         'Four voices on one treble staff: four different rhythms at once, rests and a whole-bar rest in '
         'voice 4, triplets, a tie and chords in separate voices, then all four voices in whole notes.',
         'MusicXML from tools/fixtures/mxl.py (`<voice>` 1-4 with `<backup>`), `mscore -o`.',
         'mscx bars 1-4 each contain four `<voice>` elements; re-exported MusicXML has voices 1-4 in the '
         'same staff with the right note counts per voice per bar.')
def voices_four():
    sc = Score('Four voices on one staff')
    p = sc.add(Part('P1', 'Piano', sound='keyboard.piano', program=1))
    for bar in _voices_bars():
        m = p.m()
        for v in (1, 2, 3, 4):
            m.v(v, bar[v])
    return sc


@check('voices-four')
def _(c):
    meas = c.mscx_staff(1).findall('Measure')
    require(len(meas) == 4, '%d measures' % len(meas))
    for i, m in enumerate(meas):
        nv = len(m.findall('voice'))
        require(nv == 4, 'bar %d has %d voices in mscx' % (i + 1, nv))
    notes = [n for n in c.notes() if not n['rest'] and not n['chord']]
    want = {('1', 1): 8, ('1', 2): 4, ('1', 3): 2, ('1', 4): 1, ('4', 1): 1, ('4', 2): 1, ('4', 3): 1, ('4', 4): 1,
            ('3', 1): 12, ('3', 4): 4}
    for (m, v), cnt in want.items():
        got = sum(1 for n in notes if n['m'] == m and n['voice'] == v)
        require(got == cnt, 'bar %s voice %d: %d notes, want %d' % (m, v, got, cnt))
    voices = sorted({n['voice'] for n in c.notes()})
    require(voices == [1, 2, 3, 4], 'voices %s' % voices)
    return 'voices %s; bar 1 note counts per voice 8/4/2/1; every bar has 4 mscx voices' % voices


# ------------------------------------------------------ cross-staff beams ----

_XS = {}


def _xs_bars():
    bars = []
    # 1: eighth arpeggio, voice 1 (staff 1), the low notes sit on staff 2
    p1 = ['C3', 'G3', 'C4', 'E4', 'G3', 'C4', 'E4', 'G4']
    s1 = [2, 2, 1, 1, 2, 1, 1, 1]
    b1 = [N(p, 'e', staff=s, beam=b) for p, s, b in zip(p1, s1, 'bccebcce')]
    bars.append({'rh': b1, 'lh': [N('C2', 'h'), N('G2', 'h')]})
    # 2: left hand (voice 5, staff 2) reaching up to staff 1
    p2 = ['C3', 'E3', 'G3', 'C4', 'G3', 'E3', 'C4', 'E4']
    s2 = [2, 2, 1, 1, 2, 2, 1, 1]
    lh = [N(p, 'e', staff=s, beam=b) for p, s, b in zip(p2, s2, 'bccebcce')]
    bars.append({'rh': [N('E5', 'h'), N('G5', 'h')], 'lh': lh})
    # 3: sixteenth figure, two beam levels, alternating hands
    p3 = ['C4', 'E4', 'G4', 'E4', 'C4', 'E4', 'G4', 'E4', 'A3', 'C4', 'E4', 'C4', 'A3', 'C4', 'E4', 'C4']
    s3 = [2, 1, 1, 1, 2, 1, 1, 1, 2, 2, 1, 2, 2, 1, 1, 2]
    st = [('b', 'b'), ('c', 'c'), ('c', 'c'), ('e', 'e')] * 4
    b3 = [N(p, 's', staff=s, beam=bm) for p, s, bm in zip(p3, s3, st)]
    bars.append({'rh': b3, 'lh': [N('A2', 'h'), N('F2', 'h')]})
    # 4: crossing quarter notes without a beam, then a beamed pair
    bars.append({'rh': [N('C4', 'q', staff=2), N('E4', 'q', staff=2), N('G4', 'e', staff=1, beam='b'),
                        N('C4', 'e', staff=2, beam='e'), R('q')],
                 'lh': [N('C3', 'w')]})
    return bars


@fixture('cross-staff-beaming',
         'Piano with cross-staff beams: right-hand arpeggio whose low notes sit on the left-hand staff, '
         'left hand reaching up, a two-level sixteenth figure alternating staves, and a crossing pair.',
         'MusicXML from tools/fixtures/mxl.py (a voice\'s notes carry `<staff>` 1 or 2 plus explicit beams), '
         '`mscore -o`.',
         'mscx has `<staffMove>` on exactly the notes moved; re-exported MusicXML keeps one voice on both '
         'staffs and puts notes of both staffs in the same beam group.')
def cross_staff_beaming():
    sc = Score('Cross-staff beaming')
    p = sc.add(Part('P1', 'Piano', sound='keyboard.piano', program=1, staves=2))
    moved = 0
    for bar in _xs_bars():
        m = p.m()
        m.v(1, bar['rh'], staff=1)
        m.v(5, bar['lh'], staff=2)
        for e in bar['rh']:
            if hasattr(e, 'staff') and e.staff == 2:
                moved += 1
        for e in bar['lh']:
            if hasattr(e, 'staff') and e.staff == 1:
                moved += 1
    _XS['moved'] = moved
    return sc


@check('cross-staff-beaming')
def _(c):
    moved = _XS['moved']
    chords = [ch for ch in c.mscx.iter('Chord') if ch.find('staffMove') is not None]
    require(len(chords) == moved, '%d chords with staffMove in mscx, expected %d' % (len(chords), moved))
    notes = c.notes()
    v1 = [n for n in notes if n['voice'] == 1 and not n['rest']]
    require({n['staff'] for n in v1} == {1, 2}, 'voice 1 staves %s' % {n['staff'] for n in v1})
    # a beam group (begin..end at level 1) holding notes of both staves
    mixed, group = 0, []
    for n in notes:
        if n['rest'] or not n['beams']:
            continue
        b = n['beams'].get('1')
        if b == 'begin':
            group = [n['staff']]
        elif group:
            group.append(n['staff'])
            if b == 'end':
                if len(set(group)) > 1:
                    mixed += 1
                group = []
    require(mixed >= 3, 'only %d beam groups cross staves' % mixed)
    return '%d chords carry staffMove (expected %d); %d beam groups span both staves' % (len(chords), moved, mixed)


# ----------------------------------------------------------- grace notes ----

def _grace_bars():
    bars = []
    # 1: acciaccatura (slashed eighth) before a quarter, three plain quarters
    bars.append([N('D5', 'e', grace='slash'), N('C5', 'q'), N('E5', 'q'), N('G5', 'q'), N('E5', 'q')])
    # 2: appoggiatura (unslashed eighth) before a half note
    bars.append([N('B4', 'e', grace='plain'), N('C5', 'h'), N('E5', 'h')])
    # 3: two sixteenth graces, and three thirty-second graces
    bars.append([N('B4', 's', grace='plain'), N('C5', 's', grace='plain'), N('D5', 'h'),
                 N('F5', 't', grace='plain'), N('G5', 't', grace='plain'), N('A5', 't', grace='plain'), N('E5', 'h')])
    # 4: grace note before a chord, grace chord before a note, slurred grace
    bars.append([N('F5', 'e', grace='slash'), CH(['C5', 'E5', 'G5'], 'h'),
                 CH(['A4', 'C5'], 's', grace='plain'), N('B4', 'h'),
                 ])
    # 5: grace notes after a note, at the end of the bar
    bars.append([N('C5', 'q'), N('D5', 'q'), N('E5', 'q'), N('G5', 'q'),
                 N('A5', 's', grace='plain'), N('G5', 's', grace='plain')])
    # 6: grace after, then next bar's grace before: a single sixteenth after, a slashed eighth before
    bars.append([N('E5', 'h'), N('C5', 'h'), N('D5', 's', grace='plain')])
    bars.append([N('B4', 'e', grace='slash'), N('C5', 'w')])
    return bars


@fixture('grace-notes',
         'Grace notes before and after the beat: acciaccatura (slashed eighth), appoggiatura (plain eighth), '
         'groups of two 16th and three 32nd graces, graces before a chord and a grace chord, and graces '
         'after the last note of a bar.',
         'MusicXML from tools/fixtures/mxl.py (`<grace/>` with and without slash; graces written after the '
         'main note are the "after" ones), `mscore -o`.',
         'mscx: counts of acciaccatura / appoggiatura / grace16 / grace32 / grace16after chords match; '
         're-exported MusicXML has the same number of `<grace>` notes and the after-graces follow their note.')
def grace_notes():
    sc = Score('Grace notes')
    p = sc.add(Part('P1', 'Violin', sound='strings.violin', program=41))
    for items in _grace_bars():
        p.m().v(1, items)
    return sc


@check('grace-notes')
def _(c):
    kinds = {}
    for ch in c.mscx.iter('Chord'):
        for k in ('acciaccatura', 'appoggiatura', 'grace4', 'grace8after', 'grace16', 'grace16after',
                  'grace32', 'grace32after', 'grace8'):
            if ch.find(k) is not None:
                kinds[k] = kinds.get(k, 0) + 1
    want = {'acciaccatura': 3, 'appoggiatura': 1, 'grace16': 3, 'grace32': 3, 'grace16after': 3}
    require(kinds == want, 'grace chords in mscx %s != %s' % (kinds, want))
    notes = c.notes()
    n_grace = sum(1 for n in notes if n['grace'])
    require(n_grace == 14, '%d grace notes in export' % n_grace)
    slashed = sum(1 for n in notes if n['grace'] and n['slash'])
    require(slashed == 3, '%d slashed graces in export' % slashed)
    for m in ('5', '6'):
        seq = [n for n in notes if n['m'] == m]
        last_main = max(i for i, n in enumerate(seq) if not n['grace'])
        require(all(n['grace'] for n in seq[last_main + 1:]) and seq[last_main + 1:], 'bar %s: graces do not follow the last note' % m)
    first_main = {m: [i for i, n in enumerate([x for x in notes if x['m'] == m]) if not n['grace']][0] for m in ('1', '3', '7')}
    require(first_main == {'1': 1, '3': 2, '7': 1}, 'graces before: %s' % first_main)
    return 'mscx grace chords %s; %d grace notes (3 slashed) in export; bar 5-6 graces follow their note, bars 1/3/7 precede' % (kinds, n_grace)
