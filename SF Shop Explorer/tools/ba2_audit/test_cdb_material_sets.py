"""Regression tests for DIFF-framed materialsbeta.cdb texture-set ownership."""
from __future__ import annotations

import os
import re
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_sweep import build_cdb_index  # noqa: E402

RECON = Path(__file__).resolve().parents[1] / "renderer_recon"
sys.path.insert(0, str(RECON))
from cdb_alpha_replica import scan as scan_cdb_overrides  # noqa: E402


def _diff(field_id: int, value: str) -> bytes:
    encoded = value.encode("ascii") + b"\0"
    count = len(encoded)
    length = 8 + count
    return (
        b"DIFF"
        + length.to_bytes(4, "little")
        + field_id.to_bytes(4, "little")
        + (0).to_bytes(2, "little")
        + count.to_bytes(2, "little")
        + encoded
    )


def _family_gate_source() -> str:
    """The CDB weapon-family gate and the directory helper it calls.

    The helper (`WeaponDirNamesFamily`) sits above the gate, so the span starts
    at whichever comes first: the replica parses its threshold out of it.
    """
    live_model = Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp"
    source = live_model.read_text(encoding="utf-8")
    return source[source.index("bool WeaponDirNamesFamily("):source.index("bool FindDiffuse(")]


def _family_matches(family_fn: str, material: str, texture: str) -> bool:
    """Replica of `CdbWeaponTextureMatchesFamily` in src/LiveModel.cpp.

    The constants are parsed out of the C++ function so the replica cannot
    silently drift from the blocklist or the extension threshold. The four
    decisions, in the order the C++ applies them: an identifiable material
    family is required; explicitly shareable AWP maps are allowed; a
    `/weapons/` directory segment must name the family (exactly or, for the
    Coachman case, as a longer stem such as `coachmanshotgun`); otherwise
    only a bare family token in a non-weapon texture path is accepted.
    """
    generic_block = re.search(r"kGenericTokens\s*\{(?P<body>.*?)\n\s*\};", family_fn, re.S)
    if generic_block is None:
        raise AssertionError("weapon-family generic token list not found")
    generic_tokens = set(re.findall(r'"([^" ]+)"', generic_block.group("body")))
    extension = re.search(r"kMinFamilyStemExtension\s*=\s*(\d+)", family_fn)
    if extension is None:
        raise AssertionError("weapon-family stem-extension threshold not found")
    min_extension = int(extension.group(1))
    material = material.lower().replace("\\", "/")
    texture = texture.lower().replace("\\", "/")
    prefix = "materials/weapons/"
    if not material.startswith(prefix):
        return True
    # The C++ takes the region between the prefix and the path's LAST slash,
    # and leaves immediately when there is no directory under it (a material
    # sitting directly in materials/weapons/ has no family): that is a
    # different question from the prefix-stripped remainder, and getting it
    # wrong made the replica disagree with the compiled gate on 8 of 4,960
    # pairs when both were run side by side.
    material_end = material.rfind("/")
    if material_end < len(prefix):
        return True
    family_dir = material[len(prefix):material_end]
    material_tokens = {t for t in re.findall(r"[a-z0-9]+", family_dir)
                       if len(t) >= 4 and t not in generic_tokens}
    texture_tokens = {t for t in re.findall(r"[a-z0-9]+", texture)
                      if len(t) >= 4 and t not in generic_tokens}
    shared_awp = "/awp_shareable/" in texture or "/awp_master_shareable/" in texture
    if family_dir in ("awp_master_shareable", "awp_shareable"):
        return shared_awp
    if not material_tokens:
        return True
    if shared_awp:
        return True
    if "/weapons/" in texture and _weapon_dir_names_family(texture, material_tokens, min_extension):
        return True
    for family in material_tokens:
        if family not in texture_tokens:
            continue
        if "/weapons/" not in texture:
            return True
        if f"/weapons/{family}/" in texture or f"/weapons/{family}_" in texture:
            return True
    return False


def _weapon_dir_names_family(texture: str, family_tokens: set[str], min_extension: int) -> bool:
    """Replica of `WeaponDirNamesFamily`: a directory under `/weapons/` that
    equals a family token or continues it by at least `min_extension`
    characters. Directory segments only - a dot marks the file name."""
    root = texture.find("/weapons/")
    while root != -1:
        i = root + len("/weapons/")
        while i < len(texture):
            slash = texture.find("/", i)
            end = len(texture) if slash == -1 else slash
            segment = texture[i:end]
            if segment and "." not in segment:
                for family in family_tokens:
                    if not segment.startswith(family):
                        continue
                    if len(segment) == len(family) or len(segment) >= len(family) + min_extension:
                        return True
            if slash == -1:
                break
            i = slash + 1
        root = texture.find("/weapons/", root + 1)
    return False


STARFIELD_DATA = Path(os.environ.get(
    "STARFIELD_DATA", "B:/SteamLibrary/steamapps/common/Starfield/Data"))


def _archive_texture_keys(globs: list[str]) -> set[str]:
    """Every texture key in the shipped texture archives, or a skip."""
    from ba2lib import Ba2Archive

    if not STARFIELD_DATA.is_dir():
        raise unittest.SkipTest(f"no Starfield data at {STARFIELD_DATA}")
    keys: set[str] = set()
    for pattern in globs:
        for path in STARFIELD_DATA.glob(pattern):
            archive = Ba2Archive(path)
            keys.update(archive.tex_files)
    return keys


class CdbMaterialSetTests(unittest.TestCase):
    def test_texture_paths_can_precede_or_follow_the_texture_set_name_without_leaking(self) -> None:
        blob = b"".join(
            (
                # This book's empty cover TextureSet must not inherit the
                # nearby unrelated texture-bearing material's CDB field.
                _diff(0x038A, "Data\\Textures\\SetDressing\\Books\\genbooka01_color.dds"),
                _diff(0x01F4, "unrelated_material_Material1"),
                _diff(0x01F4, "materials/setdressing/books/genbookb01_bookb04_TextureSet1"),
                _diff(0x01F4, "materials/weapons/rattler/rattler_receiver_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Weapons\\Rattler\\Heater_Receiver_color.DDS"),
                _diff(0x038A, "Data\\Textures\\Weapons\\Rattler\\Heater_Receiver_opacity.DDS"),
                _diff(0x01F4, "materials/weapons/rattler/rattler_slideandlower_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Weapons\\Rattler\\Heater_SlideandLower_color.DDS"),
                _diff(0x01F4, "materials/weapons/rattler/rattler_decals_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Weapons\\Rattler\\Heater_Decals_color.DDS"),
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            cdb = Path(directory) / "materialsbeta.cdb"
            cdb.write_bytes(blob)
            index = build_cdb_index(cdb)

        self.assertEqual(
            index["materials/setdressing/books/genbookb01_bookb04"],
            {},
        )
        self.assertEqual(
            index["materials/weapons/rattler/rattler_receiver"],
            {
                "color": "textures/weapons/rattler/heater_receiver_color.dds",
                "opacity": "textures/weapons/rattler/heater_receiver_opacity.dds",
            },
        )
        self.assertEqual(
            index["materials/weapons/rattler/rattler_slideandlower"]["color"],
            "textures/weapons/rattler/heater_slideandlower_color.dds",
        )
        self.assertEqual(
            index["materials/weapons/rattler/rattler_decals"]["color"],
            "textures/weapons/rattler/heater_decals_color.dds",
        )

    def test_planet_terrain_layer_is_indexed_as_its_material(self) -> None:
        """The desktop globe asks for a planet's colour layer by its material.

        `IH_Desktop_PlanetGlobe02.nif` names `Desktop_PlanetGlobe_Planet_
        Alchiba II.mat`, and the database answers with a layer called
        `Desktop_PlanetGlobe_Planet_Alchiba II_terrain` that carries
        BiomeMasks/Alchiba II_color.dds. Before 6r the layer word kept the
        name from matching any candidate, so the sphere drew flat grey beside
        the engine's textured planet.
        """
        blob = b"".join(
            (
                _diff(0x01F4, "Desktop_PlanetGlobe_Planet_Alchiba II_terrain"),
                _diff(0x038A, "Data\\Textures\\Planets\\HoudiniPlanets\\BiomeMasks\\Alchiba II_color.dds"),
                _diff(0x038A, "Data\\Textures\\Planets\\HoudiniPlanets\\TerrainMapsShared\\rock_03_thermal_normal.dds"),
                # The node graph and HDA beside it are not art: keying them
                # would invent thousands of sets nothing can reach.
                _diff(0x01F4, "HoudiniPlanet_Alchiba II_material_1_terrain"),
                _diff(0x038A, "Data\\Textures\\Planets\\HoudiniPlanets\\TerrainMapsShared\\rock_03_thermal_rough.dds"),
                _diff(0x01F4, "HoudiniPlanet_Alchiba II_uvstream_1_terrain"),
                _diff(0x01F4, "HoudiniPlanet_Alchiba II_textureset_1_terrain"),
                _diff(0x038A, "Data\\Textures\\Planets\\HoudiniPlanets\\BiomeMasks\\Alchiba II_color.dds"),
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            cdb = Path(directory) / "materialsbeta.cdb"
            cdb.write_bytes(blob)
            index = build_cdb_index(cdb)

        self.assertEqual(
            index["desktop_planetglobe_planet_alchiba ii"]["color"],
            "textures/planets/houdiniplanets/biomemasks/alchiba ii_color.dds",
        )
        # The planet's own `_textureset_1_terrain` spelling is not a set key:
        # its number is separated by an underscore, which the texture-set rule
        # has never matched, and inventing a key for it would add 3,500 sets
        # nothing looks up.
        self.assertNotIn("houdiniplanet_alchiba ii_textureset_1", index)
        self.assertNotIn("houdiniplanet_alchiba ii_material_1", index)
        self.assertNotIn("houdiniplanet_alchiba ii_uvstream_1", index)

    def test_shipped_desktop_globe_resolves_to_its_planet_colour(self) -> None:
        cdb = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        index = build_cdb_index(cdb)
        self.assertEqual(
            index["desktop_planetglobe_planet_alchiba ii"]["color"],
            "textures/planets/houdiniplanets/biomemasks/alchiba ii_color.dds",
        )

    def test_shipped_book_and_decal_texture_sets_resolve_to_their_own_paths(self) -> None:
        cdb = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        index = build_cdb_index(cdb)
        self.assertEqual(index["data/materials/setdressing/books/genbookb01_bookb04.mat"], {})
        self.assertEqual(
            index["data/materials/setdressing/books/genbookb01_pagesb01.mat"]["color"],
            "textures/setdressing/books/genbooka01_color.dds",
        )
        self.assertEqual(
            index["data/materials/setdressing/books/decalletterlineoverlay01_linesstripes.mat"],
            {},
        )
        self.assertEqual(
            index["data/materials/setdressing/books/decalletterlineoverlay01_letters.mat"],
            {},
        )
        self.assertEqual(index["genbookb01_bookb01"], {})
        self.assertNotIn("genbookb01_bookb01.mat", index)
        self.assertEqual(
            index["data/materials/setdressing/books/decalletterlineoverlay01_patternf.mat"]["color"],
            "textures/effects/gobos/lift02_color.dds",
        )
        # Regression for the live King-of-Hearts screenshot: the effect
        # texture preceding this set name used to bleed into its atlas lookup.
        self.assertEqual(
            index["exoticplayingcard_symbols"]["color"],
            "textures/setdressing/exotic_clutter/exoticplayingcards/exoticplayingcard_symbols_decal_color.dds",
        )
        self.assertEqual(
            index["exoticplayingcard_symbols"]["opacity"],
            "textures/setdressing/exotic_clutter/exoticplayingcards/exoticplayingcard_symbols_decal_opacity.dds",
        )
        self.assertEqual(index["exoticplayingcardbox01_trim_color02"], {})

    def test_weapon_texture_sets_keep_their_own_maps_across_unowned_preceding_paths(self) -> None:
        # These are real neighboring CDB objects from the shipped database:
        # material/texture set names close the previous object, and their
        # paths follow the texture-set name. A pre-name path must not shift the
        # weapon maps (the Rattler receiver was picking up Razorback mod art).
        blob = b"".join(
            (
                _diff(0x038A, "Data\\Textures\\Weapons\\Razorback\\Mods_Razorback_Compensator_color.dds"),
                _diff(0x01F4, "Rattler_Receiver_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Weapons\\Rattler\\Heater_Receiver_color.DDS"),
                _diff(0x038A, "Data\\Textures\\Weapons\\Rattler\\Heater_Receiver_opacity.DDS"),
                _diff(0x01F4, "Rattler_Slideandlower_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Weapons\\Rattler\\Heater_SlideandLower_color.DDS"),
                _diff(0x038A, "Data\\Textures\\Weapons\\Rattler\\Heater_SlideandLower_opacity.DDS"),
                _diff(0x01F4, "Rattler_Decals_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Weapons\\Rattler\\Heater_Decals_color.DDS"),
                _diff(0x038A, "Data\\Textures\\Weapons\\Rattler\\Heater_Decals_opacity.DDS"),
                _diff(0x01F4, "Rattler_Receiver_UVStream1"),
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            cdb = Path(directory) / "materialsbeta.cdb"
            cdb.write_bytes(blob)
            index = build_cdb_index(cdb)

        self.assertEqual(index["rattler_receiver"], {
            "color": "textures/weapons/rattler/heater_receiver_color.dds",
            "opacity": "textures/weapons/rattler/heater_receiver_opacity.dds",
        })
        self.assertEqual(index["rattler_slideandlower"], {
            "color": "textures/weapons/rattler/heater_slideandlower_color.dds",
            "opacity": "textures/weapons/rattler/heater_slideandlower_opacity.dds",
        })
        self.assertEqual(index["rattler_decals"], {
            "color": "textures/weapons/rattler/heater_decals_color.dds",
            "opacity": "textures/weapons/rattler/heater_decals_opacity.dds",
        })
        self.assertNotIn("rattler_receiver_uvstream1", index)

    def test_creation_texture_sets_resolve_through_their_own_field_id(self) -> None:
        # Measured on the installed game: the record that carries texture maps
        # is not 0x038A everywhere. sfbgs050 stores Breechblock's maps under
        # 0x0EE8, sfbgs007 under 0x03A9, shatteredspace under 0x031F. Keying
        # the parser to 0x038A alone left every Creation texture set with no
        # colour, and an empty set is authoritative "flat", so those items
        # drew as bare white geometry instead of taking their family scorer
        # fallback.
        blob = b"".join(
            (
                _diff(0x0EE8, "Data\\Textures\\Weapons\\SFBGS050\\Breechblock\\Receiver_2_color.dds"),
                _diff(0x0EE8, "Data\\Textures\\Weapons\\SFBGS050\\Breechblock\\Receiver_2_normal.dds"),
                _diff(0x0EE8, "Data\\Textures\\Weapons\\SFBGS050\\Breechblock\\Receiver_2_rough.dds"),
                _diff(0x0EE8, "Data\\Textures\\Weapons\\SFBGS050\\Breechblock\\Receiver_2_metal.dds"),
                _diff(0x01F4, "Breechblock_Receiver_2_UVStream1"),
                # The name closes the group, exactly as the shipped file does:
                # name first, then that set's own maps.
                _diff(0x01F4, "Breechblock_Receiver_2_TextureSet1"),
                _diff(0x0EE8, "Data\\Textures\\Weapons\\SFBGS050\\Breechblock\\Receiver_2_color.dds"),
                _diff(0x0EE8, "Data\\Textures\\Weapons\\SFBGS050\\Breechblock\\Receiver_2_ao.dds"),
                _diff(0x01F4, "Breechblock_Grip_UVStream1"),
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            cdb = Path(directory) / "materialsbeta.cdb"
            cdb.write_bytes(blob)
            index = build_cdb_index(cdb)

        self.assertEqual(index["breechblock_receiver_2"], {
            "color": "textures/weapons/sfbgs050/breechblock/receiver_2_color.dds",
        })
        # Only _color/_opacity (or _alpha) are maps; normal/rough/metal/ao are
        # companions, so this set must not gain an opacity entry.
        self.assertNotIn("opacity", index["breechblock_receiver_2"])

    def test_numbered_variants_are_never_merged_field_by_field(self) -> None:
        # `plasticplainsmooth02` has six numbered sets: TextureSet1 is the
        # material (a rough map and no albedo), TextureSet3 is a metal swap
        # carrying MetalCopperAged01_color.dds, TextureSet2 an unrelated
        # UnitedColoniesLogo_01_opacity.dds. The index merged them in file order,
        # so every plastic plate on every food item took the copper colour and
        # the mostly-black logo mask and turned into bronze punched through by a
        # hole. A donor is chosen whole now, and the numbering is the database's
        # own statement about which set is the material.
        blob = b"".join(
            (
                # Name first, then that set's own maps - the shipped order.
                _diff(0x01F4, "Menu_Plate_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Common\\Plastic\\Plate01_color.DDS"),
                _diff(0x038A, "Data\\Textures\\Common\\Plastic\\Plate01_opacity.DDS"),
                _diff(0x01F4, "Menu_Plate_TextureSet3"),
                _diff(0x038A, "Data\\Textures\\Common\\Metal\\MetalCopperAged01_color.DDS"),
                _diff(0x01F4, "Menu_Plate_TextureSet8"),
                _diff(0x038A, "Data\\Textures\\Decals\\Logos\\UC\\UnitedColoniesLogo_01_opacity.DDS"),
                _diff(0x038A, "Data\\Textures\\Common\\Metal\\MetalDullGeneric02_color.DDS"),
                # An empty stub set (the database ships TextureSet14 with no
                # records at all for Tomisar's coat) must never shadow the
                # number that carries the maps.
                _diff(0x01F4, "Stub_Prop_TextureSet1"),
                _diff(0x01F4, "Stub_Prop_TextureSet2"),
                _diff(0x038A, "Data\\Textures\\Common\\Plastic\\Stub_Prop_color.DDS"),
                # Among sets that both name the material, the complete one wins:
                # a companion-map-only record must not beat the colour.
                _diff(0x01F4, "Stencil_Decal_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Common\\Decal\\Stencil_Decal_opacity.DDS"),
                _diff(0x01F4, "Stencil_Decal_TextureSet2"),
                _diff(0x038A, "Data\\Textures\\Common\\Decal\\Stencil_Decal_color.DDS"),
                # The game does not spell albedo one way: every canned-beverage
                # set ships "_BaseColor.dds", which the old "_color.dds" test
                # rejected outright, so the whole family drew untextured.
                _diff(0x01F4, "Canned_Drink_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Items\\Food\\Canned\\Canned_Drink_BaseColor.dds"),
                _diff(0x01F4, "Canned_Drink_TextureSet5"),
                _diff(0x038A, "Data\\Textures\\Common\\Grunge\\GrungeOilSlick01_color.DDS"),
                # One set number seen twice is one split set, not two variants:
                # its records are unioned, which is what keeps
                # decallabelsletters03's authored colour.
                _diff(0x01F4, "Decal_Letters_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Common\\Decal\\Decal_Letters_opacity.DDS"),
                _diff(0x01F4, "Decal_Letters_TextureSet1"),
                _diff(0x038A, "Data\\Textures\\Common\\Decal\\Decal_Letters_color.DDS"),
                _diff(0x038A, "Data\\Textures\\Common\\Decal\\Decal_Letters_opacity.DDS"),
                _diff(0x01F4, "Bare_Prop_TextureSet1"),
                _diff(0x01F4, "Bare_Prop_TextureSet2"),
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            cdb = Path(directory) / "materialsbeta.cdb"
            cdb.write_bytes(blob)
            index = build_cdb_index(cdb)

        self.assertEqual(index["menu_plate"], {
            "color": "textures/common/plastic/plate01_color.dds",
            "opacity": "textures/common/plastic/plate01_opacity.dds",
        })
        # Neither borrowed field may appear anywhere in the verdict.
        self.assertNotIn("copper", repr(index["menu_plate"]))
        self.assertNotIn("unitedcolonieslogo", repr(index["menu_plate"]))
        self.assertEqual(index["stub_prop"], {
            "color": "textures/common/plastic/stub_prop_color.dds",
        })
        self.assertEqual(index["stencil_decal"], {
            "color": "textures/common/decal/stencil_decal_color.dds",
        })
        self.assertEqual(index["canned_drink"], {
            "color": "textures/items/food/canned/canned_drink_basecolor.dds",
        })
        self.assertEqual(index["decal_letters"], {
            "color": "textures/common/decal/decal_letters_color.dds",
            "opacity": "textures/common/decal/decal_letters_opacity.dds",
        })
        # Empty sets stay in the index: an authored empty set is the game's
        # statement that the material is flat shaded, and dropping it would
        # hand those materials back to the fuzzy scorer.
        self.assertEqual(index["bare_prop"], {})

        # The shipped database must agree: the plate material is flat, and the
        # Blend can's own BaseColor map is what the whole Menuitem family needs.
        cdb = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        shipped = build_cdb_index(cdb)
        self.assertEqual(shipped["plasticplainsmooth02"], {})
        self.assertEqual(shipped["menuitem_blend"]["color"],
                         "textures/items/fooddrink_singleitems/all_packaged_consumables/"
                         "menuitem_blend/menuitem_blend_basecolor.dds")

    def test_texture_path_records_are_recognised_by_content_not_field_id(self) -> None:
        # The C++ indexer used to gate on fieldId == 0x038A; the census above
        # is why it cannot. Assert the structural rule survives, and that the
        # cache schema was bumped so an index built by the field-id-only build
        # (every Creation set empty) is discarded rather than reused.
        source = (Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp").read_text(
            encoding="utf-8")
        self.assertIn("bool LooksLikeTexturePath(", source)
        self.assertIn("LooksLikeTexturePath(data, textLen)", source)
        self.assertNotIn("if (fieldId == 0x038A", source)
        self.assertIn("constexpr std::uint32_t kCdbSchema = 11;", source)

    def test_weapon_cdb_diffuse_candidate_is_scoped_to_material_family(self) -> None:
        live_model = Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp"
        source = live_model.read_text(encoding="utf-8")
        family_fn = _family_gate_source()
        self.assertIn('"materials/weapons/"', family_fn)
        self.assertIn("kGenericTokens", family_fn)
        self.assertIn('"barrel", "grip", "receiver"', family_fn)
        self.assertIn("Part names are not evidence", family_fn)
        self.assertIn('"foregrip", "handguard"', family_fn)
        self.assertIn("family : familyTokens", family_fn)
        self.assertIn("textureTokens.begin()", family_fn)
        generic_block = re.search(
            r"kGenericTokens\s*\{(?P<body>.*?)\n\s*\};", family_fn, re.S
        )
        self.assertIsNotNone(generic_block, "weapon-family generic token list not found")
        self.assertFalse(_family_matches(family_fn,
            "materials/weapons/regulator/barrel.mat",
            "textures/actors/modela/crimsonfleet/magstorm/barrel_color.dds",
        ))
        self.assertTrue(_family_matches(family_fn,
            "materials/weapons/regulator/barrel.mat",
            "textures/weapons/regulator/regulator_barrel_color.dds",
        ))
        self.assertTrue(_family_matches(family_fn,
            "materials/weapons/breach/breach_stock_tactical.mat",
            "textures/weapons/breach/stock_tactical_color.dds",
        ))
        self.assertFalse(_family_matches(family_fn,
            "materials/weapons/breach/decals.mat",
            "textures/weapons/solstice/ember/solstice_decals_color.dds",
        ))
        self.assertTrue(_family_matches(family_fn,
            "materials/weapons/breach/decals.mat",
            "textures/weapons/breach/decals_color.dds",
        ))
        self.assertFalse(_family_matches(family_fn,
            "materials/weapons/regulator/compensator.mat",
            "textures/weapons/tombstone/mods_tombstone_compensator_color.dds",
        ))
        self.assertTrue(_family_matches(family_fn,
            "materials/weapons/awp_master_shareable/grip.mat",
            "textures/weapons/awp_shareable/grip_color.dds",
        ))
        self.assertIn('texture.find("/awp_shareable/")', family_fn)
        self.assertIn("textureHasWeaponFamily", family_fn)
        self.assertIn('texture.find("/weapons/" + family + "/")', family_fn)
        diffuse_fn = source[source.index("bool FindDiffuse("):source.index("bool NifBaseIsPart(")]
        self.assertIn("rejectCrossFamilyColor", diffuse_fn)
        self.assertIn("rejectCrossFamilyOpacity", diffuse_fn)
        self.assertIn("rejectCrossFamilyMap", diffuse_fn)
        self.assertIn("!CdbWeaponTextureMatchesFamily(a_matPath, cdbColor)", diffuse_fn)
        self.assertIn("resolving within material family", diffuse_fn)

    @staticmethod
    def _family_fn() -> str:
        return _family_gate_source()

    def test_weapon_map_in_a_family_extension_directory_is_accepted(self) -> None:
        """The Coachman materials are filed under Materials\\Weapons\\Coachman\\
        while the game ships their maps in textures/weapons/coachmanshotgun/.

        The literal `/weapons/<family>/` directory requirement rejected all of
        those authored CDB answers on every resolution - the opacity map along
        with the colour - and left the fuzzy scorer to recover the same files.
        """
        family_fn = self._family_fn()
        self.assertIn("WeaponDirNamesFamily", family_fn)
        self.assertIn("WeaponDirNamesFamily(texture, familyTokens)", family_fn)
        self.assertTrue(_family_matches(family_fn,
            "Materials\\Weapons\\Coachman\\Coachman_Grip.mat",
            "textures/weapons/coachmanshotgun/coachman_grip_color.dds",
        ))
        # 'decals_color.dds' carries no family token in its file name: only
        # the directory names the family, which the token rule cannot see.
        self.assertTrue(_family_matches(family_fn,
            "Materials\\Weapons\\Coachman\\Coachman_Decals.mat",
            "textures/weapons/coachmanshotgun/decals_color.dds",
        ))
        # The opacity companion passes through the same gate, so accepting the
        # colour must carry the authored mask with it.
        self.assertTrue(_family_matches(family_fn,
            "Materials\\Weapons\\Coachman\\Coachman_Decals.mat",
            "textures/weapons/coachmanshotgun/decals_opacity.dds",
        ))
        # A nested weapon directory still only counts when its own segment
        # names the family, and the cross-family traps the exact rule was
        # built for stay rejected.
        self.assertFalse(_family_matches(family_fn,
            "materials/weapons/regulator/barrel.mat",
            "textures/weapons/magstorm/barrel_color.dds",
        ))
        self.assertFalse(_family_matches(family_fn,
            "materials/weapons/breach/decals.mat",
            "textures/weapons/solstice/ember/solstice_decals_color.dds",
        ))
        # A file name that merely repeats the family token is not evidence:
        # the directory has to name the family too.
        self.assertFalse(_family_matches(family_fn,
            "materials/weapons/regulator/regulator_barrel.mat",
            "textures/weapons/magstorm/regulator_barrel_color.dds",
        ))

    def test_weapon_family_directory_evidence_is_scoped_and_directional(self) -> None:
        """Unit behaviour of the directory rule: a segment may continue the
        family token by three or more characters, never the reverse, and a
        file name is never evidence."""
        self.assertTrue(_weapon_dir_names_family(
            "textures/weapons/coachmanshotgun/coachman_grip_color.dds", {"coachman"}, 3))
        self.assertTrue(_weapon_dir_names_family(
            "textures/weapons/dlc001/vortextgrenade/lambert1_color.dds", {"vortextgrenade"}, 3))
        self.assertTrue(_weapon_dir_names_family(
            "textures/weapons/coachman/coachman_grip_color.dds", {"coachman"}, 3))
        self.assertFalse(_weapon_dir_names_family(
            "textures/weapons/coachman2/coachman_grip_color.dds", {"coachman"}, 3))
        self.assertFalse(_weapon_dir_names_family(
            "textures/weapons/x/coachman_grip_color.dds", {"coachman"}, 3))
        self.assertFalse(_weapon_dir_names_family(
            "textures/actors/modela/magstorm/barrel_color.dds", {"regulator"}, 3))

    def test_shipped_coachman_sets_are_accepted_for_their_material_family(self) -> None:
        """Real CDB rows: every shipped Coachman texture set is accepted for a
        material under Materials\\Weapons\\Coachman\\, including the decal set's
        authored opacity companion."""
        cdb = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        index = build_cdb_index(cdb)
        sets = {k: v for k, v in index.items() if k.startswith("coachman_")}
        self.assertGreaterEqual(len(sets), 12, "shipped Coachman texture sets went missing")
        family_fn = self._family_fn()
        for name, maps in sorted(sets.items()):
            material = f"materials/weapons/coachman/{name}.mat"
            self.assertIn("coachmanshotgun", maps["color"], name)
            self.assertTrue(_family_matches(family_fn, material, maps["color"]),
                            f"CDB colour answer rejected for {name}")
            if "opacity" in maps:
                self.assertTrue(_family_matches(family_fn, material, maps["opacity"]),
                                f"CDB opacity answer rejected for {name}")
        self.assertEqual(sets["coachman_decals"]["opacity"],
                         "textures/weapons/coachmanshotgun/decals_opacity.dds")

    def test_shipped_weapon_texture_sets_resolve_to_expected_maps(self) -> None:
        cdb = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        index = build_cdb_index(cdb)
        self.assertEqual(index["rattler_receiver"], {
            "color": "textures/weapons/rattler/heater_receiver_color.dds",
            "opacity": "textures/weapons/rattler/heater_receiver_opacity.dds",
        })
        self.assertEqual(index["rattler_slideandlower"]["color"],
                         "textures/weapons/rattler/heater_slideandlower_color.dds")
        self.assertEqual(index["rattler_decals"]["color"],
                         "textures/weapons/rattler/heater_decals_color.dds")
        self.assertEqual(index["breach_receiver"]["color"],
                         "textures/weapons/breach/breach_receiver_color.dds")
        self.assertEqual(index["solstice_receiver"]["color"],
                         "textures/weapons/solstice/solstice_color.dds")
        # The shipped database contains an authored Regulator decal texture
        # set and its opacity companion. Its real color/opacity paths verify
        # CDB ownership; the screenshot alone does not justify mapping a
        # different receiver/barrel material by heuristic.
        self.assertEqual(index["regulator_decal"], {
            "color": "textures/weapons/regulator/regulator_decals_color.dds",
            "opacity": "textures/weapons/regulator/regulator_decals_opacity.dds",
        })
        # Duplicate generic Decals_TextureSet1 names are first-wins in the
        # runtime index. Razorback's later duplicate is authored correctly,
        # but material-path lookup alone would return Solstice Ember's atlas.
        self.assertEqual(index["decals"], {
            "color": "textures/weapons/solstice/ember/solstice_decals_color.dds",
            "opacity": "textures/weapons/solstice/ember/solstice_decals_opacity.dds",
        })
        # The duplicate first-wins set is deliberately wrong for other
        # weapons until the live family guard rejects it and fuzzy-scopes.
        family_fn = _family_gate_source()
        for family in ("breach", "bridger", "microgun", "razorback", "rocketlauncher", "urbaneagle"):
            self.assertFalse(_family_matches(
                family_fn,
                f"materials/weapons/{family}/decals.mat",
                index["decals"]["color"],
            ), f"generic decals key must not borrow Solstice art for {family}")

        # The binary CDB stores Data-prefixed paths with backslashes and
        # original casing. These raw-byte checks prove Razorback's later
        # duplicate record really carries its authored pair; index lookup is
        # intentionally first-wins and therefore reports Solstice above.
        raw_cdb = cdb.read_bytes().lower()
        self.assertIn(b"data\\textures\\weapons\\razorback\\decals_color.dds", raw_cdb)
        self.assertIn(b"data\\textures\\weapons\\razorback\\decals_opacity.dds", raw_cdb)
        for family in ("breach", "bridger", "microgun", "razorback", "rocketlauncher", "urbaneagle"):
            self.assertFalse(_family_matches(
                family_fn,
                f"materials/weapons/{family}/decals.mat",
                index["decals"]["color"],
            ), f"generic decals key must not borrow Solstice art for {family}")

    def test_legendary_picks_survive_resolution_and_attempt_mod_delivery(self) -> None:
        shop_source = Path(__file__).resolve().parents[2] / "src" / "ShopExplorer.cpp"
        shop = shop_source.read_text(encoding="utf-8")
        add_fn = shop[shop.index("void ShopBrowser::ConfirmAdd()"):shop.index("void ShopBrowser::ConfirmBasket()")]
        basket_fn = shop[shop.index("void ShopBrowser::ConfirmBasket()"):shop.index("void ShopBrowser::RefreshKeywords()")]
        picks_fn = shop[shop.index("std::vector<std::uint32_t> SelectedOmodsFor("):
                         shop.index("std::vector<std::uint32_t> SelectedOmodsFor(") + 1200]
        # The pick lookup moved into SelectedOmodsFor when armor rarity rolls
        # joined it; ConfirmAdd must still snapshot through that resolver rather
        # than re-reading the map at checkout. Both halves are asserted so the
        # add-time capture invariant stays guarded by this test.
        self.assertIn("SelectedOmodsFor(form)", add_fn)
        self.assertIn("g_assemblyPicks.find(formID)", picks_fn)
        self.assertIn("g_armorRarity.find(formID)", picks_fn)
        self.assertIn("entry.selectedOmods == selectedOmods", add_fn)
        self.assertIn("std::vector<std::uint32_t> selectedOmods;", basket_fn)
        self.assertIn("entry.selectedOmods });", basket_fn)
        self.assertIn("item.selectedOmods", basket_fn)
        self.assertIn("SpikeDeliverWithMods(player, item.bound, item.count, picks)", basket_fn)
        self.assertIn("SpikeOutcome::kOnFloorMods", basket_fn)
        self.assertIn("selected mods attached; pick it up", basket_fn)
        self.assertIn("mod route failed; check floor/inventory", basket_fn)
        self.assertIn("SpikeOutcome::kInventoryAddFailed", basket_fn)
        self.assertIn("AddObjectToContainer(item.bound", basket_fn)  # fallback when attach was not attempted

    def test_cache_harness_schema_assertion_tracks_live_schema_once(self) -> None:
        harness = (Path(__file__).resolve().parents[1] / "renderer_recon" / "cdb_cache_harness.cpp").read_text(encoding="utf-8")
        live_model = (Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp").read_text(encoding="utf-8")
        # 8 identifies texture paths by content instead of the base game's
        # 0x038A field id, so every pre-8 index (which held an empty colour
        # for every Creation set) must be rejected and rebuilt.
        # 9 reduces numbered texture-set variants to one whole donor and reads
        # an albedo by its role token, so a v8 index (frankenstein sets such as
        # plasticplainsmooth02, and an empty colour for every "_BaseColor.dds"
        # material) must be discarded the same way.
        # 10 groups override colours and layer kinds NAME-FIRST, so every v9
        # index holds its alphas and tints one object early.
        # 11 indexes a planet's `_terrain` layer as its material, so a v10
        # index has no key for the desktop globes and would keep them grey.
        self.assertEqual(harness.count('CdbIndexStamp().find(";s11;")'), 1)
        self.assertIn("constexpr std::uint32_t kCdbSchema = 11;", live_model)

    def test_creation_databases_are_not_capped_out_of_the_merge(self) -> None:
        # The official Creations live in the game's own Data and sort LAST in
        # g_ba2List, so a small blob cap is spent before they are reached and
        # their weapons silently lose every authored map. The cap must bound
        # the decompressed bytes, not stop at a count the population exceeds.
        live_model = (Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("constexpr std::size_t kMaxModCdbs = 256;", live_model)
        self.assertIn("constexpr std::uint64_t kMaxModCdbBytes = 512ull << 20;", live_model)
        self.assertIn("blobBytes >= kMaxModCdbBytes", live_model)
        # A wrong answer here is silent, so every database is named in the log.
        self.assertIn("cdb: indexed '{}' ({} new texture sets, {} override colours, {} bytes)", live_model)

    def test_screenshot_weapon_maps_match_shipped_cdb_records(self) -> None:
        cdb = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        index = build_cdb_index(cdb)
        self.assertEqual(index["rattler_receiver"]["color"], "textures/weapons/rattler/heater_receiver_color.dds")
        self.assertEqual(index["rattler_slideandlower"]["color"], "textures/weapons/rattler/heater_slideandlower_color.dds")
        self.assertEqual(index["rattler_decals"]["color"], "textures/weapons/rattler/heater_decals_color.dds")
        self.assertEqual(index["regulator_decal"], {
            "color": "textures/weapons/regulator/regulator_decals_color.dds",
            "opacity": "textures/weapons/regulator/regulator_decals_opacity.dds",
        })
        source = (Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp").read_text(encoding="utf-8")
        self.assertIn('normalizedMat == "materials/weapons/regulator/regulator_decal.mat"', source)
        self.assertIn('textures/weapons/regulator/regulator_decals_color.dds', source)

    def test_razorback_decals_material_uses_later_authored_razorback_atlas(self) -> None:
        cdb = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        index = build_cdb_index(cdb)
        self.assertEqual(index["decals"], {
            "color": "textures/weapons/solstice/ember/solstice_decals_color.dds",
            "opacity": "textures/weapons/solstice/ember/solstice_decals_opacity.dds",
        })
        source = (Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp").read_text(encoding="utf-8")
        diffuse_fn = source[source.index("bool FindDiffuse("):source.index("bool NifBaseIsPart(")]
        razorback_at = diffuse_fn.index('normalizedMat == "materials/weapons/razorback/decals.mat"')
        grip_route = diffuse_fn.index('normalizedNode.find("decals_grip")')
        standard_route = diffuse_fn.index('if (normalizedMat == "materials/weapons/razorback/decals.mat")')
        self.assertIn('textures/weapons/razorback/decals_color.dds', diffuse_fn[grip_route:standard_route])
        self.assertNotIn('textures/weapons/razorback/fancy/decals_color.dds', diffuse_fn[grip_route:standard_route])
        self.assertIn('textures/weapons/razorback/decals_opacity.dds', diffuse_fn[grip_route:standard_route])
        self.assertNotIn('textures/weapons/razorback/fancy/decals_opacity.dds', diffuse_fn[grip_route:standard_route])
        self.assertIn('textures/weapons/razorback/decals_color.dds', diffuse_fn[standard_route:standard_route + 700])
        self.assertIn('textures/weapons/razorback/decals_opacity.dds', diffuse_fn[standard_route:standard_route + 700])
        self.assertLess(standard_route, diffuse_fn.index("FindCdbMaterialLocked(a_matPath"))
        self.assertIn('Decals_Grip', source)

        # Verify the actual grip mesh UV rectangle against both source atlas
        # decodes: standard Decals has the wood medallion visible in the
        # inventory reference, while Fancy has a pale badge. This protects
        # the route from a name-only assertion that never checks the pixels.
        from PIL import Image
        assets = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted"
        color = Image.open(assets / "correct_textures_weapons_razorback_decals_color.png").convert("RGB")
        fancy = Image.open(assets / "correct_textures_weapons_razorback_fancy_decals_color.png").convert("RGB")
        uv = (0.253, 0.852, 0.460, 0.889)
        def center_rgb(image):
            x0, x1 = round(uv[0] * image.width), round(uv[1] * image.width)
            y0, y1 = round(uv[2] * image.height), round(uv[3] * image.height)
            return image.getpixel(((x0 + x1) // 2, (y0 + y1) // 2))
        wood = center_rgb(color)
        emblem = center_rgb(fancy)
        self.assertGreater(sum(wood), 100, "standard atlas should sample its visible wood disc")
        self.assertGreater(sum(emblem), 500, "Fancy atlas UV should sample its pale badge")
        self.assertGreater(sum(emblem) - sum(wood), 250, "the Fancy atlas should be visibly brighter than the wood medallion")
        standard_mask = Image.open(assets / "correct_textures_weapons_razorback_decals_opacity.png").convert("L")
        fancy_mask = Image.open(assets / "correct_textures_weapons_razorback_fancy_decals_opacity.png").convert("L")
        self.assertGreater(standard_mask.getpixel(((round(uv[0] * standard_mask.width) + round(uv[1] * standard_mask.width)) // 2,
                                                   (round(uv[2] * standard_mask.height) + round(uv[3] * standard_mask.height)) // 2)),
                           200, "standard opacity map must cover the grip emblem region")
        self.assertEqual(fancy_mask.getpixel(((round(uv[0] * fancy_mask.width) + round(uv[1] * fancy_mask.width)) // 2,
                                               (round(uv[2] * fancy_mask.height) + round(uv[3] * fancy_mask.height)) // 2)),
                         0, "Fancy opacity map is blank in the grip UV region")

    def test_playing_card_symbol_material_uses_its_real_atlas_not_the_cdb_neighbor(self) -> None:
        live_model = Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp"
        source = live_model.read_text(encoding="utf-8")
        diffuse_fn = source[source.index("bool FindDiffuse("):source.index("bool NifBaseIsPart(")]
        override_at = diffuse_fn.index("authored playing-card symbols atlas")
        self.assertIn("exoticplayingcard_symbols.mat", diffuse_fn[:override_at])
        self.assertIn("exoticplayingcard_symbols_decal_color.dds", diffuse_fn[:override_at])
        self.assertIn("exoticplayingcard_symbols_decal_opacity.dds", diffuse_fn[:override_at])
        self.assertLess(
            override_at,
            diffuse_fn.index("FindCdbMaterialLocked(a_matPath"),
        )

    def test_crucible_weapon_maps_are_explicit_and_do_not_depend_on_cdb_guessing(self) -> None:
        source = (Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        diffuse_fn = source[source.index("bool FindDiffuse("):source.index("bool NifBaseIsPart(")]
        expected = {
            "cruciblehilt.mat": "textures/weapons/kgcdoom/crucible/crucible_color.dds",
            "crucible_edge_flow.mat": "textures/weapons/kgcdoom/crucible/crucible_blade_color.dds",
            "crucible_test_flicker.mat": "textures/weapons/kgcdoom/crucible/crucible_blade_graid_color.dds",
        }
        for material, texture in expected.items():
            with self.subTest(material=material):
                route = diffuse_fn.index(material)
                self.assertIn(texture, diffuse_fn[route:route + 500])
                self.assertLess(route, diffuse_fn.index("FindCdbMaterialLocked(a_matPath"))
        self.assertIn('"textures/weapons/kgcdoom/crucible/crucible_blade_energy_opacity.dds"', diffuse_fn)
        self.assertIn('"textures/weapons/kgcdoom/crucible/crucible_blade_graid_emissive.dds"',
                      (Path(__file__).resolve().parents[2] / "src" / "OfflineModel.cpp").read_text(encoding="utf-8-sig"))

    def test_book_cover_tints_come_only_from_the_book_own_record(self) -> None:
        # BookLarge02's NIF requests GenBookB01_BookB01.mat. Name-first reading
        # shows that Material1 writes only its id: no override at all, so there
        # is no blue-gray tint to apply and the old expectation (0.29/0.39/0.53)
        # was the record serialized before the book's name. Note 02's sibling,
        # GenBookB01_BookB04.mat_Material1, DOES carry its own tint - that is
        # the case the bare-stem/data-prefixed fallback exists for.
        overrides = scan_cdb_overrides()
        self.assertNotIn("genbookb01_bookb01", overrides)
        book_b04 = overrides["data/materials/setdressing/books/genbookb01_bookb04.mat"]
        self.assertEqual(book_b04, (
            0.42282700538635254,
            0.43858999013900757,
            0.4135200083255768,
            0.0,
        ))
        live_model = Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp"
        source = live_model.read_text(encoding="utf-8")
        tint_fn = source[source.index("bool FindCdbTint("):source.index("bool FindDiffuse(")]
        self.assertIn("ov = tryKey(bareStem);", tint_fn)
        # The book's record is stored under its full data/materials path, so the
        # lookup must ask for that spelling too or the tint never resolves.
        self.assertIn('tryKey("data/" + path)', tint_fn)

    def test_note_and_card_material_overrides_are_narrow_and_precede_fuzzy_resolution(self) -> None:
        live_model = Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp"
        source = live_model.read_text(encoding="utf-8")
        diffuse_fn = source[source.index("bool FindDiffuse("):source.index("bool NifBaseIsPart(")]

        # The book substitutions are mesh-node scoped; ordinary book meshes
        # must not receive the Captain Galaxy note's neighboring art.
        self.assertIn('normalizedNode.find("book01") != std::string::npos', diffuse_fn)
        self.assertIn('genbookb01_bookb04.mat', diffuse_fn)
        self.assertIn('genbookb01_pagesb01.mat', diffuse_fn)
        self.assertIn('decalletterlineoverlay01_patternf.mat', diffuse_fn)
        self.assertIn('textures/setdressing/books/genbooka01_color.dds', diffuse_fn)
        self.assertIn('textures/effects/gobos/lift02_color.dds', diffuse_fn)

        # Preserve the poster and authored symbols textures, but keep the
        # CDB-corrupt plastic backing neutral. Trim uses its actual CDB color.
        self.assertIn('exoticplayingcard_plastic01.mat', diffuse_fn)
        self.assertIn('exoticplayingcardbox01_trim_color02.mat', diffuse_fn)
        self.assertIn('textures/common/metal/metalpainted01_color.dds', diffuse_fn)
        self.assertLess(diffuse_fn.index('authored playing-card symbols atlas'),
                        diffuse_fn.index('FindCdbMaterialLocked(a_matPath'))
        self.assertLess(diffuse_fn.index('FindCdbMaterialLocked(a_matPath'),
                        diffuse_fn.index('card non-symbol surface kept neutral'))
        self.assertEqual(diffuse_fn.count('if (FindCdbMaterialLocked(a_matPath'), 1)

        # The no-texture material still reaches the standard CDB flat verdict;
        # it is tinted from its own Material1 record rather than borrowing an atlas.
        self.assertIn('GenBookB01_BookB01_Material1', '\n'.join(
            name for name, _ in __import__('sys').modules['cdb_records'].objects(
                __import__('sys').modules['cdb_schema'].load())))

    def test_shared_layer_bases_do_not_become_garment_albedo(self) -> None:
        # Ryujin R&D's upper body and Tomisar's coat are authored as Material1
        # = a common/textile layer floor (insulated foam, bare silk) with the
        # garment's own art blended on top, and BOTH live in the same database
        # as numbered TextureSet variants of one stem: TextureSet1 is the
        # garment's own art, a higher number is the shared floor. The index used
        # to merge variants field by field in file order, so the floor's colour
        # won and the preview painted the coat white with silk stripes - the
        # plugin could only rescue it by holding the layer base back and
        # falling to the item's family art. TextureSet1 now wins that stem
        # outright, which is the same picture without the rescue.
        cdb = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        index = build_cdb_index(cdb)
        cases = {
            "outfit_varuun_ambassador_upperbody_tomisar":
                "textures/clothes/outfit_varuun_ambassador/outfit_varuun_ambassador_upperbody_tomisar_color.dds",
            "outfit_sciencelabtec_ryujin_upperbody_02":
                "textures/clothes/outfit_sciencelabtec_01/outfit_sciencelabtec_ryujin_upperbody_02_color.dds",
        }
        for material, own_art in cases.items():
            with self.subTest(material=material):
                # The garment's own art, never the common floor it is layered
                # over (textures/common/textile/... ).
                self.assertEqual(index[material]["color"], own_art)
                keys = _archive_texture_keys(["Starfield - Textures0*.ba2", "ShatteredSpace - Textures.ba2"])
                self.assertIn(own_art, keys)

        # A mask must come from the same set as the colour it is cut with. The
        # layer floor's cutout belongs to the floor mesh; pairing it with the
        # item's own art would blend one mesh's holes over another's UVs.
        sciencelab = index["outfit_sciencelabtec_ryujin_upperbody_02"]
        self.assertTrue(
            sciencelab["opacity"].endswith("outfit_sciencelabtec_ryujin_upperbody_01_opacity.dds"))
        colour_dir = sciencelab["color"].rsplit("/", 1)[0]
        self.assertEqual(sciencelab["opacity"].rsplit("/", 1)[0], colour_dir)

        # The hold-back stays: a garment whose database answer really is a
        # common layer base still has to defer to its family art.
        source = (Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("bool CdbColorIsGarmentLayerBase(const std::string& a_matPath, const std::string& a_texturePath)", source)
        self.assertIn('t.rfind("textures/common/", 0) == 0', source)
        # Scoped to clothes on purpose: a common map outside garments is often
        # the correct whole answer and must keep its direct route.
        self.assertIn('m.find("materials/clothes/")', source)
        diffuse_fn = source[source.index("bool FindDiffuse("):source.index("bool NifBaseIsPart(")]
        # Held back before the direct return, then used only after the family
        # singleton fallback has had its chance.
        held = diffuse_fn.index("CdbColorIsGarmentLayerBase(a_matPath, cdbColor)")
        direct = diffuse_fn.index("cdb material map '{}")
        deferred = diffuse_fn.index("cdb garment layer base '{}' -> {} (no family art)")
        self.assertLess(held, direct)
        self.assertLess(direct, deferred)

    def test_empty_part_sets_inherit_their_family_art(self) -> None:
        # The Miner Utility Outfit's shirt, sleeves and pants resolve to empty
        # texture sets while the suit family names the only atlas, under
        # Bethesda's misspelled 'outfit_miner_utililtysuit' folder - no
        # name-based matcher can reach it, so the part drew as bare geometry.
        cdb = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        index = build_cdb_index(cdb)
        for part in ("outfit_miner_utilitysuitf_upperbody", "outfit_miner_utilitysuit_sleeves",
                     "outfit_miner_utilitysuit_lowerbody"):
            self.assertEqual(index[part], {}, f"{part} is expected to be an authored empty set")
        family = index["outfit_miner_utilitysuit"]["color"]
        self.assertEqual(
            family,
            "textures/clothes/outfit_miner_utililtysuit/outfit_miner_utilitysuit_m/"
            "outfit_miner_utilitysuit_shirt_materials_color.dds",
        )
        self.assertIn(family, _archive_texture_keys(["Starfield - Textures0*.ba2"]))

        source = (Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        lookup_fn = source[source.index("bool FindCdbMaterialLocked("):source.index("std::vector<ColorEntry> g_opacs;")]
        self.assertIn("set->color.empty() && set->opacity.empty()", lookup_fn)
        self.assertIn("cdb family set '{}' supplies '{}' (part set is empty)", lookup_fn)
        self.assertLess(lookup_fn.index("set->color.empty() && set->opacity.empty()"),
                        lookup_fn.index("a_outColor = set->color;"))

    def test_garment_override_alpha_is_a_layer_weight_not_opacity(self) -> None:
        # The Chunks cap's fabric material was recorded as storing
        # (1,1,1,8/255), and at 3% opacity the cap body vanished leaving its
        # logo decal floating. Name-first reading refutes the value - the cap's
        # own Material1 writes (0,0,0,0) and the 8/255 belonged to the record
        # before its name - so the cap is opaque because the database gives it
        # no opacity. The guard stays: garment layer weights are a real class of
        # sub-floor alpha in mod databases, and it must still run before any
        # CDB alpha is accepted (asserted below).
        alpha = scan_cdb_overrides()["outfit_baseball_cap_01"]
        self.assertEqual(alpha, (0.0, 0.0, 0.0, 0.0))

        offline = (Path(__file__).resolve().parents[2] / "src" / "OfflineModel.cpp").read_text(encoding="utf-8-sig")
        blend_fn = offline[offline.index("float MaterialBlendAlpha("):offline.index("bool BuildGpuModel(")]
        self.assertIn("layer weight, not surface opacity", blend_fn)
        self.assertIn('ml.find("materials/clothes/")', blend_fn)
        # The garment guard has to run before the CDB alpha is accepted, else
        # the cap is drawn at 3% again. It also covers the measured consumable
        # weights now (Tranquilitea 155/255, Velocity beverage 128/255).
        self.assertLess(blend_fn.index("(garment || consumableItem) && !garmentSheerToken"),
                        blend_fn.index("a_translucent = true;"))

    def test_cross_family_opacity_is_not_adopted(self) -> None:
        """A mask from another family is not this set's mask.

        PackagedFood_Cheese owns cheese_color.dds and inherited
        DrinkPack_SingleBarrelBourbon's _opacity map from the record walk that
        follows it, so the cheese block was drawn through a bourbon wrapper's
        cutout - the reported "Cheese still looks the same".
        """
        from material_sweep import build_cdb_index, opacity_shares_family

        self.assertFalse(opacity_shares_family(
            "packagedfood_cheese",
            "textures/items/fooddrink_singleitems/all_packaged_consumables/cheese/cheese_color.dds",
            "textures/items/fooddrink_singleitems/all_packaged_consumables/drinkpack_singlebarrelbourbon/drinkpack_singlebarrelbourbon_opacity.dds",
        ))
        # The genuine pairing still passes: same item, same family.
        self.assertTrue(opacity_shares_family(
            "erdebrau",
            "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau pilsner_decal_color.dds",
            "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau pilsner_decal_opacity.dds",
        ))

        cdb = Path(__file__).resolve().parents[1] / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        if not cdb.exists():
            self.skipTest("materialsbeta.cdb extract not present")
        index = build_cdb_index(cdb)

        cheese = index.get("packagedfood_cheese", {})
        self.assertTrue(cheese.get("color", "").endswith("cheese_color.dds"), cheese)
        self.assertNotIn("opacity", cheese, "the bourbon wrapper mask must not reach the cheese block")
        # The rule is narrow: it drops only cross-family pairs.
        paired = sum(1 for v in index.values() if v.get("color") and v.get("opacity"))
        self.assertGreater(paired, 22000, "valid colour/opacity pairs must survive")

        live = (Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("dropped cross-family opacity", live)
        self.assertIn("!sharesToken(stemOf(set->color), stemOf(*a_outOpacity))", live)

    def test_tranquilitea_cup_sleeve_uses_the_cup_print(self) -> None:
        # The family ships one cup atlas and no CDB set for the sleeve, so the
        # family scorer handed back the wall sign (tranquilitea_sign01).
        live = (Path(__file__).resolve().parents[2] / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        self.assertIn(
            'normalizedMat == "materials/setdressing/tranquilitea/tranquilitea_togocup/tranquilitea_togocup_sleeve01.mat"',
            live,
        )
        self.assertIn("textures/setdressing/tranquilitea/tranquilitea_logo01_color.dds", live)


if __name__ == "__main__":
    unittest.main()
