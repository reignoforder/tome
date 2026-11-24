#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Developer helper: screenshots of the game's UI, in a window of any shape,
# scripted with TOME_DEBUG_CLICKS (core/debug_script.h). For checking how a
# change (widescreen most of all) looks without clicking through the game.
#
#   tools/ui-shots.sh map                         build Maps/Showcase/showcase.w3m in the test game folder
#   tools/ui-shots.sh seq <name> <n> <script>     one launch, <n> screenshots
#   tools/ui-shots.sh timed <name> <n> <seconds> <script> [wait-regex]
#   tools/ui-shots.sh sheet <out.png> <columns> <png>...
#
# seq: the script marks each place to photograph with dump:<name>M<i> (i from
# 1; the step logs "Debug dump of <name>M<i> ...": no such frame, which is the
# marker), and <build>/shots/<name>-<i>.png is taken when it appears. A shot
# is of the whole screen, so a window smaller than the screen shows black round it.
# The script goes on at once after a marker: hold the screen with frames:<n>
# (30 is enough) before the next step changes it.
#   tools/ui-shots.sh seq menus 2 "wait:Screen ready: main menu;settle;dump:menusM1;\
#       frames:20;click:SinglePlayerButton;wait:Screen ready: single player;settle;dump:menusM2"
# timed: shots every <seconds> after the game logs <wait-regex> (default: the
# main menu is ready): for what a script can't wait for (a movie presents no
# frames, so no step runs during one).
# sheet: ImageMagick's montage of screenshots, scaled down, <columns> across.
#
# The showcase map (tools/showcase-map.py lists its chat commands: a hero,
# multiboard, leaderboard, timers, quests, a dialog, a cinematic with fades,
# the interface hidden, victory...) is the (2)BootyBay map with those added;
# start it with SHOWCASE=1 (or GAMEARGS='-loadfile Maps\Showcase\showcase.w3m')
# and drive it with type:-hero;wait:TomeLogInteger(9001) and the like.
#
# Environment: TEST_WIDTH / TEST_HEIGHT pick the window (default 1056 x 552:
# a 1280 x 720 screen, about 16:9; 800 x 600 is 4:3; 2296 x 912 is 21:9),
# GAMEARGS (arguments for war3.exe), EXTRA (VAR=value for the game, e.g.
# TOME_DEBUG_APPROVE=<mod>), WAIT (seconds to wait for a marker, default 90),
# SHOTDELAY (seconds between a marker and its shot, default 0.5), EDITION,
# GAME_VERSION and BUILD as in tools/game.sh. Needs a build with
# tools/game.sh's requirements (-DTOME_DEV=ON, the Makefile's default) and
# ImageMagick. Movies play (the test folder links Movies.off as Movies).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(realpath -m "${BUILD:-$ROOT/build}")"
export HEADLESS="${HEADLESS:-1}" TEST_WIDTH="${TEST_WIDTH:-1056}" TEST_HEIGHT="${TEST_HEIGHT:-552}"
SHOTS="$BUILD/shots"
GAME_SH="$ROOT/tools/game.sh"
[[ "${SHOWCASE:-0}" == 1 ]] && GAMEARGS="${GAMEARGS:-} -loadfile Maps\\Showcase\\showcase.w3m"

die() { echo "ui-shots: $*" >&2; exit 2; }

start() {  # start <script>
    local env_args=()
    [[ -n "${EXTRA:-}" ]] && read -r -a env_args <<<"$EXTRA"
    # shellcheck disable=SC2086
    "$GAME_SH" start "TOME_DEBUG_CLICKS=$1" "${env_args[@]}" -- ${GAMEARGS:-} >/dev/null 2>&1
}

cmd="${1:-}"
case "$cmd" in
map)
    source "$ROOT/tools/test-env.sh"
    env_check
    MPQ_TOOL="$BUILD/tools/mpq/tome-mpq.exe"
    [[ -f "$MPQ_TOOL" ]] || die "build with -DTOME_BUILD_DEV_TOOLS=ON (the Makefile's default) for $MPQ_TOOL"
    [[ -f "$BUILD/loader/tome.mix" ]] || die "build first: cmake --build $BUILD"
    SOURCE="$GAME_DIR/Maps/(2)BootyBay.w3m"
    [[ -f "$SOURCE" ]] || die "no $SOURCE (the game's files go in input/<version>/)"
    env_prefix
    GAME="$BUILD/test-env/game"
    env_game "$GAME" "$BUILD/loader/tome.mix"
    WORK="$(mktemp -d)"
    trap 'rm -rf "$WORK"' EXIT
    mkdir -p "$GAME/Maps/Showcase"
    cp "$SOURCE" "$GAME/Maps/Showcase/showcase.w3m"
    chmod u+w "$GAME/Maps/Showcase/showcase.w3m"
    mpq() { wine "$MPQ_TOOL" "$@" </dev/null >/dev/null 2>&1; }
    mpq extract "$GAME/Maps/Showcase/showcase.w3m" war3map.j "$WORK/war3map.j" || die "couldn't extract war3map.j"
    python3 -I "$ROOT/tools/showcase-map.py" "$WORK/war3map.j" "$WORK/war3map.show.j" || die "couldn't patch the script"
    mpq put "$GAME/Maps/Showcase/showcase.w3m" war3map.j "$WORK/war3map.show.j" || die "couldn't write the map"
    echo "ui-shots: $GAME/Maps/Showcase/showcase.w3m (SHOWCASE=1 starts it)"
    ;;
seq)
    name="${2:?usage: ui-shots.sh seq <name> <n> <script>}" n="${3:?}" script="${4:?}"
    mkdir -p "$SHOTS"
    start "$script"
    for i in $(seq 1 "$n"); do
        if "$GAME_SH" wait "Debug dump of ${name}M$i " "${WAIT:-90}"; then
            sleep "${SHOTDELAY:-0.5}"
            "$GAME_SH" shot "$SHOTS/$name-$i.png" || die "no screenshot (ImageMagick's import?)"
            echo "$SHOTS/$name-$i.png"
        else
            echo "ui-shots: timed out at marker $i; the end of the log:" >&2
            "$GAME_SH" log | tail -n 8 | cut -c1-150 >&2
            exit 1
        fi
    done
    ;;
timed)
    name="${2:?usage: ui-shots.sh timed <name> <n> <seconds> <script> [wait-regex]}" n="${3:?}" every="${4:?}" script="${5:?}"
    regex="${6:-Screen ready: main menu}"
    mkdir -p "$SHOTS"
    start "$script"
    "$GAME_SH" wait "$regex" "${WAIT:-90}" || die "timed out waiting for: $regex"
    for i in $(seq 1 "$n"); do
        "$GAME_SH" shot "$SHOTS/$name-$i.png" || die "no screenshot (ImageMagick's import?)"
        echo "$SHOTS/$name-$i.png"
        sleep "$every"
    done
    ;;
sheet)
    out="${2:?usage: ui-shots.sh sheet <out.png> <columns> <png>...}" cols="${3:?}"
    shift 3
    [[ $# -gt 0 ]] || die "no screenshots given"
    command -v montage >/dev/null || die "needs ImageMagick (montage)"
    montage "$@" -tile "${cols}x" -geometry 640x360+4+4 "$out" && echo "$out"
    ;;
*)
    sed -n '3,41p' "$0"
    exit 2
    ;;
esac
