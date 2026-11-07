"""Regression tests for preview decal coverage versus CDB material alpha."""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2] / "src" / "OfflineModel.cpp"
RECON = SOURCE.parents[1] / "tools" / "renderer_recon"
sys.path.insert(0, str(RECON))
from cdb_alpha_replica import scan  # noqa: E402


class DecalBlendPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        source = SOURCE.read_text(encoding="utf-8-sig")
        match = re.search(
            r"float MaterialBlendAlpha\(const std::string& a_matPath, bool& a_translucent, bool& a_film\)\s*\{(?P<body>.*?)\n\t\}",
            source,
            re.S,
        )
        if match is None:
            raise AssertionError("MaterialBlendAlpha implementation not found")
        cls.body = match.group("body")
        cls.source = source

    def test_decal_sheet_is_kept_opaque_before_cdb_alpha_lookup(self) -> None:
        decal_guard = self.body.index("if (IsDecalSheet(a_matPath))")
        cdb_lookup = self.body.index("LiveModel::FindCdbAlpha(a_matPath, cdb, &layerKind)")
        self.assertLess(decal_guard, cdb_lookup)
        self.assertIn("a_translucent = false;", self.body[decal_guard:cdb_lookup])
        self.assertIn("return 1.0f;", self.body[decal_guard:cdb_lookup])

    def test_non_decal_materials_still_use_cdb_override_alpha(self) -> None:
        # The lookup also reports the object's TextureSetKind; a Multiply/Lerp
        # layer keeps its alpha out of the blend pass (see
        # test_cdb_layer_kind.py), but a material with no kind still does.
        self.assertIn(
            "if (LiveModel::FindCdbAlpha(a_matPath, cdb, &layerKind) && cdb < 0.995f && cdb > 0.0f)",
            self.body,
        )
        # ...and above the layer-weight floor: below it the value cannot be
        # coverage, so the range keeps its surface (see test_cdb_layer_kind.py).
        self.assertIn("if (cdb >= kMinSurfaceAlpha) {", self.body)
        self.assertIn("a_translucent = true;\n\t\t\t\treturn cdb;", self.body)

    def test_paired_bc3_opacity_mask_uses_texture_coverage_not_material_blend(self) -> None:
        shader = re.search(r'kPsTexAlphaSrc\[\]\s*=\s*R"\((?P<body>.*?)\)";', self.source, re.S)
        self.assertIsNotNone(shader, "textured mask shader not found")
        shader_body = shader.group("body")
        self.assertIn(
            "float mask = (g_alpha < 0.5 || (g_alpha > 4.5 && g_alpha < 5.5)) ? maskTex.a : maskTex.r;",
            shader_body,
        )
        self.assertIn("float sampleMask = g_alpha > 4.5 ? sampleMaskTex.a : sampleMaskTex.r;", shader_body)
        # A promoted cutout (mode 10) cuts on its own map: its companion opacity
        # map belongs to the layer behind it (the Sparkling Wine's net), so the
        # clip has to read the albedo when the albedo IS the mask.
        self.assertIn("clip((promotedMode ? tex.r : min(tex.a, mask)) - 0.5);", shader_body)
        self.assertIn("range.opacityRedChannel = img.opacityFormat == 80 || img.opacityFormat == 98;",
                      self.source)
        self.assertIn("(range.crucibleEmissive ? 6.0f : (range.vortexGrenadeEmissive ? 7.0f :", self.source)
        self.assertIn("(range.woodEmboss ? 2.0f : (range.opacityRedChannel ? 1.0f : 0.0f)", self.source)

    def test_rattler_decal_has_no_override_to_apply_as_coverage(self) -> None:
        # Rattler_Decals_Material1 writes no 0x25a at all; its coverage comes
        # from the paired _opacity map. The 2/255 this test used to pin was the
        # record before its name, i.e. the previous object's. Either way the
        # decal guard runs before the CDB lookup, so no override can reach it.
        self.assertNotIn("rattler_decals", scan())
        decal_guard = self.body.index("if (IsDecalSheet(a_matPath))")
        cdb_lookup = self.body.index("LiveModel::FindCdbAlpha(a_matPath, cdb, &layerKind)")
        self.assertLess(decal_guard, cdb_lookup)

    def test_card_back_poster_alpha_is_ignored_for_printed_surface(self) -> None:
        # Abstract_Poster2x1_Material1 is reused by nine poster records with
        # different alphas (0.0284-0.0461). First-wins under the name-first
        # order selects 0.03546, which is why the exact value is pinned: a
        # change in the walk that landed on another duplicate would show here.
        alpha = scan()["abstract_poster2x1"][3]
        self.assertAlmostEqual(alpha, 0.03546199947595596, places=6)
        self.assertLess(alpha, 0.05)
        classifier = re.search(
            r"bool IsDecalSheet\(const std::string& a_matPath\)\s*\{(?P<body>.*?)\n\t\t\}",
            self.source,
            re.S,
        )
        self.assertIsNotNone(classifier, "IsDecalSheet classifier not found")
        self.assertIn('ml.find("poster")', classifier.group("body"))
        decal_guard = self.body.index("if (IsDecalSheet(a_matPath))")
        cdb_lookup = self.body.index("LiveModel::FindCdbAlpha(a_matPath, cdb, &layerKind)")
        self.assertLess(decal_guard, cdb_lookup)

    def test_card_symbol_material_is_classified_as_decal(self) -> None:
        classifier = re.search(
            r"bool IsDecalSheet\(const std::string& a_matPath\)\s*\{(?P<body>.*?)\n\t\t\}",
            self.source,
            re.S,
        )
        self.assertIsNotNone(classifier, "IsDecalSheet classifier not found")
        self.assertIn('ml.find("symbol")', classifier.group("body"))

    def test_compact_razorback_grip_emblem_uses_exact_texture_and_survives_tiny_filter(self) -> None:
        live = (SOURCE.parents[0] / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        grip_route = re.search(
            r'normalizedMat == "materials/weapons/razorback/decals\.mat" &&\s*'
            r'normalizedNode\.find\("decals_grip"\).*?\{(?P<body>.*?)\n\t\t\}',
            live,
            re.S,
        )
        self.assertIsNotNone(grip_route, "Razorback grip-specific atlas route not found")
        # The standard wood atlas is the albedo; the Fancy badge is
        # separately sampled and composited in its dedicated shader mode.
        self.assertIn('textures/weapons/razorback/decals_color.dds', grip_route.group("body"))
        self.assertNotIn('textures/weapons/razorback/fancy/decals_color.dds', grip_route.group("body"))
        self.assertIn('textures/weapons/razorback/decals_opacity.dds', grip_route.group("body"))
        self.assertNotIn('textures/weapons/razorback/fancy/decals_opacity.dds', grip_route.group("body"))
        self.assertIn("Razorback grip wood atlas", grip_route.group("body"))
        self.assertIn('g_diffuseMemo.emplace(memoKey, std::move(verdict));', live)
        self.assertIn("a_outKey = a_key;", live)

        offline = (SOURCE.parent / "OfflineModel.cpp").read_text(encoding="utf-8-sig")
        self.assertIn('"textures/weapons/razorback/fancy/decals_color.dds"', offline)
        self.assertIn("loaded Razorback grip badge artwork", offline)
        self.assertIn("detail.format == 71 || detail.format == 28", offline)
        self.assertIn("detail.width == img.width && detail.height == img.height", offline)
        self.assertIn("range.razorbackGripBadge ? 3.0f", offline)
        self.assertIn("img.razorbackGripBadge", offline)
        self.assertIn("g_detail.Sample(g_samp, i.uv)", offline)
        self.assertIn("float badgeInk", offline)
        self.assertIn("neighborhood - detailLuma - 0.01", offline)
        self.assertNotIn("detailLuma - neighborhood - 0.01", offline)
        self.assertIn("neighborhood *= 0.125;", offline)
        self.assertIn("albedo *= 1.0 - badgeInk * 0.70;", offline)
        self.assertNotIn("albedo = lerp(albedo, detail, badgeInk);", offline)
        self.assertIn("if (g_alpha > 1.5 && g_alpha < 2.5)", offline)
        self.assertIn("float engravedLines", offline)
        prefetch = offline.index("void PrefetchTextures(")
        end_prefetch = offline.index("bool TakePrefetched(", prefetch)
        self.assertLess(offline.index("if (razorbackGripDecal)", prefetch), end_prefetch)
        self.assertIn("exact material map", live)

        classifier = re.search(
            r"bool IsRazorbackGripDecal\(const std::string& a_matPath\)\s*\{(?P<body>.*?)\n\t\t\}",
            self.source,
            re.S,
        )
        self.assertIsNotNone(classifier, "Razorback-specific decal classifier not found")
        self.assertIn('ml == "materials/weapons/razorback/decals.mat"', classifier.group("body"))
        self.assertIn("std::replace(ml.begin(), ml.end(), '\\\\', '/');", classifier.group("body"))
        builder = re.search(
            r"auto accumulate = \[&\]\(bool a_skipTiny\)\s*\{(?P<body>.*?)\n\t\t\};",
            self.source,
            re.S,
        )
        self.assertIsNotNone(builder, "GPU model range builder not found")
        self.assertIn("IsRazorbackGripDecal(mesh.matPath)", builder.group("body"))

    def test_tiny_decal_sheet_meshes_survive_the_size_filter(self) -> None:
        builder = re.search(
            r"auto accumulate = \[&\]\(bool a_skipTiny\)\s*\{(?P<body>.*?)\n\t\t\};",
            self.source,
            re.S,
        )
        self.assertIsNotNone(builder, "GPU model range builder not found")
        body = builder.group("body")
        tiny_filter = body.index("const bool tinyDecalSheet = tinyGeometry &&")
        decal_exemption = body.index("IsDecalSheet(mesh.matPath)", tiny_filter)
        tiny_skip = body.index("continue;", tiny_filter)
        self.assertLess(decal_exemption, tiny_skip)
        self.assertIn("tinyGeometry && !tinyDecalSheet && !MeshSpread(mesh, a_model.radius)", body[tiny_filter:tiny_skip])
        self.assertIn("IsRazorbackGripDecal(mesh.matPath)", body[tiny_filter:tiny_skip])
        self.assertIn("tinyGeometry &&", body[tiny_filter:tiny_skip])

    def test_crucible_weapon_materials_resolve_authored_atlases_and_static_emissive(self) -> None:
        live = (SOURCE.parent / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        diffuse_fn = live[live.index("bool FindDiffuse("):live.index("bool NifBaseIsPart(")]
        routes = {
            "cruciblehilt.mat": "textures/weapons/kgcdoom/crucible/crucible_color.dds",
            "crucible_edge_flow.mat": "textures/weapons/kgcdoom/crucible/crucible_blade_color.dds",
            "crucible_test_flicker.mat": "textures/weapons/kgcdoom/crucible/crucible_blade_graid_color.dds",
        }
        for material, texture in routes.items():
            with self.subTest(material=material):
                self.assertIn(material, diffuse_fn)
                self.assertIn(texture, diffuse_fn)
                self.assertLess(diffuse_fn.index(material), diffuse_fn.index("FindCdbMaterialLocked(a_matPath"))
        self.assertIn('"textures/weapons/kgcdoom/crucible/crucible_blade_energy_opacity.dds"', diffuse_fn)
        cdb_flat_return = diffuse_fn.index('REX::INFO("SFShopExplorer live: cdb flat material')
        self.assertLess(diffuse_fn.index('normalizedMat == "materials/weapons/kgcdoom/crucible/crucible_edge_flow.mat"'), cdb_flat_return)
        self.assertLess(diffuse_fn.index('normalizedMat == "materials/weapons/kgcdoom/crucible/crucible_test_flicker.mat"'), cdb_flat_return)
        self.assertIn('normalizedMat == "materials/weapons/kgcdoom/crucible/cruciblehilt.mat"', diffuse_fn)
        # An empty CDB texture set must not return early before the explicit
        # Crucible routes can provide their authored color atlases.
        self.assertLess(diffuse_fn.index('normalizedMat == "materials/weapons/kgcdoom/crucible/crucible_test_flicker.mat"'), diffuse_fn.index("FindCdbMaterialLocked(a_matPath"))

        for material in (
            "crucible_edge_flow.mat", "crucible_test_flicker.mat", "cruciblehilt.mat",
        ):
            with self.subTest(cdb_flat_material=material):
                self.assertIn(material, (RECON / "extracted" / "doom_crucible_probe.nif").read_bytes().decode("latin-1"))

        nif = (RECON / "extracted" / "doom_crucible_probe.nif").read_bytes()
        self.assertIn(b"cruciblehilt.mat", nif)
        self.assertIn(b"crucible_edge_flow.mat", nif)
        self.assertIn(b"crucible_test_flicker.mat", nif)

        offline = SOURCE.read_text(encoding="utf-8-sig")
        self.assertIn("bool crucibleEmissive{ false };", offline)
        self.assertIn('normalizedMat.ends_with("crucible_edge_flow.mat")', offline)
        self.assertIn('"textures/weapons/kgcdoom/crucible/crucible_blade_graid_emissive.dds"', offline)
        self.assertIn('"textures/weapons/kgcdoom/crucible/crucible_emissive.dds"', offline)
        self.assertIn("const bool crucibleMode = g_alpha > 5.5 && g_alpha < 6.5;", offline)
        # The clip arm names every exemption; the drink-bag print joined it in
        # mode 9 (its own coverage map is a stencil, not artwork coverage), and
        # mode 11 (a single-channel albedo read as a greyscale picture) joined
        # it with the Sparkling Wine's body. Dropping any of these would clip
        # emissive blades, grenades or a painted body.
        self.assertIn("else if (!crucibleMode && !vortexMode && !noClipMode && !maskInkMode)", offline)
        self.assertIn("const bool animateCrucible = g_gpuModel.texturesReady", offline)
        self.assertIn("if (!needUpload && !orbit && !animateCrucible)", offline)
        self.assertIn("range.vortexGrenadeEmissive = img.vortexGrenadeEmissive;", offline)
        self.assertIn("range.vortexGrenadeEmissive ? 7.0f", offline)
        self.assertIn("emissive.width == img.width && emissive.height == img.height", offline)
        self.assertNotIn("exactTexture(kVortexGrenadeColor, \"Shattered Space Vortex Grenade shared authored color/emissive atlases\",", live)
        self.assertIn("GetTickCount64() % 120000ULL", offline)
        self.assertIn("float flowPhase = frac(i.uv.y * 1.5 + i.color.r * 0.14);", offline)
        self.assertIn("albedo += float3(1.35, 0.10, 0.012) * glow;", offline)
        self.assertIn("range.crucibleEmissive ? animationTime : range.color[0]", offline)
        self.assertIn("range.crucibleEmissive = img.crucibleEmissive;", offline)
        self.assertIn("range.detailSlot >= 0", offline)

    def test_breach_bc4_decals_select_red_mask_channel(self) -> None:
        live = (SOURCE.parent / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        route = re.search(
            r'if \(normalizedMat == "materials/weapons/breach/decals\.mat"\) \{(?P<body>.*?)\n\t\t\}',
            live,
            re.S,
        )
        self.assertIsNotNone(route, "Breach exact decal texture route not found")
        self.assertIn('textures/weapons/breach/decals_color.dds', route.group("body"))
        self.assertIn('textures/weapons/breach/decals_opacity.dds', route.group("body"))

        prefetch = self.source.index("const bool maskFmtOk = opacity.format == 80 || opacity.format == 77 ||")
        self.assertLess(prefetch, self.source.index("t.opacityFormat = opacity.format;", prefetch))
        self.assertIn("range.opacityRedChannel = img.opacityFormat == 80 || img.opacityFormat == 98;",
                      self.source)
        self.assertIn(
            "float mask = (g_alpha < 0.5 || (g_alpha > 4.5 && g_alpha < 5.5)) ? maskTex.a : maskTex.r;",
            self.source,
        )
        self.assertIn("float sampleMask = g_alpha > 4.5 ? sampleMaskTex.a : sampleMaskTex.r;", self.source)
        self.assertIn("D3D12_STATIC_SAMPLER_DESC samp{};", self.source)
        self.assertIn("samp.Filter = D3D12_FILTER_ANISOTROPIC;", self.source)
        self.assertIn("samp.MaxAnisotropy = 8;", self.source)

    def test_breach_decal_atlas_contains_prints_covered_by_the_mask(self) -> None:
        from PIL import Image

        assets = SOURCE.parents[1] / "tools" / "renderer_recon" / "extracted"
        color = Image.open(assets / "decals_color_probe.png").convert("RGB")
        mask = Image.open(assets / "decals_opacity_probe.png").convert("RGB")
        self.assertEqual(color.size, (1024, 1024))
        self.assertEqual(mask.size, color.size)
        # Both decoded images retain RGB, so the same BC4 red component the
        # shader samples must contain opaque decal artwork inside the atlas.
        red_coverage = mask.getchannel("R").point(lambda value: 255 if value > 127 else 0)
        self.assertEqual(red_coverage.getbbox(), (0, 0, 259, 257))
        covered_print = 0
        for y in range(256):
            for x in range(259):
                r, g, b = color.getpixel((x, y))
                mr, _, _ = mask.getpixel((x, y))
                if mr > 127 and max(r, g, b) - min(r, g, b) > 35:
                    covered_print += 1
        self.assertGreater(covered_print, 1000)

    def test_breach_tactical_stock_preview_forces_flat_neutral_grey_without_wood(self) -> None:
        classifier = re.search(
            r"bool IsBreachTacticalStock\(const std::string& a_matPath, const std::string& a_nodeName\)\s*\{(?P<body>.*?)\n\t\t\}",
            self.source,
            re.S,
        )
        self.assertIsNotNone(classifier, "Breach Tactical Stock preview classifier not found")
        self.assertIn("std::replace(mat.begin(), mat.end(),", classifier.group("body"))
        self.assertIn('mat == "materials/weapons/breach/breach_stock_tactical.mat"', classifier.group("body"))
        self.assertIn('node.empty() || node.starts_with("breach_stock_tactical")', classifier.group("body"))
        self.assertIn('node == a_matPath || node == mat', classifier.group("body"))
        self.assertIn('node == "materials\\\\weapons\\\\breach\\\\breach_stock_tactical.mat"', classifier.group("body"))
        # Cached sfmodel meshes store the complete material path in the 64-byte
        # display-name as well as in matPath, so match that exact cached form.
        stock_cache = (SOURCE.parents[1] / "tools" / "renderer_recon" / "cache" / "breach_stock_tactical.sfmodel").read_bytes()
        self.assertEqual(stock_cache[:4], b"SFM1")
        self.assertIn(b"Materials\\Weapons\\Breach\\Breach_Stock_Tactical.mat", stock_cache[:1024])

        builder = re.search(
            r"auto accumulate = \[&\]\(bool a_skipTiny\)\s*\{(?P<body>.*?)\n\t\t\};",
            self.source,
            re.S,
        )
        self.assertIsNotNone(builder, "GPU model range builder not found")
        self.assertIn("const bool breachStock = IsBreachTacticalStock(mesh.matPath, mesh.name);", builder.group("body"))
        self.assertIn("base[0] = 0.60f;", builder.group("body"))
        self.assertIn("base[1] = 0.62f;", builder.group("body"))
        self.assertIn("base[2] = 0.64f;", builder.group("body"))
        self.assertIn("range.forceNeutralGrey = breachStock;", builder.group("body"))
        self.assertIn("if (!breachStock && LiveModel::FindCdbTint", builder.group("body"))
        cache_reader = (SOURCE.parents[1] / "tools" / "renderer_recon" / "build_model_cache.py").read_text(encoding="utf-8-sig")
        self.assertIn('"name": g.get("mat_path", "mat")', cache_reader)
        self.assertIn('"mat_path": g.get("mat_path", "")', cache_reader)
        cache_name = re.search(r'nm = m\["name"\]\.encode\("utf-8"\)\[:63\].*?o\.write\(nm\)', cache_reader, re.S)
        self.assertIsNotNone(cache_name, "cached mesh display-name writer not found")
        self.assertIn("mesh.name = name;", self.source)
        self.assertIn("node == a_matPath || node == mat", classifier.group("body"))

        upload = re.search(
            r"bool UploadModelTextures\(NativeList\* a_list, const std::vector<PrefetchedTex>& a_pre\)\s*\{(?P<body>.*?)\n\t\t\}",
            self.source,
            re.S,
        )
        self.assertIsNotNone(upload, "GPU model texture uploader not found")
        skip_stock = upload.group("body").index("if (range.forceNeutralGrey)")
        self.assertLess(skip_stock, upload.group("body").index("UploadTextureImage(", skip_stock))

        draw = re.search(
            r"bool RenderGpuModel\(NativeList\* a_list, const Model& a_model, const std::string& a_key, bool a_weapon\)\s*\{(?P<body>.*?)\n\t\t\}",
            self.source,
            re.S,
        )
        self.assertIsNotNone(draw, "GPU model render loop not found")
        self.assertIn("range.texSlot < 0 && !range.forceNeutralGrey && IsDecalSheet(range.matPath)", draw.group("body"))

        cpu_start = self.source.index("void Rasterize(const Model& a_model, bool a_weapon)")
        cpu_open = self.source.index("{", cpu_start)
        cpu_depth = 0
        for cpu_end in range(cpu_open, len(self.source)):
            if self.source[cpu_end] == "{":
                cpu_depth += 1
            elif self.source[cpu_end] == "}":
                cpu_depth -= 1
                if cpu_depth == 0:
                    break
        cpu_body = self.source[cpu_open + 1:cpu_end]
        self.assertIn("IsBreachTacticalStock(mesh.matPath, mesh.name)", cpu_body)
        self.assertIn("base[0] = 0.60f;", cpu_body)
        self.assertIn("base[1] = 0.62f;", cpu_body)
        self.assertIn("base[2] = 0.64f;", cpu_body)

    def test_neutral_wood_overrides_are_limited_to_breach_tactical_stock(self) -> None:
        self.assertIn("bool IsBreachTacticalStock(", self.source)
        self.assertIn('mat == "materials/weapons/breach/breach_stock_tactical.mat"', self.source)
        self.assertNotIn("IsWeaponStock", self.source)
        self.assertNotIn("IsRegulator", self.source)

    def test_breach_tactical_stock_uses_its_authored_stock_color_map(self) -> None:
        strings = (SOURCE.parents[1] / "tools" / "renderer_recon" / "extracted" / "cdb_strings.tsv").read_text(encoding="utf-8-sig")
        self.assertIn("Breach_Stock_Tactical_TextureSet1", strings)
        self.assertIn(r"Data\Textures\Weapons\breach\Stock_Tactical_color.dds", strings)
        self.assertIn(r"Data\Textures\Weapons\breach\Stock_Tactical_normal.dds", strings)
        self.assertIn(r"Data\Textures\Weapons\breach\Stock_Tactical_rough.dds", strings)
        self.assertIn(r"Data\Textures\Weapons\breach\Stock_Tactical_metal.dds", strings)
        self.assertIn(r"Data\Textures\Weapons\breach\Stock_Tactical_ao.dds", strings)
        self.assertIn('meshes/weapons/breach/breach_stock_tactical.nif',
                      (SOURCE.parents[1] / "tools" / "ba2_audit" / "extracted" / "ba2nameindex.tsv").read_text(encoding="utf-8-sig"))
        self.assertIn('Material1', strings)

    def test_shattered_space_plasma_cutter_materials_use_their_authored_color_maps(self) -> None:
        live = (SOURCE.parent / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        route = re.search(
            r"kPlasmaCutterMaps\[\]\s*=\s*\{(?P<body>.*?)\n\t\t\t\};",
            live,
            re.S,
        )
        self.assertIsNotNone(route, "Plasma Cutter material route table not found")
        pairs = re.findall(r'\{\s*"([^"]+)",\s*"([^"]+)"\s*\}', route.group("body"))
        self.assertEqual(dict(pairs), {
            "plasmacutter_accelerant.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_accelerant_color.dds",
            "plasmacutter_barrel.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_barrel_color.dds",
            "plasmacutter_base_1.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_base_1_color.dds",
            "plasmacutter_base_2.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_base_2_color.dds",
            "plasmacutter_decals.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_decals_color.dds",
            "plasmacutter_flapslong.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_flapslong_color.dds",
            "plasmacutter_handle.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_handle_color.dds",
            "plasmacutter_largeshieldedgrip.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_largeshieldedgrip_color.dds",
            "plasmacutter_magazine.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_magazine_color.dds",
            "plasmacutter_magazine_large.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_magazinelarge_color.dds",
            "plasmacutter_muzzlfocus.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_muzzlefocus_color.dds",
            "plasmacutter_muzzlspread.mat": "textures/weapons/sfbgs003/plasmacutter/plasmacutter_muzzlespread_color.dds",
        })
        self.assertIn('kPlasmaCutterDecalsOpacity =', live)
        self.assertIn('"textures/weapons/sfbgs003/plasmacutter/plasmacutter_decals_opacity.dds"', live)
        self.assertIn('normalizedMat.ends_with("plasmacutter_decals.mat")', live)


if __name__ == "__main__":
    unittest.main()
