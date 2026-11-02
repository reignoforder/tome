# SPDX-License-Identifier: GPL-3.0-or-later
"""Puts an SPDX-License-Identifier line at the top of each source file:
MIT-0 in what mods compile in (MIT0 below, sdk/LICENSE-MIT-0), MIT in the
rest of sdk/ (sdk/LICENSE), GPL-3.0-or-later everywhere else (LICENSE).

usage: python3 -I tools/spdx.py           add the missing lines
       python3 -I tools/spdx.py --check   list files without one (a test)

Files come from git (tracked, and untracked ones that aren't ignored).
Not third-party code (loader/third_party/monocypher carries its own),
sdk/templates/ (mod makers copy them to start a mod under their own
license), data (JSON, FDF, JASS, fuzz inputs) or docs. Generated files get
theirs from their generator.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = "SPDX-License-Identifier:"
SLASHES = {".cpp", ".hpp", ".h", ".inc", ".rs"}
HASHES = {".py", ".sh", ".cmake", ".toml", ".yml", ".yaml"}
HASH_NAMES = {"CMakeLists.txt", "Makefile", "Containerfile"}
SKIP = ("loader/third_party/monocypher/", "sdk/templates/")
# Third-party parts (loader/third_party/NOTICES.md): Unicode's case
# mappings, as data; an inflate altered from puff.c.
OTHER = {"loader/src/core/upcase_table.inc": "Unicode-3.0",
         "loader/src/core/zip.cpp": "GPL-3.0-or-later AND Zlib"}
# Built into mods: MIT-0, so a mod owes no notice (sdk/README.md, "License").
MIT0 = ("sdk/include/", "sdk/support/fpreset.c", "sdk/support/tome_wasm.c",
        "sdk/rust/tome/", "sdk/rust/tome-sys/", "sdk/rust/wasm/tome-wasm/")
SKIPPED = 77  # git can't list the files here: the test's SKIP_RETURN_CODE (tests/CMakeLists.txt)


def license_of(path):
    if path in OTHER:
        return OTHER[path]
    if path.startswith(MIT0):
        return "MIT-0"
    return "MIT" if path.startswith("sdk/") else "GPL-3.0-or-later"


def comment_of(path):
    name = os.path.basename(path)
    ext = os.path.splitext(name)[1]
    if name in HASH_NAMES or ext in HASHES:
        return "# %s %s"
    # The SDK's C headers and sources use C comments only.
    if ext == ".c" or (ext == ".h" and path.startswith("sdk/")):
        return "/* %s %s */"
    if ext in SLASHES:
        return "// %s %s"
    return None


def is_shebang(text):
    return text.startswith("#!") and not text.startswith("#![")  # not Rust's #![...]


def files():
    # GIT: the one CMake found (tests/CMakeLists.txt). safe.directory: a
    # checkout owned by another user (a container's).
    out = subprocess.run([os.environ.get("GIT") or "git", "-c", "safe.directory=" + ROOT,
                          "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
                         cwd=ROOT, check=True, capture_output=True).stdout
    for path in sorted(set(out.decode("utf-8").split("\0"))):
        if path and not path.startswith(SKIP) and comment_of(path) \
                and os.path.isfile(os.path.join(ROOT, path)):
            yield path


def main():
    # Anything else (a typo of --check) would rewrite files.
    if sys.argv[1:] not in ([], ["--check"]):
        print("usage: python3 -I tools/spdx.py [--check]", file=sys.stderr)
        return 2
    check = sys.argv[1:] == ["--check"]
    try:
        paths = list(files())
    except (OSError, subprocess.CalledProcessError) as e:
        # No git, or a checkout it can't read here: e.g. a worktree whose
        # repository isn't mounted in the container.
        why = e.stderr.decode("utf-8", "replace").strip() if isinstance(e, subprocess.CalledProcessError) else e
        print("skipped: git can't list the files (%s)" % why, file=sys.stderr)
        return SKIPPED
    missing, wrong = [], []
    for path in paths:
        full = os.path.join(ROOT, path)
        with open(full, encoding="utf-8", newline="") as f:
            text = f.read()
        want = license_of(path)
        # The first line, or the second after a shebang (CLAUDE.md: a file
        # "starts with" it).
        lines = text.splitlines()
        first = lines[1:2] if lines and is_shebang(text) else lines[:1]
        if first and TAG in first[0]:
            if not first[0].rstrip(" */").endswith(TAG + " " + want):
                wrong.append("%s: SPDX-License-Identifier isn't %s" % (path, want))
            continue
        if any(TAG in l for l in lines[:5]):
            # Lower down: adding one would make two.
            wrong.append("%s: SPDX-License-Identifier isn't on its first line" % path)
            continue
        missing.append(path)
        if check:
            continue
        line = comment_of(path) % (TAG, want) + "\n"
        at = 0
        if is_shebang(text):
            if "\n" not in text:
                text += "\n"
            at = text.index("\n") + 1
        with open(full, "w", encoding="utf-8", newline="") as f:
            f.write(text[:at] + line + text[at:])
    for message in wrong:
        print(message)
    if check:
        for path in missing:
            print("%s: no SPDX-License-Identifier (run tools/spdx.py)" % path)
        return 1 if missing or wrong else 0
    print("added %d, %d wrong" % (len(missing), len(wrong)))
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
