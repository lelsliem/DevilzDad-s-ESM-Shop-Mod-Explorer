"""THE TEST: does pool position == (class name, then that class's field
names, deduplicated, classes in file order)?

6r-10 established, by measurement only:
  * every CLAS blob begins with a class-header record
    (type=0, flags=0, off=0, size=n-1); 78/97 match that shape exactly and
    NO later record in any blob has it, so the header is one record per
    class and the other 19 blobs simply have an unusual first field;
  * after removing headers: 465 field slots, 359 distinct field ids,
    97 distinct class ids, zero overlap -> 456 distinct ids;
  * the schema name pool is 456 strings (the STRT count of 8573 is the
    whole table; only the first 456 are schema names, the rest are
    material paths), zero duplicates;
  * emitting ids as (class name, then its fields, deduplicated, in class
    order) yields exactly 456 ids.

456 == 456 with zero leftover and zero unexplained is either the map or a
coincidence, so it gets a falsification test rather than a shrug.

The discriminating test: at the 97 positions where the emitted sequence
holds a CLASS id, the pool entry should look like a type name.  The pool
contains exactly 88 names containing "::" (BSMaterial::Foo, BSComponent::Bar)
plus a handful of plain type tokens (XMFLOAT4, XMFLOAT2).  Under a correct
map, class positions should hit those ~88-97 names almost every time.  Under
a false map the rate should be near chance.  Both rates are printed, plus
the same statistic for 200 random shuffles as the null distribution.

If the test passes, cdb_field_names.py --map emits the 359 field names and
the remaining 41 unknowns get real names instead of inference.

    python cdb_field_map.py [--emit] [--json out.json]
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")


def read(b: bytes):
    tag = b.find(b"TYPE", 0x2000, 0x4000)
    _v, _ver, count = struct.unpack_from("<III", b, tag)
    p = tag + 12
    classes = []
    for _ in range(count):
        assert b[p:p + 4] == b"CLAS"
        size = struct.unpack_from("<I", b, p + 4)[0]
        blob = b[p + 8:p + 8 + size]
        recs = [struct.unpack_from("<IHHHH", blob, k) for k in range(0, len(blob), 12)]
        classes.append(recs)
        p += 8 + size
    return classes


def schema_pool(b: bytes) -> list[str]:
    """The 456 schema names: walk STRT from 0x18 until the first entry that
    is not a plausible identifier.  The table's real count field (8573)
    covers material paths too, so it cannot be used as the bound."""
    out, o = [], 0x18
    while len(out) < 456:
        j = b.find(b"\x00", o)
        s = b[o:j]
        if not s or any(c < 32 or c > 126 for c in s):
            break
        out.append(s.decode("latin-1"))
        o = j + 1
    return out


def looks_like_type(name: str) -> bool:
    """A type/class name in this schema: qualified, or a type token, or
    CamelCase.  Deliberately generous - generosity is fine because the
    control uses the identical predicate."""
    if "::" in name:
        return True
    if name in ("XMFLOAT4", "XMFLOAT2", "XMFLOAT3", "XMFLOAT", "bool", "float",
                "uint8_t", "uint16_t", "uint32_t", "int32_t", "string"):
        return True
    return name[:1].isupper() and any(c.islower() for c in name)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--json", default="")
    a = ap.parse_args()

    with open(CDB, "rb") as fh:
        b = fh.read()
    classes = read(b)
    pool = schema_pool(b)

    # emitted id sequence: class id, then its field ids, deduped, class order
    emitted: list[int] = []
    is_class: list[bool] = []
    seen: set[int] = set()
    for recs in classes:
        for k, r in enumerate(recs):
            if r[0] in seen:
                continue
            seen.add(r[0])
            emitted.append(r[0])
            is_class.append(k == 0)

    print("emitted ids      : %d" % len(emitted))
    print("pool names       : %d" % len(pool))
    print("lengths equal    : %s" % (len(emitted) == len(pool)))
    print("class positions  : %d" % sum(is_class))
    print("pool type-shaped : %d" % sum(1 for n in pool if looks_like_type(n)))
    print()

    # ---- the discriminating statistic --------------------------------
    rng2 = random.Random(2024)
    hits = sum(1 for nm, isc in zip(pool, is_class) if isc and looks_like_type(nm))
    ncls = sum(is_class)
    print("== class positions landing on a type-shaped pool name ==")
    print("  observed: %d / %d  (%.1f%%)" % (hits, ncls, 100.0 * hits / ncls))
    print("  NOTE the predicate accepts %d of %d pool names, so chance alone"
          % (sum(1 for n in pool if looks_like_type(n)), len(pool)))
    print("  gives about %.1f%%.  This test cannot discriminate - see the"
          % (100.0 * sum(1 for n in pool if looks_like_type(n)) / len(pool)))
    print("  qualified-name test below, which is the one that carries signal.")

    # ---- the SHARP test ------------------------------------------------
    # Only 88 of the 456 pool names are qualified ("BSMaterial::Foo"). If the
    # map is real, those 88 must land on class positions.  If it is false
    # they land at the 88/456 = 19.3% base rate.  There is no in-between.
    q = [n for n in pool if "::" in n]
    qualified_hits = sum(1 for nm, isc in zip(pool, is_class) if isc and "::" in nm)
    print("\n== qualified names (\"Type::Member\") landing on class positions ==")
    print("  qualified names in pool : %d" % len(q))
    print("  class positions         : %d" % ncls)
    print("  observed overlaps       : %d" % qualified_hits)
    print("  chance expectation      : %.1f" % (len(q) * ncls / len(pool)))
    qual_ctrl = []
    for _ in range(2000):
        sh = pool[:]
        rng2.shuffle(sh)
        qual_ctrl.append(sum(1 for nm, i in zip(sh, is_class) if i and "::" in nm))
    qual_ctrl.sort()
    print("  null p50 %d  p95 %d  p99 %d  max %d"
          % (qual_ctrl[len(qual_ctrl) // 2], qual_ctrl[int(0.95 * len(qual_ctrl))],
             qual_ctrl[int(0.99 * len(qual_ctrl))], qual_ctrl[-1]))
    p_q = sum(1 for c in qual_ctrl if c >= qualified_hits) / len(qual_ctrl)
    print("  p(shuffle >= observed)  : %.4f" % p_q)

    ctrl = []
    for _ in range(200):
        sh = pool[:]
        rng2.shuffle(sh)
        ctrl.append(sum(1 for nm, i in zip(sh, is_class) if i and looks_like_type(nm)))
    ctrl.sort()
    print("  control : mean %.1f  p50 %d  p95 %d  max %d"
          % (sum(ctrl) / len(ctrl), ctrl[len(ctrl) // 2], ctrl[int(0.95 * len(ctrl))], ctrl[-1]))
    p_gt = sum(1 for c in ctrl if c >= hits) / len(ctrl)
    print("  p(shuffle >= observed) = %.4f" % p_gt)
    verdict = "MAP CONFIRMED" if p_q < 0.005 and qualified_hits > qual_ctrl[int(0.99 * len(qual_ctrl))] \
        else "MAP NOT SUPPORTED"
    print("\n  verdict: %s" % verdict)

    # ---- the inverse test: field positions should NOT be types ---------
    field_pos_type = sum(1 for nm, isc in zip(pool, is_class) if not isc and looks_like_type(nm))
    print("\n== field positions landing on a type-shaped name ==")
    print("  %d / %d field positions (%.1f%%)"
          % (field_pos_type, len(pool) - ncls,
             100.0 * field_pos_type / (len(pool) - ncls)))

    # ---- id ordering sanity -------------------------------------------
    print("\n== is the pool maybe just sorted ids? ==")
    print("  emitted sorted ascending: %s" % (emitted == sorted(emitted)))
    print("  first 12 emitted ids: %s" % ["0x%04x" % i for i in emitted[:12]])

    mapping = {}
    if verdict == "MAP CONFIRMED":
        for fid, nm in zip(emitted, pool):
            mapping[fid] = nm
        # sanity: print the mapped names for ids we already know by value
        print("\n== spot check against ids already named by value ==")
        known = {
            0x0150: "?",   # seen in 16 classes
            0x025a: "?",   # the override-colour field (FindCdbAlpha)
            0x026c: "?",   # FindCdbTint area
            0x0275: "?", 0x0277: "?", 0x0279: "?", 0x027b: "?",
        }
        for fid in sorted(known):
            print("  0x%04x -> %s" % (fid, mapping.get(fid, "?")))
        if a.emit:
            print("\n== full field-name map ==")
            field_names = {}
            for fid, nm in mapping.items():
                k = classes
                field_names[fid] = nm
            for fid in sorted(field_names):
                used = sum(1 for r in classes for x in r if x[0] == fid)
                print("  0x%04x  %-46s  (%d classes)" % (fid, field_names[fid], used))
        if a.json:
            with open(a.json, "w", encoding="utf-8") as fh:
                json.dump({("0x%04x" % k): v for k, v in sorted(mapping.items())},
                          fh, indent=1, sort_keys=True)
            print("\nwrote %s (%d entries)" % (a.json, len(mapping)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())