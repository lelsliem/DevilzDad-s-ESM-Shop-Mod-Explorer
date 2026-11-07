"""Blender-exported NIFs carry their LOD0 .mesh payload inside the NIF.

The lightsaber pack reported "3 geos, 3 mesh misses" and every hilt rendered
grey: ParseNifGeos read the BSGeometry LOD entry exactly like a Bethesda file
(u32 pathLen + printable "guid\\guid" factory path), so it swallowed the inline
.mesh blob's first field as a path length and then looked for a
geometries/<guid>.mesh entry that has never existed.

These tests pin both halves of the fix:

  * a real Blender NIF (the Star Wars Genesis - Lightsabers Engraved hilt) is
    detected as inline, and each block's blob parses as a .mesh LOD0 whose
    index/vertex counts match the LOD entry header -- the proof that the
    layout assumption is right rather than merely plausible;
  * a Bethesda NIF still takes the path branch, so vanilla weapons keep
    reading their archive geometry;
  * the plugin prefers inline bytes over the archive lookup.

The mod file is external; when it is not installed the first test skips
rather than failing a checkout that cannot have it.
"""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "LiveModel.cpp"
EXTRACTED = ROOT / "tools" / "renderer_recon" / "extracted"
LIGHTSABER_DIR = Path(r"B:\MO2\Starfield\mods\Star Wars Genesis - Lightsabers\meshes\weapons\lightsaber")

sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))
from nif_uv import Nif, parse_mesh_streams  # noqa: E402


def geometry_lod_entries(blob: bytes, nif: "Nif") -> list[dict]:
    """Replica of ParseNifGeos' BSGeometry branch (prefix -> LOD entries)."""
    geo_t = nif.idx_of("BSGeometry")
    if geo_t < 0:
        return []
    out: list[dict] = []
    for i in range(nif.nb):
        if nif.type_idx[i] != geo_t:
            continue
        block = blob[nif.offsets[i] : nif.offsets[i] + nif.sizes[i]]
        name, nextra = struct.unpack_from("<II", block, 0)
        if nextra > 1024:
            continue
        # name, extras[], controller, flags, 3f translation, 9f rotation,
        # scale, collision -> then bound sphere(4f), 6f, 3 i32.
        o = 8 + nextra * 4 + 4 + 4 + 12 + 36 + 4 + 4 + 40 + 12
        if o + 13 > len(block):
            continue
        flag = block[o]
        nidx, nv, unk = struct.unpack_from("<III", block, o + 1)
        path_at = o + 13
        ln = struct.unpack_from("<I", block, path_at)[0] if path_at + 4 <= len(block) else 0
        is_path = 0 < ln <= 512 and path_at + 4 + ln <= len(block)
        if is_path:
            raw = block[path_at + 4 : path_at + 4 + ln]
            is_path = all(32 <= c <= 126 for c in raw)
        out.append(
            {
                "block": i,
                "node": nif.strings[name] if name < len(nif.strings) else "",
                "flag": flag,
                "nidx": nidx,
                "nv": nv,
                "unk": unk,
                "path": block[path_at + 4 : path_at + 4 + ln].decode("latin-1") if is_path else "",
                "inline": b"" if is_path else block[path_at:],
            }
        )
    return out


class InlineNifGeometryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        for path in (SRC, EXTRACTED / "razorback_grip.nif"):
            if not path.is_file():
                raise unittest.SkipTest(f"missing {path}")
        cls.source = SRC.read_text(encoding="utf-8-sig")

    def test_blender_nif_inline_blob_matches_its_entry_header(self) -> None:
        nif_path = LIGHTSABER_DIR / "EngravedLightsaber.nif"
        if not nif_path.is_file():
            self.skipTest(f"lightsaber pack not installed ({nif_path})")
        blob = nif_path.read_bytes()
        entries = geometry_lod_entries(blob, Nif(blob))
        self.assertEqual(len(entries), 3, "the engraved hilt ships three geometries")
        for entry in entries:
            self.assertFalse(entry["path"], "a Blender NIF names no archive path")
            self.assertTrue(entry["inline"], entry["node"])
            self.assertEqual(entry["unk"], 0x40)
            info = parse_mesh_streams(entry["inline"])
            self.assertTrue(info, entry["node"])
            # The blob's own header must agree with the LOD entry: that is
            # what makes reading the block tail as a .mesh safe.
            self.assertEqual(info["nidx"], entry["nidx"], entry["node"])
            self.assertEqual(info["nv"], entry["nv"], entry["node"])
            self.assertTrue(info["uv_sets"], f"{entry['node']} has no UV stream")

    def test_bethesda_nif_still_reads_a_factory_path(self) -> None:
        blob = (EXTRACTED / "razorback_grip.nif").read_bytes()
        entries = geometry_lod_entries(blob, Nif(blob))
        self.assertTrue(entries)
        self.assertFalse(any(e["inline"] for e in entries), "vanilla geometry must stay a path")
        self.assertTrue(any("\\" in e["path"] for e in entries), "guid\\guid factory path")

    def test_inline_bytes_are_preferred_over_the_archive_lookup(self) -> None:
        body = self.source[self.source.index("static bool BuildSingleNif(") :]
        body = body[: body.index("static void AssembleSiblings(")]
        inline = body.index("if (!g.inlineMesh.empty())")
        lookup = body.index("ResolveAsset(mkey, meshBytes)")
        self.assertLess(inline, lookup, "inline geometry must not wait on a miss")
        self.assertIn("meshBytes = g.inlineMesh;", body)

        geos = self.source[self.source.index("bool ParseNifGeos(") :]
        geos = geos[: geos.index("bool ParseMeshLod0(")]
        self.assertIn("geo.inlineMesh.assign(blk + pathAt, blk + blen);", geos)
        # A printable path keeps the historical branch, or every vanilla NIF
        # would be read as inline garbage.
        self.assertLess(geos.index("bool isPath = ln != 0"), geos.index("geo.inlineMesh.assign("))


class SiblingAssemblyTests(unittest.TestCase):
    """The outfit fallback must identify the part from the basename only."""

    @classmethod
    def setUpClass(cls) -> None:
        if not SRC.is_file():
            raise unittest.SkipTest(f"missing {SRC}")
        cls.source = SRC.read_text(encoding="utf-8-sig")

    def test_part_token_is_taken_from_the_filename(self) -> None:
        helper = self.source[self.source.index("static const char* OutfitPart(") :]
        helper = helper[: helper.index("// Outfit assembly")]
        self.assertIn("find_last_of('/')", helper,
                      "a folder part token must not select the self part")
        self.assertIn("a_key.find(p, partStart)", helper,
                      "the self token must be searched from the basename")

        body = self.source[self.source.index("static void AssembleSiblings(") :]
        body = body[: body.index("// Normalize a form model path")]
        self.assertIn("OutfitPart(a_key, &partPos)", body,
                      "assembly must use the basename token position")
        self.assertIn("a_declaredParts.count(p)", body,
                      "never add a part type the record already declares")
        self.assertIn("sib.replace(partPos, std::strlen(self), p)", body,
                      "replace the filename token, not an earlier folder token")
        self.assertNotIn("sib.find(self)", body,
                         "the old whole-key replacement is the bug")

        models = self.source[self.source.index("bool BuildModels(") :]
        models = models[: models.index("bool ReadAsset(")]
        self.assertIn("declaredParts.insert(part)", models,
                      "the record's declared part types are collected up front")
        self.assertIn(
            "AssembleSiblings(a_key, a_out, totalVerts, sibKeys, declaredParts)",
            models,
            "assembly must receive the declared-part guard",
        )


if __name__ == "__main__":
    unittest.main()
