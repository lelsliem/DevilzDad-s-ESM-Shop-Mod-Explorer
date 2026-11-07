"""A UV-less Clone3D extraction must yield to the geometry files, not win.

`3kV LZR Cartridge` (0000E8EC) rendered as a plain shape: the log said

    direct: request '3kV LZR Cartridge' (1 mesh(es), 62 tris)
    offline GPU: texturing 'direct:3kV LZR Cartridge' (1 ranges, 0 prefetched)

and the geometry leaf it came from reported `uv=0`. Clone3D answered with
geometry, so Stage A2 - which parses the NIF's `.mesh` files, the place the UV
streams actually live - was skipped because its gate only asked "are there
meshes". The direct renderer then had one range with no texture to bind.

The fix makes "meshes but no UVs" count as a failed clone so the files get
their chance, and restores the clone if the files come back empty so no item
can lose its geometry to the check.

These are source-structure assertions. Exercising the real path needs the game;
what is pinned here is that the gate exists, sits before the file adoption, and
keeps the fallback.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "src" / "NativeModel.cpp"


class UvlessCloneFallbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = NATIVE.read_text(encoding="utf-8-sig")

    def test_uvless_clone_is_detected_before_file_adoption(self) -> None:
        detect = self.src.find("no UV stream - deferring to geometry files")
        # Compare against the CALL, not the definition: AdoptFileMeshes is
        # declared above this block, so its first occurrence is the definition.
        adopt = self.src.find("AdoptFileMeshes(p, result.meshes")
        self.assertNotEqual(detect, -1, "the UV-less clone must be logged, not silently dropped")
        self.assertNotEqual(adopt, -1, "the geometry-file adoption call must still exist")
        self.assertLess(detect, adopt, "the check must run before the geometry files are adopted")

    def test_clone_is_retained_as_a_fallback(self) -> None:
        self.assertIn("uvlessClone = result.meshes;", self.src)
        self.assertIn("restored {} UV-less Clone3D mesh(es)", self.src)

    def test_geometry_is_restored_when_files_are_empty(self) -> None:
        # The restore must be gated on the file path having produced nothing,
        # otherwise it would overwrite good NIF meshes with the UV-less clone.
        self.assertRegex(
            self.src,
            r"if \(result\.meshes\.empty\(\) && !uvlessClone\.empty\(\)\)",
        )

    def test_uv_means_two_floats_per_vertex(self) -> None:
        # uv is a flat pair list; six floats is the smallest stream that can be
        # sampled, so a one-triangle stub must not count as textured geometry.
        self.assertIn("a_mesh.uv.size() >= 6", self.src)


if __name__ == "__main__":
    unittest.main()
