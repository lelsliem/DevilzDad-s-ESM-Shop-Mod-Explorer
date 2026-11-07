"""Measure the warm-hue gate: the liquid mesh's albedo footprint per flavour.

The preview's drink fill takes a maroon bias, but only when the flavour's own
colour map is warm. Pick the threshold from the shipped maps rather than from a
whole-atlas average: the atlas average is mostly film and cap, while the drink
only ever samples its own footprint.

    python _drink_warm_gate_tmp.py
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

from drinkbag_uv_footprint import geometry, footprint, mean_under, BAGS, MESHES  # noqa: E402
from drinkbag_uv_probe import owner_index, DATA  # noqa: E402
from ba2lib import Ba2Archive  # noqa: E402

ATLASES = [
    ("tea", "alientea_color.png"),
    ("tonic", "alientonic_color.png"),
    ("boba", "bobaalientea_color.png"),
    ("water", "purifiedwater_color.png"),
]


def main() -> int:
    owners = owner_index()
    key = MESHES["liquid/AlienTea"]
    ar = Ba2Archive(DATA / owners[key])
    blob, err = ar.read_entry(key)
    if blob is None:
        print("[FAIL] %s: %s" % (key, err))
        return 1
    tris, uvs = geometry(blob)
    print("liquid mesh: %d tris, %d verts" % (len(tris), len(uvs)))
    print("%-6s %-22s %-7s %s" % ("flavour", "footprint mean", "R-B", "warm (R>=G>=B, R-B>=0.05)"))
    for name, png in ATLASES:
        path = BAGS / png
        if not path.exists():
            print("%-6s missing %s" % (name, png))
            continue
        atlas = Image.open(path).convert("RGB")
        mask, mask_px = footprint(atlas, tris, uvs, False)
        m, n = mean_under(atlas, mask_px)
        spread = (m[0] - m[2]) / 255.0
        warm = m[0] >= m[1] >= m[2] and spread >= 0.05
        print("%-6s (%3.0f,%3.0f,%3.0f) %5.1f%%  %+6.3f  %s"
              % (name, m[0], m[1], m[2], 100.0 * n / (atlas.width * atlas.height), spread, warm))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
