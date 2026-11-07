"""How much override colour is the plugin throwing away?

`cdb_records` decodes field 0x25a as a float4, and 6r-6b shipped only its w
(the alpha). The rgb is the material's own tint and is discarded. This
measures what using it would buy, and in particular how many of the
textureless - flat, no albedo - materials it would reach, since those are the
ones a shopper sees as untextured blobs.

    python override_tint_census.py
"""
from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cdb_records as R  # noqa: E402
import cdb_schema as S  # noqa: E402

BS = chr(92)
SUF = re.compile(r"_(material|textureset|layer|blender|uvstream[0-9a-z]*|cell|permutationlayer)[0-9]*$", re.I)


def base_of(name: str) -> str:
    return SUF.sub("", name).lower().replace(BS, "/")


def main() -> int:
    b = S.load()
    color: dict[str, list] = {}
    has_colour_map: set[str] = set()
    mats: set[str] = set()
    for name, recs in R.objects(b):
        bs = base_of(name)
        low = name.lower()
        if "_material" in low:
            mats.add(bs)
        if "_textureset" in low:
            has_colour_map.add(bs)
        for _, fid, kind, cnt, pay in recs:
            if fid == 0x038A:
                v = R.decode(kind, cnt, pay)
                if isinstance(v, str) and v.lower().endswith("_color.dds"):
                    has_colour_map.add(bs)
            elif fid == 0x025A:
                v = R.decode(0, 0, pay)
                if isinstance(v, list) and len(v) == 4:
                    color.setdefault(bs, v)

    flat = {m for m in mats if m not in has_colour_map}
    with_col = [m for m in mats if m in color]
    tinted = [m for m in with_col if max(color[m][:3]) < 0.97]
    flat_tinted = [m for m in flat if m in color and max(color[m][:3]) < 0.97]
    print("material bases                %6d" % len(mats))
    print("no colour map (render flat)   %6d  (%.1f%%)" % (len(flat), 100.0 * len(flat) / max(len(mats), 1)))
    print("carry a CDB override colour   %6d  (%.1f%%)" % (len(with_col), 100.0 * len(with_col) / max(len(mats), 1)))
    print("override is visibly tinted    %6d  (%.1f%% of those that carry one)"
          % (len(tinted), 100.0 * len(tinted) / max(len(with_col), 1)))
    print()
    print("flat AND tinted:              %6d  <- these render as an untextured blob today" % len(flat_tinted))
    for m in sorted(flat_tinted)[:14]:
        c = color[m]
        print("   %-58s rgb=%.2f %.2f %.2f  a=%.2f" % (m[-58:], c[0], c[1], c[2], c[3]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())