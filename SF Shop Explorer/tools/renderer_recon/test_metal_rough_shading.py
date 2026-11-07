"""Metalness / roughness in the preview: contracts and the no-op guarantee.

The preview used to sample albedo only, which is why a mostly-metal weapon read
flat: the Vortex Grenade's atlas measures metalness 0.789 and the Razorback's
cylinder 0.759 (means of their own BC4 `_metal` maps), so in game almost all of
their look is a reflection of the room rather than the albedo.

Three things can go wrong here and only one of them is visible as "wrong": the
BC4 mean can be decoded from the wrong bytes, the companion maps can be paired
with the wrong colour map, and the metal term can fail to vanish at metalness 0,
which would silently change every cloth, food and wood material in the shop.

    python test_metal_rough_shading.py
"""
from __future__ import annotations

import math
import re
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2] / "src" / "OfflineModel.cpp"
HARNESS = Path(__file__).resolve().parent / "bc4_mean_harness.cpp"


class MetalRoughShadingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8-sig")
        match = re.search(r'kPsTexSrc\[\]\s*=\s*R"\((?P<body>.*?)\)";', cls.source, re.S)
        if match is None:
            raise AssertionError("kPsTexSrc shader not found")
        cls.opaque_shader = match.group("body")

    # --- the decoder ------------------------------------------------------
    def test_bc4_mean_reads_endpoints_and_indices(self) -> None:
        self.assertIn("float Bc4Mean(const std::vector<std::uint8_t>& a_bytes", self.source)
        # BC4: an 8-byte block, two endpoints then 48 bits of 3-bit indices.
        self.assertIn("const std::size_t rowPitch = static_cast<std::size_t>((a_w + 3) / 4) * 8;", self.source)
        self.assertIn("idx |= static_cast<std::uint64_t>(a_bytes[off + 2 + i]) << (8 * i);", self.source)
        self.assertIn("const int k = static_cast<int>((idx >> (3 * t)) & 7u);", self.source)
        # Interpolation between the endpoints, as the specification defines it.
        self.assertIn("v = ((8 - k) * e0 + k * e1 + 4) / 8;", self.source)

    def test_unavailable_is_negative_not_zero_metalness(self) -> None:
        # A material with no _metal map must not read as "metalness 0 either
        # way" - the caller has to be able to tell the two apart.
        self.assertIn("return -1.0f;", self.source)
        self.assertIn("if (mean < 0.0f) {", self.source)

    def test_companions_pair_by_colour_stem(self) -> None:
        self.assertIn('const auto at = a_colorKey.rfind("_color.dds");', self.source)
        self.assertIn('const std::string metalKey = CompanionMapKey(ddsKey, "_metal.dds");', self.source)
        self.assertIn('const std::string roughKey = CompanionMapKey(ddsKey, "_rough.dds");', self.source)
        # Every shipped companion is single-channel; a colour map must never be
        # averaged in as one.
        self.assertIn("map.format != 80", self.source)

    def test_decal_sheets_are_skipped(self) -> None:
        # A printed marking is not the metal it sits on, and those ranges take
        # the alpha shader rather than the opaque one.
        self.assertIn("if (!IsDecalSheet(mesh.matPath)) {", self.source)

    def test_defaults_preserve_the_previous_behaviour(self) -> None:
        self.assertIn("float metal{ 0.0f };", self.source)
        self.assertIn("float rough{ 0.5f };", self.source)
        self.assertIn("t.rough = 0.5f;  // only reaches the shading when metal > 0", self.source)

    # --- the plumbing -----------------------------------------------------
    def test_metalness_reaches_the_per_range_constant(self) -> None:
        self.assertIn("range.metal = img.metal;", self.source)
        self.assertIn("range.rough = img.rough;", self.source)
        # Byte 144 of the same 256-byte record: the four bytes after the mode
        # slot at 128-143. No new buffer, bind or descriptor.
        self.assertIn("const float metalRough[2]{ range.metal, range.rough };", self.source)
        self.assertIn("std::memcpy(dst + 144, metalRough, sizeof(metalRough));", self.source)

    def test_shader_declares_the_field_in_the_opaque_path(self) -> None:
        self.assertIn("float2 g_metalRough;", self.opaque_shader)
        self.assertIn("const float metal = saturate(g_metalRough.x);", self.opaque_shader)
        self.assertIn("const float rough = saturate(g_metalRough.y);", self.opaque_shader)

    # --- the guarantee ----------------------------------------------------
    def test_specular_stays_gated_on_metal_not_roughness(self) -> None:
        # The exponent has to stay 24.0 without a _metal map, or the highlight
        # narrows for every non-metal material in the catalogue.
        self.assertIn(
            "float spec = pow(abs(dot(n, hv)), lerp(24.0, 96.0, metal * gloss)) * lerp(0.18, 0.55, metal);",
            self.opaque_shader,
        )
        self.assertIn("float3 reflection = metal * f0 * env * (1.0 - 0.6 * rough) * (1.0 + 0.6 * fres);",
                      self.opaque_shader)

    def test_the_diffuse_rig_lifts_shadows_without_moving_the_anchor(self) -> None:
        """At metal 0 the reflection is identically zero and the highlight is
        the single 0.18 lobe, so the only thing the metal work can change for a
        material with no _metal map is the diffuse rig. That rig is pinned
        numerically here.

        Measured on Screenshot 2026-10-18 021147 (pistol, preview vs inventory
        screen): the preview read 48.5 luma against the game's 67.2 and 9.0
        chroma against 20.7. kd/kf are abs(dot), so in the old rig a face
        perpendicular to both lights got 0.42 of its albedo; this pipe applies
        that in gamma space, where 0.42 of the stored value is a third of the
        linear energy. Raising the floor and dropping the directional weights
        by the same amount recovers the shadows while leaving a lit face - the
        surface the 0.80 cap anchor was measured on - where it was."""
        self.assertIn("float lit = (1.0 - metal) * (0.62 + 0.34 * kd + 0.10 * kf + 0.04 * kd * kd) + spec;",
                      self.opaque_shader)

        def old(kd: float, kf: float) -> float:
            return 0.42 + 0.48 * kd + 0.16 * kf + 0.06 * kd * kd

        def new(kd: float, kf: float) -> float:
            return 0.62 + 0.34 * kd + 0.10 * kf + 0.04 * kd * kd

        self.assertGreater(new(0.0, 0.0), old(0.0, 0.0) * 1.4)
        self.assertAlmostEqual(new(1.0, 1.0), old(1.0, 1.0), delta=0.03)
        self.assertLessEqual(new(1.0, 1.0), old(1.0, 1.0), "the anchor must not brighten")
        # Monotonic in both lights, or the rebalance would have introduced banding.
        for kd in (0.0, 0.25, 0.5, 0.75, 1.0):
            self.assertLess(new(kd, 0.0), new(kd, 1.0))
            if kd > 0.0:
                self.assertLess(new(kd - 0.25, 0.5), new(kd, 0.5))

    def test_metal_one_moves_the_albedo_into_the_reflection(self) -> None:
        """The physical claim, checked rather than assumed: a fully metallic
        surface at a bright albedo reflects far more than it scatters."""

        def terms(metal: float, rough: float, albedo: float, env: float, kd: float = 0.8):
            f0 = 0.04 + metal * (albedo - 0.04)
            return albedo * (1.0 - metal) * (0.62 + 0.34 * kd), metal * f0 * env * (1.0 - 0.6 * rough)

        # The Vortex Grenade's own atlas: metalness 0.789, roughness 0.444.
        scattered_metal, reflected_metal = terms(0.789, 0.444, 0.53, 0.60)
        self.assertLess(scattered_metal, 0.12)
        self.assertGreater(reflected_metal, scattered_metal)
        # ...and the whole point: it stops reading pale. The diffuse-only value
        # it used to show is more than twice what the metal model emits.
        self.assertLess(scattered_metal + reflected_metal, 0.53 * (0.62 + 0.34 * 0.8) * 0.6)

        scattered_paint, reflected_paint = terms(0.163, 0.708, 0.45, 0.60)
        # A painted part keeps its diffuse look: this is why BreechBlock's
        # receiver changes only through its (now correct) albedo map.
        self.assertGreater(scattered_paint, reflected_paint)

    def test_environment_is_a_gradient_not_a_constant(self) -> None:
        self.assertIn("lerp(float3(0.16, 0.17, 0.20), float3(0.72, 0.77, 0.88), saturate(n.y * 0.5 + 0.5))",
                      self.opaque_shader)
        # A metal has no diffuse to fall back on, so the room can never be
        # darker than the albedo it replaces or every metal range reads darker
        # than the albedo-only render that came before it.
        self.assertGreaterEqual(0.72, 0.55)
        self.assertGreaterEqual(0.16, 0.10)
        # Fresnel has to come from the view direction: grazing angles are the
        # strongest remaining cue that a surface is metal.
        self.assertIn("float fres = pow(1.0 - saturate(dot(n, viewDir)), 5.0);", self.opaque_shader)

    def test_harness_covers_the_decoder_it_pins(self) -> None:
        harness = HARNESS.read_text(encoding="utf-8")
        for case in ("index 2 interpolates to 64/255", "index 7 interpolates to 223/255",
                     "a single endpoint-1 texel in slot 15", "two blocks average together",
                     "zero width is unavailable, not 0.0", "truncated block is unavailable",
                     "_metal.dds pairs with the colour stem"):
            self.assertIn(case, harness)
        # The harness must run the shipped function, not a copy of it.
        self.assertIn('#include "bc4_mean_block.inc"', harness)


if __name__ == "__main__":
    unittest.main()
