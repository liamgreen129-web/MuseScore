#!/usr/bin/env python
"""Regenerate the synthetic fixtures.

    ~/.venvs/MuseClaude/bin/python -I tools/fixtures/build_fixtures.py [--only NAME ...] [--list]
                                      [--work DIR] [--no-readme]

Writes tests/fixtures/synthetic/*.mscz and README.md. Needs the built agent app
(`agent.install/mscore.app`, or set MSCORE=/path/to/mscore).
"""
import argparse
import os
import shutil
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness  # noqa: E402

MODULES = ['fx_tuplets', 'fx_rhythm', 'fx_bars', 'fx_notation', 'fx_structure', 'fx_parts', 'fx_dense', 'fx_combined', 'fx_notes']
for mod in MODULES:
    try:
        __import__(mod)
    except ModuleNotFoundError as e:       # modules are added as the corpus grows
        if e.name != mod:
            raise


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--work', default=os.path.join(harness.ROOT, '.fixture-build'))
    ap.add_argument('--no-readme', action='store_true')
    ap.add_argument('--keep-work', action='store_true')
    a = ap.parse_args()
    if a.list:
        for n in sorted(harness.REGISTRY):
            print(n)
        return 0
    names = a.only or sorted(harness.REGISTRY)
    os.makedirs(harness.OUT_DIR, exist_ok=True)
    failed = []
    for n in names:
        fx = harness.REGISTRY[n]
        t0 = time.time()
        print('%s' % n, flush=True)
        try:
            summary = harness.build_one(fx, os.path.join(a.work, n), harness.OUT_DIR, print)
            print('  OK (%.1fs) %s' % (time.time() - t0, summary), flush=True)
        except Exception as e:  # noqa
            failed.append(n)
            print('  FAIL: %s: %s' % (type(e).__name__, e), flush=True)
            if not isinstance(e, harness.Fail):
                traceback.print_exc()
    if not a.no_readme and not a.only:
        with open(os.path.join(harness.OUT_DIR, 'README.md'), 'w', encoding='utf-8') as f:
            f.write(harness.readme_text())
    if not a.keep_work and not failed:
        shutil.rmtree(a.work, ignore_errors=True)
    print('%d fixtures, %d failed%s' % (len(names), len(failed), (': ' + ', '.join(failed)) if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
