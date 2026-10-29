"""Build and verification pipeline for the synthetic fixtures.

Pipeline per fixture
  1. build:   MusicXML text (tools/fixtures/mxl.py) -> `mscore -o x.mscz x.musicxml`
              [-> optional .mscx edits (mscxedit.py) -> `mscore -o` again so the
              file stored is always written by the app]
              or a fully custom builder (hand-written .mscx, used for linked parts)
  2. resave:  `mscore -o again.mscz x.mscz` must succeed (no -f, so MuseScore's
              corruption check is live), print nothing alarming, and give a score
              with the same element counts.
  3. export:  `mscore -o x.musicxml x.mscz`, parsed with music21 and ElementTree,
              plus the fixture's own .mscx, then the fixture's check() asserts the
              feature survived.
"""
import os
import re
import shutil
import subprocess
import sys
import zipfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Callable, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MSCORE = os.environ.get(
    'MSCORE', '/Users/liamgreen/dev/musescore-agent/agent.install/mscore.app/Contents/MacOS/mscore')
OUT_DIR = os.path.join(ROOT, 'tests', 'fixtures', 'synthetic')

REGISTRY = {}
NOT_PRODUCED = []        # (what, why)
EXTRA_NOTES = []         # README paragraphs about what MuseScore does with these files
BAD_OUTPUT = re.compile(r'corrupt|error|fail|invalid|cannot|can\'t|unable|exception', re.I)


@dataclass
class Fixture:
    name: str
    feature: str
    produced: str
    verified: str
    build: Callable
    check: Optional[Callable] = None
    kind: str = 'musicxml'          # musicxml | custom


def fixture(name, feature, produced, verified, kind='musicxml'):
    def deco(fn):
        REGISTRY[name] = Fixture(name, feature, produced, verified, fn, None, kind)
        return fn
    return deco


def check(name):
    def deco(fn):
        REGISTRY[name].check = fn
        return fn
    return deco


class Fail(Exception):
    pass


def require(cond, msg):
    if not cond:
        raise Fail(msg)


def run_mscore(*args, timeout=180):
    for attempt in range(3):
        r = subprocess.run([MSCORE, *args], capture_output=True, text=True, timeout=timeout)
        if r.returncode >= 0:
            break
        # killed by a signal (seen once or twice per full run when other processes use the machine): retry
        print('  (mscore died with signal %d, retrying)' % -r.returncode, flush=True)
    out = (r.stdout or '') + (r.stderr or '')
    if r.returncode != 0:
        raise Fail('mscore %s -> rc=%d: %s' % (' '.join(os.path.basename(a) for a in args), r.returncode, out.strip()[:500]))
    if BAD_OUTPUT.search(out):
        raise Fail('mscore %s printed: %s' % (' '.join(os.path.basename(a) for a in args), out.strip()[:500]))
    return out


def mscx_name(z):
    return [n for n in z.namelist() if n.endswith('.mscx') and '/' not in n][0]


def fingerprint(mscz):
    """Element counts of the master score and every excerpt; used to compare a
    fixture with its own re-save."""
    z = zipfile.ZipFile(mscz)
    fp = {}
    for n in sorted(z.namelist()):
        if n.endswith('.mscx'):
            root = ET.fromstring(z.read(n))
            c = {}
            for tag in ('Measure', 'Chord', 'Rest', 'Note', 'Tuplet', 'Spanner', 'Harmony', 'Lyrics',
                        'Tie', 'LayoutBreak', 'KeySig', 'TimeSig', 'Staff', 'Part'):
                c[tag] = sum(1 for _ in root.iter(tag))
            fp[os.path.dirname(n) or '.'] = c
    return fp


class Ctx:
    def __init__(self, name, mscz, work):
        self.name, self.mscz, self.work = name, mscz, work
        self.rt_path = os.path.join(work, name + '.rt.musicxml')
        run_mscore('-o', self.rt_path, mscz)
        self.rt_text = open(self.rt_path, encoding='utf-8').read()
        self.xml = ET.fromstring(re.sub(r'<!DOCTYPE[^>]*>', '', self.rt_text, count=1).encode('utf-8'))
        self.zip = zipfile.ZipFile(mscz)
        self.mscx_text = self.zip.read(mscx_name(self.zip)).decode('utf-8')
        self.mscx = ET.fromstring(self.mscx_text.encode('utf-8'))
        self._m21 = None

    @property
    def m21(self):
        if self._m21 is None:
            from music21 import converter
            self._m21 = converter.parse(self.rt_path)
        return self._m21

    # --- helpers over the re-exported MusicXML -------------------------------
    def parts(self):
        return self.xml.findall('part')

    def measures(self, part=0):
        return self.parts()[part].findall('measure')

    def notes(self, part=0):
        """Flat list of dicts, one per <note> of the re-exported MusicXML."""
        out = []
        for m in self.measures(part):
            for n in m.findall('note'):
                d = {'m': m.get('number'), 'voice': int(n.findtext('voice', '1')),
                     'staff': int(n.findtext('staff', '1')), 'rest': n.find('rest') is not None,
                     'chord': n.find('chord') is not None, 'grace': n.find('grace') is not None,
                     'slash': (n.find('grace') is not None and n.find('grace').get('slash') == 'yes'),
                     'type': n.findtext('type'), 'dots': len(n.findall('dot'))}
                p = n.find('pitch')
                if p is not None:
                    alt = int(float(p.findtext('alter', '0')))
                    d['pitch'] = p.findtext('step') + {0: '', 1: '#', -1: 'b', 2: '##', -2: 'bb'}[alt] + p.findtext('octave')
                else:
                    d['pitch'] = None
                d['dur'] = int(n.findtext('duration', '0'))
                d['tie'] = [t.get('type') for t in n.findall('tie')]
                tm = n.find('time-modification')
                d['ratio'] = (int(tm.findtext('actual-notes')), int(tm.findtext('normal-notes'))) if tm is not None else None
                d['tuplets'] = [(t.get('type'), t.get('number')) for t in n.findall('notations/tuplet')]
                d['beams'] = {b.get('number'): b.text for b in n.findall('beam')}
                d['lyrics'] = [(l.get('number'), l.findtext('syllabic'), l.findtext('text'),
                                (l.find('extend').get('type') or 'yes') if l.find('extend') is not None else None)
                               for l in n.findall('lyric')]
                out.append(d)
        return out

    def mscx_count(self, tag):
        return sum(1 for _ in self.mscx.iter(tag))

    def mscx_staff(self, sid=1):
        return [s for s in self.mscx.find('Score').findall('Staff') if s.get('id') == str(sid)][0]


def fmt_fp(fp):
    return ', '.join('%s=%s' % (k, v) for k, v in sorted(next(iter(fp.values())).items()) if v)


def build_one(fx, work, out_dir, log):
    os.makedirs(work, exist_ok=True)
    final = os.path.join(out_dir, fx.name + '.mscz')
    if fx.kind == 'custom':
        raw = fx.build(work)
        shutil.copy(raw, final)
    else:
        res = fx.build()
        score, edits, extras = (res + (None, None))[:3] if isinstance(res, tuple) else (res, None, None)
        xml_path = os.path.join(work, fx.name + '.musicxml')
        with open(xml_path, 'w', encoding='utf-8') as f:
            f.write(score.to_xml())
        raw = os.path.join(work, fx.name + '-raw.mscz')
        run_mscore('-o', raw, xml_path)
        if edits or extras:
            import mscxedit
            inj = os.path.join(work, fx.name + '-inj.mscz')
            mscxedit.inject(raw, inj, edits, extras)
            raw2 = os.path.join(work, fx.name + '-fin.mscz')
            run_mscore('-o', raw2, inj)
            raw = raw2
        shutil.copy(raw, final)
    # re-save check
    again = os.path.join(work, fx.name + '-resave.mscz')
    run_mscore('-o', again, final)
    a, b = fingerprint(final), fingerprint(again)
    require(a == b, 're-save changed element counts: %s vs %s' % (a, b))
    ctx = Ctx(fx.name, final, work)
    summary = fx.check(ctx) if fx.check else ''
    log('  resave identical (%s)' % fmt_fp(a))
    return summary


def readme_text():
    lines = []
    lines.append('# Synthetic fixtures\n')
    lines.append('Small synthetic scores that exercise one notation feature each, plus two combined stress '
                 'scores. They are generated, not hand-engraved: regenerate everything with\n')
    lines.append('    ~/.venvs/MuseClaude/bin/python -I tools/fixtures/build_fixtures.py\n')
    lines.append('(needs the built agent app at `agent.install/mscore.app`; pass `--only NAME` for one '
                 'fixture, `--list` to list them). Every score is the app\'s own output: the generators '
                 'write MusicXML, or an edited .mscx where MusicXML cannot express the feature, and the '
                 'built app converts and re-saves it. Nothing here comes from `tests/fixtures/real/`.\n')
    lines.append('Each line: file, what it exercises. *Produced*: how. *Verified*: the check run by the '
                 'generator after every build, on top of the checks all fixtures get (open and re-save '
                 'with `mscore -o` without `-f`, same element counts after re-save, export to MusicXML '
                 'and parse with music21 + ElementTree).\n')
    for name in sorted(REGISTRY):
        fx = REGISTRY[name]
        lines.append('- `%s.mscz` — %s *Produced:* %s *Verified:* %s' % (name, fx.feature, fx.produced, fx.verified))
    lines.append('')
    lines.append('## Not produced, why\n')
    if NOT_PRODUCED:
        for what, why in NOT_PRODUCED:
            lines.append('- %s — %s' % (what, why))
    else:
        lines.append('- (nothing)')
    lines.append('')
    if EXTRA_NOTES:
        lines.append('## Things the fixtures showed about MuseScore\n')
        for t in EXTRA_NOTES:
            lines.append('- ' + t)
        lines.append('')
    return '\n'.join(lines)


def tie_pairs(notes):
    """[(start measure, stop measure, pitch)] from Ctx.notes() output."""
    open_ = {}
    pairs = []
    for n in notes:
        if n['pitch'] is None or n['grace']:
            continue
        key = (n['voice'], n['staff'], n['pitch'])
        if 'stop' in n['tie'] and key in open_:
            pairs.append((open_.pop(key), n['m'], n['pitch']))
        if 'start' in n['tie']:
            open_[key] = n['m']
    require(not open_, 'dangling tie starts: %s' % open_)
    return pairs


def mscx_tuplet_stacks(measure, voice=0):
    """For one <Measure> of an .mscx: the tuplet stack ((actual, normal, base), ...)
    in force at each chord/rest of one voice, in order."""
    stack, out = [], []
    v = measure.findall('voice')[voice]
    for el in v:
        if el.tag == 'Tuplet':
            stack.append((int(el.findtext('actualNotes')), int(el.findtext('normalNotes')), el.findtext('baseNote')))
        elif el.tag in ('Chord', 'Rest'):
            out.append(tuple(stack))
        elif el.tag == 'endTuplet':
            stack.pop()
    return out
