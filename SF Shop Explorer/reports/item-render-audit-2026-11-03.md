# Full item render audit — 2026-11-03

Second full sweep of every catalog record in the seven shop forms (`ARMO`,
`WEAP`, `MISC`, `AMMO`, `ALCH`, `KEYM`, `BOOK`), run against the same install as
the 2026-11-02 baseline with the renderer unchanged, to confirm the sweep is
stable and reproducible before any rule work.

```
python -X utf8 tools/render_workbench.py \
    --type ARMO --type WEAP --type MISC --type AMMO --type ALCH \
    --type KEYM --type BOOK --limit 0 --shard <n>/8 --once \
    --out reports/item-render-audit-2026-11-03/shard-<n>
```

Catalog: `C:/Users/mrlma/Documents/My Games/Starfield/SFSE/Logs/SFDevilzDump-catalog.csv`
(94,810 records; 10,638 of the seven forms). All eight shards exited `0`
(`exit-codes.txt`), 10,638 PNG tiles written, none under 1 KB, 88 MB total.

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

Identical, record for record, to `item-render-audit-2026-11-02.md`.

## Diff against the 2026-11-02 baseline

Compared by `(form, type)` over `combined.tsv`:

| measure | count |
|---|---:|
| records in both runs | 10,638 |
| records only in one run | 0 |
| textured → flat regressions | **0** |
| flat → textured gains | **0** |
| blank → rendered | **0** |
| rendered → blank | **0** |

No renderer or resolver rule changed between the two runs, so this is the
expected result; it also means a later rule change can be judged against this
table without noise.

## Blank reasons

| reason | ARMO | WEAP | MISC | AMMO | ALCH | BOOK | total |
|---|---:|---:|---:|---:|---:|---:|---:|
| no MODL | 47 | 308 | 0 | 21 | 9 | 5 | 390 |
| no renderable geometry | 17 | 26 | 36 | 0 | 5 | 0 | 84 |
| NIF missing | 4 | 15 | 1 | 11 | 0 | 0 | 31 |

These remain the honest blanks documented in the 2026-11-02 report: creature and
`EMPTY` weapons with no model, `*_Empty.nif` Mandalorian attachments, the AT-ST
`None.nif`, and paths that exist in no archive. The AMMO `type{a..f}` crate
placeholders are not among them — the alias in `ammo_models` repoints them at
the shipped crate, and only nine ship-projectile and two FatMan paths stay
missing.

## Can the 1,255 flat tiles be improved?

No safe change was found, and the data says why.

Sampled the first 300 flat rows and re-resolved them through
`contact_sheet.prepare_item` (`flat-probe2.txt`): 871 materials ended with no
colour map.

| decision | materials |
|---|---:|
| `flat: no authored colour, no map` (CDB `no record`) | 871 |
| `authored override kept: no colour map in the material's own set` | 11 |
| `flat: clear glass family has no art in the database` | 3 |

Asking the CDB index which key shape would have matched each of 763 of those
materials (`cdb-gap.txt`, `tools/_tmp_cdb_gap.py`) gives:

| first matching key shape | materials |
|---|---:|
| no key shape at all | **647** |
| family key exists but empty | 63 |
| basename key exists but empty | 31 |
| family key with art | 17 |
| `data/<path>` key exists but empty | 4 |
| folder key with art | 1 |

The 647 are materials the extracted `materialsbeta.cdb` (44,059 sets) simply
does not name — there is no map to bind, and the game's own material system has
none either. The 98 empty-set hits carry no albedo.

The 18 art-bearing hits are parent stems (for example `Erdebrau_Dark.mat` →
`erdebrau`), and `Cdb._family_with_colour` refuses them on purpose: it borrows
only from a family that itself names a colour, because claiming any parent stem
"would replace an honest 'no record' with a misleading hit". Extending it to
every parent stem would paint a beer bottle with a can texture — a worse
preview, not a better one.

So the flats are a data limit, not a renderer path. The remaining lever is an
authored art decision (a tint table entry), which is a content change, not an
audit fix.

## Outfit assembly audit

`tools/renderer_recon/outfit_assembly_audit.py` — **PASS** (`exit=0`, 27.4 s),
report in `reports/outfit-render-all-audit-2026-11-03.md`: 4,157 ARMO records,
280 record/group pairs, 234 unique NIFs, 0 records missing a part, 0 records
with a non-buildable part, 0 unique non-buildable NIFs.

## Artifacts

* `reports/item-render-audit-2026-11-03/shard-{0..7}/tiles/` — 10,638 PNG tiles.
* `reports/item-render-audit-2026-11-03/combined.tsv` — 10,638 rows with
  `status`, `geoms`, `tris`, `textured`, chosen `model`, shard.
* `reports/item-render-audit-2026-11-03/summary.txt` — the tables above.
* `reports/item-render-audit-2026-11-03/exit-codes.txt` — eight `exit=0`.
* `flat-probe2.txt`, `cdb-gap.txt` — the flat-material evidence.
* `tools/render_audit_combine.py`, `tools/_tmp_probe_flats.py`,
  `tools/_tmp_cdb_gap.py` — the scripts that produced the TSV, the flat
  decisions and the key-shape table.

## Limits

Unchanged from the baseline: flat-shaded painter-sorted LOD0 tiles, no skeleton,
no game capture. Textures above 4096² are skipped by policy.
