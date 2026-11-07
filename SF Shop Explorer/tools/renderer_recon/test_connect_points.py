"""The assembly placement graph, without a game install.

A dummy-base gun is only a list of NIFs until the C-/P- connect points are
resolved; drawing the files at their own origins is what made Razorback an
exploded pile of parts. These tests pin the chain the C++ resolver uses:
the C-Receiver holder is the identity root, a host is
parentPlacement * hostNodeWorld * entryTransform, and a part with no host stays
at identity instead of disappearing.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import connect_points as cp  # noqa: E402
import contact_sheet as sheet  # noqa: E402


def parent(name: str, node: str = "", t: tuple[float, float, float] = (0, 0, 0)) -> dict:
    matrix = cp.IDENTITY.copy()
    matrix[:3, 3] = t
    return {"node": node, "name": name, "m": matrix}


def info(childs=(), parents=(), nodes=None) -> dict:
    return {"childs": list(childs), "parents": list(parents),
            "node_worlds": dict(nodes or {}), "root_world": cp.IDENTITY.copy()}


class PlacementTests(unittest.TestCase):
    def test_the_receiver_holder_is_the_identity_root_case_insensitive(self) -> None:
        # magshot ships a lowercase key; the C++ resolver matches it anyway.
        infos = [info(["C-Receiver"]), info(["C-Grip"])]
        places = cp.resolve_infos(infos)
        np.testing.assert_allclose(places[0], cp.IDENTITY)
        np.testing.assert_allclose(places[1], cp.IDENTITY)

    def test_a_part_lands_at_its_hosts_node_and_entry_transform(self) -> None:
        node_world = cp.IDENTITY.copy()
        node_world[:3, 3] = (0.0, 0.0, 1.0)
        infos = [
            info(["C-Receiver"], [parent("P-Grip", "GripNode", (0.0, 0.0, 0.1))],
                 {"GripNode": node_world}),
            info(["C-Grip"]),
        ]
        places = cp.resolve_infos(infos)
        # parentPlacement (identity) * hostNodeWorld (z=1) * entry (z=0.1)
        np.testing.assert_allclose(places[1][:3, 3], (0.0, 0.0, 1.1))

    def test_a_chain_compounds_through_the_mounted_part(self) -> None:
        infos = [
            info(["C-Receiver"], [parent("P-Grip", "", (0.0, 0.0, 1.0))]),
            info(["C-Grip"], [parent("P-Barrel", "BarrelMount", (0.0, 0.2, 0.0))],
                 {"BarrelMount": cp.IDENTITY.copy()}),
            info(["C-Barrel"]),
        ]
        places = cp.resolve_infos(infos)
        np.testing.assert_allclose(places[1][:3, 3], (0.0, 0.0, 1.0))
        np.testing.assert_allclose(places[2][:3, 3], (0.0, 0.2, 1.0))

    def test_a_part_with_no_host_keeps_identity(self) -> None:
        # The C++ resolver logs this fallback and draws it at the origin rather
        # than dropping the part, which is the honest blank-preview answer.
        infos = [info(["C-Receiver"]), info(["C-Orphan"])]
        places, placed = cp.resolve_placements(infos)
        np.testing.assert_allclose(places[1], cp.IDENTITY)
        self.assertEqual(placed, [True, False])

    def test_a_mounted_part_at_the_origin_is_still_placed(self) -> None:
        # The mounted scope sits exactly at the receiver origin; an identity
        # matrix here is a hit, not a fallback, and the status mask says so.
        infos = [info(["C-Receiver"], [parent("P-Scope")]), info(["C-Scope"])]
        places, placed = cp.resolve_placements(infos)
        np.testing.assert_allclose(places[1], cp.IDENTITY)
        self.assertEqual(placed, [True, True])

    def test_a_missing_host_node_uses_the_file_root(self) -> None:
        root = cp.IDENTITY.copy()
        root[:3, 3] = (0.5, 0.0, 0.0)
        entry = parent("P-Barrel", "NoSuchNode", (0.0, 0.0, 0.25))
        infos = [dict(info(["C-Receiver"], [entry]), root_world=root), info(["C-Barrel"])]
        places = cp.resolve_infos(infos)
        np.testing.assert_allclose(places[1][:3, 3], (0.5, 0.0, 0.25))


class VertexConventionTests(unittest.TestCase):
    def test_world_vertices_applies_a_node_translation(self) -> None:
        # NIF matrices are column-vector matrices; p @ M would silently drop
        # this translation and transpose the rotation. That was the difference
        # between a mounted part and one sitting at the receiver's origin.
        mesh = {"pos": np.array([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0]], dtype=np.float32),
                "nv": 2}
        world = cp.IDENTITY.copy()
        world[:3, 3] = (5.0, 6.0, 7.0)
        np.testing.assert_allclose(sheet.world_vertices(mesh, world),
                                   [[5.0, 6.0, 7.0], [6.0, 6.0, 7.0]])

    def test_world_vertices_applies_a_rotation(self) -> None:
        mesh = {"pos": np.array([[1.0, 0.0, 0.0]], dtype=np.float32), "nv": 1}
        world = cp.IDENTITY.copy()
        world[:3, :3] = [[0.0, -1.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, 1.0]]
        np.testing.assert_allclose(sheet.world_vertices(mesh, world), [[0.0, 1.0, 0.0]])


if __name__ == "__main__":
    unittest.main()
