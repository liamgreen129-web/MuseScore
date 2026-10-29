# MuseScore Agent compact notation

**Status: DRAFT for Liam's review (P1.3, STOP).** Nothing in P1.4 is built until this is
approved. Every example below was printed by `tools/notation_ref/m21_print.py` from a real
score, not typed by hand (sources at the end).

This is the one text format the agent uses to **read** music (`score_read`) and to
**write** it (`write_voice`). It has to be lossless for what it covers, short enough that a
whole movement fits in a few thousand tokens, and hard to get wrong. Where a choice was
needed the rule is: explicit beats terse, words beat symbols, and one way to say each thing.

## 1. A first look

Two bars of a string quartet, Violin I (from §9.3):

```
# s1 "Violin I" clef=treble key=F time=3/4
m5 v1 | F5:q~+< (F5:e G5:s F5:s E5:e+st+!) F5:e+st
m6 v1 | (G5:h Bb4:q)
```

Bar 5, voice 1: an F5 quarter tied into an eighth, with a crescendo hairpin starting on it; a
slur over the turn figure, whose last note is staccato and ends the hairpin; a staccato F5.
Bar 6: a slur over a half-note G5 and a quarter B-flat 4. Full worked examples are in §9.

Every line is one of:

| Line | Example | Meaning |
|---|---|---|
| Staff header | `# s3 "Trumpet in Bb" clef=treble transposes=-M2 key=F time=4/4` | Starts a staff block. Informational on read; checked, not applied, on write. |
| Measure attributes | `m17 @ time=3/4 key=Eb mark=B repeat=start` | Things that live on the bar, not on a note. |
| Voice line | `m17 v1 \| C5:q D5:e E5:e F5:h` | The events of one voice in one bar. |
| Harmony line | `m17 h \| {Cm7}:h {F7}:h` | Chord symbols for the staff, positioned by duration like events. |
| Comment | `% anything` | Ignored. |

Bars are numbered `m1, m2, …` by position in the score (the first bar, even a pickup, is
`m1`). When MuseScore's displayed bar number differs, reads add it: `m2(1)` is the second
bar, displayed as bar 1.

## 2. Pitch

`<step><accidental><octave>`: step `A`–`G`; accidental none, `#`, `##`, `b`, `bb`; octave in
scientific pitch (`C4` = middle C, `B3` is the B just below it).

- **Pitch is what sounds.** For a Bb trumpet, a written D5 is `C5`; under an 8va line the
  written C5 is `C6`. The staff header says how the instrument transposes
  (`transposes=-M2` means sounds a major second lower than written). This makes harmony
  across parts directly comparable. `score_read(pitch: "written")` and
  `write_voice(pitch: "written")` switch a call to written pitch; the header then says
  `pitch=written`.
- **Spelling is exact and kept**: `G#4` and `Ab4` are different notes. Reads print the
  spelling MuseScore holds; writes store the spelling given.
- An accidental is printed only as part of the pitch's spelling, never as "what the engraver
  shows". Courtesy and cautionary accidentals are a display property, not notation here.

## 3. Duration

`:` then a base value and optional dots:

| Code | Value | | Code | Value |
|---|---|---|---|---|
| `b` | breve | | `s` | 16th |
| `w` | whole | | `t` | 32nd |
| `h` | half | | `x` | 64th |
| `q` | quarter | | `y` | 128th |
| `e` | eighth | | | |

Dots: `q.` dotted quarter, `h..` double-dotted half. Every event states its duration; there
is no "same as before".

## 4. Events

| Event | Form | Example |
|---|---|---|
| Note | `<pitch>:<dur>` | `F#4:e.` |
| Chord | `[<pitch>,<pitch>,…]:<dur>` | `[C4,E4,G4]:h` |
| Rest | `r:<dur>` | `r:q` |
| Full-bar rest | `R` | `R` (whatever the bar's length) |
| Gap | `_:<dur>` | `_:h` (voices 2–4 only: nothing written there) |

**Ties.** `~` after a pitch ties it to the same pitch in the next event of the voice, across
barlines too: `C5:h~` … next bar `C5:q`. In a chord, tie individual notes `[C4~,E4,G4]:h`,
or all of them `[C4,E4,G4]:h~`.

**Tuplets.** `{<actual>:<normal> <events>}`: "actual notes in the time of normal". Durations
inside are the written values. `{3:2 C5:e D5:e E5:e}` is an eighth-note triplet taking a
quarter. Tuplets nest: `{3:2 C5:q {3:2 D5:e E5:e F5:e} G5:q}`.

**Grace notes** come before the note they belong to: `g(D5:e) C5:q` (appoggiatura),
`g/(D5:e) C5:q` (acciaccatura, slashed), several at once `g(B4:s C5:s) D5:q`. Grace notes
after a note: `C5:h ga(B4:s C5:s)`.

**Slurs.** `(` before the first event, `)` after the last. They may span bars and nest
(a phrase slur around shorter slurs). `(C5:e D5:e) (E5:e F5:e)`.

## 5. Marks on events

Marks follow an event, each introduced by `+`, no spaces: `C5:q+st+mf`.

| Kind | Codes |
|---|---|
| Articulation | `st` staccato, `sti` staccatissimo, `ten` tenuto, `tst` tenuto-staccato (portato), `ac` accent, `mar` marcato, `up` up-bow, `dn` down-bow, `harm` harmonic, `open`, `stop` |
| Ornament | `tr` trill (symbol, not line), `mord`, `imord` (inverted), `turn`, `iturn` |
| Fermata | `ferm`, `ferm:short`, `ferm:long`, `ferm:vlong` |
| Breath | `br` breath mark after the event, `cae` caesura |
| Dynamic | `pppp`…`ffff`, `fp`, `sf`, `sfz`, `sffz`, `sfp`, `rfz`, `fz`; other text `dyn"subito p"` |
| Hairpin | `<` starts a crescendo hairpin here, `>` a diminuendo, `!` ends the open hairpin here; text forms `<"cresc."`, `>"dim."` |
| Lines | `8va[` … `8va]`, also `8vb`, `15ma`, `15mb`, `ped`, `trl` (trill line), `lr` (let ring), `txt"…"` (text line) |
| Text | `"dolce"` staff text, `sys"…"` system text, `exp"…"` expression |
| Tempo | `tempo"Allegro"(q=132)`, `tempo(q.=60)`, `tempo"rit."` |
| Lyric | `ly"Glo-"` verse 1, `ly2"…"` verse 2; `-` at the end joins to the next syllable, `_` at the end draws an extender over a melisma |
| Cross-staff | `xs:up`, `xs:down` |
| Anything else | `sym:<SymId>` (any engraving symbol MuseScore names), so reads never drop a mark |

A mark sits at the time of its event. Spanners (hairpins, lines) start at the event that
carries the start code and end at the event that carries the end code, in the same voice.

## 6. Bar attributes

`m<N> @ key=value …` on its own line, before that bar's voice lines.

| Attribute | Example |
|---|---|
| `time` | `time=6/8`, `time=C` (common), `time=C/` (cut) |
| `key` | `key=Eb`, `key=c#` (lower case = minor), `key=none` (atonal/open) |
| `clef` | `clef=bass` (this staff; mid-bar clefs go on the event as `+clef:bass`) |
| `tempo` | `tempo="Allegro"(q=132)` (when not tied to an event) |
| `mark` | `mark=A` rehearsal mark |
| `repeat` | `repeat=start`, `repeat=end`, `repeat=end:3` (play 3 times) |
| `volta` | `volta=1`, `volta=1,2` (ending starts here), `volta=end` |
| `barline` | `barline=double`, `final`, `dashed`, `dotted`, `none` (the barline at the end of this bar) |
| `jump` | `jump="D.S. al Coda"`, `marker=segno`, `marker=coda`, `marker=fine` |
| `break` | `break=line`, `break=page`, `break=section` |
| `len` | `len=1/4` (actual length when it differs from the time signature: pickups, irregular bars) |

## 7. Rules that make writes safe

1. **Every voice line adds up to the bar.** Voice 1 with notes and rests; voices 2–4 fill
   unused time with `_`. `write_voice` rejects a bar that does not add up and says which bar
   and by how much (`E_BAR_SUM: m12 v1 is 3/8 short`).
2. **A write replaces exactly the voice and bars it names**, nothing else. Marks belonging to
   other voices, harmony lines and bar attributes are untouched unless they are written too.
3. **Ties must land**: `~` must be followed by the same pitch in the next event of the voice.
4. **Tuplets must close inside the bar**, and their contents must fill them.
5. **Reads print everything they cover in canonical form**: one event per token, marks in the
   order of §5's table, chords low to high. So read → write → read gives the same text
   (the G1 gate tests exactly this).

## 8. What the notation does not carry

Positions and offsets, colours, visibility, stem directions, beaming, note-head types,
small/cue size, play/velocity overrides, and most style. Those are properties: `score_read`
with `detail: true` returns the same range as JSON with handles and every property, and
`set_property` changes them. Reads add `% detail: …` comments when something in the range
can only be seen through detail (for example `% detail: m4 v1 has hidden notes`).

## 9. Worked examples

Printed by `tools/notation_ref/make_examples.py`. Reading notes follow each example.

### 9.1 Four-part chorale (pickup bar, fermatas, minor key)

J. S. Bach, four-part chorale BWV 66.6, bars 1–4 (music21 corpus `bach/bwv66.6`).

```
# s1 "Soprano" clef=treble key=f# time=C
m1(0) @ len=1/4
m1(0) v1 | C#5:e B4:e
m2(1) v1 | A4:q B4:q C#5:q+ferm E5:q
m3(2) v1 | C#5:q B4:q A4:q+ferm C#5:q
m4(3) v1 | A4:e B4:e G#4:q F#4:q+ferm A4:q
# s2 "Alto" clef=treble key=f# time=C
m1(0) @ len=1/4
m1(0) v1 | E4:q
m2(1) v1 | F#4:q E4:q E4:q E4:q
m3(2) v1 | E4:e A4:e G#4:q E4:q G#4:q
m4(3) v1 | F#4:e G#4:e E#4:q C#4:q F#4:q
# s3 "Tenor" clef=bass key=f# time=C
m1(0) @ len=1/4
m1(0) v1 | A3:e B3:e
m2(1) v1 | C#4:q B3:q A3:q B3:q
m3(2) v1 | A3:e E4:e E4:e D4:e C#4:q C#4:q
m4(3) v1 | C#4:e D4:e C#4:e B3:e A3:q C#4:q
# s4 "Bass" clef=bass key=f# time=C
m1(0) @ len=1/4
m1(0) v1 | A3:e G#3:e
m2(1) v1 | F#3:q G#3:q A3:q G#3:q
m3(2) v1 | A3:e C#3:e E3:q A2:q E#3:q
m4(3) v1 | F#3:e B2:e C#3:q F#2:q F#3:q
```

### 9.2 Piano, two staves (slurs, trill, Alberti bass)

W. A. Mozart, Piano Sonata in C, K. 545, i, bars 1–4 (music21 corpus `mozart/k545/movement1_exposition`).

```
# s1 "Piano (right hand)" clef=treble key=C time=C
m1 v1 | C5:h E5:q G5:q
m2 v1 | (B4:q. C5:s D5:s) C5:q r:q
m3 v1 | A5:h G5:q C6:q
m4 v1 | G5:q (F5:e+tr E5:s F5:s) E5:q r:q
# s2 "Piano (left hand)" clef=treble key=C time=C
m1 v1 | C4:e G4:e E4:e G4:e C4:e G4:e E4:e G4:e
m2 v1 | D4:e G4:e F4:e G4:e C4:e G4:e E4:e G4:e
m3 v1 | C4:e A4:e F4:e A4:e C4:e G4:e E4:e G4:e
m4 v1 | B3:e G4:e D4:e G4:e C4:e G4:e E4:e G4:e
```

### 9.3 String quartet (dynamics, hairpin, slurs across the bar, staccato, ties)

L. van Beethoven, String Quartet Op. 18 No. 1, i, bars 3–6, Violin I and Cello (music21 corpus `beethoven/opus18no1/movement1.mxl`).

```
# s1 "Violin I" clef=treble key=F time=3/4
m3 v1 | F4:q~ (F4:e G4:s F4:s) E4:e+st F4:e+st
m4 v1 | D4:q r:q r:q
m5 v1 | F5:q~+< (F5:e G5:s F5:s E5:e+st+!) F5:e+st
m6 v1 | (G5:h Bb4:q)
# s4 "Cello" clef=bass key=F time=3/4
m3 v1 | (F3:q~ F3:e G3:s F3:s) E3:e+st F3:e+st
m4 v1 | D3:q r:q r:q
m5 v1 | (D3:h.
m6 v1 | E3:h.)
```

### 9.4 Folk tune (repeats, grace notes)

*Highland Laddie*, from Aird's *Selection of Scotch, English, Irish and Foreign Airs* (c. 1782), first strain (music21 corpus `airdsAirs/book1.abc`).

```
# s1 "Fiddle" clef=treble key=G time=2/4
m1(0) @ tempo="allegro"(q=132) len=1/8
m1(0) v1 | E5:e
m2(1) v1 | D5:e G4:e B4:e G4:e
m3(2) v1 | g/(B4:e C5:e) D5:q B4:q
m4(3) v1 | E5:e A4:e A4:e B4:e
m5(4) v1 | E5:e A4:e A4:e E5:e
m6(5) v1 | D5:e B4:e G5:e B4:e
m7(6) v1 | D5:q B4:q
m8(7) v1 | (G4:s A4:s B4:s C5:s) D5:e A4:e
m9(8) @ repeat=end len=3/8
m9(8) v1 | B4:e G4:e G4:e
```

### 9.5 Everything else (lyrics, Bb trumpet, two voices, triplet, chord symbols)

*Lantern Song*, 2 bars written for this document.

```
# s1 "Voice" clef=treble key=G time=4/4
m1 v1 | D5:q+ly"Light" B4:e+ly"the" C5:e+ly"lan-" D5:q+ly"tern" G5:q~+ly"high"
m2 v1 | G5:q (F#5:e+ly"glow" E5:e) D5:h
# s2 "Trumpet in Bb" clef=treble transposes=-M2 key=G time=4/4
m1 v1 | r:h {3:2 D5:e+mf E5:e F#5:e} G5:q+ac
m2 v1 | g/(A5:e) G5:h r:h
# s3 "Piano" clef=treble key=G time=4/4
m1 v1 | B4:h C5:h
m1 v2 | G4:q F#4:q [E4,G4]:h
m1 h | {G}:h {Cmaj7}:h
m2 v1 | [B4,D5]:h A4:h
m2 v2 | G4:h _:h
m2 h | {G}:h {D7}:h
```

## 10. Grammar

```ebnf
document     = { line , newline } ;
line         = staff-header | bar-attrs | voice-line | harmony-line | comment | blank ;
staff-header = "#" , ws , "s" , int , ws , quoted , { ws , key-value } ;
bar-attrs    = bar-ref , ws , "@" , { ws , key-value } ;
voice-line   = bar-ref , ws , "v" , ( "1" | "2" | "3" | "4" ) , ws , "|" , { ws , item } ;
harmony-line = bar-ref , ws , "h" , ws , "|" , { ws , harmony } ;
comment      = "%" , { any-char } ;
bar-ref      = "m" , int , [ "(" , display-number , ")" ] ;

item         = [ "(" ] , ( event | tuplet | grace ) , [ ")" ] ;
tuplet       = "{" , int , ":" , int , ws , item , { ws , item } , "}" ;
grace        = ( "g(" | "g/(" | "ga(" ) , event , { ws , event } , ")" ;
event        = ( note | chord | rest | gap | "R" ) , { mark } ;
note         = pitch , ":" , duration , [ "~" ] ;
chord        = "[" , pitch , [ "~" ] , { "," , pitch , [ "~" ] } , "]" , ":" , duration , [ "~" ] ;
rest         = "r:" , duration ;
gap          = "_:" , duration ;
pitch        = step , [ accidental ] , octave ;
step         = "A" | "B" | "C" | "D" | "E" | "F" | "G" ;
accidental   = "##" | "#" | "bb" | "b" ;
octave       = digit | "-1" ;
duration     = ( "b" | "w" | "h" | "q" | "e" | "s" | "t" | "x" | "y" ) , { "." } ;
mark         = "+" , mark-body ;              (* the codes of section 5 *)
harmony      = "{" , chord-text , "}" , ":" , duration | "_:" , duration ;
```

## 11. Open points for Liam

1. **Sounding pitch by default** (§2). The alternative is written pitch, which reads more
   naturally for one transposing part on its own but makes cross-part harmony error-prone.
2. **Marks as `+codes`** (§5) rather than symbols like `.` `>` `-`: longer, but `.` already
   means a dot in durations and the symbols are easy to misread.
3. **Chord symbols on their own `h` line** rather than as marks on notes, so a chord can change
   in the middle of a held note.
4. **`_` gaps in voices 2–4** are printed on read so every line adds up, at the cost of a few
   tokens.
5. Anything you find awkward in the examples.

## Sources of the examples

All corpus scores are public domain and ship with music21 (`music21/corpus`); the
encodings are music21's. *Lantern Song* (§9.5) was written for this document.

Notes on the examples:

- §9.1 shows the pickup: the first bar is `m1`, displayed as bar 0, with `len=1/4`.
- §9.2 is one piano part on two staves; each staff is its own block.
- §9.3: in the source file Violin I's slur and hairpin are well formed, but the cello's bar 1
  slur is mis-paired in the encoding, so the example starts at bar 3. The C++ printer reads
  MuseScore's own model, where spanners always have real ends.
- §9.4: the grace notes print as `g/(…)` because the source marks them as acciaccaturas.
- §9.5: the trumpet is written in A major and prints in G major, because pitch is sounding
  pitch (§2). Its written grace note B5 prints as A5.
