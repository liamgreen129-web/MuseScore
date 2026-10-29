"""Small helpers shared by the fixture definitions."""
from fractions import Fraction as F

from mxl import TYPES, N, R, dur_of

LETTERS = 'CDEFGAB'
SEMI = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}

_DURS = []
for _t in ('whole', 'half', 'quarter', 'eighth', '16th', '32nd', '64th', '128th'):
    for _d in (3, 2, 1, 0):
        _DURS.append((dur_of(_t, _d), _t, _d))
_DURS.sort(key=lambda x: -x[0])
_SHORT = {'whole': 'w', 'half': 'h', 'quarter': 'q', 'eighth': 'e', '16th': 's',
          '32nd': 't', '64th': 'x', '128th': 'y'}


def dstr(frac):
    """Fraction of a whole note -> duration code ('q.', 'e', ...) if it is one."""
    for v, t, d in _DURS:
        if v == frac:
            return _SHORT[t] + '.' * d
    raise ValueError('not a single note value: %s' % frac)


def rests(frac, **kw):
    """Greedy rest decomposition of an exact length (no dotted values past one dot,
    to keep it readable)."""
    out = []
    frac = F(frac)
    cands = [(v, t, d) for v, t, d in _DURS if d <= 1]
    while frac > 0:
        for v, t, d in cands:
            if v <= frac:
                out.append(R(_SHORT[t] + '.' * d, **kw))
                frac -= v
                break
        else:
            raise ValueError('cannot fill %s with rests' % frac)
    return out


def pitch_at(step_idx, octave=4, alter=0):
    """Diatonic step index (0=C) -> pitch string, wrapping octaves."""
    o = octave + step_idx // 7
    s = LETTERS[step_idx % 7]
    return s + {0: '', 1: '#', -1: 'b', 2: '##', -2: 'bb'}[alter] + str(o)


def midi_of(p):
    import re
    m = re.match(r'^([A-G])(##|bb|#|b)?(-?\d)$', p)
    alt = {None: 0, '#': 1, 'b': -1, '##': 2, 'bb': -2}[m.group(2)]
    return 12 * (int(m.group(3)) + 1) + SEMI[m.group(1)] + alt


def written(p, diatonic, chromatic):
    """Concert pitch string -> written pitch string for an instrument whose
    MusicXML <transpose> has the given diatonic/chromatic values (negative =
    sounds lower than written, so written = concert - value)."""
    import re
    m = re.match(r'^([A-G])(##|bb|#|b)?(-?\d)$', p)
    step = LETTERS.index(m.group(1)) + 7 * int(m.group(3))
    wstep = step - diatonic
    wmidi = midi_of(p) - chromatic
    letter = LETTERS[wstep % 7]
    woct = wstep // 7
    nat = 12 * (woct + 1) + SEMI[letter]
    alt = wmidi - nat
    assert -2 <= alt <= 2, (p, diatonic, chromatic, alt)
    return letter + {0: '', 1: '#', -1: 'b', 2: '##', -2: 'bb'}[alt] + str(woct)


def fill(frac, maker, max_dots=1):
    """Decompose an exact length into single note values; maker(code) builds each event."""
    out = []
    frac = F(frac)
    cands = [(v, t, d) for v, t, d in _DURS if d <= max_dots]
    while frac > 0:
        for v, t, d in cands:
            if v <= frac:
                out.append(maker(_SHORT[t] + '.' * d))
                frac -= v
                break
        else:
            raise ValueError('cannot fill %s' % frac)
    return out


def eighths(frac, pitches):
    """Eighth notes running through `pitches` (cycled) for an exact length."""
    n = int(F(frac) / F(1, 8))
    assert F(n, 8) == F(frac), frac
    return [N(pitches[i % len(pitches)], 'e') for i in range(n)]
