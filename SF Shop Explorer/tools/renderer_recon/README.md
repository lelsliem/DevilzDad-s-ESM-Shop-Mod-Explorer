# renderer_recon

Reverse engineering of the shipped rendering data. None of this ships in the
DLL; it exists so a claim about what Starfield's files contain can be checked
instead of guessed at.

Most tools read `extracted/materialsbeta.cdb` (105,037,616 bytes), pulled out
of `Starfield - Materials.ba2`. `aid_colour_audit.Cdb` (and everything that
imports it) also merges `extracted/creations/*.cdb` after the base blob with
the plugin's first-writer-wins precedence, so a Creation's own material is
answered by the Creation's own database instead of reporting "no record".
`sfbgs007.cdb` is not extracted; it is the only Creation that carries override
colours, so the merged override index is 39,775 keys against the live 40,583.

## Engine-analysis tools (take `--exe <Starfield.exe>`)

| tool | purpose |
| --- | --- |
| `rtti_vtable.py` | TypeDescriptor → COL → vtable resolver, per-class method list, `--callers <rva>`. |
| `db2_layout.py` | Recovers `BSComponentDB2::DBFileIndex`'s layout (and its four record types) from the class-registration functions: member offsets, `sizeof`, the `std::vector` header rule and each array's stride. `--json` writes `extracted/db2_layout.json` with the build and sha256; `--cpp` emits the constants for a build-gated dumper. Exits non-zero if the binary contradicts the result. See HANDOFF, 2026-10-30. |
| `db2_instance.py` | Where the index actually lives. `--exe` shows why no static instance exists to dereference (the class has no RTTI, and no code materialises `BSComponentDB2::DB`'s vtable), `--cpp <dir>` writes `Db2Index.h/.cpp` whose `FindAll()` enumerates the heap for indexes that validate, and `--attach Starfield.exe --dump` locates them in a running game. ~43 MB/s of heap. See HANDOFF, 2026-10-30. |
| `func_probe.py` | Disassemble one function (by `--rva` or `--class`): direct calls resolved to RVAs, RIP-relative string/RTTI loads annotated, struct writes marked. |
| `verify_barter_sink.py` | Re-derives the `BarterMenu_LoadModel` sink base offset (`menu = sink - 0x150`) the native preview reader depends on; exits non-zero if a patch moves it. |
| `rederive_engine_claims.py` | Re-checks the engine claims recorded in HANDOFF/reports (TranslateMessage, SceneManager COL, Clone3D thunk, `GetFormEditorID` slot). |
| `preview_transform_callers.py`, `context_index_attribution.py`, `preview_transform_consumer_scan.py` | The `BGSPreviewTransform` consumer research described in `reports/`. |

## Shelf audit and session audit

`shelf_audit.py` walks a shelf without rendering it and writes one row per
item (flags: no-model, untextured, flat-ranges, blind-alpha, ghost,
effect-only, decal-body, shared-map) and, with `--materials-out`, one row per
resolved material range:

    python tools/renderer_recon/shelf_audit.py --shelf misc \
        --out reports/misc-audit.tsv --materials-out reports/misc-materials.tsv

`session_preview_audit.py` reads one SFShopExplorer log and reports what the
running preview actually bound, range by range, joined back to the CDB:
authored (the material's own map), substituted (a different map was bound, with
the scorer's score), flat (textureless), glass, or missing (a CDB map the range
did not get). It is the only offline way to see `FindDiffuse`'s choices:

    python tools/renderer_recon/session_preview_audit.py --out reports/session-preview.tsv

## Offline contact sheets

`contact_sheet.py` renders a shelf without the game: MODL -> NIF -> BSGeometry ->
LOD0 `.mesh`, with every material resolved through the same swap/route/CDB/alpha
rules the plugin ships (it imports them from `aid_colour_audit.py` rather than
copying them). One PNG per item plus a filterable HTML grid:

    python tools/renderer_recon/contact_sheet.py --shelf aid --out reports/contact-aid.html
    python tools/renderer_recon/contact_sheet.py --item "Alien Genetic Material" --size 256

A tile is flat-shaded (one colour per triangle, sampled from the colour map at
the triangle's UV centroid), painter-sorted rather than z-buffered, and shows
translucency by compositing against the sheet backdrop. Clear shells - glass,
clear plastic, visor panes, display-case plexiglass - get a 0.16 preview alpha
floor and a silhouette rim from the clear-shell coverage mask (1px at 160, 2px
at 512), so an empty vial's authored 0.05 transmission does not leave a tile with
only its cap visible and the rim does not become a triangle wireframe. Rim parts
are grouped by their world-space bounds first: panes in the same NIF whose 3D
boxes touch or overlap merge even when their screen gap is wide. Each group's
screen mask is then closed by 1/20 of the tile (8px at 160, 26px at 512), so
two eye panes with a frame gap between them become one island before the edge
is taken. The ten Bacta-vial models use 1/13 instead (12px at 160, 39px at 512):
their one shell is split into two panes with a gap the default close cannot
bridge, while the same `BactaGlass` shape is also used by one-pane injectors and
sprays, so `PANE_MERGE_SCALE_BY_MODEL` keys the wider close by NIF basename. The
mask is padded by the close radius first, because a box filter that reaches the
canvas edge would otherwise leave a rim running down the side of the tile. Enclosed holes are filled last. A visor and its light strip or a
lens inside its frame therefore get one outer contour instead of a rim around
every seam; separate world boxes (a rack of vials, a different NIF) keep
separate contours, and panes farther apart than two radii stay two islands. An opaque
shell body tints and softens the rim, so coloured vials are not given a pale
outline; empty shells keep the neutral, stronger contour. A pane whose material
is opaque only by omission is recognized from its BSGeometry name as well
(`MarkVIIHelmet_MARKVII_visor`, `Lense`, `Window`); it keeps a 0.30 tint rather
than the empty-vial floor, while an authored 1.0 glass body stays opaque. ARMO
records get their NIFs from the catalog's `nifPaths` because their `MODL` is an
ARMA form link, not a path; a dressing-box (`SetDressing\...Container...`)
world model is skipped and the full linked addon set is drawn in its bind pose,
with the male copy of each `_M`/`_F` ARMA pair dropped (the engine takes the
female model when the addon has one). BC1/BC3/BC4/BC5 decode
in numpy and BC7 (DXGI 98/99) through Pillow, which the store's raw payload
needs wrapped in a rebuilt DX10 container. A DDS stored as a general BA2 entry
or a loose file is read from its own header (`AssetStore.texture`), including
its DX10 format; without that, every such mod texture looked 0×0 and drew as a
flat colour. `catalog_addons` drops an ARMO's `worldModels` (`*_go.nif`,
`*groundcontainer*`) and a male ARMA model with an unsuffixed female twin, so a
tile shows the worn addon set rather than the ground prop plus both genders; a
Misc container keeps its `SetDressing\...Container...` model, because the proxy
rule belongs to armour only. An AMMO record that names an absent
`type{a..f}_{medium,large,xlarge}.nif` crate is drawn with the shipped
`ammobox_01.nif` instead; its shell casing is kept, a placeholder that exists
is left alone, and the alias is preview-only (the game record is unchanged).

`prepare_item` mirrors `FormModelPaths`: an AMMO draws its crate and shell
casing together, a KEYM with no model gets
`SetDressing\Keycards\KeycardGen01.nif`, and an ARMO draws every linked addon
when its world model is the dressing-box proxy (or falls back to that set when
a real world model cannot be parsed). A melee dummy housing and Breech Block's
complete receiver stay first as the connect-point/placement root.

A material is resolved in four steps, and the last three each used to lose art:

1. the route table, then
2. the material's own CDB texture set. The stem lookup drops the `.mat`
   extension, like `CdbPathParts` in `src/LiveModel.cpp` - keeping it sent
   `MetalBronze02.mat` to the database's separate `metalbronze02.mat_TextureSet1`,
   which carries no maps, and the search stopped there;
3. a colour-family sibling when the material's own set is empty
   (`PlasticWornScratched02_Grey02` -> `plasticwornscratched02`, the number
   stripped before the colour word is read), and
4. the .mat file itself, which names its texture set inline. The colour slot is
   read as a role from the last name token, not as a `_color` substring: a mod
   set may be `Cantina_SmallGenerator_C.dds` or `Textures\SWProps\Datapad\color.dds`.

Of the first 400 Misc items, 95 drew flat grey with a colour map sitting in the
archive. 43 of those were BC7 (now decoded) and the steps above take the rest
from 52 fully flat to 44 fully flat plus 7 partly textured. What is left is not
a parsing gap, and the evidence is in the file:
`MetalGold01_TextureSet1` exists and holds only `PaintScratched02_rough.DDS`,
`GlowDim01Blue` and the glass families hold nothing at all - a layered material
takes its albedo from the layer's constant colour, so the base tint is the
honest render. The rest of the residue are objects the database names without a
`_TextureSet` child (`AK_Kitchenware01_Base`, `OxygenRescueMask01_Metal`,
`Hourglass_Sand`) and five mod NIFs whose .mat is an absolute
`file:///E:/...` path from the author's install.

Additive overlays (caustic/gobo/glow) and colourless decal masks are not drawn:
drawn opaque they hid the parts they sit on. Regression test:
`python -m unittest tools.renderer_recon.test_contact_sheet
 tools.renderer_recon.test_connect_points tools.renderer_recon.test_omod_assembly
 tools.renderer_recon.test_aid_colour_audit`.

## Silhouette audit

`silhouette_audit.py` scans every record with a resolved NIF - the seven item
forms, containers, and other modeled records - and counts the closed rim loops
per group at a chosen tile size, using the same `rim_groups`, `merge_panes` and
`outline_mask` as `render_model`:

    python tools/renderer_recon/silhouette_audit.py --out reports/silhouette-audit.tsv
    python tools/renderer_recon/silhouette_audit.py --type ARMO --limit 100 --size 160

Texture decode and the `.mat` colour fallback are skipped - the rim decision
comes from the material path, BSGeometry name and CDB alpha - so the scan runs
at about 80ms/item after `AssetStore.index_loose()` has indexed the 549 MO2 mod
folders once (about 15s). The 2026-11-01 run scanned 11,794 records: 989 had
rimmed geometry in 1,043 groups. It flagged 47 before the Bacta override and 28
after it. The remaining 27 item-form flags are separate windows, lenses or
bubbles, not missed close merges; the container pass added one flag, the Display
Shelf's three frosted panes. The report is
`reports/silhouette-audit-2026-11-01.md`, with the per-item data in the TSV and
the 19-vial before/after in `bacta-pane-merge-before-after-160.png`.

`bacta_vial_sheet.py` is the focused regression view: it renders the 19
Bacta-family records at 160 into `reports/bacta-vials-160.png`, labels each
tile with its closed-contour count, and exits non-zero if any record is missing,
blank or still split. Run it after changing `merge_panes`, `pane_merge_scale` or
`outline_mask`:

    python tools/renderer_recon/bacta_vial_sheet.py
    python tools/renderer_recon/bacta_vial_sheet.py --size 320 --out reports/bacta-vials-320.png

`silhouette_index.py` is the whole-audit view: the 19 cleared Bacta vials plus
the 28 remaining flags, grouped by the triage families in
`reports/silhouette-audit-2026-11-01.md`, written to
`reports/silhouette-index-160.png`. It also writes a self-contained
`reports/silhouette-index-160.html` with combinable filters: one button per
family, a flagged/cleared toggle, and a text box matching name, FormID, type,
plugin or family. Matches are highlighted in the captions and family headings;
`/` focuses the box and Escape clears it. A Clear filters button resets the
family, flagged/cleared and search controls together, and is disabled when the
view is already unfiltered. The active filters are mirrored in the URL as
`family` (the section index), `status` (`all`, `flagged` or `cleared`) and `q`
(the search text) using `replaceState`, so reloading or sharing the address
restores the view without adding back-button entries. Clear filters removes
these parameters; Copy link copies the same canonical URL and shows Copied or
Copy failed feedback. It uses the Clipboard API and falls back to a temporary
textarea when that API is unavailable. Each tile also has a copy link action
for its direct `?form=<FormID>` workbench URL. Copy visible links copies the
workbench URLs for the current filter result, one per line, and is disabled
when no records match. Every tile links to its workbench focus view
(`--workbench-url`, default `http://127.0.0.1:8791/`). It checks its 28 flag records against the audit TSV
(`--audit`, default `reports/silhouette-audit-2026-11-01.tsv`), so it cannot
silently omit a new flag. It exits non-zero if the TSV and the family table
disagree, if a listed record is missing or blank, or if a Bacta vial splits; a
flag that becomes one contour is shown green but does not fail, since that is a
fix rather than a regression:

    python tools/renderer_recon/silhouette_index.py
    python tools/renderer_recon/silhouette_index.py --size 320 --out reports/silhouette-index-320.png \
        --html reports/silhouette-index-320.html

## Outfit assembly audit

`outfit_assembly_audit.py` checks the C++ outfit fallback against the installed
assets. It finds every complete `upperbody` / `lowerbody` / `sleeves` group in
loose files and BA2 name tables, applies `LiveModel::AssembleSiblings` to every
ARMO record's declared part paths, and parses each resulting NIF's LOD0 mesh
with the same offline path as the contact sheet. It writes
`reports/outfit-render-all-audit-2026-11-01.md` and exits `1` if a referenced
record is missing a part type or any assembled part NIF cannot build; a missing
catalog, Data directory or set of complete groups exits `2`:

    python tools/renderer_recon/outfit_assembly_audit.py
    python tools/renderer_recon/outfit_assembly_audit.py --out reports/outfit-audit.md
    python tools/renderer_recon/outfit_assembly_audit.py --junitxml reports/outfit-audit.xml

`--junitxml` writes one case per failed record, combining that record's missing
and non-buildable findings; a clean audit writes one passing case. That keeps
the CI report record-level instead of only saying the audit failed.

The record-level check matters: a record may take its upper body from one
complete group and its lower body from another. The declared-part guard leaves
the in-group part missing rather than adding a duplicate, so the report counts
in-group missing parts, fallback additions, and parts already covered by another
group separately. A failure names the record and the missing type or the NIF
that did not produce a LOD0 mesh. This is an asset/source audit, not a live
Starfield capture, so vertex alignment and material appearance remain visually
unverified. The exit-code and guard tests use a synthetic asset set and do not
need the game install:

    python -X utf8 -m unittest tools.renderer_recon.test_outfit_assembly_audit

For the whole Python unit suite plus this audit in one command, run
`python -X utf8 tools/check.py` from the Explorer directory.

## The live workbench (and why contact_sheet is still here)

`contact_sheet.py` is the renderer; `tools/render_workbench.py` is the loop
around it. It keeps the asset store, plugin index and material database alive in
one process, so the ~25s archive index is paid once instead of per run, then
redraws a selection on demand at a few hundred milliseconds per item. Watch it
at `http://127.0.0.1:8791/`:

    python tools/render_workbench.py --item Razorback --item "Coffee Mug"
    python tools/render_workbench.py --type AMMO --limit 40 --size 240

The page's find box is the preview loop: type a name, FormID or plugin, click a
result, and that one record redraws at `--detail-size` (512 by default) with its
model paths, geometry counts and the material decision table behind the tile.
`http://127.0.0.1:8791/?form=0003EDC8` opens a record directly and `/` focuses
the box, so a rendering question can go from "which record" to "which material"
without a game launch. The focus panel also links the record to Fandom and
StarfieldDB searches and lists any assembly part that had no connect host. This
view is the live server only; `--once` keeps the batch shape above.

Editing `contact_sheet.py`, `nif_uv.py` or `aid_colour_audit.py` on disk reloads
that module, redraws the same selection and refreshes the page. Each pass keeps
the previous tile and reports the mean pixel difference, so the tiles that moved
are the items a rule change touched - which is exactly the question
`test_contact_sheet` cannot answer. A change to `aid_colour_audit.py` rebuilds
the runtime (its classes are the runtime's), so that one re-pays the index.

Batch mode remains the regression shape:

    python tools/render_workbench.py --type MISC --limit 300 --once
    # -> reports/workbench/{workbench.html, workbench.tsv, tiles/}

Limits are contact_sheet's, listed above. Dummy-base weapons are the exception
that used to draw nothing: Razorback's record points at the empty
`BullpupDummyReceiver.nif`, so `omod_assembly.py` assembles the gun the way the
plugin does in game - the OMODs whose MNAM keyword is on the weapon's KWDA list,
standard tier first (receiver + grip + barrel), one part per slot when no tier is
marked. `python tools/render_workbench.py --item Razorback` now draws 3 parts /
7 shapes as one bullpup: `connect_points.py` resolves the NIFs' `C-<slot>`
children and `P-<slot>` parents into
`parentPlacement * hostNodeWorld * entryTransform`, with the `C-Receiver`
holder as the identity root - the same chain as
`NativeModel::ResolveAssemblyPlaces`. Before that every part drew at its own
origin, which is what made the gun an exploded pile. A part with no matching
host is drawn at identity (some OMOD meshes are authored in place) and is
listed as **unplaced** in the focus panel; the picker also adds the receiver
when the standard set omitted it, because the receiver is the placement root.
A mod NIF that carries its LOD0 geometry inside itself (no
`geometries/<guid>.mesh` entry) is drawn from those bytes too; without that, 45
of the first 400 Misc items were blank tiles.

`lab_shelf_diff.py` compares two tile folders from that tool and ranks the tiles
that moved, so a rule change can be checked item by item instead of by eye:

    python tools/renderer_recon/lab_shelf_diff.py \
        --before <old tiles> --after <new tiles> \
        --tsv reports/lab-diff.tsv --sheet reports/lab-diff.png --top 12

It reports tiles missing on one side separately rather than counting them as a
change (a half-finished render should not read as a rendering difference).

## Weapon mip-chain preview correction

The direct renderer now retains and uploads contiguous BA2 DDS mip chains
instead of decoding only the primary mip. That lets the existing anisotropic
sampler select lower-resolution levels when a weapon is minified, reducing the
primary-mip shimmer/blocking visible in the recent shop captures. The reader
accepts only contiguous chains beginning at mip 0; it uploads the valid prefix
if lower BA2 mip chunks are absent, and rejects malformed primary chunks.
Loose DDS inputs keep a valid mip chain when available and fall back to mip 0
if a mod declares missing lower levels.

## DXGI UNORM versus sRGB visual probe

Run `python tools/renderer_recon/dxgi_color_space_comparison.py` from the
Explorer project directory to generate
`tools/renderer_recon/extracted/unorm_srgb_sample_comparison.png`. It defaults
to the shipped Breach Tactical Stock BC1 albedo, finds the owning texture BA2,
and decodes the same primary mip for both sides of the comparison. `--texture`
can select another game-relative key; `--ba2` pins a particular archive and
`--data` changes the texture-archive search directory.

The left panel models the preview's current BC1 UNORM SRV sampling; the right
panel applies the standard sRGB-to-linear decode before the same fixed shader
light and UNORM output store. The generated report measures the change in
stored-output luma for that exact texture. This isolates the texture-view
format effect. It is an analytical primary-mip probe—not a Starfield capture
and not full GPU color management: it omits filtering/mips, geometry, material
masks, the preview's varying light, and the display/tone-map chain. In
particular, it cannot by itself establish that sRGB sampling is the desired
asset interpretation or explain other appearance differences. Comparative
runs for the Breach tactical stock/decal atlas and Razorback receiver/standard
decal/Fancy decal are available under `extracted/*unorm_srgb_comparison.png`;
they demonstrate only the format transfer effect and do not justify switching
the preview away from its established UNORM path.

Regression test: `python -m unittest tools.renderer_recon.test_dxgi_color_space_comparison`.

## Form model-path resolution

The live NIF builder canonicalizes form paths to the slash-separated, lowercase
`meshes/.../*.nif` key used by BA2 name tables. It handles Windows separators,
leading separators and repeated `Data/` roots, collapses repeated `meshes/`
roots before adding a missing mesh root, and rejects `..` path components,
colon/drive-qualified segments and non-NIF extensions before lookup. It also
rejects already-rooted asset strings such as `geometries/...`; those are not
model NIF paths. Loose-file
search still follows the configured extra directories, enabled MO2 mods, then
the game's Data directory; BA2 lookup remains mod-first. This is path-key
normalization only: it does not infer another record's model, change the chosen
form path, or prove that every item has a valid geometry stream.

A source-level regression lives in `tools/test_weapon_mod_selection.py` and
runs with `python -m unittest tools.test_weapon_mod_selection`. Runtime
confirmation still requires selecting representative modded and base-game
forms in Starfield and checking their logged resolved model/geometry paths.

## Evidence: TankGirl Xenology sprout assets

The installed `tankgirlsxenologyexpanded - main.ba2` name table contains
`meshes/tankgirlck/xenologymesh/itm_sproutjar.nif` and two separate desktop
plant-pod assets under `meshes/tankgirlck/xenologymesh/xenoplantpod/`:
`sproutbox.nif` and `sproutbox_bowl.nif`. The extracted samples confirm these
are distinct NIFs with separate material/geometry ranges: the jar includes
Isotopic Coolant container/lid, water, dirt and plant materials, while the pod
uses its own desktop box/bowl materials. The archive evidence proves the files
exist; it does not identify which one the user's selected catalog record
references. Do not alias all sprout-named records to a single NIF without the
runtime form-to-model path. The game's Steam Data directory was not present at
its default path during this audit, so base-game archive coverage was not
retested here.

