"""Regression tests for the BC1/BC4 decoders in `dump_tex.py`.

Two shipped bugs lived here for weeks and both were invisible in the PNGs they
produced:

- `bc1_block` / `bc4_block` iterated the four selector bytes as if each byte held
  one pixel, so **12 of every 16 pixels were never written** and stayed black. A
  quarter-filled atlas reads as "mostly padding", which is exactly what earlier
  notes concluded.
- `bc1_block` unpacked the block endpoints as RGB555. BC1 endpoints are RGB565,
  so every decoded colour had its green and blue channels shifted.

The pixel test uses a sentinel value the decoder cannot produce, so a pixel that
is never written fails the test rather than quietly looking like black art.
"""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from dump_tex import bc1_block, bc4_block, _rgb565  # noqa: E402

# Unreachable by either decoder: R and B are always floor(n*255/31), whose values
# are 0, 8, 16, ... 123, 131, ... - never 127. G is 0 for a black endpoint.
SENTINEL = bytes((0x7F, 0x00, 0x7F))


def blank(width: int, height: int) -> bytearray:
    """A destination buffer pre-filled with the unreachable sentinel."""
    return bytearray(SENTINEL * (width * height))


def unwritten(rgb: bytes) -> int:
    return sum(1 for i in range(0, len(rgb), 3) if rgb[i:i + 3] == SENTINEL)


def pixels(rgb: bytes) -> list[tuple[int, int, int]]:
    return [tuple(rgb[i:i + 3]) for i in range(0, len(rgb), 3)]


def pack565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


class Bc1DecoderTests(unittest.TestCase):
    def test_a_block_covers_all_sixteen_pixels(self) -> None:
        """The dropped-pixel bug: any pixel left at the sentinel means the
        decoder walked fewer than four rows of four."""
        block = bytes.fromhex("f6c5f6c500000000")
        rgb = blank(4, 4)
        bc1_block(block, rgb, 0, 0, 4, 4)
        self.assertEqual(unwritten(bytes(rgb)), 0)

    def test_endpoints_are_rgb565_not_rgb555(self) -> None:
        """The shipped 4x4 placeholder `textures/weapons/coachmanshotgun/
        decals_color.dds` (block f6c5f6c500000000, c0 == c1, all indices 0) is
        one flat colour: its two endpoints are equal, so the whole block must
        decode to exactly that endpoint."""
        self.assertEqual(_rgb565(0xC5F6)[:3], (197, 190, 180))
        # Green keeps six bits: a pure-green endpoint must reach 255.
        self.assertEqual(_rgb565(0x07E0)[:3], (0, 255, 0))
        block = bytes.fromhex("f6c5f6c500000000")
        rgb = blank(4, 4)
        bc1_block(block, rgb, 0, 0, 4, 4)
        self.assertEqual(set(pixels(bytes(rgb))), {(197, 190, 180)})

    def test_four_colour_mode_interpolates_between_the_endpoints(self) -> None:
        """c0 > c1 selects the 4-colour mode: entries 2 and 3 are the 1/3 and
        2/3 blends. An earlier version blended a channel with itself."""
        c0, c1 = pack565(255, 0, 0), pack565(0, 0, 0)
        # selectors 0,1,2,3 left to right on the top row, 0 for the rest
        indices = 0b11_10_01_00
        block = c0.to_bytes(2, "little") + c1.to_bytes(2, "little") + bytes([indices]) + bytes(3)
        rgb = blank(4, 4)
        bc1_block(block, rgb, 0, 0, 4, 4)
        self.assertEqual(pixels(bytes(rgb))[:4], [(255, 0, 0), (0, 0, 0), (170, 0, 0), (85, 0, 0)])

    def test_three_colour_mode_fourth_entry_is_black(self) -> None:
        """c0 <= c1 selects the 3-colour mode, where the fourth entry is
        transparent black rather than a blend."""
        c0, c1 = pack565(0, 0, 0), pack565(255, 255, 255)
        block = c0.to_bytes(2, "little") + c1.to_bytes(2, "little") + bytes([0b11_10_01_00]) + bytes(3)
        rgb = blank(4, 4)
        bc1_block(block, rgb, 0, 0, 4, 4)
        top = pixels(bytes(rgb))[:4]
        self.assertEqual(top[0], (0, 0, 0))
        self.assertEqual(top[1], (255, 255, 255))
        self.assertEqual(top[2], (127, 127, 127))
        self.assertEqual(top[3], (0, 0, 0))

    def test_partial_block_at_the_right_and_bottom_edge(self) -> None:
        """A texture whose size is not a whole number of blocks must not write
        past the edge, and must still fill every in-bounds pixel. This is also
        the test that catches block coordinates being used as pixel origins:
        with the wrong interpretation the four blocks overlap and leave stripes
        of the destination untouched."""
        block = bytes.fromhex("f6c5f6c500000000")
        rgb = blank(6, 6)
        for by in range(2):
            for bx in range(2):
                bc1_block(block, rgb, bx, by, 6, 6)
        self.assertEqual(unwritten(bytes(rgb)), 0)
        self.assertEqual(set(pixels(bytes(rgb))), {(197, 190, 180)})

    def test_second_block_lands_where_it_belongs(self) -> None:
        """Two blocks side by side: the second is a different colour, and it
        must occupy columns 4-7, not columns 1-4."""
        left, right = 0xC5F6, pack565(255, 0, 0)
        rb = right.to_bytes(2, "little") * 2 + bytes(4)
        rgb = blank(8, 4)
        bc1_block(left.to_bytes(2, "little") * 2 + bytes(4), rgb, 0, 0, 8, 4)
        bc1_block(rb, rgb, 1, 0, 8, 4)
        top = pixels(bytes(rgb))
        self.assertEqual(top[0:4], [(197, 190, 180)] * 4)
        self.assertEqual(top[4:8], [(255, 0, 0)] * 4)


class Bc4DecoderTests(unittest.TestCase):
    def test_a_block_covers_all_sixteen_pixels(self) -> None:
        block = bytes([0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
        rgb = blank(4, 4)
        bc4_block(block, rgb, 0, 0, 4, 4)
        self.assertEqual(unwritten(bytes(rgb)), 0)

    def test_eight_colour_mode_palette(self) -> None:
        """a0 > a1: entries 2..7 interpolate from a0 down to a1. Pixels 0..2 are
        given selectors 0, 1 and 2 (3 bits each, low bits first)."""
        bits = 0 | (1 << 3) | (2 << 6)
        block = bytes([200, 0]) + bits.to_bytes(6, "little")
        rgb = blank(4, 4)
        bc4_block(block, rgb, 0, 0, 4, 4)
        decoded = pixels(bytes(rgb))
        self.assertEqual(decoded[0], (200, 200, 200))  # selector 0 -> a0
        self.assertEqual(decoded[1], (0, 0, 0))        # selector 1 -> a1
        self.assertEqual(decoded[2], (171, 171, 171))  # selector 2 -> 6/7 a0 + 1/7 a1

    def test_six_colour_mode_adds_black_and_white(self) -> None:
        """a0 <= a1: entries 6 and 7 are 0 and 255. The selectors are one
        contiguous 48-bit run, so the first pixel's entry is the low three bits
        of the third byte."""
        for selector, expected in ((6, (0, 0, 0)), (7, (255, 255, 255))):
            block = bytes([0, 200, selector, 0x00, 0x00, 0x00, 0x00, 0x00])
            rgb = blank(4, 4)
            bc4_block(block, rgb, 0, 0, 4, 4)
            self.assertEqual(pixels(bytes(rgb))[0], expected, f"selector {selector}")
            self.assertEqual(unwritten(bytes(rgb)), 0)

    def test_selectors_advance_across_rows_not_within_one_byte(self) -> None:
        """Pixel 5 (second row, second column) is the sixth selector: 3*5 = 15
        bits in, i.e. bits 15-17 of the run. A per-row layout reads a different
        pixel here."""
        bits = 7 << (3 * 5)
        block = bytes([0, 0]) + bits.to_bytes(6, "little")
        rgb = blank(4, 4)
        bc4_block(block, rgb, 0, 0, 4, 4)
        decoded = pixels(bytes(rgb))
        self.assertEqual(decoded[5], (255, 255, 255))


class ShippedTextureTests(unittest.TestCase):
    """Optional end-to-end check against the installed archives."""

    ARCHIVE = Path(os.environ.get(
        "SFSE_STARFIELD_DATA", "B:/SteamLibrary/steamapps/common/Starfield/Data"
    )) / "Starfield - Textures10.ba2"
    KEY = "textures/weapons/coachmanshotgun/decals_color.dds"

    def test_shipped_four_by_four_placeholder_decodes_uniform(self) -> None:
        if not self.ARCHIVE.exists():
            self.skipTest(f"{self.ARCHIVE} not present")
        from ba2lib import Ba2Archive
        archive = Ba2Archive(self.ARCHIVE)
        self.assertTrue(archive.valid, "texture archive did not parse")
        tex = archive.tex_files.get(self.KEY)
        self.assertIsNotNone(tex, f"{self.KEY} missing from {self.ARCHIVE.name}")
        self.assertEqual((tex.width, tex.height), (4, 4))
        data, err = archive.read_texture(self.KEY)
        self.assertIsNotNone(data, err)
        self.assertEqual(data[:8].hex(), "f6c5f6c500000000")
        rgb = blank(4, 4)
        bc1_block(data[:8], rgb, 0, 0, 4, 4)
        self.assertEqual(unwritten(bytes(rgb)), 0)
        self.assertEqual(set(pixels(bytes(rgb))), {(197, 190, 180)})


if __name__ == "__main__":
    unittest.main()
