"""Linked parts / excerpts. MusicXML cannot express them, so the excerpt scores are
written into the .mscz container by tools/fixtures/excerpts.py from the master the
app produced, then the app opens and re-saves the whole thing."""
import os
import shutil
import zipfile
import xml.etree.ElementTree as ET
from fractions import Fraction as F

from harness import fixture, check, require, run_mscore, mscx_name
from mxl import Score, Part, N, R, CH, dyn, words, MR
from fxutil import pitch_at

EXCERPTS = [('Flute', [0]), ('Clarinet in Bb', [1]), ('Flute and Clarinet', [0, 1])]


def master_score():
    sc = Score('Linked parts')
    fl = sc.add(Part('P1', 'Flute', sound='wind.flutes.flute', program=74))
    cl = sc.add(Part('P2', 'Clarinet in Bb', sound='wind.reed.clarinet.bflat', program=72, transpose=(-1, -2, 0), key=2))
    bn = sc.add(Part('P3', 'Bassoon', sound='wind.reed.bassoon', program=71, clefs=[('F', 4)]))
    # flute: slur over bars 1-2, a tie from 2 to 3, a dynamic and a chord
    fl.m().v(1, dyn('mf'), N('E5', 'q', slur=[('start', 1)]), N('F5', 'q'), N('G5', 'q'), N('A5', 'q'))
    fl.m().v(1, N('G5', 'q'), N('E5', 'q', slur=[('stop', 1)]), CH(['C5', 'E5'], 'q'), N('D5', 'q', tie='start'))
    fl.m().v(1, N('D5', 'h', tie='stop'), N('C5', 'h'))
    fl.m().v(1, N('B4', 'w'))
    clpitch = lambda p, d: N(p, d)
    cl.m().v(1, R('q'), N('F#5', 'q'), N('A5', 'q'), R('q'))
    cl.m().v(1, N('B5', 'h'), N('A5', 'q'), N('F#5', 'q'))
    cl.m().v(1, words('dolce'), N('G5', 'q'), N('E5', 'q'), N('F#5', 'h'))
    cl.m().v(1, N('D5', 'w'))
    bn.m().v(1, N('C3', 'w'))
    bn.m().v(1, N('G2', 'h'), N('C3', 'h'))
    bn.m().v(1, N('B2', 'h'), N('A2', 'h'))
    bn.m().v(1, N('G2', 'w'))
    return sc


@fixture('parts-linked',
         'Linked parts: a flute / B-flat clarinet / bassoon master (ties, slur, dynamic, staff text, chord, '
         'transposing clarinet) with three part scores (excerpts) linked to it: Flute, Clarinet in Bb, and a '
         'two-staff "Flute and Clarinet" part. The bassoon has no part.',
         'Master from MusicXML through `mscore -o`; the three excerpt scores are generated from the master\'s '
         '.mscx by tools/fixtures/excerpts.py (copied staves, new EIDs, `<linkedTo>` the master element), '
         'zipped into the same .mscz, and the app opens and re-saves the file.',
         'After the app re-save: three `Excerpts/*.mscx` exist with the right names; every `<linkedTo>` in them '
         'names an EID present in the master; every note in every part is linked; the part scores hold the '
         'same pitches as their master staves; the clarinet part keeps its transposition.',
         kind='custom')
def parts_linked(work):
    import excerpts
    sc = master_score()
    xml = os.path.join(work, 'parts-linked.musicxml')
    with open(xml, 'w', encoding='utf-8') as f:
        f.write(sc.to_xml())
    master = os.path.join(work, 'parts-linked-master.mscz')
    run_mscore('-o', master, xml)
    built = os.path.join(work, 'parts-linked-built.mscz')
    excerpts.add_excerpts(master, built, EXCERPTS)
    final = os.path.join(work, 'parts-linked-fin.mscz')
    run_mscore('-o', final, built)
    return final


@check('parts-linked')
def _(c):
    z = c.zip
    ex = sorted(n for n in z.namelist() if n.startswith('Excerpts/') and n.endswith('.mscx'))
    require(len(ex) == 3, 'excerpts in file: %s' % ex)
    main_eids = {e.text for e in c.mscx.iter('eid')}
    report = []
    master_pitches = {}
    for i, st in enumerate(c.mscx.find('Score').findall('Staff')):
        master_pitches[i] = [int(n.findtext('pitch')) for n in st.iter('Note')]
    for name, idxs in EXCERPTS:
        path = [n for n in ex if n.endswith('_%s.mscx' % name.replace(' ', '_'))]
        require(len(path) == 1, 'excerpt %s not found in %s' % (name, ex))
        root = ET.fromstring(z.read(path[0]))
        score = root.find('Score')
        require(score.findtext('name') == name, 'excerpt name %s' % score.findtext('name'))
        links = [l.text for l in root.iter('linkedTo')]
        bad = [l for l in links if l not in main_eids]
        require(not bad, '%s: %d linkedTo targets are not master EIDs' % (name, len(bad)))
        staves = score.findall('Staff')
        require(len(staves) == len(idxs), '%s: %d staves' % (name, len(staves)))
        for st, mi in zip(staves, idxs):
            got = [int(n.findtext('pitch')) for n in st.iter('Note')]
            require(got == master_pitches[mi], '%s: pitches differ from master staff %d' % (name, mi))
        notes = sum(1 for _ in root.iter('Note'))
        linked_notes = sum(1 for n in root.iter('Note') if n.find('linkedTo') is not None)
        require(notes == linked_notes, '%s: %d of %d notes linked' % (name, linked_notes, notes))
        report.append('%s: %d links, %d/%d notes linked' % (name, len(links), linked_notes, notes))
    clar = [n for n in ex if n.endswith('Clarinet_in_Bb.mscx')][0]
    croot = ET.fromstring(z.read(clar))
    require(croot.find('Score/Part/Instrument').findtext('transposeChromatic') == '-2', 'clarinet transposition lost in part')
    return '; '.join(report)
