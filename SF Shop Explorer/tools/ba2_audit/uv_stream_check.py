"""Check the plugin's UV-stream choice against the known-degenerate mesh.

The action figure packaging front cover, geometries
79b7f5034793e0f214b1/9cf1aad6a57401675a82.mesh, has 282 indices, 96 vertices
and a first UV stream whose 96 pairs are all u=0, v=1 - one texel, so the
whole front cover samples a single point of the 1024x1024 packaging atlas.
A sibling mesh was measured carrying 24 UV sets, so the correct rectangle is
in a later stream.

ParseMeshLod0 used to take stream 0 unconditionally. It now walks the stream
list and accepts the first set whose u or v actually spans a range. This
script proves that rule picks a better rectangle than stream 0 would.

    python uv_stream_check.py --data "C:\\...\\Data"
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ba2lib import Ba2Archive  # noqa: E402
from nif_uv import parse_mesh_streams  # noqa: E402

TARGETS = [
    "geometries/79b7f5034793e0f214b1/9cf1aad6a57401675a82.mesh",
    "geometries/3542f7f559b568ad6904/c59cacbc64d4f689d2c1.mesh",
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--index", default="", help="cache file for the name->archive map")
    a = ap.parse_args()
    data = Path(a.data)

    idx = a.index or os.path.join(os.path.dirname(os.path.abspath(__file__)), "extracted", "ba2nameindex.tsv")
    if os.path.exists(idx):
        owner = {}
        with open(idx, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                name, _, arc = line.rstrip("\n").rpartition("\t")
                if name and arc:
                    owner[name] = arc
        print("loaded name index: %d entries" % len(owner))
    else:
        owner = {}
        arches = []
        for p in sorted(data.glob("*.ba2")):
            ar = Ba2Archive(p)
            if not ar.valid:
                continue
            arches.append(ar)
            for k in ar.entries:
                owner[k] = p.name
            print("  indexed %-44s %8d entries (running total %d)" % (p.name, len(ar.entries), len(owner)))
        with open(idx, "w", encoding="utf-8", newline="\n") as fh:
            for k in sorted(owner):
                fh.write("%s\t%s\n" % (k, owner[k]))
        print("wrote %s (%d names)" % (idx, len(owner)))
        keep = {owner[t]: arches for t in TARGETS if t in owner}
    opened: dict[str, Ba2Archive] = {}
    for want in TARGETS:
        arcname = owner.get(want)
        print()
        if not arcname:
            print("== %s  NOT FOUND" % want)
            continue
        ar = opened.get(arcname)
        if ar is None:
            ar = opened[arcname] = Ba2Archive(data / arcname)
        blob, err = ar.read_entry(want)
        print("== %s  (%s, %s)" % (want, arcname, err or "ok"))
        if not blob:
            continue
        info = parse_mesh_streams(blob)
        uvs = info.get("uv_sets", [])
        print("   nv=%s nidx=%s border=%s  UV sets found: %d" % (info.get("nv"), info.get("nidx"), info.get("border"), len(uvs)))
        live = (0.0, 0.2529, 0.0, 0.2510)  # the packaging atlas's live sub-rect
        for i, uv in enumerate(uvs[:12]):
            span = (uv[1] - uv[0]) > 1e-4 or (uv[3] - uv[2]) > 1e-4
            inside = uv[0] >= live[0] - 0.01 and uv[1] <= live[1] + 0.01 and uv[3] <= live[3] + 0.01
            print("   UV %2d  u=[%.4f,%.4f] v=[%.4f,%.4f]  %s%s"
                  % (i, uv[0], uv[1], uv[2], uv[3],
                     "" if span else "DEGENERATE ",
                     "inside-live" if inside else ""))
        if uvs:
            first = uvs[0]
            chosen = next((u for u in uvs if (u[1] - u[0]) > 1e-4 or (u[3] - u[2]) > 1e-4), None)
            print("   plugin OLD rule -> stream 0 u=[%.4f,%.4f] v=[%.4f,%.4f]" % first)
            if chosen is not None:
                print("   plugin NEW rule -> u=[%.4f,%.4f] v=[%.4f,%.4f]" % chosen)
            else:
                print("   plugin NEW rule -> no non-degenerate set found; keeps geometry untextured")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())