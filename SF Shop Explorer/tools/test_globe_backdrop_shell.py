#!/usr/bin/env python3
"""Guard for the interior-shell rule (the 2026-10-23 snow-globe dome).

The user's report was "glass still transparent? no" with the London globe in
the preview reading as a painted pale dome while the engine's render beside it
showed snow, trees, Big Ben and the London Eye through clear glass. The glass
was not the surface being looked at.

`snowglobe_landmark_london.nif` hangs its specimen inside a backdrop hull
(`Snowglobe_Landmark_Backgrounds01`, node 2) whose faces all point at the
model's centre - measured -0.99 area-weighted - and which covers the dome
(radius 0.101 against the dome's 0.103) and wraps the eight other interior
ranges. This renderer rasterises CULL_MODE_NONE, so that hull was drawn with its
NEAR wall between the eye and everything else: the sky texture painted over the
buildings, and the "glass" the capture showed was the backdrop's near wall.

The engine draws the inside of the hull. A range that is inside-out and wraps
the rest of the model is flagged, and kPsTexSrc clips the half of it whose
normal points away from the eye - the near wall - so the far wall stays behind
the item as the sky.

The direction is the part worth pinning. CULL_MODE_BACK on a dedicated
pipeline was tried first and changed nothing on screen (build 2E401C95, 19:19
session: the plugin logged the hull and the preview looked identical), which
means this renderer's winding keeps the opposite wall to the one the guess
assumed. The clip is therefore read off the interpolated normal instead, and
the test below measures on the shipped mesh that the hull's nearest triangle
points away from the eye and its farthest points at it - while the glass
dome's near wall points at the eye and survives.

    python -m unittest test_globe_backdrop_shell -v
"""
from __future__ import annotations

import math
import os
import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OFFLINE = ROOT / "src" / "OfflineModel.cpp"

sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))

DATA = Path(os.environ.get("SFSE_STARFIELD_DATA", "B:/SteamLibrary/steamapps/common/Starfield/Data"))
ARCHIVES = DATA.is_dir()

# The backdrop material family, not one numbered variant: the Star Wars
# Genesis conversion deleted the London record and ships the same globe as
# "Taris Spaceport Snow Globe" on Snowglobe_Landmark_St.nif, whose hull is
# Snowglobe_Landmark_Backgrounds02. Both variants are the same inside-out shell.
BACKDROP = "Snowglobe_Landmark_Backgrounds"
GLASS = "Snowglobe_Landmarks_Glass"
GLOBE_MODEL = "snowglobe_landmark_"


def globe_item(catalog, plugins):
    """The shipped landmark snow globe, by NIF rather than by display name.

    The display name is plugin copy (the London globe is "Taris Spaceport Snow
    Globe" in the current profile), so resolving by name breaks whenever the
    load order renames or replaces the record. The archive still carries
    snowglobe_landmark_london.nif either way; prefer it when a record names it.
    """
    found = []
    for item in catalog:
        record = plugins.record(item["plugin"], item["form"]) or {}
        model = (record.get("model") or "").lower()
        if GLOBE_MODEL in model:
            found.append((model, item))
    if not found:
        return None
    found.sort(key=lambda pair: "london" not in pair[0])
    return found[0][1]


class InteriorShellRuleTests(unittest.TestCase):
    """The rule in BuildGpuModel, as written."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")

    def test_the_rule_needs_all_three_facts(self) -> None:
        # Inside-out (faces at the centre), large (a hull, not a mug's inner
        # wall) and wrapping other ranges. Any one alone would flag ordinary
        # surfaces: a bowl is inside-out and large, and a plate is neither.
        self.assertIn("if (shell.translucent || shell.buildInward > -0.5f) {", self.source)
        self.assertIn("if (std::max({ halfX, halfY, halfZ }) < 0.5f) {", self.source)
        self.assertIn("if (wrapped < 2) {", self.source)
        self.assertIn("shell.interiorShell = true;", self.source)

    def test_the_flag_reaches_the_build_from_the_mesh(self) -> None:
        # The measurement is in source space, before the preview's own
        # rotation, and rides on the range the draw loop reads.
        self.assertIn("range.buildInward = meshInward;", self.source)
        self.assertIn("range.buildMin[a] = std::min(range.buildMin[a], pos[v * 3 + static_cast<std::size_t>(a)]);",
                      self.source)
        # ...and the flag is written into the constant the shader reads it from.
        self.assertIn("clearShell[2]{ range.clearShell ? 1.0f : 0.0f,", self.source)
        self.assertIn("range.interiorShell ? 1.0f : 0.0f }", self.source)

    def test_the_near_wall_is_clipped_by_its_normal_not_by_winding(self) -> None:
        # CULL_MODE_BACK was the first attempt and changed nothing on screen
        # (build 2E401C95, 19:19 session): it kept the near wall. The clip is
        # therefore geometric - the shader's own interpolated normal against
        # the direction to the eye - and no pipeline culls a side.
        start = self.source.index("static constexpr const char kPsTexSrc[]")
        shader = self.source[start:]
        shader = shader[:shader.index("static constexpr const char k", len("static constexpr const char kPsSrc[]"))]
        self.assertIn("float2 g_shell;", shader)
        gate = shader.index("if (g_shell.y > 0.5)")
        self.assertIn("clip(dot(n, viewDir));", shader[gate:gate + 120])
        # ...and nothing culls a side any more: that path is what did nothing.
        self.assertEqual(self.source.count("D3D12_CULL_MODE_BACK"), 0)

    def test_the_draw_loop_routes_the_hull_to_the_clipping_shader(self) -> None:
        routing = self.source[self.source.index("if (range.interiorShell) {"):]
        routing = routing[:routing.index("a_list->DrawIndexedInstanced")]
        self.assertIn("a_list->SetPipelineState(g_pipe.psoTextured);", routing)
        self.assertLess(routing.index("psoTextured"), routing.index("psoAdditive"))


def backdrop_parts(sheet, geoms: list[dict]) -> list[dict]:
    """Each geometry's world-space triangles and the material it draws with."""
    parts = []
    for geom in geoms:
        mesh, mat = geom.get("mesh"), geom.get("mat")
        if mesh is None or mat is None:
            continue
        raw = mesh["idx"]
        idx = raw[:(raw.size // 3) * 3].reshape(-1, 3)
        pos = np.concatenate([mesh["pos"], np.ones((mesh["nv"], 1), np.float32)], axis=1)
        world = (pos @ geom["world"].astype(np.float32))[:, :3]
        parts.append({"material": mat["mat"], "tris": world[idx]})
    return parts


@unittest.skipUnless(ARCHIVES, "Starfield archives not installed; shipped-mesh geometry not measured")
class ShippedGlobeGeometryTests(unittest.TestCase):
    """The premise, measured on the NIF the game ships."""

    @classmethod
    def setUpClass(cls) -> None:
        import contact_sheet as sheet

        cls.sheet = sheet
        store = sheet.audit.AssetStore([DATA], Path(sheet.audit.MODS_ROOT), False)
        plugins = sheet.audit.Plugins(store)
        swaps = sheet.audit.Swaps(Path(sheet.audit.SWAPS))
        cdb = sheet.audit.Cdb(Path(sheet.audit.CDB))
        textures = sheet.Textures(store)
        catalog = sheet.audit.parse_catalog(sheet.audit.find_catalog(None))
        item = globe_item(catalog, plugins)
        if item is None:
            raise unittest.SkipTest("no landmark snow globe record in this load order")
        geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, {})
        cls.meta = meta
        cls.parts = backdrop_parts(sheet, geoms)
        cls.centre, cls.radius = cls.centre_and_radius(cls.parts)

    @staticmethod
    def centre_and_radius(parts: list[dict]) -> tuple[np.ndarray, float]:
        pts = np.concatenate([p["tris"].reshape(-1, 3) for p in parts], axis=0)
        centre = (pts.min(axis=0) + pts.max(axis=0)) * 0.5
        radius = float(np.max(np.linalg.norm(pts - centre, axis=1)))
        return centre, radius

    def inwardness(self, part: dict) -> float:
        tris = (part["tris"] - self.centre) / self.radius
        fn = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
        length = np.linalg.norm(fn, axis=1)
        keep = length > 1e-12
        fn, length = fn[keep] / length[keep][:, None], length[keep]
        mid = tris[keep].mean(axis=1)
        mid_len = np.linalg.norm(mid, axis=1)
        keep2 = mid_len > 1e-6
        return float(np.average((fn[keep2] * mid[keep2]).sum(axis=1) / mid_len[keep2], weights=length[keep2]))

    def test_the_backdrop_is_inside_out_and_the_dome_is_not(self) -> None:
        by_mat = {p["material"]: p for p in self.parts}
        backdrop = next(p for name, p in by_mat.items() if BACKDROP in name)
        glass = next(p for name, p in by_mat.items() if GLASS in name)
        self.assertLess(self.inwardness(backdrop), -0.5)
        self.assertGreater(self.inwardness(glass), 0.5)
        # The buildings sit near zero: ordinary surfaces must never qualify.
        for name, part in by_mat.items():
            if "Buildings" in name:
                self.assertLess(abs(self.inwardness(part)), 0.5, name)

    def test_the_backdrop_covers_the_dome_and_wraps_the_specimen(self) -> None:
        by_mat = {p["material"]: p for p in self.parts}
        backdrop = next(p for name, p in by_mat.items() if BACKDROP in name)
        glass = next(p for name, p in by_mat.items() if GLASS in name)
        half = (backdrop["tris"].reshape(-1, 3).max(axis=0) - backdrop["tris"].reshape(-1, 3).min(axis=0)) * 0.5
        self.assertGreater(float(half.max()) / self.radius, 0.5)
        dome_half = (glass["tris"].reshape(-1, 3).max(axis=0) - glass["tris"].reshape(-1, 3).min(axis=0)) * 0.5
        # It is the dome's own size, not a small part inside it.
        self.assertGreater(float(half.max()), float(dome_half.max()) * 0.9)
        lo, hi = backdrop["tris"].reshape(-1, 3).min(axis=0), backdrop["tris"].reshape(-1, 3).max(axis=0)
        wrapped = 0
        for name, part in by_mat.items():
            if BACKDROP in name:
                continue
            mid = (part["tris"].reshape(-1, 3).min(axis=0) + part["tris"].reshape(-1, 3).max(axis=0)) * 0.5
            if np.all(mid > lo) and np.all(mid < hi):
                wrapped += 1
        self.assertGreaterEqual(wrapped, 2)

    def test_the_near_wall_faces_away_so_the_clip_removes_it(self) -> None:
        # The clip keeps pixels whose interpolated normal points at the eye.
        # On the shipped backdrop that has to be the FAR wall: the near wall's
        # normal points at the model's centre, i.e. away from the camera. The
        # glass dome is authored outward, so its near wall points at the eye
        # and survives - the rule must not eat the shell.
        sheet = self.sheet
        yaw, pitch = math.radians(sheet.YAW_DEG), math.radians(sheet.PITCH_DEG)
        rz = np.array([[math.cos(yaw), -math.sin(yaw), 0.0], [math.sin(yaw), math.cos(yaw), 0.0], [0.0, 0.0, 1.0]])
        rx = np.array([[1.0, 0.0, 0.0], [0.0, math.cos(pitch), -math.sin(pitch)], [0.0, math.sin(pitch), math.cos(pitch)]])
        by_mat = {p["material"]: p for p in self.parts}
        for key in (BACKDROP, GLASS):
            part = next(p for name, p in by_mat.items() if key in name)
            tris = part["tris"]
            view = tris @ rz @ rx
            depth = view[:, :, 1].mean(axis=1)
            # view-space face normal; depth is +y, so "toward the eye" is -y.
            fn = np.cross(view[:, 1] - view[:, 0], view[:, 2] - view[:, 0])
            fn /= np.maximum(np.linalg.norm(fn, axis=1), 1e-12)[:, None]
            toward_eye = fn[:, 1] < 0.0
            nearest = int(np.argmin(depth))
            farthest = int(np.argmax(depth))
            if key == BACKDROP:
                self.assertFalse(toward_eye[nearest],
                                 "near wall of the backdrop must point away, i.e. be clipped")
                self.assertTrue(toward_eye[farthest],
                                "far wall of the backdrop must point at the eye, i.e. be kept")
            else:
                self.assertTrue(toward_eye[nearest],
                                "near wall of the dome points at the eye and must survive")


def main() -> None:
    unittest.main()


if __name__ == "__main__":
    main()
