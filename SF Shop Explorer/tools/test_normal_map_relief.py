"""Guards for the 2026-10-21 finding: the Pils can's ring pull is a normal map.

The capture that prompted this showed the can all but finished and asked only
for its ring pull. Three separate probes settled where it lives:

* `Erdebrau Pilsner01.nif` has seven BSGeometry blocks and none is a tab. The
  lid is `ErdebrauPilsner:3` - 124 vertices, 110 triangles, z 0.170-0.174, a
  flat 16-gon plate. Rendered top-down it is a plain disc.
* The decal atlas' lid island (u 0.002-0.330, v 0.244-0.812) is pale foil with
  no line art: measured luma std 0.022 over that box.
* The same box in the material's BC5 `_normal` map carries the tab: a ring, a
  rivet and a slot, X-channel std 0.622. `tools/renderer_recon/lid_relief_probe.py`
  shades the plate from it and the emboss lands centred on the lid.

So the preview had been brushing no relief at all, and no amount of albedo,
coverage or glass work could ever have drawn that tab. This module holds the
three properties that make relief drawable:

1. The map is readable: the archives tag these maps BC5_UNORM_SRGB (DXGI 84),
   which MapTexFormat rejected outright, and both channels are vector
   components, so they must not be gamma-decoded either. 84 -> 83 (BC5).
2. The map is bound without disturbing the composite modes that already own the
   detail descriptor: relief rides that slot only when no composite claimed it,
   and one float in the reserved constant tail says which it is.
3. The shaders perturb the normal from a per-pixel tangent frame recovered from
   screen-space derivatives, because the meshes ship no tangent stream.

The real-archive test reads the shipped maps and checks the two halves of the
finding - foil island, embossed normal island - so a texture swap or a UV
regression cannot quietly remove the tab again.
"""
from __future__ import annotations

import os
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OFFLINE = ROOT / "src" / "OfflineModel.cpp"
LIVEMODEL = ROOT / "src" / "LiveModel.cpp"
DATA = Path(os.environ.get("SFSE_STARFIELD_DATA", "B:/SteamLibrary/steamapps/common/Starfield/Data"))
ARCHIVE = DATA / "Starfield - Textures05.ba2"
NORMAL = "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau pilsner_decal_normal.dds"
COLOUR = "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau pilsner_decal_color.dds"
# The lid plate's UV island, in texels of a 1024x1024 atlas.
LID_BOX = (2, 338, 250, 831)  # x0, x1, y0, y1

sys.path.insert(0, str(Path(__file__).resolve().parent / "ba2_audit"))


def shader_body(name: str, source: str) -> str:
    match = re.search(r'static constexpr const char ' + name + r'\[\] = R"\((.*?)\)";', source, re.S)
    if not match:
        raise AssertionError("%s not found in OfflineModel.cpp" % name)
    return match.group(1)


class Bc5IsReadableTests(unittest.TestCase):
    def test_bc5_srgb_maps_to_linear_bc5(self) -> None:
        from ba2lib import map_tex_format, tex_row_pitch

        self.assertEqual(map_tex_format(84), 83)
        self.assertEqual(map_tex_format(83), 83)
        # 16 bytes per 4x4 block either way, so an existing upload path needs
        # no new footprint arithmetic.
        self.assertEqual(tex_row_pitch(83, 1024), 16 * 256)

    def test_the_cpp_side_agrees_with_the_replica(self) -> None:
        source = LIVEMODEL.read_text(encoding="utf-8-sig")
        self.assertRegex(source, r"case 84: return 83;")

    def test_unknown_formats_still_fall_through(self) -> None:
        from ba2lib import map_tex_format

        self.assertEqual(map_tex_format(85), 0)


class ReliefBindingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")

    def test_the_companion_is_the_normal_suffix(self) -> None:
        self.assertIn('CompanionMapKey(ddsKey, "_normal.dds")', self.source)

    def test_only_a_bc5_map_of_the_same_size_is_paired(self) -> None:
        # A companion that disagrees on dimensions belongs to another asset and
        # would shear the relief across the item.
        self.assertIn("normal.format == 83", self.source)
        self.assertIn("normal.width == t.width && normal.height == t.height", self.source)

    def test_relief_yields_to_a_composite_detail_map(self) -> None:
        # The wood engraving, Crucible and Vortex arms sample the detail
        # descriptor as colour; if relief claimed the same slot the shader
        # would read that colour as a normal.
        self.assertIn("range.normalSlot >= 0 && range.detailSlot < 0 ? 1.0f : 0.0f", self.source)

    def test_the_slot_is_reset_per_upload(self) -> None:
        self.assertIn("range.normalSlot = -1;", self.source)

    def test_relief_shares_the_detail_region(self) -> None:
        self.assertIn("const auto normalSlot = 2 * kMaxGpuTextures + 1 + detailCount;", self.source)


class ReliefShaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        source = OFFLINE.read_text(encoding="utf-8-sig")
        cls.opaque = shader_body("kPsTexSrc", source)
        cls.alpha = shader_body("kPsTexAlphaSrc", source)

    def test_both_textured_shaders_perturb_the_normal(self) -> None:
        for name, body in (("kPsTexSrc", self.opaque), ("kPsTexAlphaSrc", self.alpha)):
            self.assertIn("g_metalPad.x > 0.5", body, name)
            # Two channels stored, an explicit strength, and the perturbation
            # added to the geometric normal: strength 0 leaves the surface
            # exactly flat, which is what a map with a biased plate needs.
            self.assertIn("const float strength = 0.25;", body, name)
            self.assertIn("tangent * (nm.x * scale * strength) - bitangent * (nm.y * scale * strength) + n", body, name)
            # A tangent frame from screen-space derivatives: no tangent stream
            # exists in these meshes.
            self.assertIn("cross(posDy, n)", body, name)
            self.assertIn("ddx(i.uv)", body, name)
            self.assertIn("rsqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-12))", body, name)

    def test_the_alpha_pass_can_see_the_relief_flag(self) -> None:
        self.assertIn("float2 g_metalPad;", self.alpha)

    def test_the_relief_is_footprint_filtered(self) -> None:
        # The shipped normal maps carry a single surface and no mip chain, so
        # the GPU cannot filter them. Sampled per pixel at preview scale every
        # texel of print becomes a pixel of shading - the 2026-10-22 captures
        # show that as salt-and-pepper speckle on the Pilsner lid and body, the
        # coolant jar's lid and the wine's cap. Four taps spread by the pixel's
        # own UV footprint replace the missing mip.
        for name, body in (("kPsTexSrc", self.opaque), ("kPsTexAlphaSrc", self.alpha)):
            arm = body.split("if (g_metalPad.x > 0.5)", 1)[1]
            arm = arm.split("n = normalize(tangent", 1)[0]
            self.assertIn("float2 footprint = max(abs(ddx(i.uv)), abs(ddy(i.uv)));", arm, name)
            self.assertEqual(arm.count("g_detail.Sample"), 4, name)
            self.assertIn("nm = nm * 0.5 - 1.0;", arm, name)

    def test_the_opaque_shader_re_measures_its_diffuse_terms(self) -> None:
        # kd/kf are computed before the relief arm's position in that literal,
        # so they are re-measured rather than left on the flat normal.
        self.assertIn("kd = abs(dot(n, key));", self.opaque)
        self.assertIn("kf = abs(dot(n, fill));", self.opaque)


@unittest.skipUnless(ARCHIVE.exists(), "Starfield textures are not installed here")
class RealAtlasTests(unittest.TestCase):
    """The finding itself, on the shipped maps."""

    @classmethod
    def setUpClass(cls) -> None:
        import numpy as np

        from ba2lib import Ba2Archive
        import dump_tex

        archive = Ba2Archive(ARCHIVE)

        def payload(name: str):
            tf = archive.tex_files[name]
            data, err = archive.read_texture(name)
            if data is None:
                raise AssertionError("read_texture %s: %s" % (name, err))
            return data, tf.width, tf.height

        def blocks(data: bytes, w: int, h: int, step: int, decode) -> "np.ndarray":
            out = bytearray(w * h * 3)
            bw = (w + 3) // 4
            for by in range((h + 3) // 4):
                for bx in range(bw):
                    o = (by * bw + bx) * step
                    decode(data[o:o + step], out, bx, by, w, h)
            return np.frombuffer(bytes(out), np.uint8).reshape(h, w, 3).astype("float32") / 255.0

        cls.np = np
        cls.archive = archive
        # BC5: red block then green block per 4x4, so the X channel is the
        # first 8 bytes of each 16-byte block.
        normal_data, nw, nh = payload(NORMAL)
        cls.normal_x = blocks(normal_data, nw, nh, 16,
                              lambda b, out, bx, by, w, h: dump_tex.bc4_block(b, out, bx, by, w, h))[..., 0] * 2.0 - 1.0
        # BC1 colour, decoded as three channels and reduced to luma.
        colour_data, cw, ch = payload(COLOUR)
        rgb = blocks(colour_data, cw, ch, 8, dump_tex.bc1_block)
        cls.colour_luma = rgb @ np.array([0.2126, 0.7152, 0.0722], np.float32)

    def box(self, img):
        x0, x1, y0, y1 = LID_BOX
        return img[y0:y1, x0:x1]

    def test_the_lid_island_of_the_albedo_is_plain_foil(self) -> None:
        self.assertLess(float(self.box(self.colour_luma).std()), 0.06)

    def test_the_lid_island_of_the_normal_map_carries_the_tab(self) -> None:
        self.assertGreater(float(self.box(self.normal_x).std()), 0.30)

    def test_the_lid_island_is_not_a_centred_normal_map(self) -> None:
        # Why the shaders carry a strength term at all, measured on the shipped
        # map: over the plate the signed X channel averages -0.65 rather than
        # 0, so an unscaled perturbation tilts the whole lid almost edge-on and
        # prints its texel grain as shading. A future asset that centres
        # properly would make the 0.25 worth revisiting.
        self.assertLess(float(self.box(self.normal_x).mean()), -0.5)

    def test_the_normal_map_is_the_srgb_tagged_bc5(self) -> None:
        self.assertEqual(self.archive.tex_files[NORMAL].dxgi, 84)

    def test_the_normal_map_carries_no_mip_chain(self) -> None:
        # 1 MB is exactly one 1024x1024 BC5 surface: this is the reason the
        # relief arm has to filter the map itself. If the files ever gain
        # mips, the four-tap footprint filter becomes redundant (harmless,
        # one extra tap each) - but the assertion documents which world we
        # are shading in.
        data, err = self.archive.read_texture(NORMAL)
        self.assertIsNotNone(data, err)
        tf = self.archive.tex_files[NORMAL]
        self.assertEqual(len(data), tf.width * tf.height)


if __name__ == "__main__":
    unittest.main()
