#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent: P1.7 read commands over MCP, against corpus fixtures opened in the app.
#
# Usage: tests/agent/test_read.py [path/to/Bundle.app]   (needs tests/fixtures/corpus/)
import json
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_server import AGENT_DIR, DEFAULT_BUNDLE, App, check, failures, rpc, wait_for_port  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
CORPUS = ROOT / "tests/fixtures/corpus"


def call(port, token, name, args=None):
    _, _, res = rpc(port, token, "tools/call", {"name": name, "arguments": args or {}})
    r = res.get("result", {})
    return r.get("isError"), r.get("structuredContent", {}), (r.get("content") or [{}])[0].get("text", "")


def open_score(bundle, path, token):
    app = App(bundle, args=[str(path)])
    _, port = wait_for_port(app.pid)
    deadline = time.time() + 60
    while port and time.time() < deadline:
        err, out, _ = call(port, token, "hello")
        if not err and out.get("score"):
            return app, port
        time.sleep(1)
    print(f"  note  {path.name} did not open within 60 s (pid {app.pid}, port {port}, last hello {out})")
    return app, port


def main():
    bundle = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BUNDLE
    if not (CORPUS / "bach-bwv66-6.mscz").exists():
        print("error: run tools/fixtures/make_corpus_fixtures.py first", file=sys.stderr)
        return 2
    token = (AGENT_DIR / "token").read_text().strip()

    # --- no score open
    a = App(bundle)
    try:
        _, port = wait_for_port(a.pid)
        _, _, res = rpc(port, token, "tools/list")
        names = {t["name"] for t in res["result"]["tools"]}
        check("tools/list has the read group",
              {"score_overview", "score_read", "score_validate", "score_changes"} <= names, names)
        err, _, text = call(port, token, "score_read")
        check("score_read with no score -> E_NO_SCORE", err and text.startswith("E_NO_SCORE"), text)
    finally:
        a.stop()

    # --- Bach chorale
    b, port = open_score(bundle, CORPUS / "bach-bwv66-6.mscz", token)
    try:
        err, ov, text = call(port, token, "score_overview")
        check("overview succeeds", not err, text)
        check("overview: 4 parts", len(ov.get("parts", [])) == 4, ov.get("parts"))
        check("overview: part names", [p["name"] for p in ov.get("parts", [])] == ["Soprano", "Alto", "Tenor", "Bass"],
              [p.get("name") for p in ov.get("parts", [])])
        check("overview: stateVersion 0 after open", ov.get("score", {}).get("stateVersion") == 0, ov.get("score"))
        check("overview: small (under 4000 chars)", len(text) < 4000, len(text))
        nbars = ov.get("score", {}).get("bars")

        err, rd, text = call(port, token, "score_read", {"from": 1, "to": 4, "staves": [1]})
        t = rd.get("text", "")
        check("score_read 1-4 staff 1 succeeds", not err, text)
        check("score_read: header line", t.startswith('# s1 "Soprano" clef=treble key=f# time=C'), t[:80])
        check("score_read: bar 2 as in NOTATION.md 9.1", "v1 | A4:q B4:q C#5:q+ferm E5:q" in t, t)
        err, rd, text = call(port, token, "score_read")
        check("score_read whole chorale", not err and rd.get("from") == 1 and rd.get("to") == nbars, text[:200])
        err, rd, text = call(port, token, "score_read", {"pitch": "written", "voices": [1]})
        check("score_read pitch=written accepted", not err, text[:200])

        err, _, text = call(port, token, "score_read", {"from": 5, "to": 2})
        check("from > to -> E_RANGE", err and text.startswith("E_RANGE"), text)
        err, _, text = call(port, token, "score_read", {"from": 1, "to": 999})
        check("past the end -> E_RANGE", err and text.startswith("E_RANGE"), text)
        err, _, text = call(port, token, "score_read", {"staves": [9]})
        check("unknown staff -> E_RANGE", err and text.startswith("E_RANGE"), text)
        err, _, text = call(port, token, "score_read", {"pitch": "concert"})
        check("bad enum -> E_INVALID_ARG", err and text.startswith("E_INVALID_ARG"), text)
        err, _, text = call(port, token, "score_read", {"from": 1.5})
        check("non-integer bar -> E_INVALID_ARG", err and text.startswith("E_INVALID_ARG"), text)

        err, va, text = call(port, token, "score_validate")
        check("score_validate: chorale is clean", not err and va.get("ok") is True, va)
        err, ch, text = call(port, token, "score_changes", {"since": 0})
        check("score_changes: nothing yet", not err and ch.get("stateVersion") == 0 and ch.get("bars") == "", ch)

        err, hello, _ = call(port, token, "hello")
        check("reads leave the score clean", hello.get("score", {}).get("needSave") is False, hello.get("score"))
    finally:
        b.stop()

    # --- a long quartet movement: size cap, and timing
    c, port = open_score(bundle, CORPUS / "beethoven-op18-1-i.mscz", token)
    try:
        t0 = time.time()
        err, _, text = call(port, token, "score_read")
        dt = time.time() - t0
        check("whole Beethoven movement refused with a bar count that fits",
              err and text.startswith("E_RANGE") and "bars at a time" in text, text[:200])
        check("refusal is quick (< 3 s incl. printing 4000 notes)", dt < 3, dt)
        t0 = time.time()
        err, rd, text = call(port, token, "score_read", {"from": 1, "to": 40})
        check("40 bars of the quartet read", not err and rd.get("text", "").count("\n") > 100, text[:200])
        check("40-bar read under 1 s", time.time() - t0 < 1, time.time() - t0)
        err, va, _ = call(port, token, "score_validate")
        check("validate reports the import's cross-voice ties as details", not err and len(va.get("details", [])) > 0, va.get("details"))
    finally:
        c.stop()

    print()
    if failures:
        print(f"test_read: FAIL ({len(failures)})")
        return 1
    print("test_read: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
