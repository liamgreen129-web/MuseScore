#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent: P2.1 transaction core over MCP, with the test-only write commands
# (MUSESCORE_AGENT_TEST_COMMANDS=1): one request = one undo entry, batch all-or-nothing,
# failures roll back to a byte-identical score, undo/redo with expectations, stateVersion.
#
# Usage: tests/agent/test_core.py [path/to/Bundle.app]
import pathlib
import shutil
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_server import AGENT_DIR, DEFAULT_BUNDLE, App, check, failures, rpc, wait_for_port  # noqa: E402
from test_read import call  # noqa: E402
from test_export import compare  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/synthetic/pickup-bar.mscz"
TEST_ENV = {"MUSESCORE_AGENT_TEST_COMMANDS": "1"}


def open_with(bundle, path, token, env):
    app = App(bundle, env=env, args=[str(path)])
    _, port = wait_for_port(app.pid)
    deadline = time.time() + 60
    while port and time.time() < deadline:
        err, out, _ = call(port, token, "hello")
        if not err and out.get("score"):
            return app, port
        time.sleep(1)
    return app, port


def mscz(port, token):
    err, ex, text = call(port, token, "score_export", {"format": "mscz"})
    assert not err, text
    return pathlib.Path(ex["path"]).read_bytes()


def same(a, b):
    return compare("mscz", a, b) == ""


def main():
    bundle = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BUNDLE
    token = (AGENT_DIR / "token").read_text().strip()
    work = pathlib.Path(tempfile.mkdtemp(prefix="agent-core-"))
    src = work / FIXTURE.name
    shutil.copy2(FIXTURE, src)

    # --- without the variable, no test commands
    a = App(bundle)
    try:
        _, port = wait_for_port(a.pid)
        _, _, res = rpc(port, token, "tools/list")
        names = {t["name"] for t in res["result"]["tools"]}
        check("batch, undo, redo are listed", {"batch", "undo", "redo"} <= names, names)
        check("test commands are absent in a normal launch", not any(n.startswith("test_") for n in names))
    finally:
        a.stop()

    app, port = open_with(bundle, src, token, TEST_ENV)
    try:
        _, _, res = rpc(port, token, "tools/list")
        tools = {t["name"]: t for t in res["result"]["tools"]}
        check("test commands listed with MUSESCORE_AGENT_TEST_COMMANDS=1", {"test_set_stretch", "test_fail"} <= set(tools))
        check("write commands take expectStateVersion",
              "expectStateVersion" in tools["test_set_stretch"]["inputSchema"]["properties"])

        original = mscz(port, token)
        _, ov, _ = call(port, token, "score_overview")
        v0 = ov["score"]["stateVersion"]

        # --- one write = one undo entry, undo restores the export byte for byte, redo restores the edit
        err, out, text = call(port, token, "test_set_stretch", {"bar": 2, "value": 2.0})
        check("write command runs", not err and out.get("changed") is True and out.get("stretch") == 2.0, text[:200])
        check("stateVersion moves by one", out.get("stateVersion") == v0 + 1, out)
        edited = mscz(port, token)
        check("the edit shows in the export", not same(original, edited))
        err, out, text = call(port, token, "undo", {"expectActionName": "Agent: Test stretch"})
        check("undo with the expected name", not err and out.get("undone") == ["Agent: Test stretch"], text[:200])
        check("undo restores the export byte for byte", same(original, mscz(port, token)))
        err, out, text = call(port, token, "redo", {"expectActionName": "Agent: Test stretch"})
        check("redo", not err and out.get("redone") == ["Agent: Test stretch"], text[:200])
        check("redo restores the edit", same(edited, mscz(port, token)))
        call(port, token, "undo")

        # --- batch: one entry, one undo
        ops = [{"command": "test_set_stretch", "args": {"bar": b, "value": 1.5 + b / 10}} for b in (1, 2, 3)]
        err, out, text = call(port, token, "batch", {"name": "Three stretches", "operations": ops})
        check("batch of 3 runs", not err and len(out.get("results", [])) == 3 and out.get("changed"), text[:200])
        check("each operation's results are returned", [r.get("bar") for r in out.get("results", [])] == [1, 2, 3], out)
        err, out, text = call(port, token, "undo", {"expectActionName": "Agent: Three stretches"})
        check("one undo reverts the whole batch", not err and same(original, mscz(port, token)), text[:200])

        # --- failure at step 3 leaves the score unchanged
        before = call(port, token, "score_overview")[1]["score"]["stateVersion"]
        bad = ops[:2] + [{"command": "test_fail", "args": {"how": "error"}}] + ops[2:]
        err, _, text = call(port, token, "batch", {"operations": bad})
        check("failing batch names the step", err and text == "step 3 (test_fail): E_RANGE: deliberate failure", text)
        check("failing batch leaves the score byte-identical", same(original, mscz(port, token)))
        after = call(port, token, "score_overview")[1]["score"]["stateVersion"]
        check("failing batch leaves stateVersion alone", after == before, (before, after))
        err, out, _ = call(port, token, "undo", {"expectActionName": "Agent: Three stretches"})
        check("failing batch left no undo entry (the next undo is not ours)", err, out)

        # --- the other ways a step can fail
        for how, code in (("refuse", "E_REFUSED"), ("throw", "E_INTERNAL")):
            err, _, text = call(port, token, "test_fail", {"how": how})
            check(f"{how}: {code}, rolled back", err and text.startswith(f"step 1 (test_fail): {code}"), text)
            check(f"{how}: score unchanged", same(original, mscz(port, token)))
        # an unwind is caught in any step (all-or-nothing: later steps must not run on top of it)
        err, _, text = call(port, token, "batch", {"operations": [{"command": "test_fail", "args": {"how": "unwind"}}] + ops[1:]})
        check("unwind in step 1: E_REFUSED, batch rolled back", err and text.startswith("step 1 (test_fail): E_REFUSED"), text)
        check("unwind in step 1: score unchanged", same(original, mscz(port, token)))
        err, _, text = call(port, token, "batch", {"operations": ops[:1] + [{"command": "test_fail", "args": {"how": "unwind"}}]})
        check("unwind in step 2: E_REFUSED, rolled back", err and text.startswith("step 2 (test_fail): E_REFUSED"), text)
        check("unwind: score unchanged", same(original, mscz(port, token)))
        err, out, text = call(port, token, "test_fail", {"how": "warn"})
        check("a MuseScore warning is reported and the edit kept", not err and out.get("warnings") and out.get("changed"), text[:200])
        call(port, token, "undo", {"expectActionName": "Agent: Test fail"})
        check("and undone", same(original, mscz(port, token)))

        # --- validation happens before anything changes
        for args, want in (
            ({"operations": [{"command": "nope"}]}, "E_UNKNOWN_COMMAND"),
            ({"operations": [{"command": "score_read"}]}, "E_INVALID_ARG"),
            ({"operations": ops[:1] + [{"command": "test_set_stretch", "args": {"bar": 1}}]}, "E_INVALID_ARG"),
            ({"operations": [{"command": "test_set_stretch", "args": {"bar": 1, "value": 2, "expectStateVersion": 0}}]}, "E_INVALID_ARG"),
            ({"operations": []}, "E_INVALID_ARG"),
        ):
            err, _, text = call(port, token, "batch", args)
            check(f"batch refused up front: {want}", err and want in text, text[:200])
        check("refused batches changed nothing", same(original, mscz(port, token)))

        # --- stateVersion and undo expectations
        now = call(port, token, "score_overview")[1]["score"]["stateVersion"]
        err, _, text = call(port, token, "test_set_stretch", {"bar": 1, "value": 2.0, "expectStateVersion": now - 1})
        check("stale expectStateVersion -> E_STALE", err and text.startswith("E_STALE"), text)
        err, out, text = call(port, token, "test_set_stretch", {"bar": 1, "value": 2.0, "expectStateVersion": now})
        check("current expectStateVersion is accepted", not err, text)
        err, _, text = call(port, token, "undo", {"expectActionName": "Agent: something else"})
        check("undo with the wrong name -> E_STALE", err and text.startswith("E_STALE"), text)
        call(port, token, "undo")

        # --- several steps: every name is checked before anything is undone
        call(port, token, "test_set_stretch", {"bar": 1, "value": 2.0})
        call(port, token, "test_set_stretch", {"bar": 2, "value": 3.0})
        two = mscz(port, token)
        err, _, text = call(port, token, "undo", {"steps": 2, "expectActionName": "Agent: Test stretch"})
        check("steps 2 with a single expectActionName -> E_INVALID_ARG", err and text.startswith("E_INVALID_ARG"), text)
        err, _, text = call(port, token, "undo", {"steps": 2, "expectActionNames": ["Agent: Test stretch", "Agent: other"]})
        check("second name wrong -> E_STALE", err and text.startswith("E_STALE") and "entry 2" in text, text)
        check("... and nothing was undone", same(two, mscz(port, token)))
        err, out, text = call(port, token, "undo", {"steps": 2, "expectActionNames": ["Agent: Test stretch", "Agent: Test stretch"]})
        check("undo steps 2 with both names", not err and len(out.get("undone", [])) == 2, text[:200])
        err, out, text = call(port, token, "redo", {"steps": 2})
        check("redo steps 2", not err and same(two, mscz(port, token)), text[:200])
        err, _, text = call(port, token, "redo")
        check("redo with nothing left -> E_NOTHING_TO_REDO", err and text.startswith("E_NOTHING_TO_REDO"), text)
        err, _, text = call(port, token, "undo", {"steps": 50})
        check("more steps than entries -> refused, nothing undone", err and same(two, mscz(port, token)), text)
        call(port, token, "undo", {"steps": 2})
        while not call(port, token, "undo")[0]:  # undo everything still on the stack
            pass
        err, _, text = call(port, token, "undo")
        check("undo with nothing left -> E_NOTHING_TO_UNDO", err and text.startswith("E_NOTHING_TO_UNDO"), text)
        check("back to the original", same(original, mscz(port, token)))
        # --- the guards: note input is refused, playback is stopped first
        err, out, text = call(port, token, "test_dispatch", {"action": "note-input"})
        if not err and out.get("noteInput"):
            err, _, text = call(port, token, "test_set_stretch", {"bar": 1, "value": 2.0})
            check("write in note input -> E_BUSY", err and text.startswith("E_BUSY"), text)
            err, _, text = call(port, token, "undo")
            check("undo in note input -> E_BUSY", err and text.startswith("E_BUSY"), text)
            call(port, token, "test_dispatch", {"action": "escape"})
        else:
            check("could enter note input for the guard test", False, (err, out, text[:200]))
        err, out, text = call(port, token, "test_dispatch", {"action": "play"})
        time.sleep(1)
        if not err and call(port, token, "test_dispatch", {"action": "escape"})[1].get("playing"):
            err, out, text = call(port, token, "test_set_stretch", {"bar": 1, "value": 2.0})
            check("write during playback stops it and runs", not err, text[:200])
            time.sleep(1)
            check("playback stopped", not call(port, token, "test_dispatch", {"action": "escape"})[1].get("playing"))
            call(port, token, "undo")
        else:
            print("  note  playback did not start in the background instance; playback guard not checked")
        check("original after the guard checks", same(original, mscz(port, token)))

        err, va, _ = call(port, token, "score_validate")
        check("score_validate ok at the end", not err and va.get("ok"), va.get("problems"))
        check("instance still alive", app.alive())
    finally:
        app.stop()
        shutil.rmtree(work, ignore_errors=True)

    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
