# Command registry: groups, ownership and status

This is the specification the registry is built from. Every command has a row here,
a section in its group's spec sheet (`specs/<group>.md`, written by `api-scout`),
and at least one scenario. URIs follow MuseScore 5's `command://<area>/<name>` shape.

Status values: `—` not started · `spec` spec sheet done · `impl` implemented ·
`review` in review · `merged` merged and gate-tested.

| Group | Commands | Phase | Owner | Spec | Impl |
|---|---|---|---|---|---|
| `session` | hello, instances_list, scores_list, score_open, undo, redo, save, backup_snapshot | 1–3 | Conductor | — | — |
| `read` | score_overview, score_read, score_export, score_render, score_changes, score_validate | 1 | Conductor | — | — |
| `notes` | write_voice, add_note_to_chord, remove_note, set_duration, tie, tuplet, grace, clear_range, exchange_voices, batch | 2 | Conductor | — | — |
| `pitch` | set_pitch, transpose | 2 | Wave A | — | — |
| `expression-marks` | add_dynamic, add_hairpin, add_articulation, add_ornament, add_fermata, add_breath | 2 | Wave A | — | — |
| `expression-lines` | add_slur, add_line (pedal, ottava, trill, volta, text line) | 2 | Wave A | — | — |
| `text` | add_text (staff, system, expression, tempo, rehearsal), add_chord_symbol, add_lyric | 2 | Wave A | — | — |
| `structure` | insert_measures, remove_measures, split_measure, join_measures, set_time_signature, set_key_signature, set_tempo, set_barline, set_repeat, add_break, add_clef | 3 | Wave B | — | — |
| `instrumentation` | add_part, remove_part, rename_part, set_instrument, add_staff, set_staff_type, set_transposition | 3 | Wave B | — | — |
| `generic` | set_property, remove, select, cmd (allow-listed) | 3 | Wave B | — | — |
| `style-layout` | set_style, set_measure_stretch, lock_systems, set_offset | 3 | Wave B | — | — |

Scouting order for P0.S (highest risk first): `notes`, `expression-lines`,
`structure`, `text`, `pitch`, `expression-marks`, `instrumentation`, `generic`,
`style-layout`, `read`, `session`.

## Spec sheet format (one section per command)

```markdown
### <command>  ·  command://notation/<name>

**UI entry point.** The exact function(s) the UI calls for this edit, with file:line
in v4.7.6, and how the UI reaches it (action code, menu, palette).

**Selection set-up.** What selection the method acts on, and how to construct it
for the addressed range/elements. "None" if the method takes explicit arguments.

**Arguments.** Proposed input schema (JSON Schema fragment), with addressing
({measure, beat|tick}, staff by part name + index, voice 1–4, handles).

**Validation before mutation.** Every check, mapped to an error code.

**Post-condition.** How to verify, after the call, that the edit happened
(the method likely returns void).

**Nested commands.** Does the path call startCmd/endCmd itself (cmdAddTie etc.)?
Is it safe under undoStack lock()?

**Linked parts / excerpts.** Does the edit propagate (undo* helpers) or need doing per part?

**Crash and corruption risks.** Known asserts, null paths, upstream issues/PRs,
plugin-API crashes in the same area.

**Existing upstream tests.** gtest files that exercise this path; fixtures we can reuse.

**Scenarios.** Proposed scenario IDs and one-line descriptions.

**Open questions.** Anything the scout could not resolve from source.
```

Every claim cites file:line. "Probably" is not allowed in a spec sheet; unresolved
points go under Open questions.
