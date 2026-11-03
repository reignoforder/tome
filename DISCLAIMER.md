# Disclaimer

**Tome is an unofficial, fan-made project.** It is not made, affiliated
with, authorized, endorsed or supported by Blizzard Entertainment, Inc.,
Activision Blizzard, Microsoft, or anyone connected with them. Don't ask
them for help with it.

**Trademarks.** Warcraft, Warcraft III, The Frozen Throne, Battle.net and
Blizzard Entertainment are trademarks or registered trademarks of Blizzard
Entertainment, Inc. in the U.S. and other countries. Other names are their
owners'. Tome uses them only to say which game it works with; that doesn't
mean any of their owners is involved.

**No game files.** Tome contains none of the game's files, code or art, and
doesn't download any. You need your own copy of Warcraft III (Reign of
Chaos, The Frozen Throne or both) at version 1.27b or 1.26a. Tome doesn't remove or get around the game's copy
protection or CD key checks, and isn't meant to.

**What it does to the game.** Tome is loaded by the game and changes the
game's code in memory while it runs. It writes nothing to the game's own
files: delete `tome.mix` and the game runs as it did before (the `tome/`
folder holds only Tome's own files). Changing the game this way may not be
allowed by the license agreement or terms of use that came with your copy
or with an online service. Read them and decide for yourself; this is not
legal advice.

**Online play.** Tome is meant for single player, LAN games and replays,
and for communities and servers that allow modified clients. Don't use it
where they're not allowed. Online services and their anti-cheat may
detect a modified game and act on it, up to closing your account, and
neither Tome nor its authors can do anything about that. Tome isn't a cheat
and isn't built to hide from anything.

**What other players see.** Tome makes no connections of its own and
sends nothing anywhere but to the other players of a game you're in. There,
it shows them your *required* mods (those marked `"sync": "required"`, which
every player of a game needs), each as its id, version and a hash of its
files. In a lobby, a host running Tome with required mods sends its list to
each player, and a player with Tome answers whoever sent one (host or not)
with theirs; nothing is sent otherwise. In a game, every player with required mods sends the list to the
others as game actions, so it's also recorded in the game's replay, which
anyone you share it with can read.
Mods only for you (`"sync": "client"`) aren't sent. Tome's log
(`tome/tome.log`) and crash reports (`tome/crashes/`, which can contain
parts of the game's memory) stay on your computer unless you share them.

**Mods are other people's code.** A mod with a DLL runs with the same access
to your computer as the game: it can read and change your files and do
anything else a program can. Tome asks before it runs a new mod's code, and
again when a package's files change (unless the update is signed by the
author you allowed, `SECURITY.md`), and it shows who signed it. A mod
folder you allowed isn't checked again, and Tome doesn't check what a mod
does. Install mods only from people you
trust. WebAssembly mods run sandboxed and are experimental. Tome's
authors don't review, host or vouch for mods made by others, and aren't
responsible for them.

**Antivirus.** Tome patches another program's code, which some antivirus
programs flag on that alone. Get Tome only from its official releases, and
check the file's SHA-256 against the one published there.

**No warranty.** Tome is provided as is, without any warranty, to the
extent the law allows (sections 15 and 16 of the GNU GPL v3, `LICENSE`;
the SDK: `sdk/LICENSE`). It may crash the game, lose unsaved progress, or
break saved games, replays or settings. Back up what you care about. You
use Tome and any mod at your own risk.

**Reporting.** Security problems: `SECURITY.md`. Anything else: the
issue tracker. Rights holders with a concern can reach the authors there
too.
