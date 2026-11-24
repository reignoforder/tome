# Tome

**Tome lets you add mods to Warcraft III**: Reign of Chaos and The Frozen
Throne. Drop a file into your game folder, start the game as usual, and a
**Mods** button appears on the main menu where you can turn mods on and off.

> **Unofficial.** Tome is a fan project, not affiliated with or endorsed by
> Blizzard Entertainment. It includes no game files: you need your own copy
> of Warcraft III. Tome changes the game while it runs (never its files),
> which online services may not allow, and mods with code can do anything a
> program on your computer can. Use it at your own risk: read
> [`DISCLAIMER.md`](DISCLAIMER.md).

**Status: early development.** See [`docs/ROADMAP.md`](docs/ROADMAP.md) for
what's coming next.

## Will it work with my game?

Tome works with **Warcraft III version 1.27b or 1.26a** only (you can see
your version at the bottom right of the main menu). Both Reign of Chaos and
The Frozen Throne are supported.

On any other version, Tome does nothing and the game runs exactly as it
would without it, so trying it is harmless.

It runs on Windows 7 or later, and on Linux through Wine. (Mods written in
Rust need Windows 10.)

## Installing

1. Download the latest `tome-<version>.zip` from the
   [releases page](https://github.com/reignoforder/tome/releases).
2. Unpack everything in it into your Warcraft III folder (the one with
   `war3.exe` in it).
3. Start the game as usual.

That's it. You'll know it worked when the window title says `[Tome ...]` and
the main menu has a **Mods** button.

Your folder will look like this:

```
Warcraft III/
  war3.exe
  tome.mix        Tome itself
  tome/
    mods/         put mods here
    tome.log      Tome's log, useful when something goes wrong
```

## Using mods

1. Put the mod into the `tome/mods/` folder. A mod is either a folder or a
   single `.tome` file; both work as they are, no unpacking needed.
2. Open the **Mods** menu (the button on the main menu, **Ctrl+F10**, or
   the Mods button in the in-game menu, F10).
3. Find the mod and click **Enable**.

**New mods start turned off** until you enable them yourself. That's on
purpose: a mod can run code on your computer, so only turn on mods from
people you trust. Each mod's details page tells you what it is, who signed
it and what it can do.

The Mods menu also lets you:

- turn mods on and off at any time, even while the game is running;
- change each mod's settings;
- see and change every mod's keyboard shortcuts;
- see what went wrong if a mod has a problem.

### Playing online with mods

Some mods change the game for everyone in a match. For those, every player
needs the same version of the mod: Tome checks this in the game lobby and
won't let the game start if the players' mods don't match. Mods that only
change things for you (like interface tweaks) don't need to match.

LAN games and replays are tested. Battle.net, private servers and games
over the internet haven't been tested yet.

## Updating

The first time you start the game with Tome, it asks whether it may check
for updates. If you say yes, it lets you know when a new version is out,
and **Update Now** installs it (it takes effect the next time you start the
game). Your mods and settings are kept. You can change this later in
**Mods > Tome > Settings**.

To update by hand, close the game and unpack the new zip into your
Warcraft III folder again.

## If something goes wrong

- **Start in safe mode:** hold **Shift** while the game starts (or start it
  with `-tomesafe`). All mods are turned off for that start, so you can
  disable the one causing trouble. If the game crashes three times in a row
  before reaching the main menu, Tome switches to safe mode by itself.
- **Turn off one of Tome's built-in fixes:** if you suspect one of them
  (they're listed under "Patches:" in `tome/tome.log`), turn it off in
  Tome > Settings ("Patch: <name>", from the next start), or for one start
  with `-tomenopatch=<name>` (several: `-tomenopatch=widescreen,blp-guard`;
  every one: `-tomenopatch=all`). Some of them close security holes that
  maps could use: turn those back on afterwards.
- **A new version won't start:** delete `tome.mix` and rename
  `tome.mix.old` to `tome.mix` to go back to the previous version.
- **Reporting a problem:** open an issue and attach `tome/tome.log`, plus
  the crash report from `tome/crashes/` if there is one. Say which game
  version and which mods you use.

Some things that don't work yet are listed under [Known issues](#known-issues).

## Uninstalling

Delete `tome.mix` from your Warcraft III folder. The game is back to how it
was, since Tome never changes the game's own files. To also remove your
mods, their settings and Tome's log, delete the `tome/` folder.

## Privacy

Tome makes no internet connections of its own unless you allow the update
check. If you do, it asks GitHub once a day for the latest version; nothing
about you or your mods is sent (GitHub sees your IP address, as with any
website). Nothing is downloaded unless you click Update Now (or turn on
Install Updates Automatically), and an update is only installed if it's
signed by Tome's developers.

In a multiplayer game, the other players see the names and versions of the
mods that the game requires (and the replay records them). Mods that are
only for you aren't shared. Logs and crash reports stay on your computer.

More details: [`DISCLAIMER.md`](DISCLAIMER.md).

## Known issues

- **Only versions 1.27b and 1.26a** are supported. On any other version
  Tome stays idle.
- **Tested mostly under Wine** so far; testing on Windows 10/11 is still
  to come.
- **Online play:** LAN games, hosting and replays work. Saved multiplayer
  games, joining someone else's game over the internet, Battle.net and
  private servers haven't been tested yet.
- **Chat commands** (`/tome ...` and mods' own) work during a game only,
  not in the lobby or Battle.net chat.
- **A disabled mod's interface** is hidden rather than removed; it goes
  away when you leave that screen.

## For mod makers and developers

- **Writing a mod:** start with the SDK guide,
  [`sdk/README.md`](sdk/README.md). It has templates and full examples in
  C, C++ and Rust, and the
  [`tome.mod.json` reference](sdk/docs/tome-mod-json.md). Mods can add
  their own settings pages, keyboard shortcuts, chat commands and new
  functions for map scripts, and they reload automatically when you rebuild
  them while the game runs.
- **Building Tome and running its tests:**
  [`docs/building.md`](docs/building.md).
- **Contributing:** [`CONTRIBUTING.md`](CONTRIBUTING.md).
- **How it works:** [`docs/architecture.md`](docs/architecture.md), and the
  game internals it relies on in
  [`docs/reverse-engineering.md`](docs/reverse-engineering.md).
- **Security:** [`SECURITY.md`](SECURITY.md) explains what mod approval
  does and doesn't protect against, and how to report a security problem.

## License

Tome is free software under the GNU GPL v3 or later (`LICENSE`). The SDK
in `sdk/` is under the MIT license (`sdk/LICENSE`), and what mods are built
from (templates, API headers, the Rust crates) under MIT No Attribution
(`sdk/LICENSE-MIT-0`), so mods may use any license. Tome also contains code
from other projects under their own licenses:
`loader/third_party/NOTICES.md`.

Tome is not affiliated with or endorsed by Blizzard Entertainment.
Warcraft is a trademark of Blizzard Entertainment, Inc.
