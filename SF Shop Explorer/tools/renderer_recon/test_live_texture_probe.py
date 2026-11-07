"""Regression coverage for the bounded startup texture-read diagnostic."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "ba2_audit"))
from ba2lib import Ba2Archive, lz4_block_decompress, map_tex_format, tex_row_pitch  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src" / "LiveModel.h"
SOURCE = ROOT / "src" / "LiveModel.cpp"
DATA = Path("B:/SteamLibrary/steamapps/common/Starfield/Data")
TEXTURE_ARCHIVES = ("Starfield - Textures10.ba2", "Starfield - Textures11.ba2")
HAS_TEXTURE_ARCHIVES = all((DATA / name).is_file() for name in TEXTURE_ARCHIVES)


class LiveTextureProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8-sig")
        cls.source = SOURCE.read_text(encoding="utf-8-sig")

    def test_probe_covers_known_absent_and_razorback_keys(self) -> None:
        probe = re.search(
            r"void RunTextureReadProbe\(\)\s*\{(?P<body>.*?)\n\t\}",
            self.source,
            re.S,
        )
        self.assertIsNotNone(probe, "one-shot texture read probe not found")
        body = probe.group("body")
        for label in ("known-bc4", "definitely-absent", "razorback-derived"):
            self.assertIn(f'{{ "{label}",', body)
        self.assertIn("tombstone_decals_opacity.dds", body)
        self.assertIn("diagnostic_missing_opacity_6r12.dds", body)
        self.assertIn("razorback_receiver_opacity.dds", body)
        self.assertIn("image.bytes.size()", body)
        self.assertIn("source='{}'", body)

    def test_probe_runs_after_warm_indexes_and_outside_warm_lock(self) -> None:
        warm = re.search(
            r"void WarmTick\(\)\s*\{(?P<body>.*?)\n\t\}",
            self.source,
            re.S,
        )
        self.assertIsNotNone(warm, "WarmTick implementation not found")
        body = warm.group("body")
        call = body.index("RunTextureReadProbe();")
        self.assertLess(body.index("EnsureColorIndex();"), call)
        self.assertLess(body.index("EnsureOpacityIndex();"), call)
        self.assertLess(body.index("EnsureCdbIndex();"), call)
        self.assertLess(body.index("g_warmDone = true;"), call)
        self.assertIn("RunTextureReadProbe();  // ReadTexture takes g_mutex", body)

    @unittest.skipUnless(HAS_TEXTURE_ARCHIVES,
                         "Starfield weapon texture archives are not installed here")
    def test_weapon_ba2_primary_mip_chunks_form_real_contiguous_chains(self) -> None:
        data = DATA
        samples = (
            ("Starfield - Textures10.ba2", "textures/weapons/breach/stock_tactical_color.dds"),
            ("Starfield - Textures10.ba2", "textures/weapons/breach/decals_color.dds"),
            ("Starfield - Textures11.ba2", "textures/weapons/razorback/decals_color.dds"),
            ("Starfield - Textures11.ba2", "textures/weapons/razorback/fancy/decals_color.dds"),
            ("Starfield - Textures11.ba2", "textures/weapons/razorback/razorback_receiver_color.dds"),
        )
        for archive_name, key in samples:
            with self.subTest(texture=key):
                archive = Ba2Archive(data / archive_name)
                self.assertTrue(archive.valid, archive.describe())
                texture = archive.tex_files[key]
                fmt = map_tex_format(texture.dxgi)
                mip_chunks = sorted(texture.chunks, key=lambda chunk: chunk.start_mip)
                self.assertGreaterEqual(len(mip_chunks), 2)
                next_mip = 0
                width, height = texture.width, texture.height
                for chunk in mip_chunks:
                    self.assertEqual(chunk.start_mip, next_mip)
                    self.assertGreaterEqual(chunk.end_mip, chunk.start_mip)
                    expected_bytes = 0
                    for _mip in range(chunk.start_mip, chunk.end_mip + 1):
                        pitch = tex_row_pitch(fmt, width)
                        rows = height if fmt == 28 else (height + 3) // 4
                        expected_bytes += pitch * rows
                        width, height = max(1, width // 2), max(1, height // 2)
                    self.assertGreaterEqual(chunk.size, expected_bytes)
                    with archive.path.open("rb") as stream:
                        stream.seek(chunk.off)
                        payload = stream.read(chunk.packed or chunk.size)
                    self.assertEqual(len(payload), chunk.packed or chunk.size)
                    if chunk.packed and chunk.packed != chunk.size:
                        if archive.method == 3:
                            payload = lz4_block_decompress(payload, chunk.size)
                        else:
                            import zlib
                            payload = zlib.decompress(payload)
                    self.assertEqual(len(payload), chunk.size)
                    next_mip = chunk.end_mip + 1
                self.assertEqual(next_mip, texture.mips)

    def test_runtime_reader_and_upload_preserve_the_mip_chain(self) -> None:
        dds = self.source[self.source.index("bool DdsToImage("):self.source.index("bool ReadTexture(")]
        self.assertIn("ddsFlags & 0x20000u", dds)
        self.assertIn("a_out.mipLevels = totalBytes == topMipBytes ? 1", dds)
        reader = self.source[self.source.index("bool ReadTexture("):self.source.index("struct ColorEntry")]
        self.assertIn("std::uint32_t nextMip = 0;", reader)
        self.assertIn("c.startMip == nextMip", reader)
        self.assertIn("a_out.mipLevels = mipLevels;", reader)
        self.assertIn("std::vector<std::pair<const TexChunk*, std::size_t>> mipChunks;", reader)
        self.assertIn("raw.insert(raw.end(), chunkRaw.begin(), chunkRaw.begin() + chunkBytes);", reader)
        upload = (ROOT / "src" / "OfflineModel.cpp").read_text(encoding="utf-8-sig")
        upload = upload[upload.index("bool UploadTextureImage("):upload.index("bool UploadModelTextures(")]
        self.assertIn("td.MipLevels = static_cast<UINT16>(std::clamp(a_mipLevels, 1u, 16u));", upload)
        self.assertIn("srv.Texture2D.MipLevels = mipLevels;", upload)
        self.assertIn("dst.SubresourceIndex = mip;", upload)

    def test_source_trace_is_optional_for_existing_prefetch_calls(self) -> None:
        self.assertRegex(
            self.header,
            r"bool ReadTexture\(const std::string& a_key, TexImage& a_out, "
            r"std::string\* a_outSource = nullptr\);",
        )
        self.assertIn(
            "bool ReadTexture(const std::string& a_key, TexImage& a_out, std::string* a_outSource)",
            self.source,
        )
        self.assertIn('"<cached unresolved>"', self.source)
        self.assertIn('"<no successful source; absent or unreadable>"', self.source)


if __name__ == "__main__":
    unittest.main()
