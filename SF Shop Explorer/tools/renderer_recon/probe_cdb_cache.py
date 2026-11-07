"""Read SFShopExplorer's cdbindex.bin and print what a material resolves to.

The material index the plugin actually uses is on disk, so the question "does
the CDB know a colour map for this material, and if not what does it hold"
can be answered exactly instead of inferred from a log line.

    python probe_cdb_cache.py breechblock
    python probe_cdb_cache.py antiquesmg --colors
"""
from __future__ import annotations

import argparse
import struct
import sys

CACHE = ("B:/SteamLibrary/steamapps/common/Starfield/Data/SFSE/Plugins/"
         "SFShopExplorerAssets/models/cdbindex.bin")
MAGIC = b"SFECDB1\x00"


def read_str(f) -> str:
    n = struct.unpack("<I", f.read(4))[0]
    return f.read(n).decode("latin-1")


def load(path: str):
    with open(path, "rb") as f:
        if f.read(8) != MAGIC:
            raise SystemExit("bad magic")
        schema, mods = struct.unpack("<II", f.read(8))
        set_count, alpha_count, kind_count = struct.unpack("<QQQ", f.read(24))
        stamp = read_str(f)
        sets = {}
        for _ in range(set_count):
            key = read_str(f)
            color = read_str(f)
            opacity = read_str(f)
            sets[key] = (color, opacity)
        return schema, mods, stamp, set_count, alpha_count, kind_count, sets


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("needle", nargs="?", default="breechblock")
    ap.add_argument("--path", default=CACHE)
    ap.add_argument("--limit", type=int, default=25)
    ap.add_argument("--colors", action="store_true", help="only sets with a colour")
    a = ap.parse_args()

    schema, mods, stamp, nset, nalpha, nkind, sets = load(a.path)
    print("schema=%d mods=%d sets=%d alphas=%d kinds=%d" % (schema, mods, nset, nalpha, nkind))
    print("stamp=%s" % stamp)
    hit = 0
    empties = 0
    for key, (color, opacity) in sets.items():
        if a.needle.lower() not in key:
            continue
        if a.colors and not color:
            continue
        if not color:
            empties += 1
        print("%-70s color=%s%s" % (key[:70], color or "<none>",
                                    ("  opacity=" + opacity) if opacity else ""))
        hit += 1
        if hit >= a.limit:
            break
    print("matched=%d (textureless among them=%d)" % (hit, empties))
    return 0


if __name__ == "__main__":
    sys.exit(main())
