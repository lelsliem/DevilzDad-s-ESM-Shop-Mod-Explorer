"""Is the CDB name pool per-class or global?  The decisive structural test.

The pool holds exactly as many strings as there are distinct field ids, with
no duplicates.  Two readings survive that:

  (a) GLOBAL BAG.  One name per distinct property, shared by every class
      that declares it.  Then the pool order is the order properties were
      first encountered and carries no per-class information, and 6r-8d's
      conclusion stands.

  (b) PER-TYPE ORDERED RUNS.  The pool is the concatenation, in class order,
      of each class's own field names.  Then field descriptor k of class c
      pairs with pool[offset(c) + k] - and 6r-8d's blocker is gone.

(b) is testable without knowing any name: sum the class field counts and
compare with the pool length, and check whether a single set of run
boundaries reproduces both.  If the totals disagree, (b) is dead and only
value-based inference remains.

    python cdb_field_names2.py
"""
from __future__ import annotations

import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdb_schema as S  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")


def main() -> int:
    b = S.load(CDB)
    pool = S.names(b)
    cls = S.classes(b)

    total_fields = sum(len(r) for r in cls)
    distinct_ids = len({f for r in cls for f, _, _, _, _ in r})
    print("pool strings        : %d" % len(pool))
    print("class field slots   : %d  (sum over %d classes)" % (total_fields, len(cls)))
    print("distinct field ids  : %d" % distinct_ids)
    print()
    print("per-class runs would need pool length == sum of field counts")
    print("  pool %d vs slots %d -> %s"
          % (len(pool), total_fields, "MATCH" if len(pool) == total_fields else "MISMATCH"))
    print("global-bag would need pool length == distinct ids")
    print("  pool %d vs ids %d   -> %s"
          % (len(pool), distinct_ids, "MATCH" if len(pool) == distinct_ids else "MISMATCH"))

    # Each id appears how many times?  Under (a) a shared property is
    # declared by several classes, so ids repeat across the CLAS blobs and
    # the pool cannot have a slot for each occurrence.
    per_id = collections.Counter(f for r in cls for f, _, _, _, _ in r)
    hist = collections.Counter(per_id.values())
    print("\nid reuse across classes (id -> number of classes declaring it):")
    for ndecl in sorted(hist):
        ids = [i for i, c in per_id.items() if c == ndecl]
        print("  declared by %2d classes: %4d ids  e.g. %s"
              % (ndecl, len(ids), " ".join("0x%04x" % i for i in sorted(ids)[:5])))

    # Now look at the pool's own structure.  If it were per-class runs, the
    # pool would start with a run whose length equals class 0's field count
    # and the names in it would be properties, not container vocabulary.
    print("\npool head, grouped to see whether it looks like field names:")
    for i in range(0, 96, 8):
        print("  %4d: %s" % (i, " | ".join(pool[i:i + 8])))
    print("\nclass 0 field count: %d, class 1: %d, class 2: %d"
          % (len(cls[0]), len(cls[1]), len(cls[2])))

    # The single most useful discriminator: does any prefix of the pool
    # look like class 0's own field names?  We cannot know the names, but a
    # per-type pool would have the *class* names near it.  Print classes
    # whose name we can guess (CamelCase with BSMaterial/BSComponent) and
    # see whether such names sit adjacent to runs.
    comp_names = [n for n in pool if "Component" in n or "BSMaterial" in n]
    print("\ncomponent/class-like names in pool: %d" % len(comp_names))
    print("  first 20: %s" % comp_names[:20])

    # Count how many pool entries look like container vocabulary rather than
    # material properties.  A high proportion means the pool is a schema
    # dictionary (class names + component names + property names mixed),
    # which is reading (a) with extra decoration.
    propish = [n for n in pool if n[0].islower() or "_" in n]
    print("\npool entries that look like snake_case/lowercase properties: %d / %d"
          % (len(propish), len(pool)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())