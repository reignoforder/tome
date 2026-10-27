"""Makes a copy of the game's Blizzard.j that the asset-demo mod ships, to
check that mods can override the game's scripts.

usage: python3 patch_blizzard_j.py <Blizzard.j in> <Blizzard.j out>

The copy calls ExecuteFunc("TomeBlizzardJLoaded") at the end of InitBlizzard.
The test map defines that function (patch_map_script.py), so the map logs a
value only if the game ran this copy. ExecuteFunc needs no declaration, so the
map still loads with the original Blizzard.j.
"""
import re
import sys


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding="latin-1", newline="").read()
    newline = "\r\n" if "\r\n" in text else "\n"
    m = re.search(r"^function InitBlizzard takes nothing returns nothing\b.*?^(endfunction)", text, re.M | re.S)
    if not m:
        print("no InitBlizzard found", file=sys.stderr)
        return 1
    call = '    call ExecuteFunc("TomeBlizzardJLoaded")' + newline
    text = text[: m.start(1)] + call + text[m.start(1):]
    open(dst, "w", encoding="latin-1", newline="").write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
