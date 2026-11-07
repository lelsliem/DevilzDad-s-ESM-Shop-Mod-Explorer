import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))

import contact_sheet as sheet  # noqa: E402

DATA = Path(sheet.audit.GAME_DATA)
store = sheet.audit.AssetStore([DATA], Path(sheet.audit.MODS_ROOT), False)
plugins = sheet.audit.Plugins(store)
swaps = sheet.audit.Swaps(Path(sheet.audit.SWAPS))
cdb = sheet.audit.Cdb(Path(sheet.audit.CDB))
textures = sheet.Textures(store)

catalog = sheet.audit.parse_catalog(sheet.audit.find_catalog(None))
items = [it for it in catalog if "snow globe" in it["name"].lower()]
meshes: dict = {}
print(f"{len(items)} snow globes")
for it in items:
    geoms, meta = sheet.prepare_item(it, store, plugins, swaps, cdb, textures, meshes)
    bg = [g for g in geoms if "background" in g["mat"]["mat"].lower()]
    line = []
    for g in bg:
        line.append((g["name"], g["mat"]["texture"] or "-", g["mat"]["color_why"][:60]))
    print(f"{it['name']:<28} {it['form']:08X} {meta['model']}")
    for name, tex, why in line:
        print(f"    {name:<26} {tex}")
    if not line:
        print(f"    (no background geom; geoms={meta['geoms']} status={meta['status']})")
