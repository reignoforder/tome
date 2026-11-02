# input/

Put the files of a Warcraft III **1.27b** install here (game.dll
1.27.1.7085): `war3.exe`, `game.dll`, `blizzard.ax`, the `.mpq` archives,
`Maps/` and the rest, so that `input/war3.exe` exists. A link to such a
folder works too, in place of this one (git ignores the link; it then shows
this file as deleted):

```sh
rm input/README.md && rmdir input && ln -s /path/to/your/game input
```

(`ln -s` with this folder still there would make the link inside it.)

Only the tests and the developer tools read this folder; nothing writes
into it. `make test` runs the tests on `game.dll` from here (they skip
themselves without it), and `make e2e` and `make game` run the game from a
folder of links to these files with the build's `tome.mix`.

Everything here except this file is ignored by git. Game files are
Blizzard's: never commit them.

To install a build into a game you play, copy `build/loader/tome.mix` into
that game's folder, or `make deploy INSTALL_DIR=/path/to/it` (never this
folder).
