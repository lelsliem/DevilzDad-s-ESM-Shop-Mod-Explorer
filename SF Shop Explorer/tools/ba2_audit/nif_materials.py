"""Extract one .nif out of a BA2 and list every material it references.

The plugin resolves an albedo from the material string a NIF's
BSLightingShaderProperty carries. When an item renders grey, the only
useful question is "which .mat does this NIF actually name?", and that
string is a plain header-block string - so a printable-run scan is
sufficient and far quicker than a full block-tree parse.

    python nif_materials.py --data "C:\\...\\Data" \\
        --glob "Starfield - Meshes01.ba2" \\
        --nif "meshes/items/.../food_produce_tomato.nif" [--save out.nif]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ba2lib import Ba2Archive  # noqa: E402


def material_strings(blob: bytes) -> list[str]:
    """Every .mat and .dds path a NIF names, from its printable runs.

    Factored out of main() so mod_material_trace.py can reuse it: the same
    scan answers "what does this NIF ask for?" for a base-game and a mod
    archive alike. It returns texture references as well as materials, so a
    caller that only wants materials should filter on `.mat`.
    """
    runs = [(m.start(), m.group().decode("latin-1")) for m in re.finditer(rb"[ -~]{4,}", blob)]
    seen: set[str] = set()
    out: list[str] = []
    for _, s in runs:
        low = s.lower()
        if not (low.endswith(".mat") or low.endswith(".dds")):
            continue
        if s in seen:
            continue
        seen.add(s)
        out.append(s)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    ap.add_argument("--glob", action="append", required=True)
    ap.add_argument("--nif", required=True, help="archive-relative entry name")
    ap.add_argument("--save", help="also write the extracted NIF here")
    ap.add_argument("--uv", action="store_true", help="also dump the UV set block names")
    args = ap.parse_args()

    data = Path(args.data)
    want = args.nif.lower()
    blob = None
    for pat in args.glob:
        for p in sorted(data.glob(pat)):
            ar = Ba2Archive(p)
            if not ar.valid:
                continue
            ent, err = ar.read_entry(want)
            if ent is None:
                continue
            blob = ent
            print("read %s :: %s" % (p.name, args.nif))
            break
        if blob:
            break
    if not blob:
        print("[FAIL] entry not found: %s" % args.nif)
        return 1
    if args.save:
        Path(args.save).write_bytes(blob)
        print("saved %s (%d bytes)" % (args.save, len(blob)))

    runs = [(m.start(), m.group().decode("latin-1")) for m in re.finditer(rb"[ -~]{4,}", blob)]
    print("%d printable runs" % len(runs))
    seen: set[str] = set()
    for off, s in runs:
        low = s.lower()
        keep = (low.endswith(".mat") or "material" in low or
                low.endswith(".dds") or
                (args.uv and "uvstream" in low) or
                low.endswith(".tga") or low.endswith(".png"))
        if not keep or s in seen:
            continue
        seen.add(s)
        print("  %8d %s" % (off, s))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())