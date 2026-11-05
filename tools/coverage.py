#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Line coverage of a --coverage build (tools/coverage.sh), by section.

    tools/coverage.py <build folder> [--min PERCENT] [--files] [--lines FILE]

Reads every .gcda in the build with the toolchain's gcov (JSON), merges the
lines of each source file over all the modules that compiled it (tome.mix,
the test programs, mods) and prints each section's share of lines run. A
section is a source folder; sdk/examples/* and sdk/templates/* count as one
each. Vendored code (loader/third_party) and the tests themselves aren't
counted. --min: exit 1 if a section is below it. --files: the files below
it too, most lines missed first. --lines FILE: the lines never run, file by
file (for finding what to test next).
"""
import argparse
import collections
import concurrent.futures
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXCLUDED = ("loader/third_party/", "tests/", "fuzz/")
ROLLED_UP = ("sdk/examples", "sdk/templates")


def section(path):
    for prefix in ROLLED_UP:
        if path.startswith(prefix + "/"):
            return prefix
    return os.path.dirname(path)


def gcdas(build):
    # Not glob("**"): it follows links, and the build's Wine prefix links z: to /.
    for folder, dirs, files in os.walk(build):
        for name in files:
            if name.endswith(".gcda"):
                yield os.path.join(folder, name)


def read(build, gcov):
    lines = collections.defaultdict(dict)  # file -> line number -> run
    root = ROOT + os.sep
    with tempfile.TemporaryDirectory() as out, concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        def run(gcda):
            # A file gcov can't read is left out: say so, or its lines just vanish from the report.
            try:
                done = subprocess.run([gcov, "--json-format", "--stdout", gcda], capture_output=True, cwd=out,
                                      stdin=subprocess.DEVNULL, timeout=120)
            except subprocess.TimeoutExpired:
                print("coverage: gcov timed out on %s; left out" % gcda, file=sys.stderr)
                return b""
            if done.returncode != 0:
                print("coverage: gcov failed on %s; left out: %s" % (gcda, done.stderr.decode(errors="replace").strip()),
                      file=sys.stderr)
            return done.stdout
        for stdout in pool.map(run, gcdas(build)):
            for chunk in stdout.splitlines():
                try:
                    data = json.loads(chunk)
                except ValueError:
                    continue
                cwd = data.get("current_working_directory", "")
                for f in data.get("files", []):
                    path = os.path.normpath(os.path.join(cwd, f["file"]))
                    if not path.startswith(root):
                        continue
                    rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
                    if rel.startswith("build") or "/_deps/" in rel or rel.startswith(EXCLUDED):
                        continue
                    for line in f["lines"]:
                        n = line["line_number"]
                        lines[rel][n] = lines[rel].get(n, False) or line["count"] > 0
    return lines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build")
    parser.add_argument("--min", type=float, default=0.0)
    parser.add_argument("--files", action="store_true")
    parser.add_argument("--lines")
    parser.add_argument("--gcov", default=os.environ.get("GCOV", "i686-w64-mingw32-gcov"))
    args = parser.parse_args()

    lines = read(os.path.abspath(args.build), args.gcov)
    if not lines:
        print("coverage: no coverage data in %s (a --coverage build, after its tests?)" % args.build)
        return 1
    sections = collections.defaultdict(lambda: [0, 0])
    for path, ls in lines.items():
        s = sections[section(path)]
        s[0] += sum(ls.values())
        s[1] += len(ls)

    failed = False
    run_total = sum(s[0] for s in sections.values())
    all_total = sum(s[1] for s in sections.values())
    for name, (run, total) in sorted(sections.items()):
        share = 100.0 * run / total
        low = share < args.min
        failed |= low
        print("%6.1f%%  %6d/%-6d %s%s" % (share, run, total, name, "  (below %g%%)" % args.min if low else ""))
    print("%6.1f%%  %6d/%-6d total" % (100.0 * run_total / all_total, run_total, all_total))

    if args.files:
        print("\nfiles below %g%%:" % args.min)
        rows = [(path, sum(ls.values()), len(ls)) for path, ls in lines.items()]
        for path, run, total in sorted(rows, key=lambda r: r[1] - r[2]):
            if 100.0 * run < args.min * total:
                print("%6.1f%%  %5d missed  %s" % (100.0 * run / total, total - run, path))
    if args.lines:
        with open(args.lines, "w") as out:
            for path in sorted(lines):
                missed = sorted(n for n, run in lines[path].items() if not run)
                if missed:
                    out.write("%s: %s\n" % (path, " ".join(map(str, missed))))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
