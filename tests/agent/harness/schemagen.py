# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent harness: random values that satisfy a tool's JSON input schema.
#
# Valid means valid against the schema (required, types, enum/const, minimum/maximum,
# exclusive bounds, minItems/maxItems, minLength/maxLength, pattern, oneOf/anyOf, $ref), not
# meaningful for the score: a bar past the end is a valid integer and should be refused with
# E_RANGE, which is part of what the soak exercises. Hints taken from the open score (bars,
# staves, part names) make most calls land on real music.
import random
import re
import string

UNBOUNDED_SPAN = 20


class Hints:
    def __init__(self, bars=8, staves=1, parts=(), pages=2, state_version=0):
        self.bars = max(1, bars or 1)
        self.staves = max(1, staves or 1)
        self.parts = list(parts) or ["Piano"]
        self.pages = max(1, pages or 1)
        self.state_version = state_version or 0

    @classmethod
    def from_overview(cls, ov):
        sc = ov.get("score") or {}
        return cls(bars=sc.get("bars"), staves=sc.get("staves"),
                   parts=[p.get("name", "") for p in ov.get("parts", [])], state_version=sc.get("stateVersion"))

    def int_range(self, name):
        """A preferred [lo, hi] for an integer property, by name; None when there is none."""
        n = (name or "").lower()
        if n in ("from", "to", "measure", "frommeasure", "tomeasure", "bar", "bars"):
            return 1, self.bars + 1  # one past the end now and then
        if n in ("staff", "staves", "index"):
            return 1, self.staves
        if n in ("staffindex",):
            return 0, self.staves - 1
        if n in ("voice", "voices"):
            return 1, 4
        if n == "page":
            return 1, self.pages
        if n in ("since", "expectstateversion"):
            return 0, self.state_version + 2
        if n == "steps":
            return 1, 3
        if n == "verse":
            return 1, 3
        return None


def resolve(schema, root):
    while isinstance(schema, dict) and "$ref" in schema:
        ref = schema["$ref"]
        if not ref.startswith("#/"):
            return {}
        cur = root
        for key in ref[2:].split("/"):
            cur = cur.get(key, {}) if isinstance(cur, dict) else {}
        rest = {k: v for k, v in schema.items() if k != "$ref"}
        schema = dict(cur, **rest)
    return schema


def generate(schema, rng, hints=None, root=None, name=None, depth=0):
    hints = hints or Hints()
    root = root if root is not None else schema
    schema = resolve(schema or {}, root)
    for key in ("oneOf", "anyOf"):
        if key in schema:
            branch = resolve(rng.choice(schema[key]), root)
            merged = {k: v for k, v in schema.items() if k not in ("oneOf", "anyOf")}
            for k, v in branch.items():
                if k == "properties":
                    merged["properties"] = dict(merged.get("properties", {}), **v)
                elif k == "required":
                    merged["required"] = list(dict.fromkeys(merged.get("required", []) + v))
                else:
                    merged[k] = v
            return generate(merged, rng, hints, root, name, depth)
    if "const" in schema:
        return schema["const"]
    if "enum" in schema:
        return rng.choice(schema["enum"])
    t = schema.get("type")
    if isinstance(t, list):
        t = rng.choice(t)
    if t is None:
        t = "object" if "properties" in schema else "string"
    if t == "object":
        return _object(schema, rng, hints, root, depth)
    if t == "array":
        return _array(schema, rng, hints, root, name, depth)
    if t == "integer":
        return _integer(schema, rng, hints, name)
    if t == "number":
        return _number(schema, rng, name)
    if t == "boolean":
        return rng.random() < 0.5
    if t == "null":
        return None
    return _string(schema, rng, hints, name)


def _object(schema, rng, hints, root, depth):
    props = schema.get("properties", {})
    required = set(schema.get("required", []))
    out = {}
    for k, sub in props.items():
        if k in required or (depth < 4 and rng.random() < 0.5):
            out[k] = generate(sub, rng, hints, root, k, depth + 1)
    return out


def _array(schema, rng, hints, root, name, depth):
    lo = schema.get("minItems", 0)
    hi = schema.get("maxItems", lo + 3)
    hi = min(hi, lo + 3)
    n = rng.randint(lo, max(lo, hi))
    items = schema.get("items", {})
    out = []
    for _ in range(n):
        v = generate(items, rng, hints, root, name, depth + 1)
        if schema.get("uniqueItems") and v in out:
            continue
        out.append(v)
    tries = 0
    while len(out) < lo and tries < 50:  # uniqueItems could leave it short
        tries += 1
        v = generate(items, rng, hints, root, name, depth + 1)
        if not (schema.get("uniqueItems") and v in out):
            out.append(v)
    return out  # may still be short: the caller's validate() reports it as a generator miss


def _bounds(schema, integer):
    lo, hi = schema.get("minimum"), schema.get("maximum")
    if "exclusiveMinimum" in schema and isinstance(schema["exclusiveMinimum"], (int, float)):
        lo = schema["exclusiveMinimum"] + (1 if integer else 1e-6)
    if "exclusiveMaximum" in schema and isinstance(schema["exclusiveMaximum"], (int, float)):
        hi = schema["exclusiveMaximum"] - (1 if integer else 1e-6)
    return lo, hi


def _integer(schema, rng, hints, name):
    lo, hi = _bounds(schema, True)
    pref = hints.int_range(name)
    if pref:
        plo, phi = pref
        lo2 = plo if lo is None else max(lo, plo)
        hi2 = phi if hi is None else min(hi, phi)
        if lo2 <= hi2:
            return rng.randint(int(lo2), int(hi2))
    lo = 0 if lo is None else lo
    hi = lo + UNBOUNDED_SPAN if hi is None else hi
    return rng.randint(int(lo), int(hi))


def _number(schema, rng, name):
    lo, hi = _bounds(schema, False)
    if (name or "").lower() == "beat":
        v = rng.choice([1, 1.5, 2, 2.5, 3, 4])
        if (lo is None or v >= lo) and (hi is None or v <= hi):
            return v
    lo = 0 if lo is None else lo
    hi = lo + UNBOUNDED_SPAN if hi is None else hi
    return round(rng.uniform(lo, hi), 3)


_WORDS = ["", "a", "C", "Cmaj7", "dolce", "Allegro", "x" * 70, "é", "<b>", "1"]


def _string(schema, rng, hints, name):
    if "pattern" in schema:
        return regex_sample(schema["pattern"], rng)
    lo = schema.get("minLength", 0)
    hi = schema.get("maxLength")
    if (name or "").lower() == "part" and hints.parts:
        cands = [p for p in hints.parts if len(p) >= lo and (hi is None or len(p) <= hi)]
        if cands:
            return rng.choice(cands)
    cands = [w for w in _WORDS if len(w) >= lo and (hi is None or len(w) <= hi)]
    if cands:
        return rng.choice(cands)
    n = rng.randint(lo, lo if hi is None else max(lo, min(hi, lo + 8)))
    return "".join(rng.choice(string.ascii_letters) for _ in range(n))


# --- a small regex sampler (enough for the patterns tool schemas use: classes, groups,
# alternation, ?, *, +, {n}, {n,m}, anchors, \d \w \s escapes)

def regex_sample(pattern, rng):
    p = pattern
    if p.startswith("^"):
        p = p[1:]
    if p.endswith("$") and not p.endswith("\\$"):
        p = p[:-1]
    out = ""
    for _ in range(20):  # JSON Schema patterns are unanchored searches unless they say ^...$
        out, _pos = _alt(p, 0, rng)
        try:
            if re.search(pattern, out):
                return out
        except re.error:
            return out  # an unsupported pattern: validate() cannot check it either
    return out  # no match after 20 tries: the caller's validate() reports a generator miss


def _alt(p, i, rng):
    branches, cur = [], []
    while i < len(p) and p[i] != ")":
        if p[i] == "|":
            branches.append(cur)
            cur = []
            i += 1
            continue
        atom, i = _atom(p, i, rng)
        reps, i = _quant(p, i, rng)
        cur.append((atom, reps))
    branches.append(cur)
    chosen = rng.choice(branches)
    s = "".join("".join(a() for _ in range(r)) for a, r in chosen)
    return s, i


def _atom(p, i, rng):
    c = p[i]
    if c == "(":
        j = i + 1
        if p.startswith("?:", j):
            j += 2
        # capture the group's text so each repetition re-samples it
        depth, k = 1, j
        while k < len(p) and depth:
            if p[k] == "\\":
                k += 2
                continue
            depth += {"(": 1, ")": -1}.get(p[k], 0)
            k += 1
        inner = p[j:k - 1]
        return (lambda: _alt(inner, 0, rng)[0]), k
    if c == "[":
        k = i + 1
        neg = p[k:k + 1] == "^"
        if neg:
            k += 1
        chars = []
        while k < len(p) and (p[k] != "]" or k == i + 1):
            if p[k] == "\\" and k + 1 < len(p):
                chars += _escape(p[k + 1])
                k += 2
            elif k + 2 < len(p) and p[k + 1] == "-" and p[k + 2] != "]":
                chars += [chr(x) for x in range(ord(p[k]), ord(p[k + 2]) + 1)]
                k += 3
            else:
                chars.append(p[k])
                k += 1
        if neg:
            chars = [ch for ch in string.ascii_letters + string.digits if ch not in chars]
        return (lambda: rng.choice(chars or ["a"])), k + 1
    if c == "\\":
        chars = _escape(p[i + 1])
        return (lambda: rng.choice(chars)), i + 2
    if c == ".":
        return (lambda: rng.choice(string.ascii_letters)), i + 1
    return (lambda: c), i + 1


def _escape(c):
    return {"d": list(string.digits), "w": list(string.ascii_letters + string.digits + "_"),
            "s": [" "]}.get(c, [c])


def _quant(p, i, rng):
    if i >= len(p):
        return 1, i
    c = p[i]
    if c == "?":
        return rng.randint(0, 1), i + 1
    if c == "*":
        return rng.randint(0, 3), i + 1
    if c == "+":
        return rng.randint(1, 3), i + 1
    if c == "{":
        m = re.match(r"\{(\d+)(,(\d*))?\}", p[i:])
        if m:
            lo = int(m.group(1))
            hi = lo if not m.group(2) else (int(m.group(3)) if m.group(3) else lo + 3)
            return rng.randint(lo, hi), i + m.end()
    return 1, i


def validate(value, schema, root=None):
    """A light check that `value` satisfies `schema` (used by the self-test); '' when it does."""
    root = root if root is not None else schema
    schema = resolve(schema or {}, root)
    for key in ("oneOf", "anyOf"):
        if key in schema:
            base = {k: v for k, v in schema.items() if k not in ("oneOf", "anyOf")}
            if not any(not validate(value, dict(base, **resolve(b, root)), root) for b in schema[key]):
                return f"no {key} branch matches {value!r}"
            return ""
    if "const" in schema:
        return "" if value == schema["const"] else f"{value!r} != const"
    if "enum" in schema:
        return "" if value in schema["enum"] else f"{value!r} not in enum"
    t = schema.get("type")
    types = t if isinstance(t, list) else [t] if t else []
    pytypes = {"object": dict, "array": list, "string": str, "integer": int, "number": (int, float),
               "boolean": bool, "null": type(None)}
    if types and not any(isinstance(value, pytypes[x]) and not (x in ("integer", "number") and isinstance(value, bool))
                         for x in types):
        return f"{value!r} is not {types}"
    if isinstance(value, dict):
        for r in schema.get("required", []):
            if r not in value:
                return f"missing {r}"
        for k, v in value.items():
            if k in schema.get("properties", {}):
                why = validate(v, schema["properties"][k], root)
                if why:
                    return f"{k}: {why}"
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0) or len(value) > schema.get("maxItems", 1 << 30):
            return f"array length {len(value)}"
        for v in value:
            why = validate(v, schema.get("items", {}), root)
            if why:
                return why
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        lo, hi = _bounds(schema, isinstance(value, int))
        if (lo is not None and value < lo) or (hi is not None and value > hi):
            return f"{value} outside [{lo}, {hi}]"
    if isinstance(value, str):
        if len(value) < schema.get("minLength", 0) or len(value) > schema.get("maxLength", 1 << 30):
            return f"string length {len(value)}"
        if "pattern" in schema and not re.search(schema["pattern"], value):
            return f"{value!r} !~ {schema['pattern']}"
    return ""


if __name__ == "__main__":
    # self-test against the shapes the spec sheets propose
    r = random.Random(0)
    samples = [
        {"type": "object", "required": ["pitch"], "properties": {"pitch": {"oneOf": [
            {"type": "string", "pattern": "^[A-Ga-g](bb|b|#|##|x)?-?[0-9]$"},
            {"type": "integer", "minimum": 0, "maximum": 127}]}}},
        {"type": "object", "required": ["staff", "voice", "fromMeasure", "content"], "properties": {
            "staff": {"$ref": "#/$defs/StaffRef"}, "voice": {"type": "integer", "minimum": 1, "maximum": 4},
            "fromMeasure": {"type": "integer", "minimum": 1}, "content": {"type": "string"}},
         "$defs": {"StaffRef": {"type": "object", "required": ["part"], "properties": {
             "part": {"type": "string"}, "staffIndex": {"type": "integer", "minimum": 0}}}}},
        {"type": "object", "properties": {"bpm": {"type": "number", "exclusiveMinimum": 0},
                                          "label": {"type": "string", "maxLength": 60},
                                          "tags": {"type": "array", "minItems": 1, "maxItems": 2,
                                                   "items": {"enum": ["a", "b", "c"]}}}},
    ]
    for s in samples:
        for _ in range(500):
            v = generate(s, r, Hints(bars=10, staves=2, parts=["Flute", "Oboe"]))
            why = validate(v, s)
            assert not why, (why, v)
    print("schemagen self-test: ok")
