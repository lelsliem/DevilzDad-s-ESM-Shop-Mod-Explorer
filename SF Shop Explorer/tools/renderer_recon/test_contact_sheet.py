#!/usr/bin/env python3
"""Guard for the offline contact-sheet renderer.

The tiles are only worth looking at if the parts under them are right, so this
pins the four things a wrong tile starts with: the BC1/BC4 decoders, the .mesh
layout (which is ParseMeshLod0's, byte for byte), the shared camera fit, and the
one translucent/opaque split the sheet exists to show - the Isotopic Coolant
jar's glass body and its solid lid.
"""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import contact_sheet as sheet  # noqa: E402

ARCHIVES = Path(sheet.audit.GAME_DATA).is_dir()


def bc1_block(c0: int, c1: int, indices: int) -> bytes:
    return struct.pack("<HHI", c0, c1, indices)


class DecoderTests(unittest.TestCase):
    def test_bc1_solid_block_decodes_to_its_colour(self) -> None:
        # c0 == c1 -> every texel takes c0; 0xF81F is magenta in RGB565.
        img = sheet.decode_bc1(bc1_block(0xF81F, 0xF81F, 0), 4, 4)
        self.assertIsNotNone(img)
        self.assertEqual(img.shape, (4, 4, 3))
        np.testing.assert_allclose(img.reshape(-1, 3), [[1.0, 0.0, 1.0]] * 16, atol=0.02)

    def test_bc1_punch_through_uses_the_three_colour_palette(self) -> None:
        # c0 <= c1 selects the 3-colour mode. Index 0 is still c0 (blue), and
        # index 2 is the midpoint of blue and red, so the two are separated.
        first = sheet.decode_bc1(bc1_block(0x001F, 0xF800, 0), 4, 4)
        middle = sheet.decode_bc1(bc1_block(0x001F, 0xF800, 0xAAAAAAAA), 4, 4)
        self.assertGreater(float(first[..., 2].mean()), float(first[..., 0].mean()))
        self.assertAlmostEqual(float(middle[..., 2].mean()), float(middle[..., 0].mean()), places=2)

    def test_bc4_solid_block_decodes_to_grey(self) -> None:
        block = bytes([200, 200, 0, 0, 0, 0, 0, 0])
        img = sheet.decode_bc4(block, 4, 4)
        self.assertIsNotNone(img)
        np.testing.assert_allclose(img.reshape(-1, 3), [[200 / 255.0] * 3] * 16, atol=0.01)

    def test_bc7_decodes_through_pillow(self) -> None:
        # DXGI 98/99 is BC7, which Starfield uses for almost every colour map.
        # The numpy decoders above cover BC1/3/4/5 only, so this one goes
        # through a rebuilt DX10 container; a 4x4 block is 16 bytes.
        img = sheet.decode_dds(bytes(16), 99, 4, 4)
        self.assertIsNotNone(img)
        self.assertEqual(img.shape, (4, 4, 3))
        self.assertLessEqual(float(img.max()), 1.0)

    def test_unknown_format_is_refused_rather_than_guessed(self) -> None:
        # DXGI 10 is R16G16B16A16_FLOAT: no decoder here, so the sheet must
        # fall back to the flat material colour instead of showing garbage.
        self.assertIsNone(sheet.decode_dds(b"\0" * 16, 10, 4, 4))

    def test_truncated_payload_is_refused(self) -> None:
        self.assertIsNone(sheet.decode_bc1(b"\0" * 8, 64, 64))


class _MatStore:
    """A store with one .mat blob and the textures it names."""

    def __init__(self, text: str, textures: set[str]) -> None:
        self.text = text
        self.textures = textures

    def read(self, _key: str) -> bytes:
        return self.text.encode("latin-1")

    def texture(self, key: str):
        return object() if key in self.textures else None


class _PathStore:
    """A store that only answers whether a path exists."""

    def __init__(self, *paths: str) -> None:
        self.paths = {p.replace("/", "\\").lower() for p in paths}

    def read(self, key: str) -> bytes | None:
        return b"x" if key.replace("/", "\\").lower() in self.paths else None


def _mat(*files: str) -> str:
    return "\n".join(f'"FileName" : "{f}"' for f in files)


class MaterialFileTests(unittest.TestCase):
    """The .mat last resort: the colour slot is a role, not a fixed spelling."""

    def setUp(self) -> None:
        sheet._MAT_COLOR_CACHE.clear()

    def _colour(self, text: str, textures: set[str]) -> str:
        return sheet._color_from_material("Materials\\X\\Thing.mat",
                                           _MatStore(text, textures))

    def test_a_classic_color_suffix_is_the_colour(self) -> None:
        text = _mat("Data\\Textures\\X\\thing_color.dds",
                    "Data\\Textures\\X\\thing_normal.dds")
        got = self._colour(text, {"textures/x/thing_color.dds", "textures/x/thing_normal.dds"})
        self.assertEqual(got, "textures/x/thing_color.dds")

    def test_a_mod_that_names_its_map_c_is_still_the_colour(self) -> None:
        # The Cantina mod's toaster: Cantina_SmallGenerator_C.dds, then NRM,
        # roughness and metal. Requiring a `_color` substring skipped it.
        text = _mat("Data\\Textures\\Cantina\\Cantina_SmallGenerator_C.dds",
                    "Data\\Textures\\Cantina\\Cantina_SmallGenerator_NRM.dds",
                    "Data\\Textures\\Cantina\\toaster_roughness.dds")
        got = self._colour(text, {"textures/cantina/cantina_smallgenerator_c.dds",
                                   "textures/cantina/cantina_smallgenerator_nrm.dds",
                                   "textures/cantina/toaster_roughness.dds"})
        self.assertEqual(got, "textures/cantina/cantina_smallgenerator_c.dds")

    def test_a_map_named_after_the_slot_resolves(self) -> None:
        # The Star Wars prop pack: Textures\\SWProps\\Datapad\\color.dds.
        text = _mat("Data\\Textures\\SWProps\\Datapad\\color.dds",
                    "Data\\Textures\\SWProps\\Datapad\\normal.dds")
        got = self._colour(text, {"textures/swprops/datapad/color.dds",
                                   "textures/swprops/datapad/normal.dds"})
        self.assertEqual(got, "textures/swprops/datapad/color.dds")

    def test_a_set_with_only_normal_and_rough_is_not_a_colour(self) -> None:
        text = _mat("Data\\Textures\\X\\thing_normal.dds",
                    "Data\\Textures\\X\\thing_rough.dds")
        got = self._colour(text, {"textures/x/thing_normal.dds", "textures/x/thing_rough.dds"})
        self.assertEqual(got, "")

    def test_a_map_the_host_does_not_have_is_refused(self) -> None:
        text = _mat("Data\\Textures\\X\\thing_color.dds")
        self.assertEqual(self._colour(text, set()), "")


class MeshTests(unittest.TestCase):
    def _mesh(self, nv: int = 3) -> bytes:
        idx = [0, 1, 2]
        out = struct.pack("<II", 0, len(idx))
        out += struct.pack("<3H", *idx)
        out += struct.pack("<f", 2.0)          # border
        out += struct.pack("<II", 1, nv)       # weights per vert, vertex count
        for i in range(nv):
            out += struct.pack("<3h", 1000 * i, -1000, 0)
        out += struct.pack("<I", nv)           # UV stream count == nv
        for i in range(nv):
            out += struct.pack("<2H", 0x3C00 + i, 0x3800)   # 1.0, 0.5 in half
        return out

    def test_positions_unpack_as_s16_over_32767_times_border(self) -> None:
        mesh = sheet.parse_mesh(self._mesh())
        self.assertIsNotNone(mesh)
        self.assertEqual(mesh["nv"], 3)
        self.assertAlmostEqual(float(mesh["pos"][1, 0]), 1000 / 32767.0 * 2.0, places=6)
        self.assertAlmostEqual(float(mesh["pos"][0, 1]), -1000 / 32767.0 * 2.0, places=6)

    def test_uv_set_is_read_when_its_count_matches_the_vertices(self) -> None:
        mesh = sheet.parse_mesh(self._mesh())
        self.assertIsNotNone(mesh["uv"])
        self.assertEqual(mesh["uv"].shape, (3, 2))
        self.assertAlmostEqual(float(mesh["uv"][0, 0]), 1.0, places=3)

    def test_out_of_range_index_rejects_the_file(self) -> None:
        blob = bytearray(self._mesh())
        struct.pack_into("<H", blob, 8, 7)     # first index -> vertex 7 of 3
        blob = bytes(blob)
        self.assertIsNone(sheet.parse_mesh(blob))


class CameraTests(unittest.TestCase):
    def test_unit_cube_fills_the_tile_inside_the_margin(self) -> None:
        cube = np.array([[x, y, z] for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)],
                        dtype=np.float64)
        screen, depth = sheet.camera(cube)
        self.assertEqual(screen.shape, (8, 2))
        self.assertEqual(depth.shape, (8,))
        px = sheet.fit(screen, 100, margin=0.1)
        self.assertGreaterEqual(px[:, 0].min(), 9.9)
        self.assertLessEqual(px[:, 0].max(), 90.1)
        self.assertGreaterEqual(px[:, 1].min(), 0.0)
        self.assertLessEqual(px[:, 1].max(), 100.0)

    def test_fit_keeps_the_aspect_ratio(self) -> None:
        line = np.array([[0.0, 0.0], [10.0, 2.0]])
        px = sheet.fit(line, 100, margin=0.0)
        self.assertAlmostEqual(float(np.ptp(px[:, 0])), 100.0, places=4)
        self.assertAlmostEqual(float(np.ptp(px[:, 1])), 20.0, places=4)


class GeometryTests(unittest.TestCase):
    def test_mesh_key_matches_the_archive_spelling(self) -> None:
        self.assertEqual(
            sheet.mesh_key("657478c21f1e48a01425\\52b4455363dc68a70393"),
            "geometries/657478c21f1e48a01425/52b4455363dc68a70393.mesh")

    def test_effect_overlays_are_not_drawn(self) -> None:
        for name in ("Materials\\SetDressing\\Animated\\WaterCaustic01_IdleAnimLoop.mat",
                     "Materials\\Items\\LegendaryCatalyst\\LegendaryCatalyst_Glow02.mat"):
            self.assertTrue(any(t in sheet.audit.norm(name) for t in sheet.EFFECT_TOKENS), name)

    def test_a_coloured_glass_body_tints_and_softens_the_rim(self) -> None:
        colour, alpha = sheet.rim_style(np.array([0.05, 0.45, 0.20]))
        self.assertEqual(alpha, sheet.CLEAR_RIM_ON_BODY_ALPHA)
        self.assertNotEqual(colour, sheet.CLEAR_RIM)
        self.assertGreater(colour[1], colour[0])
        self.assertEqual(sheet.rim_style(None),
                         (sheet.CLEAR_RIM, sheet.CLEAR_RIM_ALPHA))

    def test_clear_plastic_and_visors_are_clear_shells(self) -> None:
        # The industrial display case uses PlasticCleanClear01, not a glass
        # material; a visor may only name itself. Both should get the floor and
        # the contour, while an opaque worn plastic must not.
        display = {
            "mat": "Materials\\SetDressing\\PlasticCleanClear01.mat",
            "alpha": 0.0,
            "alpha_why": "clear in the name -> clear shell (fresnel only)",
        }
        self.assertTrue(sheet.is_clear_shell(display))
        self.assertEqual(sheet.preview_alpha(display), sheet.CLEAR_PREVIEW_ALPHA)
        self.assertTrue(sheet.needs_rim(display))
        visor = {"mat": "Materials\\Apparel\\Visor01.mat", "alpha": 0.0}
        self.assertTrue(sheet.needs_rim(visor))
        self.assertFalse(sheet.is_clear_shell({"mat": "Materials\\PlasticWorn02.mat",
                                               "alpha": 0.0}))

    def test_the_rim_is_for_translucent_glass_only(self) -> None:
        glass = "Materials\\SetDressing\\ScienceGlass\\GlassCleanTransparent01.mat"
        self.assertTrue(sheet.needs_rim({"mat": glass, "alpha": 0.05}))
        self.assertFalse(sheet.needs_rim({"mat": glass, "alpha": 1.0}))
        self.assertFalse(sheet.needs_rim({"mat": "Materials\\Metal\\MetalSteel02.mat", "alpha": 0.05}))

    def test_a_visor_shape_is_a_clear_pane_even_with_an_opaque_material(self) -> None:
        # The Spartan visor's material is `MARKVII_Helmet.mat` at alpha 1.0;
        # the NIF's one BSGeometry is the visor. The shape is the evidence the
        # material path does not carry.
        visor = {"mat": "Materials\\SpartanArmorCollection\\MarkVII\\Helmets\\"
                        "Default\\MARKVII_Helmet.mat",
                 "alpha": 1.0, "alpha_why": "alpha 1.0: opaque",
                 "node": "MarkVIIHelmet_MARKVII_visor"}
        self.assertTrue(sheet.is_clear_shell(visor))
        self.assertEqual(sheet.preview_alpha(visor), sheet.CLEAR_PANE_ALPHA)
        self.assertTrue(sheet.needs_rim(visor))
        # A pane geometry with a genuinely opaque material is still one: the
        # clear-shell rule does not lower a named glass body's authored 1.0.
        body = {"mat": "Materials\\Common\\Glass\\GlassCracked03_Opaque.mat",
                "alpha": 1.0, "node": "Glass"}
        self.assertEqual(sheet.preview_alpha(body), 1.0)
        self.assertFalse(sheet.needs_rim(body))

    def test_a_window_frame_or_visor_housing_is_not_a_pane(self) -> None:
        for node in ("WindowFrame", "VisorHousing", "LensMount"):
            mat = {"mat": "Materials\\Metal\\MetalSteel02.mat", "alpha": 1.0,
                   "node": node}
            self.assertFalse(sheet.clear_pane_shape(mat), node)
            self.assertFalse(sheet.needs_rim(mat), node)

    def test_armor_addons_skip_the_dressing_proxy(self) -> None:
        item = {"nif_paths": [
            "SetDressing\\SpaceSuitContainer_black.nif",
            "clothes\\SpartanArmorCollection\\MarkVII\\MarkVIIHelmet_MARKVII_visor.nif",
            "clothes\\SpartanArmorCollection\\MarkVII\\MARKVIIHelmet_MARKVII_visor.nif",
        ]}
        self.assertEqual(sheet.catalog_addons(item), [
            "clothes\\SpartanArmorCollection\\MarkVII\\MarkVIIHelmet_MARKVII_visor.nif"])

    def test_a_misc_container_keeps_its_catalog_model(self) -> None:
        # `SetDressing\Container\useful morgues\biohazardcanister.nif` is the
        # Biowaste item itself, not an armour dressing box. The proxy rule is
        # for ARMO addons only.
        paths = ["SetDressing\\Container\\useful morgues\\biohazardcanister.nif"]
        item = {"nif_paths": paths}
        self.assertEqual(sheet.catalog_models(item), paths)

    def test_armor_addons_keep_the_female_model_and_drop_the_male_copy(self) -> None:
        # FormModelPaths takes the female model of an ARMA when it exists. The
        # catalog flattens both genders, so the offline resolver has to pair
        # the basenames before drawing or the two copies overlap.
        item = {"nif_paths": [
            "SetDressing\\OutfitContainer01.nif",
            "Clothes\\coat\\Coat_M.nif",
            "Clothes\\coat\\Coat_F.nif",
            "Clothes\\coat\\Gloves_M.nif",
        ]}
        self.assertEqual(sheet.catalog_addons(item), [
            "Clothes\\coat\\Coat_F.nif",
            "Clothes\\coat\\Gloves_M.nif",
        ])

    def test_an_unsuffixed_female_model_replaces_the_male_copy(self) -> None:
        # swvt and a few mods leave the female model unsuffixed; the dump's
        # order is male then female for each ARMA.
        item = {"nif_paths": [
            "Clothes\\swvt\\UpperBody_M.nif",
            "Clothes\\swvt\\UpperBody.nif",
            "Clothes\\swvt\\LowerBody_M.nif",
            "Clothes\\swvt\\LowerBody.nif",
        ]}
        self.assertEqual(sheet.catalog_addons(item), [
            "Clothes\\swvt\\UpperBody.nif",
            "Clothes\\swvt\\LowerBody.nif",
        ])

    def test_a_ground_model_is_not_drawn_beside_the_worn_armour(self) -> None:
        # The dump puts worldModels first. A backpack's `_go` model is the
        # inventory prop; FormModelPaths draws only the ARMA addon.
        item = {"nif_paths": [
            "Clothes\\Spacesuit_GroundCrew\\spacesuit_groundcrew_01_backpack_go.nif",
            "Clothes\\spacesuit_groundcrew\\SpaceSuit_GroundCrew_Backpack_M.nif",
        ]}
        self.assertEqual(sheet.catalog_addons(item), [
            "Clothes\\spacesuit_groundcrew\\SpaceSuit_GroundCrew_Backpack_M.nif"])

    def test_a_ground_container_is_not_drawn_beside_the_armour(self) -> None:
        item = {"nif_paths": [
            "clothes\\spacesuit_commando\\commando_groundcontainer.nif",
            "clothes\\spacesuit_commando\\Commando_Helmet.nif",
        ]}
        self.assertEqual(sheet.catalog_addons(item), [
            "clothes\\spacesuit_commando\\Commando_Helmet.nif"])

    def test_a_lone_ground_model_is_still_drawn(self) -> None:
        item = {"nif_paths": ["clothes\\hat\\hat_go.nif"]}
        self.assertEqual(sheet.catalog_addons(item), ["clothes\\hat\\hat_go.nif"])

    def test_an_absent_ammo_crate_placeholder_uses_the_shipped_crate(self) -> None:
        store = _PathStore(r"SetDressing\ammo_crates\ammobox_01.nif")
        paths = [r"SetDressing\ammo_crates\typea_xlarge.nif",
                 r"Ammo\10mm\10mmShellCasing.nif"]
        self.assertEqual(sheet.ammo_models(paths, store), [
            r"SetDressing\ammo_crates\ammobox_01.nif",
            r"Ammo\10mm\10mmShellCasing.nif"])

    def test_a_present_ammo_crate_placeholder_is_left_alone(self) -> None:
        real = r"SetDressing\ammo_crates\typec_large.nif"
        store = _PathStore(real, r"SetDressing\ammo_crates\ammobox_01.nif")
        self.assertEqual(sheet.ammo_models([real], store), [real])

    def test_a_missing_non_placeholder_is_not_repointed(self) -> None:
        missing = r"testspaceshiprocketprojectile.nif"
        store = _PathStore(r"SetDressing\ammo_crates\ammobox_01.nif")
        self.assertEqual(sheet.ammo_models([missing], store), [missing])

    def test_parse_catalog_keeps_the_resolved_nif_paths(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            catalog = Path(tmp) / "catalog.csv"
            catalog.write_text(
                "plugin,formID,type,name,editorId,modelPath,nifPaths\n"
                "A.esm,FE047C89,ARMO,Mark VII Helmet Visor,Edid,"
                "SetDressing\\SpaceSuitContainer_black.nif,"
                "SetDressing\\SpaceSuitContainer_black.nif | "
                "clothes\\SpartanArmorCollection\\MarkVII\\MarkVIIHelmet_MARKVII_visor.nif\n",
                encoding="utf-8")
            items = sheet.audit.parse_catalog(catalog)
        self.assertEqual(items[0]["model"],
                         "SetDressing\\SpaceSuitContainer_black.nif")
        self.assertEqual(items[0]["nif_paths"], [
            "SetDressing\\SpaceSuitContainer_black.nif",
            "clothes\\SpartanArmorCollection\\MarkVII\\MarkVIIHelmet_MARKVII_visor.nif"])

    def test_a_glass_rim_follows_the_silhouette_not_the_triangles(self) -> None:
        # A rectangle stands in for a cylinder: the rim is its contour, and the
        # interior stays unpainted. Thickness is a pixel count, not a triangle
        # outline, so the tile does not become a wireframe.
        mask = Image.new("L", (9, 9), 0)
        ImageDraw.Draw(mask).rectangle([2, 2, 6, 6], fill=255)
        edge = sheet.outline_mask(mask, 1)
        self.assertEqual(edge.getpixel((2, 4)), 255)
        self.assertEqual(edge.getpixel((4, 4)), 0)
        thick = sheet.outline_mask(mask, 2)
        self.assertEqual(thick.getpixel((3, 4)), 255)
        self.assertEqual(thick.getpixel((4, 4)), 0)

    def test_an_enclosed_hole_is_not_an_extra_rim(self) -> None:
        # A lens inside a frame, or a light strip inside a visor, encloses area
        # the silhouette does not. Only the outside of the piece is rimmed.
        mask = Image.new("L", (11, 11), 0)
        draw = ImageDraw.Draw(mask)
        draw.rectangle([2, 2, 8, 8], fill=255)
        draw.rectangle([4, 4, 6, 6], fill=0)
        edge = sheet.outline_mask(mask, 1)
        self.assertEqual(edge.getpixel((2, 5)), 255)
        self.assertEqual(edge.getpixel((4, 4)), 0)
        self.assertEqual(edge.getpixel((5, 5)), 0)

    def test_two_separate_panes_keep_two_outer_rims(self) -> None:
        mask = Image.new("L", (41, 9), 0)
        draw = ImageDraw.Draw(mask)
        draw.rectangle([1, 1, 10, 7], fill=255)
        draw.rectangle([26, 1, 35, 7], fill=255)
        edge = sheet.outline_mask(mask, 1)
        self.assertEqual(edge.getpixel((1, 4)), 255)
        self.assertEqual(edge.getpixel((5, 4)), 0)
        self.assertEqual(edge.getpixel((26, 4)), 255)
        self.assertEqual(edge.getpixel((30, 4)), 0)
        self.assertEqual(edge.getpixel((18, 4)), 0)

    def test_nearby_panes_of_one_prop_merge_before_the_rim(self) -> None:
        # A visor's two eye panes are separate islands with the frame gap
        # between them. Outlined on their own, each facing edge becomes a
        # vertical rim; closing the gap first makes the gap interior.
        mask = Image.new("L", (160, 160), 0)
        draw = ImageDraw.Draw(mask)
        draw.rectangle([40, 60, 69, 100], fill=255)
        draw.rectangle([78, 60, 107, 100], fill=255)
        merged = sheet.merge_panes(mask, 160)
        self.assertEqual(merged.getpixel((74, 80)), 255)
        edge = sheet.outline_mask(merged, 1)
        self.assertEqual(edge.getpixel((74, 80)), 0)
        self.assertEqual(edge.getpixel((40, 80)), 255)

    def test_merging_near_the_tile_edge_does_not_rim_the_border(self) -> None:
        # BoxBlur truncates at the canvas edge; without padding, a pane within
        # the merge radius of the edge survives erosion along that edge and the
        # rim runs down the side of the tile.
        mask = Image.new("L", (160, 160), 0)
        ImageDraw.Draw(mask).rectangle([2, 60, 30, 100], fill=255)
        merged = sheet.merge_panes(mask, 160)
        self.assertEqual(merged.getpixel((0, 80)), 0)
        self.assertEqual(merged.getpixel((5, 80)), 255)

    def test_bacta_vial_models_get_the_wider_family_close(self) -> None:
        # The vial's shell is split into two panes with a gap wider than the
        # default close. Only the models that need it get a wider radius; the
        # same BactaGlass shape is used by injectors and sprays that are one
        # pane, so the override is keyed by NIF, not by shape or material.
        for model in sheet.PANE_MERGE_SCALE_BY_MODEL:
            self.assertEqual(sheet.pane_merge_scale(["SWProps\\" + model]), 13, model)
        self.assertEqual(sheet.pane_merge_scale(["Clothes\\MarkVII_visor.nif"]),
                         sheet.PANE_MERGE_SCALE)

    def test_vial_close_merges_a_gap_the_default_leaves_split(self) -> None:
        # At both preview sizes the same proportion of the tile separates the
        # two shell panes. The default radius leaves them as two islands; the
        # family radius closes the frame gap into one silhouette.
        for size, gap in ((160, 20), (512, 64)):
            left, width = size // 4, size // 6
            mask = Image.new("L", (size, size), 0)
            draw = ImageDraw.Draw(mask)
            draw.rectangle([left, size // 3, left + width, size * 2 // 3], fill=255)
            draw.rectangle([left + width + gap, size // 3,
                            left + 2 * width + gap, size * 2 // 3], fill=255)
            midpoint = (left + width + gap // 2, size // 2)
            self.assertEqual(sheet.merge_panes(mask, size).getpixel(midpoint), 0, size)
            merged = sheet.merge_panes(
                mask, size, sheet.pane_merge_scale(["SWProps\\Bacta.nif"]))
            self.assertEqual(merged.getpixel(midpoint), 255, size)

    def test_panes_far_apart_stay_separate_after_merging(self) -> None:
        # Same group, not the same prop: a rack's vials are close in world
        # space but their glass panes are not adjacent in the tile.
        mask = Image.new("L", (160, 160), 0)
        draw = ImageDraw.Draw(mask)
        draw.rectangle([10, 60, 39, 100], fill=255)
        draw.rectangle([110, 60, 139, 100], fill=255)
        merged = sheet.merge_panes(mask, 160)
        self.assertEqual(merged.getpixel((74, 80)), 0)
        self.assertEqual(merged.getpixel((25, 80)), 255)
        edge = sheet.outline_mask(merged, 1)
        self.assertEqual(edge.getpixel((10, 80)), 255)
        self.assertEqual(edge.getpixel((110, 80)), 255)

    def test_world_bounds_merge_panes_but_not_separate_props(self) -> None:
        def cube(x: float) -> np.ndarray:
            # Real rim parts carry an (n, 3, 3) triangle array, not a flat
            # vertex list; the bbox code must flatten before comparing.
            return np.array([[[x, 0, 0], [x + 1, 1, 0], [x, 1, 1]]],
                            dtype=np.float32)

        parts = [
            {"rim": True, "nif": "visor.nif", "tris": cube(0.0)},
            {"rim": True, "nif": "visor.nif", "tris": cube(0.4)},
            {"rim": True, "nif": "visor.nif", "tris": cube(8.0)},
            {"rim": True, "nif": "other.nif", "tris": cube(0.2)},
            {"rim": False, "nif": "visor.nif", "tris": cube(20.0)},
        ]
        groups = sorted(sorted(group) for group in sheet.rim_groups(parts))
        self.assertEqual(groups, [[0, 1], [2], [3]])

    def test_clear_glass_gets_a_visible_preview_alpha(self) -> None:
        # An empty vial's glass is authored at 0.05; against the dark backdrop
        # that is invisible, so the tile draws it at the preview floor while
        # the material report keeps the authored value.
        glass = "Materials\\SetDressing\\ScienceGlass\\GlassCleanTransparent01.mat"
        self.assertEqual(sheet.preview_alpha({"mat": glass, "alpha": 0.05}),
                         sheet.CLEAR_PREVIEW_ALPHA)
        self.assertGreater(sheet.preview_alpha({"mat": glass, "alpha": 0.05}), 0.05)
        # A non-glass translucent material keeps its authored alpha.
        self.assertEqual(sheet.preview_alpha({"mat": "Materials\\Metal\\MetalSteel02.mat", "alpha": 0.05}), 0.05)
        self.assertEqual(sheet.preview_alpha({"mat": glass, "alpha": 0.55}), 0.55)

    @unittest.skipUnless(ARCHIVES, "Starfield archives are not installed here")
    def test_jar_is_glass_over_a_solid_lid(self) -> None:
        store = sheet.audit.AssetStore([Path(sheet.audit.GAME_DATA)],
                                       Path(sheet.audit.MODS_ROOT), False)
        plugins = sheet.audit.Plugins(store)
        swaps = sheet.audit.Swaps(Path(sheet.audit.SWAPS))
        cdb = sheet.audit.Cdb(Path(sheet.audit.CDB))
        textures = sheet.Textures(store)
        item = {"name": "Alien Genetic Material", "form": 0x000C1F57,
                "plugin": "Starfield.esm", "type": "ALCH", "keywords": ""}
        geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, {})
        by_name = {g["name"]: g for g in geoms}
        self.assertIn("Node02_A:5", by_name, meta["status"])
        # The wall is still the translucent pass (that is what lets the
        # specimen show through), but its stand-in alpha is now 0: a clear
        # dielectric's coverage is its Fresnel reflectance and nothing else.
        # 0.30 was the 51%-grey sleeve of the 2026-10-21 captures.
        self.assertTrue(by_name["Node02_A:5"]["mat"]["translucent"])
        self.assertAlmostEqual(by_name["Node02_A:5"]["mat"]["alpha"],
                               sheet.audit.AUTHORED_SHEER_ALPHA, places=3)
        self.assertLess(by_name["Node02_A:5"]["mat"]["alpha"], 1.0)
        self.assertFalse(by_name["Node02_A:3"]["mat"].get("translucent", False))
        self.assertEqual(by_name["Node02_A:3"]["mat"]["alpha"], 1.0)
        # The specimen is drawn, not swallowed by a caustic shell.
        self.assertIn("Node01_A:0", by_name)

    @unittest.skipUnless(ARCHIVES, "Starfield archives are not installed here")
    def test_a_catalog_armor_nif_renders_a_visor(self) -> None:
        # ARMO's MODL is an ARMA form link, so the plugin walk has no path. The
        # dump's nifPaths fallback is what makes this visor drawable at all.
        catalog = sheet.audit.parse_catalog(sheet.audit.find_catalog(None))
        item = next(it for it in catalog if it["name"] == "Mark VII Helmet Visor")
        store = sheet.audit.AssetStore([Path(sheet.audit.GAME_DATA)],
                                       Path(sheet.audit.MODS_ROOT), False)
        plugins = sheet.audit.Plugins(store)
        swaps = sheet.audit.Swaps(Path(sheet.audit.SWAPS))
        cdb = sheet.audit.Cdb(Path(sheet.audit.CDB))
        textures = sheet.Textures(store)
        geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, {})
        self.assertTrue(geoms, meta["status"])
        self.assertIn("visor", meta["model"].lower())
        self.assertIn("visor", geoms[0]["mat"]["node"].lower())
        self.assertEqual(sheet.preview_alpha(geoms[0]["mat"]),
                         sheet.CLEAR_PANE_ALPHA)

    @unittest.skipUnless(ARCHIVES, "Starfield archives are not installed here")
    def test_to_go_cup_renders_nothing_translucent(self) -> None:
        store = sheet.audit.AssetStore([Path(sheet.audit.GAME_DATA)],
                                       Path(sheet.audit.MODS_ROOT), False)
        plugins = sheet.audit.Plugins(store)
        swaps = sheet.audit.Swaps(Path(sheet.audit.SWAPS))
        cdb = sheet.audit.Cdb(Path(sheet.audit.CDB))
        textures = sheet.Textures(store)
        item = {"name": "Tranquilitea Classic", "form": 0x00002449,
                "plugin": "Starfield.esm", "type": "ALCH", "keywords": ""}
        geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, {})
        self.assertTrue(geoms, meta["status"])
        self.assertTrue(all(g["mat"]["alpha"] >= 0.999 for g in geoms),
                        [(g["name"], g["mat"]["alpha"]) for g in geoms])


    @unittest.skipUnless(ARCHIVES, "Starfield archives are not installed here")
    def test_proxy_armor_draws_every_female_addon(self) -> None:
        # A dressing-box world model is not the garment. The preview draws the
        # whole linked addon set, and only the female copy of each ARMA part.
        catalog = sheet.audit.parse_catalog(sheet.audit.find_catalog(None))
        item = next(it for it in catalog if it["name"] == "Medic Uniform")
        self.assertEqual(item["type"], "ARMO")
        store = sheet.audit.AssetStore([Path(sheet.audit.GAME_DATA)],
                                       Path(sheet.audit.MODS_ROOT), False)
        plugins = sheet.audit.Plugins(store)
        swaps = sheet.audit.Swaps(Path(sheet.audit.SWAPS))
        cdb = sheet.audit.Cdb(Path(sheet.audit.CDB))
        textures = sheet.Textures(store)
        geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, {})
        self.assertTrue(geoms, meta["status"])
        self.assertEqual(sorted(Path(p).name for p in meta["parts"]), [
            "Outfit_Medical_Doctor_LowerBody_01_F.nif",
            "Outfit_Medical_Doctor_Sleeves_01_F.nif",
            "Outfit_Medical_Doctor_UpperBody_01_F.nif",
        ])

    @unittest.skipUnless(ARCHIVES, "Starfield archives are not installed here")
    def test_ammo_draws_the_crate_and_shell_casing_together(self) -> None:
        # FormModelPaths pushes both the box and the casing; a round whose box
        # is absent still has its casing to draw.
        catalog = sheet.audit.parse_catalog(sheet.audit.find_catalog(None))
        item = next(it for it in catalog if it["form"] == 0x00014CBC)
        self.assertEqual(item["type"], "AMMO")
        store = sheet.audit.AssetStore([Path(sheet.audit.GAME_DATA)],
                                       Path(sheet.audit.MODS_ROOT), False)
        plugins = sheet.audit.Plugins(store)
        swaps = sheet.audit.Swaps(Path(sheet.audit.SWAPS))
        cdb = sheet.audit.Cdb(Path(sheet.audit.CDB))
        textures = sheet.Textures(store)
        geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, {})
        self.assertTrue(geoms, meta["status"])
        self.assertEqual(sorted(Path(p).name for p in meta["parts"]), [
            "10mmShellCasing.nif", "Ammo777mm.nif"])

    @unittest.skipUnless(ARCHIVES, "Starfield archives are not installed here")
    def test_key_without_a_model_uses_the_generic_keycard(self) -> None:
        # FormModelPaths gives a key with no model string the generic keycard;
        # the offline path must do the same instead of reporting no MODL.
        catalog = sheet.audit.parse_catalog(sheet.audit.find_catalog(None))
        item = next(it for it in catalog if it["form"] == 0x01010A9E)
        store = sheet.audit.AssetStore([Path(sheet.audit.GAME_DATA)],
                                       Path(sheet.audit.MODS_ROOT), False)
        plugins = sheet.audit.Plugins(store)
        swaps = sheet.audit.Swaps(Path(sheet.audit.SWAPS))
        cdb = sheet.audit.Cdb(Path(sheet.audit.CDB))
        textures = sheet.Textures(store)
        geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, {})
        self.assertTrue(geoms, meta["status"])
        self.assertEqual(meta["parts"], [sheet.KEYCARD_FALLBACK])
        self.assertTrue(meta["model"].lower().endswith("keycardgen01.nif"))


if __name__ == "__main__":
    unittest.main()
