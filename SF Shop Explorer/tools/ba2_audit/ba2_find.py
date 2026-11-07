"""List BA2 entries whose path contains a substring, across given archives.

The plugin resolves .mat paths through the shipped material database, so when
a material set comes back empty the first thing to settle is what the archives
actually contain for that family: the .mat records, the .dds texture sets, or
neither.

    python ba2_find.py breechblock --dir "B:/.../Starfield/Data" --glob "Starfield - Textures*.ba2" "SFBGS050 - Main.ba2"
"""
from __future__ import annotations

import argparse
import glob as globmod
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "ba2_audit"))

from ba2lib import Ba2Archive  # noqa: E402


def names(ar: Ba2Archive):
    out = set()
    for attr in ("entries", "tex_files", "texFiles"):
        table = getattr(ar, attr, None)
        if table:
            out.update(table.keys() if hasattr(table, "keys") else table)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("needle")
    ap.add_argument("--dir", default="B:/SteamLibrary/steamapps/common/Starfield/Data")
    ap.add_argument("--glob", action="append", default=[])
    a = ap.parse_args()
    needles = [n.lower() for n in a.needle.split(",")]
    patterns = a.glob or ["*.ba2"]
    paths = []
    for pat in patterns:
        paths.extend(sorted(globmod.glob(os.path.join(a.dir, pat))))
    total = 0
    for path in paths:
        try:
            ar = Ba2Archive(path)
        except Exception as exc:  # noqa: BLE001 - a bad archive must not stop the sweep
            print("SKIP", os.path.basename(path), exc)
            continue
        for name in names(ar):
            low = name.lower()
            if all(n in low for n in needles):
                print("%-46s %s" % (os.path.basename(path)[:46], name))
                total += 1
    print("total", total, "across", len(paths), "archives")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
