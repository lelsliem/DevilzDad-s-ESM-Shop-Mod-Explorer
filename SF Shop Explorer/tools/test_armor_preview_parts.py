"""Guards for the two armor-preview defects in the 2026-10-18 screens.

Both are preview-only defects, so neither can be exercised without the game;
these assertions pin the decisions that fix them instead.

  1. The Chunks cap drew its red logo decal with no cap around it. The cap
     fabric material stores a 8/255 MaterialOverrideColor layer weight, which
     is a composite weight, not surface opacity. MaterialBlendAlpha already
     ignores it for garments - but its path test is written "materials/clothes/"
     while NIF material strings use backslashes and OfflineModel's LowerAscii
     (unlike LiveModel's) does not normalize them, so the garment rule never
     matched and the body drew at 3% alpha.

  2. Apparel previews showed a pair of detached forearms beside the mannequin.
     Those are the skinned Hands_3rd_F parts: they are stored in bind space at
     the rig origin and the engine attaches them to the skeleton at draw time,
     so reading their raw vertices puts them off the body.

Both are pinned as source structure because the fix itself is one predicate and
one prune; a behavioural test would need a running game with the shop open.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OFFLINE = ROOT / "src" / "OfflineModel.cpp"
EXPLORER = ROOT / "src" / "ShopExplorer.cpp"


class GarmentAlphaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        source = OFFLINE.read_text(encoding="utf-8-sig")
        start = source.index("float MaterialBlendAlpha(")
        end = source.index("bool BuildGpuModel(", start)
        cls.body = source[start:end]

    def test_material_paths_are_normalized_before_the_garment_test(self) -> None:
        normalize = self.body.index("std::replace(ml.begin(), ml.end(), '\\\\', '/')")
        garment = self.body.index('ml.find("materials/clothes/")')
        self.assertLess(normalize, garment)

    def test_garment_layer_weight_is_not_surface_opacity(self) -> None:
        # The ignore has to come before the generic "translucent = cdb" return,
        # otherwise the cap resolves to 0.031 and disappears.
        ignored = self.body.index("layer weight, not surface opacity")
        translucent = self.body.index("a_translucent = true;")
        self.assertLess(ignored, translucent)
        self.assertIn("(garment || consumableItem) && !garmentSheerToken", self.body)

    def test_sheer_garment_parts_keep_the_token_fallback(self) -> None:
        self.assertIn('ml.find("visor")', self.body)
        self.assertIn('ml.find("lens")', self.body)


class SkinnedAttachmentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = EXPLORER.read_text(encoding="utf-8-sig")
        start = cls.source.index("void PruneSkinnedAttachmentParts(")
        end = cls.source.index("std::string CategorySearchText(", start)
        cls.prune = cls.source[start:end]

    def test_only_bind_space_skin_and_hand_parts_are_dropped(self) -> None:
        # Token match, not substring: "hands" covers Hands_3rd_F / NakedHands,
        # "skin" covers the bare body skin an outfit's ARMA set carries
        # (Outfit_Civilian_FormalHalfDress_01_Skin_F), which rendered as a bald
        # scalp in Andreja's outfit. Garment names glue their words together
        # (Bodysuit_Dress) so neither token appears by accident.
        self.assertIn('token == "hands"', self.prune)
        self.assertIn('token == "skin"', self.prune)
        self.assertIn("std::isalnum", self.prune)
        # Gloves are full rest-pose meshes (Gloves_F_LOD1) and must survive.
        self.assertNotIn('"gloves"', self.prune)
        self.assertNotIn('find("skin")', self.prune)

    def test_a_hands_only_item_keeps_its_geometry(self) -> None:
        self.assertIn("const bool hasRestPoseBody", self.prune)
        self.assertIn("if (!hasRestPoseBody) {", self.prune)
        self.assertIn("return;", self.prune)

    def test_prune_runs_once_per_form_load_before_geometry_is_queued(self) -> None:
        load = self.source.index("SFE::NativeModel::LoadFromBarterMenu(fid);")
        prune = self.source.index("PruneSkinnedAttachmentParts(s_native);")
        queued = self.source.index("const std::string previewLabel", load)
        self.assertLess(load, prune)
        self.assertLess(prune, queued)



class PreviewFramingTests(unittest.TestCase):
    """The Mark I backpack filled the panel and clipped at the border.

    Camera distance was radius*5/zoom, which frames the bounding SPHERE: at the
    default zoom 2.05 that puts a sphere-bounded silhouette at 0.99 of the frame
    half-extent, so any item whose near face is closer than its centre (a deep
    pack) overflows. FitCamDist fits the projected bounding box instead.
    """

    @classmethod
    def setUpClass(cls) -> None:
        source = OFFLINE.read_text(encoding="utf-8-sig")
        start = source.index("float FitCamDist(")
        end = source.index("void BuildMvp(", start)
        cls.fit = source[start:end]
        cls.source = source

    def test_fit_projects_all_eight_box_corners(self) -> None:
        self.assertIn("for (int corner = 0; corner < 8; ++corner)", self.fit)
        self.assertIn("Rotated(lx, ly, lz,", self.fit)
        self.assertIn("kMargin", self.fit)

    def test_fit_never_frames_tighter_than_the_old_sphere_distance(self) -> None:
        self.assertIn("float dist = a_fallback;", self.fit)
        self.assertIn("dist = std::max(dist,", self.fit)

    def test_fit_margin_clears_the_viewport_hud_strips(self) -> None:
        # The viewport paints two text lines over its top edge and a caption
        # line over the bottom edge. A 0.90 fit still put the bottom of the
        # Mark I pack behind the caption and read as clipping; the margin is
        # therefore 0.80, i.e. 10% of the height clear at each edge.
        self.assertIn("constexpr float kMargin = 0.80f;", self.fit)
        self.assertIn("behind the caption strip", self.fit)

    def test_both_render_paths_use_the_fit(self) -> None:
        self.assertEqual(self.source.count("FitCamDist(a_model, yaw, pitch, a_weapon"), 2)
        self.assertIn("FitCamDist(a_model, yaw, pitch, a_weapon, 5.0f / g_zoom", self.source)
        self.assertIn("FitCamDist(a_model, yaw, pitch, a_weapon,\n\t\t\t\ta_model.radius * 5.0f / g_zoom)", self.source)

    def test_wheel_zoom_still_scales_the_fitted_framing(self) -> None:
        self.assertEqual(self.source.count(") / zoomMul;"), 2)


if __name__ == "__main__":
    unittest.main()
