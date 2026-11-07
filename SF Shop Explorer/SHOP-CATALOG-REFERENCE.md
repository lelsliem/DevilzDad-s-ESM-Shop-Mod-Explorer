# Shop Catalog Reference

Date: 2026-10-16 (model-resolution note); catalog census below remains the
2026-09-13 snapshot unless explicitly dated. Sources: project docs,
`SFDevilzDump-all-forms.csv`
(2026-09-13, 21MB), `SFDevilzDump.log`, `SFDevilzDump-catalog.csv`,
`SFDevilzDump-render-candidates.csv`, live `SFShopExplorer.log`, keyword
census + name census run today. MO2 mod folders hold no project docs
(third-party readmes only); the live profile is `Shop Test Render`.

## 1. Document inventory (project)

| Doc | Covers — key facts reused here |
|---|---|
| `SF Shop Explorer/SHOP-RENDERING-NOTES.md` (sections 1-55 plus the 6r-5x run log) | Authoritative renderer handoff. Offline 3D preview (own rasterizer from `.sfmodel` cache plus the direct NativeModel -> GPU path), texture matcher rules, `materialsbeta.cdb` findings for cards and glass, weapon orientation/camera, basket, catalog and UI fixes. Current truth, read first. |
| `SF Starfield MCM/INVESTIGATION_NOTES.md` | Early native-shop history (cursor/input/draw-stage fixes, form-array scan). Superseded for rendering; still valid for input/overlay integration. |
| `SF Starfield MCM/RENDERER_AND_SHOP_PLAN.md` | Old OSF/WebView-era plan. Reference only: candidate counts (MISC 6530 / ARMO 1650 / BOOK 1463 / ALCH 1141 / WEAP 465 / KEYM 381 / AMMO 155), retired routes (ContainerMenu handoff, BMP bridge). Do NOT revive. |
| `SF Starfield MCM/COMMONLIBSF_RENDERER_AUDIT.md` | Why no native scene embedding (unresolved InventoryMenu event IDs, no public scene API). Justifies the offline renderer. |
| `SF Starfield MCM/tools/ba2_extract/inventory_renderer_notes.md` | Scaleform flow (`uHandleID` -> Select/LoadModel -> PreviewSceneRect), handle table. Basis for any future live-instance work. |
| `SF Shop Explorer/README.md` | Build/deploy/controls. Production state summary. |

## 2. Dump inventory

- `SFDevilzDump-all-forms.csv` — every loaded record: plugin, formID, type,
  name, editorId, value, weight, modelPath, keywords, flags. `modelPath` is
  now the first resolved NIF for ARMO/WEAP/AMMO/MISC/BOOK/ALCH/KEYM (the old
  `armor-addon-present`/`weapon-data-present`/empty-placeholder behaviour is
  gone), and the catalog view carries the complete list in `nifPaths`.
  **Still a caveat:** `editorId`/`keywords` are empty for ALCH/MISC/BOOK/KEYM
  rows (dump limitation, not game truth — the shop reads both live).
- `SFDevilzDump-catalog.csv`, `-render-candidates.csv` — older filtered
  views, superseded by the live catalog.
- `SFDevilzDump*.log` — dump run diagnostics.
- Live `SFShopExplorer.log` — per-item request/resolve/texture lines
  (`prefetched 'mat' <- tex (score N)`), hidden-filter counts, assembly
  lines. Primary ground truth for what renders.

## 3. Catalog stats (today's CSV)

ARMO 1217 / BOOK 1463 / MISC 2455 / WEAP 467 / AMMO 155 / KEYM 381 /
ALCH 1141. INGR: zero rows in game data (tab explains, points to Aid).

## 4. Keyword taxonomy (WEAP + ARMO — dump keywords ARE present here)

Weapons (`WeaponType*`, `DisplayObjectType*`, `ma_*`, manufacturers):
Pistol 34 / Rifle 47 / Shotgun 14 / Melee1H 21 / HeavyGun 18 / Laser 29 /
Sniper 9 / Minigun 7 / Mine 6 / Thrown 12 / Unarmed 61 / Automatic 27 /
Explosive 27 / ParticleBeam 22. Manufacturers: CombaTech 54 / Allied 32 /
Laredo 21 / Varuun 12 / Arboron 7 / Kore 7. Hands: `s_40_Hand_Rifle_Grendel`
17, Equinox 6, Eon 6. Mod flags: `ma_*` (Grendel 9, Orion 6, HardTarget 6).

Armor: Apparel 578 (NakedBody 494) vs Spacesuit Body 123 / Helmet 115 /
Backpack 107; categories Apparel/ArmorSuit/ArmorHelmet/ArmorBackpack;
factions (Starborn 16, Ranger 12, Varuun/Miner/Explorer `ma_*`).

Aid/Misc/Books/Keys: no dump keywords — use §5 patterns (shop reads live
keywords where present, e.g. succulent swap keys).

## 5. Name-pattern taxonomy (dump names — EDIDs blank here, use live names)

- ALCH food: meat 180, fresh 65, milk/drink/meal/steak/sandwich/cream.
  ALCH meds: Med Pack, Bandages, Injector, Antibiotic, Heal Gel, Immobilizer.
- MISC flora: fertile 424, plant:/seed/tissue/tree/aquatic; toys: plushie
  28, toy/action-figure; cards: card/playing 50+; containers: crate/empty
  packs; resources: Lead/Iron/Silver/...
- BOOK: slate 173, log/journal/notes/report/guide/survey/evidence.
- KEYM: keycard/card/access (56/34/22), passcode/password/code, key.

## 6. Sub-category filters (implemented)

Each item category now has a toolbar subfilter like Weapons and Armor.
Filters are case-folded and compose with plugin selection, the global
search box, sorting, and the existing hide-test/hide-ship toggles. Unknown
records remain reachable under `Other` (Armor is the exception: its only
untyped records are actor skins and are hidden outright); choosing a subcategory resets the
list selection and page. Weapon and Armor remain keyword-first. The other
categories use the live display name, editor ID, and available keywords,
with ordered name-pattern rules and conservative word-boundary matching.

- Weapons: Pistols / Rifles / Shotguns / Snipers / Heavy / Melee / Thrown /
  Other (keyword-based; sniper priority settles dual-tagged rifles).
- Armor: Spacesuits / Helmets / Hats-Caps / Accessories / Backpacks /
  Apparel / Other (keyword-based; see the split below).
- Ammo: Ballistic / Energy-Particle / Missiles / Mines-Grenades /
  Ship-Creature / Other (caliber and ammo-family names).
- Aid: Food-Drink / Medical / Chems / Other.
- Misc: Flora / Toys / Cards / Resources / Containers / Décor / Lab / Creature /
  Tech / Slates / Food / Artifacts / Animal / Other. The eight buckets after
  Containers are model-folder groups measured on 2026-10-24: the first model
  pass left 1,417 records in Other because whole shelves (`ScienceGlass`,
  `tankgirlck\xenologymesh`, `manufactured_goods`, `Items\DataSlate`,
  `FoodDrink_Set`, `SetDressing\Earth`/`Artifacts`, `AnimalGenericIngredients`,
  the office/kitchen/poster clutter) name no category in the record. They read
  the NIF sub-folder only - no name terms - and take Other from 55% to 17.7%,
  then to 16.1% once the 0.8.349 stragglers below moved.
- Books: Slates / Notes-Reports / Logs-Journals / Guides-Books / Misc-Other.
  The NIF decides the first split since 2026-10-18: a dataslate model is a
  slate, a book model is a book. Keywords cannot do it - every printed book
  (Race to the Heavens, BookLarge01.nif) also carries ObjectTypeDataSlate.
  A Posters bucket was tried and removed: those records are dataslates.
- Keys: Keycards-Access / Keys / Codes-Passcodes / Other.

### Misc and Aid read the NIF too (2026-10-24)

Names alone filed 61% of Misc and 58% of Aid under Other. The census
(`tools/catalog_bucket_census.py`, run over the 2026-10-24 catalog: 2,579 Misc
and 1,189 Aid records with a name and a model) showed the reason: whole product
lines carry no category word in their display name, and MISC/ALCH records carry
no keywords at all in the dump. The Sabacc deck is named "Sabacc Card", Star
Wars ores are named after the metal (`Lead`, `Neutronium`, `Coaxium`), the
collectible globes are named after what is inside them, and the food families
glue the word ("QuickSnack", "Synthameat") or drop it ("Ripe Jellyfruit").

So both classifiers now take the folded model path as a second argument
(`CategoryModelText`, which is just `FormModelPath` folded) and check measured
model tokens after the name terms for each bucket:

| Bucket | Model tokens |
|---|---|
| Misc / Toys | `collectables`, `snowglobe`, `figurine`, `\toys\`, `desktoy`, `cutetoy` |
| Misc / Cards | name term `sabacc` (108 records; Cards had been empty) |
| Misc / Resources | `landscape\resources` (the ore chunk NIF, 17 records), `lootcontainers\` (the Inorg* gas/liquid containers, 14) |
| Aid / Meds | `affliction_cures`, `bacta`, `trauma_pack`, `panacaea`, `medstim` |
| Aid / Food | `fooddrink`, `xenoplantpod`, `\xenologymesh\food\`, `coffeetogo`, `tranquilitea`, `foodanddrink`, `moonshine`, `cocktailstation`, `foodpouch`, `foodpackets`, `itm_milk` |
| Aid / Chems | `\chem_`, `inhailer`, `itm_potion` |

Order matters and is pinned by the test: Meds is decided before Chems because
bacta ships the same chem-inhaler NIF; the Food name term `heart` is decided
before the Chems model token because Giant Heart (a cooking ingredient) uses
`crafted_inhailer_amp_01.nif` too. Measured result on that catalog:

| Category | Buckets after the change |
|---|---|
| Misc | Flora 654, Toys 120, Cards 108, Containers 94, Resources 200, Décor 321,
Lab 223, Creature 125, Tech 137, Slates 48, Food 57, Artifacts 48, Animal 28,
Other 416 (16.1%) |
| Aid | Food 1,039, Meds 51, Chems 53, Other 46 (4%) |

Three folders held two families each and filed both together until 2026-10-24
(0.8.349): the SFBGS003 mission board terminals were sharing a token with the
ammo primer boxes ("Bounty: The Vulture" beside "Charged Diodes"), the xenology
folder's Old Earth PC props were filed as creature parts, and the Inorg* loot
containers were loose in Other. The tokens are now the sub-folder
(`mission_board_terminals`, `ammoprimerbox`), the PC file family
(`aaa_base_pc`, read before the `xenologymesh` folder rule), and
`lootcontainers\`.

What stays in Other is deliberate: single-prop clutter with a one-off NIF
(Data Spike, Dumbbell, Tissues), loose components no bucket claims, and the
creature-part ingredients that name a body part ("Ankylosaurus Tongue") rather
than a category. The
fixture and mirror are `tools/fixtures/misc_aid_catalog.csv`,
`tools/gen_misc_aid_fixture.py` and `tools/test_misc_aid_filters.py`.

### Armor subcategory order (2026-10-18)

Order is Accessory > Suit > Helmet > Pack > Apparel > Hat > Other, and each
step exists for a measured reason:

- **Accessory first.** `ArmorTypeApparelHead` is the head *slot*, not a helmet:
  84 caps, hats and masks carry it, and so do Hal Caruso's Glasses and all ten
  Neuroamps (`ArmorTypeNeuroamp`). Their own keyword or name must win, or
  NeuroBoost Mark II files as headwear. Eyewear terms: glasses, eyewear,
  goggles, monocle, sunglasses, spectacles, eyepatch.
- **Suit before helmet.** A full suit also carries `ArmorTypeSpacesuitHelmet`
  and `ArmorTypeSpacesuitBackpack` because those parts ship with it. All 24
  ARMO records holding both body and helmet keywords are suits - that is the
  Starborn family (Astra, Materia, Locus, Tenebris, Bellum, Avitus, Gravitas,
  Venator, **Tempus**, **Solis**), the Terran robot armors and Monster Costume.
  The earlier helmet-first order filed every one of them under Helmets.
- **Garment names before hats.** "Miner Hard Hat Outfit" is the outfit, not the
  hardhat, so outfit/jumpsuit/uniform/dress names resolve to Apparel first.
  Crowd records named "Beanie" carry `ArmorTypeApparelOrNakedBody` instead of
  `ArmorTypeApparelHead`; the name tokens run before that body keyword so they
  still land in Hats / Caps.
- **Legendary pool unchanged.** `ArmorLegendarySlot` keeps helmet, hat and
  accessory on pool 1 (the head-slot pool they shared before the split), so a
  filter change does not silently change which effects a cap can roll.

### Actor skins are not catalog armor (2026-10-18)

Armor/Other held nothing but actor skins: Terrormorph, Groat, Heat Leech, Grub,
MiniBot, `SkinModelA`/`SkinModelS` human shells, turret corpses and the
`SFBGS001_Skin_*` variants. They rendered as a bare creature or robot chassis
and had value/weight 0, because they are the record an NPC's skin uses, not an
item anyone can hold. The hide toggles could not catch them: most carry no
matching identity word, and the turret corpses have no display name at all, so
the name-gated filters skipped them (`Skin_Turret_Laser_01_ShortCorpse`).

The rule is structural rather than name-based: an ARMO with no
`InventoryCategory*` and no `ArmorType*` keyword is a skin, and
`BuildFormList` drops it before classification, independent of every toggle.
The split is exact on the test profile - all 332 such records are skins and all
881 wearable records carry one of the two keyword families, whose longest list
is 360 characters. Only four skins classify as anything else (`SFBGS001_
Spacesuit_Redeemed_*`, name-marked NON PLAYABLE and carrying
`DestructibleArmorKeyword`); they resolve to Spacesuits by name and are hidden
with the rest. Armor/Other therefore reports zero rows and says so in the
header instead of looking like a broken filter.

Live counts on the 2026-10-18 catalog (1213 ARMO rows): Apparel 484, Other 328,
Spacesuits 130, Helmets 112, Backpacks 90, Hats / Caps 57, Accessories 12.
After the actor-skin exclusion the same catalog shows 881 armor rows: Apparel
484, Spacesuits 126, Helmets 112, Backpacks 90, Hats / Caps 57, Accessories 12,
Other 0.

These are practical catalog filters, not authoritative record metadata:
matching is intentionally text-based for categories whose dump has no
keywords. Priority order is explicit in the category classifiers, and every
unmatched item remains available in All or Other. Review live in-game counts
and representative rows before treating patterns as exhaustive; expand the
term lists from concrete misses rather than broad substrings that can pollute
neighboring categories.

Texture rules that are proven rather than guessed: the symbol decal on exotic
playing cards, the abstract-poster map behind the card back, the letters decal on
the deck box, and the whole produce and aid families come from named texture sets
in the shipped material database (`materialsbeta.cdb`), which is consulted before
the fuzzy matcher. Materials whose texture set is empty (card plastic, card box
trim, the blister glass) are left neutral instead of being fuzzy-matched to
neighbouring artwork. This is what fixed grey tomato/onion/potato: their maps live
in `ProduceBox_ProduceOnly`, a sibling directory the fuzzy scorer cannot reach.

Implementation: `src/ShopExplorer.h` defines the subcategory enums/names;
`src/ShopExplorer.cpp` owns classifiers, toolbar combos, and the composed
`BuildFormList` filtering. No catalog or transfer behavior changes.

Toolbar layout: category, subcategory, quantity/free, theme and
maximize/close controls flow left to right on one row. A control wraps onto a
second row only when the remaining width cannot hold it, and combo widths are
bounded so a wide window does not stretch them into full-width bars.

Preview correctness note: keep preview selection locked to the clicked form,
not the hovered table row. Hover-driven preview can show a Playing Card while
the detail panel still labels an unrelated selected item, appearing to be a
bad model assignment. For a missing/mismatched item model, show an explicit
unavailable status and the resolved form-model path rather than implying a
generic/stale mesh belongs to that form.

### Form model path versus resolved asset key (2026-10-16)

The runtime carries the selected form's model path separately from the asset
key used to read the NIF. `LiveModel::NormalizeNifKey` lowercases and converts
backslashes to `/`, removes leading `/` characters, strips repeated leading
`data/` virtual roots, collapses repeated `meshes/` roots, then adds `meshes/`
only if no mesh root exists. It rejects rooted `geometries/...` keys, `..`
path components, colon/drive-qualified segments and non-`.nif` inputs. This
avoids BA2 name-table misses caused by Windows-style form strings while leaving
the selected record's identity untouched. Loose asset precedence remains
configured extra directories, enabled MO2 mod directories, base `Data`, then
mod-first BA2 archives (as documented in the renderer notes).

This is normalization, not model discovery: it does not substitute a similarly
named NIF for a different form, and key acceptance does not prove a file exists
or that its geometry can be parsed. The latest catalog audit did not capture a
runtime selection log for `Sprout in a Jar` (MISC `040001C6`) or related
TankGirl sprout records, so do not infer their exact model from a matching
filename. The installed TankGirl Xenology BA2 contains distinct jar and
plant-pod NIF candidates; verify the form's actual path in a live log before
curating aliases. Focused source regression: `python -m unittest
 tools.test_weapon_mod_selection` from the Explorer directory.

The counts and broad catalog snapshot in this document are historical; check
the date on each generated audit before treating it as a current installed-mod
inventory.

Preview sizing: clicking the preview pops it out into its own window over the
catalog; clicking anywhere outside that window returns it to the inline strip in
the details column. The earlier full-workspace lightbox and the in-place growth
were both removed - expansion must not hide the catalog.

A future refinement can add per-category totals in the dropdown after
runtime browsing confirms the classifiers against installed mods.

## 7. Texture coverage status (2026-09-13 audit: 1317 mats, 317 miss)

Matcher is rule-complete for diffuse `_color` maps (28-case battery green).
Remaining miss classes: emissive-only maps (eink/lit/animated screens —
index covers diffuse only), rebrand families (Grendel decals etc.),
generic single-word mats (grip/sight/receiver with no family). Tint-side
(keycard/book/ammo hues) and v2-archive textures (DLC/mod) are separate
tracked gaps, as are PBR shine and player-mod assembly.

## 8. Open questions for the next evidence round

- Equinox rifle base NIF (scopes only in cache); dogfight base (parts only).
- Poster card-back bridge: RESOLVED 2026-10-04. The exotic playing-card
  NIF authors its back surface with `Abstract_Poster2x1_02.mat`, and the
  material database base texture set for that family
  (`Abstract_Poster2x1_TextureSet1`) names
  `Poster_512x1024_02_color.DDS` as the colour map, so that map is the
  game's own card-back albedo rather than an unrelated poster. The card's
  plastic body and box trim have empty texture sets and stay neutral.
- Action-figure blister glass: `GlassCleanTransparent02_TextureSet1`
  carries no textures at all, so its transparency lives in material
  component flags (`IsGlass`, `Frosting`, `MaterialOverallAlpha`) that are
  still undecoded. No blend constant has been invented for it.
- CDB tint decode (green/yellow/gold hues).
- "50 images then break" symptom (no crash on record; backlog starvation
  was fixed by cache restore — confirm gone).
