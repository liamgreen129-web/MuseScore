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
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_server import AGENT_DIR, check, failures  # noqa: E402
from test_read import call, open_score  # noqa: E402
# normalisation and comparison live in the scenario harness (tests/agent/harness/exports.py)
from harness.exports import MSCZ_IGNORED, first_diff, norm_xml, zip_members  # noqa: E402,F401
from harness.exports import compare as _compare  # noqa: E402
from harness.instance import DEFAULT_BUNDLE, list_fixtures  # noqa: E402

# corpus, then synthetic; taken from the main checkout when run inside a worktree
# (agent.install and the git-ignored corpus live only there)
FIXTURES = sorted(f for f in list_fixtures() if f.parent.name == "corpus") + \
    sorted(f for f in list_fixtures() if f.parent.name == "synthetic")
FORMATS = {"musicxml": "musicxml", "mxl": "mxl", "midi": "mid", "mscz": "mscz"}


def compare(fmt, ref, got):
    """Return '' when equal, else a short description of the first difference."""
    return _compare(fmt, ref, got, "UI save", "export")


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
