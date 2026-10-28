#!/usr/bin/env bash
# End-to-end test: runs the real game under Wine with Tome and a set of test
# mods, changes things while it runs, and checks tome.log for the expected
# behavior.
#
# Nothing is written to your Warcraft III install or your Wine prefix: the
# game runs from a temporary folder of links to the install's files, in a
# Wine prefix of the test's own, with TOME_HOME in a temporary folder
# (tools/test-env.sh). If tome-mpq was built (-DTOME_BUILD_DEV_TOOLS=ON), a
# test map made from one of the install's melee maps calls the JASS natives
# of the hello mod, its C++ version (hello-cpp) and its Rust version
# (hello-rs) if that was built (sdk/rust: cargo and its i686-pc-windows-gnu
# target).
#
# Usage: tools/e2e-test.sh [build-dir] [game-dir]
# The game is launched six times. By default it runs in its own X server
# (Xvfb), off screen: it keeps the keyboard focus, and its clicks don't move
# your mouse, so you can go on working. HEADLESS=0 shows it on your screen
# instead (then leave it the focus: a single player game pauses without it).
# The simulated input waits for log lines (TOME_DEBUG_CLICKS wait: steps),
# not for fixed times, so a slow machine only makes the test slower.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(realpath "${1:-${BUILD:-$ROOT/build}}")"
[[ -n "${2:-}" ]] && GAME_DIR="$(realpath "$2")"
HEADLESS="${HEADLESS:-1}"
source "$ROOT/tools/test-env.sh"
FIXTURES="$ROOT/tests/e2e/mods"
MPQ_TOOL="$BUILD/tools/mpq/tome-mpq.exe"

die() { echo "e2e: $*" >&2; exit 2; }
env_check
[[ -f "$BUILD/loader/tome.mix" ]] || die "build first: cmake --build $BUILD"
[[ -f "$BUILD/mods/hello/hello.dll" ]] || die "hello example mod not built"
[[ -f "$BUILD/mods/hello-cpp/hello-cpp.dll" ]] || die "hello-cpp example mod not built"
[[ -f "$BUILD/mods/hook-a/hook-a.dll" && -f "$BUILD/mods/hook-b/hook-b.dll" ]] || die "hook test mods not built"
[[ -f "$BUILD/mods/crash/crash.dll" ]] || die "crash test mod not built"
env_running && die "the test's Warcraft III is still running (tools/game.sh stop)"
# Another copy of the game (yours, or tools/game.sh) would take the LAN port.
pgrep -x war3.exe >/dev/null && echo "e2e: another Warcraft III is running; the LAN game (fifth launch) may fail"
env_prefix

WORK="$(mktemp -d)"          # scratch files; deleted at the end
GAME="$WORK/game"            # the game folder: links to GAME_DIR's files
TEST_MAP_DIR="$GAME/Maps/TomeE2E"
HOME_DIR="$WORK/home"         # TOME_HOME: tome/ (with tome/mods/) for the test
MODS="$HOME_DIR/tome/mods"
STATE="$HOME_DIR/tome/state.json"
LOG="$HOME_DIR/tome/tome.log"
mkdir -p "$MODS"
TOME_HOME_WIN="$(winepath -w "$HOME_DIR" 2>/dev/null)"
[[ -n "$TOME_HOME_WIN" ]] || die "winepath couldn't convert $HOME_DIR"
WINE_PID=""

stop_game() {
    [[ -z "$WINE_PID" ]] && return
    env_stop
    wait "$WINE_PID" 2>/dev/null
    WINE_PID=""
    # Each launch starts a new tome.log; E2E_KEEP=1 keeps them all.
    LAUNCHES=$((LAUNCHES + 1))
    [[ "${E2E_KEEP:-0}" == 1 && -f "$LOG" ]] && cp "$LOG" "$WORK/tome.$LAUNCHES.log"
}
LAUNCHES=0

cleanup() {
    stop_game
    env_display_stop
    if [[ "${E2E_KEEP:-0}" == 1 ]]; then
        echo "e2e: kept test files in $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP

# --- setup -------------------------------------------------------------------

env_game "$GAME" "$BUILD/loader/tome.mix"
env_display_start
cp -r "$BUILD/mods/hello" "$BUILD/mods/hello-cpp" "$MODS/"
RUST=0
if [[ -f "$BUILD/mods/hello-rs/hello_rs.dll" ]]; then
    RUST=1
    cp -r "$BUILD/mods/hello-rs" "$MODS/hello-rs"
else
    echo "Rust example: skipped (not built; see sdk/rust/README.md)"
fi
cp -r "$BUILD/mods/hook-a" "$BUILD/mods/hook-b" "$BUILD/mods/crash" "$MODS/"
echo native > "$MODS/crash/mode.txt"  # where the crash fixture crashes first; see crash.c
cp -r "$FIXTURES/broken-json" "$FIXTURES/needs-ghost" "$FIXTURES/needs-unmapped" "$MODS/"
# A mod with code nobody allowed (not in APPROVED): it must not even be
# loaded, so its "DLL" is only text.
cp -r "$FIXTURES/unapproved" "$FIXTURES/unapproved-script" "$MODS/"
echo "not a DLL" > "$MODS/unapproved/unapproved.dll"
cp -r "$FIXTURES/asset-demo" "$FIXTURES/asset-map" "$FIXTURES/asset-restart" "$MODS/"
ASSET_FILES=5  # asset-demo's TOC, FDF and texture, one file each in asset-map and asset-restart
# The texture asset-demo overrides: the main menu buttons' background.
PROBE='UI\Widgets\Glues\GlueScreen-Button1-BackdropBackground.blp'
TEXTURE="$MODS/asset-demo/assets/UI/Widgets/Glues/GlueScreen-Button1-BackdropBackground.blp"
mkdir -p "$(dirname "$TEXTURE")"
python3 -I "$ROOT/sdk/support/make_blp.py" "$TEXTURE" 32 20c0a0 || die "couldn't generate the test texture"
PROBE_HASH=$(python3 -I -c '
import sys
h = 2166136261
for b in open(sys.argv[1], "rb").read():
    h = ((h ^ b) * 16777619) & 0xFFFFFFFF
print("%08x" % h)' "$TEXTURE")
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
    mkdir -p "$MODS/wrong-arch"
    cp "$FIXTURES/wrong-arch/tome.mod.json" "$MODS/wrong-arch/"
    x86_64-w64-mingw32-gcc -shared -o "$MODS/wrong-arch/wrong-arch.dll" "$FIXTURES/wrong-arch/wrong-arch.c"
fi

MAP_ARG=()
if [[ -f "$MPQ_TOOL" ]]; then
    SOURCE_MAP="$(find "$GAME/Maps" -maxdepth 1 -name '*.w3m' | sort | head -n 1)"
    [[ -n "$SOURCE_MAP" ]] || die "no melee map found in $GAME/Maps"
    # A Maps/TomeE2E in the install (left by an older version of this test)
    # is linked here: replace the link, or the map would be written there.
    [[ -L "$TEST_MAP_DIR" ]] && rm "$TEST_MAP_DIR"
    mkdir -p "$TEST_MAP_DIR"
    cp "$SOURCE_MAP" "$TEST_MAP_DIR/e2e.w3m"
    mpq() { wine "$MPQ_TOOL" "$@" >/dev/null 2>&1; }
    mpq extract "$TEST_MAP_DIR/e2e.w3m" war3map.j "$WORK/war3map.j" || die "couldn't extract war3map.j"
    EXAMPLE_ARGS=(--cpp)
    ((RUST)) && EXAMPLE_ARGS+=(--rust)
    python3 -I "$ROOT/tests/e2e/patch_map_script.py" "$WORK/war3map.j" "$WORK/war3map.patched.j" "${EXAMPLE_ARGS[@]}" ||
        die "patch failed"
    mpq put "$TEST_MAP_DIR/e2e.w3m" war3map.j "$WORK/war3map.patched.j" || die "couldn't write the test map"
    # asset-demo also overrides a game script: Blizzard.j, with a call the map logs.
    for archive in War3Patch War3x war3; do
        mpq extract "$GAME/$archive.mpq" 'Scripts\Blizzard.j' "$WORK/Blizzard.j" && break
    done
    [[ -f "$WORK/Blizzard.j" ]] || die "couldn't extract Blizzard.j"
    mkdir -p "$MODS/asset-demo/assets/Scripts"
    python3 -I "$ROOT/tests/e2e/patch_blizzard_j.py" "$WORK/Blizzard.j" "$MODS/asset-demo/assets/Scripts/Blizzard.j" ||
        die "Blizzard.j patch failed"
    ASSET_FILES=$((ASSET_FILES + 1))
    MAP_ARG=(-loadfile 'Maps\TomeE2E\e2e.w3m')
    echo "test map: built from $(basename "$SOURCE_MAP")"
else
    echo "test map: skipped (configure with -DTOME_BUILD_DEV_TOOLS=ON to build tome-mpq)"
fi

# --- helpers -----------------------------------------------------------------

# The test's mods with code (a DLL, or asset-demo's Blizzard.j), allowed
# without a click (core/mod_state.h).
APPROVED="hello,hello-cpp,hello-rs,hook-a,hook-b,crash,wrong-arch,sync-test,asset-demo"

launch() {
    rm -f "$LOG"
    (cd "$GAME" && DISPLAY="$(env_display)" TOME_HOME="$TOME_HOME_WIN" TOME_DEBUG_CLICKS="${TOME_DEBUG_CLICKS:-}" \
        TOME_DEBUG_ASSET="$PROBE" TOME_DEBUG_APPROVE="$APPROVED" \
        WINEDEBUG=err+module exec wine war3.exe -window -opengl "${MAP_ARG[@]}" >"$WORK/wine.log" 2>&1) &
    WINE_PID=$!
    # Without Tome's log there's nothing to check: say why instead of
    # timing out every check.
    local deadline=$((SECONDS + 120))
    until grep -q "Tome .* starting" "$LOG" 2>/dev/null; do
        if ((SECONDS >= deadline)) || ! kill -0 "$WINE_PID" 2>/dev/null; then
            echo "e2e: the game didn't start Tome; Wine's errors:" >&2
            grep -E "err:" "$WORK/wine.log" | tail -n 20 >&2
            exit 1
        fi
        sleep 0.5
    done
}

set_enabled() {
    python3 -I - "$STATE" "$1" "$2" <<'PY'
import json, sys
path, mod, enabled = sys.argv[1], sys.argv[2], sys.argv[3] == "true"
state = json.load(open(path))
state["mods"][mod]["enabled"] = enabled
open(path, "w").write(json.dumps(state, indent=2))
PY
}

FAILED=0
pass() { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
count() { grep -cE "$1" "$LOG" 2>/dev/null || true; }

# check <regex> <description> [timeout-seconds]: waits for a matching log line.
check() {
    local deadline=$((SECONDS + ${3:-5}))
    while ((SECONDS < deadline)); do
        grep -qE "$1" "$LOG" 2>/dev/null && { pass "$2"; return; }
        sleep 0.2
    done
    fail "$2 (expected log line matching: $1)"
}

# check_more <regex> <n> <description> [timeout]: waits for more than n matching lines.
check_more() {
    local deadline=$((SECONDS + ${4:-5}))
    while ((SECONDS < deadline)); do
        (($(count "$1") > $2)) && { pass "$3"; return; }
        sleep 0.2
    done
    fail "$3 (expected more than $2 log lines matching: $1, got $(count "$1"))"
}

# crash_in <mode>: turns the crash fixture off, which clears its crash, then
# on again with mode.txt saying where to crash next.
crash_in() {
    local before
    before=$(count "crash +1\.0\.0 +disabled")
    echo "$1" > "$MODS/crash/mode.txt"
    set_enabled crash false
    check_more "crash +1\.0\.0 +disabled" "$before" "crash fixture off (a crashed mod can be turned off)" 10
    set_enabled crash true
    check "\[crash\] mode \"$1\"" "crash fixture loaded again, crashing in: $1" 10
}

# check_count <regex> <n> <description> [timeout]: waits for exactly n matching lines.
check_count() {
    local deadline=$((SECONDS + ${4:-5}))
    while ((SECONDS < deadline)); do
        [[ "$(count "$1")" == "$2" ]] && { pass "$3"; return; }
        sleep 0.2
    done
    fail "$3 (expected $2 log lines matching: $1, got $(count "$1"))"
}

# probe_sequence <n>: the hook handlers that ran during hook-b's nth probe
# call of RCString::GetText, in order, then what the call returned.
probe_sequence() {
    awk -v n="$1" '
        { sub(/\r$/, "") }
        /\[hook-b\] probe start/ { ++seen; next }
        seen == n && /\[hook-[ab]\] hook: / { sub(/.*\] hook: /, ""); printf "%s ", $0 }
        seen == n && /\[hook-b\] probe returned/ { sub(/.*probe returned /, ""); print "=> " $0; exit }
    ' "$LOG" 2>/dev/null
}

# check_probe <n> <expected sequence> <description>
check_probe() {
    local deadline=$((SECONDS + 10))
    while ((SECONDS < deadline)); do
        [[ "$(probe_sequence "$1")" == "$2" ]] && { pass "$3"; return; }
        sleep 0.2
    done
    fail "$3 (expected: $2; got: $(probe_sequence "$1"))"
}

check_absent() {
    if grep -qE "$1" "$LOG" 2>/dev/null; then fail "$2 (unexpected log line matching: $1)"; else pass "$2"; fi
}

# --- run 1: everything enabled -----------------------------------------------

# In the test map's game, timed from its start (the map saves at 6 s of game
# time, which holds the game up): the map's dialog, shown on "-dialog", pauses
# the game until its hotkey T (pressed twice in case the dialog was late; T
# does nothing for the selected units), then an arrow key; then B and O on a
# worker and a hero the map selects on "-build" and "-skill" (menu events;
# Esc closes the build menu first, or O isn't a hotkey there). Then the C++
# and Rust examples' chat commands, and the same commands making them throw
# and panic.
if ((${#MAP_ARG[@]})); then
    TOME_DEBUG_CLICKS="wait:Game started: @0;type:-dialog@2500;key:T@4500;key:T@5500;key:Left@6500;\
type:-build@7000;key:B@8000;key:Esc@8500;type:-skill@9000;key:O@10500;key:Esc@11000;type:/hellors e2e@12000;\
type:/hellors panic@13500;type:/hellocpp e2e@15000;type:/hellocpp throw@16500" launch
else
    launch
fi

echo "startup"
check "Recognized Warcraft III 1\.27b \([0-9]+ mapped symbols\)" "game build recognized from the mapping database" 30
check "Signatures match \(functions and call sites\)" "game.dll matches the byte signatures"
check "JASS natives: redirected 3 of 3 InitNatives call sites" "InitNatives hooked"
check "\[hello\] enabled \(1 time" "hello loaded and enabled"
check "broken-json +error: tome\.mod\.json: line 2" "broken JSON reported with position"
check "needs-ghost .*needs \"ghost\" \^1, which isn't installed" "missing dependency reported"
check "needs-unmapped .*needs \"CUnit\.armor\", which isn't mapped for 1\.27b" "unmapped requires reported"
check "hello: code allowed \(TOME_DEBUG_APPROVE\), sha256 [0-9a-f]{64}" "a mod's code is hashed and allowed"
check "unapproved +0\.1\.0 +approval: runs native code; turn it on to allow it" "new code waits for approval"
check "unapproved: runs native code; turn it on to allow it \(in the mod menu, or /tome enable unapproved\)" \
    "the player is told how to allow it"
check "\[hello\] CTextFrame::SetText is a thiscall function at 0x[0-9a-f]{8}" "mod resolved a mapped name"
if [[ -d "$MODS/wrong-arch" ]]; then
    check "wrong-arch: the DLL is 64-bit" "64-bit DLL rejected"
fi
# Not unapproved-script's: its script isn't allowed.
check "Assets at startup: $ASSET_FILES files from 3 mods \(0 overridden" "mods' assets mounted from DllMain"
check "Asset probe .*: [0-9]+ bytes, fnv1a $PROBE_HASH" "the game reads the mod's texture instead of its own"
check "Bootstrap complete" "bootstrap finished" 15
check "Patches: [0-9]+ applied, 0 skipped: memhack-guard applied.*path-sanitizer applied.*blp-guard applied.*d3d-reset-lights applied.*gl-context-restore applied.*background-mute applied" "every built-in patch applied"
check_absent "blp-guard.*unclamped" "the BLP header-copy clamp was applied"
check "Main-thread dispatcher installed" "main-thread dispatcher installed"
check "Events: hooked screens, map load, game start and end, frames, game ticks, shutdown, chat, pause, units, spells, damage, experience, players, resources, keys, menus, dialogs, levels and saved games" "event hooks installed"
check "\[hello\] 100 ticks" "tick events reach mods"
check "\[hello-cpp\] loaded \(Tome [0-9.]+, Warcraft III 1\.27b, game\.dll at 0x[0-9a-f]{8}\)" "a C++ mod loaded"
check "\[hello-cpp\] CTextFrame::SetText is a thiscall function at 0x[0-9a-f]{8} \(void\(CTextFrame\* this" \
    "a C++ mod resolved a mapped name"
check "\[hello-cpp\] enabled" "a C++ mod enabled"
check "\[hello-cpp\] 100 ticks" "events reach a C++ lambda"
if ((RUST)); then
    check "\[hello-rs\] loaded \(Tome [0-9.]+, Warcraft III 1\.27b, game\.dll at 0x[0-9a-f]{8}\)" "a Rust mod loaded"
    check "\[hello-rs\] CTextFrame::SetText is a thiscall function at 0x[0-9a-f]{8} \(void\(CTextFrame\* this" \
        "a Rust mod resolved a mapped name"
    check "\[hello-rs\] enabled" "a Rust mod enabled"
    check "\[hello-rs\] 100 ticks" "events reach a Rust closure"
fi
check_absent "unapproved.*(LoadLibrary|not a valid|Loaded unapproved)" "code that isn't allowed is never loaded"
check "unapproved-script +0\.1\.0 +approval: replaces game scripts; turn it on to allow it" \
    "an asset-only mod with a game script waits for approval too"
MAIN_TID=$(grep -oE "main thread [0-9]+" "$LOG" | head -n 1 | grep -oE "[0-9]+$")

if ((${#MAP_ARG[@]})); then
    echo "JASS natives (map script)"
    check "bound $((21 + RUST * 4)) Tome natives" "natives bound when the map loaded" 45
    check "\[hello\] HelloLog\(42\) called from JASS" "integer argument" 30
    check "\[hello\] HelloLogReal\(2\.5\) called from JASS" "real argument and real return value"
    check "\[hello\] HelloLog\(1001\) called from JASS" "TomeIsLoaded() returned true"
    check "\[map\] TomeLogInteger\(4\)" "native result used by the map script"
    echo "memhack guard (map script)"
    check "Map script guard: 2 local/global type-shadow\(s\) in war3map\.j" "map scan reports the shadowing locals"
    check "  line [0-9]+: local 'tome_e2e_shadow' \(integer\) shadows global of type unit" "with the name and both types"
    check "memhack-guard: endfunction restored 1 shadowed global" "the guard restored the global at endfunction"
    check "\[map\] TomeLogInteger\(9506\)" "a shadowing local keeps its type up to its return"
    check "\[map\] TomeLogInteger\(9507\)" "and up to a set just before endfunction"
    check "\[map\] TomeLogInteger\(9510\)" "the global keeps its own type in later functions"
    check "\[hello\] map script shadows: 2, 2 listed \(first: local integer tome_e2e_shadow on line [0-9]+, global unit\)" "mods get the findings (script_shadows, map_script_shadows)"
    check "Map script guard: told the player" "the player is told at game start" 30
    echo "path sanitizer (map script)"
    for native in InitGameCache PreloadGenEnd SaveGame CopySaveGame RenameSaveDirectory RemoveSaveDirectory \
        Preloader SaveGameExists CreateSound StartMeleeAI PlayMusic; do
        check "path-sanitizer: refused $native\(\"\.\.\\\\" "$native refused a name outside the data folder"
    done
    check "path-sanitizer: refused AddSpecialEffect\(\"C:" "AddSpecialEffect refused an absolute path"
    check_absent "path-sanitizer: refused AddSpecialEffect\(\"\"" "an empty model name isn't refused"
    check "BlzLoadTOCFile: refused" "BlzLoadTOCFile refused a TOC outside the data folder"
    check "\[map\] TomeLogInteger\(9600\)" "a refused InitGameCache gives a null cache that reads 0"
    for n in 9601 9602 9603 9604 9605 9606 9607 9608; do
        check "\[map\] TomeLogInteger\($n\)" "refused call $n returned false or null"
    done
    check "\[map\] TomeLogInteger\(9609\)" "the script carried on after the refused calls"
    if compgen -G "$GAME/../TomeE2E-escape*" >/dev/null; then
        fail "a file was written outside the game folder"
    else
        pass "nothing was written outside the game folder"
    fi
    echo "game events (-loadfile)"
    check "\[hello\] map load: .+ \(Maps\\\\TomeE2E\\\\e2e\.w3m\)" "map load event with the map's name and path"
    check "\[hello\] game start: " "game start event" 30
    check "\[hello\] 200 game ticks: 1\.000 s of game time" "game ticks follow game time (200 steps of 5 ms)" 15
    check "\[hello\] map description: ." "map load event with the map's description"
    check "\[map\] TomeLogInteger\(84(0[1-9]|1[0-2])\)" "the map selected some of its units" 15
    check "\[hello\] selected [0-9]+ units?" "selection event" 5
    check "\[hello\]   \* .+ \([A-Za-z0-9]{4}\) of player 1.*, handle 0x1[0-9a-f]{5}" "with the unit's name, type, owner and handle"
    check "\[map\] TomeLogInteger\(8500\)" "the map paused and resumed the game" 15
    check "\[hello\] game paused by a trigger" "pause event from PauseGame"
    check "\[hello\] game resumed by a trigger" "resume event from PauseGame"
    echo "unit, player and saved game events (map script)"
    check "\[hello\] Archmage \(Hamg\) of player 1 reached level 3" "hero level event, with the level" 15
    check "\[hello\] Archmage \(Hamg\) of player 1 picked up Potion of Invisibility \(pinv\)" "item pickup event, with the item"
    check "\[hello\] Archmage \(Hamg\) of player 1 used Potion of Invisibility \(pinv\)" "item use event"
    check "\[hello\] Archmage \(Hamg\) of player 1 dropped Scroll of Town Portal \(stwp\)" "item drop event"
    check "\[hello\] Footman \(hfoo\) of player 1 died, killed by Archmage \(Hamg\) of player 1" "death event, with the killer"
    check "\[hello\] Water Elemental \(hwat\) of player 1 summoned by Archmage \(Hamg\) of player 1" "summon event" 10
    check "\[hello\] Peasant \(hpea\) of player 1 trained in Town Hall \(htow\) of player 1" "training event, with the building" 15
    check "\[hello\] Farm \(hhou\) of player 1 built" "construction event" 15
    check "\[map\] TomeLogInteger\(8601\)" "the map ordered the Keep upgrade" 10
    check "\[hello\] upgraded to Keep \(hkee\) of player 1" "upgrade event, with the new type" 15
    check "\[map\] TomeLogInteger\(8606\)" "the map trained, cancelled, attacked, loaded, gave away and killed units" 10
    check "\[hello\] training started: Town Hall \(htow\) of player 1, Peasant \(hpea\)" "training started event, with the unit type"
    check "\[hello\] training cancelled: Town Hall \(htow\) of player 1, Peasant \(hpea\)" "training cancelled event"
    check "\[hello\] Town Hall \(htow\) of player 1 ordered: 851976" "order event without a target (cancel)"
    check "\[hello\] Peasant \(hpea\) of player 1 ordered: [0-9]+ to -?[0-9]+, -?[0-9]+" "point order event, with the point"
    check "\[hello\] Footman \(hfoo\) of player 1 ordered: 851983 on Peasant \(hpea\) of player 1" "target order event, with the target (attack)"
    check "\[hello\] attacked: Peasant \(hpea\) of player 1, other Footman \(hfoo\) of player 1" "attacked event, with the attacker" 10
    check "\[hello\] loaded: Footman \(hfoo\) of player 1, other Goblin Zeppelin \(nzep\) of player 1" "loaded event, with the transport" 10
    check "\[hello\] Footman \(hfoo\) of player 2 changed owner from player 1" "owner change event, with the previous owner"
    check "\[hello\] Archmage \(Hamg\) of player 1 learned Summon Water Elemental \(AHwe\) level 1" "hero skill event, with the ability and level"
    check "\[hello\] construction started: Farm \(hhou\) of player 1" "construction started event"
    check "\[hello\] upgrade started: Town Hall \(htow\) of player 1, Keep \(hkee\)" "upgrade started event, with the new type" 10
    check "\[hello\] revivable: Paladin \(Hpal\) of player 1" "hero revivable event" 15
    check "\[hello\] revival started: Paladin \(Hpal\) of player 1" "revival started event" 10
    check "\[hello\] revived: Paladin \(Hpal\) of player 1" "revived event"
    check "\[hello\] saving the game: Save\\\\.+\\\\TomeE2E\.w3z" "save event with the saved game's path" 15
    check "\[hello\] Archmage \(Hamg\) of player 1 begins casting Summon Water Elemental \(AHwe\)" "spell cast event, with the ability"
    check "\[hello\] Archmage \(Hamg\) of player 1 gets the effect of Summon Water Elemental \(AHwe\)" "spell effect event"
    check "\[hello\] Archmage \(Hamg\) of player 1 stops casting Summon Water Elemental \(AHwe\)" "spell end cast event"
    check "\[hello\] Footman \(hfoo\) of player 1 takes [0-9.]+ damage from Archmage \(Hamg\) of player 1" "damage event, with the source and amount"
    check "\[hello\] .+ \(LTlt\) destroyed, handle 0x1[0-9a-f]{5}" "destructable death event, with its handle" 10
    check "\[hello\] .+ \(ratf\) destroyed, handle 0x1[0-9a-f]{5}" "item death event, with its handle"
    check "\[hello\] (Gnoll \(ngno\) of player 13 acquired .+|.+ acquired Gnoll \(ngno\) of player 13)" "acquired target event" 10
    check "\[hello\] Footman \(hfoo\) of player 1 decays" "decay event" 20
    check "\[hello\] .+ clicked dialog 0x1[0-9a-f]{5} button 0x1[0-9a-f]{5}" "dialog click event (simulated hotkey), with the handles" 10
    check "\[hello\] .+ pressed Left" "arrow key pressed event (the map listens for it)"
    check "\[hello\] .+ released Left" "arrow key released event"
    check "\[hello\] .+ opened the build menu" "build menu event (simulated B on a selected worker)" 10
    check "\[hello\] .+ opened the skill menu" "skill menu event (simulated O on a selected hero)" 10
    check "\[hello\] created Rifleman \(hrif\) of player 1" "unit created event"
    check "\[hello\] removed Rifleman \(hrif\) of player 1" "unit removed event (RemoveUnit)"
    check "\[hello\] Archmage \(Hamg\) of player 1 gained [0-9]+ experience \(level 3\)" "hero experience event, with the level"
    check "\[hello\] .+'s gold: [0-9]+ -> 777" "resource event (SetPlayerState), with the new value"
    check "\[hello\] .+'s food used: [0-9]+ -> [0-9]+" "resource event for food"
    check "\[hello\] night falls at 18\.[0-9]+" "night event (the time of day passed 18:00)"
    check "\[hello\] day breaks at 12\.00" "day event (a script set the time of day to noon)"
    check "\[hello\] player 1 \(.+\) won" "victory event (melee, from RemovePlayer)" 20
    echo "frame natives (map script)"
    check "\[map\] TomeLogInteger\(7001\)" "BlzLoadTOCFile read its string argument and loaded the TOC"
    check "\[map\] TomeLogInteger\(7100\)" "BlzGetFrameByName returns 0 for an unknown frame"
    check "\[map\] TomeLogInteger\(7201\)" "BlzCreateFrame created a frame findable by name and context"
    check "\[map\] TomeLogInteger\(7301\)" "the created frame's children use the create context"
    check "\[map\] TomeLogInteger\(7400\)" "BlzCreateFrame without an owner returns 0"
    check "\[map\] TomeLogInteger\(7999\)" "SetText, SetPoint and SetVisible ran without stopping the script"
    check "\[map\] TomeLogInteger\(7002\)" "a TOC from a mod's assets folder loaded"
    check "\[map\] TomeLogInteger\(7501\)" "a frame defined in a mod's assets folder created"
    check "\[map\] TomeLogInteger\(7600\)" "the game ran Blizzard.j from a mod's assets folder"
    echo "JASS strings and code (map script)"
    check '\[map\] TomeLogString\("Hello, Tome! \(4 letters\)"\)' "string argument read, string returned"
    check $'\\[map\\] TomeLogString\\("Hello, \u0396\u03b5\u03cd\u03c2! \\(4 letters\\)"\\)' "UTF-8 through a native both ways"
    check '\[map\] TomeLogString\("Hello, world! \(5 letters\)"\)' "null string argument"
    check '\[map\] TomeLogString\(null\)' "null string read as NULL"
    check "\[map\] TomeLogInteger\(8001\)" "returned string compares equal in JASS"
    check "\[map\] TomeLogInteger\(8028\)" "returned string has its UTF-8 byte length"
    check "\[map\] TomeLogInteger\(8100\)" "code argument called from inside a native"
    check "\[hello\] HelloCall: callback returned 1 \(status 0\)" "the callback's boolean came back"
    check "\[map\] TomeLogInteger\(8101\)" "the native's result reached the script"
    check "\[map\] TomeLogInteger\(8200\)" "a code argument returning nothing runs"
    check "\[map\] TomeLogInteger\(8201\)" "and counts as false"
    check "\[map\] TomeLogInteger\(8999\)" "the script ran to the end"
    check '\[map\] TomeLogString\("Hello, game start! \(10 letters\)"\)' "a stored callback ran from the game start event" 30
    check "\[hello\] game start callback returned 1 \(status 0\)" "and returned its result"
    check_count "\[hello\] HelloRun: status 0" 2 "code run as an action thread"
    check_count "\[map\] TomeLogInteger\(830[0-2]\)" 5 "both threads waited, the script went on, then both resumed" 30
    if [[ "$(grep -oE "TomeLogInteger\(830[0-2]\)" "$LOG" | tr -d '\r' | paste -sd' ')" == \
          "TomeLogInteger(8300) TomeLogInteger(8300) TomeLogInteger(8301) TomeLogInteger(8302) TomeLogInteger(8302)" ]]; then
        pass "in that order"
    else
        fail "in that order (got: $(grep -oE "TomeLogInteger\(830[0-2]\)" "$LOG" | paste -sd' '))"
    fi
    check "\[hook-a\] unseen code value: status 1, result 0" "a code value no native received is refused"
    check "code value 2147483632 wasn't passed to a native by the current map script" "and logged"
    echo "a mod in C++ (map script and chat)"
    check "\[hello-cpp\] map load: .+ \(Maps\\\\TomeE2E\\\\e2e\.w3m\)" "map load event in C++, with the map's path"
    check "\[hello-cpp\] HelloCppLog\(42\) called from JASS" "C++ natives: integer and real arguments, real result"
    check $'\\[map\\] TomeLogString\\("Hello from C\\+\\+, \u0396\u03b5\u03cd\u03c2! \\(4 letters\\)"\\)' \
        "UTF-8 strings both ways through a C++ native"
    check "\[hello-cpp\] HelloCppCall: callback returned 1 \(status 0\)" "a code argument run from C++"
    check "\[hello-cpp\] HelloCppLog\(2002\)" "its boolean reached the script"
    check "\[hello-cpp\] first CTextFrame::SetText after enable: \"" "a C++ hook lambda read the call's string argument"
    check "\[hello-cpp\] game start: " "game start event in C++" 30
    check "\[hello-cpp\] /hellocpp command: Hello from C\+\+, e2e\." "a C++ chat command, with a string setting" 30
    check "\[hello-cpp\] uncaught exception: /hellocpp throw" "an exception leaving a lambda is logged" 15
    check "hello-cpp crashed in chat command /hellocpp: the mod reported a failure \(logged above\)" \
        "and Tome stops the call like a crash"
    check "Unloaded hello-cpp \(its DLL stays loaded after the crash\)" "the throwing mod was disabled" 10
    if ((RUST)); then
        echo "a mod in Rust (map script and chat)"
        check "\[hello-rs\] MAP_LOAD: .+ \(Maps\\\\TomeE2E\\\\e2e\.w3m\)" "map load event in Rust, with the map's path"
        check "\[hello-rs\] HelloRsLog\(42\) called from JASS" "Rust natives: integer and real arguments, real result"
        check $'\\[map\\] TomeLogString\\("Hello from Rust, \u0396\u03b5\u03cd\u03c2! \\(4 letters\\)"\\)' \
            "UTF-8 strings both ways through a Rust native"
        check_more "\[map\] TomeLogInteger\(8100\)" 2 "a code argument run from Rust (and from hello and hello-cpp)"
        check "\[hello-rs\] HelloRsCall: callback returned Ok\(true\)" "with its boolean"
        check "\[hello-rs\] HelloRsLog\(2002\)" "which reached the script"
        check "\[hello-rs\] first CTextFrame::SetText after enable: \"" "a Rust hook handler read the call's string argument"
        check "\[hello-rs\] GAME_START: " "game start event in Rust" 30
        check "\[hello-rs\] /hellors command: Hello from Rust, e2e\." "a Rust chat command, with a string setting" 30
        check "\[hello-rs\] panicked at .*examples.hello-rs.src.lib\.rs:[0-9]+.*: /hellors panic" \
            "a panic is logged with where it happened" 15
        check "hello-rs crashed in chat command /hellors: the mod reported a failure \(logged above\)" \
            "and Tome stops the call like a crash"
        check "Unloaded hello-rs \(its DLL stays loaded after the crash\)" "the panicking mod was disabled" 10
    fi
    echo "crash isolation: a native (map script)"
    check "crash crashed in native CrashNative: access violation writing 0x00000000 at crash\.[0-9]+\.dll\+0x[0-9a-f]+; Tome stopped the call" \
        "a crash in a mod's native is caught, with the module and offset"
    check "\[map\] TomeLogInteger\(9000\)" "the crashed native returned 0 and the script went on"
    check "\[map\] TomeLogInteger\(9100\)" "the crashed mod's natives return 0 from then on"
    check "\[crash\] disabled" "the crashed mod still got tome_mod_disable" 10
    check "Unloaded crash \(its DLL stays loaded after the crash\)" "it was disabled and unloaded, its DLL kept"
    check "crash +1\.0\.0 +error: crashed in native CrashNative" "its status says where it crashed"
fi

echo "asset overlay"
set_enabled asset-demo false
check "Assets: 2 files from 2 mods" "assets of a disabled live mod removed"
check_count "Asset probe" 2 "probe ran after the change"
if grep -E "Asset probe" "$LOG" | tail -n 1 | grep -q "fnv1a $PROBE_HASH"; then
    fail "the game still reads the disabled mod's texture"
else
    pass "the game reads its own texture again"
fi
set_enabled asset-demo true
check_count "Asset probe .*fnv1a $PROBE_HASH" 2 "re-enabling brings the texture back" 10
if ((${#MAP_ARG[@]})); then
    # A map is running, so a "reload": "map" mod's assets stay until the main menu.
    set_enabled asset-map false
    check "Assets of asset-map: changes wait for the next map load or the main menu" "reload: map waits while a map runs"
    check_count "Assets: " 2 "the archive wasn't rebuilt for it"
fi

echo "hooks (two mods on RCString::GetText)"
BOTH='b-before a-before b-replace b-next-returned "TomeHookProbe from hook-a" b-after a-after => "TomeHookProbe from hook-a"'
B_ONLY='b-before b-replace b-next-returned "TomeHookProbe" b-after => "TomeHookProbe"'
check "Hooked RCString::GetText at 0x[0-9a-f]{8} \(5 bytes moved, 1 arguments\)" "the game function was patched once"
check "\[hook-a\] hooked RCString::GetText" "hook-a registered its handlers"
check_probe 1 "$BOTH" "both mods' handlers ran in priority order, around the original"
set_enabled hook-a false
check "Disabled hook-a" "hook-a disabled"
set_enabled hook-b false
check "Disabled hook-b" "hook-b disabled"
set_enabled hook-b true
check_probe 2 "$B_ONLY" "hook-a's handlers were revoked; hook-b's still run"
set_enabled hook-a true
check_count "\[hook-a\] hooked RCString::GetText" 2 "hook-a re-enabled"
set_enabled hook-b false
check_count "Disabled hook-b" 2 "hook-b disabled again"
set_enabled hook-b true
check_probe 3 "$BOTH" "re-enabling restores the chain"
check_count "Hooked RCString::GetText" 1 "the patch was installed only once"

echo "crash isolation: events, hooks, lifecycle"
crash_in tick
check "crash crashed in event handler \(tick\): access violation writing 0x00000000" "a crash in an event handler is caught" 10
check_count "Unloaded crash \(its DLL stays loaded" $(( ${#MAP_ARG[@]} ? 2 : 1 )) "the mod was stopped" 10
crash_in hook
check "crash crashed in replace hook RCString::GetText: access violation" "a crash in a REPLACE hook handler is caught"
check '\[crash\] probe returned "TomeCrashProbe"' "the original ran in place of the crashed handler"
crash_in enable
check "crash crashed in tome_mod_enable: access violation" "a crash in tome_mod_enable is caught"
check_count "Unloaded crash \(its DLL stays loaded" $(( ${#MAP_ARG[@]} ? 4 : 3 )) "the mod was stopped again" 10
crash_in load
check "Failed to load crash: crashed in tome_mod_load: access violation" "a crash in tome_mod_load is a load failure"
check "crash +1\.0\.0 +error: crashed in tome_mod_load" "and shows in its status"
if env_running; then pass "the game survived every crash"; else fail "the game survived every crash"; fi

echo "live toggling"
set_enabled hello false
check "\[hello\] disabled" "disable via state.json"
if [[ -n "$MAIN_TID" ]] && grep -E "\] \[ *$MAIN_TID\] .*Disabled hello" "$LOG" >/dev/null; then
    pass "disable ran on the game's main thread"
else
    fail "disable didn't run on the main thread (main thread: ${MAIN_TID:-unknown})"
fi
set_enabled hello true
check "\[hello\] enabled \(2 times" "re-enable via state.json"

echo "hot reload"
cp "$BUILD/mods/hello/hello.dll" "$MODS/hello/hello.dll.new"
touch "$MODS/hello/hello.dll.new"
mv -f "$MODS/hello/hello.dll.new" "$MODS/hello/hello.dll"
check "Unloaded hello" "old DLL unloaded"
check_count "\[hello\] loaded \(Tome" 2 "new DLL loaded"
if grep -E "\[hello\] enabled" "$LOG" | tail -n 1 | grep -q "(1 time"; then
    pass "new DLL starts with fresh state"
else
    fail "new DLL kept the old DLL's state"
fi

echo "removing a mod"
rm -rf "$MODS/needs-ghost"
REMAINING=$(find "$MODS" -mindepth 1 -maxdepth 1 -type d ! -name config | wc -l)
ACTIVE=$(( ${#MAP_ARG[@]} ? 5 : 6 ))  # hello, hook-a, hook-b, asset-demo, asset-restart, and asset-map unless disabled above
check "Mods: $REMAINING installed, $ACTIVE active" "mod list refreshed"
if ! tail -n 5 "$LOG" | grep -q "needs-ghost"; then pass "removed mod gone from the list"; else fail "removed mod still listed"; fi

echo ".tome packages"
PACKAGES="$HOME_DIR/tome/packages"
if [[ -f "$BUILD/packages/hello.tome" ]]; then
    # Next to the hello folder (same id): the folder is used.
    cp "$BUILD/packages/hello.tome" "$MODS/hello-pkg.tome"
    check "Unpacked hello-pkg\.tome" "a .tome is unpacked" 10
    check "hello +error: the folder hello has the same id and is used instead" "a folder wins over a package with its id" 10
    # Without the folder, the package's copy runs.
    before=$(count "\[hello\] loaded \(Tome")
    mv "$MODS/hello" "$WORK/hello-folder"
    check_more "\[hello\] loaded \(Tome" "$before" "the package runs once the folder is gone" 10
    check "\[hello\] enabled" "the packaged mod is enabled" 10
    # Updated in place: a new version in the same file reloads it from a new copy.
    python3 -I - "$BUILD/packages/hello.tome" "$WORK/hello-pkg.tome" <<'PY'
import json, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
    for info in zin.infolist():
        data = zin.read(info)
        if info.filename == "tome.mod.json":
            meta = json.loads(data)
            meta["version"] = "1.0.1"
            data = json.dumps(meta).encode()
        zout.writestr(info.filename, data)
PY
    mv -f "$WORK/hello-pkg.tome" "$MODS/hello-pkg.tome"
    check "Loaded hello 1\.0\.1" "an updated package reloads the mod" 15
    check "Removed the unused unpacked package hello-pkg\." "the old unpacked copy is removed" 10
    COPIES=$(find "$PACKAGES" -mindepth 1 -maxdepth 1 -name 'hello-pkg.*' | wc -l)
    if [[ "$COPIES" == 1 ]]; then pass "one unpacked copy left"; else fail "$COPIES unpacked copies of hello-pkg"; fi
    # Removed: the mod stops, and its unpacked copy goes too.
    before=$(count "Unloaded hello")
    rm -f "$MODS/hello-pkg.tome"
    check_more "Unloaded hello" "$before" "a removed package's mod is unloaded" 10
    deadline=$((SECONDS + 10))
    while ((SECONDS < deadline)) && [[ -n "$(find "$PACKAGES" -mindepth 1 -maxdepth 1 -name 'hello-pkg.*')" ]]; do sleep 0.2; done
    if [[ -z "$(find "$PACKAGES" -mindepth 1 -maxdepth 1 -name 'hello-pkg.*')" ]]; then
        pass "no unpacked copy left"
    else
        fail "the unpacked copy stayed after the package was removed"
    fi
    before=$(count "\[hello\] loaded \(Tome")
    mv "$WORK/hello-folder" "$MODS/hello"
    check_more "\[hello\] loaded \(Tome" "$before" "the hello folder runs again" 10
else
    fail ".tome packages: $BUILD/packages/hello.tome not built"
fi

stop_game

# --- run 2: map uses natives of a disabled mod --------------------------------

if ((${#MAP_ARG[@]})); then
    echo "disabled mod's natives (second launch)"
    set_enabled hello false
    set_enabled crash false
    launch
    check "hello +1\.0\.0 +disabled" "hello starts disabled" 30
    check "\[map\] TomeLogInteger\(0\)" "map still loads; disabled native returned 0" 45
    check_absent "HelloLog\(" "disabled natives don't reach the mod"
    # The map script loads its saved game in this launch only (it saves at
    # 6 s, loads at 8 s); hello comes back on to see it.
    echo "loading a saved game (second launch)"
    set_enabled hello true
    check "\[hello\] enabled" "hello enabled mid-game" 15
    check "Saving the game: Save\\\\.+\\\\TomeE2E\.w3z" "the map saved the game" 20
    check "\[hello\] the level ends" "level end event (LoadGame from the script)" 30
    check "\[hello\] loaded a saved game: Save\\\\.+\\\\TomeE2E\.w3z" "loaded event with the saved game's path" 30
    check_count "Game started: " 2 "the loaded game started" 30
    check "\[map\] TomeLogInteger\(8604\)" "the loaded game's timers run (and don't load again)" 15
    stop_game
fi

# --- run 3: the mod menu on the main screen ------------------------------------

echo "mod menu (third launch, simulated clicks)"
rm -rf "$MODS/broken-json" "$MODS/wrong-arch" "$MODS/needs-ghost" "$MODS/asset-demo" "$MODS/asset-map" \
    "$MODS/asset-restart" "$MODS/hook-a" "$MODS/hook-b" "$MODS/crash" "$MODS/unapproved" "$MODS/unapproved-script"
set_enabled hello true
# A mod whose icon is a malformed BLP (valid "BLP1" magic, mip past EOF), to
# check the blp-guard patch refuses it when the menu decodes the icon. It
# sorts after hello, so hello stays row 0 and the clicks below are unchanged.
cp -r "$FIXTURES/zz-bad-icon" "$MODS/zz-bad-icon"
python3 -I - "$MODS/zz-bad-icon/icon.blp" <<'PY'
import struct, sys
h = bytearray(0xA0)
h[0:4] = b"BLP1"
struct.pack_into("<I", h, 0x04, 0)          # JPEG content
struct.pack_into("<I", h, 0x0C, 32)         # width
struct.pack_into("<I", h, 0x10, 32)         # height
struct.pack_into("<I", h, 0x18, 0)          # one mip level
struct.pack_into("<I", h, 0x1C, 0xA0)       # mipOffsets[0]
struct.pack_into("<I", h, 0x5C, 0x7fffffff) # mipSizes[0]: runs far past EOF
struct.pack_into("<I", h, 0x9C, 0)          # jpegHeaderSize
open(sys.argv[1], "wb").write(h)
PY
# Click positions are fractions of the window: Mods button, row 0's toggle
# (twice, once the panel has lowered), Done, then the hello mod's own button
# (which was hidden and recreated by the toggling), timed from the main
# menu's setup and then from the panel's opening. Then the panel again:
# hello's Details, Settings, the Shout toggle (row 2: a setting takes two), the Greeting text box
# (row 0: "Hello" replaced by "Howdy", Enter), Esc, Esc (back to the list);
# Tome, Keybinds, row 1's key (Greet; row 0 is Tome's own), Ctrl+J for it,
# Ctrl+J again (runs it), Esc three times (closes the panel), then Ctrl+F10
# (Tome's key for the menu). They follow the layout in
# loader/src/core/main_menu_fdf.cpp and sdk/examples/hello/hello.c.
SAVED_MAP_ARG=("${MAP_ARG[@]}")
MAP_ARG=()
TOME_DEBUG_CLICKS="wait:Main menu set up@0;0.874,0.84@4000;wait:Mod menu opened@0;0.447,0.227@2500;\
0.447,0.227@4500;0.447,0.81@6500;0.5,0.892@8000;0.874,0.84@9500;wait:Mod menu opened@9500;0.347,0.227@2500;\
0.209,0.81@3500;0.447,0.367@4500;0.403,0.227@5300;key:End@5700;key:Backspace@5800;key:Backspace@5900;\
key:Backspace@6000;key:Backspace@6100;key:Backspace@6200;text:Howdy@6400;key:Enter@7000;key:Esc@8000;\
key:Esc@9000;0.322,0.81@10500;0.209,0.81@11500;0.447,0.297@12500;key:Ctrl+J@13500;key:Ctrl+J@15500;\
key:Esc@16500;key:Esc@17500;key:Esc@18500;key:Ctrl+F10@20500" launch
MAP_ARG=("${SAVED_MAP_ARG[@]}")
check "Main menu set up; Mods button added" "Mods button added to the main menu" 30
check "Quit moved to the lower slot" "Quit shares the bottom box with Mods"
check "\[hello\] loaded frame definitions from ui/hello\.fdf" "mod loaded its own FDF"
check "\[hello\] added a Hello button to the main menu" "mod added its own button to the main menu"
check "\[hello-cpp\] added a Hello \(C\+\+\) button to the main menu" "a C++ mod's frames on the main menu"
((RUST)) && check "\[hello-rs\] added a Hello \(Rust\) button to the main menu" "a Rust mod's frames on the main menu"
check "Mod menu opened" "clicking Mods opens the panel" 15
check "Mod menu: disabling hello" "row button disables the mod" 10
check "\[hello\] disabled" "mod disabled live"
check "Mod menu: enabling hello" "row button enables it again" 10
check "\[hello\] enabled \(2 times" "mod re-enabled live"
check "Mod menu closed" "Done closes the panel" 10
check_count "\[hello\] added a Hello button" 2 "re-enabling recreated the mod's button right away"
check "\[hello\] Hello button clicked \(1\)" "the mod's click callback ran" 10
if python3 -I -c "import json,sys; sys.exit(0 if json.load(open('$STATE'))['mods']['hello']['enabled'] else 1)"; then
    pass "state.json saved by the menu"
else
    fail "state.json doesn't show hello enabled"
fi
check "hello: icon icon\.blp loaded" "the list shows the mod's icon" 15
check "blp-guard: refused a malformed texture .*mip runs past the file" "the BLP guard refused a malformed icon" 15
rm -rf "$MODS/zz-bad-icon"
check "hello: setting shout = On" "the settings page changes a setting" 15
check "hello: setting greeting = Howdy" "a text setting typed into its box, saved with Enter" 10
check "\[hello\] setting changed: shout" "the mod hears about it"
if python3 -I -c "import json,sys; sys.exit(0 if json.load(open('$MODS/config/hello.json'))['shout'] is True else 1)"; then
    pass "config/hello.json saved"
else
    fail "config/hello.json saved"
fi
if awk '/hello: setting shout = On/ { on = 1 } /hello: setting greeting = Howdy/ { on = 0 } on && /Refreshing mods/ { bad = 1 } END { exit bad }' "$LOG"; then
    pass "Tome's own settings writes don't refresh the mods"
else
    fail "Tome's own settings writes don't refresh the mods"
fi
# A hand edit while the game runs: only that mod's settings are read again.
python3 -I - "$MODS/config/hello.json" <<'PY'
import json, sys
path = sys.argv[1]
values = json.load(open(path))
values["seconds"] = 7.5
open(path, "w").write(json.dumps(values))
PY
check "hello: config\\\\hello.json changed on disk \\(1 setting\\)" "a settings file edited by hand applies" 10
check "\\[hello\\] setting changed: seconds" "and the mod hears about it"
check "Keybind hello/greet: Ctrl\+J" "the keybinds page takes a new key" 15
check "\[hello\] Greet key: Howdy, world!" "the new key runs the mod's keybind, which reads the setting" 10
if python3 -I -c "import json,sys; sys.exit(0 if json.load(open('$HOME_DIR/tome/keybinds.json'))['hello']['greet'] == 'Ctrl+J' else 1)"; then
    pass "tome/keybinds.json saved"
else
    fail "tome/keybinds.json saved"
fi
check_count "Mod menu closed" 2 "Esc goes back page by page and closes the panel" 10
check_count "Mod menu opened" 3 "Ctrl+F10 opens it" 10
stop_game

# --- run 4: main menu -> game -> main menu, with waiting asset changes -----------

echo "asset reload modes (fourth launch, a game started from the menu)"
cp -r "$FIXTURES/asset-map" "$FIXTURES/asset-restart" "$MODS/"
python3 -I - "$STATE" <<'PY'
import json, sys
path = sys.argv[1]
state = json.load(open(path))
for mod in ("asset-map", "asset-restart"):
    state["mods"][mod] = {"enabled": True}
open(path, "w").write(json.dumps(state, indent=2))
PY
# Single Player, Custom Game, Start Game; in the game Menu, End Game, Quit
# Mission; on the score screen OK; then Cancel twice back to the main menu.
SAVED_MAP_ARG=("${MAP_ARG[@]}")
MAP_ARG=()
# On the single player screen also the hello mod's button. In the game (timed
# from its start), chat commands, Esc, then the hello mod's in-game button;
# in the Menu, Mods, Esc (back to the menu), Esc (closes it), Ctrl+F10 (the
# menu with Mods open), Done.
TOME_DEBUG_CLICKS="wait:Main menu set up@0;0.86,0.245@5000;wait:Screen: single player@0;0.5,0.892@1500;\
0.867,0.657@3000;wait:Screen: custom game@0;0.875,0.80@4000;wait:Game started: @0;type:/hello e2e@4000;\
type:/tome disable hello@6500;type:/hello ignored@9000;type:/tome enable hello@11000;type:/tome nonsense@13000;\
key:Esc@14200;0.5,0.475@15000;0.163,0.018@16000;0.574,0.418@17000;key:Esc@17700;key:Esc@18200;\
key:Ctrl+F10@18700;0.676,0.653@19400;0.5,0.541@20200;0.5,0.29@22000;wait:Screen: score screen@0;\
0.788,0.942@4000;wait:Screen: custom game@0;0.859,0.925@3000;wait:Screen: single player@0;0.859,0.878@3000" launch
MAP_ARG=("${SAVED_MAP_ARG[@]}")
check "Assets at startup: 2 files from 2 mods" "both mods' assets mounted" 30
check "Engine initialized natives" "a game started from the main menu" 40
set_enabled asset-map false
set_enabled asset-restart false
check "Assets of asset-map: changes wait for the next map load or the main menu" "reload: map waits during a game started from the menu"
check "Assets of asset-restart: changes wait for a restart" "reload: restart waits"
echo "events, screens and chat commands (fourth launch)"
check "\[hello\] added a Hello button to the single player screen" "on_ui for the single player screen"
check "\[hello\] Hello button clicked \(1\)" "click on a single player screen button"
check "\[hello\] map load: " "map load event for a game started from the menu"
check "\[hello\] added a Hello button to the game" "on_ui for the in-game UI"
check "\[hello\] game start: " "game start event"
check "\[hello\] /hello command: Howdy, e2e!" "a mod's chat command ran (with the settings from the third launch)" 30
check "Chat command: /tome disable hello" "/tome disable" 10
check "\[hello\] disabled" "/tome disable disabled the mod"
check "Chat command: /tome enable hello" "/tome enable" 10
check_count "\[hello\] added a Hello button to the game" 2 "re-enabling mid-game rebuilt the mod's in-game button"
check_count "\[hello\] game start: " 2 "a mod enabled mid-game gets the game start"
check "Chat command: /tome nonsense" "unknown /tome subcommands are still swallowed" 10
check_absent "Hello, ignored" "a disabled mod's command doesn't run"
check "\[hello\] .+ pressed Esc" "Esc event (sent whether or not the map listens)"
check "\[hello\] Hello button clicked \(2\)" "click on an in-game button" 10
check "Esc menu set up; Mods button added" "the Esc menu has a Mods button"
check "\[hello\] game paused by .+" "the Esc menu pauses a single player game: pause event with the player" 10
check "\[hello\] game resumed by .+" "and closing it resumes" 10
check "Mod menu opened in game" "Mods opens the mod menu in game" 10
check "Mod menu closed in game" "Esc goes back to the Esc menu" 10
check_count "Mod menu opened in game" 2 "Ctrl+F10 opens the Esc menu with Mods" 10
check_count "Mod menu closed in game" 2 "Done goes back to the Esc menu" 10
check "\[hello\] game end: .* after [0-9.]+ s of game time" "game end event, with the game time" 20
check_count "Main menu set up" 2 "back at the main menu after the game" 45
check "Assets: 1 file from 1 mod" "the reload: map change applied at the main menu" 5
check "asset-restart: restart to apply asset changes" "the restart mod's change is still waiting"
check_count "Assets of asset-map: changes wait" 1 "the wait is logged once"
stop_game

# --- run 5: a LAN game, hosted alone against the computer ----------------------

echo "LAN game (fifth launch)"
# Local Area Network, Create Game, Create Game; Cancel the lobby; Create Game
# again, the second slot to Computer (Easy), Start Game. In the game, the
# hello button, then a chat line (the button mustn't keep the keyboard
# focus), then Menu, End Game, Exit Program, Exit: the program ends from
# inside the game.
SAVED_MAP_ARG=("${MAP_ARG[@]}")
MAP_ARG=()
TOME_DEBUG_CLICKS="wait:Main menu set up@0;0.855,0.434@5000;wait:Screen: LAN@0;0.394,0.925@3000;\
0.862,0.845@6000;0.861,0.918@9000;0.394,0.925@12000;0.862,0.845@15000;0.161,0.119@18000;0.101,0.214@19500;\
0.861,0.823@21500;wait:Game started: @0;0.5,0.475@4000;type:/hello focus@6000;type:hi lan@8000;\
0.163,0.018@10000;0.5,0.541@13000;0.5,0.347@16000;0.414,0.484@18500" launch
MAP_ARG=("${SAVED_MAP_ARG[@]}")
check "\[hello\] map load: " "map load for a LAN game" 45
check_count "Map loading: " 1 "creating and cancelling a lobby is no map load"
check_absent "Game ended" "nor a game end"
check "\[hello\] game start: " "game start for a LAN game" 20
check "\[hello\] Hello button clicked \(1\)" "click on an in-game button" 15
check "\[hello\] /hello command: Howdy, focus!" "Enter opens the chat box again after a click" 10
check "\[hello\] chat from .+ \(player 1, to [0-3]\): hi lan" "chat event in a LAN game, with the sender" 10
check "\[hello\] game end: " "game end when the program exits from a LAN game" 20
check_count "\[hello\] game end: " 1 "the game ended once"
for _ in $(seq 1 40); do env_running || break; sleep 0.5; done
if env_running; then fail "the game exited"; else pass "the game exited"; fi
check "Exiting: disabled and unloaded [0-9]+ mods?" "mods shut down at exit"
if awk '/\[hello\] game end/ { e = NR } /\[hello\] disabled/ && e { d = NR } /\[hello\] unloading/ && d { u = NR } END { exit !(e && d && u) }' "$LOG"; then
    pass "at exit: game end, then tome_mod_disable, then tome_mod_unload"
else
    fail "at exit: game end, then tome_mod_disable, then tome_mod_unload"
fi
stop_game

# --- run 6: safe mode -------------------------------------------------------------

echo "safe mode (sixth launch)"
rm -rf "$MODS/asset-map" "$MODS/asset-restart"
touch "$HOME_DIR/tome/safe"
# The Mods button, then row 0's toggle (hello) once the panel has lowered.
SAVED_MAP_ARG=("${MAP_ARG[@]}")
MAP_ARG=(-tomesafe)
TOME_DEBUG_CLICKS="wait:Main menu set up@0;0.874,0.84@4000;0.447,0.227@6500" launch
MAP_ARG=("${SAVED_MAP_ARG[@]}")
check "Safe mode \(-tomesafe, tome/safe\): no mod starts this session" "-tomesafe and tome/safe turn safe mode on" 30
check "Assets at startup: no mod assets" "no mod assets mounted"
check "hello +1\.0\.0 +disabled: safe mode" "mods are listed as disabled by safe mode" 15
check_absent "\[hello\] loaded" "no mod DLL loaded"
check "Mod menu: enabling hello" "the mod menu enables a mod despite safe mode" 15
check "\[hello\] enabled \(1 time" "and it starts"
if python3 -I -c "import json,sys; sys.exit(0 if json.load(open('$STATE'))['mods']['hello']['enabled'] else 1)"; then
    pass "state.json still has hello enabled"
else
    fail "state.json still has hello enabled"
fi
stop_game
rm -f "$HOME_DIR/tome/safe"

echo
if ((FAILED)); then
    echo "e2e: FAILED (log: $LOG)"
    exit 1
fi
echo "e2e: all checks passed"
