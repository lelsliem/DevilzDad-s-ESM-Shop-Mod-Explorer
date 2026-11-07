"""Field ids as INDICES into the full STRT string table.

The numbers line up suspiciously well and 6r-10 nearly missed it:

  * the STRT section declares 8573 strings (cdb_strt.py);
  * field ids run from 0x0000 to 0x2175 = 8565 - eight below 8573.

If a field id is an index into that table, then every one of the 359
distinct field ids and all 97 class ids has a NAME, and the 41 unknowns
stop being unknown.  This is the mapping 6r-8d was looking for and could
not find, because it was hunting for a second pool when the answer was
the first one.

The test is not "do the names look plausible" - every name in the table is
a real string, so plausibility proves nothing.  The tests are:

  1. BOUNDS.  Every id must be < 8573.
  2. NON-COLLISION.  456 distinct ids must yield 456 DISTINCT strings.  A
     wrong offset usually collides; a right one cannot, because the pool
     has no duplicates.
  3. THE KILLER TEST.  97 class ids must land on type-shaped names
     ("BSMaterial::AlphaSettingsComponent", "XMFLOAT4", ...).  Only ~100
     of 8573 strings are type-shaped, so a correct index map puts a class
     id on a type name essentially every time, and any offset error
     destroys that instantly.  Chance rate is about 1.2%.
  4. NULL.  The same statistic under 2000 shuffles, to prove test 3
     discriminates rather than merely being high.

    python cdb_strindex.py [--dump]
"""
from __future__ import annotations

import argparse
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")

TYPE_TOKENS = {
    "XMFLOAT", "XMFLOAT2", "XMFLOAT3", "XMFLOAT4", "XMVECTOR", "bool",
    "float", "uint8_t", "uint16_t", "uint32_t", "int32_t", "string",
}


def strt(b: bytes) -> list[str]:
    """Exactly `count` NUL-terminated strings from 0x18."""
    (count,) = struct.unpack_from("<I", b, 0x14)
    out, o = [], 0x18
    for _ in range(count):
        j = b.find(b"\x00", o)
        if j < 0:
            break
        out.append(b[o:j].decode("latin-1"))
        o = j + 1
    return out


def classes(b: bytes):
    tag = b.find(b"TYPE", 0x2000, 0x4000)
    _v, _ver, count = struct.unpack_from("<III", b, tag)
    p = tag + 12
    out = []
    for _ in range(count):
        assert b[p:p + 4] == b"CLAS"
        size = struct.unpack_from("<I", b, p + 4)[0]
        blob = b[p + 8:p + 8 + size]
        recs = [struct.unpack_from("<IHHHH", blob, k) for k in range(0, len(blob), 12)]
        out.append(recs)
        p += 8 + size
    return out


def type_shaped(s: str) -> bool:
    return "::" in s or s in TYPE_TOKENS


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", action="store_true")
    a = ap.parse_args()

    with open(CDB, "rb") as fh:
        b = fh.read()
    table = strt(b)
    cls = classes(b)

    class_ids = [recs[0][0] for recs in cls]
    field_ids = sorted({r[0] for recs in cls for r in recs[1:]})
    all_ids = sorted(set(class_ids) | set(field_ids))

    print("STRT strings        : %d" % len(table))
    print("class ids           : %d  (range 0x%04x..0x%04x)"
          % (len(class_ids), min(class_ids), max(class_ids)))
    print("distinct field ids  : %d  (range 0x%04x..0x%04x)"
          % (len(field_ids), min(field_ids), max(field_ids)))
    print("max id              : %d   table size %d   slack %d"
          % (max(all_ids), len(table), len(table) - 1 - max(all_ids)))
    print()

    print("== test 1: bounds ==")
    oob = [i for i in all_ids if i >= len(table)]
    print("ids outside the table: %d %s" % (len(oob), oob[:10]))

    print("\n== test 2: non-collision ==")
    names = [table[i] for i in all_ids]
    print("456 ids -> %d distinct strings (collisions: %d)"
          % (len(set(names)), len(names) - len(set(names))))

    print("\n== test 3: class ids must land on type-shaped names ==")
    hit = sum(1 for i in class_ids if type_shaped(table[i]))
    print("  observed : %d / %d  (%.1f%%)" % (hit, len(class_ids),
                                             100.0 * hit / len(class_ids)))
    ntyped = sum(1 for s in table if type_shaped(s))
    print("  type-shaped strings in the table: %d / %d  -> chance %.2f%%"
          % (ntyped, len(table), 100.0 * ntyped / len(table)))
    rng = random.Random(99)
    ctrl = []
    idxs = class_ids[:]
    for _ in range(2000):
        rng.shuffle(idxs)
        ctrl.append(sum(1 for i in idxs if type_shaped(table[i])))
    ctrl.sort()
    print("  null p50 %d  p95 %d  p99 %d  max %d"
          % (ctrl[len(ctrl) // 2], ctrl[int(0.95 * len(ctrl))],
             ctrl[int(0.99 * len(ctrl))], ctrl[-1]))
    p = sum(1 for c in ctrl if c >= hit) / len(ctrl)
    print("  p(shuffle >= observed) = %.4f" % p)
    print("  verdict: %s" % ("INDEX MAP CONFIRMED" if p < 0.005
                             else "INDEX MAP NOT SUPPORTED"))

    if a.dump:
        print("\n== class id -> name ==")
        for ci, i in enumerate(class_ids):
            print("  class %2d 0x%04x -> %s" % (ci, i, table[i]))
        print("\n== field id -> name ==")
        for i in field_ids:
            used = sum(1 for recs in cls for r in recs[1:] if r[0] == i)
            print("  0x%04x  %-46s  %d classes" % (i, table[i], used))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())