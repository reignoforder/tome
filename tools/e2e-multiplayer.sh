#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Multiplayer end-to-end test: two instances of the game, a host and a
# client, play a LAN game with the sync-test mod (required by every player),
# and tome.log of each is checked for the handshake (docs/architecture.md,
# "Multiplayer handshake"):
#
#   lobby   the client joins with another version of sync-test: both sides
#           say so, and the host's Start is refused; the client's mod is
#           then put right (a live reload), both say they match, and Start
#           goes through;
#   game    each side sends its set and finds every player's the same; the
#           game locks the required mods: the host turns sync-test off, it
#           keeps running until the host leaves the game, and then stops;
#   replay  the host watches the game's replay without sync-test: Tome says
#           it was recorded with other required mods.
#
# Usage: tools/e2e-multiplayer.sh [build-dir]   (make e2e runs it after
# tools/e2e-test.sh; make e2e-multiplayer runs it alone)
#
# How (docs/architecture.md, "Testing with two instances"): two copies of
# war3.exe in one Wine prefix shut each other down, and both want LAN's
# port 6112. So the client has a Wine prefix of its own (<build>/test-env/
# wine-client), and each instance has its own network namespace, joined by a
# veth pair, all inside one unprivileged user namespace (unshare -rn): no
# root, no change to your network. The script runs itself again inside it,
# since only the process holding the namespaces can start programs in them.
# Every Wine command of the client runs in the client's namespace: Wine
# creates sockets in its wineserver, so a wineserver started elsewhere (even
# by winepath) would put the client's sockets next to the host's.
#
# Off screen only, each instance in an Xvfb of its own. Those start outside
# the namespaces: inside, Xvfb can't see your display's socket, takes :0
# and deletes /tmp/.X11-unix/X0 when it ends. Nothing in it is timed: the
# input waits for log lines and frames, like tools/e2e-test.sh. Needs
# unprivileged user namespaces, ip (iproute2) and nsenter; without them it
# says it's skipped.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(realpath "${1:-${BUILD:-$ROOT/build}}")"
HEADLESS=1
source "$ROOT/tools/test-env.sh"
source "$ROOT/tools/e2e-lib.sh"
FIXTURES="$ROOT/tests/e2e/mods"
HOST_PREFIX="$TEST_PREFIX"
CLIENT_PREFIX="$(realpath -m "$BUILD/test-env/wine-client")"
WATCHDOG="${E2E_WATCHDOG:-120}"

die() { echo "e2e-multiplayer: $*" >&2; exit 2; }

# --- outside the namespaces: the prefixes, registries and displays -----------------

if [[ -z "${TOME_MP_INSIDE:-}" ]]; then
    [[ $# -le 1 ]] || die "usage: tools/e2e-multiplayer.sh [build-dir]"
    env_check
    [[ -f "$BUILD/loader/tome.mix" ]] || die "build first: cmake --build $BUILD"
    grep -qiE '^TOME_DEV:BOOL=(ON|1|TRUE|YES|Y)$' "$BUILD/CMakeCache.txt" 2>/dev/null \
        || die "$BUILD isn't a developer build (the test needs TOME_DEBUG_*): cmake -S . -B $BUILD -DTOME_DEV=ON"
    if ! command -v ip >/dev/null || ! command -v nsenter >/dev/null || ! unshare -rn true 2>/dev/null; then
        echo "e2e-multiplayer: skipped (needs ip, nsenter and unprivileged user namespaces: unshare -rn)"
        exit 0
    fi
    env_running && die "the test's Warcraft III is still running (tools/game.sh stop)"
    (TEST_PREFIX="$CLIENT_PREFIX"; env_running) && die "a client game of an earlier run is still running in $CLIENT_PREFIX"
    env_prefix
    (TEST_PREFIX="$CLIENT_PREFIX" WINEPREFIX="$CLIENT_PREFIX"; env_prefix)

    WORK="$(mktemp -d)"
    XVFB_PIDS=()
    finish() {
        ((${#XVFB_PIDS[@]})) && kill "${XVFB_PIDS[@]}" 2>/dev/null && wait "${XVFB_PIDS[@]}" 2>/dev/null
        [[ -f "$WORK/user.reg.yours" ]] && env_registry_load "$WORK/user.reg.yours"
        if [[ "${E2E_KEEP:-0}" == 1 ]]; then echo "e2e-multiplayer: kept test files in $WORK"; else rm -rf "$WORK"; fi
    }
    trap finish EXIT
    INNER=""
    # A signal goes on to the test inside (it runs in the background so this
    # one can take it at once): it stops the games, then this cleans up.
    # Ctrl+C reaches it as TERM, since bash ignores INT in what it runs in
    # the background.
    trap '[[ -n "$INNER" ]] && kill -TERM "$INNER" 2>/dev/null || exit 1' INT TERM HUP
    # The host's prefix is tools/game.sh's too: yours is put back at the end.
    # Each launch starts from the prefix's fresh registry (e2e-test.sh).
    env_registry_save "$WORK/user.reg.yours"
    env_registry_pristine "$WORK/host.reg"
    (TEST_PREFIX="$CLIENT_PREFIX" WINEPREFIX="$CLIENT_PREFIX"; env_registry_pristine "$WORK/client.reg")

    env_xvfb; XVFB_PIDS+=("$NEW_XVFB_PID"); HOST_DISPLAY="$NEW_XVFB_DISPLAY"
    env_xvfb; XVFB_PIDS+=("$NEW_XVFB_PID"); CLIENT_DISPLAY="$NEW_XVFB_DISPLAY"
    env_sound
    TOME_MP_INSIDE=1 WORK="$WORK" HOST_DISPLAY="$HOST_DISPLAY" CLIENT_DISPLAY="$CLIENT_DISPLAY" \
        unshare -rn bash "$0" "$BUILD" &
    INNER=$!
    # wait returns early when a trap runs; the test is done once it's gone.
    while :; do
        wait "$INNER"; status=$?
        kill -0 "$INNER" 2>/dev/null || break
    done
    exit "$status"
fi

# --- inside: the network ----------------------------------------------------------
#
# This namespace is the host's; the client's is a child's (sleep), reached
# with nsenter. 10.77.0.1 and .2, each the other's default route, so LAN
# broadcasts cross.
CLIENT_NS="" IN_CLIENT=()
stop_games() {
    WINEPREFIX="$HOST_PREFIX" wineserver -k 2>/dev/null
    WINEPREFIX="$CLIENT_PREFIX" "${IN_CLIENT[@]}" wineserver -k 2>/dev/null
    WINEPREFIX="$HOST_PREFIX" wineserver -w 2>/dev/null
    WINEPREFIX="$CLIENT_PREFIX" "${IN_CLIENT[@]}" wineserver -w 2>/dev/null
}
# Before the client's namespace exists, so a failed setup doesn't leave its
# sleep running (holding the namespace and this script's output).
cleanup() {
    [[ -n "$CLIENT_NS" ]] || return 0
    stop_games
    kill "$CLIENT_NS" 2>/dev/null
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP

ip link add veth-host type veth peer name veth-client || die "couldn't make the veth pair"
unshare -n sleep infinity &
CLIENT_NS=$!
IN_CLIENT=(nsenter -t "$CLIENT_NS" -n)
for _ in $(seq 1 50); do [[ "$(readlink /proc/$CLIENT_NS/ns/net)" != "$(readlink /proc/self/ns/net)" ]] && break; sleep 0.1; done
ip link set veth-client netns "$CLIENT_NS" || die "couldn't move the veth's end into the client's namespace"
ip addr add 10.77.0.1/24 dev veth-host
ip link set veth-host up
ip link set lo up
ip route add default via 10.77.0.2
"${IN_CLIENT[@]}" sh -c 'ip addr add 10.77.0.2/24 dev veth-client && ip link set veth-client up &&
    ip link set lo up && ip route add default via 10.77.0.1' || die "couldn't set up the client's network"

# --- the games -----------------------------------------------------------------------

# Each instance has its own game folder (links to input/), TOME_HOME and
# sync-test. The client's starts at another version.
for who in host client; do
    env_game "$WORK/$who/game" "$BUILD/loader/tome.mix"
    mkdir -p "$WORK/$who/home/tome/mods"
    cp -r "$FIXTURES/sync-test" "$WORK/$who/home/tome/mods/"
done
HOST_LOG="$WORK/host/home/tome/tome.log"
CLIENT_LOG="$WORK/client/home/tome/tome.log"
CLIENT_META="$WORK/client/home/tome/mods/sync-test/tome.mod.json"
sed -i 's/"version": "1.0.0"/"version": "1.0.1"/' "$CLIENT_META"

LAUNCHES=0
# launch <host|client> <TOME_DEBUG_CLICKS> [arguments]
launch() {
    local who="$1" clicks="$2" prefix display home
    shift 2
    local in_ns=()
    if [[ "$who" == host ]]; then
        prefix="$HOST_PREFIX" display="$HOST_DISPLAY"
    else
        prefix="$CLIENT_PREFIX" display="$CLIENT_DISPLAY" in_ns=("${IN_CLIENT[@]}")
    fi
    [[ "$clicks" =~ @[0-9]+(;|$) ]] && die "a TOME_DEBUG_CLICKS step has a fixed time (@ms); wait for what it needs instead"
    cp "$WORK/$who.reg" "$prefix/user.reg"
    # Each launch starts a new tome.log; the last one's is kept (E2E_KEEP=1).
    LAUNCHES=$((LAUNCHES + 1))
    [[ -f "$WORK/$who/home/tome/tome.log" ]] && mv "$WORK/$who/home/tome/tome.log" "$WORK/tome.$who.$LAUNCHES.log"
    home="$(WINEPREFIX="$prefix" "${in_ns[@]}" winepath -w "$WORK/$who/home" 2>/dev/null)"
    [[ -n "$home" ]] || die "winepath couldn't convert $WORK/$who/home"
    (cd "$WORK/$who/game" && WINEPREFIX="$prefix" DISPLAY="$display" TOME_HOME="$home" \
        TOME_DEBUG_CLICKS="$clicks" TOME_DEBUG_APPROVE=sync-test WINEDEBUG=err+module \
        exec "${in_ns[@]}" wine war3.exe -window -opengl "$@" >"$WORK/$who/wine.log" 2>&1) &
}

# mark <log>: its lines so far, for a check of what comes after them.
mark() { wc -l <"$1" 2>/dev/null || echo 0; }
# check <log> <regex> <description> [n]: waits for a matching line (after line n).
check() {
    local deadline=$((SECONDS + WATCHDOG))
    while ((SECONDS < deadline)); do
        e2e_log "$1" "${4:-0}" | e2e_has "$2" && { pass "$3"; return 0; }
        sleep 0.2
    done
    fail "$3 (expected a line matching: $2)"
    return 1
}
check_absent() {
    if e2e_log "$1" | e2e_has "$2"; then fail "$3 (unexpected line matching: $2)"; else pass "$3"; fi
}
# check_order <log> <regex> <regex> <description>: both, the first one first.
check_order() {
    if e2e_log "$1" | awk -v a="$2" -v b="$3" '$0 ~ a && !x { x = NR } $0 ~ b && x { y = NR } END { exit !(x && y) }'; then
        pass "$4"
    else
        fail "$4 (expected a line matching: $2, then one matching: $3)"
    fi
}
# players_read <log>: how many players' sync-test sets it read (from the
# game's actions; each side logs every player's, its own too).
players_read() {
    e2e_log "$1" | sed -nE "s/.*Handshake: player ([0-9]+)'s required mods: sync-test 1\.0\.0.*/\1/p" | sort -u | wc -l
}
# check_players <log> <description>: waits for both players' sets.
check_players() {
    local deadline=$((SECONDS + WATCHDOG))
    while ((SECONDS < deadline)); do
        (($(players_read "$1") >= 2)) && { pass "$2"; return 0; }
        sleep 0.2
    done
    fail "$2 (expected both players' sets, got $(players_read "$1"))"
    return 1
}

# --- launch 1: the lobby and the game ----------------------------------------------------

echo "lobby and game (two instances)"
# Host: Local Area Network, Create Game, Create Game, then Start Game once
# the client's set has come (refused), and again when it matches. In the
# game, after the report and once sync-test's toggle waits: the menu (F10),
# End Game, Quit Mission, and OK on the score screen.
launch host "wait:Screen ready: main menu;click:LocalAreaNetworkButton;wait:Screen ready: LAN;\
click:CreateButton;wait:Screen ready: create LAN game;click:PlayButton;wait:Screen ready: lobby;\
wait:My mods 1/1: sync-test 1.0.1;click:StartGameButton;wait:not starting, the players' required mods differ;\
wait:My mods 1/1: sync-test 1.0.0;click:StartGameButton;wait:Game started: ;\
wait:Handshake: every player;wait:turns off when it ends;settle;\
key:F10;settle;click:EndGameButton;settle;click:QuitButton;wait:Screen: score screen;settle;click:ScoreScreenOkButton"
check "$HOST_LOG" "Handshake: lobby opened" "host: the lobby is open" || exit 1
check "$HOST_LOG" "Screen ready: lobby" "host: the lobby takes input" || exit 1
# Client, once the host's game is there to see: Local Area Network, the
# name Guest, the first game in the list (it has no name to click by; it
# shows up within a few frames of the screen), Join Game. In the game it
# waits for the report and stays until the end.
launch client "wait:Screen ready: main menu;click:LocalAreaNetworkButton;wait:Screen ready: LAN;\
focus:PlayerNameEditBox;text:Guest;frames:120;0.26,0.335;settle;click:JoinButton;wait:Screen ready: lobby"

# Every check below needs the client in the lobby: without it they would
# each wait out the watchdog.
check "$CLIENT_LOG" "Handshake: lobby: .*, joined, players: 128 Tome" "client: joined the host's lobby" || exit 1
check "$HOST_LOG" "Handshake: lobby: .*, hosting, players: 129 Guest" "host: sees the client"
check "$HOST_LOG" "Handshake: to lobby player 129: \[Tome\] Required mods 1/1: sync-test 1\.0\.0 #" \
    "host: sends the client its required mods"
check "$CLIENT_LOG" "Handshake: from lobby player 128: \[Tome\] Required mods 1/1: sync-test 1\.0\.0" \
    "client: gets the host's"
check "$CLIENT_LOG" "Handshake: Your required mods differ from the host's: .*sync-test" \
    "client: says its version differs"
check "$HOST_LOG" "Handshake: from lobby player 129: \[Tome\] My mods 1/1: sync-test 1\.0\.1" "host: gets the client's"
check "$HOST_LOG" "Handshake: Guest: .*sync-test.*The game can't start until they match" "host: says the client's differ"
check "$HOST_LOG" "Handshake: not starting, the players' required mods differ" "host: Start is refused"
check "$HOST_LOG" "Handshake:   Guest: " "host: the refusal names the player"
check_absent "$HOST_LOG" "Game started: " "host: no game started"
# The client's sync-test made right: the same file as the host's, so the
# same hash; a live reload sends the host the new set. Through a rename, so
# the watcher never reads half a file.
cp "$FIXTURES/sync-test/tome.mod.json" "$WORK/tome.mod.json.new" && mv "$WORK/tome.mod.json.new" "$CLIENT_META"
check "$HOST_LOG" "Handshake: from lobby player 129: \[Tome\] My mods 1/1: sync-test 1\.0\.0" \
    "host: gets the client's new set (a live reload in the lobby)"
check "$HOST_LOG" "Handshake: Guest has the same required mods" "host: says they match now"
check "$CLIENT_LOG" "Handshake: Your required mods match the host's \(sync-test 1\.0\.0" "client: says so too"
check "$HOST_LOG" "Handshake: starting; every player's required mods match" "host: Start goes through"
for who in host client; do
    log="$HOST_LOG"; [[ "$who" == client ]] && log="$CLIENT_LOG"
    check "$log" "Game started: " "$who: the game started"
    check "$log" "Multiplayer game: required mods are locked until it ends \(sync-test\)" "$who: required mods locked"
    check "$log" "Handshake: sent this game 1 required mod\(s\): sync-test 1\.0\.0" "$who: sent its set as game actions"
    check_players "$log" "$who: got the other player's too"
    check "$log" "Handshake: every player has the same required mods \(sync-test 1\.0\.0" "$who: every player matches"
done
# The host is in the game: what it logs from here on is about leaving it
# (its "Screen: LAN" from before the game mustn't count).
IN_GAME=$(mark "$HOST_LOG")
e2e_set_enabled "$WORK/host/home/tome/state.json" sync-test false
check "$HOST_LOG" "sync-test: locked during this multiplayer game; turns off when it ends" \
    "host: turning sync-test off waits for the game to end"
check "$HOST_LOG" "Multiplayer game ended: required mods are unlocked" "host: leaving the game unlocks them"
check "$HOST_LOG" "Disabled sync-test" "host: and sync-test stops then"
check_order "$HOST_LOG" "Multiplayer game ended" "Disabled sync-test" "host: not before the game ended"
check "$HOST_LOG" "Screen: LAN" "host: back on the LAN screen" "$IN_GAME"
stop_games
REPLAY="$WORK/host/game/replay/LastReplay.w3g"
if [[ -f "$REPLAY" ]]; then pass "host: the game's replay was saved"; else fail "host: the game's replay was saved"; fi

# --- launch 2: the replay ------------------------------------------------------------------

echo "replay (the host's, without sync-test)"
launch host "wait:Game started: " -loadfile 'replay\LastReplay.w3g'
check "$HOST_LOG" "Game started: " "the replay plays"
check "$HOST_LOG" "Multiplayer game: required mods are locked until it ends \(none running\)" "a replay locks required mods too"
check_players "$HOST_LOG" "both recorded players' sets are read back"
check "$HOST_LOG" "Handshake: This replay was recorded with other required mods than yours" \
    "Tome says the replay had other required mods"
stop_games

echo
if ((FAILED)); then
    echo "e2e-multiplayer: FAILED (logs: E2E_KEEP=1 keeps them)"
    exit 1
fi
echo "e2e-multiplayer: all checks passed"
