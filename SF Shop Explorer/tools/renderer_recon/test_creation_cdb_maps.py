"""Data test: a Creation's own database must answer for its own materials.

`EnsureModCdbIndex` merges every `materials/creations/<name>/materialsbeta.cdb`
it can find, and it has to actually reach them: the official Creations live in
the game's own Data and sort last in `g_ba2List`, so a small blob cap is spent
before they are reached. When that happens the fuzzy scorer answers instead and
returns a plausible sibling of the same weapon, which is invisible in the log.

The fixtures come from `extract_creation_cdb.py`. They are the shipped bytes,
so these assertions pin what the game itself says, not a paraphrase of it:

    SFBGS050  breechblock_receiver_2 -> receiver_2_color.dds
              (the scorer handed back breechblock_mods_color.dds, mean
               RGB 69/70/69 against the authored 115/99/95)

    python test_creation_cdb_maps.py
"""
from __future__ import annotations

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "ba2_audit"))
import cdb_mods as M  # noqa: E402

BASE_CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")
CREATION_DIR = os.path.join(HERE, "extracted", "creations")
# Archive order the plugin scans in: everything in the game's Data, sorted by
# path, which puts the official Creations before Shattered Space.
CREATION_ORDER = ["sfbgs003", "sfbgs008", "sfbgs00d", "sfbgs047", "sfbgs050", "shatteredspace"]


def merged_index() -> dict:
    """Base game first, then each Creation database, first writer wins.

    This is `IndexCdbBlob`'s precedence policy: both passes only ever fill a
    gap, so whichever blob is indexed first owns the key.
    """
    with open(BASE_CDB, "rb") as fh:
        sets = M.pass1(fh.read())
    for stem in CREATION_ORDER:
        path = os.path.join(CREATION_DIR, stem + ".cdb")
        if not os.path.exists(path):
            continue
        with open(path, "rb") as fh:
            for key, value in M.pass1(fh.read()).items():
                sets.setdefault(key, value)
    return sets


def resolve(sets: dict, mat_path: str):
    """`FindCdbMaterialLocked`'s key order: path, data/path, stem, family base."""
    path = mat_path.lower().replace("\\", "/").lstrip("/")
    stem = path.rsplit("/", 1)[-1]
    if stem.endswith(".mat"):
        stem = stem[:-4]
    for key in (path, "data/" + path, stem):
        if key in sets:
            return sets[key]
    base = stem.rstrip("0123456789").rstrip("_")
    if base and base != stem and base in sets:
        return sets[base]
    return None


class CreationCdbMapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        missing = [p for p in (BASE_CDB, os.path.join(CREATION_DIR, "sfbgs050.cdb")) if not os.path.exists(p)]
        if missing:
            raise unittest.SkipTest("fixtures absent: run extract_creation_cdb.py --extract (%s)" % ", ".join(missing))
        cls.sets = merged_index()

    def test_creation_database_answers_for_its_own_material(self) -> None:
        # The whole point: a key that exists only in SFBGS050's database.
        self.assertIn("breechblock_receiver_2", self.sets)
        self.assertEqual(
            self.sets["breechblock_receiver_2"]["color"],
            "textures/weapons/sfbgs050/breechblock/receiver_2_color.dds",
        )

    def test_breechblock_material_paths_resolve_to_the_authored_maps(self) -> None:
        expected = {
            "materials/weapons/sfbgs050/breechblock/breechblock_receiver_2.mat":
                "textures/weapons/sfbgs050/breechblock/receiver_2_color.dds",
            "materials/weapons/sfbgs050/breechblock/breechblock_receiver_1.mat":
                "textures/weapons/sfbgs050/breechblock/receiver_1_color.dds",
            "materials/weapons/sfbgs050/breechblock/breechblock_grip.mat":
                "textures/weapons/sfbgs050/breechblock/grip_color.dds",
            "materials/weapons/sfbgs050/breechblock/breechblock_barrel.mat":
                "textures/weapons/sfbgs050/breechblock/barrel_color.dds",
            "materials/weapons/sfbgs050/breechblock/breechblock_mag.mat":
                "textures/weapons/sfbgs050/breechblock/mag_color.dds",
            "materials/weapons/sfbgs050/breechblock/breechblock_mods.mat":
                "textures/weapons/sfbgs050/breechblock/breechblock_mods_color.dds",
        }
        for mat, color in expected.items():
            with self.subTest(mat=mat):
                got = resolve(self.sets, mat)
                self.assertIsNotNone(got, "database has no set for %s" % mat)
                self.assertEqual(got["color"], color)

    def test_sibling_atlases_the_scorer_returned_are_not_the_authored_answers(self) -> None:
        # These two substitutions are exactly what the shop preview showed:
        # the receiver and the grip wore the *mods* atlas of the same weapon.
        receiver = resolve(self.sets, "materials/weapons/sfbgs050/breechblock/breechblock_receiver_2.mat")
        grip = resolve(self.sets, "materials/weapons/sfbgs050/breechblock/breechblock_grip.mat")
        self.assertNotIn("breechblock_mods_color.dds", receiver["color"])
        self.assertNotIn("breechblock_mod_grip_color.dds", grip["color"])
        # ...and both siblings do exist as maps of their own, which is what
        # made the wrong answer look plausible.
        self.assertEqual(self.sets["breechblock_mods"]["color"],
                         "textures/weapons/sfbgs050/breechblock/breechblock_mods_color.dds")
        self.assertEqual(self.sets["breechblock_mods_grip"]["color"],
                         "textures/weapons/sfbgs050/breechblock/breechblock_mod_grip_color.dds")

    def test_decal_material_keeps_its_paired_opacity_mask(self) -> None:
        got = resolve(self.sets, "materials/weapons/sfbgs050/breechblock/breechblock_decals.mat")
        self.assertEqual(got["color"], "textures/weapons/sfbgs050/breechblock/decals_color.dds")
        self.assertEqual(got["opacity"], "textures/weapons/sfbgs050/breechblock/decals_opacity.dds")

    def test_base_game_answer_is_not_shadowed_by_a_creation(self) -> None:
        # Precedence policy: whichever blob is indexed first owns the key, and
        # the base game is always indexed first.
        with open(BASE_CDB, "rb") as fh:
            base = M.pass1(fh.read())
        with open(os.path.join(CREATION_DIR, "sfbgs050.cdb"), "rb") as fh:
            creation = M.pass1(fh.read())
        shared = [k for k in creation if k in base]
        self.assertGreater(len(shared), 100, "too few overlapping keys to check precedence")
        for key in shared:
            with self.subTest(key=key):
                self.assertEqual(self.sets[key], base[key])


if __name__ == "__main__":
    unittest.main()
