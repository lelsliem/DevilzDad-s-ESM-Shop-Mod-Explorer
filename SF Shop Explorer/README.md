# SF Shop Explorer 1.0.0 (v1 native)

A native in-game Starfield item browser and shop implemented with Dear ImGui and DirectX 12. It does not use OSF UI, WebView2, HTML, SWF, or a browser bridge.

Build: 2026-11-03 17:38 (x64, releasedbg).
Plugin SHA-256: `5DBCB955CBCCECF43A6226DCA366AC4E0179169390A305E0B32D6D6FBB72122F` (2,363,904 bytes).

## What changed in 1.0.0

- **Your own music.** Tracks can be wav, mp3, flac, m4a, aac or wma; the non-RIFF formats use the Media Foundation decoder Windows ships, so no codec is bundled. A folder under `Audio\Music\` is a soundtrack: it plays in file-name order, wraps at the end, and the Audio popup gains previous / next track buttons while it is selected.
- **A custom theme and layout presets.** The tenth theme unlocks when Starfield, Hell and Heaven are earned. CUSTOMISE edits its name, backdrop and 19 colours, and the LAYOUT block offers four arrangements (basket right, centre, left, or catalog above). Both are stored in the INI and survive a restart.
- **Your own backdrop art.** Any `space_<name>.png`, `.jpg`, `.jpeg` or `.bmp` beside the plugin joins the custom theme's Background list. A file that fails to decode is not offered, so a choice always draws something.
- **Duplicate records collapse.** Records the load order defines twice become one row with a count; only priced records group, so identical plot slates and keys stay separate. The item pane lists every FormID behind the row.
- **Outfit previews assemble complete sets.** The sibling lookup takes the part token from the file name and only fills a part type the record does not already declare, so a custom upper body keeps its own lower body instead of pulling in a mismatched one.

## Controls

| Action | Control |
|---|---|
| Open / close | **PageUp** |
| Choose a plugin, type, or form | Click the row |
| Filter by category / subcategory / theme | Combos in the top toolbar |
| Set quantity, transfer for free | **Qty** spinner and **Free** checkbox in the toolbar |
| Search the form table | Search box, top right |
| Add an item | Select a form, set quantity, click **ADD TO BASKET**, then **ACCEPT** |
| Grow / shrink the 3D preview | Click the preview viewport (pops out into its own window; click away from it to bring it back) |
| Move the popped-out preview | Drag its title bar — it keeps position and size between openings |
| Turn the preview model | Drag the preview viewport |
| Zoom the preview | Mouse wheel over the viewport |
| Reset the preview camera | Double-click the viewport |

Typing needs `input=1`. The overlay synthesises `WM_CHAR` from `WM_KEYDOWN`
because the game does not run `TranslateMessage` on the path the shop is open
on; see `SHOP-RENDERING-NOTES.md` for the double-translation guard.

The toolbar controls flow left to right on a single row; a control wraps onto a
second row only when the remaining width cannot hold it, so a narrow window
never truncates them.

The plugin list is loaded from Starfield's `TESDataHandler`. Forms are filtered by record type and plugin compile index. Inventory mutation is queued from the render UI and executed by the SFSE task callback rather than directly from the Present hook.

## Build

Requires MSVC 2022, xmake, and the repository's CommonLibSF tree at `../Depends/commonlibsf-main`.

```bat
build.cmd
```

The build vendors the local Dear ImGui core plus the DX12 and Win32 backends. Build output is:

```text
build/windows/x64/releasedbg/SFShopExplorer.dll
```

> **Deploy from `releasedbg`, not `release`.** `build.cmd` configures the
> `releasedbg` mode and that is the only directory xmake writes. A sibling
> `release` directory can survive from an earlier configuration and will not be
> updated; deploying from it ships a stale binary that behaves in game exactly
> like a no-op change. This happened during the 0.8.123 UI pass and cost a full
> session of fixes that never reached the game. `build.cmd` now fails loudly if
> the `releasedbg` output is missing and prints the built DLL's SHA-256 so the
> deploy can be checked against it.

`build.cmd` configures the `releasedbg` mode on first run (debug symbols) and
then calls `xmake build -y`. Verify size and SHA-256 against the deployment
record in `SHOP-RENDERING-NOTES.md` before overwriting a deployed DLL, and never
while Starfield is running.

Before compiling, `build.cmd` runs the outfit-assembly audit when Starfield's
Data directory and a `SFDevilzDump` catalog are present. It mirrors
`LiveModel::AssembleSiblings` against the installed loose files and BA2s, writes
its report to `%TEMP%\sfse_outfit_assembly_audit.md`, and aborts the build if a
referenced record is missing a part or an assembled NIF cannot produce a LOD0
mesh. Skip it with `SFSE_SKIP_OUTFIT_AUDIT=1`; use `SFSE_CATALOG_OVERRIDE` or
`SFSE_MODS_ROOT_OVERRIDE` to point at another install. A machine without the
Data directory or catalog skips this gate, matching the archive probe.

The full local check runs the three Python unittest trees (`tools/`,
`tools/ba2_audit/`, `tools/renderer_recon/`) and then the outfit audit, returning
non-zero if any part fails:

```bat
python -X utf8 tools\check.py
```

The suites take about seven minutes; `--no-audit` runs only the tests, and
`--report <path>` changes the audit report location. The audit is skipped when
Starfield's Data directory or the catalog dump is absent, so the command still
works on a checkout that has never run the game.

For CI, add `--junitxml` to write one merged JUnit document:

```bat
python -X utf8 tools\check.py --junitxml reports\check-junit.xml
```

The result is a single `<testsuites>` document with a suite for each unittest
tree and an `outfit_assembly_audit` suite. The audit emits one case per failed
record, naming the record and its missing or non-buildable parts; a clean audit
emits one passing case. Test cases keep their class, name, duration and
failure/skip detail, so CI can publish per-suite results instead of parsing
console output. A suite that fails before writing its XML is included as an
error case rather than disappearing from the report; `--no-audit` omits the
audit case as well as the audit run.

`.github/workflows/windows-checks.yml` has two jobs on `windows-latest` for
pushes to `main` and pull requests that change a non-Markdown file under
`SF Shop Explorer`, the workflow, or `.github/patches`. Docs-only edits
therefore do not start either job; a manual dispatch always runs both. A newer
run on the same branch or pull request cancels the older one.

The Python check pins Python 3.12 and the NumPy/Pillow versions in
`tools/requirements-ci.txt`, caches pip downloads from that file, writes
`reports/check-junit.xml`, and uploads it as the `python-check-junit` artifact.
The outfit audit skips automatically on a runner without Starfield's Data
directory and catalog; asset-backed tests use their normal `skipUnless` guards.

The C++ job restores a cached CommonLibSF checkout keyed by the pinned commit
and patch hash, validates it, and applies
`.github/patches/commonlibsf-tesobjectweap.patch` idempotently. It uses the
xmake setup action's package cache (keyed by xmake version, the project package
hash and the runner platform), caches the installed xmake binary in
`.xmake-cache`, and caches xmake's compiled-object store with the action's
`build-cache` input. `xmake.lua` enables `build.ccache` only when
`XMAKE_ACTION_BUILD_CACHE` is set, because xmake otherwise disables it for
MSVC. It runs `build.cmd` with the game-data audit gates disabled. It then
builds the non-default `DevilzShopOSF` target with xmake. Each `releasedbg` DLL and its PDB is uploaded separately as the
`SFShopExplorer-releasedbg` and `DevilzShopOSF-releasedbg` artifacts.

### Crash dumps

The target sets `set_symbols("debug")`, so the link also writes
`build/windows/x64/releasedbg/SFShopExplorer.pdb` (22 MB, not shipped). Keep it:
Starfield writes its own dumps next to the game executable
(`Starfield_<date>.dmp`), and cdb resolves a plugin frame straight to a function
with that PDB:

```bat
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" ^
  -z "<Starfield>\Starfield_10-18-11-56.dmp" ^
  -y "build\windows\x64\releasedbg" -c ".ecxr; kv; q"
```

The DLL embeds the absolute path of the PDB, not the PDB itself; a dump from an
older build needs that build's PDB (same source, same flags), so rebuild before
analysing if the timestamps disagree.

## Deploy

Copy the DLL to `<Starfield>/Data/SFSE/Plugins/`, or use this MO2 layout:

```text
SF Shop Explorer/
└── SFSE/
    └── Plugins/
        └── SFShopExplorer.dll
```

The DLL reads `SFShopExplorer.ini` and `SFShopExplorerAssets\` from its own
directory, so both must sit beside it.

Remove or disable older shop DLLs that register the same feature, especially `SFStarfieldMCM.dll` or `DevilzESMShop.dll`, to avoid duplicate behavior.

### Material swaps

Starfield garments do not carry their colours on the mesh: the armor record
names `BGSLayeredMaterialSwap` forms that **rename** materials, e.g. Resort Wear's
`Outfit_Trucker_Tee_Upperbody.mat` becomes `Outfit_Trucker_Tee_Resort_Upperbody.mat`
and the Star Roamer suit becomes the white `Generic` one. The preview reads the
NIF's own material names, so it needs that table or every recoloured garment
draws as its stock parent (grey tee, camouflage jacket, blue SpaceTrucker suit).

Generate it from the plugin files and put it where the preview cache lives:

```bat
python tools\gen_material_swaps.py --data "<Starfield>\Data" --out materialswaps.tsv
copy materialswaps.tsv "<plugin dir>\SFShopExplorerAssets\models\"
```

One `"<ARMO formID hex>\t<key material>\t<override material>"` per line; the
plugin logs `SFShopExplorer swaps: N material override(s) for M item form(s)` on
first use. Regenerate after a game update or a plugin change, and delete the file
to fall back to stock NIF materials. Mods that add their own armor need their
plugin passed with `--plugin <file> --index <load-order index>`.

`tools/test_material_swaps.py` pins the material-key rule and the five garments
that exposed the bug, so a key mismatch fails offline instead of rendering the
wrong colour in game.

Swaps only choose *which* material a mesh wears. Three more rules decide what
that material then looks like, all learned from garments that rendered wrong
with the swaps already working:

- **Empty part sets inherit the family.** The Miner Utility Outfit's shirt,
sleeves and pants have authored-but-empty texture sets; only the suit family
names art (in Bethesda's misspelled `outfit_miner_utililtysuit` folder). An empty
set whose material folder names a set falls back to it.
- **A garment's shared layer floor is not its albedo.** Ryujin R&D's upper body
is authored as `textileinsulatedfoam01` with the red apron blended on top and
Tomisar's coat as bare `textilesilk01`. When a clothes set's colour sits under
`textures/common/`, the item's own family map wins and the floor is only the
fallback.
- **Garment override alpha is a layer weight.** The Chunks cap's fabric stores
`8/255`; drawing it as 3% opacity hid the cap and left its logo floating. Clothes
that do not name a see-through part (glass, visor, lens) draw opaque.

## Themes

Ten themes: six always available, three earned by using the shop (progress
persists in the INI under `[premium]`), and a custom theme unlocked once all
three earned themes are earned. The custom theme's name, backdrop, colours and
layout are edited in the CUSTOMISE window and kept under `[custom]` /
`[customcolours]`.

| Theme | Unlock metric | Goal |
|---|---|---|
| Starfield | shop opens | 8 |
| Hell | credits spent | 25000 |
| Heaven | free hauls | 12 |

Each theme carries a palette, a music track and an optional art suffix. The
suffix selects per-theme art beside the shared fallback:

```text
SFShopExplorerAssets/
├── Chest.png  space.png          shared, used by the six basic themes
├── Chest_heaven.png   space_heaven.png
├── Chest_hell.png     space_hell.png
└── Chest_starfield.png space_starfield.png
```

A missing per-theme file only warns and the shared texture is used, so adding a
theme's art is additive and needs no code change. All art is loaded through WIC
and uploaded as `R8G8B8A8_UNORM`; chests are downscaled to a 320px long edge
before upload because they draw at 160px, while the 1920x1080 backdrops keep
native resolution.

Backdrops vary hugely in brightness between themes, so their strength is a
setting rather than a constant — see `[overlay] backdropAlpha` below. A single
hardcoded value that flatters a dark nebula washes a bright one out through the
semi-transparent panels until the text is unreadable.

## Settings

All keys live in `SFShopExplorer.ini` beside the DLL.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[overlay]` | `toggleKey` | `33` | VK code that opens the shop (33 = PageUp) |
| `[overlay]` | `uiScale` | `1.0` | Scales font, paddings and the fixed shop heights together (0.75–2.0) |
| `[overlay]` | `backdropAlpha` | `150` | Strength of the themed backdrop, 0–255. Lower it for bright art |
| `[overlay]` | `render` | `0` | 4 = full native draw + input; 3 = init verification only |
| `[overlay]` | `input` | `0` | 1 = capture mouse and keyboard |
| `[offlinepreview]` | `gpu` | `1` | 1 = GPU rasteriser, 0 = CPU path |
| `[offlinepreview]` | `width` / `height` | `512` / `384` | Render target size |
| `[offlinepreview]` | `pitch` / `yaw` / `zoom` | `-22` / `32` / `2.05` | Camera for non-weapon models |
| `[offlinepreview]` | `bg` / `bgg` / `bgb` | unset | Model backdrop, 0–255 per channel. Unset = dark slate |
| `[weaponview]` | `wyaw` / `wpitch` | `40` / `20` | Camera for weapon-oriented models |
| `[itemcapture]` | `mode` | `0` | 2 = show the engine's own inventory scene instead of the direct render |
| `[premium]` | `opens` / `spent` / `free` | — | Unlock progress, written by the plugin |
| `[logging]` | `verbose` | `0` | 1 = include the preview RTTI field-scan and node-walk probes (`REX::DEBUG`). Leave 0 for play: they are diagnosis-only and can write hundreds of lines per item |
| `[audio]` | `pauseDiag` | `0` | 1 = log the pause-state ring buffer every ~2s while the shop is open, on top of the two freeze-check lines every open always writes |
| `[audio]` | `musicVolume` / `sfxVolume` | `45` / `55` | 0–100 level per channel; the Audio popup writes these on slider release |
| `[audio]` | `musicMuted` / `sfxMuted` | — | Per-channel mute. When `musicMuted` is absent, the old `[shop] music` toggle is read and inverted, so an existing install keeps its setting |
| `[audio]` | `musicTrack` | — | A file name under `Audio\` (or `Music\`); empty means the visual theme's own track |
| `[shop]` | `attachMods` | `1` | 1 = apply the selected OMODs/rarity: drop a world ref, attach the OMODs, then activate it as the player so the engine's own take path moves the modified instance into inventory. 0 = skip the OMODs and add a plain item straight to inventory |
| `[shop]` | `debugSpike` | `0` | 1 = also log the mod-delivery probes (per-pick record and hex dumps). Leave 0: they write eleven lines per pick |

Two lines answer "is the world actually paused?" on every open, without a
diagnostic setting: `freeze check t0` ~1.5s after the freeze and `freeze check
t1` ~2.5s in, each with the UI pause counter, the game's own
`isGameMenuPaused` byte and the world clock delta. `t1` states whether the world
clock held (`world clock held`) or kept advancing. The hold is re-asserted from
the Present hook as well as the SFSE task, because holding the counter is what
stops the task. Sound continuing is not evidence either way: Wwise ignores the
pause counter, which is why the shop also zeroes the game's
`fAudioVolume*` prefs (`Game audio duck` lines) and restores them on close.

## Offline tooling

Two tools answer questions that otherwise cost a game launch.

### `tools/audit_records.py` - duplicates before the shop hides them

The catalog collapses records a player cannot tell apart (the **Hide duplicate
names** toggle). That rule lives in `BuildFormList`, so this tool re-derives it
from a form dump and reports what the collapse will do:

    python tools/audit_records.py                                  # the seven catalog types
    python tools/audit_records.py --type WEAP AMMO --show 0        # every group, one type at a time
    python tools/audit_records.py --all-types --json reports/dupes.json
    python tools/audit_records.py --include-free                   # the quest slates the rule skips

The rule, in one line: same **type, name, value and weight** - weight ignored for
AMMO, because the pre-ammo-weight record and its replacement are the same box of
rounds - and **a price**, because identically-labelled plot objects are different
things (62 `Crew Member's Slate`s, 13 `Dying Words`, seven `Lab Key`s). The
surviving record is the earliest plugin's newest FormID: the master's current
definition rather than a later plugin's clone, and within one file the appended
record rather than the one it replaced.

On the 2026-10-30 dump that is 543 groups and 728 rows (`--include-free` adds
126 groups the rule deliberately leaves alone). Each group is labelled `clone`
(a later plugin copied an earlier one's item, e.g. tailoredspaces'
`SPP_TS_RazorbackTrophy`) or `same-file` (one file defines the name twice, e.g.
the ammo families). `tools/test_audit_records.py` pins every clause of the rule.

### `tools/render_workbench.py` - draw items, edit a rule, watch it change

`tools/renderer_recon/contact_sheet.py` renders a shelf as a batch. The workbench
keeps it running and shows the result in a browser, which is the fast loop for
renderer fixes: the archive index (512 BA2s, ~1.4M entries) is built once, an
item then costs a few hundred milliseconds, and editing `contact_sheet.py`,
`nif_uv.py` or `aid_colour_audit.py` reloads it and redraws the same selection.

    python tools/render_workbench.py --item Razorback --item "Coffee Mug"
    python tools/render_workbench.py --type AMMO --limit 40 --size 240
    python tools/render_workbench.py --type MISC --limit 300 --once      # batch + TSV

Every pass keeps the previous image of each item, reports the mean pixel
difference per tile and shows the before/after pair side by side, so "did my
change do what I meant" is one number instead of a rebuild and a launch. The
page has a name/FormID/type selection form and a *changed only* filter;
`--once` writes a self-contained `reports/workbench/workbench.html` plus a TSV,
which is the regression shape: render, edit, render, diff the TSVs.

A full sweep is eight `--shard n/8` processes over `--limit 0`, and
`--no-tiles` keeps the TSV without the PNG tree (10,638 tiles is 88 MB) when
only counts will be compared:

    python tools/render_workbench.py --type ARMO --limit 0 --shard 3/8 --once --no-tiles \
        --out reports/item-render-audit-2026-11-03/shard-3
    python tools/render_audit_combine.py reports/item-render-audit-2026-11-03

`render_audit_combine.py` joins the shard TSVs into `combined.tsv` (de-duplicated
on form and type, sorted) and writes `summary.txt` with records / rendered /
blank / flat / textured per type plus the blank reasons. Load two sweeps'
`combined.tsv` files and key on `(form, type)` to diff a rule change: an
unchanged renderer must show no textured-to-flat regressions and no flat-to-
textured gains.

Limits are contact_sheet's: flat shading, painter-sorted, BC1/BC3/BC4/BC5 in
numpy plus BC7 through Pillow, and a material the route table and CDB both miss
is read from its own .mat (that chain took the flat Misc tiles from 95 to 44,
with 7 more partly textured; what is left has no albedo map in the database at
all - see the renderer_recon README). A dummy-base weapon (Razorback and the other Bullpup dummies) is drawn as
its OMOD assembly - receiver, grip and barrel NIFs picked by the weapon's own
keyword list, the same family binding the plugin uses in game - and a mod NIF
that keeps its geometry inline is drawn from those bytes. It is for material,
tint, silhouette and resolved paths, not for judging a hero shot. `tools/test_render_workbench.py` covers the
selection rules, the difference metric and both page modes.

## Current native milestone

- Native DX12 Present hook and Win32 input subclass are implemented.
- Dear ImGui is initialized against Starfield's renderer device and direct queue.
- Native presentation follows the DevilzDad shop reference layout with a branded header, catalog navigation, form table, item detail, basket, and transfer panel.
- Opening the shop freezes the world (UI pause counter, floor 1) and mutes the game's Core Audio session; the shop renders into its own session, so its music and stings stay audible. Nothing is written to the player's settings. Both are cleared on close.
- Audio is two independent channels: the toolbar's **Audio** popup carries a volume slider and a mute checkbox for music and for sound, plus a track picker. A chosen track overrides every theme and `(theme track)` follows the visual theme. `Audio\` stays flat - `ui_*.wav` is the six-sting sound bank and is never offered as music; any other `.wav` in `Audio\` or `Audio\Music\` is a track, and the list is rescanned whenever the popup opens.
- The shop is a standalone ImGui overlay: it opens no game menu and owns no console host, so closing it never touches a menu the player has open. The console-in-the-background idea was dropped on 2026-10-24.
- Purchases go to the player's inventory, weapons and armor included. A plain item is added with `AddObjectToContainer` and the count is verified on the spot. A modded one (selected OMODs, quality tier, armor rarity) has to pass through a world reference, because OMODs only attach to one: the basket drops the stack, attaches the picks, then **adds the stack with the ref's own extra data list** - a refcounted copy of the list the attach wrote the OMODs into - so the modified instance, rarity included, is what lands in inventory without a manual pickup, and the world ref is retired once the count confirms. Every engine take entry was tried and fails on 1.16.244 (`Activate` no-op, `RemoveItem` handle 0, `PickUpObject` faults); they remain as fallbacks, in that order, each gated on the inventory count and on the ref still naming the dropped form. `[shop] attachMods=0` skips that route and adds a plain item instead.
- Cursor/input capture remains configuration-controlled; the current Test-profile deployment uses `input=1`.
- Plugin, item-type, form browsing, armor add-on model metadata, and inventory transfers are rendered/queued through the native ImGui shop.
- Category sub-filters are keyword-first where the records carry keywords (weapons, armor) and measured everywhere else. **Misc and Aid also read the item's NIF**: their records carry no keywords and whole product lines name no category (Sabacc cards, ore chunks named after the metal, Holocron collectibles, "QuickSnack"), so the folded model path is the second signal. The 2026-10-24 census moved Aid / Other from 58% to 4% and Misc / Other from 61% to 55%, and filled the Cards bucket (0 -> 108). Measure with `tools/catalog_bucket_census.py`, pin with `tools/test_misc_aid_filters.py`; rules in `SHOP-CATALOG-REFERENCE.md` §6.
- The PREVIEW pane renders a real 3D image: the direct GPU path draws the item's native meshes with resolved textures (mesh/triangle/textured counts are shown in the pane), and with `[itemcapture] mode=2` it can instead show the engine's own inventory scene.
- Clicking the preview pops it out into its own window over the catalog; clicking outside that window returns it to the inline strip. It is deliberately neither a full-window overlay nor an in-place growth that hides the catalog.
- Texture resolution order is exact `.mat` albedo, then the shipped material database (`materialsbeta.cdb`, ~36k named material texture sets, including the companion opacity map), then a fuzzy family matcher; a material the database leaves empty renders neutral rather than borrowing unrelated art.
- Live NIF lookup normalizes form model paths to lowercase slash-separated `meshes/.../*.nif` keys before lookup: it removes leading separators and repeated `Data/` roots, collapses repeated `meshes/` roots before adding a missing mesh root, and rejects rooted `geometries/...`, `..` path components, colon/drive-qualified segments, and non-NIF extensions. Enabled loose mod files and mod BA2s retain priority over base-game assets. A valid key still does not guarantee the archive contains the NIF or that its geometry streams parse; see `tools/renderer_recon/README.md` for the current scope and runtime-check limits.
- Material lookup tries the full normalized path first, then a `data/`-prefixed form, then the bare stem. This matters: 26,748 of the database's 36,438 mappable materials are keyed by full path and their bare stem is usually not a key at all, so stem-only lookup could reach barely a quarter of them (all packaged food, crafted consumables and menu items live in the missed group).
- The material database is read from `Starfield - Materials.ba2` and indexed during startup warm-up, not on the first click. Its single entry is 17,576,587 compressed bytes with a stored size field of 105,037,616: Beth's BA2 `GNRL` size field is the *decompressed* length, so archive reads are bounded by the name-table offset. See `SHOP-RENDERING-NOTES.md` 6r-5j.
- Offline regression tests for all of the above live in `tools/ba2_audit/` (see its README): a Python replica of the archive reader, an archive probe, and a material sweep that reports which materials will render neutral and why. `build.cmd` runs the probe against one archive of each generation before compiling and aborts on failure, so a wrong entry stride or stream length is caught at build time. Set `SFSE_SKIP_BA2AUDIT=1` to bypass, or `SFSE_STARFIELD_DATA_OVERRIDE` to point at another install.
- The runtime form-array discovery (`0x0E0..0x1510` window probe) is memoised per data handler: it ran, and logged one line per type, on every form-list rebuild. On 2026-10-24 that single diagnostic accounted for 27,030 of 35,199 log lines and ~400 window reads per rebuild.
- Preview probes (the BSGeometry RTTI field scan and the node walk) are `REX::DEBUG`. The default logger level is `info`, so a normal session writes no probe lines and `flush_on(warn)` keeps the per-frame `INFO` chatter out of the file write path; `[logging] verbose=1` restores the full trace for a diagnostic session.
- Unresolved textures are remembered for the session (`g_failed`), so a missing texture no longer re-walks all 383 archives on every hover.
- Previewed armor applies its form's layered material swaps (see *Material swaps* above), so recoloured variants preview as themselves rather than as the stock garment that shares their NIF.
- NIF geometry resolves from an archive `geometries/<guid>/<guid>.mesh` entry when the BSGeometry names a printable factory path, and from the geometry block itself when it does not (Blender's Starfield exporter embeds the LOD0 `.mesh` payload inline; see `SHOP-RENDERING-NOTES.md` 0.8.334).
- The engine-preview readback (`ReadEnginePreviewChain`) walks objects the UI owns — menu, `UICellRenderer`, previewed `TESObjectREFR`, its 3D node — from the SFSE task thread. Those belong to the main thread, so every pointer load goes through a fault-gated reader (`SafeReadPtr`) and only menus the game reports **open** are walked. `CanRead`, a VirtualQuery gate with a whole-VAD cache, is a pre-filter only: after a partial decommit it still answers "ok", which is how the inventory-open CTD on 2026-10-18 happened. See
  `../../reports/preview-chain-ctd-2026-10-18.md`.
- Clicking the preview pops it out into its own window (`Preview`, draggable); clicking anywhere outside that window returns it to the inline strip. Rotating inside the popped-out window does not dismiss it.
- The temporary `ContainerMenu` handoff was tested and retired: opening that menu did not provide an item image/texture for the ImGui pane, so it is not part of the production build.
- The old OSF UI/WebView implementation in `SF Starfield MCM` is retained only as reference and is not required by this target.

This is a runtime-sensitive renderer hook. Test first with a save you can restore and keep the old plugin disabled while validating the native DLL.
