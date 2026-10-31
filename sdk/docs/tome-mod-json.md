# `tome.mod.json` and `config.schema.json`

Every mod is a folder (or a `.tome` zip of one) with a `tome.mod.json` at
the top. Tome checks it when the mod is found. Errors keep the mod from
loading, and both errors and warnings show on the mod's Details page and in
`tome/tome.log`.

For your editor, start the file with
`"$schema": "<path to the SDK>/schemas/tome.mod.schema.json"`. It then
completes the fields and flags mistakes as you type. Tome ignores `$schema`.

## A complete example

```json
{
  "$schema": "../tome-sdk/schemas/tome.mod.schema.json",
  "schema": 1,
  "id": "hp-bars",
  "version": "1.2.0",
  "name": "HP Bars",
  "description": "Colored, scalable health bars.",
  "authors": ["Someone"],
  "license": "MIT",
  "icon": "icon.blp",
  "game": "1.27b",
  "tome": ">=0.1.0 <0.2.0",
  "depends": { "ui-lib": "^1.0" },
  "entry": "hp-bars.dll",
  "requires": ["CTextFrame::SetText"],
  "natives": ["native HpBarsSetColor takes integer argb returns nothing"],
  "sync": "client",
  "reload": "live",
  "config": "config.schema.json",
  "keybinds": [{ "id": "toggle", "label": "Toggle bars", "default": "Ctrl+H" }]
}
```

## Fields

Required fields are marked **\***. Paths are relative to the mod's folder
and can't leave it: no absolute paths, drive letters or `..`.

| Field | Type | Meaning |
|-------|------|---------|
| `schema` **\*** | `1` | The format version. |
| `id` **\*** | string | Unique, and stable forever: other mods depend on it, and settings and approval are kept under it. 2–64 characters: a lowercase letter, then lowercase letters, digits, `-` or `_`. `tome` is reserved. |
| `version` **\*** | string | A [semantic version](https://semver.org): `1.0.0`, `2.1.0-beta.1`. At most 64 characters. |
| `name` | string | Shown in the mod menu. Defaults to the id. |
| `description` | string | Shown on the Details page. |
| `authors` | list of strings | |
| `license` | string | |
| `icon` | path | A `.blp` or `.tga`, shown in the mod list. |
| `game` **\*** | game range | The game builds the mod works on, e.g. `"1.27b"`. Tome doesn't load it on others. |
| `tome` **\*** | version range | The Tome versions it works with, e.g. `">=0.1.0 <0.2.0"`. |
| `depends` | `{ id: range }` | Mods that must be installed and enabled. They load first, and when one is disabled, this mod is disabled first. |
| `recommends` | `{ id: range }` | A warning when one is missing. |
| `conflicts` | `{ id: range }` | A warning when both run. |
| `breaks` | `{ id: range }` | Can't run together: one of them stays off. |
| `entry` | path, or `{ game range: path }` | The DLL (32-bit). A map is checked in order and the first matching range wins, for different DLLs per build. Leave it out for a mod without code. |
| `wasm` | path | **Experimental.** A WebAssembly module (`.wasm`) Tome runs sandboxed, instead of a DLL: it can only use Tome's imports (`docs/wasm-mods.md` in Tome's repository). Not with `entry`. |
| `assets` | path | A folder of game files the mod adds or replaces (textures, models, FDF, `Blizzard.j`...), merged into the game's files. Default: `assets`. |
| `requires` | list of strings | Names from the mapping database that the mod can't work without (`"CTextFrame::SetText"`). Tome won't load the mod on a build where one isn't mapped. |
| `natives` | list of strings | The JASS natives the mod provides, declared as a map declares them: `"native MyLog takes integer value returns nothing"`. Register their code in `tome_mod_enable`. |
| `provides` | list of ids | Other ids this mod stands in for in other mods' `depends`. |
| `sync` **\*** | `"client"` or `"required"` | `client`: only matters to this player. `required`: every player in a multiplayer game needs the same version. The lobby checks it, and a running game keeps it from changing. |
| `reload` | `"live"`, `"map"` or `"restart"` | When changes to the mod's assets apply: right away (`live`, the default), at the next map load, or after a restart. |
| `load` | `{ "after": [ids], "before": [ids] }` | Load order among mods that don't depend on each other. |
| `config` | path | The settings schema (below). Tome makes the mod's settings page from it. |
| `keybinds` | list | Keys the player can rebind on the Keybinds page (below). |
| `links` | `{ name: url }` | Reserved; not shown yet. |

Unknown fields are warnings, not errors, so a newer file still loads on an
older Tome.

### Version ranges

- **Mods and Tome** use semver ranges:
  - `*` for any version, or an exact version: `1.2.0`;
  - comparators: `>=1.2 <2`;
  - caret: `^1.2` (1.2.0 up to, but not including, 2.0.0);
  - tilde: `~1.2` (1.2.x);
  - partial versions: `1.2` is 1.2.x;
  - alternatives joined with `||`.
- **Game builds** are the game's four-part file version (`1.27.1.7085`) or
  its common name. Known names: `1.24e`, `1.26a`, `1.27a` and `1.27b`. A
  range can use `*`, comparators, partial versions (`1.27` is 1.27.x.x) and
  `||`. Tome runs on 1.27b today.

### Keybinds

```json
"keybinds": [{ "id": "toggle", "label": "Toggle bars", "default": "Ctrl+H" }]
```

- `id` (1–64 lowercase letters, digits, `-`, `_` or `.`) is the name
  `on_keybind` uses.
- `label` is shown on the Keybinds page; it defaults to the id.
- `default` is the key until the player changes it. Keys are written as
  modifiers (`Ctrl`, `Shift`, `Alt`) and a key joined with `+`, or `None`
  for no key. Key names:
  - letters `A`–`Z`, digits `0`–`9`, `F1`–`F24`;
  - `Num0`–`Num9`, `NumAdd`, `NumSubtract`, `NumMultiply`, `NumDivide`,
    `NumDecimal`;
  - `Space`, `Enter`, `Esc`, `Tab`, `Backspace`, `Insert`, `Delete`, `Home`,
    `End`, `PageUp`, `PageDown`, `Left`, `Up`, `Right`, `Down`, `Pause`,
    `PrintScreen`;
  - the punctuation keys ``; = , - . / ` [ \ ] '``.

  The Keybinds page warns when two mods, or a mod and the game, use the
  same key.

## Settings: `config.schema.json`

A small subset of JSON Schema. Each entry of `properties` is one setting, in
the order the page shows them. Its key (1–64 letters, digits, `-`, `_` or
`.`) is what `setting_*` and `on_setting_changed` use. A mod can have up to
64 settings. For your editor:
`"$schema": "<SDK>/schemas/config.schema.schema.json"`.

```json
{
  "type": "object",
  "properties": {
    "show":  { "type": "boolean", "title": "Show bars", "description": "...", "default": true },
    "scale": { "type": "number", "minimum": 0.5, "maximum": 2, "step": 0.1, "default": 1 },
    "count": { "type": "integer", "minimum": 0, "maximum": 10, "default": 3 },
    "limit": { "type": "integer", "minimum": 30, "maximum": 1000, "input": "text" },
    "mode":  { "type": "string", "enum": ["auto", "always"], "enumTitles": ["Auto", "Always"] },
    "name":  { "type": "string", "maxLength": 32, "default": "" }
  }
}
```

| Keyword | For | Meaning |
|---------|-----|---------|
| `type` | all | `boolean`, `integer`, `number` or `string`. |
| `title`, `description` | all | The label (default: the key) and up to about three lines of help. |
| `default` | all | The value before the player changes it. |
| `minimum`, `maximum` | integer, number | The range. Values are kept in it. |
| `step` | integer, number | What the - and + buttons change it by. |
| `input` | integer, number | `"buttons"` (the default) or `"text"`: a box to type the value in. |
| `enum`, `enumTitles` | string | A list of choices, and how each is shown. |
| `maxLength` | string | The longest value, 1–4096. |

Values are saved in `tome/mods/config/<id>.json`, outside the mod's folder,
so they survive updates. The player changes them on the mod's settings page,
and a mod reads them with `setting_bool`, `setting_int`, `setting_number`
and `setting_string`.
