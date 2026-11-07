"""Tests for the visual DXGI UNORM versus sRGB sample comparison."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "renderer_recon" / "dxgi_color_space_comparison.py"
TEXTURE_ARCHIVE = Path("B:/SteamLibrary/steamapps/common/Starfield/Data/Starfield - Textures10.ba2")
spec = importlib.util.spec_from_file_location("dxgi_color_space_comparison", SCRIPT)
comparison = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(comparison)


class DxgiColorSpaceComparisonTests(unittest.TestCase):
    def test_transfer_curve_and_channel_conversion(self) -> None:
        self.assertAlmostEqual(comparison.srgb_to_linear(0.0), 0.0)
        self.assertAlmostEqual(comparison.srgb_to_linear(1.0), 1.0)
        self.assertAlmostEqual(comparison.srgb_to_linear(0.5), 0.21404114048223255)
        self.assertGreater(
            comparison.unorm_sample_to_unorm_byte(128),
            comparison.srgb_sample_to_unorm_byte(128),
        )
        self.assertEqual(comparison.unorm_sample_to_unorm_byte(0), 0)
        self.assertEqual(comparison.srgb_sample_to_unorm_byte(0), 0)

    def test_bc1_decoder_rejects_short_blocks_and_decodes_solid_red(self) -> None:
        with self.assertRaisesRegex(ValueError, "short BC1 primary mip"):
            comparison.decode_bc1_rgb(b"", 4, 4)
        block = bytes((0x00, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00))
        image = comparison.decode_bc1_rgb(block, 4, 4)
        self.assertEqual(image.getpixel((0, 0)), (255, 0, 0, 255))
        self.assertEqual(image.getpixel((3, 3)), (255, 0, 0, 255))

    @unittest.skipUnless(TEXTURE_ARCHIVE.is_file(),
                         "Starfield weapon textures are not installed here")
    def test_real_breach_stock_map_produces_a_measurable_visual_difference(self) -> None:
        from ba2lib import Ba2Archive

        key = comparison.DEFAULT_TEXTURE
        archive_path = TEXTURE_ARCHIVE
        archive = Ba2Archive(archive_path)
        self.assertTrue(archive.valid, archive.describe())
        actual_key = key.lower()
        tex = archive.tex_files[actual_key]
        self.assertIn(tex.dxgi, (71, 72))
        raw, error = archive.read_texture(actual_key)
        self.assertIsNotNone(raw, error)
        image = comparison.decode_bc1_rgb(raw, tex.width, tex.height)
        out = ROOT / "tools" / "renderer_recon" / "extracted" / "unorm_srgb_sample_comparison_test.png"
        unorm_luma, srgb_luma = comparison.render_comparison(
            image, out, actual_key, archive_path.name, tex.dxgi)
        self.assertGreater(unorm_luma - srgb_luma, 20.0)
        self.assertTrue(out.is_file())
        self.assertGreater(out.stat().st_size, 20_000)


if __name__ == "__main__":
    unittest.main()
