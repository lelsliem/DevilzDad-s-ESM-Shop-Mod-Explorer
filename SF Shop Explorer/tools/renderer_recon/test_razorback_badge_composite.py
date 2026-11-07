"""Minification-aware probe for the Razorback badge engraving shader."""
from __future__ import annotations

import unittest
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "extracted"
SHADER_BADGE_CONTRAST = 24.0
SHADER_BADGE_BIAS = 0.01
SHADER_SAMPLE_OFFSETS = ((-2, -2), (0, -2), (2, -2), (-2, 0), (2, 0), (-2, 2), (0, 2), (2, 2))
PROBE_SIZES = ((16, 11), (24, 17), (32, 23), (48, 34), (64, 46), (96, 69))


def source_equivalent_badge_mask(image: Image.Image, size: tuple[int, int]) -> np.ndarray:
    """Downsample the atlas to a screen footprint and evaluate the HLSL mask."""
    reduced = image.convert("RGB").resize(size, Image.Resampling.BILINEAR)
    rgb = np.asarray(reduced, dtype=np.float32) / 255.0
    luma = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    # In the shader each offset is two screen pixels along ddx/ddy. At
    # screen-pixel centers, linear sampling lands on these integer neighbors.
    background = np.zeros_like(luma)
    for dx, dy in SHADER_SAMPLE_OFFSETS:
        background += np.roll(luma, (dy, dx), axis=(0, 1))
    background *= 0.125
    # The source mask is the local-neighborhood luminance minus pixel
    # luminance, so dark engraving on a lighter local field gets selected.
    return np.clip((background - luma - SHADER_BADGE_BIAS) * SHADER_BADGE_CONTRAST, 0.0, 1.0)


class RazorbackBadgeCompositeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.art = Image.open(ASSETS / "correct_textures_weapons_razorback_fancy_decals_color_gripregion_zoom.png")
        cls.standard = Image.open(ASSETS / "correct_textures_weapons_razorback_decals_color_gripregion_zoom.png")
        cls.coverage = Image.open(ASSETS / "correct_textures_weapons_razorback_decals_opacity_gripregion.png")
        cls.empty_fancy_mask = Image.open(ASSETS / "correct_textures_weapons_razorback_fancy_decals_opacity_gripregion.png")

    def test_dark_linework_survives_at_minified_screen_sizes(self) -> None:
        source_mask = self.coverage.convert("L")
        for size in PROBE_SIZES:
            ink = source_equivalent_badge_mask(self.art, size)
            coverage = np.asarray(source_mask.resize(size, Image.Resampling.BILINEAR), dtype=np.float32) / 255.0
            selected = ink[coverage > 0.5] > 0.5
            self.assertGreater(int(selected.sum()), 0, f"no dark engraving survives at {size[0]}x{size[1]}")
            self.assertLess(float(selected.mean()), 0.40, f"engraving is too dense at {size[0]}x{size[1]}")
            print(
                f"engraving probe {size[0]}x{size[1]}: "
                f"selected>0.5={int(selected.sum())} ({selected.mean():.1%} of covered pixels)"
            )

    def test_blank_fancy_opacity_is_not_used_as_badge_coverage(self) -> None:
        self.assertIsNone(self.empty_fancy_mask.convert("RGB").getbbox())
        self.assertIsNotNone(self.coverage.convert("RGB").getbbox())
        source = (ROOT.parent.parent / "src" / "OfflineModel.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("float badgeInk = saturate((neighborhood - detailLuma - 0.01) * 24.0);", source)
        self.assertNotIn("float badgeInk = saturate((detailLuma - neighborhood", source)

    def test_standard_coverage_is_present_and_fancy_opacity_crop_is_blank(self) -> None:
        self.assertGreater(self.coverage.convert("RGB").getbbox()[2], 0)
        self.assertIsNone(self.empty_fancy_mask.convert("RGB").getbbox())

    def test_probe_inputs_are_matching_atlas_crops(self) -> None:
        self.assertEqual(self.art.size, self.standard.size)
        self.assertEqual(self.art.size, (768, 549))

    def test_write_minification_sweep_composite_probe(self) -> None:
        cell_width, cell_height = 310, 220
        board = Image.new("RGB", (cell_width * 3, cell_height * 2), (28, 32, 40))
        draw = ImageDraw.Draw(board)
        for index, size in enumerate(PROBE_SIZES):
            ink = source_equivalent_badge_mask(self.art, size)
            wood = np.asarray(self.standard.resize(size, Image.Resampling.BILINEAR), dtype=np.float32) / 255.0
            coverage = np.asarray(self.coverage.resize(size, Image.Resampling.BILINEAR), dtype=np.float32) / 255.0
            composite = wood * (1.0 - ink[..., None] * 0.70)
            display = np.full_like(composite, 0.78)
            display[coverage > 0.5] = composite[coverage > 0.5]
            pixels = Image.fromarray(np.clip(display * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGB")
            x, y = (index % 3) * cell_width, (index // 3) * cell_height
            scale = min((cell_width - 32) // size[0], (cell_height - 42) // size[1])
            enlarged = pixels.resize((size[0] * scale, size[1] * scale), Image.Resampling.NEAREST)
            draw.text((x + 12, y + 8), f"screen footprint: {size[0]} x {size[1]} px", fill=(235, 239, 245))
            board.paste(enlarged, (x + 12, y + 32))
        out = ASSETS / "razorback_badge_minification_probe.png"
        board.save(out)
        print(f"minification-sweep shader-equivalent probe: {out}")
        self.assertGreater(out.stat().st_size, 10_000)
        self.assertEqual(board.size, (930, 440))


if __name__ == "__main__":
    unittest.main()
