#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent: P1.2 tests (registry, hello, discovery records).
#
# Usage: tests/agent/test_hello.py [path/to/Bundle.app]   (default: agent.install/mscore.app)
import json
import pathlib
import stat
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_server import (AGENT_DIR, DEFAULT_BUNDLE, App, check, failures, rpc, wait_for_port)  # noqa: E402

INSTANCES = AGENT_DIR / "instances"
MUSIC21 = pathlib.Path.home() / ".venvs/MuseClaude/bin/python"


def call(port, token, name, args=None, rid=1):
    s, _, res = rpc(port, token, "tools/call", {"name": name, "arguments": args or {}}, rid=rid)
    return s, res


def make_score(directory):
    """A two-bar test score, written as MusicXML by music21 and converted by the agent build itself."""
    xml = directory / "p12-test.musicxml"
    subprocess.run([str(MUSIC21), "-c", f"""
from music21 import stream, note, meter, metadata
s = stream.Score(); s.metadata = metadata.Metadata(title='P12 test')
p = stream.Part(); p.append(meter.TimeSignature('4/4'))
for n in 'C4 E4 G4 C5 B4 G4 D4 C4'.split(): p.append(note.Note(n, quarterLength=1))
s.insert(0, p); s.write('musicxml', r'{xml}')
"""], check=True)
    return xml


def main():
    bundle = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BUNDLE
    token = (AGENT_DIR / "token").read_text().strip() if (AGENT_DIR / "token").exists() else None

    # a leftover record from a "crashed" process that no longer exists
    INSTANCES.mkdir(parents=True, exist_ok=True)
    stale = INSTANCES / "999999.json"
    stale.write_text('{"pid": 999999, "port": 2499}')

    a = App(bundle)
    b = None
    tmp = tempfile.TemporaryDirectory()
    try:
        _, port = wait_for_port(a.pid)
        check("server up", port is not None)
        if port is None:
            return 1
        token = token or (AGENT_DIR / "token").read_text().strip()

        # --- registry
        s, _, res = rpc(port, token, "tools/list")
        tools = {t["name"]: t for t in res.get("result", {}).get("tools", [])}
        check("tools/list has hello", "hello" in tools, list(tools))
        if "hello" in tools:
            t = tools["hello"]
            check("hello has a description", len(t.get("description", "")) > 40)
            check("hello output schema names app and score",
                  set(t.get("outputSchema", {}).get("properties", {})) == {"app", "score"}, t.get("outputSchema"))
            check("tool names are API-safe", all(c.isalnum() or c in "_-" for c in t["name"]))

        s, res = call(port, token, "hello", {"bogus": 1})
        r = res.get("result", {})
        check("unknown argument -> isError E_INVALID_ARG",
              r.get("isError") is True and r["content"][0]["text"].startswith("E_INVALID_ARG"), r)
        s, res = call(port, token, "nope")
        r = res.get("result", {})
        check("unknown tool -> E_UNKNOWN_COMMAND",
              r.get("isError") is True and r["content"][0]["text"].startswith("E_UNKNOWN_COMMAND"), r)

        # --- hello
        s, res = call(port, token, "hello")
        r = res.get("result", {})
        hello = r.get("structuredContent", {})
        app = hello.get("app", {})
        check("hello succeeds", s == 200 and r.get("isError") is False, r)
        check("hello: title MuseScore Agent", app.get("title") == "MuseScore Agent", app)
        check("hello: bundle id", app.get("bundleId") == "com.liamgreen.MuseScoreAgent", app)
        check("hello: file format 4.70", app.get("fileFormat") == "4.70", app)
        check("hello: pid is this process", app.get("pid") == a.pid, (app.get("pid"), a.pid))
        check("hello: port is this server", app.get("port") == port, (app.get("port"), port))
        check("hello: instance id present", bool(app.get("instanceId")), app)
        check("hello: version 4.7.6", str(app.get("version", "")).startswith("4.7.6"), app)
        check("hello: score key present (null or object)", "score" in hello, hello)
        check("text content mirrors structuredContent", json.loads(r["content"][0]["text"]) == hello)

        # --- discovery record
        rec_path = INSTANCES / f"{a.pid}.json"
        check("discovery record written", rec_path.exists(), rec_path)
        if rec_path.exists():
            rec = json.loads(rec_path.read_text())
            check("record: port and url", rec.get("port") == port and rec.get("url") == f"http://127.0.0.1:{port}/mcp", rec)
            check("record: pid", rec.get("pid") == a.pid, rec)
            check("record never contains the token", token not in rec_path.read_text())
            check("record is owner read/write only", stat.S_IMODE(rec_path.stat().st_mode) == 0o600)
            check("instances folder is owner only", stat.S_IMODE(INSTANCES.stat().st_mode) == 0o700)
        check("stale record from a dead pid pruned", not stale.exists())

        # --- an instance with a score open
        score = make_score(pathlib.Path(tmp.name))
        mscz = pathlib.Path(tmp.name) / "p12-test.mscz"
        subprocess.run([str(bundle / "Contents/MacOS/mscore"), "-o", str(mscz), str(score)],
                       check=True, capture_output=True)
        before = {p for p in INSTANCES.glob("*.json")}
        b = App(bundle, args=[str(mscz)])
        _, port_b = wait_for_port(b.pid)
        check("score instance up", port_b is not None)
        if port_b:
            deadline = time.time() + 30
            sc = None
            while time.time() < deadline:
                _, res = call(port_b, token, "hello", rid=2)
                sc = res.get("result", {}).get("structuredContent", {}).get("score")
                if sc:
                    break
                time.sleep(1)
            check("hello reports the open score", bool(sc) and sc.get("path", "").endswith("p12-test.mscz"), sc)
            if sc:
                check("score: not dirty after open", sc.get("needSave") is False, sc)
                check("score: undo index 0", sc.get("undo", {}).get("index") == 0, sc.get("undo"))
                check("score: modes all off",
                      sc.get("modes") == {"noteInput": False, "textEdit": False, "drag": False}, sc.get("modes"))
                check("score: current view is the full score", bool(sc.get("currentView")), sc)
            rec_b = INSTANCES / f"{b.pid}.json"
            time.sleep(1)
            rb = json.loads(rec_b.read_text()) if rec_b.exists() else {}
            check("score instance's record names the score",
                  str((rb.get("score") or {}).get("path", "")).endswith("p12-test.mscz"), rb)
    finally:
        if b:
            b.stop()
        a.stop()
        tmp.cleanup()

    # SIGTERM skips MuseScore's shutdown (like a crash), so records may remain; they must be
    # ignored as dead and pruned by the next instance. Graceful quit removes them in onDeinit.
    time.sleep(1)
    left = [p for p in (INSTANCES / f"{a.pid}.json", INSTANCES / f"{b.pid}.json" if b else None) if p and p.exists()]
    c = App(bundle)
    try:
        wait_for_port(c.pid)
        time.sleep(1)
        check("records of killed instances pruned by the next start", not any(p.exists() for p in left), left)
    finally:
        c.stop()

    print()
    if failures:
        print(f"test_hello: FAIL ({len(failures)})")
        return 1
    print("test_hello: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
