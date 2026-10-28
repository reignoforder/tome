#!/usr/bin/env bash
# Developer helper: runs the build's tome.mix in the game under Wine, in a
# test game folder (tools/test-env.sh), never in your own install.
#
#   tools/game.sh start [VAR=value ...] [-- game args]   restart the game (stops any running copy)
#   tools/game.sh stop                                   stop the game
#   tools/game.sh wait <regex> [seconds]                 wait for a tome.log line (default 30 s)
#   tools/game.sh shot <file.png>                        screenshot of the game's display
#   tools/game.sh log                                    print tome.log
#   tools/game.sh dir                                    print the game folder
#
# Examples:
#   tools/game.sh start TOME_DEBUG_OPEN_MOD_MENU=1
#   tools/game.sh start "TOME_DEBUG_CLICKS=wait:Main menu set up@0;0.874,0.84@4000"
#   tools/game.sh start -- -loadfile 'Maps\Test\test.w3m'
#
# The game folder is <build>/test-env/game: links to your install's files
# (GAME_DIR, default ./WarCraft III) with the build's tome.mix, and its own
# tome/ (put mods to try in tome/mods/ there), Maps/, save/ and replay/.
# BUILD picks the build folder (default ./build). HEADLESS=1 runs the game
# in its own X server (Xvfb) instead of on your screen. TOME_HOME (a Windows
# path) moves tome/ elsewhere, as in the game.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(realpath -m "${BUILD:-$ROOT/build}")"
source "$ROOT/tools/test-env.sh"
GAME="$BUILD/test-env/game"
STATE_FILE="$BUILD/test-env/display"  # "<Xvfb pid> <display>" of a headless game
LOG_DIR="$GAME"
if [[ -n "${TOME_HOME:-}" ]]; then
    LOG_DIR="$(winepath -u "$TOME_HOME" 2>/dev/null || echo "$GAME")"
fi
LOG="$LOG_DIR/tome/tome.log"

if [[ -f "$STATE_FILE" ]]; then
    read -r XVFB_PID XVFB_DISPLAY <"$STATE_FILE"
    kill -0 "$XVFB_PID" 2>/dev/null || { XVFB_PID="" XVFB_DISPLAY=""; rm -f "$STATE_FILE"; }
fi

stop() {
    env_stop
    if [[ -n "$XVFB_PID" ]]; then
        kill "$XVFB_PID" 2>/dev/null
        XVFB_PID="" XVFB_DISPLAY=""
        rm -f "$STATE_FILE"
    fi
}

case "${1:-}" in
start)
    shift
    envs=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    env_check
    [[ -f "$BUILD/loader/tome.mix" ]] || env_die "build first: cmake --build $BUILD"
    stop
    env_prefix
    env_game "$GAME" "$BUILD/loader/tome.mix"
    if [[ "$HEADLESS" == 1 ]]; then
        env_display_start
        echo "$XVFB_PID $XVFB_DISPLAY" >"$STATE_FILE"
        disown "$XVFB_PID" 2>/dev/null
    fi
    rm -f "$LOG"
    (cd "$GAME" && DISPLAY="$(env_display)" setsid env "${envs[@]}" wine war3.exe -window -opengl "$@" \
        >/dev/null 2>&1 &)
    ;;
stop)
    stop
    ;;
wait)
    pattern="${2:?usage: game.sh wait <regex> [seconds]}"
    for _ in $(seq 1 $(( ${3:-30} * 4 ))); do
        grep -qE "$pattern" "$LOG" 2>/dev/null && exit 0
        sleep 0.25
    done
    echo "timed out waiting for: $pattern" >&2
    exit 1
    ;;
shot)
    env_shot "${2:?usage: game.sh shot <file.png>}"
    ;;
log)
    cat "$LOG"
    ;;
dir)
    echo "$GAME"
    ;;
*)
    sed -n '2,23p' "$0"
    exit 2
    ;;
esac
