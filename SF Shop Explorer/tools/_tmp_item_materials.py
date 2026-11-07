"""Print the audit's per-geometry material decisions for one or more forms."""

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

forms = [int(x, 16) for x in sys.argv[1:]] or [0x00009A2F, 0x0002775C]
catalog = sheet.audit.parse_catalog(sheet.audit.find_catalog(None))
by_form = {it["form"]: it for it in catalog}
meshes: dict = {}
for form in forms:
    item = by_form.get(form)
    if not item:
        print(f"{form:08X} not in catalog")
        continue
    geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, meshes)
    print(f"== {item['name']} {form:08X} model={meta['model']} status={meta['status']}")
    for g in geoms:
        m = g["mat"]
        tex = m["texture"] or "-"
        print(f"   {g['name']:<28} {m['mat']}")
        print(f"      tex={tex}")
        print(f"      alpha={m['alpha']:.3f} translucent={m['translucent']} color={m['color']} why={m['color_why']}")
    for m in meta["materials"]:
        if m.get("effect"):
            print(f"   [skipped effect] {m['mat']}  tex={m['texture']}")
