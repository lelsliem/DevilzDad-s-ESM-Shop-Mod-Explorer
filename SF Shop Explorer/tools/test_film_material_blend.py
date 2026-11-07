#!/usr/bin/env python3
"""Guard for the crafted drink-bag film rule in MaterialBlendAlpha.

Alien Tea rendered as an opaque gold shell: the pouch is a transparent bag in
the inventory, but the material database stores no alpha for it and the old
name tokens (glass/clear/translucent/transpar) do not describe a drink pouch.

The pouch's own opacity map is coverage, not a stencil. Measured from
Starfield - Textures05.ba2: BC4, 1024x1024, mean 87/255, 34.5% of texels near
black, 17.5% near white, smooth in between. A cutout keeps only the bright band
and erases the rest; blending it keeps the soft film edges and the faint print.
So the tests pin four things: the rule's narrow keying, that the range is drawn
in the blended pass (not the constant-alpha pass, which never samples the map),
that the shader blends rather than clips for this mode, and that the film keeps
the database's override tint (its atlas region is a neutral grey, so the tint
is the bag's colour).

The rule must stay narrow. The renderer's own notes record that the broader
"this material names an opacity map" rule was measured and rejected -- it fires
on 52% of materials the database calls opaque. So this rule keys on the pouch
inside the crafted drink-bag folder, and must NOT catch the tea, leaf or tonic
materials beside it, or the bag's contents would vanish.

Source-structure assertions only; the look needs a running game.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OFFLINE = ROOT / "src" / "OfflineModel.cpp"


class FilmMaterialBlendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = OFFLINE.read_text(encoding="utf-8-sig")
        # The MaterialBlendAlpha body, trimmed at its final opaque return.
        body = cls.src.split(
            "float MaterialBlendAlpha(const std::string& a_matPath, bool& a_translucent, bool& a_film)", 1
        )[1]
        cls.body = body[: body.index("bool BuildGpuModel")]
        cls.shader = cls.src.split("static constexpr const char kPsTexAlphaSrc[]", 1)[1]
        cls.shader = cls.shader[: cls.shader.index("void ReleaseGpuPipe")]
        cls.render = cls.src.split("bool RenderGpuModel(", 1)[1]
        # Trim at the next function that closes the render body.
        cls.render = cls.render[: cls.render.index("void Rasterize")]

    def test_rule_requires_both_the_folder_and_the_pouch_name(self) -> None:
        # The folder test lives in its own helper, shared with the override-tint
        # rule so the two cannot disagree about what a drink bag is.
        helper = self.src.split("bool IsCraftedDrinkBagMaterial(const std::string& a_matPath)", 1)[1]
        helper = helper[: helper.index("float MaterialBlendAlpha")]
        self.assertIn('find("crafted_drink_bags") != std::string::npos', helper)
        self.assertIn('ml.find("pouch") != std::string::npos', self.body)
        # Both conditions joined by &&, not ||: the leaf and tea materials sit in
        # the same folder and must stay opaque.
        self.assertRegex(
            self.body,
            r'IsCraftedDrinkBagMaterial\(a_matPath\)[^;]*&&[^;]*ml\.find\("pouch"\)',
        )

    def test_film_is_flagged_not_constant_alpha(self) -> None:
        # The constant-alpha pass (psoBlend) never samples the opacity map, so a
        # film range flagged translucent would show a flat wash, not a bag.
        film = self.body.split('ml.find("pouch")', 1)[1]
        film = film[: film.index("return")]
        self.assertIn("a_film = true;", film)
        self.assertNotIn("a_translucent = true;", film)

    def test_flag_defaults_off_before_any_rule_runs(self) -> None:
        self.assertIn("a_film = false;", self.body)
        self.assertLess(self.body.index("a_film = false;"), self.body.index('ml.find("pouch")'))

    def test_rule_runs_after_the_decal_and_database_checks(self) -> None:
        # Decals must keep their own coverage path, and a database alpha (when
        # present) is authoritative; the film rule is a fallback for neither.
        decal = self.body.index("IsDecalSheet(a_matPath)")
        cdb = self.body.index("FindCdbAlpha")
        film = self.body.index('ml.find("pouch")')
        self.assertLess(decal, film)
        self.assertLess(cdb, film)

    def test_rule_is_not_a_general_opacity_map_heuristic(self) -> None:
        # The measured-bad rule: decide translucency from the presence of an
        # opacity map. If someone later widens this, the comment trail in
        # OfflineModel.cpp says why that fails.
        self.assertNotIn("opacity", self.body.split("IsCraftedDrinkBagMaterial(a_matPath)")[1][:200].lower())

    def test_shader_inverts_the_two_layer_composite(self) -> None:
        # The pouch is a closed bag (314 front triangles against 342 back of
        # equal area, 0.3905 / 0.3892) and this renderer rasters with
        # CULL_MODE_NONE, so the map is composited twice outside the drink:
        # 1-(1-mask)^2 = 0.41 where the map says 0.23, which measured ~40 of
        # grey over the background against the reference's +10. a =
        # 1-sqrt(1-mask) puts the pair back on the map's own coverage (0.12
        # each) without a second PSO or a winding test. An additive floor (the
        # first revision used 0.35 + 0.65 * mask) must not grow back.
        self.assertIn("const bool filmBlend = g_alpha > 7.5 && g_alpha < 8.5;", self.shader)
        blend_arm = self.shader.index("else if (filmBlend)")
        arm = self.shader[blend_arm:self.shader.index("else if (!crucibleMode", blend_arm)]
        self.assertIn("outputAlpha = 1.0 - sqrt(1.0 - mask);", arm)
        self.assertNotIn("kFilmFloor", arm)
        self.assertNotIn("kFilmFloor", self.shader)
        self.assertNotIn("0.35", arm)
        # And the branch still reads the mask at all.
        self.assertIn("float mask =", self.shader)

    def test_drink_bags_keep_the_database_override_tint(self) -> None:
        # UploadModelTextures resets every textured range's colour to white, so
        # a range has to carry the override tint across that boundary. For the
        # crafted drink bags that reset is wrong: CraftedFood_AlienTea.mat's
        # override is (0.426, 0.426, 0.426), an even darkening the game applies
        # and the preview dropped, leaving the liquid far brighter than the
        # inventory reference. Scoped to the folder rather than to every
        # material, because one measured case is not a catalogue-wide rule.
        self.assertIn("bool cdbOverrideTint{ false };", self.src)
        self.assertIn(
            "range.cdbOverrideTint = IsCraftedDrinkBagMaterial(mesh.matPath);", self.src
        )
        self.assertIn("if (!img.maskCutout && !range.cdbOverrideTint)", self.src)
        # The tint has to be routed after the range exists and before it is
        # pushed, and the upload has to consult the flag it set.
        tint = self.src.index("range.cdbOverrideTint = IsCraftedDrinkBagMaterial")
        self.assertLess(self.src.index("FindCdbTint(mesh.matPath, tint)"), tint)
        self.assertLess(tint, self.src.index("g_gpuModel.ranges.push_back(range);"))

    def test_film_mode_is_excluded_from_the_clip(self) -> None:
        # clip() is the behaviour this change replaces: the film branch must be
        # its own arm, not fall through to the min(tex.a, mask) cutout.
        blend_arm = self.shader.index("else if (filmBlend)")
        clip_arm = self.shader.index("clip((promotedMode ? tex.r : min(tex.a, mask)) - 0.5);")
        self.assertLess(blend_arm, clip_arm)

    def test_render_routes_film_through_the_blended_pipeline(self) -> None:
        self.assertIn("const bool filmBlend = range.filmCoverage && range.opacitySlot >= 0", self.render)
        # Second pass, then the blend-enabled PSO -- not psoBlend, and not the
        # opaque ones.
        # The routing is a pass selector now that the additive shell pass
        # exists; the blended ranges are still exactly the pass-1 branch.
        self.assertIn("} else if (range.translucent || decalCoverage || filmBlend) {", self.render)
        self.assertIn("if (pass != 1) {", self.render)
        self.assertIn("if (filmBlend) {", self.render)
        film_arm = self.render.split("if (filmBlend) {", 1)[1]
        film_arm = film_arm[: film_arm.index("} else if")]
        self.assertIn("psoDecalCoverage", film_arm)
        self.assertNotIn("psoBlend", film_arm)

    def test_film_mode_is_encoded_into_the_constant_buffer(self) -> None:
        self.assertIn("const float shaderMode = filmBlend ? 8.0f :", self.render)

    def test_drink_inside_the_bag_is_classified_from_its_own_coverage(self) -> None:
        # The bag's four parts are only distinguishable by the shared map:
        # under each mesh's real UV footprint it measures film 0.24, printed
        # leaf 0.47, drink 0.76, cap 0.96. The drink has to blend (the
        # reference shows the submerged leaf through it) while the leaf and cap
        # stay solid, so the rule is that band and nothing else.
        self.assertIn("std::vector<float> uvSamples;", self.src)
        self.assertIn("static constexpr std::size_t kMaxUvSamples = 64;", self.src)
        self.assertIn("float SampleBc4Mask(", self.src)
        # BC4 is not BC3: sixteen 3-bit selectors and an 8-entry palette. Read as
        # 2-bit selectors (a first draft did) a quarter of the texels come back
        # as 255. The decode below was checked against the reference decoder on
        # 3,000 random texels of the shipped map, 0 mismatches.
        bc4 = self.src.split("float SampleBc4Mask(", 1)[1]
        bc4 = bc4[: bc4.index("bool UploadModelTextures")]
        self.assertIn("const int sel = static_cast<int>((bits >> (3 * texel)) & 7u);", bc4)
        self.assertIn("((8 - sel) * r0 + (sel - 1) * r1) / 7", bc4)
        self.assertIn("((6 - sel) * r0 + (sel - 1) * r1) / 5", bc4)
        self.assertIn("value = 255;", bc4)
        self.assertNotIn("(2 * r0 + r1) / 3", bc4)
        # The band, and the fact that the drink reuses the film's shader mode
        # rather than inventing a second blend path.
        self.assertIn("if (mean > 0.55f && mean < 0.90f)", self.src)
        self.assertIn("range.filmCoverage = true;", self.src)
        self.assertIn("range.liquidCoverage = true;", self.src)
        # Both exclusions: the pouch is already film, and a range with no map
        # samples must not be guessed at.
        self.assertIn("!range.filmCoverage &&", self.src)
        self.assertIn("!range.uvSamples.empty() && img.opacityFormat == 80", self.src)

    def test_solid_parts_of_a_drink_bag_are_not_clipped_by_its_coverage_map(self) -> None:
        # The CDB pairs the shared coverage map with every flavour material, so
        # the solid parts got it too. Under the printed leaf the map averages
        # 0.47, so the default clip() erased 53% of the artwork and the creature
        # came out broken and disconnected. Mode 9 is the arm that skips it.
        self.assertIn("bool noCoverageClip{ false };", self.src)
        self.assertIn("const bool noClipMode = g_alpha > 8.5 && g_alpha < 9.5;", self.shader)
        self.assertIn("else if (!crucibleMode && !vortexMode && !noClipMode && !maskInkMode)", self.shader)
        # The clip itself must not run in that mode.
        arm = self.shader.split("else if (!crucibleMode && !vortexMode && !noClipMode && !maskInkMode)", 1)[1]
        arm = arm[: arm.index("}")]
        self.assertIn("clip((promotedMode ? tex.r : min(tex.a, mask)) - 0.5);", arm)
        # Encoded only for the solid parts, and only after the film/liquid test.
        self.assertIn("range.noCoverageClip ? 9.0f :", self.render)
        gate = self.src.split("if (!range.filmCoverage) {", 1)[1]
        self.assertIn("range.noCoverageClip = true;", gate[:120])

    def test_every_shader_carries_the_inventory_brightness_anchor(self) -> None:
        # The cap is the unambiguous surface: 236 in the atlas, 186-194 on the
        # inventory screen, i.e. ~0.80 on a lit face, where this model reached
        # 1.01. Every pixel shader has to carry the scale, because HLSL has no
        # shared header here and a stray unscaled shader would render only the
        # ranges that use it a quarter too bright.
        #
        # The decal shader keeps its measured 0.42 floor verbatim. The two
        # opaque arms (kPsTexSrc textured, kPsSrc vertex-coloured) both carry
        # the 0.80 exposure over a lifted 0.62 floor, because a weapon preview
        # was measured at 0.72 of the inventory screen's luma and half its
        # chroma: abs(dot) light with a 0.42 floor crushes every face that
        # points away from both lights. Their lit faces sum to ~1.10 against
        # the original 1.12, so the cap anchor is unchanged.
        #
        # kPsSrc joined the lifted rig on 2026-10-23. It is not a decal shader:
        # it draws every 3D range with no albedo map (range.texSlot < 0 in
        # RenderGpuModel), which is the whole textureless CDB population -
        # including the snow globes' buildings, bronze ring and plastic base.
        # Holding it at 0.42 made those ~1.5x darker in shadow than a textured
        # range of the same albedo, which is what the 2026-10-23 captures show
        # against the engine's own render of the same item.
        # Per shader, not per file: a count alone let one literal stand in for
        # another (the blend shader joined the opaque one's metal-aware form on
        # 2026-10-20 and the arithmetic was unchanged, but the count could not
        # tell the two apart).
        import re as _re
        pattern = r'static constexpr const char (k\w+)\[\] = R"\((.*?)\)";'
        shaders = dict(_re.findall(pattern, self.src, _re.S))
        pixel = {n: b for n, b in shaders.items() if n != "kVsSrc"}
        self.assertEqual(len(pixel), 5, "expected five pixel shaders, found %s" % sorted(pixel))
        flat = "0.80 * (0.42 + 0.48 * kd + 0.16 * kf + 0.06 * kd * kd + spec)"
        lifted = "float lit = (1.0 - metal) * (0.62 + 0.34 * kd + 0.10 * kf + 0.04 * kd * kd) + spec;"
        # The vertex-coloured arm has no albedo slot, so it carries the same
        # lifted floor without the metal gate.
        untextured = "float lit = 0.62 + 0.34 * kd + 0.10 * kf + 0.04 * kd * kd + spec;"
        for name, body in pixel.items():
            with self.subTest(shader=name):
                # The additive shell has no lighting rig at all: it adds its
                # map's own colour, and the exposure anchor is the 0.80 on the
                # emission (see kPsAdditiveSrc). Holding it to a diffuse floor
                # would be testing a term it must not have.
                if name == "kPsAdditiveSrc":
                    self.assertIn("ink * i.color * 0.80", body)
                else:
                    self.assertTrue(flat in body or lifted in body or untextured in body,
                        "%s carries no inventory exposure anchor" % name)
                    self.assertIn("0.80 *", body)
        # The decal arm keeps the measured cap verbatim.
        self.assertIn(flat, pixel["kPsTexAlphaSrc"])
        # Every arm that shades a 3D surface: the two that mirror a room carry
        # the lifted floor plus the metal term, the vertex-coloured one the same
        # floor without it (it has no albedo slot to take metalness from).
        for name in ("kPsTexSrc", "kPsBlendTexSrc"):
            self.assertIn(lifted, pixel[name])
        self.assertIn("float lit = 0.62 + 0.34 * kd + 0.10 * kf + 0.04 * kd * kd + spec;", pixel["kPsSrc"])
        self.assertIn("return float4(0.80 * (i.color * lit), 1.0);", pixel["kPsSrc"])
        self.assertIn("return float4(0.80 * (albedo * i.color * lit + reflection), 1.0);", self.src)
        self.assertNotIn("float shade = 0.42 + 0.48 * kd", self.src)
        # No opaque arm may keep the pre-calibration weights. (kPsTexAlphaSrc
        # still does, above: those ranges are decals, which is the measurement
        # that produced the 0.42 floor in the first place.)
        self.assertNotIn("0.42 + 0.48 * kd + 0.16 * kf + 0.06 * kd * kd + spec", pixel["kPsSrc"])

    def test_drink_fill_hue_is_authored_and_scoped(self) -> None:
        # The atlas has no maroon to give: it samples olive (114,73,1) under the
        # drink's own footprint while the reference reads (77,38,41), so the
        # bias is explicit rather than sampled. It may only touch a range that
        # was classified as the drink.
        # Green trimmed from 0.55 after the first capture still read 5 counts
        # high against the reference; red and blue unchanged.
        self.assertIn("static constexpr float kDrinkFillBias[3] = { 1.25f, 0.48f, 1.0f };", self.src)
        liquid = self.src.split("range.liquidCoverage = true;", 1)[1]
        liquid = liquid[: liquid.index("REX::INFO")]
        self.assertIn("range.color[0] *= kDrinkFillBias[0];", liquid)
        self.assertIn("range.color[1] *= kDrinkFillBias[1];", liquid)
        self.assertIn("range.color[2] *= kDrinkFillBias[2];", liquid)        # Scoped to the liquid branch: no other rule may reach for the bias.
        self.assertEqual(self.src.count("kDrinkFillBias"), 4)  # declaration + 3 uses

    def test_drink_fill_hue_bias_is_gated_on_a_warm_atlas(self) -> None:
        # The coverage band is not enough to identify a maroon liquid. Measured
        # under the liquid mesh's own footprint (11.9% of each atlas): tea
        # (114,73,1) and tonic (228,173,155) are warm, but Boba Alien Tea is
        # (136,151,102) and Purified Water (117,118,118). All four sit in the
        # same 0.55-0.90 coverage band, so an ungated bias would pour red into
        # a bag of water. The gate reads the fill's own BC1 albedo over the
        # same UV samples; margins are +0.29 at the closest accepting flavour
        # and -0.003 at the closest rejecting one against a 0.10 threshold.
        self.assertIn("void SampleBc1Rgb(", self.src)
        bc1 = self.src.split("void SampleBc1Rgb(", 1)[1]
        bc1 = bc1[: bc1.index("bool UploadModelTextures")]
        # BC1 is the one with a 565 pair; reading it as BC3 would be wrong.
        self.assertIn("const auto c0 = static_cast<std::uint16_t>", bc1)
        self.assertIn("((c >> 11) & 31) / 31.0f", bc1)
        self.assertIn("((c >> 5) & 63) / 63.0f", bc1)
        # 3-colour mode when c0 <= c1: the fourth entry is black, not a blend.
        self.assertIn("if (c0 > c1) {", bc1)
        # The gate itself, and the bias nested inside it.
        self.assertIn(
            "warm = rgb[0] >= rgb[1] && rgb[1] >= rgb[2] && (rgb[0] - rgb[2]) >= 0.10f;", self.src
        )
        liquid = self.src.split("range.filmCoverage = true;", 1)[1]
        liquid = liquid[: liquid.index("REX::INFO")]
        self.assertIn("if (warm) {", liquid)
        gate = liquid.split("if (warm) {", 1)[1]
        gate = gate[: gate.index("}")]
        self.assertIn("range.color[0] *= kDrinkFillBias[0];", gate)
        # Blending is not gated: a neutral fill still has to show what is
        # behind it, only its hue stays as authored.
        self.assertLess(liquid.index("range.liquidCoverage = true;"), liquid.index("if (warm) {"))




if __name__ == "__main__":
    unittest.main()
