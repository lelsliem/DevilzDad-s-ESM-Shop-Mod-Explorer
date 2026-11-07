"""Why a Creation Kit mod item resolves 0 of 1 textured.

Traced end to end for the Plushie Mancubus, and the cause is general to every
CK mod rather than specific to this item:

  * `Official CK/kgcdoom - main.ba2` ships
    `materials/creations/kgcdoom/materialsbeta.cdb` - the mod carries its OWN
    copy of the material database, holding the mod's materials.
  * The plugin resolves exactly one database, the base game's
    `materials/materialsbeta.cdb` (ResolveAsset in EnsureCdbIndex).
  * So every material a CK mod invents is "not in the database", which sends
    it down the fuzzy scorer, which only indexes base-game texture archives.

This script proves each link: the NIF's material string, what the mod's own
CDB says about it, and whether the texture it names exists in any archive.

    python mod_material_trace.py --ba2 "B:\\MO2\\Starfield\\mods\\Official CK\\kgcdoom - main.ba2" \
        --nif meshes/kgcdoom/clutter/plush/doom_mancubus_plush_01.nif
"""
from __future__ import annotations

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ba2lib import Ba2Archive  # noqa: E402
from nif_materials import material_strings  # noqa: E402

BS = chr(92)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ba2", required=True)
    ap.add_argument("--nif", required=True)
    ap.add_argument("--cdb-entry", default="")
    ap.add_argument("--index", default=os.path.join(HERE, "extracted", "ba2nameindex.tsv"))
    a = ap.parse_args()

    ar = Ba2Archive(a.ba2)
    print("archive %s  valid=%s  entries=%d" % (os.path.basename(a.ba2), ar.valid, len(ar.entries)))
    if not ar.valid:
        return 1

    blob, err = ar.read_entry(a.nif.lower())
    print("\n1. NIF %s -> %s" % (a.nif, err or "ok"))
    if not blob:
        return 1
    mats = material_strings(blob)
    print("   material strings in the NIF (%d):" % len(mats))
    for m in mats:
        print("      %s" % m)
    if not mats:
        return 1

    cdb_name = a.cdb_entry
    if not cdb_name:
        for k in ar.entries:
            if k.endswith("materialsbeta.cdb"):
                cdb_name = k
                break
    if not cdb_name:
        print("\n2. this mod ships NO materialsbeta.cdb")
        return 0
    cdb, cerr = ar.read_entry(cdb_name)
    print("\n2. the mod's own database %s -> %s" % (cdb_name, cerr or "ok"))
    if not cdb:
        return 0
    print("   size %d bytes (base game is 105,037,616)" % len(cdb))

    stems = []
    for m in mats:
        s = m.lower().replace(BS, "/").rsplit("/", 1)[-1]
        if s.endswith(".mat"):
            s = s[:-4]
        stems.append(s)
        print("\n   searching the mod CDB for %r" % s)
        found = False
        p = cdb.find(s.encode("latin-1"))
        while p >= 0:
            seg = cdb[p:p + 300]
            txt = "".join(chr(c) if 32 <= c < 127 else " " for c in seg)
            print("      @%d  %s" % (p, txt[:200].strip()))
            found = True
            p = cdb.find(s.encode("latin-1"), p + 1)
            if p > 0 and not found:
                break
        if not found:
            print("      (not present)")

    print("\n3. does any archive on this host hold a .dds for it?")
    owner = {}
    with open(a.index, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            name, _, arc = line.rstrip("\n").rpartition("\t")
            if name and arc:
                owner[name] = arc
    for s in stems:
        pats = [s + "_color.dds", s + "_diff.dds", s + "_albedo.dds"]
        for pat in pats:
            for k in owner:
                if k.endswith("/" + pat):
                    print("   FOUND %-60s in %s" % (k, owner[k]))
                    break
        else:
            for k in owner:
                if s in k and k.endswith(".dds"):
                    print("   partial %-58s in %s" % (k, owner[k]))
                    break
            else:
                print("   nothing in the base-game archive index matches %r" % s)
    print("\n   (the index only covers the 110 base-game archives; mod texture")
    print("    archives are only reachable through [livemodel] extraba2, which")
    print("    is empty in the deployed ini)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())