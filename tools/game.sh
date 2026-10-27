#!/usr/bin/env bash
# Developer helper for running the game under Wine while working on Tome.
#
#   tools/game.sh start [VAR=value ...] [-- game args]   restart the game (stops any running copy)
#   tools/game.sh stop                                   stop the game
#   tools/game.sh wait <regex> [seconds]                 wait for a tome.log line (default 30 s)
#   tools/game.sh shot <file.png>                        screenshot (the headless game's screen, or
#                                                        the full screen with KDE Spectacle)
#   tools/game.sh log                                    print tome.log
#
# Examples:
#   tools/game.sh start TOME_DEBUG_OPEN_MOD_MENU=1
#   tools/game.sh start "TOME_DEBUG_CLICKS=0.874,0.84@4000"
#   tools/game.sh start -- -loadfile 'Maps\Test\test.w3m'
#
# The game is found by its exact process name (war3.exe), never by command
# line: `pkill -f` patterns also match the shell that runs them.
# Set GAME_DIR to use a game folder other than ./WarCraft III, and TOME_HOME
# (a Windows path) to keep mods/ and tome/ somewhere else. HEADLESS=1 runs
# the game off screen, in gamescope (tools/headless.sh).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tools/headless.sh"
GAME="${GAME_DIR:-$ROOT/WarCraft III}"
LOG_DIR="$GAME"
if [[ -n "${TOME_HOME:-}" ]]; then
    LOG_DIR="$(winepath -u "$TOME_HOME" 2>/dev/null || echo "$GAME")"
fi
LOG="$LOG_DIR/tome/tome.log"

stop() {
    pkill -x war3.exe 2>/dev/null
    for _ in $(seq 1 10); do pgrep -x war3.exe >/dev/null || return 0; sleep 0.5; done
    pkill -9 -x war3.exe 2>/dev/null
    sleep 1
    if pgrep -x war3.exe >/dev/null; then echo "warning: game still running" >&2; fi
}

case "${1:-}" in
start)
    shift
    envs=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    stop
    rm -f "$LOG"
    (cd "$GAME" && headless_exec env WINEDEBUG=-all "${envs[@]}" wine war3.exe -window -opengl "$@" >/dev/null 2>&1 &)
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
    file="${2:?usage: game.sh shot <file.png>}"
    if [[ -n "$(headless_display)" ]]; then
        headless_shot "$file"
    else
        timeout 20 spectacle -b -n -f -o "$file"
    fi
    ;;
log)
    cat "$LOG"
    ;;
*)
    sed -n '2,20p' "$0"
    exit 2
    ;;
esac
