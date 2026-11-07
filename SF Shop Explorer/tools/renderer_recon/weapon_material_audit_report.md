# Weapon/material preview audit

**Date:** 2026-10-14  
**Scope:** installed Starfield base-game CDB and BA2 archives, enabled MO2 Test profile archives, CDB resolver behavior, direct-preview texture/mask validation. Archive/source evidence is not the same as an in-game pixel comparison.

## Base-game material sweep

Ran `material_report.py` against all 25 installed base-game texture BA2s and `material_sweep.py` against all 110 Data BA2s, indexing the extracted 42,933-set base CDB:

- 36,331 albedo paths resolve; 5,596 materials are intentionally flat; 941 are opacity-only cutouts.
- Zero CDB color paths resolved to BC4/BC5 single-channel textures used as albedo.
- 65 CDB color paths do not resolve in the texture archives. The grouped report shows most are test/debug assets and generated LODs; the full paths are in the raw TSV report generated at `B:\MO2\Starfield\overwrite\SFShopExplorer-material-report.tsv`.
- 23,026 opacity maps resolve. 253 have lower dimensions and are otherwise compatible. 397 format/shape cases require classification; many have UV/atlas-specific aspect differences, so they are not automatically “bad.” 50 named opacity maps are not present.
- Among 25 base texture archives: 133,175 unique names, 3,226 duplicate names, and seven metadata conflicts. The seven are patch-vs-original resolution updates; none were weapon assets.

The only generic weapon albedo pairs in the CDB sample showed the same repeated 4×4 BC4 payload `ff00499224499224` for eight weapon masks; decoded values were 0/218/255. These are not useful spatial masks for 1K–2K albedos. A separate 4×4 Pacifier glass color/opacity pair has a different payload and is a deliberate flat/single-block surface, not part of that placeholder-mask group. The live prefetch gate now rejects 4×4 by requiring >=8×8. Valid downscaled masks (e.g. Magshear's 512×512 opacity for a 2048×2048 color map) remain allowed.

## Weapon-wide map-path trace

Inspected 1,333 weapon NIF archive entries (1,333 unique weapon material paths; 2,138 path references across base archives) and compared each material against the runtime CDB lookup precedence. This finds systemic bad mappings that a generic “does path resolve?” sweep cannot see.

Confirmed defect: duplicate bare CDB keys are first-wins, while many weapon NIFs use generic material stems. `FindCdbMaterialLocked` therefore returned whichever weapon happened to appear first in the CDB for unrelated weapons, and accepted the map before the fuzzy family-scoped resolver could help. Confirmed examples:

- `materials/weapons/{breach,bridger,microgun,razorback,rocketlauncher,urbaneagle}/decals.mat` resolved to Solstice Ember decal art.
- Regulator compensator resolved to Tombstone's compensator; multiple weapon families' muzzlebreaks resolved to Urban Eagle.
- Bridger, Kodama, Lawgiver and Tombstone reflex sights resolved to RocketLauncher reflex-sight art; several long barrels borrowed Regulator art.
- Generic `grip`/`stock`/`barrel` first-wins entries can resolve to Regulator, Bridger, or MagStorm texture paths regardless of the requesting material; the previously added family guard caught some, but admitted unrelated shareable or token-overlap cases.
- Two duplicate compound keys (`mods_grip`, `mods_supressor`) have a Razorback color map but Microgun opacity map; material-family rejection of the color entry now prevents that poisoned CDB pair from being used for Razorback, allowing its authored exact .mat route/family fallback.

Changed `CdbWeaponTextureMatchesFamily` to require weapon family evidence in the map's weapon directory (not merely a shared part-name token). The deliberately shared AWP maps remain permitted when the asset path explicitly contains `awp_shareable`/`awp_master_shareable`. A rejected CDB color result proceeds to the existing fuzzy resolver, which only considers material-family-scoped candidates for these weapon materials. Regression cases cover the wrong Solstice/Breach decal mapping, unrelated Tombstone/Regulator compensator map, MagStorm barrel trap, and explicit shared-AWP positive case.

## Additional audit observations

- The 140 BA2s under the enabled MO2 Test profile plus 110 base-game BA2s were included for the weapon material-path inventory. Weapon-specific texture-pack presence and CDB path correctness are separate; some enabled mod NIFs can reference uninstalled textures, and some mod materials are loose files rather than base CDB records.
- A mod-CDB merge audit found 23 Creation Kit databases with 508 new keys and 532 named color maps, but its check only looked for those maps inside the archive that contained the CDB, reporting zero there. This is a known false-negative for CK packs split into `main.ba2` and `textures.ba2`; it is not evidence that all mod maps are absent. Mod CDBs remain separately evaluated against the active archive/loose-file resolution path.
- No wide code-wide “all renders” claim is made: 42,933 base material sets and 1,333 archived weapon NIFs were audited at data/map ownership level, not every item’s appearance in the shop UI. Non-weapon meshes, item assembly selection, PBR parity/lighting, geometry omissions, and NIF UV correctness need their own end-to-end capture validation.

## Full-audit output artifacts

The complete raw reports were written to the MO2 overwrite directory:

- `SFShopExplorer-material-report.tsv` — verdict per CDB material from the 25 base texture archives.
- `SFShopExplorer-material-sweep.tsv` — full sweep including format/dimension checks using all 110 base BA2s.

The executable regression suite and source build still need to be rerun after the family-guard changes before treating this report as a finished repair.