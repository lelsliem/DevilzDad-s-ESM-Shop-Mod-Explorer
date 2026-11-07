"""Regressions for the direct weapon-preview material diagnostic."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHATTERED_SPACE_TEXTURES = Path(
    "B:/SteamLibrary/steamapps/common/Starfield/Data/ShatteredSpace - Textures.ba2")
sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))
from material_sweep import TextureIndex, build_cdb_index, opacity_pair_issue  # noqa: E402


class WeaponMaterialDiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.offline_h = (ROOT / "src" / "OfflineModel.h").read_text(encoding="utf-8-sig")
        cls.offline_cpp = (ROOT / "src" / "OfflineModel.cpp").read_text(encoding="utf-8-sig")
        cls.shop = (ROOT / "src" / "ShopExplorer.cpp").read_text(encoding="utf-8-sig")
        cls.live_cpp = (ROOT / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        cls.overlay_cpp = (ROOT / "src" / "ImguiOverlay.cpp").read_text(encoding="utf-8-sig")
        cls.shop_h = (ROOT / "src" / "ShopExplorer.h").read_text(encoding="utf-8-sig")

    def test_snapshot_exposes_per_mesh_resolved_texture_and_validation_fields(self) -> None:
        for field in (
            "std::string mesh;", "std::string material;", "std::string colorMap;",
            "std::string opacityMap;", "std::string colorStatus;", "std::string opacityStatus;",
            "std::uint32_t colorFormat", "std::uint32_t colorWidth", "std::uint32_t colorHeight",
            "std::uint32_t opacityFormat", "std::uint32_t opacityWidth", "std::uint32_t opacityHeight",
        ):
            self.assertIn(field, self.offline_h)
        self.assertIn("std::vector<MaterialDiagnostic> materials;", self.offline_h)

    def test_cdb_tint_log_reports_candidates_not_final_tinted_ranges(self) -> None:
        build = self.offline_cpp[
            self.offline_cpp.index("bool BuildGpuModel("):self.offline_cpp.index("bool UploadTextureImage(")
        ]
        self.assertIn("cdbTintCandidates", build)
        self.assertIn("CDB override tint candidate", build)
        self.assertNotIn("range(s) tinted from the CDB override colour", build)
        upload = self.offline_cpp[
            self.offline_cpp.index("bool UploadModelTextures("):self.offline_cpp.index("bool EnsureWhite(")
        ]
        self.assertIn("range.color[0] = range.color[1] = range.color[2] = 1.0f", upload)


    def test_prefetch_reports_flat_missing_single_channel_and_opacity_rejection(self) -> None:
        prefetch = self.offline_cpp[
            self.offline_cpp.index("void PrefetchTextures("):self.offline_cpp.index("bool TakePrefetched(")
        ]
        for status in (
            "flat (no albedo in material set)", "missing/unreadable", "rejected single-channel albedo",
            "promoted opacity cutout", "exact material albedo", "resolved color map", "paired",
            "unsupported mask format", "aspect mismatch", "degenerate <8x8 mask", "empty mask data", "none (opaque)",
        ):
            self.assertIn(status, prefetch)
        self.assertIn("diagnostic.colorMap = ddsKey;", prefetch)
        self.assertIn("diagnostic.opacityMap = opacityKey;", prefetch)
        self.assertIn("constexpr std::uint32_t kMinAuthoredMaskDimension = 8;", prefetch)
        self.assertIn("opacity.width < kMinAuthoredMaskDimension", prefetch)
        self.assertIn("opacity.height < kMinAuthoredMaskDimension", prefetch)
        self.assertIn("!degenerateMask", prefetch)
        self.assertNotIn("bigEnough && !opacity.bytes.empty()", prefetch)
        self.assertIn("g_materialDiagnostics[a_key] = diagnostics;", prefetch)
        self.assertIn("g_snapshot.materials = std::move(diagnostics);", prefetch)
        self.assertIn("g_snapshot.materials = std::move(cachedDiagnostics);", prefetch)

    def test_sub_block_masks_are_rejected_but_small_valid_masks_are_kept(self) -> None:
        sweep = (ROOT / "tools" / "ba2_audit" / "material_sweep.py").read_text(encoding="utf-8-sig")
        self.assertIn("def opacity_pair_issue(color, opacity)", sweep)
        self.assertIn("stats[\"opacity_scaled\"] += 1", sweep)

        class FakeTex:
            def __init__(self, width: int, height: int, dxgi: int):
                self.width, self.height, self.dxgi = width, height, dxgi

        class FakeArchive:
            path = Path("base.ba2")

        index = TextureIndex.__new__(TextureIndex)
        index.map = {
            "c.dds": (FakeArchive(), FakeTex(2048, 2048, 72)),
            "valid_small_mask.dds": (FakeArchive(), FakeTex(128, 128, 80)),
            "degenerate_mask.dds": (FakeArchive(), FakeTex(4, 4, 80)),
            "small_authored_mask.dds": (FakeArchive(), FakeTex(8, 8, 80)),
            "magshear_color.dds": (FakeArchive(), FakeTex(2048, 2048, 72)),
            "magshear_opacity.dds": (FakeArchive(), FakeTex(512, 512, 80)),
        }
        self.assertTrue(index.has("valid_small_mask.dds"))
        self.assertEqual(index.map["valid_small_mask.dds"][1].width, 128)
        self.assertEqual(index.map["degenerate_mask.dds"][1].width, 4)
        self.assertEqual(opacity_pair_issue(index.map["c.dds"][1], index.map["degenerate_mask.dds"][1]),
                         "degenerate <8x8")
        self.assertEqual(opacity_pair_issue(index.map["c.dds"][1], index.map["small_authored_mask.dds"][1]), None)
        pair_color, pair_mask = index.map["magshear_color.dds"][1], index.map["magshear_opacity.dds"][1]
        self.assertEqual(opacity_pair_issue(pair_color, pair_mask), None)
        self.assertEqual(opacity_pair_issue(index.map["c.dds"][1], FakeTex(2, 2, 80)), "degenerate <8x8")
        self.assertEqual(opacity_pair_issue(None, FakeTex(1024, 512, 80)), None)
        self.assertEqual(opacity_pair_issue(index.map["c.dds"][1], FakeTex(128, 32, 80)), "aspect ratio differs")

    def test_dxgi_srgb_variants_stay_unorm_for_preview_compatibility(self) -> None:
        live = (ROOT / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        map_format = live[live.index("std::uint32_t MapTexFormat("):live.index("std::size_t TexRowPitch(")]
        # DXGI's BC1/2/3 _SRGB values (72/75/78) normalize to their
        # corresponding UNORM view formats, preserving established sampling.
        self.assertIn("case 72: return 71;", map_format)
        self.assertIn("case 75: return 74;", map_format)
        self.assertIn("case 78: return 77;", map_format)
        self.assertIn("Match the preview's original UNORM sampling", map_format)
        self.assertIn("sRGB variants darkened weapon colors", map_format)
        read_texture = live[live.index("bool ReadTexture("):live.index("struct ColorEntry")]
        self.assertIn("a_out.mipLevels = mipLevels;", read_texture)
        self.assertIn("a_out.bytes = std::move(raw);", read_texture)
        self.assertIn("std::vector<std::pair<const TexChunk*, std::size_t>> mipChunks;", read_texture)
        self.assertIn("raw.insert(raw.end(), chunkRaw.begin(), chunkRaw.begin() + chunkBytes);", read_texture)
        dds_reader = live[live.index("bool DdsToImage("):live.index("bool ReadTexture(")]
        self.assertIn("a_out.mipLevels = totalBytes == topMipBytes ? 1", dds_reader)

        upload = self.offline_cpp[
            self.offline_cpp.index("bool UploadTextureImage("):self.offline_cpp.index("bool UploadModelTextures(")
        ]
        self.assertIn("td.Format = static_cast<DXGI_FORMAT>(a_format);", upload)
        self.assertIn("td.MipLevels = static_cast<UINT16>(std::clamp(a_mipLevels, 1u, 16u));", upload)
        self.assertIn("GetCopyableFootprints(&td, 0, mipLevels", upload)
        self.assertIn("a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);", upload)
        self.assertIn("srv.Texture2D.MipLevels = mipLevels;", upload)
        self.assertIn("srv.Format = static_cast<DXGI_FORMAT>(a_format);", upload)
        self.assertIn("srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;", upload)

    def test_runtime_texture_reader_keeps_contiguous_base_mip_chains(self) -> None:
        live = (ROOT / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        reader = live[live.index("bool ReadTexture("):live.index("struct ColorEntry")]
        self.assertIn("std::uint32_t nextMip = 0;", reader)
        self.assertIn("c.startMip == nextMip", reader)
        self.assertIn("std::vector<std::pair<const TexChunk*, std::size_t>> mipChunks;", reader)
        self.assertIn("raw.insert(raw.end(), chunkRaw.begin(), chunkRaw.begin() + chunkBytes);", reader)
        self.assertIn("a_out.mipLevels = mipLevels;", reader)
        dds_reader = live[live.index("bool DdsToImage("):live.index("bool ReadTexture(")]
        self.assertIn("a_out.mipLevels = totalBytes == topMipBytes ? 1", dds_reader)
        self.assertIn("std::uint32_t mipLevels{ 1 };", self.offline_cpp)
        self.assertIn("t.mipLevels = img.mipLevels;", self.offline_cpp)
        self.assertIn("img.mipLevels, img.rowPitch", self.offline_cpp)
        self.assertIn("img.opacityMipLevels, img.opacityRowPitch", self.offline_cpp)
        self.assertIn("img.detailMipLevels, img.detailRowPitch", self.offline_cpp)

    def test_weapon_details_render_scrollable_material_records(self) -> None:
        self.assertIn('ImGui::CollapsingHeader("Resolved weapon materials")', self.shop)
        self.assertNotIn('ImGui::CollapsingHeader("Resolved weapon materials", ImGuiTreeNodeFlags_DefaultOpen)', self.shop)
        self.assertIn("snap.label == FormPreviewLabel(form)", self.shop)
        self.assertIn("snap.materials.size(), 48", self.shop)
        self.assertIn('ImGui::TextWrapped("Material: %s"', self.shop)
        self.assertIn('ImGui::TextWrapped("Color: %s%s%s"', self.shop)
        self.assertIn('ImGui::TextWrapped("Opacity: %s%s%s"', self.shop)
        self.assertIn("ImGui::TreePop();", self.shop)

    def test_body_text_size_includes_persisted_extra_large_option(self) -> None:
        self.assertIn("ImFont* g_bodyFonts[4]{};", self.overlay_cpp)
        self.assertIn("g_bodyFonts[3] = LoadUiFont(ranges, fontPx * 1.40f);", self.overlay_cpp)
        self.assertIn("std::clamp(g_bodyFontSize, 0, 3)", self.overlay_cpp)
        self.assertIn("g_bodyFontSize = std::clamp(a_size, 0, 3);", self.overlay_cpp)
        self.assertIn('"Extra Large"', self.shop)
        self.assertIn("kTextSizes[std::clamp(textSize, 0, 3)]", self.shop)
        self.assertIn("for (int size = 0; size < 4; ++size)", self.shop)
        self.assertIn("a_size = std::clamp(saved, 0, 3);", self.shop)
        self.assertIn("std::clamp(a_size, 0, 3)", self.shop)
        self.assertIn("int textSize{ 1 };  // 0 small, 1 medium, 2 large, 3 extra large", self.shop_h)

    def test_windows_breechblock_paths_reach_both_weapon_path_branches(self) -> None:
        # CaseFold preserves Windows separators; normalize one backslash
        # before searching for the Breech Block path components.
        normalizers = [line for line in self.shop.splitlines()
                       if "std::replace(foldedModel.begin()" in line or "std::replace(foldedBase.begin()" in line]
        self.assertEqual(len(normalizers), 2)
        for line in normalizers:
            self.assertIn("'\\\\'", line)
            self.assertNotIn("'\\\\\\\\'", line)
        self.assertIn('foldedModel.find("/breechblock/")', self.shop)
        self.assertIn('foldedBase.find("/breechblock/")', self.shop)
        self.assertIn("dummyBase = dummyBase || breechBlockAssembly;", self.shop)
        self.assertIn("CollectWeaponAssemblyNifs(form, pushUnique);", self.shop)

    @unittest.skipUnless(SHATTERED_SPACE_TEXTURES.is_file(),
                         "Shattered Space textures are not installed here")
    def test_vortex_grenade_texture_bindings_exist_in_shipped_texture_archive(self) -> None:
        # NIF material paths use vortexgrenade; the archived texture folder
        # has the typo vortextgrenade. Verify that mapping against the actual
        # DLC texture archive, not just source-code string assertions.
        from ba2lib import Ba2Archive, map_tex_format

        nif = ROOT / "tools" / "renderer_recon" / "extracted" / "vortexgrenadephasing_probe.nif"
        nif_text = nif.read_bytes().decode("latin-1").lower()
        self.assertIn(r"\materials\weapons\dlc001\vortexgrenade\vortexgrenade_phasing.mat", nif_text)
        material_prefix = "materials/weapons/dlc001/vortexgrenade/"
        color_key = "textures/weapons/dlc001/vortextgrenade/lambert1_color.dds"
        emissive_key = "textures/weapons/dlc001/vortextgrenade/lambert1_emissive.dds"
        tex_ba2 = Ba2Archive(SHATTERED_SPACE_TEXTURES)
        for key in (color_key, emissive_key):
            self.assertIn(key, tex_ba2.tex_files)
            tex_file = tex_ba2.tex_files[key]
            self.assertGreaterEqual(tex_file.width, 8)
            self.assertGreaterEqual(tex_file.height, 8)
            decoded, error = tex_ba2.read_texture(key)
            self.assertEqual(error, "", f"{key}: {error}")
            self.assertIsNotNone(decoded, f"{key} must decode")
            self.assertGreaterEqual(len(decoded), tex_file.width * tex_file.height // 2)
            self.assertEqual(map_tex_format(tex_file.dxgi), 71)
        self.assertIn(f'"{color_key}"', self.live_cpp.lower())
        self.assertIn(material_prefix, self.live_cpp.lower())
        self.assertIn(f'"{emissive_key}"', self.offline_cpp.lower())
        self.assertIn('normalizedmat == "materials/weapons/dlc001/vortexgrenade/vortexgrenade_phasing.mat"', self.offline_cpp.lower())
        self.assertIn("vortexGrenadeEmissive", self.offline_cpp)
        self.assertIn("const bool vortexMode = g_alpha > 6.5 && g_alpha < 7.5;", self.offline_cpp)
        self.assertIn("albedo += detail * (0.45 + glow * 0.65);", self.offline_cpp)

    def test_selected_breach_and_regulator_maps_have_shipped_ownership(self) -> None:
        cdb = ROOT / "tools" / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        index = build_cdb_index(cdb)
        self.assertEqual(index["breach_stock_tactical"]["color"],
                         "textures/weapons/breach/stock_tactical_color.dds")
        self.assertEqual(index["breach_receiver"]["color"],
                         "textures/weapons/breach/breach_receiver_color.dds")
        self.assertEqual(index["regulator_decal"], {
            "color": "textures/weapons/regulator/regulator_decals_color.dds",
            "opacity": "textures/weapons/regulator/regulator_decals_opacity.dds",
        })
        # These shipped albedo/mask pairs include 4x4 placeholder masks;
        # dimensions/format must not be mistaken for evidence of authored coverage.
        self.assertEqual(index["breach_magazine"]["opacity"],
                         "textures/weapons/breach/breach_magazine_opacity.dds")
        self.assertEqual(index["tombstone_scope"]["opacity"],
                         "textures/weapons/tombstone/tombstone_scope_opacity.dds")
        self.assertEqual(index["rattler_slideandlower"]["opacity"],
                         "textures/weapons/rattler/heater_slideandlower_opacity.dds")
        # These generic CDB keys are the concrete cross-family traps which
        # appeared on the supplied Breach/Regulator preview investigation.
        self.assertIn("magstorm/barrel_color.dds", index["barrel"]["color"])
        self.assertIn("awp_shareable/receiver_color.dds", index["receiver"]["color"])
        # This guard lives in LiveModel.cpp, which is the source resolver the
        # diagnostic reports; keep the exact family check covered separately.
        live = (ROOT / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        family_fn = live[live.index("bool CdbWeaponTextureMatchesFamily("):live.index("bool FindDiffuse(")]
        self.assertIn('"barrel", "grip", "receiver"', family_fn)
        self.assertIn("!CdbWeaponTextureMatchesFamily(a_matPath, cdbColor)", live)


if __name__ == "__main__":
    unittest.main()
