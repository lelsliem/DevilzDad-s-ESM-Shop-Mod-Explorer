"""Guards the direct preview against handing the same mesh list to the GPU twice.

The 2026-10-20 10:02 session drew every item at double size: the Alien Genetic
Material's 6 NIF shapes arrived as 12 ranges / 9440 tris, the Pils can's 7 as
14, the Sparkling Wine's 3 as 6. The cause was two identical loops over
``s_native.meshes`` in ShopExplorer.cpp, both pushing into ``direct``. Doubled
geometry also doubled every translucent shell, which is why the coolant jar read
as a dark tub instead of clear glass.

The NIF is the ground truth and the log is the evidence, so the count is
asserted from both ends: one builder loop in the source, and the shape count of
a shipped NIF that the duplicate doubled. A behavioural test would need the game
running with the shop open; this pins the structure that produced the doubling.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPLORER = ROOT / "src" / "ShopExplorer.cpp"

sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))

# The jar whose 6 shapes became 12 ranges. Its shape count is what the duplicate
# inflated, so it is the case worth pinning.
GAME_DATA = Path(r"B:\SteamLibrary\steamapps\common\Starfield\Data")
CATALYST_NIF = "items\\legendarycatalyst\\legendarycatalyst.nif"


class DirectMeshLoopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = EXPLORER.read_text(encoding="utf-8-sig")

    def test_only_one_loop_builds_the_direct_mesh_list(self) -> None:
        pushes = self.source.count("direct.push_back(")
        self.assertEqual(
            pushes, 1,
            "the direct preview mesh list must be built once; a second "
            "push_back doubles every item's geometry and every translucent "
            "shell with it",
        )

    def test_only_one_request_is_made(self) -> None:
        self.assertEqual(self.source.count("RequestPreviewDirect(std::move(direct)"), 1)


@unittest.skipUnless(GAME_DATA.is_dir(), "Starfield Data directory not present")
class CatalystShapeCountTests(unittest.TestCase):
    def test_catalyst_nif_has_six_shapes(self) -> None:
        import nif_uv  # noqa: PLC0415

        arch = nif_uv.Arch(GAME_DATA)
        key, blob, nif, err = nif_uv.read_nif(arch, CATALYST_NIF)
        self.assertIsNotNone(nif, err)
        shapes = nif_uv.parse_shapes(blob, nif)
        self.assertEqual(len(shapes), 6, f"{key} shape count changed: {[s['name'] for s in shapes]}")


if __name__ == "__main__":
    unittest.main()
