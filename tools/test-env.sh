# Sourced by tools/game.sh and tools/e2e-test.sh: the place the game runs
# in for testing, kept apart from your own install and desktop.
#
# - Game folder: a folder of links to the game's files in input/ (which is
#   only ever read; input/README.md), with its own tome.mix, tome/, Maps/,
#   save/ and replay/. Nothing is copied into input/.
# - Wine prefix: its own (TEST_PREFIX, default <build>/test-env/wine), made
#   on first use with fixed settings (800x600, a profile called Tome, no
#   intro movie), so the game looks the same on every machine and
#   TOME_DEBUG_CLICKS positions land on the same buttons.
# - Display: with HEADLESS=1 the game runs in its own Xvfb server. Nothing
#   shows up on your screen, the game keeps the keyboard focus whatever you
#   do, and simulated clicks don't move your mouse. Rendering is Mesa's
#   software OpenGL there, so no GPU is needed (CI, containers).
#
# - Sound: SOUND=0 (the default with HEADLESS=1) gives the game a sound
#   device that plays nothing: Wine's ALSA driver (its PulseAudio one off)
#   on ALSA's null device. No sound server, nothing reaches your speakers,
#   and the game's sound code still runs. (Without any device the game
#   shows a dialog over the main menu that takes the clicks.) SOUND=1 uses
#   your sound as usual.
#
# Needs: wine, Xvfb (HEADLESS=1), ImageMagick's `import` (screenshots),
# Wine's ALSA driver (SOUND=0: wine-alsa).

# BUILD and ROOT are set by the script that sources this.
GAME_DIR="$ROOT/input"
TEST_PREFIX="$(realpath -m "${TEST_PREFIX:-$BUILD/test-env/wine}")"  # Wine wants it absolute
HEADLESS="${HEADLESS:-0}"
SOUND="${SOUND:-$([[ "$HEADLESS" == 1 ]] && echo 0 || echo 1)}"
export WINEPREFIX="$TEST_PREFIX" WINEDEBUG="${WINEDEBUG:--all}"
# The game's resolution in the test prefix; Xvfb's screen is a bit bigger,
# since there's no window manager to place the window.
TEST_WIDTH=800 TEST_HEIGHT=600

env_die() { echo "test-env: $*" >&2; exit 2; }

# env_check: the tools and the game are there.
env_check() {
    command -v wine >/dev/null || env_die "wine not found"
    command -v winepath >/dev/null || env_die "winepath not found"
    [[ "$HEADLESS" != 1 ]] || command -v Xvfb >/dev/null || env_die "HEADLESS=1 needs Xvfb"
    [[ -f "$GAME_DIR/war3.exe" ]] || env_die "no war3.exe in $GAME_DIR (put a Warcraft III 1.27b folder's files there: input/README.md)"
}

# What the prefix was made with: Wine copies its DLLs into a prefix when it
# makes it, so another Wine (or one with more of its packages) needs a new one.
# The 2 is the prefix's own layout (bump it to remake every prefix): since
# 2, a snapshot of its fresh user registry (env_registry_pristine).
env_wine_id() {
    { echo "tome prefix 2"; wine --version; find /usr/lib*/wine* /usr/local/lib*/wine* /opt/wine* -name '*.dll' 2>/dev/null | sort; } | cksum
}

# env_prefix: makes the Wine prefix if it isn't there yet or Wine changed (about 15 s).
env_prefix() {
    local id
    id="$(env_wine_id)"
    [[ "$(cat "$TEST_PREFIX/.tome-ready" 2>/dev/null)" == "$id" ]] && return 0
    echo "test-env: making a Wine prefix in $TEST_PREFIX ($(wine --version))" >&2
    rm -rf "$TEST_PREFIX"
    mkdir -p "$TEST_PREFIX"
    # No Mono or Gecko install prompts; the game needs neither.
    WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i >/dev/null 2>&1 || env_die "wineboot failed"
    local reg="$TEST_PREFIX/tome-settings.reg"
    printf '%s\r\n' 'Windows Registry Editor Version 5.00' '' \
        '[HKEY_CURRENT_USER\Software\Blizzard Entertainment\Warcraft III\String]' \
        '"userlocal"="Tome"' '' \
        '[HKEY_CURRENT_USER\Software\Blizzard Entertainment\Warcraft III\Video]' \
        "\"reswidth\"=dword:$(printf %08x $TEST_WIDTH)" "\"resheight\"=dword:$(printf %08x $TEST_HEIGHT)" \
        '"colordepth"=dword:00000020' '' \
        '[HKEY_CURRENT_USER\Software\Blizzard Entertainment\Warcraft III\Misc]' \
        '"seenintromovie"=dword:00000001' '' \
        '[HKEY_CURRENT_USER\Software\Blizzard Entertainment\Warcraft III\Sound]' \
        '"nosoundwarn"=dword:00000001' >"$reg"
    wine regedit /S "$(winepath -w "$reg")" >/dev/null 2>&1 || env_die "couldn't write the game's settings"
    wineserver -w
    cp "$TEST_PREFIX/user.reg" "$TEST_PREFIX/user.reg.pristine"
    echo "$id" >"$TEST_PREFIX/.tome-ready"
}

# env_link <target> <link>: (re)points a link there, also one left dangling
# or pointing into another game folder; never replaces a real file or folder.
env_link() {
    [[ -L "$2" || ! -e "$2" ]] && ln -sfn "$1" "$2"
}

# env_game <folder> <tome.mix>: makes (or refreshes) a game folder there.
# Links to everything in input/ except what the game or Tome writes.
env_game() {
    local dir="$1" mix="$2" entry name
    mkdir -p "$dir/Maps" "$dir/save" "$dir/replay"
    for entry in "$GAME_DIR"/*; do
        name="$(basename "$entry")"
        case "${name,,}" in
        # mods/: an old layout's, which Tome moves to tome/mods and writes in.
        # A link to one of these left by an older version of this goes.
        tome | tome.mix | mods | maps | save | replay | screenshots | errors | logs | glcache | bncache.dat)
            [[ -L "$dir/$name" ]] && rm "$dir/$name"
            continue ;;
        esac
        env_link "$entry" "$dir/$name"
    done
    for entry in "$GAME_DIR"/Maps/*; do
        [[ -e "$entry" ]] || continue
        name="$(basename "$entry")"
        env_link "$entry" "$dir/Maps/$name"
    done
    cp "$mix" "$dir/tome.mix"
}

# --- the display ------------------------------------------------------------------

XVFB_PID=""
XVFB_DISPLAY=""

# env_display_start: starts Xvfb on a free display (HEADLESS=1 only).
env_display_start() {
    [[ "$HEADLESS" == 1 && -z "$XVFB_PID" ]] || return 0
    local fifo
    fifo="$(mktemp -u)"
    mkfifo "$fifo"
    Xvfb -displayfd 3 -screen 0 "$((TEST_WIDTH + 224))x$((TEST_HEIGHT + 168))x24" -nolisten tcp \
        3>"$fifo" >/dev/null 2>&1 &
    XVFB_PID=$!
    read -r -t 10 XVFB_DISPLAY <"$fifo"
    rm -f "$fifo"
    [[ -n "$XVFB_DISPLAY" ]] || env_die "Xvfb didn't start"
    XVFB_DISPLAY=":$XVFB_DISPLAY"
}

env_display_stop() {
    [[ -n "$XVFB_PID" ]] || return 0
    kill "$XVFB_PID" 2>/dev/null
    wait "$XVFB_PID" 2>/dev/null
    XVFB_PID="" XVFB_DISPLAY=""
}

# --- sound ---------------------------------------------------------------------------

# env_sound: with SOUND=0, the game's sound goes to ALSA's null device (see
# the top); exported for the wine commands that follow. The ALSA
# configuration is only that device: with the system's (alsa.conf and its
# conf.d), Wine also lists PipeWire's or PulseAudio's devices and plays
# through one of them.
env_sound() {
    [[ "$SOUND" == 1 ]] && return 0
    local conf="$BUILD/test-env/silent-alsa.conf"
    mkdir -p "$(dirname "$conf")"
    echo 'pcm.!default { type null }' >"$conf"
    export ALSA_CONFIG_PATH="$conf"
    export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}winepulse.drv=d"
}

# The DISPLAY the game runs on.
env_display() { echo "${XVFB_DISPLAY:-${DISPLAY:-}}"; }

# --- the game ----------------------------------------------------------------------

# env_pids: war3.exe processes in the test prefix (never your own game's).
env_pids() {
    local pid
    for pid in $(pgrep -x war3.exe); do
        grep -qzxF "WINEPREFIX=$TEST_PREFIX" "/proc/$pid/environ" 2>/dev/null && echo "$pid"
    done
}

env_running() { [[ -n "$(env_pids)" ]]; }

# env_stop: ends every program in the test prefix.
env_stop() {
    local pids
    pids="$(env_pids)"
    [[ -n "$pids" ]] && kill $pids 2>/dev/null
    for _ in $(seq 1 10); do env_running || break; sleep 0.5; done
    wineserver -k 2>/dev/null
    wineserver -w 2>/dev/null
}

# --- registry ---------------------------------------------------------------------
#
# The game keeps what you last did in the registry (the map you picked, the
# profile, options), and tools/game.sh shares the prefix. The e2e test runs
# every launch from the same user registry instead: the prefix's fresh one
# plus its own settings. Wine is stopped (env_stop) around each copy.

# env_registry_save <file> / env_registry_load <file>: the user registry.
env_registry_save() { env_stop; cp "$TEST_PREFIX/user.reg" "$1"; }
env_registry_load() { env_stop; cp "$1" "$TEST_PREFIX/user.reg"; }

# env_registry_pristine <file> [reg add args...]...: writes to <file> the
# fresh registry with these values set (each a "key|name|type|data").
env_registry_pristine() {
    local out="$1" item key name type data
    shift
    [[ -f "$TEST_PREFIX/user.reg.pristine" ]] || env_die "the prefix has no registry snapshot; delete $TEST_PREFIX"
    env_registry_load "$TEST_PREFIX/user.reg.pristine"
    for item in "$@"; do
        IFS='|' read -r key name type data <<<"$item"
        wine reg add "$key" /v "$name" /t "$type" /d "$data" /f >/dev/null 2>&1 || env_die "couldn't set $key\\$name"
    done
    env_registry_save "$out"
}

# env_shot <file.png>: a screenshot of the game's display.
env_shot() {
    command -v import >/dev/null || { echo "test-env: screenshots need ImageMagick (import)" >&2; return 1; }
    DISPLAY="$(env_display)" import -window root "$1"
}
