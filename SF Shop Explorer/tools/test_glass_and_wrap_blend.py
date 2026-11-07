"""Guards for the two render rules behind the 2026-10-20 "all four still wrong" pass.

Both defects were visible in the same four previews (Erdebrau Pils Can,
Sparkling Wine, Hypergiant Heart, Alien Genetic Material) and both were
render-rule defects, not material-resolution defects - the log for that session
shows the right atlases bound to the right ranges:

    range 1 'Materials\\Items\\LegendaryCatalyst\\LegendaryCatalyst.mat' <- slot 2
    range 0 'Materials\\Items\\FoodDrink_SingleItems\\Erdebrau_Beers\\Erdebrau_Pilsner_Decal.mat' <- slot 1

1. Glass was drawn with no reflection at all. kPsBlendTexSrc - the only
   shader the translucent pass uses - had a diffuse rig and nothing else, so
   an untextured glass body was its own tint times 30% alpha. Against the
   preview's dark backdrop that is invisible: the wine lost its whole bottle
   and the coolant jar read as a black tub with the specimen buried in it.
   The opaque shader has mirrored a room gradient since the weapon pass; the
   blend shader now does the same and takes its coverage from Fresnel.

   The 2026-10-21 captures showed the second half of that fix: all three
   items came back as white sleeves. The Fresnel dot was one-sided
   (saturate, unlike every other lighting term in the file), so the far wall
   of a closed shell - normals pointing away from the eye - had Fresnel 1
   over its whole surface and stamped an opaque room-coloured cup over the
   item. abs() plus a Schlick F0 of 0.04 makes the far wall behave like the
   near one, and the transparent stand-in alpha is now 0 (clear), not 0.30.

2. A decal material whose opacity map is a wrap was coverage-blended. The
   Erdebrau label IS the can body; its mask measures 0.881 mean coverage over
   the whole atlas, and drawing it as decal coverage (alpha = coverage,
   depth-write off, both walls) made the can see-through. A mask that is
   mostly solid is now drawn opaque with the ordinary cutout.

The mask measurement is exercised on a real archive map when the game data is
present, and skipped with a reason when it is not.
"""
from __future__ import annotations

import os
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OFFLINE = ROOT / "src" / "OfflineModel.cpp"
DATA = Path(os.environ.get("SFSE_STARFIELD_DATA", "B:/SteamLibrary/steamapps/common/Starfield/Data"))
PILSNER_OPACITY = "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau pilsner_decal_opacity.dds"


def shader_body(name: str, source: str) -> str:
    match = re.search(r'static constexpr const char ' + name + r'\[\] = R"\((.*?)\)";', source, re.S)
    if not match:
        raise AssertionError("%s not found in OfflineModel.cpp" % name)
    return match.group(1)


class BlendShaderReflectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")
        cls.blend = shader_body("kPsBlendTexSrc", cls.source)

    def test_blend_shader_reads_the_surface_material(self) -> None:
        # Without the metal/rough tail the sheen cannot tell frosted glass from
        # a polished one, and the constant buffer layout would not match the
        # 256-byte per-range block every range already writes.
        self.assertIn("g_metalRough", self.blend)
        self.assertIn("g_metalPad", self.blend)

    def test_blend_shader_mirrors_the_preview_studio(self) -> None:
        # The reflection term itself: the environment plus Fresnel. The
        # environment is the preview's own dark slate plus one key-light lobe,
        # not an invented bright room - the game's top-down jar render measures
        # its glass at 18-21/255 over a 11-26 backdrop with one 245 arc, and
        # the old 0.16..0.72 gradient mirrored pale grey instead.
        self.assertIn("float3 reflection", self.blend)
        self.assertIn("float fres", self.blend)
        self.assertIn("float3(0.055, 0.070, 0.095)", self.blend)
        self.assertIn("pow(saturate(dot(n, key) * 0.5 + 0.5), 24.0)", self.blend)
        self.assertNotIn("lerp(float3(0.16, 0.17, 0.20), float3(0.72, 0.77, 0.88)", self.blend)

    def test_fresnel_is_two_sided(self) -> None:
        # The far wall of a closed shell faces away from the eye. Saturating
        # that dot to zero gave it Fresnel 1 over its whole surface, i.e. an
        # opaque room-coloured cup inside every glass item - the white sleeve
        # in the 2026-10-21 captures. The same abs() the rest of the file uses
        # is what keeps the item visible through both walls.
        self.assertIn("const float ndv = abs(dot(n, viewDir));", self.blend)
        self.assertIn("const float fres = pow(1.0 - ndv, 5.0);", self.blend)
        self.assertNotIn("saturate(dot(n, viewDir))", self.blend)

    def test_reflected_and_transmitted_light_are_split(self) -> None:
        # A dielectric reflects F of what hits it and transmits (1 - F). The
        # coverage is that split (F reflected opaquely, the rest at the
        # authored opacity), and the body has to carry the transmission weight:
        # painting the body at the *coverage* instead is what turned the jar
        # into a pewter canister at grazing angles (2026-10-22 10:34 capture:
        # 108-176/255, where a = F predicts 153), and capping F's coverage at
        # 0.25 (0.8.314) left a grey shell over the specimen from every angle.
        self.assertIn("const float f0 = lerp(0.04, 1.0, metal);", self.blend)
        self.assertIn("const float F = f0 + (1.0 - f0) * fres;", self.blend)
        self.assertIn("float a = saturate(F + (1.0 - F) * g_alpha);", self.blend)
        self.assertIn("float3 body = albedo * i.color * lit * (1.0 - F) * (1.0 - g_clearShell);", self.blend)
        # The capped-sheen form this replaces.
        self.assertNotIn("saturate(g_alpha + 0.25 * F)", self.blend)
        # The reflection is added, never multiplied into the absorption: the
        # rim of a glass must stay as bright as the room it mirrors.
        self.assertIn("return float4(0.80 * (reflection + body), a);", self.blend)
        self.assertNotIn("g_alpha + fres * 0.85", self.blend)

    def test_blend_shader_keeps_the_inventory_anchor(self) -> None:
        # 0.80 is the measured inventory brightness anchor every shader in this
        # file scales by (see kShadeScale's comment); a reflection term added
        # outside it would re-expose the whole preview.
        self.assertIn("0.80 * (", self.blend)


class UntexturedGlassTintTests(unittest.TestCase):
    def test_glass_without_an_albedo_is_pale_not_black(self) -> None:
        source = OFFLINE.read_text(encoding="utf-8-sig")
        end = source.index("UNTEXTURED (no prefetch)")
        start = source.rindex("if (range.translucent) {", 0, end)
        block = source[start:end]
        for channel in ("range.color[0] = 0.62f;", "range.color[1] = 0.66f;", "range.color[2] = 0.72f;"):
            self.assertIn(channel, block)
        # The value this replaced. 0.10 was a mirror tint with no mirror term
        # behind it, i.e. a black glass body.
        self.assertNotIn("range.color[0] = 0.10f;", block)


class StandInGlassIsClearTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")
        cls.blend = shader_body("kPsBlendTexSrc", cls.source)

    def test_authored_sheer_glass_keeps_a_small_presence(self) -> None:
        # The coolant jar body (and every Legendary Catalyst that reuses it)
        # has an EMPTY database set, so this constant is the only thing that
        # says how much of the item it hides. 0.30 was a guess and composited
        # to 51% over two walls (the milky sleeve); 0.14 still painted a grey
        # shell over the specimen. The game's own top-down render of the jar
        # adds ~10/255 over the backdrop through two walls, i.e. ~5% per wall.
        self.assertIn("static constexpr float kAuthoredSheerAlpha = 0.05f;", self.source)
        self.assertNotIn("kAuthoredSheerAlpha = 0.30f", self.source)
        self.assertNotIn("kAuthoredSheerAlpha = 0.14f", self.source)

    def test_body_colour_is_not_multiplied_by_the_coverage(self) -> None:
        # Colour and coverage are separate: src = the surface's own colour,
        # alpha = how much of the target it covers. Scaling the body by
        # (1-F)*g_alpha squared the attenuation and made every shell black
        # instead of pale. The coverage is the authored opacity plus the
        # capped sheen -- never the whole of F.
        # The body carries the transmission weight (1 - F) and nothing else:
        # multiplying it by the coverage as well squared the attenuation.
        self.assertIn("float3 body = albedo * i.color * lit * (1.0 - F) * (1.0 - g_clearShell);", self.blend)
        self.assertIn("float a = saturate(F + (1.0 - F) * g_alpha);", self.blend)
        self.assertIn("return float4(0.80 * (reflection + body), a);", self.blend)

    def test_a_clear_shell_transmits_instead_of_painting_a_body(self) -> None:
        # A snow globe's dome is Snowglobe_Landmarks_Glass: no colour map and no
        # authored opacity, so it binds the white texture under the pale glass
        # tint. Painted as a body at Fresnel coverage that is a milky film over
        # the whole interior - the 2026-10-23 NY pair has the engine's dome
        # clear with one bright rim and the plugin's veiled grey. The database
        # naming nothing IS the statement that the shell is clear: pass the
        # interior through (body = 0).
        self.assertIn("float g_clearShell;", self.blend)
        self.assertIn("* (1.0 - g_clearShell)", self.blend)
        # ...and it reflects the same dark studio as every other glass surface.
        # The bright room an earlier revision invented for the clear shell put
        # a pale canopy back over the interior: the engine's own Opportunity
        # and NASA globe captures show the dome adding nothing at all over the
        # starfield behind it.
        self.assertIn("float3 env = float3(0.055, 0.070, 0.095) +", self.blend)
        self.assertNotIn("0.62, 0.66, 0.74", self.blend)
        # The flag is set where the pale tint is set, and written to the free
        # float of the per-range block the shader reads.
        end = self.source.index("UNTEXTURED (no prefetch)")
        start = self.source.rindex("if (range.translucent) {", 0, end)
        self.assertIn("range.clearShell = true;", self.source[start:end])
        # .y of the same pair is the interior-shell flag (see
        # test_globe_backdrop_shell); .x here is this shader's clear shell.
        self.assertIn("const float clearShell[2]{ range.clearShell ? 1.0f : 0.0f,", self.source)
        self.assertIn("std::memcpy(dst + 160, clearShell, sizeof(clearShell));", self.source)

    def test_name_token_glass_has_no_authored_opacity(self) -> None:
        # Same for the glass/clear/translucent/transpar fallback: that is the
        # path the Sparkling Wine bottle and both can sleeves take.
        body = self.source.split("static constexpr const char* kTokens[]", 1)[1]
        body = body[: body.index("a_translucent = false;")]
        self.assertIn("return 0.00f;", body)
        self.assertNotIn("0.30f", body)


class PromotedMaskAndEffectShellTests(unittest.TestCase):
    """The two defects the clear glass uncovered on 2026-10-21.

    With the shell no longer an opaque sleeve, the surfaces behind it became
    visible again, and two of them were wrong on their own:

    * Sparkling Wine's shell is the material's *mask*. Its texture set names an
      opacity map and no colour, so the upload promotes that map to the albedo
      - and an R8/BC4 SRV samples as (r,0,0), which reads as maroon-black, not
      as white. Mode 10 reads the same coverage as a white body.
    * Alien Genetic Material's WaterCaustic01_IdleAnimLoop and
      LegendaryCatalyst_Glow shells are additive effect quads modelled as full
      bodies. Drawn opaque they paint the item black and hide the specimen -
      the capture where the jar is a black bottle with white wisps inside.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")
        cls.shader = shader_body("kPsTexAlphaSrc", cls.source)

    def test_promoted_cutout_reads_its_mask_as_a_white_body(self) -> None:
        self.assertIn("const bool promotedMode = g_alpha > 9.5 && g_alpha < 10.5;", self.shader)
        arm = self.shader.split("if (promotedMode)", 1)[1]
        arm = arm[: arm.index("else if (coverageBlend)")]
        self.assertIn("albedo = tex.rrr;", arm)
        # It must stay before coverageBlend: a promoted mask has no colour, so
        # coverage blending it would draw its own coverage as colour.
        self.assertLess(self.shader.index("if (promotedMode)"),
                        self.shader.index("if (coverageBlend)"))

    def test_the_opaque_pass_carries_the_promoted_arm(self) -> None:
        # Promoted cutouts draw in pass 0, i.e. through kPsTexSrc - the pass
        # the wine's body actually uses. The arm had only ever been added to
        # the alpha pass, where routing sends no promoted range at all, so the
        # bottle kept drawing its BC4 mask as (r,0,0): maroon-black, with no
        # cutout, which is the 2026-10-22 capture.
        opaque = shader_body("kPsTexSrc", self.source)
        self.assertIn("const bool promotedMode = g_alpha > 9.5 && g_alpha < 10.5;", opaque)
        self.assertIn("albedo = tex.rrr;", opaque)
        self.assertIn("if (tex.r < 0.5)", opaque)
        self.assertIn("discard;", opaque)

    def test_a_mask_used_as_albedo_becomes_a_white_body(self) -> None:
        # Single-channel maps carry coverage, not colour. The Sparkling Wine's
        # set resolves a BC4 opacity map as its *colour*, which is neither
        # "flat" (so no promotion) nor a decal sheet ("_details"), so it fell
        # through both arms and painted (r,0,0): the dark bottle in the
        # 2026-10-22 10:00 capture. Whitening it draws the white silhouette
        # with the net's pixels cut out.
        self.assertIn("range.maskAlbedo = img.patternInk || IsSingleChannelFormat(img.format);",
                      self.source)
        # ...and it is shaded as a *picture* (mode 11), not cut out as coverage.
        # Clipping it was tried first and deleted the bottle: the map is a
        # greyscale sheet of the bottle with the net inked on it, so cutting on
        # it kept only the net.
        self.assertIn("const bool maskInkBody = range.maskAlbedo && !range.translucent;", self.source)
        self.assertIn("(maskInkBody ? 11.0f", self.source)
        opaque = shader_body("kPsTexSrc", self.source)
        alpha = shader_body("kPsTexAlphaSrc", self.source)
        for name, body in (("kPsTexSrc", opaque), ("kPsTexAlphaSrc", alpha)):
            self.assertIn("const bool maskInkMode = g_alpha > 10.5 && g_alpha < 11.5;", body, name)
        # The alpha pass must not clip in that mode.
        self.assertIn("else if (!crucibleMode && !vortexMode && !noClipMode && !maskInkMode)", alpha)

    def test_a_promoted_cutout_clips_on_its_own_map(self) -> None:
        # The companion opacity map is a different layer: the wine's body
        # silhouette paired with the net's mask clipped the whole bottle away.
        self.assertIn("clip((promotedMode ? tex.r : min(tex.a, mask)) - 0.5);", self.source)

    def test_a_mask_only_decal_sheet_is_skipped(self) -> None:
        # A print needs ink: a decal whose colour map is single-channel has no
        # ink at all, so drawing it can only paint a mask as colour. A promoted
        # mask normally still draws (the foil wraps), unless that mask belongs
        # to another part - see BorrowedMaskDecalTests. The borrowed arm is
        # deliberately not gated on the decal's own channel count: a repacked
        # mask can be a multi-channel BC7 sheet.
        self.assertIn("if (IsDecalSheet(range.matPath) &&", self.source)
        self.assertIn(
            "(borrowedMask || (IsSingleChannelFormat(img.format) && !img.promotedMask))",
            self.source)

    def test_promoted_cutout_body_is_whitened_not_palette_grey(self) -> None:
        # Mode 10 reads the mask's coverage as a white body, but the range's
        # invented palette grey (0.55 for the wine's wrap, 0.70 for its decal)
        # was still multiplying it: 0.90 coverage * 0.55 * shade landed at 0.28
        # of white, i.e. a near-black bottle under a clear shell. The tint has
        # to be white for a surface whose set carries no colour at all.
        self.assertIn("if (img.promotedMask) {", self.source)
        block = self.source.split("if (img.promotedMask) {", 1)[1]
        block = block[: block.index("}")]
        self.assertIn("range.color[0] = range.color[1] = range.color[2] = 1.0f;", block)
        # And after the mask/override block that would otherwise reset it.
        self.assertGreater(self.source.index("if (img.promotedMask) {"),
                           self.source.index("if (!img.maskCutout && !range.cdbOverrideTint)"))

    def test_promoted_cutout_is_routed_and_not_coverage_blended(self) -> None:
        self.assertIn("bool promotedCutout{ false };", self.source)
        self.assertIn("range.promotedCutout = img.promotedMask;", self.source)
        self.assertIn("t.promotedMask = true;", self.source)
        self.assertIn("(range.promotedCutout ? 10.0f :", self.source)
        decal = self.source.index("const bool decalCoverage =")
        line_end = self.source.index(";", self.source.index("range.promotedCutout", decal))
        self.assertIn("!range.promotedCutout", self.source[decal:line_end])

    def test_effect_shells_are_not_drawn_as_surfaces(self) -> None:
        self.assertIn("bool IsEffectOverlayMaterial(const std::string& a_matPath)", self.source)
        helper = self.source.split("bool IsEffectOverlayMaterial(const std::string& a_matPath)", 1)[1]
        helper = helper[: helper.index("bool IsDecalSheet(")]
        for token in ("caustic", "gobo", "idleanimloop", "animloop"):
            self.assertIn('"%s"' % token, helper)
        self.assertIn("items/legendarycatalyst/legendarycatalyst_glow", helper)
        self.assertIn("range.effectOverlay = IsEffectOverlayMaterial(mesh.matPath);", self.source)
        self.assertIn("range.additiveShell = IsAdditiveShellMaterial(mesh.matPath);", self.source)
        # A shell that is not the additive caustic loop is still never drawn as
        # a surface (the glow shell); the loop itself is drawn only by pass 2,
        # pinned in CoolantJarColourAndFluidTests.
        skip = self.source.index("if (range.effectOverlay && !range.additiveShell) {")
        self.assertIn("continue;", self.source[skip:skip + 80])
        self.assertNotIn("if (range.effectOverlay) {", self.source)

    def test_effect_tokens_agree_with_the_offline_contact_sheet(self) -> None:
        # The contact sheet has skipped these since it was written
        # (EFFECT_TOKENS); the renderer is the one that had to catch up, so the
        # two lists must keep agreeing on the two names that carry the rule.
        sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
        import contact_sheet
        for token in ("caustic", "gobo"):
            self.assertIn(token, contact_sheet.EFFECT_TOKENS)


class WrapMaskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")

    def test_mostly_solid_mask_is_not_coverage_blended(self) -> None:
        self.assertIn("bool maskMostlySolid{ false };", self.source)
        decal = self.source.index("const bool decalCoverage =")
        line_end = self.source.index(";", self.source.index("range.maskMostlySolid", decal))
        self.assertIn("!range.maskMostlySolid", self.source[decal:line_end])

    def test_mask_measurement_decodes_bc4_texels(self) -> None:
        # The endpoints alone are not coverage; the six interpolants carry most
        # of a mask's pixels, so the mean has to walk all sixteen codes.
        self.assertIn("(8 - code) * c0 + (code - 1) * c1) / 7.0f", self.source)
        self.assertIn("range.maskMostlySolid = mean >= 0.70;", self.source)

    def test_pilsner_label_wrap_measures_as_solid(self) -> None:
        if not DATA.is_dir():
            self.skipTest("Starfield Data dir not present at %s" % DATA)
        sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))
        try:
            import dump_tex  # noqa: F401  (import proves the decoder is available)
            from ba2lib import Ba2Archive
        except Exception as exc:  # pragma: no cover - environment dependent
            self.skipTest("archive helpers unavailable: %s" % exc)
        archive = DATA / "Starfield - Textures05.ba2"
        if not archive.is_file():
            self.skipTest("Textures05.ba2 not found under %s" % DATA)
        ar = Ba2Archive(archive)
        self.assertTrue(ar.valid, "archive did not parse")
        key = PILSNER_OPACITY.lower()
        self.assertIn(key, ar.tex_files, "pilsner opacity map missing from the archive")
        raw, err = ar.read_texture(key)
        self.assertIsNotNone(raw, "read_texture failed: %s" % err)
        tf = ar.tex_files[key]
        need = ((tf.width + 3) // 4) * ((tf.height + 3) // 4) * 8
        payload = raw[:need]
        total, blocks = 0.0, 0
        for b in range(0, len(payload) - 7, 8):
            c0 = payload[b] / 255.0
            c1 = payload[b + 1] / 255.0
            bits = int.from_bytes(payload[b + 2:b + 8], "little")
            for t in range(16):
                code = (bits >> (3 * t)) & 7
                total += c0 if code == 0 else (c1 if code == 1 else ((8 - code) * c0 + (code - 1) * c1) / 7.0)
            blocks += 1
        mean = total / (blocks * 16)
        self.assertGreater(mean, 0.70, "the can label wrap should read as mostly solid, got %.3f" % mean)


class BorrowedMaskDecalTests(unittest.TestCase):
    """A label whose only map is another part's cutout is not this part's art.

    The Alien Genetic Material jar's label sleeve
    (`Isotopic_Coolant_Decals.mat`) has a database set naming exactly one
    texture: the LID's cutout, `isotopic_coolant_lid_opacity.dds`, which the lid
    range also binds as its mask. Drawn as ink it stamps the lid's star onto the
    jar's body - the black star of the 2026-10-22 12:57 capture - where the
    game's render shows no print at all.

    The exemption the skip used to make for promoted masks is what let it
    through, and it must stay for the foil wraps that legitimately reach that
    route (the Sparkling Wine's cap). So the test is narrow: the other
    material's copy of the same mask must itself carry colour artwork.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")

    def test_the_bound_mask_path_travels_with_the_prefetch(self) -> None:
        # Without the path there is nothing to compare across the model.
        self.assertIn("std::string opacityMap;", self.source)
        self.assertIn("t.opacityMap = ddsKey;", self.source)
        self.assertIn("t.opacityMap = opacityKey;", self.source)

    def test_a_borrowed_mask_skips_the_decal(self) -> None:
        body = self.source.split("bool borrowedMask = false;", 1)[1]
        body = body[: body.index("// A grunge atlas is an overlay")]
        # Another MATERIAL, the same mask, and that copy carries colour art.
        self.assertIn("other.matPath == range.matPath || other.opacityMap != img.opacityMap", body)
        self.assertIn("if (!IsSingleChannelFormat(other.format)) {", body)
        self.assertIn("(borrowedMask || (IsSingleChannelFormat(img.format) && !img.promotedMask))",
                      body)
        # A borrowed mask is skipped whatever the decal's own map is stored as.
        self.assertIn("if (IsDecalSheet(range.matPath) &&", body)
        # A promoted mask that is its own sheet still draws.
        self.assertIn("REX::INFO(\"SFShopExplorer offline GPU: mask-only decal", body)

    def test_the_shared_sheet_between_two_prints_is_not_borrowed(self) -> None:
        # Both Sparkling Wine prints resolve to the same mask; neither may see
        # the other as a lender, or the bottle loses its body.
        body = self.source.split("bool borrowedMask = false;", 1)[1]
        body = body[: body.index("// A grunge atlas is an overlay")]
        self.assertNotIn("other.format == img.format", body)
        self.assertIn("IsSingleChannelFormat(other.format)", body)


class CoolantJarColourAndFluidTests(unittest.TestCase):
    """The jar's green and its bubbles (2026-10-22 13:10 capture).

    The preview matched the game for shape, specimen and lid, and had two
    differences left: the glass was blue-grey where the game's is green, and
    the jar had no fluid at all where the game's is full of bubbles.

    Neither is a resolution miss. `Isotopic_Coolant_Container`'s database set
    is empty (two `cdb_records.py --name` records: an id and the name), so no
    colour exists to find; the green has to be authored, exactly like the
    alpha. And the bubbles are `WaterCaustic01_IdleAnimLoop`, the additive
    effect shell the renderer had been skipping since the contact sheet needed
    it out of the way of the specimen - deliberate then, wrong now that the
    glass is see-through and the game is the reference.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")

    def test_the_authored_tint_is_the_measured_one(self) -> None:
        self.assertIn(
            "static constexpr float kAuthoredGlassTint[3] = { 0.34f, 0.37f, 0.25f };",
            self.source)
        # The pale tint this replaces is still the generic one for every other
        # untextured glass (the wine, the snow globe): only the authored
        # material moves.
        self.assertIn("range.color[0] = 0.62f;", self.source)

    def test_the_tint_applies_to_the_glass_and_its_fluid(self) -> None:
        # Glass: the no-prefetch branch, keyed by the same material list as the
        # authored alpha.
        glass = self.source.split("if (range.translucent) {", 1)[1]
        glass = glass[: glass.index("UNTEXTURED (no prefetch)")]
        self.assertIn("if (IsAuthoredSheerMat(range.matPath)) {", glass)
        self.assertIn("range.color[c] = kAuthoredGlassTint[c];", glass)
        # Fluid: the additive shell wears the jar's colour, scoped to a model
        # that actually wears the authored glass.
        self.assertIn("bool authoredGlass = false;", self.source)
        self.assertIn("if (range.additiveShell && authoredGlass) {", self.source)
        self.assertIn("r.translucent && IsAuthoredSheerMat(r.matPath)", self.source)

    def test_the_fluid_shell_is_drawn_additively(self) -> None:
        self.assertIn("::ID3D12PipelineState* psoAdditive{ nullptr };", self.source)
        self.assertIn("REL(g_pipe.psoAdditive);", self.source)
        pso = self.source.split("// Additive pass: the effect shells.", 1)[1]
        pso = pso[: pso.index("// Restore the opaque defaults")]
        self.assertIn("D3D12_BLEND_ONE", pso)
        self.assertIn("D3D12_DEPTH_WRITE_MASK_ZERO", pso)
        self.assertIn("psoAdditive", pso)
        # The blend is ONE/ONE: source added to destination, not SRC_ALPHA.
        self.assertNotIn("D3D12_BLEND_SRC_ALPHA", pso)

    def test_the_shell_shader_adds_its_map_and_nothing_else(self) -> None:
        shader = shader_body("kPsAdditiveSrc", self.source)
        self.assertIn("float3 ink = g_albedo.Sample(g_samp, i.uv).rgb;", shader)
        self.assertIn("return float4(ink * i.color * 0.80, 1.0);", shader)
        self.assertIn("CompileShader(kPsAdditiveSrc", self.source)

    def test_only_the_caustic_loop_is_additive(self) -> None:
        # The glow shell stays skipped: it is an emissive companion of the
        # artwork and binding that artwork's full colour additively would
        # wash the specimen out.
        body = self.source.split("bool IsAdditiveShellMaterial(", 1)[1]
        body = body[: body.index("bool IsDecalSheet(")]
        for token in ("caustic", "gobo", "idleanimloop", "animloop"):
            self.assertIn(f'"{token}"', body, token)
        self.assertNotIn("glow", body)
        self.assertIn("if (range.effectOverlay && !range.additiveShell) {", self.source)
        # Three passes, the shell last.
        self.assertIn("for (std::uint32_t pass = 0; pass < 3; ++pass)", self.source)
        self.assertIn("if (pass != 2) {", self.source)
        self.assertIn("a_list->SetPipelineState(g_pipe.psoAdditive);", self.source)

    def test_a_shell_without_a_map_is_not_drawn(self) -> None:
        # No map means nothing to add; falling through to the opaque pass would
        # paint the white shell that caused the original blackout.
        self.assertIn("if (range.additiveShell && range.texSlot < 0) {", self.source)


class SparklingWinePatternTests(unittest.TestCase):
    """The bottle's web is a Blender-layer mask, not a transparency effect.

    materialsbeta.cdb object `GenericSpaceWine_Glass_Blender1` names exactly one
    texture (`DetailAbstractPattern06_mask.DDS`), and that map is white cells
    under a black web - the bottle in the 2026-10-22 inventory capture. Its
    texture set is empty, so the audit row "no colour map, only an opacity
    mask" was reading the DECAL's crest label, not the body.

    2026-10-23: the map was bound but sampled through the shell's raw UV
    island, which is a quarter of the sheet in each axis, so the print read
    zoomed in. The island is normalised onto [0,1] at build time.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")

    def test_the_pattern_is_the_blender_layer_map(self) -> None:
        self.assertIn("struct AuthoredPattern", self.source)
        table = self.source.split("static constexpr AuthoredPattern kAuthoredPattern[] = {", 1)[1]
        table = table[: table.index("};")]
        self.assertIn('"genericspacewinebottle/genericspacewine_glass"', table)
        self.assertIn(
            '"textures/common/detail/manmade/detailabstractpattern06_mask.dds"', table)

    def test_the_pattern_is_bound_instead_of_a_colour_map(self) -> None:
        self.assertIn("} else if (FindAuthoredPatternMap(mesh.matPath, albedo)) {", self.source)
        arm = self.source.split("} else if (FindAuthoredPatternMap(mesh.matPath, albedo)) {", 1)[1]
        arm = arm[: arm.index("} else if (LiveModel::FindMatAlbedo")]
        self.assertIn("patternInk = true;", arm)
        # A single-channel map is otherwise rejected as an albedo.
        self.assertIn("if (!promotedMask && !patternInk && IsSingleChannelFormat(img.format))",
                      self.source)

    def test_the_pattern_body_is_opaque_and_white(self) -> None:
        # The game's bottle is an opaque white body with the web printed on it,
        # not a clear shell: this range must leave the blend pass.
        arm = self.source.split("if (FindAuthoredPatternMap(mesh.matPath, patternMap)) {", 1)[1]
        arm = arm[: arm.index("}")]
        self.assertIn("range.translucent = false;", arm)
        self.assertIn("range.alpha = 1.0f;", arm)
        self.assertIn("range.color[0] = 1.0f;", arm)

    def test_the_pattern_is_mapped_at_the_sheets_own_density(self) -> None:
        # The mask is a tiling sheet, not an atlas. Drawn through the shell's
        # raw island - a quarter of the map in each axis - the web comes out
        # four times too large: the 2026-10-23 capture shows two or three cells
        # across the body where the game has five or six, and strands as thick
        # as the cells. Mapping the island onto [0,1] restores the sheet's own
        # texel density, and the box is square while the bottle's body height
        # and circumference are within a few percent of each other, so no axis
        # is stretched by that mapping.
        self.assertIn("patternMesh = FindAuthoredPatternMap(mesh.matPath, patternProbe) && hasUv;",
                      self.source)
        self.assertIn("patternUvScale[0] = 1.0f / (uhi - ulo);", self.source)
        self.assertIn("patternUvScale[1] = 1.0f / (vhi - vlo);", self.source)
        self.assertIn("(mesh.uv[v * 2] - patternUvLo[0]) * patternUvScale[0]", self.source)
        self.assertIn("(mesh.uv[v * 2 + 1] - patternUvLo[1]) * patternUvScale[1]", self.source)
        # Scoped to the authored pattern: every other range keeps the UVs the
        # mesh shipped, or the whole catalogue would be re-textured.
        self.assertIn(": mesh.uv[v * 2]) : 0.0f);", self.source)
        self.assertIn(": mesh.uv[v * 2 + 1]) : 0.0f);", self.source)

    def test_the_shell_island_is_a_quarter_of_the_mask(self) -> None:
        """The four-times factor, measured on the shipped mesh rather than assumed."""
        if not DATA.is_dir():
            self.skipTest("Starfield Data dir not present at %s" % DATA)
        sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))
        sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
        try:
            import contact_sheet  # noqa: F401  (its parse_mesh is the C++ replica)
            from ba2lib import Ba2Archive
        except Exception as exc:  # pragma: no cover - environment dependent
            self.skipTest("archive helpers unavailable: %s" % exc)
        archive = DATA / "Starfield - Meshes02.ba2"
        if not archive.is_file():
            self.skipTest("Meshes02.ba2 not found under %s" % DATA)
        ar = Ba2Archive(archive)
        self.assertTrue(ar.valid, "archive did not parse")
        # GenericSpaceWine_mesh:0, the glass shell the pattern is bound to.
        key = "geometries/f7731245d42a81a8e0d8/8e6295cf0c6852bb972e.mesh"
        blob, err = ar.read_entry(key)
        self.assertIsNotNone(blob, "shell mesh unreadable: %s" % err)
        mesh = contact_sheet.parse_mesh(blob)
        self.assertIsNotNone(mesh, "shell mesh did not parse")
        uv = mesh["uv"]
        span_u = float(uv[:, 0].max() - uv[:, 0].min())
        span_v = float(uv[:, 1].max() - uv[:, 1].min())
        self.assertAlmostEqual(span_u, 0.252, delta=0.01)
        self.assertAlmostEqual(span_v, 0.248, delta=0.01)
        # Two axes of ~0.25: the normalisation is a ~4x densification.
        self.assertGreater(1.0 / span_u, 3.5)
        self.assertGreater(1.0 / span_v, 3.5)

    def test_the_pattern_is_shaded_as_ink(self) -> None:
        self.assertIn("range.maskAlbedo = img.patternInk || IsSingleChannelFormat(img.format);",
                      self.source)
        # Mode 11 needs a non-translucent range, which the build guarantees.
        self.assertIn("const bool maskInkBody = range.maskAlbedo && !range.translucent;",
                      self.source)
        # And a neutral tint, or the white cells come out palette grey.
        ink = self.source.split("if (img.patternInk) {", 1)[1]
        ink = ink[: ink.index("}")]
        self.assertIn("range.color[0] = range.color[1] = range.color[2] = 1.0f;", ink)


class SheerNameWordTests(unittest.TestCase):
    """The sheer scan matched substrings, so the whole hourglass was glass.

    `Hourglass_Case` and `Hourglass_Sand` both carry the "glass" inside
    "hourglass". The 2026-10-23 Misc audit found the hourglass preview drawing
    as an empty outline because every one of its three materials came back
    translucent at alpha 0; the case and the sand have no map of their own, so
    nothing of the item was left on screen.

    The rule is now a word start (path start, separator, or lower->upper camel
    hump), read before lowercasing, which keeps `Hourglass_Glass`,
    `GlassHelmets01` and `ScienceGlassLiquid` clear while `PlasticFiberglass01`
    and the hourglass' own parts stay solid.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")
        sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
        import aid_colour_audit
        cls.audit = aid_colour_audit

    def test_the_hourglass_case_and_sand_are_solid(self) -> None:
        for name in ("Hourglass_Case", "Hourglass_Sand"):
            alpha, translucent, why = self.audit.blend_alpha(
                "Materials\\SetDressing\\Exotic_Clutter\\Exotic_Hourglass\\" + name + ".mat",
                None, None)
            self.assertFalse(translucent, name)
            self.assertEqual(alpha, 1.0, name)
            self.assertIn("no database alpha", why, name)

    def test_the_hourglass_glass_is_still_clear(self) -> None:
        alpha, translucent, _ = self.audit.blend_alpha(
            "Materials\\SetDressing\\Exotic_Clutter\\Exotic_Hourglass\\Hourglass_Glass.mat",
            None, None)
        self.assertTrue(translucent)
        self.assertEqual(alpha, self.audit.TOKEN_FALLBACK_ALPHA)

    def test_a_fiberglass_body_is_not_a_window(self) -> None:
        _, translucent, _ = self.audit.blend_alpha(
            "Materials\\Common\\Plastic\\PlasticFiberglass01.mat", None, None)
        self.assertFalse(translucent)

    def test_a_camel_case_glass_still_matches(self) -> None:
        for name in ("ScienceGlassLiquid01", "GlassHelmets01"):
            _, translucent, _ = self.audit.blend_alpha(
                "Materials\\Items\\Science\\" + name + ".mat", None, None)
            self.assertTrue(translucent, name)

    def test_the_inhaler_keeps_its_authored_alpha(self) -> None:
        alpha, translucent, _ = self.audit.blend_alpha(
            "Materials\\SetDressing\\Manufactured_Goods\\GlassHelmets01.mat", 0.686, 0)
        self.assertTrue(translucent)
        self.assertAlmostEqual(alpha, 0.686, places=3)

    def test_the_renderer_uses_the_same_word_rule(self) -> None:
        self.assertIn("bool MaterialNameHasWord(const std::string& a_path, const char* a_word)",
                      self.source)
        self.assertIn("std::string MaterialOwnName(const std::string& a_path)", self.source)
        # The scan reads the material's OWN name, not the path: the folder is a
        # namespace, and `ScienceGlassPlastic02` is a plastic cap.
        self.assertIn("const std::string ownName = MaterialOwnName(a_matPath);", self.source)
        self.assertIn("if (MaterialNameHasWord(ownName, *t)) {", self.source)
        self.assertNotIn("if (MaterialNameHasWord(a_matPath, *t)) {", self.source)
        # The old substring test is what let "hourglass" through. The guard is
        # scoped to the sheer token loop: the effect-overlay and additive-shell
        # scans keep their own substring tests deliberately (their token
        # `animloop` only ever appears inside `idleanimloop` and
        # `WaterCaustic01`), so a file-wide search would fail on correct code.
        sheer = self.source.split("static constexpr const char* kTokens[]", 1)
        self.assertEqual(len(sheer), 2, "the sheer token table is gone")
        body = sheer[1].split("a_translucent = false;", 1)[0]
        self.assertIn("MaterialNameHasWord(ownName, *t)", body)
        self.assertNotIn("ml.find(", body)


class MaterialOwnNameTests(unittest.TestCase):
    r"""The shelf name repeats into its files; the shelf is not the material.

    `Materials\SetDressing\ScienceGlass\` holds the plastic caps and the vial
    liquid beside the glass fills, and every file repeats the shelf name. The
    sheer word scan read the whole path, so the "Glass" of `ScienceGlass` was a
    word start (camel hump) and `ScienceGlassPlastic02` / `ScienceGlassLiquid01`
    were declared clear shells at alpha 0 - the plastic vanished and the liquid
    drew as a film over nothing. The material's own name is what follows the
    shelf: `Plastic02`, `Liquid01`, `GlassCleanTransparent01_Base_Green03`.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")
        sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
        import aid_colour_audit
        cls.audit = aid_colour_audit

    def test_the_plastic_and_liquid_are_solid(self) -> None:
        for name, own in (("ScienceGlassPlastic01", "Plastic01"),
                          ("ScienceGlassPlastic01_Base", "Plastic01_Base"),
                          ("ScienceGlassPlastic02", "Plastic02"),
                          ("ScienceGlassLiquid01", "Liquid01"),
                          ("ScienceGlassLiquid02", "Liquid02")):
            mat = "Materials\\SetDressing\\ScienceGlass\\" + name + ".mat"
            alpha, translucent, why = self.audit.blend_alpha(mat, None, None)
            self.assertFalse(translucent, name)
            self.assertEqual(alpha, 1.0, name)
            self.assertEqual(self.audit.own_name(mat), own, name)

    def test_the_shelf_still_lets_its_glass_fills_through(self) -> None:
        fill = "ScienceGlass_GlassCleanTransparent01_Base_Green"
        mat = "Materials\\SetDressing\\ScienceGlass\\" + fill + ".mat"
        alpha, translucent, why = self.audit.blend_alpha(mat, None, None)
        self.assertTrue(translucent)
        self.assertAlmostEqual(alpha, self.audit.AUTHORED_SHEER_ALPHA, places=3)
        # The fill's own name keeps the glass word after the shelf prefix.
        self.assertEqual(self.audit.own_name(mat),
                         "GlassCleanTransparent01_Base_Green")

    def test_a_stem_that_repeats_a_normal_folder_is_left_whole(self) -> None:
        # A folder only becomes a namespace when it has been checked to hold
        # materials that are not what it is called; PowerIndicator holds glass
        # and must keep the repeat.
        mat = ("Materials\\SetDressing\\PowerIndicator\\"
               "PowerIndicator_Glass02_Frost.mat")
        self.assertEqual(self.audit.own_name(mat), "PowerIndicator_Glass02_Frost")
        _, translucent, _ = self.audit.blend_alpha(mat, None, None)
        self.assertTrue(translucent)

    def test_a_dirty_frosted_pane_stays_glass(self) -> None:
        # The measured cost of a blanket folder strip: `Common\Glass` repeats
        # its folder name into every file, and `GlassDirtyHeavyFrost01` is worn
        # by the Vortex Ichor. Stripping "Glass" there made a real frosted pane
        # opaque, which is why the strip is gated on NAMESPACE_FOLDERS.
        mat = "Materials\\Common\\Glass\\GlassDirtyHeavyFrost01.mat"
        self.assertEqual(self.audit.own_name(mat), "GlassDirtyHeavyFrost01")
        alpha, translucent, why = self.audit.blend_alpha(mat, None, None)
        self.assertTrue(translucent)
        self.assertEqual(alpha, self.audit.TOKEN_FALLBACK_ALPHA)

    def test_a_stem_named_exactly_after_its_folder_is_kept_whole(self) -> None:
        self.assertEqual(
            self.audit.own_name("Materials\\Common\\Glass\\Glass.mat"), "Glass")

    def test_the_strip_keeps_the_stems_own_case(self) -> None:
        # MaterialNameHasWord and its mirror read camel humps from the case, so
        # a lowercased stem would lose `GlassHelmets01` inside a longer name.
        self.assertEqual(
            self.audit.own_name("Materials\\SetDressing\\ScienceGlass\\"
                                "ScienceGlassGlassHelmets01.mat"),
            "GlassHelmets01")

    def test_a_bare_stem_is_its_own_name(self) -> None:
        self.assertEqual(self.audit.own_name("ScienceGlassLiquid01"),
                         "ScienceGlassLiquid01")

    def test_the_mirror_matches_the_renderer(self) -> None:
        body = self.source.split("std::string MaterialOwnName(", 1)[1]
        body = body[: body.index("\n\t}")]
        self.assertIn(".mat", body)
        self.assertIn("rfind('/', slash - 1)", body)
        self.assertIn("LowerAscii(stem.substr(0, folder.size())) != folder", body)
        # Only a leading repeat is dropped, only when something follows it, and
        # only for a folder on the measured namespace list.
        self.assertIn("stem.size() <= folder.size()", body)
        self.assertIn("kNamespaceFolders", body)
        for folder in self.audit.NAMESPACE_FOLDERS:
            self.assertIn(f'"{folder}"', self.source, folder)


class AuthoredLiquidTintTests(unittest.TestCase):
    r"""The vial liquid takes the fill family's measured green.

    `ScienceGlassLiquid01/02` stores no colour anywhere - no `0x025a` override,
    no `0x022b` layer kind, an empty set, and its texture set points at the
    kitchen Scissors prop (HANDOFF §0d). Drawn opaque it fell back to the
    neutral grey "no data" colour and the bottle read as empty. The fill family
    behind it, `ScienceGlass_GlassCleanTransparent01_Base_Green03`, stores
    (0, 0.6524, 0.5964), so that is the authored colour of the liquid body.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")
        sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
        import aid_colour_audit
        cls.audit = aid_colour_audit
        cls.cdb = aid_colour_audit.Cdb(aid_colour_audit.CDB)

    def rgb(self, mat: str):
        return self.audit.color_decision(mat, self.cdb, None, None)

    def test_the_liquid_takes_the_fill_family_green(self) -> None:
        for name in ("ScienceGlassLiquid01", "ScienceGlassLiquid02"):
            mat = "Materials\\SetDressing\\ScienceGlass\\" + name + ".mat"
            rgb, why = self.rgb(mat)
            self.assertAlmostEqual(rgb[0], 0.0, places=3, msg=name)
            self.assertAlmostEqual(rgb[1], 0.6524, places=3, msg=name)
            self.assertAlmostEqual(rgb[2], 0.5964, places=3, msg=name)
            self.assertIn("authored liquid tint", why, name)

    def test_the_tint_is_the_colour_the_fill_family_stores(self) -> None:
        fill, _ = self.rgb("Materials\\SetDressing\\ScienceGlass\\"
                           "ScienceGlass_GlassCleanTransparent01_Base_Green03.mat")
        liquid, _ = self.rgb("Materials\\SetDressing\\ScienceGlass\\"
                             "ScienceGlassLiquid02.mat")
        for channel in range(3):
            self.assertAlmostEqual(liquid[channel], fill[channel], places=4)

    def test_the_plastics_keep_their_own_colours(self) -> None:
        # Plastic02 carries an authored override (0.30, 0.58, 0.82); Plastic01
        # has no record at all and stays neutral grey. Neither may pick up the
        # liquid's green through a loose substring match.
        blue, _ = self.rgb("Materials\\SetDressing\\ScienceGlass\\"
                           "ScienceGlassPlastic02.mat")
        self.assertGreater(blue[2], blue[1])          # blue, not green
        grey, _ = self.rgb("Materials\\SetDressing\\ScienceGlass\\"
                           "ScienceGlassPlastic01.mat")
        self.assertEqual(grey, (0.70, 0.70, 0.72))

    def test_other_glass_keeps_its_pale_tint(self) -> None:
        rgb, why = self.rgb("Materials\\Common\\Glass\\GlassCleanTransparent01.mat")
        self.assertEqual(rgb, self.audit.FLAT_GLASS_TINT)
        self.assertIn("clear glass family", why)

    def test_the_mirror_matches_the_renderer(self) -> None:
        body = self.source.split("static constexpr AuthoredFlat kAuthoredFlat[]", 1)[1]
        body = body[: body.index("nullptr, {}")]
        for key in self.audit.AUTHORED_FLAT_TINTS:
            self.assertIn(f'"{key}"', body, key)
        for value in ("0.00f", "0.6524f", "0.5964f"):
            self.assertIn(value, body, value)
        # It sits in FlatBaseColor, so both the GPU build and the CPU
        # rasteriser take it, and the upload's white-reset still applies to
        # any range that does get an albedo.
        start = self.source.index("bool FlatBaseColor(")
        self.assertIn("kAuthoredFlat", self.source[start:start + 4000])


class FlatClearGlassTests(unittest.TestCase):
    r"""The clear-glass family the database leaves empty (2026-10-24).

    `Materials\Common\Glass\GlassCleanTransparent01.mat` and the ScienceGlass
    `..._Base_Green` fills are authored EMPTY sets: no colour, no opacity, no
    map. The sheer token scan then declared them clear shells at alpha 0, which
    is right for a printed can's sleeve and wrong for a beaker - a beaker IS
    the glass - so the preview drew an empty tile. Measured on the contact
    sheet: 00265ADA (Beaker) and 00265ADE (Graduated Cylinder) were blank
    before this rule and both draw a faint vessel after it.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")
        sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
        import aid_colour_audit
        cls.audit = aid_colour_audit

    def test_the_rule_needs_the_folder_and_the_stem(self) -> None:
        body = self.source.split("bool IsFlatClearGlassMat(", 1)[1]
        body = body[: body.index("\n\t}")]
        for token in ("/common/glass/", "/setdressing/scienceglass/",
                      "glasscleantransparent"):
            self.assertIn(token, body, token)
        # "Glass" alone is not enough: it is also inside ScienceGlassPlastic02,
        # which is an opaque body and must not become a window.
        self.assertIn("knownFolder && ml.find(\"glasscleantransparent\")", body)

    def test_the_family_keeps_the_measured_transmission(self) -> None:
        for name in ("GlassCleanTransparent01", "GlassCleanTransparent01_double"):
            mat = "Materials\\Common\\Glass\\" + name + ".mat"
            alpha, translucent, why = self.audit.blend_alpha(mat, None, None)
            self.assertTrue(translucent, name)
            self.assertAlmostEqual(alpha, self.audit.AUTHORED_SHEER_ALPHA, places=3)
            self.assertIn("clear glass family", why, name)
        fill = ("Materials\\SetDressing\\ScienceGlass\\"
                "ScienceGlass_GlassCleanTransparent01_Base_Green03.mat")
        alpha, translucent, why = self.audit.blend_alpha(fill, None, None)
        self.assertTrue(translucent)
        self.assertAlmostEqual(alpha, self.audit.AUTHORED_SHEER_ALPHA, places=3)

    def test_the_plastic_and_liquid_in_the_same_folder_are_not_the_family(self) -> None:
        for name in ("ScienceGlassPlastic02", "ScienceGlassLiquid01"):
            mat = "Materials\\SetDressing\\ScienceGlass\\" + name + ".mat"
            self.assertFalse(self.audit.is_flat_clear_glass(mat), name)

    def test_the_mirror_matches_the_renderer(self) -> None:
        for folder in self.audit.FLAT_CLEAR_GLASS_FOLDERS:
            self.assertIn(f'"{folder}"', self.source, folder)
        self.assertIn(f'"{self.audit.FLAT_CLEAR_GLASS_STEM}"', self.source)
        # The pale tint the upload falls back to, and the renderer's own copy.
        for channel, value in zip((0, 1, 2), self.audit.FLAT_GLASS_TINT):
            self.assertIn(f"range.color[{channel}] = {value:.2f}f;", self.source)

    def test_a_coloured_fill_keeps_its_database_tint(self) -> None:
        # The `_Base_Green` fills store a tint (0, 0.652, 0.596); the upload
        # used to overwrite it with the pale glass tint, which would bleach
        # the contents of every sample vial.
        upload = self.source[self.source.index("UNTEXTURED (no prefetch)") - 4000:]
        branch = upload.split("IsFlatClearGlassMat(range.matPath)", 1)[1]
        branch = branch[: branch.index("range.clearShell = true;")]
        self.assertIn("LiveModel::FindCdbTint(range.matPath, authored)", branch)
        self.assertIn("range.color[c] = authored[c];", branch)
        self.assertIn("range.clearShell = false;", branch)

    def test_the_token_glass_path_is_untouched(self) -> None:
        # Everything else that names itself glass still draws as a clear shell:
        # the new branch sits before the token scan, not inside it.
        body = self.source.split("bool IsFlatClearGlassMat(", 1)[1]
        body = body[: body.index("bool BuildGpuModel(")]
        self.assertIn("IsFlatClearGlassMat(a_matPath)", body)
        tokens = self.source.split("static constexpr const char* kTokens[]", 1)[1]
        tokens = tokens[: tokens.index("a_translucent = false;")]
        self.assertIn("return 0.00f;", tokens)


if __name__ == "__main__":
    unittest.main()
