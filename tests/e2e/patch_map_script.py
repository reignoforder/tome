# SPDX-License-Identifier: GPL-3.0-or-later
"""Adds the e2e test's native declarations and calls to a map's war3map.j.

usage: python3 patch_map_script.py <war3map.j in> <war3map.j out> [--cpp] [--rust] [--wasm]

--cpp and --rust also declare and call the natives of the hello-cpp and
hello-rs examples (sdk/examples/hello-cpp, sdk/rust/examples/hello-rs), for
a test with those mods installed; --wasm those of hello-wasm
(sdk/examples/hello-wasm), whose results it logs as 8042-8046 and a greeting.

Natives are declared before the globals block, and the calls go at the
start of main() right after InitBlizzard(), which every melee map calls.
TomeBlizzardJLoaded goes before main(); the asset-demo mod's Blizzard.j
calls it (patch_blizzard_j.py). The melee victory check starts from a timer
instead of at once.
"""
import re
import sys

DECLARATIONS = """\
// Added by Tome's e2e test
native TomeIsLoaded takes nothing returns boolean
native TomeLogInteger takes integer value returns nothing
native TomeLogString takes string value returns nothing
native HelloLog takes integer value returns nothing
native HelloLogReal takes real value returns nothing
native HelloDouble takes real value returns real
native HelloGreet takes string name returns string
native HelloCall takes code callback returns boolean
native HelloRun takes code callback returns nothing
native HelloOnGameStart takes code callback returns nothing
native CrashNative takes integer value returns integer
native BlzLoadTOCFile takes string TOCFile returns boolean
native BlzGetFrameByName takes string name, integer createContext returns integer
native BlzCreateFrame takes string name, integer owner, integer priority, integer createContext returns integer
native BlzFrameSetPoint takes integer frame, integer point, integer relative, integer relativePoint, real x, real y returns nothing
native BlzFrameSetText takes integer frame, string text returns nothing
native BlzFrameSetVisible takes integer frame, boolean visible returns nothing
"""

GLOBALS = """\
    // Added by Tome's e2e test
    boolean tome_e2e_loaded = false
    boolean tome_e2e_load = false
    unit tome_e2e_paladin = null
    dialog tome_e2e_dialog = null
    trigger tome_e2e_dialog_trigger = null
    trigger tome_e2e_done_trigger = null
    boolean tome_e2e_map_done = false
    boolean tome_e2e_input_done = false
    trigger tome_e2e_arrow_trigger = null
    trigger tome_e2e_menu_trigger = null
    trigger tome_e2e_movie_trigger = null
    unit tome_e2e_shadow = null
    string tome_e2e_init_string = I2S(9700)
"""

CALLS = """\
    // Added by Tome's e2e test
    call HelloLog(42)
    call HelloLogReal(HelloDouble(1.25))
    if TomeIsLoaded() then
        call HelloLog(1001)
    endif
    // HelloDouble returns 0.0 while the hello mod is disabled.
    call TomeLogInteger(R2I(HelloDouble(2.0)))
    // Tome's frame natives. CinematicScenePanel exists in every game (hidden
    // outside cinematics); Tome.toc holds Tome's own frame definitions.
    if BlzLoadTOCFile("UI\\\\FrameDef\\\\Tome.toc") then
        call TomeLogInteger(7001)
    endif
    call TomeLogInteger(7100 + BlzGetFrameByName("TomeNoSuchFrame", 0))
    if BlzCreateFrame("TomeModsBackdrop", BlzGetFrameByName("CinematicScenePanel", 0), 0, 7) == BlzGetFrameByName("TomeModsBackdrop", 7) and BlzGetFrameByName("TomeModsBackdrop", 7) != 0 then
        call TomeLogInteger(7201)
    endif
    if BlzGetFrameByName("TomeModsButtonText", 7) != 0 then
        call TomeLogInteger(7301)
    endif
    call BlzFrameSetText(BlzGetFrameByName("TomeModsButtonText", 7), "From JASS")
    call BlzFrameSetPoint(BlzGetFrameByName("TomeModsBackdrop", 7), 4, BlzGetFrameByName("CinematicScenePanel", 0), 4, 0.0, 0.0)
    call BlzFrameSetVisible(BlzGetFrameByName("TomeModsBackdrop", 7), false)
    call TomeLogInteger(7400 + BlzCreateFrame("TomeModsBackdrop", 0, 0, 0))
    // The asset-demo mod ships this TOC and its FDF in its assets folder.
    if BlzLoadTOCFile("UI\\\\FrameDef\\\\TomeAssetDemo.toc") then
        call TomeLogInteger(7002)
    endif
    if BlzCreateFrame("TomeAssetDemoBackdrop", BlzGetFrameByName("CinematicScenePanel", 0), 0, 0) != 0 then
        call TomeLogInteger(7501)
    endif
    call TomeLogInteger(7999)
    // Strings: arguments, results (UTF-8), null.
    call TomeLogString(HelloGreet("Tome"))
    call TomeLogString(HelloGreet("ZEUS_GREEK"))
    call TomeLogString(HelloGreet(null))
    call TomeLogString(null)
    if HelloGreet("Tome") == "Hello, Tome! (4 letters)" then
        call TomeLogInteger(8001)
    endif
    call TomeLogInteger(8000 + StringLength(HelloGreet("ZEUS_GREEK")))
    // Code: called from inside a native, and later from the game start event.
    if HelloCall(function TomeE2ECallback) then
        call TomeLogInteger(8101)
    endif
    if not HelloCall(function TomeE2ECallbackNothing) then
        call TomeLogInteger(8201)
    endif
    call HelloOnGameStart(function TomeE2EGameStart)
    // Selection and pause events, once the starting units exist and before
    // the melee victory check (the map has no opponents). Not in one go: a
    // trigger's pause resets the UI, which clears the selection.
    call TimerStart(CreateTimer(), 1.0, false, function TomeE2ESelect)
    call TimerStart(CreateTimer(), 1.5, false, function TomeE2EPause)
    // Unit, item and saved game events; the melee victory (the map has no
    // opponents) waits until they're done. The saved game is only loaded in
    // the launch that starts with the hello mod disabled (HelloDouble
    // returns 0.0), which has no checks a second map load would disturb.
    set tome_e2e_load = HelloDouble(1.0) == 0.0
    call TimerStart(CreateTimer(), 2.0, false, function TomeE2EUnits)
    call TimerStart(CreateTimer(), 3.0, false, function TomeE2EMore)
    call TimerStart(CreateTimer(), 4.0, false, function TomeE2EUpgrade)
    call TimerStart(CreateTimer(), 9.0, false, function TomeE2ERevive)
    // With hello off, the save and load wait until the test has turned it
    // back on (its natives answer again), so it sees them however long that
    // took; with hello on, they run at 6 and 8 s.
    if tome_e2e_load then
        call TimerStart(CreateTimer(), 0.5, true, function TomeE2EWaitForHello)
    else
        call TimerStart(CreateTimer(), 6.0, false, function TomeE2ESave)
        call TimerStart(CreateTimer(), 8.0, false, function TomeE2ELoad)
    endif
    // The victory check (its dialog pauses the game and takes the keyboard)
    // starts once both are done: the map's own events above, which run on
    // the game's clock (deterministic, unlike the test's wall clock: 20 s of
    // game time covers them, revival and decay included), and the test's
    // input, which types "-e2e-done" last.
    call TimerStart(CreateTimer(), 20.0, false, function TomeE2EMapDone)
    set tome_e2e_done_trigger = CreateTrigger()
    call TriggerRegisterPlayerChatEvent(tome_e2e_done_trigger, Player(0), "-e2e-done", true)
    call TriggerAddAction(tome_e2e_done_trigger, function TomeE2EInputDone)
    // A dialog with a hotkey (T) on the chat line "-dialog", for runs with
    // simulated keys: a dialog pauses a single player game until it's clicked.
    set tome_e2e_dialog_trigger = CreateTrigger()
    call TriggerRegisterPlayerChatEvent(tome_e2e_dialog_trigger, Player(0), "-dialog", true)
    call TriggerAddAction(tome_e2e_dialog_trigger, function TomeE2EDialog)
    // The game only sends a player's arrow keys while a trigger listens.
    set tome_e2e_arrow_trigger = CreateTrigger()
    call TriggerRegisterPlayerEvent(tome_e2e_arrow_trigger, Player(0), EVENT_PLAYER_ARROW_LEFT_DOWN)
    call TriggerRegisterPlayerEvent(tome_e2e_arrow_trigger, Player(0), EVENT_PLAYER_ARROW_LEFT_UP)
    // "-build" and "-skill" select a new worker or hero alone, for a
    // simulated B (build menu) or O (hero skill menu).
    set tome_e2e_menu_trigger = CreateTrigger()
    call TriggerRegisterPlayerChatEvent(tome_e2e_menu_trigger, Player(0), "-build", true)
    call TriggerRegisterPlayerChatEvent(tome_e2e_menu_trigger, Player(0), "-skill", true)
    call TriggerAddAction(tome_e2e_menu_trigger, function TomeE2EMenuUnit)
    // "-movie" queues a movie, which plays when the map ends (Tome's movie
    // patches; TOME_DEBUG_SKIP_MOVIES skips it).
    set tome_e2e_movie_trigger = CreateTrigger()
    call TriggerRegisterPlayerChatEvent(tome_e2e_movie_trigger, Player(0), "-movie", true)
    call TriggerAddAction(tome_e2e_movie_trigger, function TomeE2EMovie)
    // Code that waits: HelloRun returns at the wait, twice on one trigger.
    call HelloRun(function TomeE2EWaits)
    call HelloRun(function TomeE2EWaits)
    call TomeLogInteger(8301)
    // The memhack guard: locals that shadow a global of another type
    // (TomeE2EShadow*). The script only compiles if each local keeps its
    // type up to its endfunction and the global gets its own back after.
    call TomeLogInteger(9500 + TomeE2EShadowReturn())
    call TomeE2EShadowSet()
    call TomeE2EShadowAfter()
    call TomeE2EPaths()
    call TomeE2EGuards()
    call TomeLogInteger(9000 + CrashNative(1))
    call TomeLogInteger(9100 + CrashNative(2))
    call TomeLogInteger(8999)
"""

ZEUS = "\u0396\u03b5\u03cd\u03c2"  # Ζεύς: 4 letters, 8 bytes in UTF-8

# The natives the hello-cpp and hello-rs examples share, with their prefix:
# integer, real, string and code values in C++ and Rust.
EXAMPLE_DECLARATIONS = """\
native {p}Log takes integer value returns nothing
native {p}Double takes real value returns real
native {p}Greet takes string name returns string
native {p}Call takes code callback returns boolean
"""

EXAMPLE_CALLS = """\
    call {p}Log(R2I({p}Double(21.0)))
    call TomeLogString({p}Greet("ZEUS_GREEK"))
    if {p}Call(function TomeE2ECallback) then
        call {p}Log(2002)
    endif
"""

EXAMPLES = {"--cpp": "HelloCpp", "--rust": "HelloRs"}

FUNCTIONS = """\
// Added by Tome's e2e test: called by the asset-demo mod's Blizzard.j
function TomeBlizzardJLoaded takes nothing returns nothing
    call TomeLogInteger(7600)
endfunction

// The memhack pattern, which the memhack-guard patch makes harmless: each
// local shadows the unit global tome_e2e_shadow with an integer. The
// function's last statement (just before endfunction) must still see the
// local, and TomeE2EShadowAfter the unit global.
function TomeE2EShadowReturn takes nothing returns integer
    local integer tome_e2e_shadow = 5
    set tome_e2e_shadow = tome_e2e_shadow + 1
    return tome_e2e_shadow
endfunction

function TomeE2EShadowSet takes nothing returns nothing
    local integer tome_e2e_shadow
    call TomeLogInteger(9507)
    set tome_e2e_shadow = 7
endfunction

function TomeE2EShadowAfter takes nothing returns nothing
    set tome_e2e_shadow = null
    if tome_e2e_shadow == null then
        call TomeLogInteger(9510)
    endif
endfunction

// The path-sanitizer patch: each name escapes the data folder, so each call
// is refused (logged) and returns 0. A refused InitGameCache gives a null
// gamecache, which the cache natives must take without crashing (9600 shows
// GetStoredInteger read 0 from it). 9609 shows the function ran to its end.
function TomeE2EPaths takes nothing returns nothing
    local gamecache cache = InitGameCache("..\\\\TomeE2E-escape.w3v")
    call StoreInteger(cache, "e2e", "x", 1)
    call TomeLogInteger(9600 + GetStoredInteger(cache, "e2e", "x"))
    if not SaveGameCache(cache) then
        call TomeLogInteger(9601)
    endif
    call FlushGameCache(cache)
    call PreloadGenClear()
    call Preload("e2e")
    call PreloadGenEnd("..\\\\TomeE2E-escape.txt")
    call SaveGame("..\\\\..\\\\TomeE2E-escape.w3z")
    if not CopySaveGame("TomeE2E.w3z", "..\\\\..\\\\TomeE2E-escape.w3z") then
        call TomeLogInteger(9602)
    endif
    if not RenameSaveDirectory("..\\\\TomeE2E-escape", "..\\\\TomeE2E-escape2") then
        call TomeLogInteger(9603)
    endif
    if not RemoveSaveDirectory("..\\\\TomeE2E-escape") then
        call TomeLogInteger(9604)
    endif
    // Natives that read a file by name are refused the same way, except that
    // an empty name (a special effect with no model) is fine.
    call Preloader("..\\\\TomeE2E-escape.pld")
    if not SaveGameExists("..\\\\..\\\\TomeE2E-escape.w3z") then
        call TomeLogInteger(9605)
    endif
    if AddSpecialEffect("C:\\\\TomeE2E-escape.mdx", 0.0, 0.0) == null then
        call TomeLogInteger(9606)
    endif
    if CreateSound("..\\\\TomeE2E-escape.wav", false, false, false, 10, 10, "") == null then
        call TomeLogInteger(9607)
    endif
    call StartMeleeAI(Player(1), "..\\\\TomeE2E-escape.ai")
    call PlayMusic("..\\\\TomeE2E-escape.mp3")
    call DestroyEffect(AddSpecialEffect("", 0.0, 0.0))
    if not BlzLoadTOCFile("..\\\\TomeE2E-escape.toc") then
        call TomeLogInteger(9608)
    endif
    // A PreloadGenEnd name too long to fit after the game's folder is refused
    // (the game would leave its path buffers unterminated, which can crash
    // it), though it is under kMaxPathLen. Preload's argument may be any
    // length: maps carry data in it.
    call Preload("LONGDATA_300")
    call PreloadGenEnd("LONGPATH_239")
    // Preload keeps any data, even one that isn't a safe path ("Gold: ..\\"),
    // but then the natives that load the preload list's files are refused
    // until PreloadGenClear empties it.
    call PreloadGenClear()
    call Preload("e2e save: ..\\\\Gold")
    call PreloadEnd(0.0)
    call PreloadGenClear()
    call PreloadRefresh()
    call TomeLogInteger(9609)
endfunction

// execute-func-guard and global-string-guard: ExecuteFunc on a function that
// takes parameters (the map's and Blizzard.j's) is refused instead of
// crashing (9790 never shows), and a string native in a global's initial
// value (tome_e2e_init_string) gave null instead of crashing the map load
// (9701). 9702 shows the function ran to its end.
function TomeE2ETakesArg takes integer i returns nothing
    call TomeLogInteger(9790 + i)
endfunction

function TomeE2EGuards takes nothing returns nothing
    if tome_e2e_init_string == null then
        call TomeLogInteger(9701)
    endif
    call ExecuteFunc("TomeE2ETakesArg")
    call ExecuteFunc("ClearTextMessagesBJ")
    call TomeLogInteger(9702)
endfunction

// Called by the hello mod through code arguments.
function TomeE2ECallback takes nothing returns boolean
    call TomeLogInteger(8100)
    return true
endfunction

function TomeE2ECallbackNothing takes nothing returns nothing
    call TomeLogInteger(8200)
endfunction

function TomeE2EWaits takes nothing returns nothing
    call TomeLogInteger(8300)
    call TriggerSleepAction(0.5)
    call TomeLogInteger(8302)
endfunction

// Selects Player 1's units (SelectUnit skips those not selectable) and logs
// how many got selected; the hello mod logs the selection event.
function TomeE2ESelect takes nothing returns nothing
    local group g = CreateGroup()
    local unit u
    local integer selected = 0
    call GroupEnumUnitsOfPlayer(g, Player(0), null)
    call ClearSelection()
    loop
        set u = FirstOfGroup(g)
        exitwhen u == null
        call GroupRemoveUnit(g, u)
        call SelectUnit(u, true)
        if IsUnitSelected(u, Player(0)) then
            set selected = selected + 1
        endif
    endloop
    call DestroyGroup(g)
    call TomeLogInteger(8400 + selected)
endfunction

// Pauses and resumes the game: the hello mod logs both events.
function TomeE2EPause takes nothing returns nothing
    call PauseGame(true)
    call PauseGame(false)
    call TomeLogInteger(8500)
endfunction

function TomeE2ETownHall takes nothing returns unit
    local group g = CreateGroup()
    local unit u
    call GroupEnumUnitsOfType(g, "townhall", null)
    set u = FirstOfGroup(g)
    call DestroyGroup(g)
    return u
endfunction

// Hero levels, a summon, items, a death with a killer, training and a
// building; warpten makes training and building quick.
function TomeE2EUnits takes nothing returns nothing
    local real x = GetStartLocationX(GetPlayerStartLocation(Player(0)))
    local real y = GetStartLocationY(GetPlayerStartLocation(Player(0)))
    local unit hero = CreateUnit(Player(0), 'Hamg', x, y - 300.0, 0.0)
    local unit footman = CreateUnit(Player(0), 'hfoo', x + 100.0, y - 300.0, 0.0)
    local unit peasant = CreateUnit(Player(0), 'hpea', x, y - 400.0, 0.0)
    local item potion
    call Cheat("warpten")
    call SetHeroLevel(hero, 3, true)
    call AddHeroXP(hero, 50, true)
    set potion = UnitAddItemById(hero, 'pinv')
    call UnitUseItem(hero, potion)
    call UnitRemoveItem(hero, UnitAddItemById(hero, 'stwp'))
    call UnitDamageTarget(hero, footman, 100000.0, true, false, ATTACK_TYPE_CHAOS, DAMAGE_TYPE_UNIVERSAL, WEAPON_TYPE_WHOKNOWS)
    // Last: using an item would replace the cast order.
    call SelectHeroSkill(hero, 'AHwe')
    call IssueImmediateOrder(hero, "waterelemental")
    call IssueImmediateOrderById(TomeE2ETownHall(), 'hpea')
    call IssueBuildOrderById(peasant, 'hhou', x - 500.0, y - 500.0)
    call TomeLogInteger(8600)
endfunction

// Training started and cancelled, an attack order (target order, attacked),
// a hero killed (revivable), a transport loaded, a unit given away.
function TomeE2EMore takes nothing returns nothing
    local real x = GetStartLocationX(GetPlayerStartLocation(Player(0)))
    local real y = GetStartLocationY(GetPlayerStartLocation(Player(0)))
    local unit attacker = CreateUnit(Player(0), 'hfoo', x + 300.0, y - 300.0, 0.0)
    local unit target = CreateUnit(Player(0), 'hpea', x + 350.0, y - 300.0, 0.0)
    local unit zeppelin = CreateUnit(Player(0), 'nzep', x - 300.0, y - 300.0, 0.0)
    local unit passenger = CreateUnit(Player(0), 'hfoo', x - 330.0, y - 300.0, 0.0)
    set tome_e2e_paladin = CreateUnit(Player(0), 'Hpal', x, y - 600.0, 0.0)
    call IssueImmediateOrderById(TomeE2ETownHall(), 'hpea')
    call IssueImmediateOrderById(TomeE2ETownHall(), 851976)
    call IssueTargetOrder(attacker, "attack", target)
    call IssueTargetOrder(zeppelin, "load", passenger)
    call SetUnitOwner(CreateUnit(Player(0), 'hfoo', x, y - 700.0, 0.0), Player(1), true)
    call KillUnit(tome_e2e_paladin)
    // A destructable and an item destroyed; a creep that acquires a target.
    call KillDestructable(CreateDestructable('LTlt', x - 700.0, y - 700.0, 0.0, 1.0, 0))
    call SetWidgetLife(CreateItem('ratf', x - 600.0, y - 700.0), 0.0)
    call CreateUnit(Player(PLAYER_NEUTRAL_AGGRESSIVE), 'ngno', x + 800.0, y - 800.0, 0.0)
    call CreateUnit(Player(0), 'hfoo', x + 750.0, y - 800.0, 0.0)
    // A unit created and removed, gold set (nothing else spends in this
    // tick), and the time of day set just before dusk: night falls a
    // moment later, and TomeE2EUpgrade sets it back to noon.
    call RemoveUnit(CreateUnit(Player(0), 'hrif', x - 800.0, y - 300.0, 0.0))
    call SetPlayerState(Player(0), PLAYER_STATE_RESOURCE_GOLD, 777)
    call SetFloatGameState(GAME_STATE_TIME_OF_DAY, 17.98)
    call TomeLogInteger(8606)
endfunction

function TomeE2EMenuUnit takes nothing returns nothing
    local real x = GetStartLocationX(GetPlayerStartLocation(Player(0)))
    local real y = GetStartLocationY(GetPlayerStartLocation(Player(0)))
    local unit u
    if GetEventPlayerChatString() == "-build" then
        set u = CreateUnit(Player(0), 'hpea', x - 200.0, y - 450.0, 0.0)
    else
        // A hero in Reign of Chaos too (the Blood Mage isn't): EDITION=roc.
        set u = CreateUnit(Player(0), 'Hmkg', x - 300.0, y - 450.0, 0.0)
    endif
    call ClearSelection()
    call SelectUnit(u, true)
    // What the e2e test's input waits for before the menu's hotkey.
    if GetEventPlayerChatString() == "-build" then
        call TomeLogString("e2e: selected the worker")
    else
        call TomeLogString("e2e: selected the hero")
    endif
endfunction

function TomeE2EMovie takes nothing returns nothing
    call PlayCinematic("TutorialOp")
    call TomeLogString("e2e: movie queued")
endfunction

function TomeE2EDialog takes nothing returns nothing
    set tome_e2e_dialog = DialogCreate()
    call DialogSetMessage(tome_e2e_dialog, "Tome")
    call DialogAddButton(tome_e2e_dialog, "Test", 'T')
    call DialogDisplay(Player(0), tome_e2e_dialog, true)
    call TomeLogString("e2e: dialog shown")
endfunction

// The Paladin killed at 3 s (revivable once its death animation ends),
// revived at a new altar; warpten makes it quick.
function TomeE2ERevive takes nothing returns nothing
    local real x = GetStartLocationX(GetPlayerStartLocation(Player(0)))
    local real y = GetStartLocationY(GetPlayerStartLocation(Player(0)))
    local unit altar = CreateUnit(Player(0), 'halt', x + 600.0, y + 600.0, 0.0)
    if IssueImmediateOrderById(altar, 'Hpal') then
        call TomeLogInteger(8607)
    elseif ReviveHero(tome_e2e_paladin, x, y - 600.0, false) then
        call TomeLogInteger(8608)
    endif
endfunction

function TomeE2EUpgrade takes nothing returns nothing
    call SetFloatGameState(GAME_STATE_TIME_OF_DAY, 12.0)
    call SetPlayerState(Player(0), PLAYER_STATE_RESOURCE_LUMBER, 1000)
    call SetPlayerState(Player(0), PLAYER_STATE_RESOURCE_GOLD, 1000)
    if IssueImmediateOrderById(TomeE2ETownHall(), 'hkee') then
        call TomeLogInteger(8601)
    else
        call TomeLogInteger(8605)
    endif
endfunction

// The saved game has tome_e2e_loaded set, this session doesn't: the load
// timer, restored with the save, then doesn't load again.
function TomeE2ESave takes nothing returns nothing
    set tome_e2e_loaded = true
    call SaveGame("TomeE2E.w3z")
    set tome_e2e_loaded = false
    call TomeLogInteger(8602)
endfunction

// Loads the game saved 2 s earlier: a saved game load inside the session.
// The timers come back with it.
function TomeE2ELoad takes nothing returns nothing
    if not tome_e2e_load then
        return
    endif
    if tome_e2e_loaded then
        call TomeLogInteger(8604)
        return
    endif
    call TomeLogInteger(8603)
    call LoadGame("TomeE2E.w3z", false)
endfunction

function TomeE2EMaybeVictory takes nothing returns nothing
    if tome_e2e_map_done and tome_e2e_input_done then
        call MeleeInitVictoryDefeat()
    endif
endfunction

function TomeE2EMapDone takes nothing returns nothing
    set tome_e2e_map_done = true
    call TomeE2EMaybeVictory()
endfunction

function TomeE2EInputDone takes nothing returns nothing
    set tome_e2e_input_done = true
    call TomeE2EMaybeVictory()
endfunction

// Defined after TomeE2ESave and TomeE2ELoad: JASS needs a function before its use.
function TomeE2EWaitForHello takes nothing returns nothing
    if HelloDouble(1.0) == 0.0 then
        return
    endif
    call PauseTimer(GetExpiredTimer())
    call TomeLogString("e2e: hello is back; saving and loading")
    call TimerStart(CreateTimer(), 1.0, false, function TomeE2ESave)
    call TimerStart(CreateTimer(), 3.0, false, function TomeE2ELoad)
endfunction

function TomeE2EGameStart takes nothing returns boolean
    call TomeLogString(HelloGreet("game start"))
    return GetPlayerName(Player(0)) != null
endfunction

"""


# The WebAssembly example's natives (sdk/examples/hello-wasm): integers, a
# real, a string both ways (UTF-8), and a handle result.
WASM_DECLARATIONS = """\
native HelloWasmAdd takes integer a, integer b returns integer
native HelloWasmHalf takes real value returns real
native HelloWasmGreet takes string name returns string
native HelloWasmSame takes unit u returns unit
native HelloWasmCall takes code callback returns boolean
"""

# A map function a Wasm native runs (jass_code_call), which calls a native of
# the same mod: a nested call into the module.
WASM_FUNCTIONS = """\
function TomeE2EWasmInner takes nothing returns boolean
    call TomeLogInteger(HelloWasmAdd(8045, 0))
    return true
endfunction

"""

WASM_CALLS = """\
    call TomeLogInteger(HelloWasmAdd(8000, 42))
    call TomeLogInteger(R2I(HelloWasmHalf(16086.0)))
    call TomeLogString(HelloWasmGreet("ZEUS_GREEK"))
    if HelloWasmSame(null) == null then
        call TomeLogInteger(8044)
    endif
    if HelloWasmCall(function TomeE2EWasmInner) then
        call TomeLogInteger(8046)
    endif
"""


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    flags = sys.argv[3:]
    wasm = "--wasm" in flags
    prefixes = [EXAMPLES[flag] for flag in flags if flag in EXAMPLES]
    declarations = DECLARATIONS + "".join(EXAMPLE_DECLARATIONS.format(p=p) for p in prefixes)
    if wasm:
        declarations += WASM_DECLARATIONS
    text = open(src, encoding="latin-1", newline="").read()
    newline = "\r\n" if "\r\n" in text else "\n"

    # The map has no opponents, so melee would declare victory at once (and
    # its dialog pauses the game); a timer in CALLS starts it later.
    text, n = re.subn(r"^(\s*)(call MeleeInitVictoryDefeat\(\s*\))", r"\1// \2", text, flags=re.M)
    if n != 1:
        print("expected one call MeleeInitVictoryDefeat()", file=sys.stderr)
        return 1

    globals_at = re.search(r"^globals\b", text, re.M)
    if not globals_at:
        print("no globals block found", file=sys.stderr)
        return 1
    text = text[: globals_at.start()] + declarations.replace("\n", newline) + text[globals_at.start():]
    globals_line = re.search(r"^globals\b[^\n]*\n", text, re.M)
    text = text[: globals_line.end()] + GLOBALS.replace("\n", newline) + text[globals_line.end():]

    main_at = re.search(r"^function main takes nothing returns nothing", text, re.M)
    if not main_at:
        print("no main function found", file=sys.stderr)
        return 1
    functions = FUNCTIONS + (WASM_FUNCTIONS if wasm else "")
    # A 239-character name ("longpath\" and 230 'a's): under kMaxPathLen, but
    # over PreloadGenEnd's limit (at most 238, with the shortest game folder,
    # "C:\"), so the path sanitizer refuses it. And a 307-character Preload
    # argument, which it lets through. (A string literal of ~4000 characters
    # would crash the JASS compiler: docs/known-bugs.md.)
    functions = functions.replace("LONGPATH_239", "longpath\\\\" + "a" * 230)
    functions = functions.replace("LONGDATA_300", "e2edata" + "x" * 300)
    text = text[: main_at.start()] + functions.replace("\n", newline) + text[main_at.start():]

    init_call = re.search(r"^function main takes nothing returns nothing.*?^(\s*call InitBlizzard\(\s*\)[^\n]*\n)",
                          text, re.M | re.S)
    if not init_call:
        print("no call InitBlizzard() in main found", file=sys.stderr)
        return 1
    # The script is read as latin-1, so UTF-8 goes in as its bytes.
    calls = CALLS + "".join(EXAMPLE_CALLS.format(p=p) for p in prefixes) + (WASM_CALLS if wasm else "")
    calls = calls.replace("ZEUS_GREEK", ZEUS.encode("utf-8").decode("latin-1"))
    text = text[: init_call.end(1)] + calls.replace("\n", newline) + text[init_call.end(1):]

    open(dst, "w", encoding="latin-1", newline="").write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
