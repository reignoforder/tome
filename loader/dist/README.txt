Tome @TOME_VERSION@: a mod loader for Warcraft III: The Frozen Throne 1.27b

Tome is unofficial and has nothing to do with Blizzard Entertainment.
Read DISCLAIMER.txt (next to this file) before you use it.

NEEDS

- Warcraft III: The Frozen Throne at version 1.27b (game.dll 1.27.1.7085;
  the version is on the main menu, bottom right). On any other version Tome
  logs it and does nothing: the game runs as it would without it.
- Windows 7 or later, or Wine. Mods written in Rust need Windows 10.

INSTALL

Unpack everything in this zip into your Warcraft III folder, the one with
war3.exe in it, so that you get:

  Warcraft III\
    war3.exe
    tome.mix       Tome itself
    tome\          README.txt, DISCLAIMER.txt, licenses\
      mods\        put mods here

Start the game as usual. The window title says [Tome @TOME_VERSION@] and the
main menu has a Mods button. Tome writes its log to tome\tome.log.

MODS

A mod is a folder or a .tome file: put it in tome\mods\. A new mod stays off
until you turn it on in the Mods menu (Mods on the main menu, Ctrl+F10, or
in a game the menu's Mods button). Its details page says what it is, who
signed it and what it can do. A mod with code (a DLL) can do anything a
program on your computer can: only turn on mods from people you trust.

In a multiplayer game, every player needs the same version of each mod
that the game requires; Tome compares them in the lobby and the game, so
the other players see those mods' names and versions, and the game's
replay records them. Nothing else is sent: Tome makes no connections of
its own, except the update check if you allow it (DISCLAIMER.txt).

UPDATES

On the first start Tome asks whether it may check for updates. If you say
yes, it asks GitHub once a day for its latest release and tells you when
there's a newer one. Update Now downloads it, checks it against the
release's signature and installs it; it runs from the next start, and your
mods and settings stay. If the new version fails to start twice in a row,
the start after that puts the old one back, and the old one runs from the
next start. Change the answer in Mods > Tome > Settings; there,
Install Updates Automatically (off unless you turn it on) installs a new
version as soon as it's found, without asking, from the next start.

Updating by hand works too: close the game and unpack the new zip into the
Warcraft III folder as above. If a new tome.mix ever stops the game from
starting, delete tome.mix and rename tome.mix.old to tome.mix.

IF SOMETHING GOES WRONG

- Hold Shift while the game starts, or start it with -tomesafe, for safe
  mode: Tome runs with every mod off. After three starts in a row that
  crash before the main menu, Tome starts in safe mode by itself.
- Crash reports go into tome\crashes\. When reporting a problem, include
  tome\tome.log and the crash report, and say which mods you use.

UNINSTALL

Delete tome.mix: the game is as it was before, since Tome changes none of
its files. Delete the tome\ folder too to remove your mods, their settings
and Tome's log.

LICENSE

Tome is free software under the GNU GPL v3 or later (licenses\GPL-3.0.txt),
with code from other projects under their own licenses
(licenses\NOTICES.md). Source: https://github.com/reignoforder/tome
