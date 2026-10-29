# The agent core (core-v1)

How to write a command against the frozen transaction core (tag `core-v1`, P2.1).
Implementers read this, `CLAUDE.md` and their group's spec sheet. The core is
changed only by the conductor (next version: core-v2 at P3.1).

## Files

| What | Where |
|---|---|
| Transaction (one request = one undo entry) | `src/agent/core/transaction.h` |
| User selection and input-state save/restore | `src/agent/core/selectionguard.h` |
| Handles (naming elements between requests) | `src/agent/core/handles.h` |
| Post-condition helpers | `src/agent/core/postconditions.h` |
| Error codes | `src/agent/registry/errors.h` |
| Command description (`CommandInfo`, `WriteContext`) | `src/agent/registry/commandtypes.h` |
| Running writes and batches, the busy checks | `src/agent/writes/writerunner.h` |
| batch, undo, redo | `src/agent/commands/session/sessioncommands.cpp` |
| Example write commands (test only) | `src/agent/commands/test/testcommands.cpp` |

## Writing a write command

```cpp
CommandInfo c;
c.uri = "command://notation/add_dynamic";
c.name = "add_dynamic";                 // last URI segment, unique
c.title = "Add a dynamic";
c.description = "...";                  // what Claude reads: say what it does and refuses
c.kind = CommandKind::Write;
c.undoName = "Add dynamic";             // the undo entry is "Agent: Add dynamic"
c.input["bar"] = { DataType::Integer, "Bar, 1-based", true };
c.output["handle"] = { DataType::String, "Handle of the new dynamic" };
c.write = [](const Params& p, WriteContext& ctx) -> muse::Ret {
    // 1. validate everything first and return E_* errors (nothing has changed yet)
    // 2. edit ctx.score (the MasterScore) with engraving-level calls
    // 3. check the effect; return core::expect(...) / core::barsAddUp(...) on failure
    // 4. ctx.out["handle"] = muse::Val(core::makeHandle(element));
    return muse::make_ok();
};
registry.add(c);
```

Register it in your group's own `register.cpp`. Only the conductor adds the group to
`commands/allcommands.cpp`, at merge.

**What the runner does for you.** Before the handler runs:
- refuse when the user is busy (E_BUSY: drag, text edit, element edit, note input, or
  another edit holding the undo stack) or a dialog is open (E_MODAL_OPEN);
- stop playback;
- check `expectStateVersion` (E_STALE), which every write command accepts automatically;
- open the transaction (`prepareChanges`, `lock`) and save and clear the user's selection.

Afterwards it checks MuseScore's error flag and whether the undo macro was unwound. It
commits, or on any error rolls everything back, restores the selection and notifies the views.
It adds `stateVersion`, `changed` and `warnings` to the output (reserved names: a command may not
define them, nor `results` or `expectStateVersion`). In a `batch`, your handler is
one step of a larger transaction, so it must not assume it is alone.

**Never, in a handler** (CLAUDE.md rule 4):
- open, lock, unlock, commit or roll back anything. This includes `startCmd`/`endCmd`,
  `prepareChanges`, `apply()`, and the `MasterNotationParts` or `NotationStyle` setters;
- call `Score::update()`, `selectAll()`, or anything that shows a dialog: `checkAndShowError`,
  `MessageBox` paths such as `putNote` over a measure repeat or `addTuplet` with an invalid
  ratio. Validate first so that those branches are never reached;
- enter note input or text editing, or simulate input;
- depend on layout done by an earlier step. There is no layout until commit;
- keep a pointer past the request (rule 2). Return handles instead.

**Selection.** It is empty when your handler starts. Select what an engraving path needs
(REPLACE, never ADD). Leave it as it is afterwards: the runner restores the user's selection.

**Errors.** Return `makeErr(Err::X, "message for Claude")`. Say what was wrong and what to do
instead. Codes: E_INVALID_ARG, E_RANGE, E_STALE_HANDLE, E_EXISTS, E_POSTCONDITION,
E_REFUSED (MuseScore refused, or undid the change itself), E_BUSY, E_MODAL_OPEN, E_STALE,
E_NOTHING_TO_UNDO, E_NOTHING_TO_REDO, E_INTERNAL. New codes are
added to `errors.h` by the conductor at merge.

## Command kinds

`Read` never changes anything. `Write` runs inside the runner's transaction (your handler is
`write`). `Control` changes things but manages the undo stack itself (`batch`, `undo`, `redo`):
only the conductor writes those.

## Handles

`core::makeHandle(item)` gives `Note:<eid>@m12+1/4.s1.v1#60`. `core::resolveHandle(score, h)`
walks the live score at that location and returns the element only if its type, EID and
fingerprint all match. Otherwise it returns E_STALE_HANDLE, E_RANGE or E_INVALID_ARG. It never
asks the EID register for an object. Always resolve a handle inside the request that uses it.

## Tests

- **Unit tests:** `src/agent/tests/`, run by `scripts/test.sh`. The core's own tests are in
  `core_tests.cpp` and show the pattern: `Transaction` with a `ScoreUndoBackend` on a fixture.
- **Integration:** `tests/agent/`. `test_core.py` shows the over-MCP checks: an undo restores
  the export byte for byte, and a failure at step N changes nothing.
- **Scenarios:** `tests/agent/scenarios/` (P2.2 driver).
