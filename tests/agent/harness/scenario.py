# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent harness: scenario files and the engine that runs them.
#
# The file format is documented in tests/agent/scenarios/README.md. In short:
#
#   {"id": "S01", "title": "...", "fixture": "corpus/bach-bwv66-6.mscz",
#    "steps": [{"tool": "add_dynamic", "args": {...}, "expect": {"bars": "2"}},
#              {"tool": "score_read", "args": {...}, "error": "E_RANGE"},
#              {"assert": "unchanged"}],
#    "goldens": [{"format": "musicxml", "path": "goldens/S01.musicxml"}],
#    "invariants": {"validate": true, "undoRedo": true, "format": "mscz"}}
import json
import pathlib
import re

from .exports import compare, norm_xml, normalise, zip_members
from .instance import Instance, InstanceError, find_fixture

UNDO_ALIAS, REDO_ALIAS = "$undo", "$redo"
DEFAULT_INVARIANTS = {"validate": True, "undoRedo": True, "format": "mscz", "errorsLeaveScoreUnchanged": True}


class Result:
    """Check lines for one scenario: (status, label, detail), status in ok / FAIL / SKIP."""

    def __init__(self, sid, out=print):
        self.sid = sid
        self.lines = []
        self.out = out

    def add(self, status, label, detail=""):
        self.lines.append((status, label, detail))
        pad = {"ok": "  ok    ", "FAIL": "  FAIL  ", "SKIP": "  SKIP  "}[status]
        self.out(f"{pad}{self.sid} {label}" + (f"  ({detail})" if detail else ""))

    def check(self, label, ok, detail=""):
        self.add("ok" if ok else "FAIL", label, "" if ok else detail)
        return ok

    @property
    def failed(self):
        return any(s == "FAIL" for s, _, _ in self.lines)

    @property
    def skipped(self):
        return bool(self.lines) and all(s == "SKIP" for s, _, _ in self.lines)


# --- scenario files

def load(path):
    path = pathlib.Path(path)
    sc = json.loads(path.read_text())
    sc.setdefault("id", path.stem.split("_", 1)[0])
    sc["_file"] = path
    sc["_name"] = path.stem
    inv = dict(DEFAULT_INVARIANTS)
    inv.update(sc.get("invariants") or {})
    sc["invariants"] = inv
    g = sc.get("goldens", sc.get("golden"))
    sc["goldens"] = [] if g is None else ([g] if isinstance(g, dict) else g)
    return sc


def required_tools(sc, undo_tool, redo_tool):
    names = []
    for st in sc.get("steps", []):
        t = st.get("tool")
        if t == UNDO_ALIAS:
            t = undo_tool
        elif t == REDO_ALIAS:
            t = redo_tool
        if t and t not in names:
            names.append(t)
        # a batch needs the commands it runs
        for op in (st.get("args") or {}).get("operations") or []:
            c = isinstance(op, dict) and op.get("command")
            if c and c not in names:
                names.append(c)
    for t in sc.get("requires", []):
        if t not in names:
            names.append(t)
    return names


# --- values: captures, substitution, matchers

_VAR = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


def substitute(value, env):
    if isinstance(value, str):
        m = _VAR.fullmatch(value)
        if m:
            return env[m.group(1)]
        return _VAR.sub(lambda mm: str(env[mm.group(1)]), value)
    if isinstance(value, list):
        return [substitute(v, env) for v in value]
    if isinstance(value, dict):
        return {k: substitute(v, env) for k, v in value.items()}
    return value


_MISSING = object()


def get_path(obj, path):
    """'parts.0.name' -> obj['parts'][0]['name']; _MISSING when absent. '' is obj itself."""
    if path in ("", "."):
        return obj
    cur = obj
    for key in path.split("."):
        if isinstance(cur, dict) and key in cur:
            cur = cur[key]
        elif isinstance(cur, list) and re.fullmatch(r"-?\d+", key) and -len(cur) <= int(key) < len(cur):
            cur = cur[int(key)]
        else:
            return _MISSING
    return cur


def match(value, spec):
    """'' when value satisfies spec, else why not.

    spec is a literal (equality) or an object of operators, all of which must hold:
    eq, ne, contains, notContains, startswith, regex, len, gte, lte, gt, lt, exists, type, in,
    all (a list of specs that must all hold)."""
    if not (isinstance(spec, dict) and spec and all(k in _OPS for k in spec)):
        return "" if value == spec else f"{value!r} != {spec!r}"
    for op, arg in spec.items():
        why = _OPS[op](value, arg)
        if why:
            return why
    return ""


def _len(v, n):
    if v is _MISSING or not hasattr(v, "__len__"):
        return f"{v!r} has no length"
    return match(len(v), n) if isinstance(n, dict) else ("" if len(v) == n else f"length {len(v)} != {n}")


def _cmp(fn, sym):
    def op(v, a):
        try:
            return "" if v is not _MISSING and fn(v, a) else f"{v!r} not {sym} {a!r}"
        except TypeError:
            return f"{v!r} not comparable with {a!r}"
    return op


_TYPES = {"string": str, "integer": int, "number": (int, float), "boolean": bool, "array": list, "object": dict,
          "null": type(None)}
_OPS = {
    "eq": lambda v, a: "" if v == a else f"{v!r} != {a!r}",
    "ne": lambda v, a: "" if v != a else f"{v!r} == {a!r}",
    "contains": lambda v, a: "" if v is not _MISSING and a in v else f"{a!r} not in {str(v)[:300]!r}",
    "notContains": lambda v, a: "" if v is _MISSING or a not in v else f"{a!r} found in {str(v)[:300]!r}",
    "startswith": lambda v, a: "" if isinstance(v, str) and v.startswith(a) else f"{str(v)[:120]!r} does not start with {a!r}",
    "regex": lambda v, a: "" if isinstance(v, str) and re.search(a, v) else f"{str(v)[:200]!r} !~ /{a}/",
    "len": _len,
    "gte": _cmp(lambda v, a: v >= a, ">="),
    "lte": _cmp(lambda v, a: v <= a, "<="),
    "gt": _cmp(lambda v, a: v > a, ">"),
    "lt": _cmp(lambda v, a: v < a, "<"),
    "exists": lambda v, a: "" if (v is not _MISSING) == bool(a) else ("missing" if a else f"present: {v!r}"),
    "type": lambda v, a: "" if isinstance(v, _TYPES[a]) and not (a in ("integer", "number") and isinstance(v, bool))
    else f"{v!r} is not {a}",
    "in": lambda v, a: "" if v in a else f"{v!r} not in {a!r}",
    "all": lambda v, a: next((w for w in (match(v, s) for s in a) if w), ""),
}


def capture(spec, res):
    """A capture spec: 'path.in.structuredContent', or {"path": ...} / {"text": regex, "int": bool}."""
    if isinstance(spec, str):
        spec = {"path": spec}
    if "text" in spec:
        m = re.search(spec["text"], res.text)
        if not m:
            raise KeyError(f"/{spec['text']}/ not found in {res.text[:200]!r}")
        v = m.group(1) if m.groups() else m.group(0)
    else:
        v = get_path(res.structured, spec["path"])
        if v is _MISSING:
            raise KeyError(f"{spec['path']} not in result")
    return int(v) if spec.get("int") else v


# --- the engine

class Runner:
    def __init__(self, scenarios_dir, bundle=None, undo_tool="undo", redo_tool="redo",
                 update_goldens=False, out=print):
        self.dir = pathlib.Path(scenarios_dir)
        self.bundle = bundle
        self.undo_tool = undo_tool
        self.redo_tool = redo_tool
        self.update_goldens = update_goldens
        self.out = out
        self.available = None  # tool names offered by the build (from a probe instance)

    def probe(self):
        """Start the build once with no score to learn its tools."""
        with Instance(bundle=self.bundle) as inst:
            self.available = set(inst.tools())
        return self.available

    def has(self, tool):
        return tool in self.available or tool.rsplit("/", 1)[-1] in self.available

    def run(self, sc):
        res = Result(sc["id"], self.out)
        title = sc.get("title", sc["_name"])
        hooks = sc.get("needsHooks") or []
        missing_hooks = [h for h in hooks if not self.has(h)]
        missing = [t for t in required_tools(sc, self.undo_tool, self.redo_tool)
                   if t not in hooks and not self.has(t)]
        if missing_hooks or missing:
            why = []
            if missing_hooks:
                why.append("needs test hook(s) not in the build: " + ", ".join(missing_hooks) + " (see 'manual')")
            if missing:
                why.append("needs tools not in tools/list: " + ", ".join(missing))
            res.add("SKIP", title, "; ".join(why))
            return res
        fixture = find_fixture(sc["fixture"])
        if fixture is None:
            res.add("FAIL", title, f"fixture {sc['fixture']} not found (corpus: tools/fixtures/make_corpus_fixtures.py)")
            return res
        try:
            with Instance(fixture, bundle=self.bundle) as inst:
                self._run_in(inst, sc, res)
                if not inst.alive():
                    res.add("FAIL", "instance alive at the end", f"crash reports: {inst.crash_reports()}")
        except InstanceError as e:
            res.add("FAIL", title, f"instance: {e}")
        except (OSError, KeyError, ValueError) as e:
            res.add("FAIL", title, f"{type(e).__name__}: {e}")
        return res

    def _resolve_tool(self, name):
        if name == UNDO_ALIAS:
            return self.undo_tool
        if name == REDO_ALIAS:
            return self.redo_tool
        return name

    def _run_in(self, inst, sc, res):
        inv = sc["invariants"]
        fmt = inv["format"]
        env = dict(sc.get("vars") or {})
        v0 = inst.call("score_validate")
        initial_problems = v0.structured.get("problems", [])
        initial = inst.export(fmt)
        idx0 = inst.undo_index()
        snaps = {idx0: initial}  # undo index -> export at that index
        current = initial
        idx = idx0

        for n, st in enumerate(sc.get("steps", []), 1):
            if "assert" in st:
                what = st["assert"]
                label = f"step {n} assert {what}"
                if what == "unchanged":
                    diff = compare(fmt, initial, inst.export(fmt), "start", "now")
                    res.check(label, not diff, diff)
                elif what == "changed":
                    res.check(label, bool(compare(fmt, initial, inst.export(fmt))), "export equals the start")
                elif what == "undoIndex":
                    got = inst.undo_index()
                    want = idx0 + st.get("delta", 0)
                    res.check(label, got == want, f"undo index {got}, expected {want}")
                elif what == "exportMember":
                    # zip exports (mscz/mxl): every member whose name matches `member` must satisfy `text`
                    members = zip_members(inst.export(st.get("format", "mscz")))
                    names = [m for m in sorted(members) if re.search(st["member"], m)]
                    label += f" {st['member']}"
                    if not names:
                        res.add("FAIL", label, f"no member matches; members: {sorted(members)}")
                    for m in names:
                        why = match(members[m].decode("utf-8", "replace"), substitute(st["text"], env))
                        res.check(f"{label} [{m}]", not why, why[:300])
                elif what == "exportText":
                    # text exports (musicxml): the normalised export must satisfy `text`
                    data = norm_xml(inst.export(st.get("format", "musicxml")))
                    why = match(data.decode("utf-8", "replace"), substitute(st["text"], env))
                    res.check(label, not why, why[:300])
                else:
                    res.add("FAIL", label, "unknown assert")
                continue

            tool = self._resolve_tool(st["tool"])
            name = inst.tool_name(tool) or tool
            args = substitute(st.get("args") or {}, env)
            label = f"step {n} {tool}" + (f" ({st['note']})" if st.get("note") else "")
            r = inst.call(name, args)
            for key in ("path",):
                if isinstance(r.structured.get(key), str):
                    inst.forget_export_file(r.structured[key])
            want_err = st.get("isError", "error" in st)
            ok = res.check(label + (" -> error" if want_err else ""), r.is_error == want_err,
                           f"isError={r.is_error}: {r.text[:300]}")
            if ok and st.get("error"):
                res.check(label + f" error code {st['error']}", r.text.startswith(st["error"]), r.text[:300])
            for path, spec in (st.get("expect") or {}).items():
                why = match(get_path(r.structured, path), substitute(spec, env))
                res.check(f"{label} {path}", not why, why)
            if "text" in st:
                why = match(r.text, substitute(st["text"], env))
                res.check(f"{label} text", not why, why)
            for var, spec in (st.get("capture") or {}).items():
                try:
                    env[var] = capture(spec, r)
                except KeyError as e:
                    res.add("FAIL", f"{label} capture {var}", str(e))
            if not inst.alive():
                res.add("FAIL", f"{label}: instance alive", f"crash reports: {inst.crash_reports()}")
                return

            new_idx = inst.undo_index()
            if r.is_error:
                if inv["errorsLeaveScoreUnchanged"]:
                    res.check(f"{label} error left no undo entry", new_idx == idx, f"undo index {idx} -> {new_idx}")
                    diff = compare(fmt, current, inst.export(fmt), "before", "after")
                    res.check(f"{label} error left the score unchanged", not diff, diff)
                continue
            if new_idx != idx:
                now = inst.export(fmt)
                if tool in (self.undo_tool, self.redo_tool):
                    if new_idx in snaps:
                        diff = compare(fmt, snaps[new_idx], now, f"state {new_idx}", "now")
                        res.check(f"{label} restores state {new_idx}", not diff, diff)
                    snaps.setdefault(new_idx, now)
                else:
                    for k in [k for k in snaps if k >= new_idx]:
                        del snaps[k]
                    snaps[new_idx] = now
                current, idx = now, new_idx
            elif st.get("writes"):
                res.add("FAIL", f"{label} made an undo entry", f"undo index stayed {idx}")

        # --- goldens
        for g in sc["goldens"]:
            gpath = self.dir / g["path"]
            data = normalise(g["format"], inst.export(g["format"]))
            label = f"golden {g['format']} {g['path']}"
            if self.update_goldens:
                gpath.parent.mkdir(parents=True, exist_ok=True)
                gpath.write_bytes(data)
                res.add("ok", label + " written")
            elif not gpath.exists():
                res.add("FAIL", label, "golden missing; run with --update-goldens and review the diff")
            else:
                diff = compare(g["format"], gpath.read_bytes(), data, "golden", "export")
                res.check(label, not diff, diff)

        # --- invariants
        if inv["validate"]:
            v = inst.call("score_validate")
            probs = v.structured.get("problems", [])
            new = [p for p in probs if p not in initial_problems]
            res.check("invariant score_validate ok" + ("" if v.structured.get("ok") else " (no new problems)"),
                      not v.is_error and (v.structured.get("ok") is True or not new),
                      v.text[:300] if v.is_error else json.dumps(new)[:300])
        if inv["undoRedo"]:
            self._undo_redo(inst, res, fmt, snaps, idx0, idx, current)

    def _undo_redo(self, inst, res, fmt, snaps, idx0, top, final):
        if not self.has(self.undo_tool) or not self.has(self.redo_tool):
            res.add("SKIP", "invariant undo/redo", f"SKIP: no undo tool ({self.undo_tool}/{self.redo_tool} not in tools/list)")
            return
        if top == idx0:
            res.add("FAIL", "invariant undo/redo", "no step made an undo entry (set invariants.undoRedo false for read-only scenarios)")
            return
        undo, redo = inst.tool_name(self.undo_tool), inst.tool_name(self.redo_tool)
        idx = top
        for _ in range(abs(top - idx0) + 5):
            if idx == idx0:
                break
            r = inst.call(undo)
            if not res.check(f"invariant undo from state {idx}", not r.is_error, r.text[:300]):
                return
            nidx = inst.undo_index()
            if nidx in snaps:
                diff = compare(fmt, snaps[nidx], inst.export(fmt), f"state {nidx}", "after undo")
                res.check(f"invariant undo to state {nidx} restores its export", not diff, diff)
            idx = nidx
        res.check("invariant undo reaches the starting state", idx == idx0, f"stopped at {idx}, start {idx0}")
        for _ in range(abs(top - idx0) + 5):
            if idx == top:
                break
            r = inst.call(redo)
            if not res.check(f"invariant redo from state {idx}", not r.is_error, r.text[:300]):
                return
            nidx = inst.undo_index()
            if nidx in snaps:
                diff = compare(fmt, snaps[nidx], inst.export(fmt), f"state {nidx}", "after redo")
                res.check(f"invariant redo to state {nidx} restores its export", not diff, diff)
            idx = nidx
        diff = compare(fmt, final, inst.export(fmt), "edited", "after redo")
        res.check("invariant redo restores the edited export", idx == top and not diff, diff or f"at {idx}, want {top}")
