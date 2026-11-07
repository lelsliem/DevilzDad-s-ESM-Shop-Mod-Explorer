"""Test whether the CDB header name pool maps onto field ids POSITIONALLY.

Background (6r-8d): the CLAS descriptor is
    u32 fieldId, u16 typeCode, u16 flags, u16 byteOffset, u16 byteSize
with no name index, and the header string pool looked globally deduplicated
rather than per-type.  6r-10 revisits that with a sharper question: the pool
holds exactly 456 strings and the database has exactly 456 distinct field
ids.  If both sets are sorted the same way, the pool is not a deduplicated
bag at all - it is the id table in name order, and the "deduplication" was
just the coincidence that no two classes reuse a field name.

This script prints:
  * the two sorted sets side by side,
  * how many positions agree exactly,
  * a sane-name ratio under the positional hypothesis vs. under random,
which is the actual test: a true positional map should put real property
names (albedo, translucency, emissive...) on real field ids, and a false
one should not.

    python cdb_field_names.py            # full report
    python cdb_field_names.py --pairs    # just the pairing table
    python cdb_field_names.py --sort name  # pool sorted by string
"""
from __future__ import annotations

import argparse
import collections
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdb_schema as S  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")

# Words that actually appear in this material domain.  Used to score a
# candidate mapping: a positional map should light these up.
DOMAIN = (
    "albedo", "diffuse", "opacity", "alpha", "blend", "translucen", "emissive",
    "emissiv", "metal", "rough", "normal", "bump", "height", "ao", "ambient",
    "occlusion", "specular", "gloss", "shine", "reflect", "refract", "ior",
    "detail", "base", "color", "colour", "texture", "tex", "map", "layer",
    "float", "parallax", "rim", "fresnel", "decal", "hair", "skin", "cloth",
    "grass", "foliage", "snow", "water", "glass", "metalness", "texcoord",
    "uv", "vertex", "wind", "displacement", "material", "shader", "property",
    "flag", "intensity", "scale", "offset", "tint", "multiply", "add",
    "power", "curve", "root", "bone", "weight", "mask", "channel", "index",
    "count", "name", "type", "size", "count", "version", "id", "data",
)


def sane(name: str) -> bool:
    n = name.lower()
    return any(w in n for w in DOMAIN)


def camelish(name: str) -> bool:
    """Bethesda class/property strings are CamelCase or snake-ish, never
    sentence fragments with spaces or punctuation."""
    if not name or len(name) < 3 or len(name) > 48:
        return False
    if any(c in name for c in " ,.;:/\\*?<>|=+-()[]{}"):
        return False
    return all(32 <= ord(c) < 127 for c in name)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", action="store_true")
    ap.add_argument("--sort", default="id", choices=["id", "name"])
    a = ap.parse_args()

    b = S.load(CDB)
    pool = S.names(b)
    cls = S.classes(b)

    ids = set()
    shape = collections.defaultdict(set)
    per_class = collections.defaultdict(list)
    for ci, recs in enumerate(cls):
        for fid, typ, flags, off, sz in recs:
            ids.add(fid)
            shape[fid].add((typ, off, sz))
            per_class[ci].append(fid)

    ids_sorted = sorted(ids)
    print("pool strings: %d" % len(pool))
    print("distinct ids : %d" % len(ids_sorted))
    print("classes      : %d" % len(cls))

    # ---- are the two sets even the same cardinality? -----------------
    print("\n== cardinality ==")
    print("pool == ids : %s (pool-only %d, ids-only %d)"
          % (len(pool) == len(ids_sorted),
             max(0, len(pool) - len(ids_sorted)),
             max(0, len(ids_sorted) - len(pool))))

    # ---- duplicate strings in the pool?  If the pool were per-type it
    # ---- would repeat a name once per class that uses it.
    dupes = [n for n, c in collections.Counter(pool).items() if c > 1]
    print("duplicate strings in pool: %d %s"
          % (len(dupes), dupes[:8] if dupes else ""))

    # ---- ordering test ------------------------------------------------
    print("\n== ordering of the pool as stored ==")
    print("pool sorted ascending : %s" % (pool == sorted(pool)))
    print("pool sorted descending: %s" % (pool == sorted(pool, reverse=True)))
    # Where does each id sit in the pool if the pool were per-type?  A
    # positional map implies pool position == rank of the id.
    print("pool[0:6]  : %s" % pool[:6])
    print("pool[-6:]  : %s" % pool[-6:])
    print("ids [0:6]  : %s" % ["0x%04x" % i for i in ids_sorted[:6]])
    print("ids [-6:]  : %s" % ["0x%04x" % i for i in ids_sorted[-6:]])

    # ---- the candidate mapping, sorted by id --------------------------
    pairs = list(zip(ids_sorted, pool))
    hits = sum(1 for _, nm in pairs if sane(nm))
    hits_pool = sum(1 for nm in pool if sane(nm))
    print("\n== positional hypothesis (id rank -> pool rank) ==")
    print("pairs: %d" % len(pairs))
    print("domain-word hits on the pairing : %d / %d  (%.1f%%)"
          % (hits, len(pairs), 100.0 * hits / max(1, len(pairs))))
    print("domain-word hits on the pool    : %d / %d  (%.1f%%)"
          % (hits_pool, len(pool), 100.0 * hits_pool / max(1, len(pool))))

    # Control: shuffle the pool and rescore.  If the positional hypothesis
    # is real, shuffling must destroy the signal.
    rng = random.Random(12345)
    ctrl = []
    for _ in range(20):
        sh = pool[:]
        rng.shuffle(sh)
        ctrl.append(sum(1 for _, nm in zip(ids_sorted, sh) if sane(nm)))
    print("control (shuffled pool, 20 draws): min %d max %d mean %.1f"
          % (min(ctrl), max(ctrl), sum(ctrl) / len(ctrl)))

    # ---- sanity of the pool as strings at all ------------------------
    print("\n== pool composition ==")
    print("camelish names : %d / %d" % (sum(1 for n in pool if camelish(n)), len(pool)))
    print("names with '::': %d" % sum(1 for n in pool if "::" in n))
    print("all-caps names : %d" % sum(1 for n in pool if n.isupper()))

    if a.pairs:
        print("\n== pairing table (id rank -> pool rank) ==")
        for i, (fid, nm) in enumerate(pairs):
            sh = ",".join("0x%04x/%d/%d" % s for s in sorted(shape[fid])[:2])
            print("%4d 0x%04x %-9s %-46s %s"
                  % (i, fid, sh, nm, "*" if sane(nm) else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())