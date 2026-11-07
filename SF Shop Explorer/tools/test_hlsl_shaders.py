#!/usr/bin/env python3
"""Compile every HLSL literal in OfflineModel.cpp with the real shader compiler.

The renderer's shaders are string literals compiled at runtime, so a syntax
error in a shader passes the C++ build and only shows up in game as a failed
PSO with a "PSO create failed" line. This compiles each literal with fxc at the
same profile (vs_5_0 / ps_5_0, entry `main`) against the same object model the
D3D12 path uses, so a broken literal fails here instead.

The SDK is a build-time dependency of the machine, not of the repo. Locate fxc
under any installed Windows Kits version; when none is present, say so and skip
rather than pretend the shaders compiled.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "OfflineModel.cpp"

SHADER_RE = re.compile(r'static constexpr const char (k\w+)\[\] = R"\((.*?)\)";', re.S)
KITS = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"


def find_fxc() -> Path | None:
    on_path = shutil.which("fxc")
    if on_path:
        return Path(on_path)
    if not KITS.is_dir():
        return None
    # Newest SDK first; layout is bin/<version>/x64/fxc.exe.
    for version in sorted(KITS.iterdir(), reverse=True):
        candidate = version / "x64" / "fxc.exe"
        if candidate.is_file():
            return candidate
    return None


class ShaderCompileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.shaders = SHADER_RE.findall(SOURCE.read_text(encoding="utf-8-sig"))
        cls.fxc = find_fxc()

    def test_the_source_still_contains_the_expected_shaders(self) -> None:
        # A rename or a move to a different literal syntax must not silently
        # turn this suite into a no-op.
        names = {name for name, _ in self.shaders}
        for expected in ("kVsSrc", "kPsSrc", "kPsBlendTexSrc", "kPsTexSrc", "kPsTexAlphaSrc"):
            self.assertIn(expected, names)

    def test_every_shader_compiles(self) -> None:
        if self.fxc is None:
            self.skipTest("fxc.exe not found (Windows SDK not installed); shaders not compiled")
        with tempfile.TemporaryDirectory() as tmp:
            for name, body in self.shaders:
                target = "vs_5_0" if "Vs" in name else "ps_5_0"
                stem = Path(tmp) / f"{name}.hlsl"
                stem.write_text(body, encoding="utf-8")
                with self.subTest(shader=name, profile=target):
                    result = subprocess.run(
                        [str(self.fxc), "/nologo", "/T", target, "/E", "main", str(stem)],
                        capture_output=True,
                        text=True,
                    )
                    output = (result.stdout or "") + (result.stderr or "")
                    self.assertEqual(result.returncode, 0, f"{name} failed to compile:\n{output}")
                    self.assertNotIn("error X", output, f"{name} reported a shader error:\n{output}")


if __name__ == "__main__":
    unittest.main()
