"""BC3 alpha versus BC4 red opacity-mask channel regressions."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2] / "src" / "OfflineModel.cpp"


class DecalMaskChannelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8-sig")

    def test_shader_selects_alpha_for_bc3_and_red_for_bc4(self) -> None:
        match = re.search(r"kPsTexAlphaSrc\[\]\s*=\s*R\"\((?P<body>.*?)\)\";", self.source, re.S)
        self.assertIsNotNone(match, "textured alpha-mask shader not found")
        body = match.group("body")
        self.assertIn(
            "float mask = (g_alpha < 0.5 || (g_alpha > 4.5 && g_alpha < 5.5)) ? maskTex.a : maskTex.r;",
            body,
        )
        self.assertIn("float sampleMask = g_alpha > 4.5 ? sampleMaskTex.a : sampleMaskTex.r;", body)

    def test_bc3_alpha_and_bc4_red_formats_select_the_matching_shader_channel(self) -> None:
        self.assertIn("const bool maskFmtOk = opacity.format == 80 || opacity.format == 77 ||", self.source)
        self.assertIn("range.opacityRedChannel = img.opacityFormat == 80 || img.opacityFormat == 98;",
                      self.source)
        self.assertIn(
            "float mask = (g_alpha < 0.5 || (g_alpha > 4.5 && g_alpha < 5.5)) ? maskTex.a : maskTex.r;",
            self.source,
        )
        self.assertIn("float sampleMask = g_alpha > 4.5 ? sampleMaskTex.a : sampleMaskTex.r;", self.source)
        # BC3 (77) keeps mode 0 and therefore samples alpha; BC4 (80)
        # selects mode 1 and samples red. Detail modes 2/3 stay separate.
        self.assertIn("(range.crucibleEmissive ? 6.0f : (range.vortexGrenadeEmissive ? 7.0f :", self.source)
        # Nesting depth is not pinned: the film mode was added as one more
        # arm of the same conditional, so only the mapping matters here.
        self.assertIn("(range.woodEmboss ? 2.0f : (range.opacityRedChannel ? 1.0f : 0.0f)", self.source)
        self.assertIn("if (g_alpha > 1.5 && g_alpha < 2.5)", self.source)
        self.assertIn("g_alpha > 1.5 && g_alpha < 2.5 ? saturate", self.source)
        self.assertIn("float badgeInk = saturate((neighborhood - detailLuma - 0.01) * 24.0);", self.source)
        self.assertIn("albedo *= 1.0 - badgeInk * 0.70;", self.source)
        self.assertIn("range.translucent ? range.alpha : shaderMode", self.source)
        self.assertIn("if (g_alpha > 1.5 && g_alpha < 3.5)", self.source)
        self.assertIn("g_detail.Sample(g_samp, i.uv)", self.source)
        self.assertIn("D3D12_DESCRIPTOR_RANGE detailRange", self.source)
        self.assertIn("SetGraphicsRootDescriptorTable(3, detailHandle)", self.source)
        # Compact atlas patches must survive the GPU size filter, while full
        # geometry cannot bypass the spread test solely due to a decal name.
        self.assertIn("const bool tinyDecalSheet = tinyGeometry &&", self.source)
        self.assertIn("IsDecalSheet(mesh.matPath)", self.source)
        self.assertIn("tinyGeometry && !tinyDecalSheet && !MeshSpread(mesh, a_model.radius)", self.source)
        self.assertIn("IsRazorbackGripDecal(mesh.matPath)", self.source)
        self.assertIn("const bool tinyDecalSheet = tinyGeometry && IsDecalSheet(mesh.matPath);", self.source)

    def test_weapon_textures_use_anisotropic_filtering_for_grazing_surfaces(self) -> None:
        self.assertIn("samp.Filter = D3D12_FILTER_ANISOTROPIC;", self.source)
        self.assertIn("samp.MaxAnisotropy = 8;", self.source)
        self.assertIn("samp.MaxLOD = D3D12_FLOAT32_MAX;", self.source)
        self.assertIn("samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;", self.source)
        self.assertIn("samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;", self.source)

    def test_default_weapon_preview_resolution_matches_large_viewports(self) -> None:
        self.assertIn("std::uint32_t g_width{ 1024 };", self.source)
        self.assertIn("std::uint32_t g_height{ 768 };", self.source)
        self.assertIn('GetPrivateProfileIntW(L"offlinepreview", L"width", 1024, ini.c_str())', self.source)
        self.assertIn('GetPrivateProfileIntW(L"offlinepreview", L"height", 768, ini.c_str())', self.source)

    def test_format_80_bc4_uses_red_default_component_mapping(self) -> None:
        self.assertIn("opacity.format == 77 ? \"alpha\" : \"red\"", self.source)
        self.assertIn("range.opacityRedChannel = img.opacityFormat == 80 || img.opacityFormat == 98;",
                      self.source)
        self.assertIn("srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;", self.source)

    def test_repacked_bc7_mask_keeps_its_cutout(self) -> None:
        """A mask repacked as BC7 is still a mask, and a borrowed one is
        skipped whatever channel count it has.

        The 2026-10-24 Test profile repacks both the Erdebrau pilsner label
        mask and the coolant jar's lid mask as BC7_UNORM_SRGB (greyscale ink in
        RGB, alpha at 255). Rejecting 98 cost the can its cutout - its atlas'
        black surround printed over the body - and made the jar's label sleeve
        stamp the lid's mask as ink, which is the black star back on screen.
        """
        self.assertIn("opacity.format == 98", self.source)
        self.assertIn("img.opacityFormat == 98", self.source)
        start = self.source.index("if (IsDecalSheet(range.matPath) &&")
        gate = self.source[start:start + 220]
        self.assertIn("(borrowedMask ||", gate,
                      "the borrowed-mask skip is gated on the decal's own channel count")


if __name__ == "__main__":
    unittest.main()
