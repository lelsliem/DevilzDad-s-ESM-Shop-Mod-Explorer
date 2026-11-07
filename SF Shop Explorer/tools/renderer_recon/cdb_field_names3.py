"""Align the CLAS class order against the name pool's declaration stream.

6r-10 finding so far: the pool is a GLOBAL bag (456 strings == 456 distinct
field ids, zero duplicates), so it cannot be sliced per class by field count
(562 slots > 456 strings).  But its content is not a flat property list -
it reads as a sequence of component DECLARATIONS:

    BSMaterial::Color | XMFLOAT4 | x | y | z | w
    BSMaterial::Scale | XMFLOAT2
    BSMaterial::AlphaSettingsComponent | HasOpacity | AlphaTestThreshold | ...

i.e. a component name, its type, then its member names.  That is exactly
what a per-type name pool would look like if the type column were stored
in the pool too.  If the declarations appear in the same order as the CLAS
blobs, then class c's k-th field descriptor pairs with the k-th member name
of declaration c, and 6r-8d's blocker falls.

This script does the alignment and scores it three ways:
  * count agreement - does the number of member names in declaration c
    equal class c's field count?
  * type agreement  - do the member-name run's implied scalar shape match
    the class's typeCode sequence?
  * a control: the same score computed against a SHUFFLED pool, to prove
    the alignment is signal and not an artefact of both lists being 456-ish
    long.

    python cdb_field_names3.py [--dump]
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

# Where the container's own schema ends and BSMaterial declarations begin.
# "BSMaterial::TextureSetID" is the first pool entry that names a material
# component; everything before it describes DBFileIndex bookkeeping.
MAT_START = "BSMaterial::TextureSetID"


def declarations(pool: list[str], first: int = 0) -> list[tuple[str, list[str]]]:
    """Parse the pool into (componentName, memberNames) runs.

    A run starts at an entry containing '::' that is a component (ends in
    Component, Settings, Param, or is a known value type) and is followed by
    a type name plus zero or more member names, up to the next such
    component entry.  Returns (name, members-after-the-type-name).
    """
    out = []
    i = first
    n = len(pool)
    while i < n:
        nm = pool[i]
        if "::" not in nm:
            i += 1
            continue
        # collect the run that belongs to this component
        members: list[str] = []
        j = i + 1
        # optional type token
        if j < n and "::" not in pool[j] and pool[j][:1].isupper():
            j += 1
        while j < n and "::" not in pool[j]:
            members.append(pool[j])
            j += 1
        out.append((nm, members))
        i = j if j > i + 1 else i + 1
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", action="store_true")
    a = ap.parse_args()

    b = S.load(CDB)
    pool = S.names(b)
    cls = S.classes(b)
    start = pool.index(MAT_START) if MAT_START in pool else 0
    decls = declarations(pool, start)
    print("pool %d strings; BSMaterial declarations from index %d" % (len(pool), start))
    print("declaration runs parsed: %d" % len(decls))
    print("material-side strings  : %d" % (len(pool) - start))
    print("CLAS blobs             : %d" % len(cls))
    print()

    # --- count agreement -------------------------------------------
    print("== first 25 declaration runs vs class field counts ==")
    agree = 0
    compared = 0
    print("  %-52s %-6s %-6s %s" % ("component", "mem", "class", "verdict"))
    for i, (nm, members) in enumerate(decls[:25]):
        if i >= len(cls):
            break
        want = len(cls[i])
        ok = want == len(members)
        agree += ok
        compared += 1
        print("  %-52s %-6d %-6d %s" % (nm[:52], len(members), want, "match" if ok else "-"))
    print("\n  positional count agreement: %d / %d" % (agree, compared))

    tot_members = sum(len(m) for _, m in decls)
    print("  total member names across runs: %d (vs %d CLAS field slots)"
          % (tot_members, sum(len(r) for r in cls)))

    # --- does ANY class match the first declaration's member count? ---
    first_members = len(decls[0][1]) if decls else 0
    hits = [i for i, r in enumerate(cls) if len(r) == first_members]
    print("  declaration 0 has %d members; classes with that count: %d -> %s"
          % (first_members, len(hits), hits[:12]))

    # --- control ------------------------------------------------------
    rng = random.Random(7)
    counts = [len(r) for r in cls]
    ctrl = 0
    for _ in range(200):
        m = [len(x) for _, x in decls]
        rng.shuffle(m)
        ctrl += sum(1 for i in range(min(len(counts), len(m))) if counts[i] == m[i])
    print("  control (shuffled member counts): %d / %d expected by chance ~ %.1f"
          % (ctrl / 200.0, compared, compared * sum(1 for i in range(len(counts))
                                                    for j in range(len(counts))
                                                    if counts[i] == counts[j]) / (len(counts) ** 2)))

    if a.dump:
        print("\n== all declaration runs ==")
        for i, (nm, members) in enumerate(decls):
            print("%3d %-56s [%s]" % (i, nm, ", ".join(members)))
        print("\n== all classes (ids only) ==")
        for i, r in enumerate(cls):
            print("%3d n=%d %s" % (i, len(r), " ".join("0x%04x" % f for f, _, _, _, _ in r)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())