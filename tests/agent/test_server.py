#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent: P1.1 server tests (transport security and JSON-RPC basics).
#
# Launches the agent build off-screen, talks to it over HTTP like an MCP client would,
# and quits it. Standard library only.
#
# Usage: tests/agent/test_server.py [path/to/Bundle.app]   (default: agent.install/mscore.app)
import http.client
import json
import os
import pathlib
import signal
import stat
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_BUNDLE = ROOT / "agent.install/mscore.app"
AGENT_DIR = pathlib.Path.home() / "Library/Application Support/MuseScore/MuseScoreAgent/agent"
PORT_BASE, PORT_SPAN = 2412, 20

failures = []


def check(name, ok, detail=""):
    print(("  ok    " if ok else "  FAIL  ") + name + (f"  ({detail})" if detail and not ok else ""))
    if not ok:
        failures.append(name)


def request(port, method="POST", path="/mcp", body=None, headers=None, raw_body=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    hdrs = {"Host": f"127.0.0.1:{port}", "Content-Type": "application/json", "Accept": "application/json"}
    hdrs.update(headers or {})
    data = raw_body if raw_body is not None else (json.dumps(body).encode() if body is not None else None)
    conn.request(method, path, body=data, headers={k: v for k, v in hdrs.items() if v is not None})
    resp = conn.getresponse()
    payload = resp.read()
    conn.close()
    return resp.status, dict(resp.getheaders()), payload


def rpc(port, token, method, params=None, rid=1, extra_headers=None):
    body = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        body["params"] = params
    headers = {"Authorization": f"Bearer {token}"}
    headers.update(extra_headers or {})
    status, hdrs, payload = request(port, body=body, headers=headers)
    return status, hdrs, (json.loads(payload) if payload and hdrs.get("Content-Type", "").startswith("application/json") else payload)


def agent_pids():
    out = subprocess.run(["pgrep", "-f", "MacOS/mscore"], capture_output=True, text=True).stdout.split()
    return {int(p) for p in out}


def listening_ports(pid):
    out = subprocess.run(["lsof", "-a", "-p", str(pid), "-iTCP", "-sTCP:LISTEN", "-nP"], capture_output=True, text=True).stdout
    return [line.split()[8] for line in out.splitlines()[1:]]


def wait_for_port(pid, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for addr in listening_ports(pid):
            port = int(addr.rsplit(":", 1)[1])
            if PORT_BASE <= port < PORT_BASE + PORT_SPAN:
                return addr, port
        time.sleep(0.5)
    return None, None


class App:
    """An agent-build process started with `open -n -g` (new instance, in the background).

    Qt's offscreen/minimal platforms crash MuseScore 4.7.6 during GUI setup (with or without
    the agent module), so tests run the normal GUI without bringing it to the front."""

    def __init__(self, bundle, env=None, args=None):
        before = agent_pids()
        args_ = ["open", "-n", "-g", "-j"]  # -j: launch hidden, so it never covers the user's windows
        env = dict({"MUSESCORE_AGENT_BACKGROUND": "1"}, **(env or {}))  # never cover the user's windows
        for k, v in env.items():
            args_ += ["--env", f"{k}={v}"]
        subprocess.run(args_ + ["-a", str(bundle)] + list(args or []), check=True)
        self.pid = None
        deadline = time.time() + 30
        while time.time() < deadline and self.pid is None:
            new = agent_pids() - before
            self.pid = min(new) if new else None
            time.sleep(0.3)
        if self.pid is None:
            raise RuntimeError("agent build did not start")

    def alive(self):
        return subprocess.run(["kill", "-0", str(self.pid)], capture_output=True).returncode == 0

    def stop(self):
        if self.alive():
            os.kill(self.pid, signal.SIGTERM)
            deadline = time.time() + 20
            while self.alive() and time.time() < deadline:
                time.sleep(0.3)
            if self.alive():
                os.kill(self.pid, signal.SIGKILL)


def main():
    bundle = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BUNDLE
    if not bundle.exists():
        print(f"error: no bundle at {bundle}", file=sys.stderr)
        return 2

    before = agent_pids()
    if any(p for p in before):
        print("note: other mscore processes are running; port numbers may shift")

    a = App(bundle)
    b = None
    try:
        addr, port = wait_for_port(a.pid)
        check("server listens on a port in 2412-2431", port is not None, "no listening port")
        if port is None:
            return 1
        check("bound to 127.0.0.1 only", addr.startswith("127.0.0.1:"), addr)

        token = (AGENT_DIR / "token").read_text().strip()
        check("token is 64 hex chars", len(token) == 64 and all(c in "0123456789abcdef" for c in token))
        check("token file is owner read/write only", stat.S_IMODE((AGENT_DIR / "token").stat().st_mode) == 0o600,
              oct(stat.S_IMODE((AGENT_DIR / "token").stat().st_mode)))
        check("agent folder is owner only", stat.S_IMODE(AGENT_DIR.stat().st_mode) == 0o700,
              oct(stat.S_IMODE(AGENT_DIR.stat().st_mode)))

        init = {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}}

        # --- auth and transport security
        s, _, _ = request(port, body={"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": init})
        check("no token -> 401", s == 401, s)
        s, h, _ = request(port, body={"jsonrpc": "2.0", "id": 1, "method": "initialize"},
                          headers={"Authorization": "Bearer " + "0" * 64})
        check("wrong token -> 401", s == 401, s)
        check("401 carries WWW-Authenticate: Bearer", h.get("WWW-Authenticate") == "Bearer", h)
        s, _, _ = request(port, body={"jsonrpc": "2.0", "id": 1, "method": "initialize"},
                          headers={"Authorization": f"Basic {token}"})
        check("non-Bearer scheme -> 401", s == 401, s)
        s, _, _ = request(port, body={"jsonrpc": "2.0", "id": 1, "method": "initialize"},
                          headers={"Authorization": f"Bearer {token}", "Host": "evil.example"})
        check("foreign Host header -> 403 (DNS rebinding)", s == 403, s)
        s, _, _ = request(port, body={"jsonrpc": "2.0", "id": 1, "method": "initialize"},
                          headers={"Authorization": f"Bearer {token}", "Origin": "http://evil.example"})
        check("foreign Origin -> 403", s == 403, s)
        s, h, _ = rpc(port, token, "initialize", init, extra_headers={"Origin": "http://localhost:3000"})
        check("loopback Origin allowed and echoed", s == 200 and h.get("Access-Control-Allow-Origin") == "http://localhost:3000", (s, h))
        s, _, _ = request(port, method="GET", headers={"Authorization": f"Bearer {token}"})
        check("GET -> 405", s == 405, s)
        s, _, _ = request(port, path="/other", body={}, headers={"Authorization": f"Bearer {token}"})
        check("unknown path -> 404", s == 404, s)
        s, _, _ = request(port, body={}, headers={"Authorization": f"Bearer {token}", "Content-Type": "text/plain"})
        check("non-JSON content type -> 415", s == 415, s)

        # --- JSON-RPC
        s, _, res = rpc(port, token, "initialize", init)
        ok = s == 200 and isinstance(res, dict) and res.get("result", {}).get("serverInfo", {}).get("name") == "MuseScoreAgent"
        check("initialize -> serverInfo MuseScoreAgent", ok, res)
        if ok:
            check("protocol version 2025-11-25", res["result"]["protocolVersion"] == "2025-11-25")
            check("version names the agent build", "-agent" in res["result"]["serverInfo"]["version"], res["result"]["serverInfo"])
        s, _, res = rpc(port, token, "ping", rid=7)
        check("ping -> empty result, same id", s == 200 and res.get("id") == 7 and res.get("result") == {}, res)
        s, _, res = rpc(port, token, "tools/list")
        check("tools/list -> tools array", s == 200 and isinstance(res.get("result", {}).get("tools"), list), res)
        s, _, res = rpc(port, token, "tools/call", {"name": "nope", "arguments": {}})
        check("unknown tool -> isError result", s == 200 and res.get("result", {}).get("isError") is True, res)
        s, _, res = rpc(port, token, "tools/call", {"arguments": {}})
        check("tools/call without name -> -32602", s == 200 and res.get("error", {}).get("code") == -32602, res)
        s, _, res = rpc(port, token, "no/such/method")
        check("unknown method -> -32601", s == 200 and res.get("error", {}).get("code") == -32601, res)
        s, h, payload = request(port, raw_body=b"{not json", headers={"Authorization": f"Bearer {token}"})
        res = json.loads(payload) if payload else {}
        check("malformed JSON -> -32700", s == 200 and res.get("error", {}).get("code") == -32700, (s, payload[:200]))
        s, _, payload = request(port, body={"jsonrpc": "2.0", "method": "notifications/initialized"},
                                headers={"Authorization": f"Bearer {token}"})
        check("notification -> 202 Accepted", s == 202, s)
        s, h, payload = request(port, body={"jsonrpc": "2.0", "id": 3, "method": "ping"},
                                headers={"Authorization": f"Bearer {token}", "Accept": "text/event-stream"})
        check("Accept: text/event-stream -> SSE framing",
              s == 200 and h.get("Content-Type") == "text/event-stream" and payload.startswith(b"event: message\ndata: "), (s, h, payload[:80]))

        # --- a second instance takes the next free port
        b = App(bundle)
        _, port_b = wait_for_port(b.pid)
        check("second instance listens on a different port", port_b is not None and port_b != port, (port, port_b))
        if port_b:
            s, _, res = rpc(port_b, token, "ping", rid=9)
            check("same token works on the second instance", s == 200 and res.get("id") == 9, res)
    finally:
        if b:
            b.stop()
        a.stop()

    # --- kill switch
    c = App(bundle, env={"MUSESCORE_AGENT_DISABLE": "1"})
    try:
        time.sleep(12)
        check("MUSESCORE_AGENT_DISABLE=1 -> no server", c.alive() and not [
            p for p in listening_ports(c.pid) if PORT_BASE <= int(p.rsplit(":", 1)[1]) < PORT_BASE + PORT_SPAN])
    finally:
        c.stop()

    print()
    if failures:
        print(f"test_server: FAIL ({len(failures)})")
        return 1
    print("test_server: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
