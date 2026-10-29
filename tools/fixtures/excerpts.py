"""Add linked part scores (excerpts) to an .mscz that the app wrote (MuseScore 4.7 format).

In the 4.7 file format an excerpt is a separate score file in `Excerpts/<n>_<name>/`;
its elements carry their own `<eid>` and, when linked to the master, a
`<linkedTo>` holding the master element's EID (rw/write/twrite.cpp writeItemLink).
Here the excerpt is built by copying master staves and giving every copied element a
fresh EID plus `<linkedTo>` the master EID. The app then loads and re-saves the file.
"""
import copy
import json
import random
import re
import zipfile
import xml.etree.ElementTree as ET

CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
NO_LINK = {'Measure', 'Beam', 'VBox', 'HBox', 'TBox', 'FBox', 'Score'}
FRAMES = {'VBox', 'HBox', 'TBox', 'FBox'}


def _b64(n):
    out = ''
    while True:
        out += CHARS[n % 64]
        n //= 64
        if n == 0:
            return out


class EidMaker:
    def __init__(self, seed=470):
        self.rng = random.Random(seed)
        self.used = set()

    def new(self):
        while True:
            s = '%s_%s' % (_b64(self.rng.getrandbits(40) + 1), _b64(self.rng.getrandbits(40) + 1))
            if s not in self.used:
                self.used.add(s)
                return s


def relink(el, mk, link=True):
    """Give every element below `el` that has an <eid> a new one, and link it to the old."""
    for e in list(el.iter()):
        eid = e.find('eid')
        if eid is None or e.tag in ('Score',):
            continue
        old = eid.text
        eid.text = mk.new()
        if link and e.tag not in NO_LINK:
            idx = list(e).index(eid)
            lt = ET.Element('linkedTo')
            lt.text = old
            e.insert(idx + 1, lt)


def add_excerpts(master_path, out_path, specs):
    zin = zipfile.ZipFile(master_path)
    mname = [n for n in zin.namelist() if n.endswith('.mscx') and '/' not in n][0]
    root = ET.fromstring(zin.read(mname))
    score = root.find('Score')
    parts = score.findall('Part')
    staves = score.findall('Staff')
    # master staff index for each part
    first_staff = []
    n = 0
    for p in parts:
        first_staff.append(n)
        n += len(p.findall('Staff'))
    style = zin.read('score_style.mss')
    mk = EidMaker()
    mk.used.update(e.text for e in root.iter('eid'))
    files = {}
    for i, (name, pidx) in enumerate(specs):
        folder = '%d_%s' % (i, re.sub(r'[^A-Za-z0-9]+', '_', name).strip('_'))
        exroot = ET.Element('museScore', version=root.get('version'))
        for tag in ('programVersion', 'programRevision'):
            ET.SubElement(exroot, tag).text = root.findtext(tag)
        sc = ET.SubElement(exroot, 'Score')
        ET.SubElement(sc, 'eid').text = mk.new()
        ET.SubElement(sc, 'name').text = name
        ET.SubElement(sc, 'initialPartId').text = parts[pidx[0]].get('id')
        ET.SubElement(sc, 'Division').text = score.findtext('Division')
        for tag in ('showInvisible', 'showUnprintable', 'showFrames', 'showMargins', 'open'):
            ET.SubElement(sc, tag).text = score.findtext(tag) or '0'
        mt = ET.SubElement(sc, 'metaTag', name='partName')
        mt.text = name
        staff_no = 0
        body = []
        for pi in pidx:
            part = copy.deepcopy(parts[pi])
            for st in part.findall('Staff'):
                eid = st.find('eid')
                old = eid.text
                eid.text = mk.new()
                lt = ET.Element('linkedTo')
                lt.text = old
                st.insert(list(st).index(eid) + 1, lt)
            sc.append(part)
            for k in range(len(part.findall('Staff'))):
                src = staves[first_staff[pi] + k]
                cp = copy.deepcopy(src)
                staff_no += 1
                cp.set('id', str(staff_no))
                for fr in [c for c in cp if c.tag in FRAMES]:
                    cp.remove(fr)
                relink(cp, mk)
                body.append(cp)
        for b in body:
            sc.append(b)
        ET.indent(exroot, space='  ')
        files['Excerpts/%s/%s.mscx' % (folder, folder)] = (
            b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(exroot, encoding='utf-8'))
        files['Excerpts/%s/%s.mss' % (folder, folder)] = style
        files['Excerpts/%s/viewsettings.json' % folder] = json.dumps({'notation': {'viewMode': 'page'}}, indent=4).encode()
        tracks = []
        for pi2, p in enumerate(parts):
            if pi2 not in pidx:
                tracks.append({'instrumentId': p.find('Instrument').get('id'), 'partId': p.get('id'),
                               'soloMuteState': {'mute': True, 'solo': False}})
        files['Excerpts/%s/audiosettings.json' % folder] = json.dumps({'tracks': tracks}, indent=4).encode()
    names = zin.namelist()
    ordered = [n for n in names if n != 'META-INF/container.xml'] + sorted(files)
    root_files = [n for n in ordered if not n.endswith('/')]
    container = ('<?xml version="1.0" encoding="UTF-8"?>\n<container>\n  <rootfiles>\n' +
                 ''.join('    <rootfile full-path="%s"/>\n' % n for n in root_files) +
                 '    </rootfiles>\n  </container>\n').encode()
    with zipfile.ZipFile(out_path, 'w', zipfile.ZIP_DEFLATED) as zout:
        zout.writestr('META-INF/container.xml', container)
        for n in names:
            if n != 'META-INF/container.xml':
                zout.writestr(n, zin.read(n))
        for n, data in sorted(files.items()):
            zout.writestr(n, data)
