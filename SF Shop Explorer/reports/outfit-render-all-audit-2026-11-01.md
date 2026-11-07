# Outfit "render all" audit — 2026-11-04

Status: **PASS**. The command exits 0 when run against this install.

Scope: every installed ARMO record and every complete `upperbody` / `lowerbody` / `sleeves` NIF set in the active profile. The check mirrors `LiveModel::BuildModels` and `AssembleSiblings`, including the filename-only part token and the declared-part guard. It parses each NIF's LOD0 mesh with the same offline path as the contact sheet; it does not verify vertex alignment or material appearance.

Catalog: `C:\Users\mrlma\Documents\My Games\Starfield\SFSE\Logs\SFDevilzDump-catalog.csv`

## Result

| Measure | Count |
|---|---:|
| ARMO records | 4,157 |
| Complete 3-part asset groups | 94 |
| Complete groups referenced by an ARMO | 78 |
| Record/group pairs checked | 280 |
| Pairs declaring 3 parts | 195 |
| Pairs declaring 2 parts | 31 |
| Pairs declaring 1 part | 54 |
| Pairs with an in-group missing part | 85 |
| Pairs where the fallback supplied a part | 34 |
| Pairs already covered by another group | 51 |
| Unique NIFs in the assembled sets | 234 |
| Records missing a part | **0** |
| Records with a non-buildable part | **0** |
| Unique non-buildable NIFs | **0** |

An in-group missing part is not automatically a failed outfit: a record can take that type from another complete group it declares. The failure check is the record's assembled union, not each group in isolation.

## Failures

None. Every referenced record has all three part types and every assembled part NIF yields a usable LOD0 mesh.

## Path health

| Declared paths | Records |
|---|---:|
| All present | 4,066 |
| Some missing | 50 |
| All missing | 2 |
| No NIF paths | 39 |

## Warnings

- **2 records have every declared NIF missing.** These are not outfit-assembly failures by themselves.
  - `Starfield.esm` `00319B43` **SkinModelA Nasa**: `actors/zna4/zna4.nif`
  - `TrackerAlliance_HQ_Overhaul.esm` `FE04F821` **Death Watch - Nite Owl Mandalorian Helmet**: `clothes/mandaloriansuits/groundhelmetniteowl.nif`, `clothes/mandaloriansuits/mandalorianhelmet.nif`, `clothes/mandaloriansuits/mandalorianhelmet_niteowl.nif`
- **2 records have a missing token-defined part path.** The remaining parts still render.
  - `Starfield.esm` `001341E4` **Andreja's Clothes**: `clothes/ahsoka/ahsokasleevesm.nif`
  - `Starfield.esm` `001C8E09` **Andreja's Clothes**: `clothes/ahsoka/ahsokasleevesm.nif`
- **32 one-part records have no sibling on disk.** They are standalone or single-part assets, not failed half-outfits.
- **2 complete-group NIFs are unused by any record and do not build.** They are orphan assets, not record gaps.
  - `greedo/greedo_sleeves_m.nif` (no usable LOD0 mesh)
  - `greedo/greedo_sleeves_f.nif` (no usable LOD0 mesh)

Missing optional/alternate paths are not assembly failures. A record can combine a custom upper body from one group with a lower body from another; the declared-part guard deliberately leaves such a group mixed. The failure check is record-level: the record's union of declared and fallback parts must contain all three types, and every part NIF in that union must build. A referenced record that fails either check is listed above. `xmake build -y` and the unit tests cover the C++ rule; this command covers the installed asset set.

## Sibling rule

`OutfitPart` takes the token from the basename; `AssembleSiblings` replaces that exact position. `BuildModels` collects the record's declared part types and only fills a type the record does not already have. This avoids both the `SWCLMT/upperbodyaddon/...lowerbody.nif` folder-token mismatch and adding a second torso to a record that already supplies a custom upper body. `test_inline_nif_geometry.SiblingAssemblyTests` pins both rules.

## Reproduce

```bash
python tools/renderer_recon/outfit_assembly_audit.py
```

Use `--out` to write a different report, `--junitxml` to write one JUnit case per failed record, `--catalog` to use another dump, and `--data` / `--mods-root` to point at another install. The command exits 1 on a missing or non-buildable referenced part, 2 when the catalog or complete asset groups cannot be found.

## Verification limit

The report is an asset/source audit, not a live Starfield capture. NIF geometry is parsed offline; part alignment and material appearance remain visually untested. Run `xmake build -y` and `python -X utf8 -m unittest tools.test_inline_nif_geometry` separately for the C++ rule.
