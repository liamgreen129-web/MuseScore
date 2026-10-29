# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent harness: MCP over HTTP (JSON-RPC, bearer token, loopback only).
import http.client
import json
import pathlib
from collections import namedtuple

AGENT_DIR = pathlib.Path.home() / "Library/Application Support/MuseScore/MuseScoreAgent/agent"

# is_error: bool; structured: dict ({} when absent); text: first text content ("" when absent)
ToolResult = namedtuple("ToolResult", "is_error structured text")


def read_token():
    return (AGENT_DIR / "token").read_text().strip()


def rpc(port, token, method, params=None, rid=1, timeout=120):
    """One JSON-RPC request. Returns (http status, decoded body or raw bytes)."""
    body = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        body["params"] = params
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        conn.request("POST", "/mcp", body=json.dumps(body).encode(), headers={
            "Host": f"127.0.0.1:{port}", "Content-Type": "application/json",
            "Accept": "application/json", "Authorization": f"Bearer {token}"})
        resp = conn.getresponse()
        payload = resp.read()
        ctype = resp.getheader("Content-Type", "")
    finally:
        conn.close()
    if payload and ctype.startswith("application/json"):
        return resp.status, json.loads(payload)
    return resp.status, payload


def call_tool(port, token, name, args=None, rid=1, timeout=120):
    """tools/call. Returns ToolResult(is_error, structuredContent, text).

    A JSON-RPC level error (no `result`) is reported as is_error=True with the error's
    message as text, prefixed by `RPC<code>`."""
    status, res = rpc(port, token, "tools/call", {"name": name, "arguments": args or {}}, rid=rid, timeout=timeout)
    if not isinstance(res, dict):
        return ToolResult(True, {}, f"HTTP{status} {res[:200]!r}")
    if "error" in res:
        err = res["error"]
        return ToolResult(True, {}, f"RPC{err.get('code')} {err.get('message', '')}")
    r = res.get("result", {})
    content = r.get("content") or [{}]
    return ToolResult(bool(r.get("isError")), r.get("structuredContent") or {}, content[0].get("text", ""))
