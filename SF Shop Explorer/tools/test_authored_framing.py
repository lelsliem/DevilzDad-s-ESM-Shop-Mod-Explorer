#!/usr/bin/env python3
"""Guard for authored preview framing ([preview] authoredOrientation=1).

The panel has always composed items from geometry bounds: normalize to a unit
sphere, then apply a hard-coded orientation guess (a cyclic permutation for
weapons, a "lay flat" turn for tall clutter). The engine does something else --
it applies the form's own `BGSTransform` record for the preview context. The
authored data is available in game and, measured offline, slot 4 covers 5,486
of the catalog's authored forms.

The trap this file pins is double-orientation: if the panel rotates the geometry
AND the renderer still applies its own guess, every item is turned twice and
looks worse than before. The rotation and the suppression of the renderer's
guess must travel together, decided by one flag.

These are source-structure assertions; the visual result needs a running game.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHOP = ROOT / "src" / "ShopExplorer.cpp"
OFFLINE = ROOT / "src" / "OfflineModel.cpp"
OFFLINE_H = ROOT / "src" / "OfflineModel.h"


class AuthoredFramingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.shop = SHOP.read_text(encoding="utf-8-sig")
        cls.offline = OFFLINE.read_text(encoding="utf-8-sig")
        cls.offline_h = OFFLINE_H.read_text(encoding="utf-8-sig")

    def test_toggle_is_opt_in_and_reads_its_own_key(self) -> None:
        self.assertIn('GetPrivateProfileIntW(L"preview", L"authoredOrientation", 0,', self.shop)
        self.assertIn("bool AuthoredOrientationEnabled()", self.shop)

    def test_slot_order_is_general_item_first_then_icon(self) -> None:
        # 4 is the general item transform; 0 is the icon transform and is only
        # a fallback. Reading 0 first would frame most items as icons.
        body = self.shop.split("AuthoredPlacement ReadAuthoredPlacement(", 1)[1]
        body = body[: body.index("void RotateAboutCenter")]
        self.assertIn("for (const int slot : { 4, 0 })", body)
        self.assertIn("bound->previewTransform.transforms[slot]", body)

    def test_forms_without_a_record_keep_bounds_framing(self) -> None:
        # 14.7% of catalog forms author nothing (most weapons), so the flag must
        # stay false unless a transform was actually found.
        body = self.shop.split("AuthoredPlacement ReadAuthoredPlacement(", 1)[1]
        body = body[: body.index("void RotateAboutCenter")]
        self.assertIn("out.found = true;", body)
        self.assertIn("if (!transform) continue;", body)
        call = self.shop.split("bool authored = false;", 1)[1]
        call = call[: call.index("RequestPreviewDirect")]
        self.assertIn("if (placement.found) {", call)
        self.assertIn("authored = true;", call)

    def test_weapons_keep_the_confirmed_cyclic_orientation(self) -> None:
        # Measured: 169 of 467 weapons author any preview transform, and the
        # cyclic mapping was confirmed in game. Authored rotation on weapons
        # would regress the other 298, so weapons are excluded by construction.
        call = self.shop.split("bool authored = false;", 1)[1]
        call = call[: call.index("RequestPreviewDirect")]
        self.assertIn("if (!direct.empty() && !isWeapon && AuthoredOrientationEnabled()) {", call)

    def test_rotation_happens_about_the_model_centre(self) -> None:
        # Rotating about the world origin would swing off-centre models out of
        # view; the authored record's position is irrelevant to a normalized
        # preview, only its rotation is reused.
        self.assertIn("void RotateAboutCenter(std::vector<float>& a_pos, const float a_center[3]", self.shop)
        call = self.shop.split("bool authored = false;", 1)[1]
        call = call[: call.index("RequestPreviewDirect")]
        self.assertIn("RotateAboutCenter(d.pos, center, placement.rot);", call)
        self.assertIn("const float center[3]{", call)

    def test_renderer_skips_its_own_guess_when_geometry_is_already_oriented(self) -> None:
        # The double-orientation trap. Both the weapon permutation and layFlat
        # must be behind the same branch.
        body = self.offline.split("float lx = (mesh.pos[v * 3]", 1)[1]
        body = body[: body.index("pos.push_back(lx);")]
        self.assertIn("if (authoredOrientation) {", body)
        self.assertLess(body.index("if (authoredOrientation) {"), body.index("} else if (a_weapon) {"))
        self.assertIn("} else if (layFlat) {", body)

    def test_flag_is_threaded_and_reset_for_the_legacy_path(self) -> None:
        self.assertIn("bool a_authoredOrientation = false", self.offline_h)
        self.assertIn("g_authoredOrientation.store(a_authoredOrientation, std::memory_order_release);", self.offline)
        # A legacy model built after a direct one must not inherit the flag.
        legacy = self.offline.split("void RequestPreview(", 1)[1]
        legacy = legacy[: legacy.index("void RequestPreviewDirect(")]
        self.assertIn("g_authoredOrientation.store(false, std::memory_order_release);", legacy)

    def test_orientation_is_logged_once_per_form(self) -> None:
        call = self.shop.split("bool authored = false;", 1)[1]
        call = call[: call.index("RequestPreviewDirect")]
        self.assertIn("authored framing: {:08X} slot={} rot=", call)
        self.assertIn("if (fid != s_loggedFraming) {", call)


if __name__ == "__main__":
    unittest.main()
