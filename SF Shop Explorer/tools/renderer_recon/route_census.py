"""Does the CDB shader route predict translucency? Measure before shipping.

6r-6 decoded two fields that name a material's shader:

    0x0472  on TextureSet / PermutationLayer objects
            HighResUniqueMap / UniqueMap / DetailMapTiling / Tiling
    0x0543  ShaderRouteComponent::Route
            Effect / Deferred / Water / PrecomputedScattering

Neither rides on the `_Material` object - they sit on the `_TextureSet`
objects, so deciding translucency from them means joining the material's base
name onto its texture sets. Before any of that replaces the name-token
heuristic it has to be shown that the route actually separates translucent
materials from opaque ones. This script does that cross-tab and nothing else.

    python route_census.py
    python route_census.py --examples 25
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cdb_records as R  # noqa: E402
import cdb_schema as S  # noqa: E402

# name words the current shipped heuristic (MaterialBlendAlpha) keys on
TRANS = ("glass", "clear", "translucent", "transpar")
OPAQUE = ("metal", "wood", "stone", "concrete", "rubber", "leather", "cardboard")

SUFFIX = re.compile(r"_(material|textureset|layer|blender|uvstream[0-9a-z]*|cell|permutationlayer)[0-9]*$", re.I)


def base_of(name: str) -> str:
    """'data\\materials\\x\\y_TextureSet1' -> 'data/materials/x/y'."""
    n = SUFFIX.sub("", name)
    return n.lower().replace("\\", "/")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--examples", type=int, default=15)
    a = ap.parse_args()
    b = S.load()

    routes: dict[str, set] = collections.defaultdict(set)   # base -> {route}
    kind_of_base: dict[str, set] = collections.defaultdict(set)
    alpha: dict[str, float] = {}
    for name, recs in R.objects(b):
        base = base_of(name)
        low = name.lower()
        if "_material" in low:
            kind_of_base[base].add("material")
        for _, fid, kind, cnt, pay in recs:
            if fid in (0x0472, 0x0543):
                v = R.decode(kind, cnt, pay)
                if isinstance(v, str) and v:
                    routes[base].add("%04x:%s" % (fid, v))
            elif fid == 0x025A:
                v = R.decode(0, 0, pay)
                if isinstance(v, list) and len(v) == 4:
                    alpha.setdefault(base, v[3])

    mats = {b for b, k in kind_of_base.items() if "material" in k}
    print("material bases %d   bases with a route %d   with an alpha %d"
          % (len(mats), sum(1 for b in mats if b in routes), sum(1 for b in mats if b in alpha)))

    def bucket(base: str) -> str:
        # use the stem so 'data/materials/x/Glass01' and 'glass01' agree
        stem = base.rsplit("/", 1)[-1]
        if any(w in stem for w in TRANS):
            return "translucent-word"
        if any(w in stem for w in OPAQUE):
            return "opaque-word"
        return "neither"

    table: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    noroute = collections.Counter()
    for m in mats:
        b = bucket(m)
        if not routes.get(m):
            noroute[b] += 1
            table[b]["<no route record>"] += 1
        for r in routes.get(m, ()):
            table[b][r] += 1

    for b in ("translucent-word", "opaque-word", "neither"):
        print()
        print("=== %s (%d materials, %d with no route record)" % (b, sum(table[b].values()), noroute[b]))
        for r, n in table[b].most_common(10):
            print("    %-34s %6d" % (r, n))

    # The decisive comparison: does a translucent-word material carry a
    # different route distribution than an opaque-word one?
    tw = {r for r in table["translucent-word"] if r != "<no route record>"}
    ow = {r for r in table["opaque-word"] if r != "<no route record>"}
    only_t = tw - ow
    print()
    print("routes seen only on translucent-word materials:", sorted(only_t) or "none")
    print("routes seen only on opaque-word materials    :", sorted(ow - tw) or "none")

    # Would a route rule beat the name heuristic? Count how each would call
    # every material whose CDB alpha is < 0.995 (i.e. the database already
    # says it is see-through) - that is the ground truth we already ship.
    truth = {b for b in mats if 0.0 < alpha.get(b, 1.0) < 0.995}
    print()
    print("materials the CDB itself calls see-through (0 < alpha < 0.995): %d" % len(truth))
    hit_t = sum(1 for b in truth if bucket(b) == "translucent-word")
    print("   of those, the name heuristic would also call them translucent: %d (%.0f%%)"
          % (hit_t, 100.0 * hit_t / max(len(truth), 1)))
    print("   alpha distribution of translucent-word materials:")
    tv = sorted(alpha[b] for b in mats if bucket(b) == "translucent-word" and b in alpha)
    if tv:
        n = len(tv)
        print("      n=%d  p10=%.3f median=%.3f p90=%.3f" % (n, tv[n // 10], tv[n // 2], tv[9 * n // 10]))

    print()
    print("examples of translucent-word materials and their routes:")
    shown = 0
    for m in sorted(mats):
        if bucket(m) != "translucent-word":
            continue
        print("   %-58s %-22s alpha=%s"
              % (m[-58:], ",".join(sorted(routes.get(m, ()))) or "<none>",
                 ("%.3f" % alpha[m]) if m in alpha else "-"))
        shown += 1
        if shown >= a.examples:
            break
    return 0


if __name__ == "__main__":
    raise SystemExit(main())