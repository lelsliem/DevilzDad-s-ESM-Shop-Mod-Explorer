"""Find archive entries by name substring, using the name table only.

Parsing a 4 GB texture archive to answer "which .nif is the tomato?" wastes
minutes; the name table is a few MB and `ba2lib.Ba2Archive` already skips
payload reads, so this indexes names and filters.

    python find_nif.py --data "C:\\...\\Data" --glob "*.ba2" --match tomato
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ba2lib import Ba2Archive  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    ap.add_argument("--glob", action="append", default=None)
    ap.add_argument("--match", required=True, help="case-insensitive substring")
    ap.add_argument("--ext", default="", help="also require this extension, e.g. .nif")
    ap.add_argument("--limit", type=int, default=80)
    args = ap.parse_args()

    data = Path(args.data)
    pats = args.glob or ["*.ba2"]
    hits: list[str] = []
    seen: set[str] = set()
    files: list[Path] = []
    for pat in pats:
        files += sorted(data.glob(pat))
    files = [f for f in files if not (f in seen or seen.add(f))]
    print("scanning %d archive(s)..." % len(files))
    for p in files:
        ar = Ba2Archive(p)
        if not ar.valid:
            continue
        names = list(ar.entries) + list(ar.tex_files)
        for nm in names:
            if args.match.lower() in nm.lower() and (not args.ext or nm.endswith(args.ext)):
                hits.append("%-46s %s" % (p.name, nm))
    print("%d hit(s) for %r%s" % (len(hits), args.match, " ext=" + args.ext if args.ext else ""))
    for h in hits[: args.limit]:
        print("  " + h)
    if len(hits) > args.limit:
        print("  ... %d more" % (len(hits) - args.limit))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())