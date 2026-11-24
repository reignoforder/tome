# SPDX-License-Identifier: GPL-3.0-or-later
"""Adds chat-command scenarios to a melee map's war3map.j, so every piece of
UI a map can raise can be put on screen on demand and photographed
(tools/ui-shots.sh map builds the map; docs/building.md, "Looking at the UI").

usage: python3 -I tools/showcase-map.py <war3map.j in> <war3map.j out>

Commands (typed in the game's chat; each logs "[map] TomeLogInteger(<n>)" when
it's done, which a script waits for with wait:TomeLogInteger(<n>)):

  -hero 9001      a level 4 Paladin with items, footmen, a peasant and a knight
  -sel 9002       selects every unit that isn't a building
  -mb 9003        a multiboard           -lb 9004   a leaderboard
  -timer 9005     two timer dialogs      -quest 9006   quests and quest messages
  -dlg 9007       a script dialog (pauses the game)
  -text 9008      game messages          -float 9009   floating text, a ping
  -cine 9010      cinematic mode with a transmission; timers then fade red (9012),
                  fade with a mask (9013), switch the fade off (9014) and leave
                  cinematic mode (9011)  (chat is off in cinematic mode)
  -fade 9012  -mask 9013  -fadeoff 9014  -cineoff 9011
  -hide 9015      hides the interface    -show 9016   shows it again
  -win / -lose    the victory or defeat dialog and, after it, the score screen
  -movie 9020     queues a movie, which plays when the mission ends
  -cam 9021       zooms the camera out   -line 9030  a grid of footmen
  -peasant 9040   a peasant, selected (B opens the build menu)

The melee victory check is commented out (the map has no opponents, so it
would declare victory at once). Needs the natives of Tome (TomeLogInteger).
"""
import re
import sys

DECL = """\
// Showcase
native TomeLogInteger takes integer value returns nothing
"""

FUNCS = r"""
// ---- showcase ----
function ScMark takes integer n returns nothing
    call TomeLogInteger(n)
endfunction

function ScHero takes nothing returns nothing
    local unit u = CreateUnit(Player(0), 'Hpal', 0.0, 0.0, 270.0)
    local unit f1 = CreateUnit(Player(0), 'hfoo', 150.0, 0.0, 270.0)
    local unit f2 = CreateUnit(Player(0), 'hfoo', -150.0, 0.0, 270.0)
    local unit p = CreateUnit(Player(0), 'hpea', 0.0, -200.0, 270.0)
    local unit k = CreateUnit(Player(0), 'hkni', 200.0, -150.0, 270.0)
    call SetHeroLevel(u, 4, false)
    call UnitAddItemById(u, 'ratf')
    call UnitAddItemById(u, 'rde1')
    call UnitAddItemById(u, 'pghe')
    call PanCameraToTimed(0.0, 0.0, 0.0)
    call ClearSelection()
    call SelectUnit(u, true)
    call ScMark(9001)
endfunction

function ScSelect takes nothing returns nothing
    local group g = CreateGroup()
    local unit u
    call GroupEnumUnitsOfPlayer(g, Player(0), null)
    call ClearSelection()
    loop
        set u = FirstOfGroup(g)
        exitwhen u == null
        call GroupRemoveUnit(g, u)
        if IsUnitType(u, UNIT_TYPE_STRUCTURE) == false then
            call SelectUnit(u, true)
        endif
    endloop
    call DestroyGroup(g)
    call ScMark(9002)
endfunction

function ScMultiboard takes nothing returns nothing
    local multiboard mb = CreateMultiboardBJ(4, 5, "Multiboard Title")
    local integer r = 1
    loop
        exitwhen r > 5
        call MultiboardSetItemValueBJ(mb, 1, r, "Player " + I2S(r))
        call MultiboardSetItemValueBJ(mb, 2, r, I2S(r * 100))
        call MultiboardSetItemValueBJ(mb, 3, r, "Kills " + I2S(r))
        call MultiboardSetItemValueBJ(mb, 4, r, "Gold")
        call MultiboardSetItemWidthBJ(mb, 1, r, 8.0)
        call MultiboardSetItemWidthBJ(mb, 2, r, 5.0)
        call MultiboardSetItemWidthBJ(mb, 3, r, 6.0)
        call MultiboardSetItemWidthBJ(mb, 4, r, 4.0)
        set r = r + 1
    endloop
    call MultiboardDisplay(mb, true)
    call ScMark(9003)
endfunction

function ScLeaderboard takes nothing returns nothing
    local leaderboard lb = CreateLeaderboard()
    call LeaderboardSetLabel(lb, "Leaderboard")
    call LeaderboardAddItem(lb, "Alpha", 10, Player(0))
    call LeaderboardAddItem(lb, "Bravo", 25, Player(1))
    call LeaderboardAddItem(lb, "Charlie", 3, Player(2))
    call PlayerSetLeaderboard(Player(0), lb)
    call LeaderboardDisplay(lb, true)
    call ScMark(9004)
endfunction

function ScTimer takes nothing returns nothing
    local timer t = CreateTimer()
    local timerdialog d
    call TimerStart(t, 300.0, false, null)
    set d = CreateTimerDialog(t)
    call TimerDialogSetTitle(d, "Next Wave")
    call TimerDialogDisplay(d, true)
    set t = CreateTimer()
    call TimerStart(t, 120.0, false, null)
    set d = CreateTimerDialog(t)
    call TimerDialogSetTitle(d, "Boss Spawns")
    call TimerDialogDisplay(d, true)
    call ScMark(9005)
endfunction

function ScQuest takes nothing returns nothing
    local quest q
    set q = CreateQuestBJ(bj_QUESTTYPE_REQ_DISCOVERED, "Required Quest", "Defeat the enemy. Description text that is long enough to wrap onto more than one line so the quest panel shows its layout properly.", "ReplaceableTextures\\CommandButtons\\BTNHeroPaladin.blp")
    call CreateQuestItemBJ(q, "Kill ten footmen")
    call CreateQuestItemBJ(q, "Build a town hall")
    set q = CreateQuestBJ(bj_QUESTTYPE_OPT_DISCOVERED, "Optional Quest", "Find the hidden treasure.", "ReplaceableTextures\\CommandButtons\\BTNChestOfGold.blp")
    call CreateQuestItemBJ(q, "Search the island")
    call QuestMessageBJ(bj_FORCE_ALL_PLAYERS, bj_QUESTMESSAGE_DISCOVERED, "Quest discovered: Required Quest")
    call QuestMessageBJ(bj_FORCE_ALL_PLAYERS, bj_QUESTMESSAGE_UPDATED, "Quest updated: Required Quest")
    call QuestMessageBJ(bj_FORCE_ALL_PLAYERS, bj_QUESTMESSAGE_COMPLETED, "Quest completed: Optional Quest")
    call QuestMessageBJ(bj_FORCE_ALL_PLAYERS, bj_QUESTMESSAGE_WARNING, "Warning: the enemy approaches")
    call ScMark(9006)
endfunction

function ScDialogClick takes nothing returns nothing
    call ScMark(9099)
endfunction

function ScDialog takes nothing returns nothing
    local dialog d = DialogCreate()
    local trigger t = CreateTrigger()
    local button b
    call DialogSetMessage(d, "Choose your fate, hero of the Alliance. This is a script dialog.")
    set b = DialogAddButton(d, "First option", 49)
    set b = DialogAddButton(d, "Second option", 50)
    set b = DialogAddButton(d, "Third option", 51)
    call TriggerRegisterDialogEvent(t, d)
    call TriggerAddAction(t, function ScDialogClick)
    call DialogDisplay(Player(0), d, true)
    call ScMark(9007)
endfunction

function ScText takes nothing returns nothing
    local integer i = 0
    loop
        exitwhen i > 7
        call DisplayTimedTextToPlayer(Player(0), 0.0, 0.0, 120.0, "Game message line " + I2S(i) + ": the quick brown fox jumps over the lazy dog")
        set i = i + 1
    endloop
    call DisplayTextToPlayer(Player(0), 0.0, 0.0, "|cffffcc00Gold text|r and normal text, untimed")
    call ScMark(9008)
endfunction

function ScFloating takes nothing returns nothing
    local texttag tt = CreateTextTag()
    call SetTextTagText(tt, "+250 gold", 0.03)
    call SetTextTagPos(tt, 0.0, 0.0, 100.0)
    call SetTextTagColor(tt, 255, 220, 0, 255)
    call SetTextTagPermanent(tt, true)
    call PingMinimapEx(0.0, 0.0, 30.0, 255, 0, 0, true)
    call ScMark(9009)
endfunction

function ScCineOff takes nothing returns nothing
    call CinematicModeBJ(false, bj_FORCE_ALL_PLAYERS)
    call ScMark(9011)
endfunction

function ScFadeRed takes nothing returns nothing
    call SetCineFilterTexture("ReplaceableTextures\\CameraMasks\\White_mask.blp")
    call SetCineFilterBlendMode(BLEND_MODE_BLEND)
    call SetCineFilterTexMapFlags(TEXMAP_FLAG_NONE)
    call SetCineFilterStartUV(0, 0, 1, 1)
    call SetCineFilterEndUV(0, 0, 1, 1)
    call SetCineFilterStartColor(255, 0, 0, 150)
    call SetCineFilterEndColor(255, 0, 0, 150)
    call SetCineFilterDuration(100.0)
    call DisplayCineFilter(true)
    call ScMark(9012)
endfunction

function ScFadeMask takes nothing returns nothing
    call SetCineFilterTexture("ReplaceableTextures\\CameraMasks\\DiagonalSlash_mask.blp")
    call SetCineFilterBlendMode(BLEND_MODE_BLEND)
    call SetCineFilterTexMapFlags(TEXMAP_FLAG_NONE)
    call SetCineFilterStartUV(0, 0, 1, 1)
    call SetCineFilterEndUV(0, 0, 1, 1)
    call SetCineFilterStartColor(0, 0, 255, 255)
    call SetCineFilterEndColor(0, 0, 255, 255)
    call SetCineFilterDuration(100.0)
    call DisplayCineFilter(true)
    call ScMark(9013)
endfunction

function ScFadeOff takes nothing returns nothing
    call DisplayCineFilter(false)
    call ScMark(9014)
endfunction

function ScCineOn takes nothing returns nothing
    local unit u = CreateUnit(Player(0), 'Hpal', 0.0, 0.0, 270.0)
    call CinematicModeBJ(true, bj_FORCE_ALL_PLAYERS)
    call TransmissionFromUnitWithNameBJ(bj_FORCE_ALL_PLAYERS, u, "Uther", null, "Transmission with a portrait. The text of the dialogue wraps across the panel and shows the layout.", bj_TIMETYPE_SET, 120.0, false)
    call ScMark(9010)
    call TimerStart(CreateTimer(), 6.0, false, function ScFadeRed)
    call TimerStart(CreateTimer(), 12.0, false, function ScFadeOff)
    call TimerStart(CreateTimer(), 12.5, false, function ScFadeMask)
    call TimerStart(CreateTimer(), 18.0, false, function ScFadeOff)
    call TimerStart(CreateTimer(), 19.0, false, function ScCineOff)
endfunction

function ScHideUI takes nothing returns nothing
    call ShowInterface(false, 0.0)
    call ScMark(9015)
endfunction

function ScShowUI takes nothing returns nothing
    call ShowInterface(true, 0.0)
    call ScMark(9016)
endfunction

function ScWin takes nothing returns nothing
    call CustomVictoryBJ(Player(0), true, true)
endfunction

function ScLose takes nothing returns nothing
    call CustomDefeatBJ(Player(0), "You were defeated")
endfunction

function ScMovie takes nothing returns nothing
    call PlayCinematic("TutorialOp")
    call ScMark(9020)
endfunction

function ScMovieEnd takes nothing returns nothing
    call EndGame(true)
endfunction

function ScCamera takes nothing returns nothing
    call SetCameraField(CAMERA_FIELD_TARGET_DISTANCE, 3000.0, 0.0)
    call SetCameraField(CAMERA_FIELD_ANGLE_OF_ATTACK, 304.0, 0.0)
    call ScMark(9021)
endfunction

function ScLine takes nothing returns nothing
    local integer i = -10
    local integer j
    loop
        exitwhen i > 10
        set j = -3
        loop
            exitwhen j > 3
            call CreateUnit(Player(0), 'hfoo', I2R(i) * 300.0, I2R(j) * 300.0, 270.0)
            set j = j + 1
        endloop
        set i = i + 1
    endloop
    call PanCameraToTimed(0.0, 0.0, 0.0)
    call ScMark(9030)
endfunction

function ScPeasant takes nothing returns nothing
    local unit p = CreateUnit(Player(0), 'hpea', 0.0, -200.0, 270.0)
    call ClearSelection()
    call SelectUnit(p, true)
    call ScMark(9040)
endfunction

function ScReg takes string cmd, code c returns nothing
    local trigger t = CreateTrigger()
    call TriggerRegisterPlayerChatEvent(t, Player(0), cmd, true)
    call TriggerAddAction(t, c)
endfunction

function ScInit takes nothing returns nothing
    call ScReg("-hero", function ScHero)
    call ScReg("-sel", function ScSelect)
    call ScReg("-mb", function ScMultiboard)
    call ScReg("-lb", function ScLeaderboard)
    call ScReg("-timer", function ScTimer)
    call ScReg("-quest", function ScQuest)
    call ScReg("-dlg", function ScDialog)
    call ScReg("-text", function ScText)
    call ScReg("-float", function ScFloating)
    call ScReg("-cine", function ScCineOn)
    call ScReg("-cineoff", function ScCineOff)
    call ScReg("-fade", function ScFadeRed)
    call ScReg("-mask", function ScFadeMask)
    call ScReg("-fadeoff", function ScFadeOff)
    call ScReg("-hide", function ScHideUI)
    call ScReg("-show", function ScShowUI)
    call ScReg("-win", function ScWin)
    call ScReg("-lose", function ScLose)
    call ScReg("-movie", function ScMovie)
    call ScReg("-cam", function ScCamera)
    call ScReg("-line", function ScLine)
    call ScReg("-peasant", function ScPeasant)
    call ScMark(9000)
endfunction
"""

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("usage:")[1].split("\n")[0].strip() and "usage: python3 -I tools/showcase-map.py <war3map.j in> <war3map.j out>")
    text = open(sys.argv[1], encoding="latin-1", newline="").read()
    nl = "\r\n" if "\r\n" in text else "\n"
    text, n = re.subn(r"^(\s*)(call MeleeInitVictoryDefeat\(\s*\))", r"\1// \2", text, flags=re.M)
    if n != 1:
        sys.exit("expected one MeleeInitVictoryDefeat")
    g = re.search(r"^globals\b", text, re.M)
    text = text[: g.start()] + DECL.replace("\n", nl) + text[g.start():]
    m = re.search(r"^function main takes nothing returns nothing", text, re.M)
    text = text[: m.start()] + FUNCS.replace("\n", nl) + text[m.start():]
    init = re.search(r"^function main takes nothing returns nothing.*?^(\s*call InitBlizzard\(\s*\)[^\n]*\n)", text, re.M | re.S)
    text = text[: init.end(1)] + "    call ScInit()" + nl + text[init.end(1):]
    open(sys.argv[2], "w", encoding="latin-1", newline="").write(text)

if __name__ == "__main__":
    main()
