#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent: Gate G1 "score_export matches a save from the UI".
#
# For each fixture, the reference is made by the same bundle's converter (`mscore -o`): for
# .mscz it calls NotationProject::save, the path File > Save takes; for the other formats it
# uses the registered writers the Export dialog uses. The agent's file comes from score_export
# in a running instance with the fixture open. Compared after removing what differs between
# any two saves (dates, zip timestamps):
#   musicxml  text, <encoding-date> removed
#   mxl       every zip member, the score XML normalised as above
#   midi      bytes
#   mscz      every zip member except thumbnail/audio/view settings (window state, not score)
#
# Usage: tests/agent/test_export.py [path/to/Bundle.app] [fixture-name-substring ...]
import io
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_server import AGENT_DIR, DEFAULT_BUNDLE, check, failures  # noqa: E402
from test_read import call, open_score  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = sorted((ROOT / "tests/fixtures/corpus").glob("*.mscz")) + sorted((ROOT / "tests/fixtures/synthetic").glob("*.mscz"))
FORMATS = {"musicxml": "musicxml", "mxl": "mxl", "midi": "mid", "mscz": "mscz"}
MSCZ_IGNORED = re.compile(r"(^Thumbnails/|audiosettings\.json$|viewsettings\.json$)")


def norm_xml(data):
    return re.sub(rb"<encoding-date>[^<]*</encoding-date>", b"", data)


def zip_members(data, ignored=None):
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        members = {n: z.read(n) for n in z.namelist() if not (ignored and ignored.search(n))}
    # the main .mscx is named after the file it was saved as; compare it as score.mscx
    main = [n for n in members if "/" not in n and n.endswith(".mscx")]
    if len(main) == 1 and main[0] != "score.mscx":
        old = main[0]
        members["score.mscx"] = members.pop(old)
        for n in members:
            if n.endswith("container.xml"):
                members[n] = members[n].replace(old.encode(), b"score.mscx")
    # and the container lists the ignored members only when they were written
    if ignored:
        for n in members:
            if n.endswith("container.xml"):
                members[n] = b"\n".join(line for line in members[n].split(b"\n")
                                        if not (b"full-path=" in line and ignored.search(line.split(b'"')[1].decode())))
    return members


def compare(fmt, ref, got):
    """Return '' when equal, else a short description of the first difference."""
    if fmt == "musicxml":
        a, b = norm_xml(ref), norm_xml(got)
    elif fmt == "midi":
        a, b = ref, got
    else:
        ra = zip_members(ref, MSCZ_IGNORED if fmt == "mscz" else None)
        rb = zip_members(got, MSCZ_IGNORED if fmt == "mscz" else None)
        if set(ra) != set(rb):
            return f"members differ: only in UI save {sorted(set(ra) - set(rb))}, only in export {sorted(set(rb) - set(ra))}"
        for n in sorted(ra):
            x, y = ra[n], rb[n]
            if n.endswith((".xml", ".musicxml")):
                x, y = norm_xml(x), norm_xml(y)
            if x != y:
                return f"{n}: {first_diff(x, y)}"
        return ""
    return "" if a == b else first_diff(a, b)


def first_diff(a, b):
    la, lb = a.splitlines(), b.splitlines()
    for i, (x, y) in enumerate(zip(la, lb)):
        if x != y:
            return f"line {i + 1}: UI {x[:120]!r} vs export {y[:120]!r}"
    return f"lengths {len(a)} vs {len(b)}"


def main():
    args = sys.argv[1:]
    bundle = pathlib.Path(args.pop(0)) if args and args[0].endswith(".app") else DEFAULT_BUNDLE
    fixtures = [f for f in FIXTURES if not args or any(s in f.name for s in args)]
    if not fixtures:
        print("error: no fixtures (run tools/fixtures/make_corpus_fixtures.py)", file=sys.stderr)
        return 2
    token = (AGENT_DIR / "token").read_text().strip()
    mscore = bundle / "Contents/MacOS/mscore"

    work = pathlib.Path(tempfile.mkdtemp(prefix="agent-g1-export-"))
    try:
        for fixture in fixtures:
            src = work / fixture.name
            shutil.copy2(fixture, src)  # neither side ever touches the fixture itself
            refs = {}
            for fmt, suffix in FORMATS.items():
                out = work / f"{fixture.stem}.ui.{suffix}"
                subprocess.run([str(mscore), "-o", str(out), str(src)], capture_output=True, timeout=120)
                refs[fmt] = out.read_bytes() if out.exists() else None

            app, port = open_score(bundle, src, token)
            try:
                for fmt in FORMATS:
                    err, ex, text = call(port, token, "score_export", {"format": fmt})
                    if err or refs[fmt] is None:
                        check(f"{fixture.stem} {fmt}", False, text[:200] if err else "converter wrote nothing")
                        continue
                    diff = compare(fmt, refs[fmt], pathlib.Path(ex["path"]).read_bytes())
                    check(f"{fixture.stem} {fmt} matches the UI save", not diff, diff)
            finally:
                app.stop()
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
