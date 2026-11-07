#!/usr/bin/env python3
"""Guard for the original (v1) BTDX layout and BC7-sRGB textures.

The 2026-10-23 full-profile audit found two silent gaps that only appear once
mods are installed:

  * 31 archives in one profile - the whole Star Wars Genesis armour texture
    compendium - are BTDX v1. That generation has a 24-byte header, a 4-byte
    prefix before the entry body and ABSOLUTE chunk offsets. Reading 36 header
    bytes shifted every entry by 12 and the archive parsed as invalid, so its
    textures were invisible to the preview while the engine drew them.

  * 10,230 texture entries in the same profile are DXGI 99, BC7_UNORM_SRGB.
    MapTexFormat knew 97/98 only, answered 0, and every repacked texture
    archive was dropped as "unsupported format".

The test builds a v1 archive byte by byte (no game install needed) and pins
the reader against it, then checks the C++ carries the same branches.

    python -m unittest test_ba2_v1_archives -v
"""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from ba2lib import Ba2Archive, map_tex_format  # noqa: E402

ROOT = HERE.parents[1]
LIVE_MODEL = ROOT / "src" / "LiveModel.cpp"


def make_v1_archive(path: Path, texture: str, raw: bytes) -> None:
    """Write a one-texture BTDX v1 DX10 archive with an absolute chunk offset."""
    packed = zlib.compress(raw)
    name = texture.encode("latin-1")
    header = 24
    entry_body = 20
    chunk = 24
    bulk_off = header + 4 + entry_body + chunk
    nt_off = bulk_off + len(packed)

    out = bytearray()
    out += b"BTDX"
    out += struct.pack("<I", 1)            # version 1: the legacy header
    out += b"DX10"
    out += struct.pack("<I", 1)            # one file
    out += struct.pack("<Q", nt_off)
    out += struct.pack("<I", 0xDEADBEEF)   # 4-byte entry prefix
    out += b"dds\0"
    out += struct.pack("<I", 0x11223344)   # dir hash
    out += bytes([0, 1])                   # unk8, nchunks=1
    out += struct.pack("<H", 0)            # unk16
    out += struct.pack("<HH", 8, 8)        # height, width
    out += bytes([1, 99])                  # mips, BC7_UNORM_SRGB
    out += b"\0\0"
    out += struct.pack("<QIIHHI", bulk_off, len(packed), len(raw), 0, 0, 0xBAADF00D)
    assert len(out) == bulk_off
    out += packed
    out += struct.pack("<H", len(name)) + name
    path.write_bytes(bytes(out))


def make_v1_gnrl(path: Path, name: str, raw: bytes) -> None:
    """One-file BTDX v1 GNRL archive: name hash first, then the v2 fields."""
    packed = zlib.compress(raw)
    encoded = name.encode("latin-1")
    bulk_off = 24 + 36
    nt_off = bulk_off + len(packed)
    out = bytearray()
    out += b"BTDX"
    out += struct.pack("<I", 1)
    out += b"GNRL"
    out += struct.pack("<I", 1)
    out += struct.pack("<Q", nt_off)
    out += struct.pack("<I", 0xAABBCCDD)   # name hash (v1 only)
    out += b"dds\0"
    out += struct.pack("<I", 0x11223344)   # dir hash
    out += struct.pack("<I", 0x00100100)   # flags
    out += struct.pack("<Q", bulk_off)
    out += struct.pack("<II", len(packed), len(raw))
    out += struct.pack("<I", 0xBAADF00D)
    assert len(out) == bulk_off
    out += packed
    out += struct.pack("<H", len(encoded)) + encoded
    path.write_bytes(bytes(out))


class V1ArchiveTests(unittest.TestCase):
    def test_v1_archive_parses_and_decodes(self) -> None:
        texture = "textures/test/thing_color.dds"
        raw = bytes(range(64)) * 4
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "legacy.ba2"
            make_v1_archive(path, texture, raw)
            ar = Ba2Archive(path)
            self.assertTrue(ar.valid, "v1 archive must parse")
            self.assertEqual(ar.version, 1)
            self.assertEqual(ar.header_bytes, 24)
            self.assertEqual(len(ar.tex_files), 1)
            tf = ar.tex_files[texture]
            self.assertEqual((tf.width, tf.height, tf.mips, tf.dxgi), (8, 8, 1, 99))
            self.assertEqual(tf.chunks[0].off, 72, "v1 chunk offsets are absolute")
            blob, err = ar.read_texture(texture)
            self.assertEqual(err, "")
            self.assertEqual(blob, raw)

    def test_v1_gnrl_reads_with_the_shifted_fields(self) -> None:
        name = "textures/test/plate_d.dds"
        raw = b"DDS " + bytes(96)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "legacy-gnrl.ba2"
            make_v1_gnrl(path, name, raw)
            ar = Ba2Archive(path)
            self.assertTrue(ar.valid)
            self.assertFalse(ar.is_tex)
            self.assertIn(name, ar.entries)
            self.assertEqual(ar.entries[name].offset, 60)
            blob, err = ar.read_entry(name)
            self.assertEqual(err, "")
            self.assertEqual(blob, raw)

    def test_bc7_srgb_is_readable(self) -> None:
        # 97 is BC7_TYPELESS: creating the preview's SRV with that format is an
        # invalid call and removed the device on the first BC7 atlas (A-300).
        # Every BC7 variant has to resolve to the concrete BC7_UNORM.
        self.assertEqual(map_tex_format(99), 98)
        self.assertEqual(map_tex_format(98), 98)
        self.assertEqual(map_tex_format(97), 98)

    def test_cpp_matches_the_v1_reader(self) -> None:
        src = LIVE_MODEL.read_text(encoding="utf-8")
        self.assertIn("ar->file.read(hdr, 24);", src)
        self.assertIn("const bool legacyHeader = hver == 1;", src)
        self.assertIn("if (!bad && ar->isTex && hver == 2)", src)
        self.assertIn("const bool legacyGnrl = hver == 1;", src)
        self.assertIn("case 99: return 98;", src)


if __name__ == "__main__":
    unittest.main()
