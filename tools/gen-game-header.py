#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes sdk/include/tome/tome_game.h and tome_wasm_game.h: the typed side
of hooks and resolve, generated from the mapping files (mappings/<build>.json).

For every mapped function and import that has a signature:

- TOME_FN_<name>: its name as a string, for `hook` and `resolve`
  (TOME_FN_CUnit_SetHP is "CUnit::SetHP");
- tome_fn_<name>: a function pointer type with the right calling convention,
  for calling what `resolve` returns;
- tome_args_<name>: a struct laid over a hook's `call->args` (the arguments
  are 32-bit words in the signature's order, a double or 64-bit integer
  taking two), so a handler reads and changes them by name:
  `TOME_ARGS(CUnit_SetHP, call)->hp`.

Only functions whose signature is the same in every mapping file (the ones
that have it) are written; the rest would not type the same on every build.

tome_wasm_game.h is the same for WebAssembly mods' typed hooks
(loader/src/core/wasm_hooks.h has the rule: numbers and CUnit*, CItem*,
CPlayerWar3* arguments, a number or nothing as the result): for each function
that qualifies, TOME_FN_<name> and tome_wasm_hook_<name>, the block a hook's
handler gets (flags, result, then the arguments, a word each).

  tools/gen-game-header.py           write it
  tools/gen-game-header.py --check   exit 1 if it isn't current (a test runs this)

Run it after changing a mapping file's functions.
"""
import glob
import json
import os
import re
import sys
import textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(ROOT, "sdk", "include", "tome", "tome_game.h")
WASM_TARGET = os.path.join(ROOT, "sdk", "include", "tome", "tome_wasm_game.h")

# Types the generated code knows without a declaration, by how many 32-bit
# words one takes in a call.
WORDS = {
    "void": 0, "int": 1, "unsigned": 1, "unsigned int": 1, "int32_t": 1, "uint32_t": 1, "float": 1,
    "uintptr_t": 1, "intptr_t": 1, "bool": 1, "char": 1, "short": 1, "unsigned char": 1, "unsigned short": 1,
    "int8_t": 1, "uint8_t": 1, "int16_t": 1, "uint16_t": 1, "double": 2, "int64_t": 2, "uint64_t": 2,
}
# What a small type takes in the arguments' words: a whole word.
WIDEN = {"bool": "uint32_t", "char": "int32_t", "short": "int32_t", "unsigned char": "uint32_t",
         "unsigned short": "uint32_t", "int8_t": "int32_t", "uint8_t": "uint32_t", "int16_t": "int32_t",
         "uint16_t": "uint32_t"}
# Windows types the signatures use, as plain ones (the header doesn't include windows.h).
ALIASES = {"HRESULT": "int32_t", "DWORD": "uint32_t", "BOOL": "int32_t", "LONG": "int32_t", "ULONG": "uint32_t"}
KEYWORDS = {"this", "new", "delete", "class", "default", "template", "operator", "register", "switch", "case",
            "union", "struct", "enum", "typename", "namespace", "private", "public", "friend", "virtual",
            "export", "signed", "unsigned", "long", "short", "auto", "inline", "restrict", "ref"}
# Names the Wasm hook structs use for their own first two members.
RESERVED_MEMBERS = {"flags", "result"}
CONV = {"cdecl": "TOME_GAME_CDECL", "stdcall": "TOME_GAME_STDCALL", "fastcall": "TOME_GAME_FASTCALL",
        "thiscall": "TOME_GAME_THISCALL"}


def mangle(name):
    return re.sub(r"[^A-Za-z0-9_]", "_", name.replace("::", "_"))


def load():
    """{name: (symbol, [builds])} for the functions and imports."""
    found = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "mappings", "*.json"))):
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict) or "symbols" not in data:
            continue
        for name, symbol in data["symbols"].items():
            if symbol.get("kind") in ("function", "import") and "sig" in symbol:
                found.setdefault(name, []).append((data.get("name", os.path.basename(path)), symbol))
    return found


def parse_sig(sig):
    """(return type, [(type, name)], variadic)."""
    sig = re.sub(r"\b(%s)\b" % "|".join(ALIASES), lambda a: ALIASES[a.group(1)], sig)
    m = re.match(r"^(.*?)\((.*)\)$", sig.strip())
    if not m:
        raise ValueError("can't read the signature: " + sig)
    ret = m.group(1).strip()
    params, variadic = [], False
    body = m.group(2).strip()
    if body not in ("", "void"):
        for i, p in enumerate(body.split(",")):
            p = " ".join(p.split())
            if p == "...":
                variadic = True
                continue
            n = re.match(r"^(.*?[*\s])(\w+)$", p)
            if p in WORDS or not n:
                ptype, pname = p, None  # a type alone ("int", "unsigned int", "CUnit*")
            else:
                ptype, pname = n.group(1).strip(), n.group(2)
            if not pname:
                pname = "a%d" % i
            if pname in KEYWORDS or pname in RESERVED_MEMBERS:
                pname = "this_" if pname == "this" else pname + "_"
            params.append((ptype, pname))
    return ret, params, variadic


def base_name(ctype):
    t = ctype.replace("*", " ").replace("const", " ").split()
    return " ".join(t)


def words_of(ctype):
    if "*" in ctype:
        return 1
    b = base_name(ctype)
    if b in WORDS:
        return WORDS[b]
    raise ValueError("unknown type " + ctype)


def c_type(ctype):
    """A type as the header writes it: a pointed-to game type is `struct Name`
    (declared up front; a tag can't clash with a mod's own typedef)."""
    if "*" not in ctype:
        return WIDEN.get(ctype, ctype)
    b = base_name(ctype)
    if b in WORDS:
        return ctype
    return re.sub(r"\b%s\b" % re.escape(b), "struct " + b, ctype)


def comment(text, indent=""):
    text = text.replace("*/", "* /")
    lines = textwrap.wrap(text, 96 - len(indent))
    if len(lines) == 1:
        return "%s/* %s */\n" % (indent, lines[0])
    return "%s/* %s\n%s" % (indent, lines[0], "".join("%s * %s\n" % (indent, l) for l in lines[1:])) + indent + " */\n"


HEADER = r"""/* SPDX-License-Identifier: MIT-0 */
/* Generated by tools/gen-game-header.py from the mapping files: don't edit. */
#pragma once

/*
 * The game's mapped functions and imports, typed (tome.h: hook, resolve).
 * For each one that has a signature:
 *
 *   TOME_FN_<name>    its name, as `hook` and `resolve` take it ("CUnit::SetHP"
 *                     is TOME_FN_CUnit_SetHP; "::" and "." become "_")
 *   tome_fn_<name>    the function's pointer type, for calling what `resolve`
 *                     gives you
 *   tome_args_<name>  the arguments of a hook's call->args, by name
 *
 * A hook handler reads and changes its arguments through the struct:
 *
 *   static void TOME_CALL on_set_hp(tome_mod* mod, tome_hook_call* call, void* user) {
 *       TOME_ARGS(CUnit_SetHP, call)->hp = 100.0f;
 *   }
 *
 * `this` is called `this_`. The arguments are 32-bit words, so smaller types
 * take a whole one (a float is its bits), and a double or 64-bit integer
 * takes two. A variadic function's extra words come after the struct
 * (tome_hook_call.extra_count). Pointer types are opaque: `struct CUnit*` points at
 * a struct this header only names. The return value is call->result (a
 * float or double: call->result_float).
 *
 * Declared only for functions whose signature is the same in every mapping
 * file; the mapping database holds the rest. A function that isn't mapped
 * for the running game fails `hook` with TOME_ERR_UNAVAILABLE: list the
 * ones you use in tome.mod.json "requires".
 */

#include <stdint.h>

#include "tome.h"

#if defined(_MSC_VER)
#define TOME_GAME_CDECL __cdecl
#define TOME_GAME_STDCALL __stdcall
#define TOME_GAME_FASTCALL __fastcall
#define TOME_GAME_THISCALL __thiscall
#elif defined(__i386__)
#define TOME_GAME_CDECL __attribute__((cdecl))
#define TOME_GAME_STDCALL __attribute__((stdcall))
#define TOME_GAME_FASTCALL __attribute__((fastcall))
#define TOME_GAME_THISCALL __attribute__((thiscall))
#else
/* Not a 32-bit x86 build: the game isn't here; the types still compile. */
#define TOME_GAME_CDECL
#define TOME_GAME_STDCALL
#define TOME_GAME_FASTCALL
#define TOME_GAME_THISCALL
#endif

/* The arguments of a hook's call as the named struct (a handler for TOME_FN_<name>). */
#define TOME_ARGS(name, call) ((tome_args_##name*)(call)->args)

#if defined(__GNUC__) && !defined(__clang__)
/* GCC warns about thiscall on a pointer type that isn't a class method, and applies it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif

#ifdef __cplusplus
extern "C" {
#endif

"""


def generate():
    found = load()
    entries, opaque = [], set()
    skipped = []
    for name in sorted(found):
        symbols = found[name]
        sigs = {s["sig"] for _, s in symbols}
        convs = {s.get("conv") for _, s in symbols}
        if len(sigs) != 1 or len(convs) != 1:
            skipped.append(name)
            continue
        symbol = symbols[0][1]
        try:
            ret, params, variadic = parse_sig(symbol["sig"])
            for t, _ in params:
                words_of(t)
            if "*" not in ret:
                words_of(ret)
        except ValueError as e:
            raise SystemExit("%s: %s" % (name, e))
        for t in [ret] + [t for t, _ in params]:
            if "*" in t:
                b = base_name(t)
                if b not in WORDS and b != "":
                    opaque.add(b)
        entries.append((name, symbol, ret, params, variadic, [b for b, _ in symbols]))

    names = [mangle(e[0]) for e in entries]
    if len(set(names)) != len(names):
        raise SystemExit("two mapped names become the same identifier")

    out = [HEADER]
    # Opaque types: a struct tag the header only declares.
    declared = sorted(o for o in opaque if re.match(r"^[A-Za-z_]\w*$", o) and o not in WORDS)
    out.append("/* Types the game's functions point at: declared, not defined. */\n")
    for o in declared:
        out.append("struct %s;\n" % o)
    out.append("\n")
    for name, symbol, ret, params, variadic, builds in entries:
        m = mangle(name)
        doc = symbol.get("doc", "")
        head = "%s: %s(%s)" % (name, ret, ", ".join("%s %s" % p for p in params) or "void")
        out.append(comment(head + (". " + doc if doc else "")))
        out.append('#define TOME_FN_%s "%s"\n' % (m, name))
        plist = ", ".join("%s %s" % (c_type(t), n) for t, n in params)
        if variadic:
            plist = (plist + ", ..." if plist else "...")
        out.append("typedef %s (%s *tome_fn_%s)(%s);\n" % (c_type(ret), CONV[symbol["conv"]], m, plist or "void"))
        out.append("#pragma pack(push, 4)\n")
        if params:
            out.append("typedef struct tome_args_%s {\n" % m)
            for t, n in params:
                out.append("    %s %s;\n" % (c_type(t), n))
            out.append("} tome_args_%s;\n" % m)
        else:
            out.append("typedef struct tome_args_%s { uint32_t none_; } tome_args_%s; /* no arguments */\n" % (m, m))
        out.append("#pragma pack(pop)\n")
        size = sum(words_of(t) for t, _ in params) or 1
        out.append("typedef char tome_args_%s_layout[sizeof(tome_args_%s) == %d ? 1 : -1];\n\n" % (m, m, 4 * size))
    out.append("#ifdef __cplusplus\n}\n#endif\n\n")
    out.append("#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC diagnostic pop\n#endif\n")
    return "".join(out), len(entries), skipped


WASM_INTS = {"int", "int32_t", "uint32_t", "unsigned", "unsigned int"}
WASM_OBJECTS = {"CUnit*": "tome_unit_t", "CItem*": "tome_item_t", "CPlayerWar3*": "tome_player_t"}


# What core/wasm_hooks.cpp refuses and allows (a test compares them): the game's
# scripting, CRT, crash handling and start/end can't be hooked, and only these
# functions' int arguments can be changed (any other int may be an index).
WASM_DENY_PREFIXES = ("Native.", "__", "CrashHandler::")
WASM_DENY = {"GameShutdown", "InitNatives"}
WASM_WRITABLE_INTS = {"CUnit::GetHeroAgi", "CUnit::GetHeroInt", "CUnit::GetHeroStr", "CUnit::SetHeroAgi",
                      "CUnit::SetHeroInt", "CUnit::SetHeroStr", "CUnit::SetHealthBarShown", "HealthBars::Consider"}


def wasm_denied(name):
    return name in WASM_DENY or name.startswith(WASM_DENY_PREFIXES)


def wasm_type(ctype):
    """The block member type for an argument or result a module can take, or None."""
    t = re.sub(r"\s+", " ", ctype.replace("const ", "")).replace(" *", "*").strip()
    if t in WASM_INTS:
        return "uint32_t" if t.startswith("u") else "int32_t"
    if t == "float":
        return "float"
    return WASM_OBJECTS.get(t)


WASM_HEADER = r"""/* SPDX-License-Identifier: MIT-0 */
/* Generated by tools/gen-game-header.py from the mapping files: don't edit. */
#pragma once

/*
 * The game functions a WebAssembly mod can hook (tome_wasm.h, tome_hook), typed.
 * A function qualifies when every argument is a number (int, uint32_t, float) or
 * a unit, item or player (a handle, `this` included) and it returns nothing or a
 * number. For each one:
 *
 *   TOME_FN_<name>          its name, for tome_hook ("CUnit::SetHP" is TOME_FN_CUnit_SetHP)
 *   tome_wasm_hook_<name>   what its handler gets: `flags` (set TOME_WASM_HOOK_SKIP in a
 *                           BEFORE handler to return `result` without the game's
 *                           function), `result`, then the arguments in order
 *
 * A handler changes numbers in the block: float arguments, and the int arguments not
 * marked const, in a BEFORE handler (the game's function gets them), and `result` in an
 * AFTER handler or with TOME_WASM_HOOK_SKIP. Units, items and players are read-only
 * handles (tome_wasm.h's object calls take them), and so are the int arguments marked
 * const: the game may use them as an index. The game's scripting (Native.*), start,
 * shutdown and crash handling can't be hooked.
 *
 *   static void on_agi(void* block, void* user) {
 *       tome_wasm_hook_CUnit_GetHeroAgi* call = (tome_wasm_hook_CUnit_GetHeroAgi*)block;
 *       call->result += 5;
 *   }
 *   tome_hook(TOME_FN_CUnit_GetHeroAgi, TOME_HOOK_AFTER, 0, on_agi, NULL);
 */

#include <stdint.h>

#ifndef TOME_WASM_HOOK_SKIP
#define TOME_WASM_HOOK_SKIP 1
#endif

typedef uint32_t tome_unit_t;
typedef uint32_t tome_item_t;
typedef uint32_t tome_player_t;

"""


def generate_wasm():
    found = load()
    out = [WASM_HEADER]
    count = 0
    for name in sorted(found):
        symbols = found[name]
        if len({s["sig"] for _, s in symbols}) != 1 or symbols[0][1].get("kind") != "function":
            continue
        if wasm_denied(name):
            continue
        ret, params, variadic = parse_sig(symbols[0][1]["sig"])
        members = [wasm_type(t) for t, _ in params]
        result = "uint32_t" if ret == "void" else wasm_type(ret)
        if variadic or None in members or result is None or ret in WASM_OBJECTS or len(params) > 16:
            continue
        if ret != "void" and ret.rstrip().endswith("*"):
            continue
        m = mangle(name)
        out.append('#define TOME_FN_%s "%s"\n' % (m, name))
        out.append("typedef struct tome_wasm_hook_%s {\n    uint32_t flags;\n" % m)
        out.append("    %s result;%s\n" % (result, "  /* unused: no result */" if ret == "void" else ""))
        for (t, n), mt in zip(params, members):
            # An int that isn't on the list is read-only: the game may take it as an index.
            ro = mt in ("int32_t", "uint32_t") and name not in WASM_WRITABLE_INTS
            out.append("    %s%s %s;\n" % ("const " if ro else "", mt, n))
        out.append("} tome_wasm_hook_%s;\n" % m)
        out.append("typedef char tome_wasm_hook_%s_layout[sizeof(tome_wasm_hook_%s) == %d ? 1 : -1];\n\n" % (m, m, 8 + 4 * len(params)))
        count += 1
    return "".join(out), count


def main():
    content, count, skipped = generate()
    wasm_content, wasm_count = generate_wasm()
    targets = [(TARGET, content), (WASM_TARGET, wasm_content)]
    if "--check" in sys.argv:
        stale = []
        for path, text in targets:
            try:
                with open(path, encoding="utf-8") as f:
                    current = f.read()
            except OSError:
                current = None
            if current != text:
                stale.append(os.path.relpath(path, ROOT))
        if stale:
            print("out of date: %s: run tools/gen-game-header.py" % ", ".join(stale))
            return 1
        print("tome_game.h and tome_wasm_game.h are current")
        return 0
    for path, text in targets:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    print("wrote %s: %d functions%s" % (os.path.relpath(TARGET, ROOT), count,
                                         ", %d left out (their signatures differ between builds)" % len(skipped) if skipped else ""))
    print("wrote %s: %d functions a Wasm mod can hook" % (os.path.relpath(WASM_TARGET, ROOT), wasm_count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
