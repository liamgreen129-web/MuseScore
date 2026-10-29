"""Text for the README sections that are not per-file lines."""
import harness

harness.NOT_PRODUCED.extend([
    ('Grace notes after a note in the middle of a bar',
     'MusicXML has no way to say "after"; MuseScore\'s importer gives a grace note written after a note, '
     'with another note following in the bar, to the next chord (tested: it came out as a `grace16` before '
     'the next note). grace-notes.mscz therefore has after-graces only at the end of a bar, which the importer '
     'does turn into `grace16after`. A mid-bar "after" grace would need .mscx editing.'),
    ('Part-specific differences in linked parts (unlinked elements, per-part layout/style/visibility, '
     '"exclude from other parts", independent staff text)',
     'parts-linked.mscz holds only fully linked content. The excerpt writer copies master staves, so anything '
     'that differs per part would be invented rather than produced by the app, and could not be verified as '
     'a state MuseScore itself reaches. Needs a part made and edited in the GUI (or a later fixture produced '
     'through the agent\'s own part commands).'),
    ('Golden export files (tests/golden/)',
     'outside the paths this task may change (tests/fixtures/synthetic/ and tools/fixtures/); the round-trip '
     'MusicXML the generator exports for each check is thrown away with the work directory.'),
])

harness.EXTRA_NOTES.extend([
    'MuseScore\'s MusicXML importer cannot build nested tuplets: it takes the combined `<time-modification>` '
    '(e.g. 9:4 for a triplet inside a triplet) as one flat tuplet and pads the bar with rests; the app\'s '
    'own nested export does not re-import either. Nested tuplets in the corpus (`tuplets-nested`, three bars of '
    '`stress-dense-rhythms`, bar 13 of `stress-combined`) are therefore written into the .mscx '
    '(`mscxedit.py`) and re-saved by the app. In the .mscx a nested tuplet carries its ratio relative to its '
    'parent (the inner triplet is 3:2, not 9:4) and nesting is `<Tuplet>` ... `<endTuplet/>` pairs.',
    'MusicXML import turns `<segno/>`, `<coda/>`, `<sound dalsegno/tocoda/fine/dacapo>` into plain staff text, '
    'not Marker/Jump elements, so `jumps-markers` (and the markers in `stress-combined`) get real Marker/Jump '
    'measure elements by .mscx editing. In MuseScore the "To Coda" marker has `markerType` `coda` and the coda '
    'sign has `markerType` `codab`.',
    'Linked parts are written as separate score files under `Excerpts/` inside the .mscz; an excerpt element '
    'is tied to the master by `<linkedTo>` holding the master element\'s `<eid>`. `excerpts.py` builds this '
    'from the master and the app has to accept and re-save it, which is the check.',
    'Melismas are stored on the first syllable as `<ticks_f>` (a duration), not as a chain of notes; the '
    'MusicXML export writes a bare `<extend/>` on that one note only.',
    'Spanner end ticks are exclusive: a hairpin, 8va or pedal that ends at the first beat of bar N+1 is '
    'exported as ending in bar N. The spanner fixtures end their lines mid-bar so the exported bar equals '
    'the written bar.',
])
