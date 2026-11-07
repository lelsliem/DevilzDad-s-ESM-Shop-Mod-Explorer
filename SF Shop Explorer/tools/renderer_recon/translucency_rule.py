"""Pick the best available translucency signal, against the CDB's own opinion.

route_census.py killed the shader-route idea: only 4% of materials carry a
route record and its distribution is the same for glass as for metal, so it
cannot decide translucency. This tests the two remaining candidates against
ground truth - the set of materials the database itself marks see-through by
storing an override alpha below 0.995:

    A  the shipped name-token list (glass/clear/translucent/transpar)
    B  "this material's texture set names an _opacity/_alpha map", which is
       the game's own statement that the shader blends
    C  the union, with the CDB alpha taking priority over both

B has ~100% coverage because the texture-set index already exists, and it is
a statement about the shader rather than about the spelling of a name.

    python translucency_rule.py
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

SUFFIX = re.compile(r"_(material|textureset|layer|blender|uvstream[0-9a-z]*|cell|permutationlayer)[0-9]*$", re.I)
NAME_TOKENS = ("glass", "clear", "translucent", "transpar")
OPAQUE_WORDS = ("metal", "wood", "stone", "concrete", "rubber", "leather", "cardboard")


def base_of(name: str) -> str:
    return SUFFIX.sub("", name).lower().replace("\\", "/")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tokens", type=int, default=24)
    a = ap.parse_args()
    b = S.load()

    alpha: dict[str, float] = {}
    has_opacity_map: set[str] = set()
    has_color_map: set[str] = set()
    is_material: set[str] = set()
    for name, recs in R.objects(b):
        base = base_of(name)
        low = name.lower()
        if "_material" in low:
            is_material.add(base)
        for _, fid, kind, cnt, pay in recs:
            if fid == 0x025A:
                v = R.decode(0, 0, pay)
                if isinstance(v, list) and len(v) == 4:
                    alpha.setdefault(base, v[3])
            elif fid == 0x038A:
                v = R.decode(kind, cnt, pay)
                if not isinstance(v, str):
                    continue
                vl = v.lower()
                if vl.endswith("_opacity.dds") or vl.endswith("_alpha.dds"):
                    has_opacity_map.add(base)
                elif vl.endswith("_color.dds"):
                    has_color_map.add(base)

    mats = is_material
    truth = {m for m in mats if 0.0 < alpha.get(m, 1.0) < 0.995}
    print("material bases %d   see-through per the CDB %d (%.1f%%)"
          % (len(mats), len(truth), 100.0 * len(truth) / max(len(mats), 1)))
    print("bases whose texture set names an opacity map: %d" % len(has_opacity_map))

    def named(b):
        stem = b.rsplit("/", 1)[-1]
        return any(t in stem for t in NAME_TOKENS)

    def opaque_word(b):
        stem = b.rsplit("/", 1)[-1]
        return any(t in stem for t in OPAQUE_WORDS)

    def score(fn, label):
        hit = sum(1 for m in truth if fn(m))
        print("  %-46s recall %6d / %d  = %5.1f%%" % (label, hit, len(truth), 100.0 * hit / max(len(truth), 1)))
        return hit

    print()
    print("recall on the CDB's own see-through set:")
    a_hit = score(named, "A  name tokens (shipped fallback)")
    b_hit = score(lambda m: m in has_opacity_map, "B  texture set has an opacity/alpha map")
    c_hit = score(lambda m: named(m) or m in has_opacity_map, "C  A or B")
    # precision: how often each rule fires on something the CDB calls opaque
    opq = {m for m in mats if m not in truth}
    for fn, label in ((named, "A"), (lambda m: m in has_opacity_map, "B"),
                      (lambda m: named(m) or m in has_opacity_map, "C")):
        fired = sum(1 for m in opq if fn(m))
        print("  %-46s fires on %6d opaque materials (%.1f%% of %d)" % (label, fired, 100.0 * fired / max(len(opq), 1), len(opq)))

    # Which name tokens do the see-through materials actually contain?
    # This is the data-derived replacement for the four guessed ones.
    toks = collections.Counter()
    toks_opq = collections.Counter()
    for m in truth:
        stem = m.rsplit("/", 1)[-1]
        for t in re.findall(r"[a-z]+", stem):
            if len(t) >= 4:
                toks[t] += 1
    for m in opq:
        stem = m.rsplit("/", 1)[-1]
        for t in re.findall(r"[a-z]+", stem):
            if len(t) >= 4:
                toks_opq[t] += 1
    print()
    print("name tokens most enriched in see-through materials (count, ratio vs opaque):")
    rows = []
    for t, n in toks.items():
        if n < 8:
            continue
        ratio = n / max(toks_opq.get(t, 0), 1)
        rows.append((ratio, n, t))
    rows.sort(reverse=True)
    for ratio, n, t in rows[:a.tokens]:
        print("    %-22s n=%-6d ratio=%.2f" % (t, n, ratio))

    # And: what do see-through materials WITHOUT an opacity map look like?
    nomap = [m for m in truth if m not in has_opacity_map]
    print()
    print("see-through materials with no opacity map: %d of %d" % (len(nomap), len(truth)))
    print("   of those the name tokens still catch: %d (%.0f%%)"
          % (sum(1 for m in nomap if named(m)), 100.0 * sum(1 for m in nomap if named(m)) / max(len(nomap), 1)))
    print("   examples:")
    for m in sorted(nomap)[:10]:
        print("      %-70s alpha=%.3f" % (m[-70:], alpha[m]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())