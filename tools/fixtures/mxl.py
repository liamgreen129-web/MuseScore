"""Tiny MusicXML 4.0 writer used to build the synthetic fixtures.

Design notes
- Durations are exact Fractions of a whole note. The XML <divisions> value is
  computed at the end as the smallest number that makes every duration an
  integer, so any tuplet ratio (7:8, 11:8, 13:8, nested ...) is representable.
- Every voice line in a measure is checked to add up to the bar length, so a
  fixture that is internally inconsistent fails here, not inside MuseScore.
- Only what the corpus needs is implemented.
"""
from fractions import Fraction as F
from math import lcm
import re
from xml.sax.saxutils import escape, quoteattr

TYPES = {'breve': F(2), 'whole': F(1), 'half': F(1, 2), 'quarter': F(1, 4),
         'eighth': F(1, 8), '16th': F(1, 16), '32nd': F(1, 32),
         '64th': F(1, 64), '128th': F(1, 128)}
SHORT = {'b': 'breve', 'w': 'whole', 'h': 'half', 'q': 'quarter', 'e': 'eighth',
         's': '16th', 't': '32nd', 'x': '64th', 'y': '128th'}


def parse_dur(d):
    """'q', 'e.', 'h..' -> (type name, dots)."""
    dots = len(d) - len(d.rstrip('.'))
    base = d.rstrip('.')
    return SHORT.get(base, base), dots


def dur_of(typ, dots):
    v = TYPES[typ]
    return v * (2 - F(1, 2 ** dots))


def ph(fr):
    return '@@%d/%d@@' % (fr.numerator, fr.denominator)


class Raw:
    """Zero-duration XML (direction, harmony, sound ...) placed in a voice line."""
    def __init__(self, xml):
        self.xml = xml


class Ev:
    def __init__(self, pitch, d, **kw):
        self.pitch = pitch               # 'C#4' or None for a rest
        self.type, self.dots = parse_dur(d) if d else (None, 0)
        self.chord = False
        self.tm = []                     # [(actual, normal)] outermost first
        self.tup = []                    # [[kind, number]]
        self.tie = kw.get('tie')         # None | start | stop | both
        self.grace = kw.get('grace')     # None | slash | plain
        self.slurs = list(kw.get('slur', []))  # [('start', 1)]
        self.ly = list(kw.get('ly', []))  # [(verse, syllabic, text, extend)]
        self.notations = list(kw.get('nt', []))  # raw xml inside <notations>
        self.beam = kw.get('beam')       # None | b | c | e (level 1)
        self.staff = kw.get('staff')
        self.voice = kw.get('voice')
        self.stem = kw.get('stem')
        self.measure_rest = kw.get('measure', False)
        self.dur_override = kw.get('dur')
        self.accidental = kw.get('acc')
        self.notehead = kw.get('head')
        self.print_object = kw.get('hidden', False)

    @property
    def dur(self):
        if self.dur_override is not None:
            return self.dur_override
        v = dur_of(self.type, self.dots)
        for a, n in self.tm:
            v = v * n / a
        return v


def N(pitch, d, **kw):
    return Ev(pitch, d, **kw)


def R(d, **kw):
    return Ev(None, d, **kw)


def MR(frac, **kw):
    """Whole-measure rest of the given length."""
    return Ev(None, None, measure=True, dur=frac, **kw)


def CH(pitches, d, **kw):
    """Chord. Keyword args apply to the first note; tie applies to all."""
    out = []
    for i, p in enumerate(pitches):
        e = Ev(p, d, **(kw if i == 0 else {'tie': kw.get('tie'), 'staff': kw.get('staff'), 'grace': kw.get('grace')}))
        e.chord = i > 0
        out.append(e)
    return out


def flat(items):
    out = []
    for it in items:
        if isinstance(it, (list, tuple)):
            out.extend(flat(it))
        else:
            out.append(it)
    return out


def T(actual, normal, *items, bracket=True, base=None):
    """Tuplet: `actual` notes in the time of `normal`. Nestable."""
    evs = flat(items)
    leaders = [e for e in evs if isinstance(e, Ev) and not e.chord]
    for e in evs:
        if isinstance(e, Ev):
            e.tm.insert(0, (actual, normal))
            for m in e.tup:
                m[1] += 1
    leaders[0].tup.insert(0, ['start', 1, bracket, base or leaders[0].type])
    leaders[-1].tup.append(['stop', 1, bracket, None])
    return evs


# --- directions and friends -------------------------------------------------

def _dir(inner, placement='above', staff=None, sound='', offset=None):
    parts = inner if isinstance(inner, (list, tuple)) else [inner]
    s = '<direction placement="%s">' % placement + ''.join('<direction-type>%s</direction-type>' % x for x in parts)
    if offset is not None:
        s += '<offset>%s</offset>' % ph(offset)
    if staff:
        s += '<staff>%d</staff>' % staff
    s += sound + '</direction>'
    return Raw(s)


def words(txt, placement='above', staff=None, bold=False):
    attr = ' font-weight="bold"' if bold else ''
    return _dir('<words%s>%s</words>' % (attr, escape(txt)), placement, staff)


def dyn(name, staff=None, placement='below'):
    return _dir('<dynamics><%s/></dynamics>' % name, placement, staff)


def wedge(kind, n=1, staff=None):
    return _dir('<wedge type="%s" number="%d"/>' % (kind, n), 'below', staff)


def octshift(kind, size=8, n=1, staff=None):
    k = kind if kind != 'stop' else 'stop'
    return _dir('<octave-shift type="%s" size="%d" number="%d"/>' % (k, size, n), 'above', staff)


def pedal(kind, staff=None):
    return _dir('<pedal type="%s" line="yes"/>' % kind, 'below', staff)


def bracket(kind, n=1, staff=None, text=None):
    b = '<bracket type="%s" number="%d" line-end="%s" line-type="solid"/>' % (
        kind, n, 'none' if kind == 'stop' else 'down')
    if kind == 'start' and text:
        return _dir(['<words>%s</words>' % escape(text), b], 'above', staff)
    return _dir(b, 'above', staff)


def tempo(bpm, txt=None, beat='quarter'):
    inner = []
    if txt:
        inner.append('<words>%s</words>' % escape(txt))
    inner.append('<metronome><beat-unit>%s</beat-unit><per-minute>%d</per-minute></metronome>' % (beat, bpm))
    return _dir(inner, 'above', None, '<sound tempo="%d"/>' % bpm)


def segno(staff=None):
    return _dir('<segno/>', 'above', staff, '<sound segno="segno"/>')


def coda(staff=None):
    return _dir('<coda/>', 'above', staff, '<sound coda="coda"/>')


def jump(txt, **soundattrs):
    s = '<sound %s/>' % ' '.join('%s="%s"' % kv for kv in soundattrs.items())
    return _dir('<words>%s</words>' % escape(txt), 'above', None, s)


def rehearsal(txt):
    return _dir('<rehearsal>%s</rehearsal>' % escape(txt), 'above')


def H(root, kind, bass=None, text=None, degrees=(), staff=None, offset=None):
    """Chord symbol. root/bass like 'C', 'F#', 'Bb'. degrees: [('add', 9, 0)] etc."""
    def step_alter(x):
        step = x[0]
        alt = {'#': 1, 'b': -1}.get(x[1:2], 0) if len(x) > 1 else 0
        if len(x) > 2:
            alt = 2 if x[1:] == '##' else (-2 if x[1:] == 'bb' else alt)
        return step, alt
    rs, ra = step_alter(root)
    s = '<harmony><root><root-step>%s</root-step>' % rs
    if ra:
        s += '<root-alter>%d</root-alter>' % ra
    s += '</root><kind%s>%s</kind>' % (' text=%s' % quoteattr(text) if text is not None else '', kind)
    if bass:
        bs, ba = step_alter(bass)
        s += '<bass><bass-step>%s</bass-step>' % bs
        if ba:
            s += '<bass-alter>%d</bass-alter>' % ba
        s += '</bass>'
    for typ, val, alt in degrees:
        s += ('<degree><degree-value>%d</degree-value><degree-alter>%d</degree-alter>'
              '<degree-type>%s</degree-type></degree>' % (val, alt, typ))
    if offset is not None:
        s += '<offset>%s</offset>' % ph(offset)
    if staff:
        s += '<staff>%d</staff>' % staff
    s += '</harmony>'
    return Raw(s)


# --- measures, parts, score -------------------------------------------------

KEYMODES = ('major', 'minor')


def key_xml(fifths, mode='major'):
    return '<key><fifths>%d</fifths><mode>%s</mode></key>' % (fifths, mode)


def time_xml(n, d=None):
    if n == 'C':
        return '<time symbol="common"><beats>4</beats><beat-type>4</beat-type></time>'
    if n == 'C/':
        return '<time symbol="cut"><beats>2</beats><beat-type>2</beat-type></time>'
    return '<time><beats>%d</beats><beat-type>%d</beat-type></time>' % (n, d)


def clef_xml(sign, line, number=None, octave=None):
    s = '<clef%s><sign>%s</sign><line>%d</line>' % (' number="%d"' % number if number else '', sign, line)
    if octave:
        s += '<clef-octave-change>%d</clef-octave-change>' % octave
    return s + '</clef>'


class Measure:
    def __init__(self, part, number, **kw):
        self.part = part
        self.number = number
        self.kw = kw
        self.lines = []        # (voice, staff, items)

    def v(self, voice, *items, staff=None):
        self.lines.append((voice, staff or 1, flat(items)))
        return self

    def line_len(self, items):
        t = F(0)
        for it in items:
            if isinstance(it, Ev) and not it.chord and not it.grace:
                t += it.dur
        return t


class Part:
    def __init__(self, pid, name, *, abbr=None, sound=None, program=1, staves=1,
                 clefs=None, transpose=None, key=0, mode='major', time=(4, 4),
                 channel=1, instrument_name=None):
        self.pid, self.name, self.abbr = pid, name, abbr or name[:4] + '.'
        self.sound, self.program, self.staves = sound, program, staves
        self.clefs = clefs or ([('G', 2)] if staves == 1 else [('G', 2), ('F', 4)])
        self.transpose = transpose           # (diatonic, chromatic, octave_change)
        self.key, self.mode, self.time = key, mode, time
        self.channel = channel
        self.instrument_name = instrument_name or name
        self.measures = []
        self.cur_time = time

    def m(self, *, number=None, implicit=False, length=None, new_system=False,
          new_page=False, key=None, mode=None, time=None, clefs=None,
          left='', right='', pre=(), width=None):
        first = not self.measures
        if number is None:
            number = getattr(self, 'next_number', 1)
        self.next_number = number + 1
        if time is not None:
            self.cur_time = time
        meas = Measure(self, number)
        meas.implicit, meas.new_system, meas.new_page = implicit, new_system, new_page
        meas.first = first
        meas.key, meas.mode, meas.time, meas.clefs = key, mode, time, clefs
        meas.left, meas.right, meas.pre, meas.width = left, right, list(pre), width
        if length is not None:
            meas.length = length
        else:
            n, d = self.cur_time if self.cur_time[0] not in ('C', 'C/') else ((4, 4) if self.cur_time[0] == 'C' else (2, 2))
            meas.length = F(n, d)
        self.measures.append(meas)
        return meas


def _note_xml(e, staves_in_part, voice, staff):
    s = '<note>'
    if e.grace:
        s += '<grace slash="yes"/>' if e.grace == 'slash' else '<grace/>'
    if e.chord:
        s += '<chord/>'
    if e.pitch is None:
        s += '<rest measure="yes"/>' if e.measure_rest else '<rest/>'
    else:
        m = re.match(r'^([A-G])(##|bb|#|b)?(-?\d)$', e.pitch)
        assert m, e.pitch
        alt = {'#': 1, 'b': -1, '##': 2, 'bb': -2, None: 0}[m.group(2)]
        s += '<pitch><step>%s</step>' % m.group(1)
        if alt:
            s += '<alter>%d</alter>' % alt
        s += '<octave>%s</octave></pitch>' % m.group(3)
    if not e.grace:
        s += '<duration>%s</duration>' % ph(e.dur)
    if e.tie in ('stop', 'both'):
        s += '<tie type="stop"/>'
    if e.tie in ('start', 'both'):
        s += '<tie type="start"/>'
    s += '<voice>%d</voice>' % voice
    if e.type:
        s += '<type>%s</type>' % e.type
        s += '<dot/>' * e.dots
    if e.accidental:
        s += '<accidental>%s</accidental>' % e.accidental
    if e.tm:
        a = 1
        n = 1
        for x, y in e.tm:
            a *= x
            n *= y
        s += '<time-modification><actual-notes>%d</actual-notes><normal-notes>%d</normal-notes>' % (a, n)
        s += '</time-modification>'
    if e.stem:
        s += '<stem>%s</stem>' % e.stem
    if e.notehead:
        s += '<notehead>%s</notehead>' % e.notehead
    if staves_in_part > 1:
        s += '<staff>%d</staff>' % staff
    if e.beam:
        levels = e.beam if isinstance(e.beam, tuple) else (e.beam,)
        for lv, b in enumerate(levels, 1):
            st = {'b': 'begin', 'c': 'continue', 'e': 'end', 'f': 'forward hook', 'k': 'backward hook'}[b]
            s += '<beam number="%d">%s</beam>' % (lv, st)
    nt = ''
    if e.tie in ('stop', 'both'):
        nt += '<tied type="stop"/>'
    if e.tie in ('start', 'both'):
        nt += '<tied type="start"/>'
    for kind, num in e.slurs:
        nt += '<slur type="%s" number="%d"/>' % (kind, num)
    for kind, num, brk, _base in e.tup:
        if kind == 'start':
            nt += '<tuplet type="start" number="%d" bracket="%s"/>' % (num, 'yes' if brk else 'no')
        else:
            nt += '<tuplet type="stop" number="%d"/>' % num
    nt += ''.join(e.notations)
    if nt:
        s += '<notations>%s</notations>' % nt
    for verse, syl, text, ext in e.ly:
        s += '<lyric number="%d">' % verse
        if text is not None:
            s += '<syllabic>%s</syllabic><text>%s</text>' % (syl, escape(text))
        if ext:
            s += '<extend type="%s"/>' % ext
        s += '</lyric>'
    s += '</note>'
    return s


class Score:
    def __init__(self, title, composer='Synthetic fixture', parts=None, groups=()):
        self.title, self.composer = title, composer
        self.parts = parts or []
        self.groups = groups          # [(first_pid, last_pid, symbol)]

    def add(self, p):
        self.parts.append(p)
        return p

    def to_xml(self):
        nmeas = {len(p.measures) for p in self.parts}
        assert len(nmeas) == 1, 'parts differ in measure count: %s' % nmeas
        out = ['<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
               '<!DOCTYPE score-partwise PUBLIC "-//Recordare//DTD MusicXML 4.0 Partwise//EN" '
               '"http://www.musicxml.org/dtds/partwise.dtd">\n'
               '<score-partwise version="4.0">\n'
               '<work><work-title>%s</work-title></work>\n'
               '<identification><creator type="composer">%s</creator>'
               '<encoding><software>tools/fixtures</software></encoding></identification>\n'
               '<part-list>' % (escape(self.title), escape(self.composer))]
        gi = 1
        starts = {a: (b, sym) for a, b, sym in self.groups}
        ends = {}
        for a, b, sym in self.groups:
            ends[b] = gi
        for p in self.parts:
            if p.pid in starts:
                out.append('<part-group type="start" number="%d"><group-symbol>%s</group-symbol>'
                           '<group-barline>yes</group-barline></part-group>' % (gi, starts[p.pid][1]))
            out.append('<score-part id="%s"><part-name>%s</part-name>'
                       '<part-abbreviation>%s</part-abbreviation>' % (p.pid, escape(p.name), escape(p.abbr)))
            iid = p.pid + '-I1'
            out.append('<score-instrument id="%s"><instrument-name>%s</instrument-name>' % (iid, escape(p.instrument_name)))
            if p.sound:
                out.append('<instrument-sound>%s</instrument-sound>' % p.sound)
            out.append('</score-instrument>')
            out.append('<midi-instrument id="%s"><midi-channel>%d</midi-channel>'
                       '<midi-program>%d</midi-program></midi-instrument></score-part>' % (iid, p.channel, p.program))
            if p.pid in ends:
                out.append('<part-group type="stop" number="%d"/>' % ends[p.pid])
        out.append('</part-list>\n')
        for p in self.parts:
            out.append('<part id="%s">\n' % p.pid)
            for m in p.measures:
                out.append(self._measure_xml(p, m))
            out.append('</part>\n')
        out.append('</score-partwise>\n')
        xml = ''.join(out)
        return finalize(xml)

    def _measure_xml(self, p, m):
        attrs = ''
        if m.first or m.key is not None or m.time is not None or m.clefs is not None:
            a = ''
            if m.first:
                a += '<divisions>@@DIV@@</divisions>'
            if m.first:
                a += key_xml(m.key if m.key is not None else p.key, m.mode or p.mode)
            elif m.key is not None:
                a += key_xml(m.key, m.mode or 'major')
            if m.first:
                a += time_xml(*(m.time or p.time))
            elif m.time is not None:
                a += time_xml(*m.time)
            if m.first and p.staves > 1:
                a += '<staves>%d</staves>' % p.staves
            clefs = m.clefs if m.clefs is not None else (p.clefs if m.first else None)
            if clefs:
                for i, c in enumerate(clefs):
                    sign, line = c[0], c[1]
                    octv = c[2] if len(c) > 2 else None
                    num = (c[3] if len(c) > 3 else i + 1) if p.staves > 1 else None
                    a += clef_xml(sign, line, num, octv)
            if m.first and p.transpose:
                dia, chrom, octc = (list(p.transpose) + [0])[:3]
                a += '<transpose><diatonic>%d</diatonic><chromatic>%d</chromatic>' % (dia, chrom)
                if octc:
                    a += '<octave-change>%d</octave-change>' % octc
                a += '</transpose>'
            attrs = '<attributes>%s</attributes>' % a
        s = '<measure number=%s%s%s>' % (quoteattr(str(m.number)),
                                        ' implicit="yes"' if m.implicit else '',
                                        ' width="%s"' % m.width if m.width else '')
        if m.new_system or m.new_page:
            s += '<print%s%s/>' % (' new-system="yes"' if m.new_system else '',
                                    ' new-page="yes"' if m.new_page else '')
        if m.left:
            s += m.left
        s += attrs
        for r in m.pre:
            s += r.xml
        lines = m.lines
        assert lines, 'measure %s of %s has no lines' % (m.number, p.pid)
        for i, (voice, staff, items) in enumerate(lines):
            ln = m.line_len(items)
            if ln != m.length:
                raise AssertionError('%s m%s voice %d: length %s != bar %s' % (p.pid, m.number, voice, ln, m.length))
            for it in items:
                if isinstance(it, Raw):
                    s += it.xml
                else:
                    st = it.staff or staff
                    s += _note_xml(it, p.staves, voice, st)
            if i < len(lines) - 1:
                s += '<backup><duration>%s</duration></backup>' % ph(ln)
        if m.right:
            s += m.right
        s += '</measure>\n'
        return s


def finalize(xml):
    fr = {F(int(a), int(b)) for a, b in re.findall(r'@@(\d+)/(\d+)@@', xml)}
    div = 1
    for f in fr:
        div = lcm(div, (f * 4).denominator)
    div = max(div, 1)
    # keep numbers readable but still integers for every duration
    xml = xml.replace('@@DIV@@', str(div))
    return re.sub(r'@@(\d+)/(\d+)@@',
                  lambda mo: str(int(F(int(mo.group(1)), int(mo.group(2))) * 4 * div)), xml)


# --- barline helpers --------------------------------------------------------

def repeat_fwd():
    return '<barline location="left"><bar-style>heavy-light</bar-style><repeat direction="forward"/></barline>'


def repeat_back(times=None):
    t = ' times="%d"' % times if times else ''
    return '<barline location="right"><bar-style>light-heavy</bar-style><repeat direction="backward"%s/></barline>' % t


def ending_start(num):
    return '<barline location="left"><ending number="%s" type="start"/></barline>' % num


def ending_stop(num, closed=True, repeat=False):
    t = 'stop' if closed else 'discontinue'
    bs = '<bar-style>light-heavy</bar-style>' if repeat else ''
    rep = '<repeat direction="backward"/>' if repeat else ''
    return '<barline location="right">%s<ending number="%s" type="%s"/>%s</barline>' % (bs, num, t, rep)


def barline(style):
    return '<barline location="right"><bar-style>%s</bar-style></barline>' % style
