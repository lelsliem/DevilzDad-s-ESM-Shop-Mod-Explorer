# Full item render audit — 2026-11-02

Scope: every catalog record in the seven shop forms (`ARMO`, `WEAP`, `MISC`,
`AMMO`, `ALCH`, `KEYM`, `BOOK`). The sweep drew each record at 160 px with the
offline renderer, wrote one PNG per form plus a status row, and was run in eight
shards so the whole inventory completed in one pass.

```
python -X utf8 tools/render_workbench.py \
    --type ARMO --type WEAP --type MISC --type AMMO --type ALCH \
    --type KEYM --type BOOK --limit 0 --shard <n>/8 --once \
    --out reports/item-render-audit/shard-<n>
```

Catalog: `C:/Users/mrlma/Documents/My Games/Starfield/SFSE/Logs/SFDevilzDump-catalog.csv`
(94,810 records; 10,638 of the seven forms). All eight shards exited `0`.

## Result

| type | records | rendered | blank | flat | textured |
|---|---:|---:|---:|---:|---:|
| ARMO | 4,157 | 4,089 | 68 | 814 | 3,275 |
| WEAP | 642 | 293 | 349 | 66 | 227 |
| MISC | 2,656 | 2,619 | 37 | 331 | 2,288 |
| AMMO | 182 | 150 | 32 | 1 | 149 |
| ALCH | 1,199 | 1,185 | 14 | 37 | 1,148 |
| KEYM | 368 | 368 | 0 | 0 | 368 |
| BOOK | 1,434 | 1,429 | 5 | 6 | 1,423 |
| **total** | **10,638** | **10,133** | **505** | **1,255** | **8,878** |

`rendered` means at least one LOD0 geometry was built. `flat` means geometry
built but no material range resolved a colour map; `textured` means at least one
range did. That is 95.3 % of records with geometry and 87.6 % of rendered
records with at least one textured range.

## Residual blanks (505)

| reason | ARMO | WEAP | MISC | AMMO | ALCH | BOOK | total |
|---|---:|---:|---:|---:|---:|---:|---:|
| no MODL (record names no model) | 47 | 308 | 0 | 21 | 9 | 5 | 390 |
| NIF missing (path resolved, asset absent) | 4 | 15 | 1 | 11 | 0 | 0 | 31 |
| no renderable geometry (asset present, no LOD0) | 17 | 26 | 36 | 0 | 5 | 0 | 84 |

These are honest blanks, not renderer paths:

* **WEAP 308 no MODL** — the creature/unarmed/`EMPTY` weapons the live
  `FormModelPaths` deliberately leaves empty (a stinger is not a gun).
* **ARMO 47 no MODL** — skin and strut tokens with no `nifPaths`, plus records
  whose only path is the `SetDressing\...Container...` dressing proxy. The game
  draws no addon for these either.
* **AMMO 11 NIF missing** — nine records name
  `testspaceshiprocketprojectile.nif` and two name `Ammo\FatMan\AmmoFatMan.nif`;
  neither asset exists in the install. Separately, 100 generic
  `SetDressing\ammo_crates\type{a..f}_{medium,large,xlarge}.nif` placeholders
  were repointed at the shipped generic crate (fix 7), so they render.
* **WEAP 15 NIF missing** — ship-weapon parts such as
  `Ships\Discovery\Parts\WeaponTypeA.nif`; the asset is absent.
* **ARMO/MISC missing** — four mod garment/helmet paths (`ZNA4.nif`, the Varuun
  beanie pair, the Death Watch helmet) and one Misc (`Itm_Hookneck_Eye.nif`).
* **no renderable geometry** — the NIF exists but carries no LOD0 BSGeometry:
  the 30 Fertile Egg `GG_Egg_Glow.nif` overlays, the `BullpupDummyReceiver`
  families whose OMODs are not in the owner plugin (RK-3, ND-5, EquiNOX, Ember,
  T-21 and the mod blasters), the literal `Actors\Mesh\ATST\None.nif`, the
  `*_Empty.nif` Mandalorian attachments, and the voice-module armours.

## Defects fixed in this pass

### 1. Batch texture cache no longer grows without bound

`contact_sheet.Textures` is bounded by entry count (64), decoded bytes (512 MiB)
and a 4096×4096 pixel ceiling. The old 512-entry cache could reach 24 GB in one
worker; four of eight shards in the first attempt died with `MemoryError`. The
ceiling also skips 16384×16384 archive records whose float decode alone would be
3 GB. All eight final shards completed with no `MemoryError`.

### 2. A DDS packed as a general BA2 entry is decoded from its header

`AssetStore.texture` found mod textures in the general-entry fallback but
returned `TexInfo(blob, 0, 0, 0)`, so the renderer refused every one of them.
`aid_colour_audit.dds_image` now reads width, height and the DX10/DXT format
from the container. The same helper is used for loose DDS files, so loose BC7
content is no longer misread as DXGI 0 either.

Measured on a 60-item sample of the previous flat set: 8 items now resolve a
texture. The full sweep went from 1,436 flat to 1,255 flat and from 8,588 to
8,778 textured records (8,878 after the AMMO alias in fix 7). Example:
`Levat's Outfit` (0001D1DB) went from 1 geom /
0 textured to 1 / 1; its Lando body map is a 2048² BC7 DDS inside a general
archive entry.

### 3. Armour ground models are not drawn beside the worn addon

`FormModelPaths` draws an ARMO's ARMA addons, never the `worldModels` array. The
catalog flattens both into `nifPaths`, and only the `SetDressing\...Container...`
proxy was filtered. 662 ARMO records carry a second world model named
`*_go.nif` (helmet_go, backpack_go, SSMiner_Backpack_GO) or
`*groundcontainer*`; those were drawn on top of the worn armour. `catalog_addons`
now drops them when an addon remains, keeping the ground model only when it is
the record's sole geometry.

### 4. An unsuffixed female addon replaces the male copy

43 ARMO records pair `UpperBody_M.nif` with `UpperBody.nif` (swvt and similar
mods) instead of an explicit `_F` name. `_female_preferred` only paired `_M`
with `_F`, so both copies overlapped. It now also pairs a male path with the
immediately following same-stem path with no suffix, which is the dump's
male-then-female order per ARMA.

### 5. A geometry-less placeholder addon no longer blanks the outfit

Eight ARMO records (Pyke Syndicate suits, Dark Trooper Armor) contain one addon
with a valid NIF header but no block table. `_load_candidate` returned the first
parse exception and discarded the other nine sound parts. It now skips the
unreadable part and only reports `NIF parse failed` when no part of the set
could be read. All eight records render in the final sweep.

### 6. The dressing-proxy rule is scoped to ARMO addons

`catalog_models` was filtering every `SetDressing\...Container...` path for all
types. That is correct for an ARMO's dressing box but wrong for a Misc prop:
`Biowaste` (FD0A31D8) points at
`SetDressing\Container\useful morgues\biohazardcanister.nif`, which is the item.
The proxy filter now lives in `catalog_addons`; `catalog_models` returns the raw
resolved list. Biowaste went from blank to 5 geoms / 4 textured. 50 Misc and 3
Aid records carry only container paths and are no longer at risk of the same
false drop.

### 7. Absent generic AMMO crate placeholders render the shipped crate

100 AMMO records name a `SetDressing\ammo_crates\type{a..f}_{medium,large,xlarge}.nif`
crate that exists in none of the 495 indexed BA2s or the loose mod folders. The
shipped crates are instead named by caliber (`ammo11mm.nif`, `ammo20mmxpl.nif`,
...). `ammo_models` maps an absent placeholder to the shipped generic
`SetDressing\ammo_crates\ammobox_01.nif`, preserving the shell-casing path and
leaving a placeholder that actually exists untouched. The AMMO re-render went
from 50 to 150 records with geometry; only the nine ship-projectile and two
FatMan paths remain `NIF missing`.

This is an audit/preview alias, not a plugin edit: it does not add a NIF to the
install or change any game record. The live preview still sees the original
path and would report it missing.

Regression coverage: `tools/renderer_recon/test_contact_sheet.py` (ground model,
ground container, lone ground model, unsuffixed female, proxy scoping, Misc
container, AMMO placeholder alias) and `tools/test_render_workbench.py`. Run:

```
python -X utf8 -m unittest tools.renderer_recon.test_contact_sheet \
    tools.renderer_recon.test_aid_colour_audit \
    tools.renderer_recon.test_omod_assembly tools.test_render_workbench
```

## Remaining flat tiles (1,255)

Flat is not the same as broken. Of 60 still-flat records re-resolved after the
DDS fix, 52 had no texture key at all: every range reported `no record` for the
material in the CDB and no `.mat` file existed to name a set. Those are mod
assets that ship a NIF and a material path without the material definition; the
game's material system would have no albedo to bind either. The remaining flat
records are the known authored cases documented in
`reports/misc-full-audit-2026-10-23.md` (textureless CDB sets, layer composites,
clear glass with no art).

## Artifacts

* `reports/item-render-audit/shard-{0..7}/tiles/` — 10,638 PNG tiles, with the
  182 AMMO tiles replaced by the repointed pass.
* `reports/item-render-audit/ammo-repoint/` — the 182-record AMMO re-render,
  its TSV, page and tiles, kept as the evidence for the crate alias.
* `reports/item-render-audit/combined.tsv` — all 10,638 rows, one per record,
  with `status`, `geoms`, `tris`, `textured`, chosen `model` and shard.
* `reports/item-render-audit/shard-{0..7}/workbench.html` — the baseline page
  per shard. The AMMO rows and tiles were replaced after the repoint pass, so
  use `reports/item-render-audit/ammo-repoint/workbench.html` for AMMO and
  `combined.tsv` for the final per-record data.
* `reports/item-render-audit/exit-codes.txt` — all eight shards `exit=0`;
  `ammo-repoint.log` records the AMMO pass `exit=0`.

## Limits

Tiles are flat-shaded, painter-sorted and built from LOD0 only; they are not a
game capture. Armour is drawn from the flattened ARMA addon set without a
skeleton, so cloth interpenetration and skinning are not audited. Textures above
4096×4096 are skipped by policy. In-game confirmation still requires selecting
representative modded and base-game forms in Starfield and comparing their
logged resolved model/geometry paths.
