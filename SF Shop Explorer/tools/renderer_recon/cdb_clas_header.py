"""6r-10: the CLAS blob starts with a CLASS HEADER record, not a field.

Measured from the raw bytes (cdb_clas_probe.py --raw): for every one of the
97 blobs the first 12-byte record is

    typeCode 0x0000, flags 0x0000, byteOffset 0, byteSize == nRecords - 1

while every subsequent record in the same blob has flags 0xffff (a real
field) or a non-zero type.  The leading u32 is the CLASS's own id: it is
what a DIFF record names to pick its component type.

cdb_schema.py's classes() returns the header as if it were field 0.  Three
consequences, all of which have been wrong in this project for three turns:

  1. every class's field list is offset by one, so "field k" was really
     field k-1;
  2. 97 class ids were counted as field ids, inflating the distinct-id
     census from (whatever the real number is) to 456;
  3. and this is the important one - with the header removed, the real
     field slots number 562 - 97 = 465, and the pool holds 456 names.  A
     global bag would have to hold one name per DISTINCT id; a per-type
     pool would hold one name per SLOT.  The gap is the number of slots
     whose name is shared with an earlier class, i.e. the deduplicated
     surplus.  Whether that surplus lands exactly on the observed id reuse
     is the test.

If the pool is in fact the concatenation of per-class runs in class order,
then class c's field k pairs with the k-th name emitted by class c, and the
field-name map that 6r-8d could not build becomes buildable.

    python cdb_clas_header.py [--dump]
"""
from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")


def blobs(b: bytes):
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", action="store_true")
    a = ap.parse_args()

    with open(CDB, "rb") as fh:
        b = fh.read()
    sys.path.insert(0, HERE)
    import cdb_schema as S
    pool = S.names(b)
    cls = blobs(b)

    # --- 1. prove the header record ------------------------------------
    print("== the leading record of each blob ==")
    ok_hdr = 0
    for recs in cls:
        fid, typ, fl, off, sz = recs[0]
        if typ == 0x0000 and fl == 0x0000 and off == 0 and sz == len(recs) - 1:
            ok_hdr += 1
    print("blobs whose record 0 matches (type=0,flags=0,off=0,size=n-1): %d / %d"
          % (ok_hdr, len(cls)))

    # Contrast: how many SUBSEQUENT records have that shape?
    cont = sum(1 for recs in cls for r in recs[1:]
               if r[1] == 0x0000 and r[2] == 0x0000 and r[3] == 0)
    print("later records with the same shape: %d" % cont)

    class_ids = [recs[0][0] for recs in cls]
    fields = [[r for r in recs[1:]] for recs in cls]

    # --- 2. corrected census -------------------------------------------
    slots = sum(len(f) for f in fields)
    distinct_field_ids = {r[0] for f in fields for r in f}
    distinct_class_ids = set(class_ids)
    print("\n== corrected census ==")
    print("classes                 : %d" % len(cls))
    print("field slots (was 562)   : %d" % slots)
    print("distinct field ids      : %d" % len(distinct_field_ids))
    print("distinct class ids      : %d" % len(distinct_class_ids))
    print("class id also a field id: %d"
          % len(distinct_class_ids & distinct_field_ids))
    print("union                   : %d" % len(distinct_class_ids | distinct_field_ids))
    print("name pool               : %d" % len(pool))

    # --- 3. the deduplicated surplus -------------------------------------
    per_id = collections.Counter(r[0] for f in fields for r in f)
    surplus = slots - len(distinct_field_ids)
    print("\n== surplus accounting ==")
    print("slots - distinct field ids = %d" % surplus)
    reuse = collections.Counter(per_id.values())
    print("field-id reuse histogram   = %s" % dict(sorted(reuse.items())))
    expected_surplus = sum((n - 1) * c for n, c in reuse.items())
    print("sum over ids of (uses-1)   = %d  (== surplus by construction)" % expected_surplus)
    print("pool - distinct field ids  = %d"
          % (len(pool) - len(distinct_field_ids)))
    print("pool - slots              = %d" % (len(pool) - slots))

    # --- 4. is the pool per-type? ---------------------------------------
    # Necessary condition for a per-type pool in class order: the number of
    # names emitted must equal pool length.  Names are emitted once per
    # CLASS (its own name) plus once per FIELD slot, deduped.
    emitted = []
    seen = set()
    for recs, frs in zip(cls, fields):
        for r in [recs[0]] + frs:  # class name then field names
            if r[0] not in seen:
                seen.add(r[0])
                emitted.append(r[0])
    print("\n== per-type pool test ==")
    print("ids emitted (class + fields, deduped, class order) = %d" % len(emitted))
    print("pool length                                          = %d" % len(pool))
    print("match: %s" % (len(emitted) == len(pool)))
    print("deficit (pool has names no id explains) = %d"
          % (len(pool) - len(emitted)))
    if len(pool) > len(emitted):
        print("  -> pool carries container-schema names too (DBFileIndex etc.)")
    print("surplus (id no name) = %d" % (len(emitted) - len(pool)))

    # --- 5. the real test: can we pair them? ----------------------------
    # Walk classes in order, consuming pool names for class name + fields,
    # allowing a skip for container-schema names at the head.  If the pool
    # is per-type in class order this walk stays in step for every class.
    print("\n== alignment walk ==")
    # container schema occupies the head of the pool
    mat_at = pool.index("BSMaterial::TextureSetID")
    print("container schema occupies pool[0:%d]" % mat_at)
    pos = mat_at
    total_slots = 0
    class_hits = 0
    for ci, (recs, frs) in enumerate(zip(cls, fields)):
        want = 1 + len(frs)
        if pos + want > len(pool):
            break
        chunk = pool[pos:pos + want]
        # a class name should look like a component name; its fields should
        # not.  Count how often the first chunk entry looks like a type name.
        if "::" in chunk[0] or chunk[0][:1].isupper():
            class_hits += 1
        pos += want
        total_slots += want
    print("classes walked: %d of %d, names consumed %d of %d"
          % (total_slots, len(cls), pos - mat_at, len(pool) - mat_at))
    print("chunks whose first entry looks like a type/class name: %d" % class_hits)
    print("(expected %d if the walk stayed in step)" % total_slots)

    if a.dump:
        print("\n== classes with corrected field lists ==")
        for ci, (recs, frs) in enumerate(zip(cls, fields)):
            print("class %2d id=0x%04x fields=%d %s"
                  % (ci, recs[0][0], len(frs),
                     " ".join("0x%04x" % r[0] for r in frs)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())