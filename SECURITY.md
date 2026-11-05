# Security

## Reporting a problem

Report security problems privately through GitHub's private vulnerability
reporting (the repository's **Security** tab → **Report a vulnerability**),
not in a public issue. Include the Tome version (the first line of
`tome/tome.log`), the game build, and the steps or files that show it.

Worth reporting, for example:

- anything that runs a mod's code or loads its files **without** the user
  allowing it (an approval bypass);
- a map, a replay, or another player in a lobby or game making Tome or the
  game run code, write files outside the game's own folders, or crash in a
  way that looks exploitable, despite the built-in guards (the memhack guard,
  the path sanitizer, the BLP guard);
- a way to make Tome accept a `tome.sig` that the key's owner didn't make,
  or apply one mod's signature to other files;
- a WebAssembly mod (experimental, `docs/wasm-mods.md`) reading or writing
  outside its own memory, calling anything but Tome's imports, or running
  past its instruction budget;
- a crash in one of Tome's parsers of untrusted bytes (handshake messages,
  chat commands, `.tome` packages, `tome.mod.json`, settings, JASS scans).

## What Tome protects against

**Mods arrive by download and get passed around.** Tome treats a mod it has
never seen as untrusted:

- **Nothing of a new mod runs or is read by the game until you turn it on**
  in the Mods menu or with `/tome enable <id>`. That covers its DLL (native
  code), its game scripts (`.j`, `.ai`, `.pld`), its other assets and its
  icon, since the game's own 1.27b file loaders trust the files they read.
  Turning it on records a SHA-256 of the mod's files (shown on its details
  page, and checkable by hand with `sha256sum`); a package whose files
  change asks again.
- **Signed mods** (`tome.sig`, Ed25519): an update signed with the key of the
  version you allowed runs without asking; one signed with another key, or
  not signed, asks with a warning that it may not be from the same author.
  A signature says who made a mod, not that it's safe, and it's only as good
  as the author's care for their key.
- **Tome's own parsers** of mod files (`.tome` zips, `tome.mod.json`, `tome.sig`,
  `config.schema.json`) run before approval, so they're written to refuse
  bad input and are fuzzed (`fuzz/`).

**Maps, replays and other players** are untrusted too. Tome closes known
1.27b holes for every map, with or without mods: the JASS local/global
shadowing typecast ("memhack"), path traversal in the file natives, and
malformed BLP textures (`docs/known-bugs.md`). The multiplayer handshake
only compares mod lists: **Tome never downloads or installs a mod from
another player or a map**, and it never will. A missing mod is something you
install yourself.

**Releases** are built by CI from the tagged commit (reproducibly, with
build provenance attestations) and signed offline by the maintainer
(`SHA256SUMS.sig`, keys in `loader/release-keys.txt`). Tome's update check
only announces a release with a valid signature, and a published release
without one goes back to draft. How to check a download yourself:
[`docs/releasing.md`](docs/releasing.md). Worth reporting: a way to make
Tome announce a release that isn't signed with a release key, or make
Update Now install anything but the signed zip's `tome.mix` and Tome's own
texts.

The release keys (Ed25519, base64; the same as `loader/release-keys.txt`):

```
5CUwVuraMMdqoZNTaIEBw3d7m+h/LtExSvvtbZPtkt4=   main
5i6B8yJFM3zGlnhSMaAH/V6np2s6Ag398fZ4Dw0x0HA=   backup
```

## What it doesn't protect against

- **A mod you allowed.** A WebAssembly mod (experimental) runs in an
  interpreter (WAMR) and can only use Tome's imports, but the sandbox is as
  good as the interpreter, and the game still reads its files. A DLL mod is native code in the game's process,
  with the same access to your computer as the game. Tome can't sandbox it:
  anything in the process can undo anything Tome would set up. Crash
  isolation keeps a buggy mod from taking the game down, but that's not a
  security boundary. Allow a DLL mod only if you trust where it came from.
  Game scripts and assets you allowed run with the game's own (weaker)
  checks.
- **Programs already on your computer.** Anything that can write
  `tome/mods/` can also write `tome/state.json` or replace `tome.mix`.
  Approval is about mods that arrive from elsewhere, not malware that's
  already running.
- **Developer builds.** `TOME_DEBUG_APPROVE` allows mods without a click.
  Only developer builds (`-DTOME_DEV=ON`) read it; release builds ignore it.
- **Game bugs Tome doesn't know about.** The 1.27b game has more parsers
  (models, sounds, FDF) than Tome has guards for. Allowed mods and
  downloaded maps go through them.
