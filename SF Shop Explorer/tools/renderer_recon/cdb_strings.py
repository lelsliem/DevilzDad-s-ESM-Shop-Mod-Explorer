"""Cache the printable-string runs of materialsbeta.cdb for fast grepping.

The CDB interleaves names, field labels and texture paths with binary
record data, so a linear scan of the 105 MB blob is the only way to see
them. Scanning once and writing "offset<TAB>string" makes every later
question a plain grep instead of a 105 MB read.

    python cdb_strings.py build            # (re)build the cache
    python cdb_strings.py find <substring> # grep the cache, print context
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")
CACHE = os.path.join(HERE, "extracted", "cdb_strings.tsv")
BS = chr(92)


def build():
    with open(CDB, "rb") as fh:
        blob = fh.read()
    out = []
    i = 0
    n = len(blob)
    while i < n:
        b = blob[i]
        if 0x20 <= b <= 0x7E:
            j = i
            while j < n and 0x20 <= blob[j] <= 0x7E:
                j += 1
            out.append("%d\t%s\n" % (i, blob[i:j].decode("ascii")))
            i = j
        else:
            i += 1
    with open(CACHE, "w", encoding="utf-8", newline="") as fh:
        fh.writelines(out)
    print("wrote %d strings (%d bytes) to %s" % (len(out), os.path.getsize(CACHE), CACHE))


def load():
    rows = []
    with open(CACHE, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            off, _, s = line.rstrip("\n").partition("\t")
            try:
                rows.append((int(off), s))
            except ValueError:
                continue
    return rows


def find(needle, ctx=0, limit=200):
    rows = load()
    low = needle.lower()
    hits = [i for i, (_, s) in enumerate(rows) if low in s.lower()]
    print("%d hits for %r" % (len(hits), needle))
    for i in hits[:limit]:
        lo = max(0, i - ctx)
        hi = min(len(rows), i + ctx + 1)
        for j in range(lo, hi):
            mark = ">>" if j == i else "  "
            print("%s %10d %s" % (mark, rows[j][0], rows[j][1]))
        if ctx:
            print("-" * 60)
    if len(hits) > limit:
        print("... %d more" % (len(hits) - limit))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "find"
    if cmd == "build":
        build()
    else:
        find(sys.argv[2] if len(sys.argv) > 2 else "tomato",
             int(sys.argv[3]) if len(sys.argv) > 3 else 0)