# input/

One folder per game version, named after it, holding the files of that
version's install: `war3.exe`, `game.dll`, `blizzard.ax`, the `.mpq`
archives, `Maps/` and the rest, so that `input/<version>/war3.exe` exists.
The tests' default is **1.27b** (game.dll 1.27.1.7085), in `input/1.27b/`:

```
input/1.27b/war3.exe
input/1.27b/game.dll
input/1.26a/...
```

A version folder may be a link to an install elsewhere:

```sh
ln -s /path/to/your/1.27b/game input/1.27b
```

Name the version after the target to use another one: `make test 1.26a`,
`make e2e 1.26a`, `make game 1.26a` (or `GAME_VERSION=1.26a`; the scripts
in `tools/` read `GAME_VERSION` too). A version named that isn't here
stops `make` with an error; without the default one, the tests skip what
needs the game. The tests also skip a `game.dll` Tome has no mappings for
(`mappings/<build>.json`): a new version folder is for building them.

Only the tests and the developer tools read these folders; nothing writes
into them. `make test` runs the tests on the version's `game.dll` (they
skip themselves without it), and `make e2e` and `make game` run the game
from a folder of links to its files with the build's `tome.mix`.

Everything here except this file is ignored by git. Game files are
Blizzard's: never commit them.

To install a build into a game you play, copy `build/loader/tome.mix` into
that game's folder, or `make deploy INSTALL_DIR=/path/to/it` (never a
folder here).
