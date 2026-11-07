"""Differential check: the compiled CDB weapon-family gate vs its replica.

`test_cdb_material_sets.py` carries a Python replica of
`CdbWeaponTextureMatchesFamily` because the real thing lives in an anonymous
namespace inside a plugin translation unit. That replica is only evidence while
it agrees with the shipped code, and asserting source strings does not prove
agreement on inputs nobody thought of.

So this tool extracts `LowerAscii`, `WeaponDirNamesFamily` and
`CdbWeaponTextureMatchesFamily` **verbatim** out of `src/LiveModel.cpp`,
compiles them into a throwaway console program, and feeds both the compiled C++
and the replica the same pairs: every shipped Coachman CDB row, every CDB weapon
texture directory against its own name and every prefix of it, the deliberate
cross-family traps, and a deterministic fuzz over material/texture token soup.

    python tools/ba2_audit/family_gate_diff.py
    python tools/ba2_audit/family_gate_diff.py --fuzz 40000

Exit status: 0 when the two agree, 1 on any disagreement (with the pairs
printed), 2 when the check could not run - no MSVC toolchain, or the extracted
`materialsbeta.cdb` is absent. 2 is a skip, not a pass.

Nothing is left behind: the generated .cpp, the object file and the executable
go into a temporary directory.
"""
from __future__ import annotations

import argparse
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RECON = ROOT / "tools" / "renderer_recon"
TAB = chr(9)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from material_sweep import build_cdb_index  # noqa: E402

from test_cdb_material_sets import (  # noqa: E402
    _family_gate_source,
    _family_matches,
)

# Family token -> weapon texture directory pairs measured across the whole
# shipped CDB: the only prefix relations it contains. Kept as a literal so the
# check still runs (and still exercises the Coachman case) if the archive moves.
CENSUS = [
    ("ammo", "ammocounters"),
    ("arboron", "arborondagger"),
    ("coachman", "coachmanshotgun"),
    ("hardta", "hardtarget"),
    ("inflictor", "inflictorpistol"),
    ("inflictor", "inflictorrifle"),
    ("lasercutter", "lasercutter_worn"),
    ("orion", "orionrifle"),
    ("pacifier", "pacifiershotgun"),
    ("side", "sidestar"),
]

TRAPS = [
    ("materials/weapons/regulator/barrel.mat",
     "textures/actors/modela/crimsonfleet/magstorm/barrel_color.dds"),
    ("materials/weapons/regulator/regulator_barrel.mat",
     "textures/weapons/magstorm/regulator_barrel_color.dds"),
    ("materials/weapons/breach/decals.mat",
     "textures/weapons/solstice/ember/solstice_decals_color.dds"),
    ("materials/weapons/breach/decals.mat", "textures/weapons/breach/decals_color.dds"),
    ("materials/weapons/regulator/compensator.mat",
     "textures/weapons/tombstone/mods_tombstone_compensator_color.dds"),
    ("materials/weapons/awp_master_shareable/grip.mat",
     "textures/weapons/awp_shareable/grip_color.dds"),
    ("materials/weapons/breach/breach_stock_tactical.mat",
     "textures/weapons/breach/stock_tactical_color.dds"),
    ("materials/weapons/dlc001/vortexgrenade/vortexgrenade.mat",
     "textures/weapons/dlc001/vortextgrenade/lambert1_color.dds"),
    # Degenerate shapes: no directory under materials/weapons/, an empty
    # material, an empty texture, a directory that only extends by one
    # character.
    ("materials/weapons/coachman.mat", "textures/weapons/coachmanshotgun/decals_color.dds"),
    ("materials/weapons/coachman/decals.mat", ""),
    ("materials/weapons/coachman/decals.mat", "textures/weapons/coachmanshotgun/"),
    ("materials/weapons/coachman/decals.mat", "textures/weapons/coachman2/decals_color.dds"),
    ("materials/setdressing/books/genbookb01_bookb04.mat",
     "textures/weapons/coachmanshotgun/decals_color.dds"),
]

FUZZ_VOCAB = ["coachman", "coachmanshotgun", "barrel", "decals", "weapons", "textures",
              "regulator", "magstorm", "solstice", "awp_shareable", "awp_master_shareable",
              "dlc001", "vortextgrenade", ".dds", ".mat", "_color", "_opacity", "color",
              "grip", "12", "sfbgs050", "x"]


def build_pairs(cdb: Path, fuzz: int) -> list[tuple[str, str]]:
    pairs: list[tuple[str, str]] = []
    for family, directory in CENSUS:
        pairs.append((f"materials/weapons/{family}/{family}.mat",
                      f"textures/weapons/{directory}/{directory}_color.dds"))
    index = build_cdb_index(cdb)
    for name, maps in index.items():
        if name.startswith("coachman_"):
            material = f"materials/weapons/coachman/{name}.mat"
            for kind in ("color", "opacity"):
                if kind in maps:
                    pairs.append((material, maps[kind]))
    weapon_maps: dict[str, str] = {}
    for maps in index.values():
        for kind in ("color", "opacity"):
            key = maps.get(kind)
            if not key or "/weapons/" not in key:
                continue
            for seg in key.split("/weapons/", 1)[1].split("/")[:-1]:
                if seg and "." not in seg:
                    weapon_maps.setdefault(seg, key)
    wrong = ["regulator", "breach", "coachman", "magstorm", "solstice", "tombstone", "rattler"]
    for directory, key in sorted(weapon_maps.items()):
        families = {directory} | {directory[:k] for k in range(4, len(directory) - 2)} | set(wrong)
        for family in sorted(f for f in families if len(f) >= 2):
            pairs.append((f"materials/weapons/{family}/{family}_barrel.mat", key))
    pairs += TRAPS
    rng = random.Random(20261017)
    for _ in range(fuzz):
        material = "materials/" + "/".join(rng.choice(FUZZ_VOCAB) for _ in range(rng.randint(1, 6)))
        material += rng.choice([".mat", ""])
        texture = "textures/" + "/".join(rng.choice(FUZZ_VOCAB) for _ in range(rng.randint(1, 6)))
        pairs.append((material, texture))
    return pairs


def extract_source() -> str:
    source = (ROOT / "src" / "LiveModel.cpp").read_text(encoding="utf-8")

    def between(start: str, end: str) -> str:
        i = source.index(start)
        return source[i:source.index(end, i)]

    def deindent(text: str) -> str:
        return "\n".join(line[2:] if line.startswith(TAB * 2) else line
                         for line in text.split("\n")).strip()

    return (
        "// GENERATED by tools/ba2_audit/family_gate_diff.py - do not edit.\n"
        "#include <algorithm>\n#include <cctype>\n#include <cstddef>\n#include <iostream>\n"
        "#include <string>\n#include <string_view>\n#include <unordered_set>\n#include <vector>\n\n"
        "namespace extracted {\n\n"
        + deindent(between("std::string LowerAscii(std::string_view a_text)",
                           TAB * 2 + "std::vector<std::filesystem::path> SplitPaths")) + "\n\n"
        + deindent(between("bool WeaponDirNamesFamily(", "bool CdbWeaponTextureMatchesFamily(")) + "\n\n"
        + deindent(between("bool CdbWeaponTextureMatchesFamily(", TAB + "bool FindDiffuse(")) + "\n\n}\n\n"
        "int main() {\n"
        "    std::string line;\n"
        "    while (std::getline(std::cin, line)) {\n"
        "        const auto tab = line.find(char(9));\n"
        "        if (tab == std::string::npos) { continue; }\n"
        "        std::cout << (extracted::CdbWeaponTextureMatchesFamily("
        "line.substr(0, tab), line.substr(tab + 1)) ? 1 : 0) << '\\n';\n"
        "    }\n"
        "    return 0;\n}\n"
    )


def find_vcvars() -> str | None:
    vswhere = Path("C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
    if not vswhere.exists():
        return None
    result = subprocess.run([str(vswhere), "-latest", "-property", "installationPath"],
                            capture_output=True, text=True)
    install = result.stdout.strip()
    if not install:
        return None
    candidate = Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    return str(candidate) if candidate.exists() else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fuzz", type=int, default=4000,
                        help="number of random material/texture pairs (default 4000)")
    parser.add_argument("--cdb", type=Path,
                        default=RECON / "extracted" / "materialsbeta.cdb")
    args = parser.parse_args()

    if not args.cdb.exists():
        print(f"SKIP: {args.cdb} is not present")
        return 2
    vcvars = find_vcvars()
    if vcvars is None:
        print("SKIP: no MSVC vcvars64.bat found (set up Visual Studio to run this check)")
        return 2

    with tempfile.TemporaryDirectory(prefix="family_gate_diff_") as temp:
        workdir = Path(temp)
        (workdir / "gate.cpp").write_text(extract_source(), encoding="utf-8", newline="\n")
        bat = workdir / "build.cmd"
        bat.write_bytes((
            "@echo off\r\n"
            'call "' + vcvars + '" > vcvars.log 2>&1\r\n'
            "cl /nologo /std:c++20 /EHsc /W4 /Fe:gate.exe /Fo:gate.obj gate.cpp\r\n"
            "exit /b %errorlevel%\r\n").encode("ascii"))
        compiled = subprocess.run(["cmd", "/c", str(bat)], cwd=str(workdir),
                                  capture_output=True, text=True)
        if compiled.returncode != 0 or not (workdir / "gate.exe").exists():
            print("FAILED to compile the extracted gate:")
            print((compiled.stdout + compiled.stderr).strip()[-2000:])
            return 1

        pairs = build_pairs(args.cdb, args.fuzz)
        payload = "".join(material + TAB + texture + "\n" for material, texture in pairs)
        run = subprocess.run([str(workdir / "gate.exe")], input=payload, cwd=str(workdir),
                             capture_output=True, text=True)
        if run.returncode != 0:
            print("FAILED to run the extracted gate:", run.stderr.strip()[-800:])
            return 1
        cpp = [line.strip() == "1" for line in run.stdout.splitlines() if line.strip()]
        if len(cpp) != len(pairs):
            print(f"FAILED: the gate answered {len(cpp)} of {len(pairs)} pairs")
            return 1

    gate_fn = _family_gate_source()
    mismatches = [(material, texture, replica, native)
                  for (material, texture), native in zip(pairs, cpp)
                  if (replica := _family_matches(gate_fn, material, texture)) != native]
    print(f"pairs: {len(pairs)}   accepted by the compiled gate: {sum(cpp)}")
    print(f"replica disagreements: {len(mismatches)}")
    for material, texture, replica, native in mismatches[:20]:
        print(f"  MISMATCH replica={replica} cpp={native}  {material}  <-  {texture}")
    print("OK: the replica matches the compiled gate" if not mismatches
          else "FAILED: the replica has drifted from src/LiveModel.cpp")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
