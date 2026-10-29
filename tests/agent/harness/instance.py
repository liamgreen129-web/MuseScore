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


def allowed_fixture(path):
    """`path` resolved, if it is inside tests/fixtures/{synthetic,corpus}; otherwise InstanceError.
    Resolving first means neither `..` nor a symlink can reach tests/fixtures/real."""
    real = pathlib.Path(path).resolve()
    for d in FIXTURE_DIRS:
        try:
            real.relative_to(d.resolve())
            return real
        except ValueError:
            continue
    raise InstanceError(f"{path}: only tests/fixtures/synthetic and tests/fixtures/corpus may be opened")


def find_fixture(name):
    """'corpus/bach-bwv66-6.mscz', 'bach-bwv66-6.mscz' or 'bach-bwv66-6' -> an existing path."""
    p = pathlib.Path(name)
    if p.is_absolute():
        return allowed_fixture(p) if p.exists() else None
    if p.suffix != ".mscz":
        p = p.with_suffix(".mscz")
    for d in FIXTURE_DIRS:
        for cand in (d.parent / p, d / p.name):
            if cand.exists():
                try:
                    return allowed_fixture(cand)
                except InstanceError:
                    continue
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


def agent_pids(bundle):
    """Pids running exactly this bundle's executable. Never a bare "MacOS/mscore" match: that
    would also catch the official MuseScore 4 and the installed MuseScore Agent.app."""
    exe = str(pathlib.Path(bundle).resolve() / "Contents/MacOS/mscore")
    out = subprocess.run(["ps", "-axo", "pid=,command="], capture_output=True, text=True).stdout
    pids = set()
    for line in out.splitlines():
        pid, _, cmd = line.strip().partition(" ")
        if cmd == exe or cmd.startswith(exe + " "):
            pids.add(int(pid))
    return pids


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

    def __init__(self, fixture=None, bundle=None, open_timeout=90, log=None, env=None):
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
        self.env = dict(env or {})
        if fixture is not None:
            src = find_fixture(str(fixture))
            if src is None:
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
        self._before = agent_pids(self.bundle)
        env = ["--env", "MUSESCORE_AGENT_BACKGROUND=1"]
        for k, v in self.env.items():
            env += ["--env", f"{k}={v}"]
        cmd = ["open", "-n", "-g", "-j"] + env + ["-a", str(self.bundle)] + args
        self.started = time.time()
        subprocess.run(cmd, check=True, capture_output=True)
        deadline = time.time() + 30
        while time.time() < deadline:
            if agent_pids(self.bundle) - self._before:
                return
            time.sleep(0.3)
        raise InstanceError("agent build did not start")

    def _find_port(self, pid):
        rec = AGENT_DIR / "instances" / f"{pid}.json"
        try:
            port = json.loads(rec.read_text()).get("port")
            if port:
                return int(port)
        except (OSError, ValueError):
            pass
        ports = _listening_ports(pid)
        return ports[0] if ports else None

    def _claims(self, pid, port):
        """True if the instance on `port` is `pid` with our score open (or, without a fixture,
        with no score). False if it is someone else's. None if it cannot tell yet."""
        try:
            r = call_tool(port, self.token, "hello", None, rid=1, timeout=10)
        except OSError:
            return None
        if r.structured.get("app", {}).get("pid") not in (None, pid):
            return False
        score = r.structured.get("score") or {}
        if not self.fixture:
            return not score
        if not score:
            return None  # still opening
        try:
            return pathlib.Path(score.get("path", "")).resolve() == self.score_path.resolve()
        except OSError:
            return False

    def _wait_ready(self):
        """Adopt the new process of our bundle that answers `hello` with our temp score open.
        Two runs may start at once (PLAN allows two implementers), so a new pid is not ours until
        it says so; one that has another score open is left alone."""
        deadline = time.time() + self.open_timeout
        self.token = read_token()
        foreign = set()
        while time.time() < deadline:
            cands = agent_pids(self.bundle) - self._before - foreign
            for pid in sorted(cands):
                port = self._find_port(pid)
                if port is None:
                    continue
                claim = self._claims(pid, port)
                if claim:
                    self.pid, self.port = pid, port
                    return
                if claim is False:
                    foreign.add(pid)
            time.sleep(0.5)
        raise InstanceError(f"{self.fixture and self.fixture.name} did not open within {self.open_timeout} s "
                            f"(new pids of this bundle: {sorted(agent_pids(self.bundle) - self._before)}, "
                            f"others' instances: {sorted(foreign)})")

    def _orphans(self):
        """After a failed start: new processes of our bundle that never claimed another score.
        Only these are stopped; the official app and other runs' instances never match."""
        if not hasattr(self, "_before"):
            return []
        out = []
        for pid in agent_pids(self.bundle) - self._before:
            port = self._find_port(pid)
            claim = self._claims(pid, port) if port and self.token else None
            if claim is not False:
                out.append(pid)
        return out

    def alive(self, pid=None):
        pid = self.pid if pid is None else pid
        if pid is None:
            return False
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        # a zombie is not alive
        st = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
        return bool(st) and not st.startswith("Z")

    def crash_reports(self):
        """Crash reports macOS wrote for mscore since this instance started."""
        if not CRASH_DIR.exists() or self.started is None:
            return []
        import plistlib
        import re
        try:
            with open(self.bundle / "Contents/Info.plist", "rb") as f:
                bundle_id = plistlib.load(f).get("CFBundleIdentifier")
        except (OSError, ValueError):
            bundle_id = None
        out = []
        for p in CRASH_DIR.glob("mscore*.ips"):
            if p.stat().st_mtime < self.started - 1:
                continue
            try:
                text = p.read_text(errors="replace")
                header = json.loads(text.split("\n", 1)[0])
            except (OSError, ValueError):
                continue
            # the official app writes mscore-*.ips too: only this bundle, and this pid when known
            if header.get("bundleID") != bundle_id:
                continue
            m = re.search(r'"pid"\s*:\s*(\d+)', text)
            if self.pid is None or (m and int(m.group(1)) == self.pid):
                out.append(str(p))
        return sorted(out)

    def _kill(self, pid):
        try:
            if self.alive(pid):
                os.kill(pid, signal.SIGTERM)
                deadline = time.time() + 20
                while self.alive(pid) and time.time() < deadline:
                    time.sleep(0.3)
                if self.alive(pid):
                    os.kill(pid, signal.SIGKILL)
                    time.sleep(0.5)
        except ProcessLookupError:
            pass

    def stop(self):
        try:
            for pid in ([self.pid] if self.pid is not None else self._orphans()):
                self._kill(pid)
        finally:
            self.__dict__.pop("_before", None)  # a second stop() never looks for orphans
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
