#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent: scenario driver (P2.2).
#
# Runs every scenario file in this folder (or the named ones) against the agent build, one
# background instance at a time, each on a fresh temp copy of its fixture. Scenarios whose
# tools are not in the build's tools/list are SKIP, not FAIL. Exit 1 on any FAIL.
#
# Usage: python3 -I tests/agent/scenarios/run.py [names...] [--update-goldens]
#            [--bundle path/to/mscore.app] [--undo-tool undo] [--redo-tool redo] [--list]
#   names   scenario ids or file-name substrings (S01, R02, dynamic, ...)
import argparse
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from harness.instance import DEFAULT_BUNDLE, InstanceError  # noqa: E402
from harness.scenario import Runner, load  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("names", nargs="*")
    ap.add_argument("--bundle", default=str(DEFAULT_BUNDLE))
    ap.add_argument("--update-goldens", action="store_true", help="write goldens instead of comparing")
    ap.add_argument("--undo-tool", default="undo")
    ap.add_argument("--redo-tool", default="redo")
    ap.add_argument("--list", action="store_true", help="list scenarios and exit")
    a = ap.parse_args()

    files = sorted(HERE.glob("*.json"))
    scenarios = [load(f) for f in files]
    if a.names:
        scenarios = [s for s in scenarios
                     if any(n == s["id"] or n.lower() in s["_name"].lower() for n in a.names)]
    if not scenarios:
        print("error: no scenarios match", file=sys.stderr)
        return 2
    if a.list:
        for s in scenarios:
            print(f"{s['id']:5} {s['fixture']:42} {s.get('title', '')}")
        return 0
    if not pathlib.Path(a.bundle).exists():
        print(f"error: no bundle at {a.bundle}", file=sys.stderr)
        return 2

    runner = Runner(HERE, bundle=a.bundle, undo_tool=a.undo_tool, redo_tool=a.redo_tool,
                    update_goldens=a.update_goldens)
    try:
        tools = runner.probe()
    except InstanceError as e:
        print(f"error: could not start the build to read tools/list: {e}", file=sys.stderr)
        return 2
    print(f"build offers {len(tools)} tool names; undo tool {a.undo_tool!r} "
          f"{'present' if runner.has(a.undo_tool) else 'absent'}")

    t0 = time.time()
    counts = {"ok": 0, "FAIL": 0, "SKIP": 0}
    failed = []
    for s in scenarios:
        r = runner.run(s)
        status = "FAIL" if r.failed else "SKIP" if r.skipped else "ok"
        counts[status] += 1
        if status == "FAIL":
            failed.append(s["id"])
    print()
    print(f"scenarios: {counts['ok']} ok, {counts['FAIL']} FAIL, {counts['SKIP']} SKIP "
          f"({time.time() - t0:.0f} s)" + (f"  failed: {' '.join(failed)}" if failed else ""))
    print("run: " + ("FAIL" if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
