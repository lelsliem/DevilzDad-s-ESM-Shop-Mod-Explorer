# Material matcher audit — 2026-10-17

Review of the offline renderer's texture resolver: `FindDiffuse` and its helpers
(`LiveModel.cpp:4105-4990`), the CDB family gate (`LiveModel.cpp:4024-4103`),
and the prefetch layer that consumes them (`OfflineModel.cpp:1768-2060`).

Method: read the whole resolution path, then check every decision against one
session log (`SFShopExplorer.log`, 08:16-08:20, 19,874 lines, 79 prefetch rows,
28 distinct materials) and against the shipped archives where a texture looked
suspicious. Counts below are from that session unless stated.

This is a review, not a change. One finding has since been fixed (F2, marked
below); the others are still open.

## Resolution order (what actually runs)

`FindDiffuse` tries these in order and returns at the first hit:

| # | Path | Code | Score | This session |
|---|---|---|---|---|
| 1 | Session memo (mat + node) | 4124 | -1 | 0 logged (first hits only are logged) |
| 2 | Hardcoded exact rules (plasma cutter, vortex, crucible, Breach, Razorback, Regulator, celery, succulents, pot, action figures, poster, playing card, book) | 4151-4390 | 2000 | 19 |
| 3 | Material database (CDB) texture set | 4393-4434 | 1900 | 28 |
| 4 | Card/playing-card neutral guard | 4435-4446 | - | 0 |
| 5 | Exact base match, family-scoped (glass may cross families) | 4667 | 1000+len | see fuzzy |
| 6 | Containment match (min 3 chars), extended family scope | 4667 | len | see fuzzy |
| 7 | Token overlap, extended scope | 4667 | shared token | see fuzzy |

Family scope itself: `familyScoped` (`LiveModel.cpp:4494`) and the sibling/
DLC variant `familyScopedExt` (`LiveModel.cpp:4507`).
| 8 | Dir-dirname candidates (only if nothing matched) | 4790 | 3 | 0 |
| 9 | `_opacity` pool candidates (only if nothing matched) | 4800 | 1 | 0 |
| 10 | 5-char family fallback (only if nothing matched) | 4809 | -2 | 0 |
| 11 | Family singleton (only if nothing matched) | 4842 | -3 | 0 |
| 12 | Flat-kind refusal gate (plastic/glass/decal/...) | 4877-4956 | - | 0 ("stays flat") |

Of the 79 prefetch rows: 19 exact rules, 28 CDB sets, 32 fuzzy scores. The
standalone fallbacks (8-12) did not fire, so this session says nothing about
their accuracy — only about their dormancy.

## Findings

### F1 — A 4x4 placeholder albedo is accepted and stretched (visible, evidenced)

`Materials\Weapons\Coachman\Coachman_Decals.mat` resolves through the CDB to
`textures/weapons/coachmanshotgun/decals_color.dds`. The archive entry is a
**4x4 BC1 with a single uniform block**:

```
Starfield - Textures10.ba2  TexFile(width=4, height=4, mips=3, dxgi=72)
first block f6c5f6c500000000 -> c0 == c1, all indices 0 -> RGB (197,190,180)
```

So the decal surface samples one flat pale grey over its whole UV set. (This
figure was first written as (197,123,180) "pink-mauve" from an ad-hoc 5-5-5
unpack; BC1 endpoints are RGB565, and the block is now confirmed fully uniform at
191 luma through the corrected `dump_tex.py` decoder.)

```
[08:19:44.998] offline GPU: prefetched 'Materials\Weapons\Coachman\Coachman_Decals.mat'
               <- textures/weapons/coachmanshotgun/decals_color.dds (score 1506)
[08:19:44.998] offline GPU: uv '...Coachman_Decals.mat' v=8 u=[0.045,0.677] v=[0.048,0.328] tex=4x4
```

The renderer already refuses degenerate **masks** — `kMinAuthoredMaskDimension
= 8` at `OfflineModel.cpp:2005` — but there is no equivalent guard for a colour
map. The asymmetry is the bug: a 4x4 mask is treated as a placeholder, a 4x4
albedo is treated as art.

Suggested fix: apply the same 8x8 floor to the colour map, fail closed to the
range's base colour (or the CDB tint), and log it once per material. Risk: low,
but any legitimately tiny albedo would start rendering flat, so log loudly.

### F2 — The CDB cross-family gate rejects the game's own answer (17 hits, 5 materials)

`CdbWeaponTextureMatchesFamily` (`LiveModel.cpp:4024`) accepts a CDB texture
only if the texture path contains `/weapons/<material family>/` or
`/weapons/<family>_` (line 4097). Shipped weapon textures are not always in a
directory named after the material folder:

```
Materials\Weapons\Coachman\Coachman_Grip.mat
  CDB -> textures/weapons/coachmanshotgun/coachman_grip_color.dds   REJECTED
  fuzzy fallback -> textures/weapons/coachmanshotgun/coachman_grip_color.dds (score 2513)
```

All five Coachman materials are rejected every time they are resolved (17
log lines total for the session):
BarrelStandard, Decals, Grip, Sight, StStock. Today the result is still correct
because the sibling-scope fuzzy rule (`familyScopedExt`) finds the same files
and the score shows it (1223-2513). Two real costs remain:

- the CDB's **opacity** pairing is discarded (`Coachman_Decals` lost
  `decals_opacity.dds` from the CDB and only got it back via the derived
  `_opacity` guess), and
- every node re-runs a full archive scan for a map the database already named.

Suggested fix: also accept when the texture's weapon directory **starts with**
the family token (`coachman` -> `coachmanshotgun`), keeping the
`/weapons/` requirement and the generic-token blocklist. The case the gate was
built for stays rejected: `magstorm` does not start with `regulator`.

**Fixed 2026-10-17.** `WeaponDirNamesFamily` (`LiveModel.cpp:4036`) now supplies
that evidence, needing a segment to continue the family token by at least three
characters. Directory segments only - a file name is still not evidence - and
the blocklist is unchanged. Census over the whole shipped database: the CDB
contains exactly 10 family-token/weapon-directory prefix relations
(`coachman`/`coachmanshotgun`, `orion`/`orionrifle`, `pacifier`/`pacifiershotgun`,
`inflictor`/`inflictorpistol`+`rifle`, `arboron`/`arborondagger`,
`side`/`sidestar`, `hardta`/`hardtarget`, `ammo`/`ammocounters`,
`lasercutter`/`lasercutter_worn`), every one of them the same weapon, and the
three recorded cross-family traps are rejected before and after. Regression:
`test_weapon_map_in_a_family_extension_directory_is_accepted`,
`test_weapon_family_directory_evidence_is_scoped_and_directional`,
`test_shipped_coachman_sets_are_accepted_for_their_material_family`, plus the
compiled-gate differential in `tools/ba2_audit/family_gate_diff.py`
(4,957 pairs, 0 disagreements).

### F3 — Derived `_opacity` guesses are mostly nonexistent files (50 of 51)

When the resolver returns a `_color.dds` with no companion, the prefetcher
derives `_opacity.dds` from the same stem (`OfflineModel.cpp:1965`) and reads
it. In this session 51 opacity reads failed and **50 were `fmt=0 0x0`** — the
file does not exist. Verified in the archives for two of them:

```
textures/weapons/coachmanshotgun/coachman_barrelstandart_opacity.dds  NOT FOUND
textures/weapons/sfbgs050/breechblock/mag_opacity.dds                 NOT FOUND
```

Effect: no mask (correct outcome), but 50 doomed archive lookups per session
and 50 rejection lines that drown the 27 real pairs.

Suggested fix: negative-cache derived opacity paths per session so a repeated
guess is free, and log a missing *guess* at debug level rather than as a
rejection. Keep the current treatment of real CDB opacity keys.

### F4 — The `_opacity`-pool fallback cannot survive the offline renderer (latent)

`FindDiffuse` step 9 (`LiveModel.cpp:4800`) searches the opacity index and can
return an `_opacity.dds` as the albedo — the documented e-ink glyph route.
`OfflineModel` then rejects any single-channel map as an albedo
(`OfflineModel.cpp:1879`) unless `promotedMask` is set, and this path never
sets it. So the route always ends in "rejected single-channel map as albedo"
plus a flat range. No occurrences in this session (0 such rejects), so it is
untested rather than proven broken.

Suggested fix: mark a hit that came from the opacity pool as a promoted cutout
(the same flag the CDB "opacity only" set uses).

### F5 — `VariantNum` is called on a full path, not a base (code-level)

`LiveModel.cpp:4629` calls `VariantNum(LowerAscii(a_matPath))`, but
`VariantNum` returns the **first** run of 2+ digits anywhere in the string and
documents a base input. For `Materials\SetDressing\SFBGS00D\...` that is `00`;
for `.../512x1024/poster_512x1024_02.mat` it is `512`. The +400 variant bonus
then compares these against `VariantNum(e.base)` and can prefer the wrong
variant, or award the bonus to unrelated bases that share the number.

No wrong pick observed this session (Coachman has no digits in its path, and
the poster is handled by an exact rule first). Fix: compute the variant from
the normalized base, i.e. the same string `BaseVariants` produced.

### F6 — The 5-character family fallback is still a guess (dormant)

`LiveModel.cpp:4809-4832` takes the shortest family-scoped map whose base
shares the first 5 characters with any candidate, and marks it `score -2`
(not a real match). Raising 3 to 5 characters stopped `AKWoodVeneerWorn01`
taking `akwrightsshopsign` (now correctly untextured), but five characters is
still weak: `genboo*`, `coach*`, `stal*` style prefixes will collide inside a
family. Dormant this session (0 hits).

### F7 — The singleton fallback trusts a one-entry family (dormant)

`LiveModel.cpp:4842` takes the family's only `_color` map when nothing matched,
`score -3`. The flat-kind gate still runs afterwards, so plastic/glass/decals
fail closed, but any other one-texture family is accepted without a token or
art check. Dormant this session (0 hits). This is the rule that most easily
produces a confidently wrong texture on a new item type.

## What checked out

- 28 CDB-set resolutions: every one lands in the material's own tree or a
  deliberately mapped subdirectory. The three cross-directory picks are
  `crops -> hydroponics` (HydroCabbage, CDB answer), and the two exact rules
  `exoticplayingcardbox01_trim -> common/metal` and
  `abstract_poster2x1 -> 512x1024`.
- 19 exact rules, including the playing-card symbol atlas with its mask and
  the Vortex Grenade shared atlas.
- 27 opacity pairs. The single non-missing rejection is a valid BC4 map with a
  2:1 aspect against a 1:1 colour (`digipick_decal_material_opacity.dds`,
  1024x512 vs 1024x1024) — refusing it is correct, since the content is a
  different decal sheet.
- The 3-letter prefix bug from the previous pass stays fixed: the wood planter
  resolves to no texture and renders from its base colour.

## Limits

- One session, 28 materials. F4-F7 are dormant in it, so their verdicts rest
  on reading the code, not on observed output.
- F1's flat decal is derived from the decoded BC1 block, not from a screenshot
  of the Coachman preview, so what it looks like under the preview's lighting is
  still unmeasured. The block itself is confirmed uniform: the whole 4x4 is one
  colour, 100% of pixels, after the decoder fix below.
- The matcher was reviewed by reading; no fuzzing or differential test against
  the game's own renderer was run.
