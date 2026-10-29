# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent harness: one running instance of the agent build.
#
# The build is a normal GUI app started in the background (`open -n -g -j`, with
# MUSESCORE_AGENT_BACKGROUND=1 so it never covers Liam's windows): Qt's offscreen/minimal
# platforms crash MuseScore 4.7.6, and there is no --agent-port flag. The port comes from
# the instance's discovery record (agent/instances/<pid>.json), with the listening socket as
# a fallback. A fixture is always copied to a fresh temp folder first; the fixture itself is
# never opened.
import json
import os
import pathlib
import shutil
import signal
import subprocess
import tempfile
import time

from .mcp import AGENT_DIR, call_tool, read_token, rpc

HERE = pathlib.Path(__file__).resolve()
ROOT = HERE.parents[3]


def _main_checkout(root):
    """The main checkout when running from .claude/worktrees/<name> (agent.install and the
    git-ignored corpus live only there)."""
    parts = root.parts
    if ".claude" in parts and "worktrees" in parts:
        i = parts.index(".claude")
        if i + 1 < len(parts) and parts[i + 1] == "worktrees":
            return pathlib.Path(*parts[:i])
    return root


MAIN_ROOT = _main_checkout(ROOT)
DEFAULT_BUNDLE = pathlib.Path(os.environ.get("MUSESCORE_AGENT_BUNDLE") or (
    ROOT / "agent.install/mscore.app" if (ROOT / "agent.install/mscore.app").exists()
    else MAIN_ROOT / "agent.install/mscore.app"))
# synthetic and corpus only: tests/fixtures/real (Liam's own scores) is never picked up
FIXTURE_DIRS = []
for _r in dict.fromkeys([ROOT, MAIN_ROOT]):
    for _sub in ("synthetic", "corpus"):
        FIXTURE_DIRS.append(_r / "tests/fixtures" / _sub)
PORT_BASE, PORT_SPAN = 2412, 20
CRASH_DIR = pathlib.Path.home() / "Library/Logs/DiagnosticReports"


class InstanceError(RuntimeError):
    pass


def find_fixture(name):
    """'corpus/bach-bwv66-6.mscz', 'bach-bwv66-6.mscz' or 'bach-bwv66-6' -> an existing path."""
    p = pathlib.Path(name)
    if p.is_absolute():
        if "fixtures/real" in str(p):
            raise InstanceError("tests/fixtures/real is off limits to the harness")
        return p if p.exists() else None
    if p.suffix != ".mscz":
        p = p.with_suffix(".mscz")
    for d in FIXTURE_DIRS:
        for cand in (d.parent / p, d / p.name):
            if cand.exists() and d.name in cand.parts:
                return cand
    return None


def list_fixtures():
    seen, out = set(), []
    for d in FIXTURE_DIRS:
        for f in sorted(d.glob("*.mscz")):
            key = (d.name, f.name)
            if key not in seen:
                seen.add(key)
                out.append(f)
    return out


def agent_pids():
    out = subprocess.run(["pgrep", "-f", "MacOS/mscore"], capture_output=True, text=True).stdout.split()
    return {int(p) for p in out}


def _listening_ports(pid):
    out = subprocess.run(["lsof", "-a", "-p", str(pid), "-iTCP", "-sTCP:LISTEN", "-nP"],
                         capture_output=True, text=True).stdout
    ports = []
    for line in out.splitlines()[1:]:
        addr = line.split()[8]
        port = int(addr.rsplit(":", 1)[1])
        if PORT_BASE <= port < PORT_BASE + PORT_SPAN:
            ports.append(port)
    return ports


class Instance:
    """A running agent build with (optionally) a copy of one fixture open.

        with Instance("corpus/bach-bwv66-6.mscz") as inst:
            r = inst.call("score_overview")

    Always stops the process and removes the temp folder and the export files it fetched."""

    def __init__(self, fixture=None, bundle=None, open_timeout=90, log=None):
        self.bundle = pathlib.Path(bundle or DEFAULT_BUNDLE)
        self.fixture = None
        self.score_path = None
        self.pid = None
        self.port = None
        self.token = None
        self.started = None
        self._tmp = None
        self._exports = []
        self._tools = None
        self._rid = 0
        self.log = log or (lambda msg: None)
        if fixture is not None:
            src = find_fixture(fixture) if not isinstance(fixture, pathlib.Path) else fixture
            if src is None or not src.exists():
                raise InstanceError(f"fixture not found: {fixture}")
            self.fixture = src
        self.open_timeout = open_timeout

    # --- lifecycle
    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
        return False

    def start(self):
        if not self.bundle.exists():
            raise InstanceError(f"no bundle at {self.bundle}")
        self._tmp = pathlib.Path(tempfile.mkdtemp(prefix="agent-scenario-"))
        args = []
        if self.fixture:
            self.score_path = self._tmp / self.fixture.name
            shutil.copy2(self.fixture, self.score_path)
            args = [str(self.score_path)]
        try:
            self._launch(args)
            self._wait_ready()
        except BaseException:
            self.stop()
            raise
        return self

    def _launch(self, args):
        before = agent_pids()
        cmd = ["open", "-n", "-g", "-j", "--env", "MUSESCORE_AGENT_BACKGROUND=1", "-a", str(self.bundle)] + args
        self.started = time.time()
        subprocess.run(cmd, check=True, capture_output=True)
        deadline = time.time() + 30
        while time.time() < deadline:
            new = agent_pids() - before
            if new:
                self.pid = min(new)
                return
            time.sleep(0.3)
        raise InstanceError("agent build did not start")

    def _find_port(self):
        rec = AGENT_DIR / "instances" / f"{self.pid}.json"
        try:
            port = json.loads(rec.read_text()).get("port")
            if port:
                return int(port)
        except (OSError, ValueError):
            pass
        ports = _listening_ports(self.pid)
        return ports[0] if ports else None

    def _wait_ready(self):
        deadline = time.time() + self.open_timeout
        while time.time() < deadline and self.port is None:
            if not self.alive():
                raise InstanceError(f"pid {self.pid} exited during start-up")
            self.port = self._find_port()
            if self.port is None:
                time.sleep(0.5)
        if self.port is None:
            raise InstanceError(f"pid {self.pid}: no agent port within {self.open_timeout} s")
        self.token = read_token()
        last = None
        while time.time() < deadline:
            if not self.alive():
                raise InstanceError(f"pid {self.pid} exited while opening {self.fixture}")
            try:
                r = self.call("hello")
            except OSError as e:  # server not answering yet
                last = e
                time.sleep(0.5)
                continue
            last = r
            app = r.structured.get("app", {})
            if app.get("pid") not in (None, self.pid):
                raise InstanceError(f"port {self.port} belongs to pid {app.get('pid')}, not {self.pid}")
            if not self.fixture:
                return
            score = r.structured.get("score") or {}
            if score and pathlib.Path(score.get("path", "")).name == self.score_path.name:
                return
            time.sleep(1)
        raise InstanceError(f"{self.fixture and self.fixture.name} did not open within {self.open_timeout} s "
                            f"(pid {self.pid}, port {self.port}, last hello {str(last)[:200]})")

    def alive(self):
        if self.pid is None:
            return False
        try:
            os.kill(self.pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        # a zombie is not alive
        st = subprocess.run(["ps", "-o", "stat=", "-p", str(self.pid)], capture_output=True, text=True).stdout.strip()
        return bool(st) and not st.startswith("Z")

    def crash_reports(self):
        """Crash reports macOS wrote for mscore since this instance started."""
        if not CRASH_DIR.exists() or self.started is None:
            return []
        return sorted(str(p) for p in CRASH_DIR.glob("mscore*")
                      if p.stat().st_mtime >= self.started - 1)

    def stop(self):
        try:
            if self.pid is not None and self.alive():
                os.kill(self.pid, signal.SIGTERM)
                deadline = time.time() + 20
                while self.alive() and time.time() < deadline:
                    time.sleep(0.3)
                if self.alive():
                    os.kill(self.pid, signal.SIGKILL)
                    time.sleep(0.5)
        except ProcessLookupError:
            pass
        finally:
            for p in self._exports:
                try:
                    p.unlink()
                except OSError:
                    pass
            self._exports = []
            if self._tmp:
                shutil.rmtree(self._tmp, ignore_errors=True)
                self._tmp = None

    # --- MCP
    def call(self, name, args=None, timeout=120):
        self._rid += 1
        return call_tool(self.port, self.token, name, args, rid=self._rid, timeout=timeout)

    def tools(self, refresh=False):
        """name -> tool description from tools/list. A name given as a URI
        (command://notation/x) is also reachable as x."""
        if self._tools is None or refresh:
            self._rid += 1
            _, res = rpc(self.port, self.token, "tools/list", rid=self._rid)
            tools = {}
            for t in res.get("result", {}).get("tools", []):
                tools[t["name"]] = t
                short = t["name"].rsplit("/", 1)[-1]
                tools.setdefault(short, t)
            self._tools = tools
        return self._tools

    def tool_name(self, name):
        """The name to call for `name` (short or URI), or None if the instance lacks it."""
        t = self.tools().get(name) or self.tools().get(name.rsplit("/", 1)[-1])
        return t["name"] if t else None

    def hello(self):
        return self.call("hello").structured

    def undo_index(self):
        score = self.hello().get("score") or {}
        return (score.get("undo") or {}).get("index")

    def export(self, fmt):
        """Export the open score; return the file's bytes. The export file is removed on stop()."""
        r = self.call("score_export", {"format": fmt})
        if r.is_error:
            raise InstanceError(f"score_export {fmt}: {r.text[:200]}")
        path = pathlib.Path(r.structured["path"])
        data = path.read_bytes()
        if path.parent == AGENT_DIR / "exports":
            self._exports.append(path)
        return data

    def forget_export_file(self, path):
        """Register an export/render file returned by a tool call for removal on stop()."""
        p = pathlib.Path(path)
        if p.parent == AGENT_DIR / "exports" and p not in self._exports:
            self._exports.append(p)
