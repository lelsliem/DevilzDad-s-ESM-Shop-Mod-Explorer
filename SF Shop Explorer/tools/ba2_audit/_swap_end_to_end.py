"""Scratch: prove the NIF's material strings hit the generated swap table."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ba2lib import Ba2Archive  # noqa: E402
from nif_materials import material_strings  # noqa: E402

DATA = Path(r"B:/SteamLibrary/steamapps/common/Starfield/Data")
TABLE = Path(__file__).resolve().parents[1] / "materialswaps.tsv"


def swap_key(p: str) -> str:
    s = p.lower().replace(chr(92), "/").lstrip("/ ")
    if s.startswith("data/"):
        s = s[5:]
    if s.startswith("materials/"):
        s = s[10:]
    at = s.rfind(":")
    if at >= 0:
        s = s[:at]
    if s.endswith(".mat"):
        s = s[:-4]
    return s


table: dict[int, dict[str, str]] = {}
for line in TABLE.read_text(encoding="utf-8").splitlines():
    if not line:
        continue
    form, key, over = line.split("\t")
    table.setdefault(int(form, 16), {})[swap_key(key)] = over

targets = {
    0x3BF6: "meshes/clothes/outfit_trucker_tee_01/outfit_trucker_tee_01_f.nif",
    0x3E8F: "meshes/clothes/spacesuit_spacetrucker/spacesuit_spacetrucker_helmet.nif",
    0x4E78: "meshes/clothes/spacesuit_spacetrucker/spacesuit_spacetrucker_upperbody_1st_f.nif",
}
for form, nif in targets.items():
    blob = None
    for archive in sorted(DATA.glob("*Meshes*.ba2")):
        ar = Ba2Archive(archive)
        if not ar.valid:
            continue
        ent, _ = ar.read_entry(nif)
        if ent is not None:
            blob = ent
            print(f"{form:08X} nif from {archive.name}")
            break
    if blob is None:
        print(f"{form:08X} MISSING {nif}")
        continue
    mats = [s for s in material_strings(blob) if s.lower().endswith(".mat")]
    swaps = table.get(form, {})
    print(f"  {len(mats)} material(s) in nif, {len(swaps)} in table")
    for m in mats:
        hit = swaps.get(swap_key(m))
        print(f"   {m} -> " + (f"SWAP {hit}" if hit else "stock"))
