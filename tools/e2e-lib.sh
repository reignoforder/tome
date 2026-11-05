# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by tools/e2e-test.sh and tools/e2e-multiplayer.sh: what their
# checks share.

FAILED=0
pass() { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }

# e2e_log <log> [n]: the log as checks see it, from line n+1: never the
# input driver's own lines ("Debug click ...", "Debug typing ..."), which
# repeat what the test sent. grep -a throughout: one stray byte must not
# make the log a "binary file".
e2e_log() { tail -n +$(( ${2:-0} + 1 )) "$1" 2>/dev/null | grep -av '\] Debug '; }

# e2e_has <regex>: stdin has a matching line. Reads all of it: with
# pipefail, a grep -q that stops early fails the pipe (SIGPIPE in the
# writer) on a log longer than the pipe's buffer.
e2e_has() { grep -aE "$1" >/dev/null; }

# e2e_set_enabled <state.json> <mod> <true|false>: turns a mod on or off as
# the mod menu would. Through a temp file and a rename, as Tome writes it:
# its watcher never reads half a file.
e2e_set_enabled() {
    python3 -I - "$1" "$2" "$3" <<'PY'
import json, os, sys
path, mod, enabled = sys.argv[1], sys.argv[2], sys.argv[3] == "true"
state = json.load(open(path))
state["mods"][mod]["enabled"] = enabled
with open(path + ".e2e", "w") as out:
    out.write(json.dumps(state, indent=2))
os.replace(path + ".e2e", path)
PY
}
