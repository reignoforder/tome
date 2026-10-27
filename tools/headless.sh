# Sourced by tools/game.sh and tools/e2e-test.sh: with HEADLESS=1 the game
# runs inside gamescope's headless backend, so no window shows up on your
# screen. Gamescope gives the game its own X server, so it also keeps the
# keyboard focus there (key presses from TOME_DEBUG_CLICKS reach it even
# while you use the desktop). Screenshots go through gamescopectl.
#
#   HEADLESS=1 tools/game.sh start ...
#   HEADLESS=1 tools/e2e-test.sh
#
# HEADLESS_SIZE=WxH sets the virtual screen. By default it's the game's own
# resolution (the Video settings in the Wine registry), so the window fills
# it and TOME_DEBUG_CLICKS fractions land where they would on your screen.

HEADLESS="${HEADLESS:-0}"

# The resolution in the game's settings, as WxH, or nothing.
game_resolution() {
    local reg="${WINEPREFIX:-$HOME/.wine}/user.reg" width height
    [[ -f "$reg" ]] || return
    read -r width height < <(awk '
        /^\[/ { video = tolower($0) ~ /^\[software\\\\blizzard entertainment\\\\warcraft iii\\\\video\]/ }
        video && /^"reswidth"=dword:/  { w = strtonum("0x" substr($0, index($0, ":") + 1)) }
        video && /^"resheight"=dword:/ { h = strtonum("0x" substr($0, index($0, ":") + 1)) }
        END { if (w && h) print w, h }' "$reg")
    [[ -n "$width" ]] && echo "${width}x${height}"
}
HEADLESS_SIZE="${HEADLESS_SIZE:-$(game_resolution)}"
HEADLESS_SIZE="${HEADLESS_SIZE:-800x600}"
# Gamescope's own output, which names the Wayland display that screenshots need.
HEADLESS_LOG="${XDG_RUNTIME_DIR:-/tmp}/tome-gamescope.log"

# The command to put in front of `wine war3.exe ...` (nothing unless HEADLESS=1).
HEADLESS_PREFIX=()
if [[ "$HEADLESS" == 1 ]]; then
    command -v gamescope >/dev/null || { echo "HEADLESS=1 needs gamescope" >&2; exit 2; }
    width="${HEADLESS_SIZE%x*}" height="${HEADLESS_SIZE#*x}"
    HEADLESS_PREFIX=(gamescope --backend headless -W "$width" -H "$height" -w "$width" -h "$height" --)
    unset width height
fi

# Runs "$@" with the prefix, gamescope's output going to HEADLESS_LOG, and
# exits with its status. Gamescope sometimes fails to set up Vulkan right
# after the previous instance exited ("Failed to create backend"); that's
# retried. A TERM or INT is passed on to gamescope, which then ends the game.
headless_exec() {
    [[ "$HEADLESS" == 1 ]] || exec "$@"
    local attempt pid status
    for attempt in 1 2 3 4 5; do
        "${HEADLESS_PREFIX[@]}" "$@" >"$HEADLESS_LOG" 2>&1 &
        pid=$!
        trap 'kill -TERM "$pid" 2>/dev/null' TERM INT
        wait "$pid"
        status=$?
        # A trapped signal ends `wait` early; wait for gamescope itself.
        while kill -0 "$pid" 2>/dev/null; do wait "$pid"; status=$?; done
        trap - TERM INT
        grep -q "Failed to create backend" "$HEADLESS_LOG" || exit "$status"
        echo "headless: gamescope couldn't start (attempt $attempt), retrying" >&2
        sleep 2
    done
    exit "$status"
}

# The Wayland display of the running headless game, or nothing.
headless_display() {
    local display
    display="$(grep -oE "Running compositor on wayland display '[^']+'" "$HEADLESS_LOG" 2>/dev/null |
               tail -1 | sed -E "s/.*'([^']+)'/\1/")"
    [[ -n "$display" && -S "${XDG_RUNTIME_DIR:-/tmp}/$display" ]] && echo "$display"
}

# Screenshot of the headless game: headless_shot <file.png>
headless_shot() {
    local display
    display="$(headless_display)"
    [[ -n "$display" ]] || { echo "no headless game running (see $HEADLESS_LOG)" >&2; return 1; }
    local out
    out="$(realpath -m "$1")"
    rm -f "$out"
    GAMESCOPE_WAYLAND_DISPLAY="$display" gamescopectl screenshot "$out" >/dev/null 2>&1
    # Gamescope writes it from its own thread.
    for _ in $(seq 1 20); do [[ -s "$out" ]] && return 0; sleep 0.25; done
    echo "screenshot failed (see $HEADLESS_LOG)" >&2
    return 1
}
