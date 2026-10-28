"""Adds the e2e test's native declarations and calls to a map's war3map.j.

usage: python3 patch_map_script.py <war3map.j in> <war3map.j out>

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
    trigger tome_e2e_arrow_trigger = null
    trigger tome_e2e_menu_trigger = null
    unit tome_e2e_shadow = null
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
    call TimerStart(CreateTimer(), 6.0, false, function TomeE2ESave)
    call TimerStart(CreateTimer(), 8.0, false, function TomeE2ELoad)
    call TimerStart(CreateTimer(), 12.0, false, function MeleeInitVictoryDefeat)
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
    call TomeLogInteger(9000 + CrashNative(1))
    call TomeLogInteger(9100 + CrashNative(2))
    call TomeLogInteger(8999)
"""

ZEUS = "\u0396\u03b5\u03cd\u03c2"  # Ζεύς: 4 letters, 8 bytes in UTF-8

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
        set u = CreateUnit(Player(0), 'Hblm', x - 300.0, y - 450.0, 0.0)
    endif
    call ClearSelection()
    call SelectUnit(u, true)
endfunction

function TomeE2EDialog takes nothing returns nothing
    set tome_e2e_dialog = DialogCreate()
    call DialogSetMessage(tome_e2e_dialog, "Tome")
    call DialogAddButton(tome_e2e_dialog, "Test", 'T')
    call DialogDisplay(Player(0), tome_e2e_dialog, true)
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

// Loads the game saved at 6 s: a saved game load inside the session. The
// timers come back with it, so the victory check still starts at 12 s.
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

function TomeE2EGameStart takes nothing returns boolean
    call TomeLogString(HelloGreet("game start"))
    return GetPlayerName(Player(0)) != null
endfunction

"""


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
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
    text = text[: globals_at.start()] + DECLARATIONS.replace("\n", newline) + text[globals_at.start():]
    globals_line = re.search(r"^globals\b[^\n]*\n", text, re.M)
    text = text[: globals_line.end()] + GLOBALS.replace("\n", newline) + text[globals_line.end():]

    main_at = re.search(r"^function main takes nothing returns nothing", text, re.M)
    if not main_at:
        print("no main function found", file=sys.stderr)
        return 1
    text = text[: main_at.start()] + FUNCTIONS.replace("\n", newline) + text[main_at.start():]

    init_call = re.search(r"^function main takes nothing returns nothing.*?^(\s*call InitBlizzard\(\s*\)[^\n]*\n)",
                          text, re.M | re.S)
    if not init_call:
        print("no call InitBlizzard() in main found", file=sys.stderr)
        return 1
    # The script is read as latin-1, so UTF-8 goes in as its bytes.
    calls = CALLS.replace("ZEUS_GREEK", ZEUS.encode("utf-8").decode("latin-1"))
    text = text[: init_call.end(1)] + calls.replace("\n", newline) + text[init_call.end(1):]

    open(dst, "w", encoding="latin-1", newline="").write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
