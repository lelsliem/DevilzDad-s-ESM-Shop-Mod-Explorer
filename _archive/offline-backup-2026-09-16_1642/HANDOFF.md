# SF-Shop Live Render — HANDOFF
Updated: 2026-09-16 15:30 — consolidated to current truth

## What this is
`SFShopExplorer.dll` — native ImGui/DX12 shop (no OSF/WebView) in `B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\`. Two threads: task (`ServicePending` resolve + `PrefetchTextures`) and Present (`UploadGPU → RenderGpuModel → UploadModelTextures` exact `matPath+nodeName`).

## Where rendering stands
Offline is the shop preview. Live is the experiment:

- **Offline `.sfmodel` cache** `B:\MO2\...\SFShopExplorerAssets\models\` (732 files) built by `tools/renderer_recon/build_model_cache.py:133` via `MeshConverter.dll`. Fixed: per-`(pos,uv)` expand + `V=1-t` flip (`OfflineModel.cpp:167-240`), else sampler hit wrong texels (brown slides). All 617 weapons batch-flipped and redeployed. Tag now `offline 3D — <key> — N meshes, T tris (P / Q textured)`.

- **Live build** `LiveModel.cpp:211` owns NIF 20.2.0.7 + `.mesh` LOD0 + BA2 (`BTDX` v2/v3 `LiveModel.cpp:237` `packed/size` tail-shift). v2 bulk `base=ntOff-totalBulk` verified on `SFBGS003/004/ShatteredSpace` with real zlib decodes. `usecache=0` in `[offlinepreview]` skips disk reads except just-written reload and forces NIF discovery (`LiveModel.cpp:2531` EDID-token scan, `asm_<edid>` namespaced so curated caches are never clobbered). Default `1` (curated).

- **Textures:** mod `.mat` JSON exact `Albedo.File` via `FindMatAlbedo:1932` first (`score albedo`), else fuzzy `FindDiffuse:2064` over `textures/**/*_color.dds` indexed from BA2s (now 38042, should be ~45K when MO2 discovery is fixed — see below). Family singleton for `FragMine` → `default_color`.

## Why this got complicated — and the simpler path
You're right: vendors and the inventory *already* display every item. The engine has a working preview scene (InventoryMenu / BarterMenu `LoadModel` + `Item3DSceneRect`). We elected to *reimplement* it offline (NIF+.mesh→`.sfmodel`→our PSO/VB/RT) because no stable public hook exists to embed that scene into our ImGui viewport:

- `CommonLibSF_RENDERER_AUDIT.md:44` — `BarterMenu_LoadModel` is `91377` but `InventoryMenu_LoadModel` is `0`; the `TUIEventDispatcher` vtable entries (`442885`) are registration metadata, not callable; calling with guessed addresses crashes. So we stayed read-only.

The simpler alternative is *not* to rebuild meshes — reuse the engine's scene:

1. **Inventory RTV capture (already in-tree):** `ItemCapture.cpp:10` hooks `ID3D12Device::CreateRenderTargetView` slot 20, observes the inventory HDR chain while `InventoryMenu` is open, and shows its `sub-rect` in `PREVIEW` (`mode 2`). No geometry, no materials — just the pixels Bethesda already rendered. Requires `InventoryMenu` open once (or `OpenInventory` with a handle) and has no mod-gun assembly cost. This is what vendors do.

2. **Container/Inventory dispatch (untried safely):** `ShopTestRender.cpp:404` already proves the passive sink is safe; send a real `uItemHandle` to `BarterMenu_LoadModel` once the ABI is captured via debugger instead of guessed. One call paints the vendor's own preview — no offline cache at all.

If the goal is correctness per FormID (base vs instance naming `TESFullName:302` vs `BGSInstanceNamingRulesForm:690` — `MODIFIED ADVANCED` prefixes are instance data, not base rows), path 1 is the least code.

## Open gap that explains "few still broken"
`LiveModel.cpp:DiscoverMoMods:1215` reported `momods=0` this session (`SFShopExplorer.log:16`). That means `241 BA2 (0 mod override)` — mod texture BA2s never entered `g_moBa2`, hence `crimx_visor_f 0/2`, `breach` `AWP_Master_Shareable` misses, and the 38K vs 45K shortfall. Today's `M1919:15:38:17` `6 [receiver+barrel+muzzle+mag+grip+scope]` and `Kraken` detached mag were caught by the same session; `Novalight` 9/9 bound but white ghost remains (material-less base NIF, falls to receiver). Fix for discovery is deployed `15:04` (10-deep walk, `weakly_canonical`, hard `B:\MO2\Starfield\mods` fallback, 3-way `ModOrganizer.ini` probe) — needs a relaunch to prove.

## How to test what matters
- Default (`usecache=1`): sweep catalog, compare by `FormID` (`00040826:Rattler`, `0024561C:XM-2311`, `000476C4:Eon`) — not `MODIFIED…` display names.
- Live-only (`[offlinepreview] usecache=0`): slow first touch (archive open + NIF/.mesh parse on task thread) then cached; path-less dummies resolve via `DiscoverNif:2531` → `asm_<edid>`.

## Deployed builds (each verified `build ok` + MO2 dll + orphan sweep)
- 2026-09-16 15:04 sweep — MO2 discovery + truncation + mag/assembly mutexes
- 2026-09-15 22:09 family-gated assembly — anchors to receiver dir, honest empty on miss
- 2026-09-15 19:22 usecache + NIF discovery
- plus 22:21 `asm_*` namespacing, 22:05 neutral placeholders, 21:37 EDID-first, 19:48 OMOD receiver, etc. — full history compressed here.

## Layout gotchas
- MO2 mount: `mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll` only copy (checked `B:\MO2\Starfield\mods\` depth 4). `overwrite\SFSE\Plugins` holds only newly live-built models, not curated.
- `xmake` orphans `cl/xmake` after kills — sweep before rebuild or `touch src\ShopExplorer.cpp` to force detection.
