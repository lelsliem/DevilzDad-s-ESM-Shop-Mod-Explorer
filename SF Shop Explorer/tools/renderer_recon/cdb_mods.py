"""Do mod databases parse with the base-game framing?

The plugin's EnsureCdbIndex resolves exactly one database, the base game's
`materials/materialsbeta.cdb`. 23 mod archives ship their own at
`materials/creations/<name>/materialsbeta.cdb`, and if those use the same
BETH/DIFF framing then reading them is a loop over more blobs rather than a
second parser.

This script proves it before any C++ is written: extract each mod database
found under the mods folder, run it through cdb_records (the same walk the
6r-6a notes describe), and report what material -> texture mappings come out.
A database that yields nothing usable is a different fix entirely.

    python cdb_mods.py --ba2 "B:\\MO2\\Starfield\\mods\\Official CK\\kgcdoom - main.ba2"
    python cdb_mods.py --all          # every mod database on this host
    python cdb_mods.py --merge        # what the merged index would gain

`--merge` answers the only question that matters once the loop exists: how many
of those keys are genuinely NEW (the base game has no record of a mod material
at all), and how many of their colour maps are actually installed. A key whose
colour map is nowhere on disk still renders grey, however well it is indexed.

Pass 1 is a `re.finditer` over printable runs rather than the plugin's
byte-at-a-time loop. The two agree by construction - same run boundaries, same
"longer than 200 printable bytes is not a name" rule - and --all reproduces the
byte-loop's totals exactly (850 texture sets across 23 databases), which is
what makes the 105 MB base-game pass cheap enough to run here.
"""
from __future__ import annotations

import argparse
import collections
import glob
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "ba2_audit"))
import cdb_records as R  # noqa: E402
from ba2lib import Ba2Archive  # noqa: E402

BS = chr(92)
RUN = re.compile(rb"[\x20-\x7e]+")
BASE_CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")


def texture_key(path: str) -> str:
    """CdbTextureKey() from LiveModel.cpp, verbatim in behaviour."""
    flat = "".join(("/" if c == BS else chr(ord(c) + 32) if "A" <= c <= "Z" else c) for c in path)
    flat = flat.lstrip("/ ")
    if flat.startswith("data/"):
        flat = flat[5:]
    flat = flat.lstrip("/")
    if len(flat) < 15 or not flat.endswith(".dds") or not flat.startswith("textures/"):
        return ""
    return flat


def pass1(blob: bytes) -> dict[str, dict]:
    """Texture sets, keyed as the plugin keys them."""
    sets: dict[str, dict] = {}
    last_set = ""
    for m in RUN.finditer(blob):
        run = m.group().decode("latin-1")
        if len(run) > 200:
            continue
        low = run.lower()
        sfx = low.rfind("_textureset")
        if sfx >= 0 and len(low) <= 128:
            digits = sfx + len("_textureset")
            while digits < len(low) and low[digits].isdigit():
                digits += 1
            if digits > sfx + len("_textureset") and digits == len(low):
                last_set = low[:sfx].replace(BS, "/")
                sets.setdefault(last_set, {})
                continue
        if not last_set:
            continue
        key = texture_key(run)
        if not key:
            continue
        e = sets.setdefault(last_set, {})
        if key.endswith("_color.dds"):
            e.setdefault("color", key)
        elif key.endswith("_opacity.dds") or key.endswith("_alpha.dds"):
            e.setdefault("opacity", key)
    return sets


def pass2(blob: bytes) -> dict[str, list]:
    """MaterialOverrideColorTypeComponent::Color, keyed by bare stem."""
    override: dict[str, list] = {}
    for name, recs in R.objects(blob):
        for _, fid, _k, _c, pay in recs:
            if fid == 0x025A:
                v = R.decode(0, 0, pay)
                if isinstance(v, list) and len(v) == 4:
                    key = name.lower()
                    for suf in ("_material", "_material0", "_material1"):
                        if key.endswith(suf):
                            key = key[: -len(suf)]
                            break
                    override.setdefault(key, v)
                break
    return override


def index_blob(blob: bytes) -> tuple[dict, dict]:
    """Both indexes the plugin builds, in its own order."""
    return pass1(blob), pass2(blob)


def find_mod_databases(mods: str) -> list[tuple[str, str]]:
    targets: list[tuple[str, str]] = []
    for p in sorted(glob.glob(os.path.join(mods, "**", "*.ba2"), recursive=True)):
        try:
            ar = Ba2Archive(p)
        except Exception:
            continue
        if not ar.valid:
            continue
        for k in ar.entries:
            if k.endswith("materialsbeta.cdb") and "/creations/" in k:
                targets.append((p, k))
    return targets


def run_merge(a) -> int:
    if not os.path.isfile(BASE_CDB):
        print("base-game database not extracted: %s" % BASE_CDB)
        return 1
    with open(BASE_CDB, "rb") as f:
        base = pass1(f.read())
    print("base-game texture-set keys: %d" % len(base))

    targets = find_mod_databases(a.mods)
    tot_keys = tot_new = tot_col = tot_col_here = 0
    per_mod = []
    for path, entry in targets:
        ar = Ba2Archive(path)
        blob, err = ar.read_entry(entry)
        if not blob:
            print("%-46s read failed: %s" % (os.path.basename(path), err))
            continue
        sets = pass1(blob)
        new = [k for k in sets if k not in base]
        cols = [v["color"] for v in sets.values() if v.get("color")]
        here = [c for c in cols if c in ar.entries or c in ar.tex_files]
        tot_keys += len(sets)
        tot_new += len(new)
        tot_col += len(cols)
        tot_col_here += len(here)
        per_mod.append((len(new), os.path.basename(path), len(sets), len(cols), len(here)))
    per_mod.sort(reverse=True)
    print("\n%-38s %5s %5s %6s %6s" % ("mod database", "sets", "new", "colour", "present"))
    for nnew, name, nset, ncol, nhere in per_mod:
        print("%-38s %5d %5d %6d %6d" % (name, nset, nnew, ncol, nhere))
    print("\n%d mod databases: %d texture sets, %d new to the index, "
          "%d colour maps, %d of them installed in the archive that named them"
          % (len(per_mod), tot_keys, tot_new, tot_col, tot_col_here))
    if tot_col and not tot_col_here:
        print("\nWARNING: no mod colour map is installed - indexing the mod databases "
              "cannot change anything that renders.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ba2", default="")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--merge", action="store_true")
    ap.add_argument("--mods", default=r"B:\MO2\Starfield\mods")
    ap.add_argument("--show", type=int, default=6)
    a = ap.parse_args()

    if a.merge:
        return run_merge(a)

    targets: list[tuple[str, str]] = []
    if a.all:
        targets = find_mod_databases(a.mods)
    else:
        targets.append((a.ba2, ""))

    total_sets = total_over = 0
    for path, entry in targets:
        ar = Ba2Archive(path)
        if not entry:
            entry = next((k for k in ar.entries
                          if k.endswith("materialsbeta.cdb") and "/creations/" in k), "")
        if not entry:
            print("%-46s no mod database" % os.path.basename(path))
            continue
        blob, err = ar.read_entry(entry)
        if not blob:
            print("%-46s read failed: %s" % (os.path.basename(path), err))
            continue
        sets, over = index_blob(blob)
        withcol = sum(1 for v in sets.values() if v.get("color"))
        total_sets += len(sets)
        total_over += len(over)
        print("%-46s %8d bytes  %4d texture sets (%3d with a colour map)  %3d overrides"
              % (os.path.basename(path), len(blob), len(sets), withcol, len(over)))
        if not a.all:
            for k in sorted(sets)[:a.show]:
                v = sets[k]
                print("      %-46s %s" % (k, v.get("color", "<no colour map>")))
            for k in sorted(over)[:4]:
                c = over[k]
                print("      tint %-40s %.3f %.3f %.3f  a=%.3f" % (k, c[0], c[1], c[2], c[3]))
    if a.all:
        print("\ntotals: %d texture sets, %d overrides across %d mod databases"
              % (total_sets, total_over, len(targets)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
