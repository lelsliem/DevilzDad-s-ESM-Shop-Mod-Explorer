"""Regression test for the Mark I backpack drawing flat grey.

The pack is the one armour piece whose preview was still wrong after the
skinned-part and framing fixes: 2497 vertices, a 1024x1024 atlas resolved
from Materials\\Clothes\\SuitZero\\V3\\Spacesuit_SuitZero_Backpack.mat, and
`uv=0` in the geometry log - so every face sampled texel (0,0) and the whole
pack drew as one flat grey sheet.

The UVs were in the file all along. The .mesh stream is a normal f16 UV set
except for 72 vertices that carry whole-tile wrap offsets (u to -11.30, v to
26.17); the parser's "is this a UV stream?" bound was +/-8 and rejected the
ENTIRE set on the first one. The renderer samples with
D3D12_TEXTURE_ADDRESS_MODE_WRAP, so those offsets fold back into ordinary
atlas coordinates and are not an error.

These tests pin the rule on both copies: the plugin's ParseMeshLod0 and the
Python replica the audit tools use.
"""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from nif_uv import parse_mesh_streams  # noqa: E402

LIVEMODEL = ROOT / "src" / "LiveModel.cpp"


def half(value: float) -> bytes:
    return struct.pack("<e", value)


def lod0(uvs: list[tuple[float, float]], extra_stream: bytes = b"") -> bytes:
    """One .mesh LOD0: 2 tris over len(uvs) verts, one f16 UV set."""
    nv = len(uvs)
    idx = [0, 1, 2, 2, 3, 0][: max(3, min(6, nv * 3 // 2))]
    out = bytearray()
    out += struct.pack("<II", 0, len(idx))
    for i in idx:
        out += struct.pack("<H", i)
    out += struct.pack("<f", 1.0)          # border scale
    out += struct.pack("<I", 0)            # weights per vertex
    out += struct.pack("<I", nv)
    for _ in range(nv):
        out += struct.pack("<3h", 1000, -1000, 500)
    out += struct.pack("<I", nv)           # UV set: count == nv
    for u, v in uvs:
        out += half(u) + half(v)
    return bytes(out) + extra_stream


class WrapUvTests(unittest.TestCase):
    def test_a_wrapped_uv_set_is_accepted(self) -> None:
        # The measured pack: three ordinary vertices and one at u=-9.35,
        # v=24.64. The old bound threw the whole stream away.
        uvs = [(0.10, 0.20), (0.30, 0.40), (0.50, 0.60), (-9.3515625, 24.640625)]
        info = parse_mesh_streams(lod0(uvs))
        self.assertEqual(info["nv"], 4)
        self.assertEqual(len(info["uv_sets"]), 1, "wrap offsets must not void the set")
        umin, umax, vmin, vmax = info["uv_sets"][0]
        self.assertAlmostEqual(umin, -9.3515625, places=4)
        self.assertAlmostEqual(vmax, 24.640625, places=3)

    def test_non_finite_values_still_reject_the_stream(self) -> None:
        uvs = [(0.0, 0.0), (0.0, 0.0), (0.0, 0.0), (float("nan"), 0.0)]
        info = parse_mesh_streams(lod0(uvs))
        self.assertEqual(info.get("uv_sets", []), [])

    def test_absurd_values_still_reject_the_stream(self) -> None:
        uvs = [(0.0, 0.0), (0.0, 0.0), (0.0, 0.0), (9000.0, 0.0)]
        info = parse_mesh_streams(lod0(uvs))
        self.assertEqual(info.get("uv_sets", []), [])


class PluginSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        source = LIVEMODEL.read_text(encoding="utf-8-sig")
        start = source.index("bool ParseMeshLod0(")
        # ParseMeshLod0 ends at the next function. The old marker was the MO2
        # discovery routine, which no longer exists now that assets come from
        # the loaded Data view.
        end = source.index("void LoadConfig()", start)
        cls.parser = source[start:end]
        cls.source = source

    def test_the_tight_uv_bound_is_gone(self) -> None:
        self.assertNotIn("-8.0f", self.parser)
        self.assertIn("kMaxAbsUv", self.parser)

    def test_the_wrap_case_is_recorded_at_the_constant(self) -> None:
        start = self.source.index("constexpr float kMaxAbsUv")
        note = self.source[max(0, start - 1400):start]
        self.assertIn("Mark I backpack", note)
        self.assertIn("52b4455363dc68a70393", note)
        self.assertIn("WRAP", note)

    def test_the_bound_still_rejects_a_non_uv_stream(self) -> None:
        # Candidacy is `count == nv`; the numeric bound remains the second
        # guard, so it must stay a real comparison and stay finite-checked.
        self.assertIn("std::abs(su) <= kMaxAbsUv", self.parser)
        self.assertIn("std::abs(sv) <= kMaxAbsUv", self.parser)
        self.assertIn("std::isfinite(su)", self.parser)


if __name__ == "__main__":
    unittest.main()
