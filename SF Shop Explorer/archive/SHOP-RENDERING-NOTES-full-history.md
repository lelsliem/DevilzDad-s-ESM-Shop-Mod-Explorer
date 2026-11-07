# Shop Explorer — Shop Rendering Notes

**Session note:** started 2026-09-07; model-path resolution addendum 2026-10-16  
**Purpose:** authoritative handoff for Shop Explorer rendering and asset resolution

> This file is a chronological investigation log, not a single-date snapshot.
> Later corrections and current-state addenda supersede earlier statements; read
> the latest dated entries and source before relying on older section summaries.

## Current scope addendum — NIF model-key normalization (2026-10-16)

A focused source fix now canonicalizes the selected form's NIF path before the
BA2 lookup: lowercase, Windows `\\` to `/`, remove leading separators, strip
repeated leading `data/` virtual roots, collapse repeated `meshes/` roots,
then add `meshes/` only when no mesh root exists. It rejects rooted
`geometries/...` keys, `..` path components, colon/drive-qualified segments,
and non-NIF extensions. Validation is component-based, so harmless names that
merely contain two dots are not rejected. The source regression is in
`tools/test_weapon_mod_selection.py`
(`test_live_model_path_normalization_handles_form_separators_and_data_root`).

Resolution detail: the normalized key is used for loose-file and BA2 lookup;
loose-file precedence remains configured extra directories, enabled MO2 mod
directories, then base `Data`, followed by mod-first BA2 archives. The `Data/`
virtual root is removed before applying the `meshes/` default, so
`Data/meshes/meshes/foo.nif` normalizes to `meshes/foo.nif`; already-rooted
`geometries/...` strings are not treated as NIF paths.
This addresses malformed-key lookup misses only. It does not guess a replacement
model or prove asset presence, successful NIF/mesh parsing, or visual correctness.

Evidence check for the user's sprout example: the installed
`tankgirlsxenologyexpanded - main.ba2` contains distinct
`meshes/tankgirlck/xenologymesh/itm_sproutjar.nif`,
`.../xenoplantpod/sproutbox.nif`, and
`.../xenoplantpod/sproutbox_bowl.nif`. Extracted NIF strings show they use
different authored materials and geometry names, so they are not interchangeable
aliases. No current runtime log line was found tying the selected `Sprout in a
Jar` MISC record (FormID `040001C6`) or other sprout-named forms to one of these
paths. The safe next verification is to select the exact record in-game and
capture its `NativeModel`/model-path log, then confirm loaded meshes and compare
the visible preview. The default Steam Data path was unavailable to the offline
archive search in this workspace; the evidence cited here is from the installed
MO2 TankGirl Xenology archive and extracted NIF samples only.

`tools/renderer_recon/README.md`, `README.md`, and `SHOP-CATALOG-REFERENCE.md`
were updated alongside this note. Do not change the older investigation entries
below into claims of current runtime verification.

---


This file is the first document to read before changing the shop renderer. It records the current truth after the earlier renderer experiments and prevents the failed paths from being repeated.

## 1. The target

The shop must eventually show a **live Starfield item scene** in the custom preview area, matching the behavior seen in the supplied Inventory screenshots:

- selecting a weapon displays the actual weapon model;
- selecting armor/clothing displays the paper-doll character with the selected equipment;
- selecting a misc/resource item displays its actual 3D item when Starfield supports one;
- changing the selected row changes the model without opening another menu;
- the model is lit and rendered by Starfield, not replaced with a PNG, icon, model-path label, or screenshot.

The rectangle in the Shop Explorer is **custom UI owned by Shop Explorer**. It is not a native Starfield menu panel and must not be mistaken for `PreviewSceneRect_mc` merely because it has a similar visual purpose. The supplied screenshots are references for the live scene behavior, not proof that the custom ImGui child already has an engine render surface.

## 2. Current production shop

`SF Shop Explorer` is an independent native Dear ImGui/DX12 overlay:

- source: `src/ShopExplorer.cpp`, `src/ImguiOverlay.cpp`, `src/main.cpp`;
- catalog: plugins, record types, forms, search, values, weights, keywords, model metadata;
- transfer path: queued SFSE task action, not direct mutation from the Present hook;
- preview: currently metadata-only;
- preview code: `ShopBrowser::DrawImGui()` → child named `item_render_viewport`;
- current placeholder text explicitly says that live 3D preview is unavailable;
- the active production source does not open `ContainerMenu`, dispatch unresolved InventoryMenu events, capture a BMP, or inject a texture.

Do not replace the stable browser while renderer research is incomplete. A renderer experiment must be separately guarded and fail closed.

## 3. Facts verified from Starfield's normal InventoryMenu

The native inventory menu already proves the desired behavior. Its decompiled Scaleform flow is:

```text
PlayerInventoryData.aItems
  -> ItemList_mc.selectedEntry.uHandleID
  -> InventoryMenu_SelectItem { uItemHandle }
  -> InventoryMenu_LoadModel { uItemHandle }
  -> BS3DSceneRectManager / PreviewSceneRect_mc
```

For paper-doll-compatible armor/clothing it can send:

```text
InventoryMenu_PaperDollTryOn { uItemHandle }
```

The important input is the live inventory-instance handle (`uHandleID`), not a base FormID. The handle carries the current stack/extra-data context. It must be resolved from the current inventory/session state and must never be manufactured by arithmetic from a FormID.

The native scene is owned by the menu/engine. The available CommonLibSF declarations expose evidence of that ownership:

- `IMenu` exposes the native Scaleform movie/root object;
- `UICellRenderer` has an undocumented `BSMenu3D::Scene`-related field;
- `MenuPaperDoll` has the native actor/scene-related fields;
- `BS3DSceneRectManager` registers a screen-space scene rectangle from the Scaleform side.

Those declarations do **not** expose a verified public API that attaches the menu-owned scene to an arbitrary ImGui child, returns a D3D12 texture for that scene, or constructs a second InventoryMenu renderer.

## 4. Rejected approaches — do not repeat

### Temporary object + ContainerMenu

The temporary-box → `ContainerMenu` route was attempted twice. It opened the native container lifecycle, but it did not provide an item image/texture to the independent Shop Explorer preview rectangle. Container opening is not renderer integration and is not evidence that the custom ImGui pane can display the model.

This route is retired. Do not:

- spawn or use a temporary storage box for preview;
- copy a shop item into player storage for preview;
- open `ContainerMenu` from the production shop;
- describe a ContainerMenu lifecycle log as an image-rendering success.

### BMP/PostFX capture

The SFPostFX Present-time marker/capture path can produce a valid composited frame or cropped BMP. That is useful for diagnostics and comparison only. It is a snapshot after rendering, not a live scene inside the Shop Explorer rectangle. It is excluded from the production preview target.

### OSF/WebView/SWF replacement and static art

The old OSF/WebView path, browser bridge, SWF experiments, model-path labels, item icons, and supplied artwork are not substitutes for the target live Starfield scene. They remain historical/reference material only.

### Guessed native calls or pointers

Do not call unresolved `InventoryMenu_*` event IDs, read undocumented scene fields as if they were a stable API, substitute generated registration/vtable IDs for callable functions, or inject a guessed D3D12 resource. A crash-free call is not proof that the ABI is correct.

## 5. Current unresolved boundary

The missing bridge is not the ImGui rectangle itself. The missing bridge is how Starfield's engine receives the native inventory selection and how its engine-owned scene rectangle is made visible alongside a separately rendered ImGui overlay.

The research found these leads:

- `BS3DSceneRectManager` and `PreviewSceneRect_mc` describe screen-space scene registration;
- `InventoryMenu` sends typed selection/model events using a live `uHandleID`;
- generated `TUIEventDispatcher` registration/factory IDs exist in `IDs_VTABLE.h`;
- direct `InventoryMenu_*::GetEventSource()` IDs remain unresolved/zero in the active CommonLibSF snapshot;
- generated vtable/RTTI/auto-registration IDs are metadata, not automatically callable event-source accessors;
- the external generic GFx object lookup sees public menu state but does not expose the private selected `InvItem` state during the model view.

Therefore the next implementation must first identify a version-correct, passive event/scene-host boundary for the installed Starfield build. Until then, production remains metadata-only.

## 6. Safe next renderer pass

Work in this order:

1. Keep `SF Shop Explorer` stable and do not change its transfer/catalog behavior.
2. Use a separately named, kill-switched research probe rather than putting guessed renderer code in the shop.
3. Observe a normal InventoryMenu selection and record the real item handle, form ID, event timing, and scene-rectangle state without changing inventory or opening another menu.
4. Verify the installed runtime ABI for the UI event dispatcher or a native Scaleform/scene host.
5. Determine whether the engine can render its scene rectangle as an independent screen-space overlay. If it can, map its bounds to the Shop Explorer rectangle while leaving the model scene engine-owned. If it cannot, a native Scaleform host may be required; do not pretend an ImGui child is that host.
6. Only after the event payload and host lifetime are verified should a minimal one-item integration be attempted.
7. Keep a runtime kill switch and restore the metadata-only view automatically on menu close, invalid handles, missing scenes, or any failed initialization.

A valid success must show a model changing with the selected item in the Shop Explorer rectangle during normal browsing. It must not require ContainerMenu, a temporary item copy, a screenshot file, or a guessed address.

## 8. Passive scene-boundary probe created

A separate research target now exists in `SF Starfield MCM`:

- target: `InventorySceneBoundaryProbe`;
- source: `src/InventorySceneBoundaryProbe.cpp`;
- configuration: `SFSE/Plugins/InventorySceneBoundaryProbe.ini`;
- default: `enabled=false`;
- build result: `xmake build InventorySceneBoundaryProbe` passed;
- output: `build/windows/x64/releasedbg/InventorySceneBoundaryProbe.dll`.

The probe uses the version-correct passive boundary that is actually exposed by this CommonLibSF/runtime combination:

1. `RE::UI::GetSingleton()` and the inherited `UI::MenuOpenCloseEvent` source observe `InventoryMenu` opening and closing.
2. SFSE's supported menu callback observes the live `InventoryMenu` object.
3. `IMenu::menuObj` is sampled through the existing Scaleform `Value` API.
4. `PreviewSceneRect_mc` is read, and its public `QData()` method can be invoked only when the INI allows it; the returned fields are logged with a bounded member visitor.
5. `ItemList_mc.selectedEntry` is sampled read-only so a normal inventory run can show whether `uHandleID` and related fields are visible at the callback/sample timing.

The probe does **not**:

- call `UpdateSceneRectEvent::GetEventSource()`;
- call `InventoryMenu_LoadModel`, `InventoryMenu_SelectItem`, or `InventoryMenu_PaperDollTryOn`;
- use generated `TUIEventDispatcher` registration/factory/vtable IDs as functions;
- invoke a renderer, open a menu, mutate inventory, capture pixels, or inject a D3D resource.

The decisive result from the static boundary audit is that `UpdateSceneRectEvent::GetEventSource()` remains ID `0`, while `UI::GetSingleton()` and `UI`'s menu-open/close source are structurally available. The probe can therefore establish the native rectangle's live screen-space data and lifetime safely, but it cannot yet attach that engine-owned scene to the Shop Explorer ImGui child. The DLL has been built but not deployed or enabled; the next runtime action is a controlled normal-inventory observation with the probe INI explicitly set to `enabled=true`, followed by restoring `enabled=false`.

## 9. Phase 2 result and hardened probe deployment

The first `InventorySceneBoundaryProbe v0.1.0` run produced useful Phase 2 evidence but later caused a CTD during extended Scaleform polling. The usable evidence was recorded before the crash:

- `InventoryMenu` opened and closed and the supported SFSE menu callback fired.
- `PreviewSceneRect_mc` was present and enabled; the logged bounds were `0,0 -> 1920,1080`.
- `QData()` returned `sSceneRectName=PreviewSceneRect_mc`, `bVisible=true`, `bHasMask=false`, and the full-screen rectangle.
- `ItemList_mc.selectedEntry` exposed a live item instance: `sName=SysDef Crew Uniform`, `uFormID=3353019` (`0x0033301B`), `uHandleID=4207675575` (`0xFAE1C1B7`), weight `1.3`, value `225`, and `ArmorInfo=object`.
- The selected entry was observed in both equipped and unequipped states.

The crash-prone v0.1 probe was replaced with `InventorySceneBoundaryProbe v0.2.0`, built with `xmake build InventorySceneBoundaryProbe` and deployed to the active `Shop Test Render` profile. Deployed DLL SHA-256: `d0377e19ba553bbda9fa96d3991bc21a68433de376ab8068b43f4891fd830570` (source and deployed hashes match).

The post-deployment test completed cleanly with **no CTD and no new crash dump**, which is the expected result for a stable run. The available file-backed probe log still ends with the old v0.1 `SAMPLE`/Scaleform output and does not contain a v0.2 startup or `INVENTORY_MENU observed` marker, so the runtime callback evidence for v0.2 is not independently visible in that log. This is a logging/activation-verification gap, not a crash result. The older `ShopTestRender` Scaleform sampler was disabled afterward to keep the next run isolated. The v0.2 DLL remains deployed with matching SHA-256 `d0377e19ba553bbda9fa96d3991bc21a68433de376ab8068b43f4891fd830570`, and its boundary INI remains `enabled=true`.

## 10. Resume rule

Treat the Phase 2 log as evidence about the native inventory scene boundary only. The item handle/FormID pair and `PreviewSceneRect_mc` facts are verified observations, not a scene-host API. The next renderer investigation must use the v0.2 lifecycle-only probe or a separately reviewed live D3D12 diagnostic; it must not return to repeated Scaleform traversal or screenshot capture.

## 11. Resume checklist

Before the next renderer change, read:

- this file;
- `README.md` in this folder;
- `SF Starfield MCM/COMMONLIBSF_RENDERER_AUDIT.md`;
- `SF Starfield MCM/RENDERER_AND_SHOP_PLAN.md`;
- `SF Starfield MCM/INVESTIGATION_NOTES.md`;
- `SF Starfield MCM/tools/ba2_extract/inventory_renderer_notes.md`.

Then inspect the current source before editing. The authoritative production state is: **native ImGui catalog working; preview metadata-only; no ContainerMenu handoff; no screenshot bridge; no unresolved InventoryMenu dispatch active; v0.2 lifecycle-only probe deployed separately and kill-switchable.**

## 12. Renderer-document and dump-log audit — 2026-09-07

The renderer-relevant documentation, decompiled UI sources, dump outputs, CSV references, and local D3D12 implementation were cross-checked before planning another probe. This section records what is evidence and what is not.

### Evidence that explains how the native item is rendered

The decompiled `InventoryMenu` gives the complete high-level handoff:

```text
ItemList_mc.selectedEntry.uHandleID
  -> InventoryMenu_SelectItem { uItemHandle }
  -> InventoryMenu_Change3DView { iFilterFlag }
  -> InventoryMenu_LoadModel { uItemHandle }
     or InventoryMenu_PaperDollTryOn { uItemHandle }
  -> BS3DSceneRectManager.Register3DSceneRect(PreviewSceneRect_mc)
```

`BS3DSceneRectManager` immediately forwards `PreviewSceneRect_mc.QData()` as an `UpdateSceneRectEvent` through `BSUIDataManager`. `BSAnimating3DSceneRect` computes the global screen rectangle, visibility, background color, and optional mask each frame. This explains the native scene's **where/when** and confirms that the model is an engine-owned scene associated with the menu's live handle.

`BSUIEventDispatcherBackend` is the actual Scaleform-to-game boundary: `BSUIDataManager.dispatchCustomEvent()` eventually calls its `DispatchEventToGame(Event)` function. The public AS3 sources expose the function shape and event names, but not a safe native plugin-callable object, singleton address, or payload ABI for the unresolved InventoryMenu events. The existing CommonLibSF audit correctly leaves those direct event-source IDs at zero.

### Evidence from the D3D12 renderer code

`Starfield PostFX` is a valid local reference for the engine render boundary:

- it obtains the engine's direct `ID3D12CommandQueue` and device;
- it captures the real swap chain and Present timing;
- it identifies the back buffers and the main scene depth resource;
- it can observe `ExecuteCommandLists`, but the current hook only identifies the submitting queue and does not decode command-list contents;
- its proven Present pass operates on the finished composited frame, not on a menu-private item scene or an exposed scene texture.

The local D3D12 evidence therefore supports a **read-only render telemetry probe** at the queue/swap-chain/Present boundary, and it supports a later depth/resource diagnostic. It does not provide a verified way to select or isolate the inventory model's mesh, material, render target, camera, or draw calls. `PreviewSceneRect_mc` is a screen-space registration contract, not a D3D12 resource handle.

### What the dump files and offline tools add

`SFDevilzDump.log` and `SFDevilzDump-all-forms.csv` are useful catalog/form evidence: they enumerate loaded records, record types, plugin ownership, and model-related metadata/classifications. They do not contain GPU allocations, render-target identities, command-list packets, mesh submissions, or an inventory scene binding. The CSV's `renderCandidate`/model fields must remain classifications rather than claims that a record can be rendered by the native inventory menu.

`inventory_renderer_notes.md`, `inventory_dumpas3.txt`, `render_item.py`, and the preview HTML files corroborate the Scaleform contract and provide an offline Blender/reference-art path. They do not capture Starfield's live renderer. The sun/sky and PostFX notes describe the separate world-frame post-process path; they are not evidence of the inventory model's private scene pipeline.

### Decision for the next probe

This audit strengthens, but does not change, the existing plan:

1. Keep `SF Shop Explorer` metadata-only and unchanged.
2. Do not re-enable Scaleform member traversal, call `QData()` from native code, dispatch an unresolved InventoryMenu event, inject a texture, or use screenshots as the product path.
3. If a new diagnostic is built, make it a separately named, kill-switched, read-only D3D12 telemetry probe. It may record Present/queue/device/resource timing and bounded command-list metadata, but it must not mutate command lists or claim that a generic draw-call trace identifies the inventory object.
4. Treat the native AS3 event/backend boundary and a possible scene-host/viewport boundary as separate unresolved work. The documents do not justify deploying a live render-pass hook yet.

**Audit result:** the existing Phase 2 facts are sufficient for the scene lifecycle and screen-space boundary. The documentation does not reveal a hidden public render API or a reusable item texture. A Phase 3 D3D12 probe may still be useful for learning frame/resource timing, but it is an exploratory telemetry step—not an implementation of the Shop Explorer 3D preview.

## 13. Clean test interpretation and Phase 3 telemetry build — 2026-09-07

The controlled hardened-boundary test was clean: **Starfield did not crash, so no crash dump was expected or produced**. The absence of a dump is a successful no-CTD result, not a failed test. As already recorded above, the old file-backed v0.1 sampler log cannot independently prove that the v0.2 callback loaded, but it also contains no evidence of a new crash.

A separate `InventoryRenderTelemetryProbe v0.1.0` was then added and built, without changing the production Shop Explorer or re-enabling the old Scaleform sampler:

- source: `SF Starfield MCM/src/InventoryRenderTelemetryProbe.cpp`;
- target: `InventoryRenderTelemetryProbe`;
- configuration: `SF Starfield MCM/SFSE/Plugins/InventoryRenderTelemetryProbe.ini`;
- build command: `xmake build InventoryRenderTelemetryProbe` — passed;
- artifact: `SF Starfield MCM/build/windows/x64/releasedbg/InventoryRenderTelemetryProbe.dll`;
- SHA-256: `1388c982d40306be915cf0b45443944c342c863abb4be02eee4d2807987d5b1d`.

The new probe is disabled by default. When explicitly enabled for a controlled run, it only:

- tracks `InventoryMenu` open/close through the supported SFSE menu callback and `UI::IsMenuOpen`;
- locates the existing engine `ID3D12Device`/DIRECT `ID3D12CommandQueue` through the verified CommonLibSF renderer declarations;
- observes `ID3D12CommandQueue::ExecuteCommandLists` through the queue vtable slot already used by the local SFPostFX diagnostics;
- logs a bounded number of command-list submissions, list types, object addresses, queue identity, session number, and a short post-close grace window;
- leaves every command list, resource, descriptor, render target, and GPU operation untouched.

It does **not** capture screenshots, inspect Scaleform, call `QData()`, dispatch InventoryMenu events, inject resources, hook individual draw methods, decode draw calls, or claim that a generic submission identifies the inventory mesh. The individual draw-hook extension was deliberately removed: the local CommonLibSF snapshot does not expose a verified graphics-command-list ABI, so adding guessed slots would trade useful telemetry for CTD risk. The final build stops at the verified queue `ExecuteCommandLists` boundary and read-only `GetType()` metadata. The INI remains `enabled=false`. The DLL and INI were subsequently deployed, while Starfield was stopped, to `B:/MO2/Starfield/mods/Shop Test Render/SFSE/Plugins/`. The deployed DLL SHA-256 matches the build: `1388c982d40306be915cf0b45443944c342c863abb4be02eee4d2807987d5b1d`. The deployed configuration remains disabled. A later runtime run should enable only this probe, keep `ShopTestRender` disabled, and restore `enabled=false` after the observation.

## 14. Run with ShopTestRender enabled — 2026-09-08

The later Shop Test Render run was **not isolated**: the active `ShopTestRender.ini` had `enabled=true`, `sampleEveryTicks=20`, and `maxMembers=48`. That enabled the older v1.5 Scaleform sampler. Its log grew to approximately 110 MB and passed sample `#15400`, confirming that it was actively traversing the InventoryMenu Scaleform object graph.

Useful observations from that run include:

- selected item name: `Swimsuit`;
- selected `uHandleID`: `4207674510`;
- selected `uFormID`: `12196`;
- `PreviewSceneRect_mc.visible=true`;
- `PreviewSceneRect_mc.enabled=true`;
- the public Scaleform object exposed the selected entry and its 27 fields.

The new `InventoryRenderTelemetryProbe` was present but its deployed INI remained `enabled=false`. Its overwrite log contains only startup, callback registration, and task registration lines—no `EXECUTE` telemetry—so this run did **not** test the D3D12 probe. No screenshot or inventory mutation was performed. The next D3D12 test must first set `ShopTestRender.ini` to `enabled=false`, enable only `InventoryRenderTelemetryProbe.ini`, run the normal InventoryMenu observation, then restore the telemetry INI to `enabled=false`.

## 15. Isolated D3D12 telemetry test armed — 2026-09-08

The active `Shop Test Render` profile is now prepared for the controlled D3D12 observation:

- `InventoryRenderTelemetryProbe.ini`: `enabled=true`;
- `ShopTestRender.ini`: `enabled=false` (the older Scaleform sampler is isolated);
- `InventorySceneBoundaryProbe.ini`: `enabled=false`;
- `InventorySceneCaptureProbe.ini`: not deployed/enabled;
- telemetry DLL: deployed at `B:/MO2/Starfield/mods/Shop Test Render/SFSE/Plugins/InventoryRenderTelemetryProbe.dll`;
- deployed DLL SHA-256 matches the verified build: `1388c982d40306be915cf0b45443944c342c863abb4be02eee4d2807987d5b1d`.

Starfield was stopped before the configuration copy. The user can now launch normally through MO2, open `InventoryMenu`, select or change an item, close it, then load the Shop. This run is intended to collect only bounded, read-only D3D12 queue submission timing; it does not capture pixels, mutate command lists, open menus, or change inventory. After the run, disable `InventoryRenderTelemetryProbe.ini` before normal gameplay and report that the run is complete so the log can be inspected.

### Result of the controlled run

The user reported that the Shop Explorer test ran. The telemetry artifacts confirm a valid, clean D3D12 observation:

- `InventoryRenderTelemetryProbe.dll` loaded correctly through SFSE;
- the probe loaded with `enabled=true`, located the engine device and direct queue, and installed its verified `ExecuteCommandLists` observer;
- `INVENTORY_OPEN` and `INVENTORY_CLOSE` were recorded for session 1;
- 160 bounded `EXECUTE` batches were recorded during the InventoryMenu window, containing 324 direct command lists total;
- command-list batches contained 1–11 direct lists, with no command-list mutation or GPU work performed by the probe;
- `SFShopExplorer.log` recorded the native `InventoryMenu` callback at 11:32:22;
- Starfield exited cleanly; no new crash dump was created, as expected.

This proves that the D3D12 queue boundary and InventoryMenu timing correlation work. It does **not** identify the inventory mesh, render target, camera, material, or individual draw call: the safe probe intentionally does not decode command lists or hook unverified graphics-list methods. The data is therefore useful Phase 3 timing evidence, not yet a live-render bridge for the Shop Explorer preview.

After inspection, the probe was restored to `enabled=false` in the deployed profile. `ShopTestRender` and `InventorySceneBoundaryProbe` remain disabled.

## 16. Run of 2026-09-09 22:19 — old probe ran again; enhanced probe was not deployed

The user's report was correct: the test ran and was clean. The logs show what actually happened:

- `sfse.txt` (22:19) confirms both `InventoryRenderTelemetryProbe.dll` and `InventorySceneBoundaryProbe.dll` loaded correctly.
- The deployed telemetry DLL was still the **queue-only v0.1 build** (`1388c982…`). The **enhanced build** (`eb56cff5…`, with per-list draw/PSO/resource telemetry) existed on disk in `build/windows/x64/releasedbg/` but had never been copied to the profile, so tonight's run produced the same bounded queue telemetry as before — not draw data.
- The telemetry log (41,717 bytes, 22:21) records two full InventoryMenu sessions: each `INVENTORY_OPEN` → 160 bounded `EXECUTE` batches (1–11 DIRECT lists per batch) → `INVENTORY_CLOSE` with the 96-batch grace window. Queue identity changed between sessions (`0x1597B106880` vs `0x160070BDE80`), consistent with a renderer/scene reset between menu loads.
- `SFShopExplorer.log` (22:19–22:22) shows the normal sequence: `DataMenu` at 22:20:49 → `InventoryMenu` at 22:20:52 → shop opened at 22:21:50 (16 plugins, 406 weapons) → armor catalog browsing at 22:22:21. No errors.
- `InventorySceneBoundaryProbe.log` shows its **lifecycle-only** v0.2 startup line and one `INVENTORY_MENU observed` marker — its INI had been left `enabled=true` in the profile, so it was active but harmless (it performs no Scaleform access).
- No crash dumps newer than 2026-09-08. The run was clean, as the user reported.

### Conclusion and correction

The test was valid but **repeat evidence**: the enhanced probe was built but not deployed, so the run could not produce draw/PSO/resource telemetry. Correction applied after the run (Starfield stopped):

- `InventoryRenderTelemetryProbe.dll` (enhanced, SHA-256 `eb56cff5fedb306cbf6fd990c5fe0e617a1a688df1ad726d22a166723ceb887e`) and its INI deployed to `B:/MO2/Starfield/mods/Shop Test Render/SFSE/Plugins/`.
- Deployed INI: `enabled=true`, with the new keys `traceGraphicsCommands`, `traceResourceEvents`, `tracePipelineEvents`, `maxDrawsPerWindow=4000`, `maxBarriersPerWindow=3000`, `maxResourceEvents=1500`, `maxPipelineEvents=500`.
- `InventorySceneBoundaryProbe.ini` restored to `enabled=false` in the profile.
- `ShopTestRender.ini` remains `enabled=false`.

The enhanced probe hooks, per command list, only documented Windows SDK COM slots (Close=9, Reset=10, Draw=12, DrawIndexed=13, Dispatch=14, PSO=25, Barrier=26, RT/DS=46, VB/IB=44/43) on a per-object cloned vtable, plus device-creation observers for PSO/root signature/command list/descriptor heap/resource/descriptor creation. It remains read-only: it never mutates command lists, resources, or GPU work, and all logging is bounded by the INI caps. Expected log lines in the next run: `DRAW …`, `PSO_BIND …`, `PSO_CREATE …`, and resource-event lines, in addition to the previous `EXECUTE` batches.

Next run (same protocol): launch through MO2, open Inventory, change a few items, close, load the Shop, then report. Afterward the probe INI goes back to `enabled=false`.

## 17. Launch CTD root cause and corrected build — 2026-09-09 23:04

The first attempt at the enhanced probe crashed at launch (attempt 1 hung, attempt 2 hung and Windows captured `Starfield.exe.15064.dmp`, 58 MB, 23:05:06). The probe log isolates the failure precisely:

- Both attempts printed the new line `device object vtable cloned; resource, descriptor, PSO, and command-list creation observers installed` — a stage the old queue-only probe never reached — and then produced no further output before dying.
- Root cause, verified against the Windows SDK 10.0.26100 `d3d12.h` vtable layouts: three **device** slot constants were wrong. The source hooked slot 27 as `CreateCommittedResource`, 29 as `CreatePlacedResource`, and 31 as `CreateReservedResource`, but the documented order is **29, 31, 32** (slots 27/28 are the two `GetCustomHeapProperties` overloads and 30 is `CreateHeap`). Every engine committed-resource creation therefore entered the placed-resource hook, which called the stored original with placed-resource arguments — a signature mismatch on the engine's hottest resource-creation path. That is an instant startup hang/crash, and it matches the log stopping right after the device-clone line.
- The graphics-list slot table was checked in the same audit and is **correct** (Close=9, Reset=10, Draw=12, DrawIndexed=13, Dispatch=14, IASetPrimitiveTopology=20, RSSetViewports=21, RSSetScissorRects=22, SetPipelineState=25, ResourceBarrier=26, SetDescriptorHeaps=28, SetGraphicsRootSignature=30, SetGraphicsRootDescriptorTable=32, IASetIndexBuffer=43, IASetVertexBuffers=44, OMSetRenderTargets=46, table length 60). The device table length was also corrected to 46 slots (through `GetAdapterLuid` at 45).

Fixes applied to `SF Starfield MCM/src/InventoryRenderTelemetryProbe.cpp`:

1. Corrected `kDeviceCreateCommittedResourceSlot` 27→29, `kDeviceCreatePlacedResourceSlot` 29→31, `kDeviceCreateReservedResourceSlot` 31→32; `kDeviceVtableSlots` 44→46.
2. Added hook-family kill switches `installDeviceHooks` and `installGraphicsHooks` to the INI. They gate whether each vtable family is patched at install time (install-time only; flipping mid-run does not uninstall). This makes CTD bisecting a config change instead of a rebuild.
3. Rebuilt: SHA-256 `2fbe7d90e9bade655e1e62009156185fd472a6f33af8121e65887b7346a66708` (640,512 bytes, 23:21).

Deployed state (Starfield stopped): corrected DLL `2fbe7d90…` + INI in `B:/MO2/Starfield/mods/Shop Test Render/SFSE/Plugins/`, telemetry probe `enabled=true` with **`installGraphicsHooks=false`** for the first retry — the safer bisect: device creation observers (now slot-correct) plus the proven queue `EXECUTE` telemetry, with no per-list vtable patching. `InventorySceneBoundaryProbe` and `ShopTestRender` remain disabled. Expected log for a stable run: `RESOURCE_CREATE`, `DESCRIPTOR`, `PSO_CREATE`, `ROOT_SIGNATURE_CREATE` events plus `EXECUTE` batches; `DRAW`/`PSO_BIND` lines only appear in a later run with `installGraphicsHooks=true`.

Bisect protocol if this launch also fails: set `enabled=false` (probe fully off) and confirm a clean launch; then try `installDeviceHooks=false, installGraphicsHooks=true`; then both true. Whatever combination launches cleanly is the one to run the inventory test with.

## 18. Second CTD and the decisive finding: clone+repoint is the crash mechanism — 2026-09-09 23:23

The slot-corrected build (`2fbe7d90…`) still CTD'd at launch, twice, with an identical signature: the log stops right after `device object vtable cloned` + `queue hook installed`, with `installGraphicsHooks=false` and a fresh Windows dump (`Starfield.exe.28008.dmp`, 23:23:47). With graphics hooks already excluded by the bisect, the remaining new element was the device **vtable clone** itself.

Cross-checking SFPostFX's log exposed the real mechanism:

- SFPostFX **already patches the same ID3D12Device vtable in place** (log: `hooked device vtable slot CreateDepthStencilView(21)`, `CreateCommittedResource(27)`, `CreatePlacedResource(29)` — note it uses the OLD slot numbers, which are its own live proven configuration) minutes before the probe cloned that same table.
- The probe's clone captured PostFX's thunks as "originals", then **repointed the engine device's vptr to a private clone**. Every other stable hook in this stack — the probe's own queue `ExecuteCommandLists` hook (stable across many runs), SFPostFX's factory/queue/device patches, SFShopExplorer's factory hooks — patches **slots in place on the shared vtable** and never repoints an object's vptr. The clone+repoint pattern was the single technique the engine's runtime (and possibly the shader/anticheat pipeline) does not tolerate.

Fix (probe source): both installers were rewritten from clone+repoint to **in-place shared-vtable slot patching** — capture current slot contents as originals (chain over PostFX's thunks if present), publish originals first, then `WriteSafeData` each hook into the shared slot. Object vptrs are never modified; per-list state tracking is now keyed off the shared vtable, so the per-list clone map is gone entirely. A stale-`enabled=false` INI copy on first deploy was corrected to `installGraphicsHooks=true` after verification caught it.

Deployed: build `d98d134e286cf937aa6113fcc6d378f7ded1b5806355ac8d62b0b4cd47af31fe`, `enabled=true`, `installDeviceHooks=true`, `installGraphicsHooks=true` — full telemetry (resource/PSO/descriptor creation + per-list DRAW/PSO_BIND/barriers) in one run, since in-place patching is the pattern the stack has already proven stable. If this still CTDs at the same signature, the next bisect is `installDeviceHooks=false` (graphics-only), then queue-only, per the protocol in section 17.

### Addendum — slot re-verification against PostFX's LIVE layout — 2026-09-10 00:03

The 23:41 in-place run did not CTD (no dump) but exited silently ~17s in. Re-audit showed the section-17 "slot correction" was itself wrong: the SDK 10.0.26100 header's vtbl structs contain `#if !defined(_WIN32)` UWP duplicates that inflated the textual parse. The runtime layout — proven live by SFPostFX's long-stable DepthHunt (`DSV=21, committed=27, placed=29`) — is the classic numbering. The 23:41 build had therefore hooked committed-resource semantics into runtime slot 29 (placed) and overwritten PostFX's thunk there: a double ABI break on every resource creation, matching the silent exit. Source reverted to 27/29/30 with a warning comment; the render-thread descriptor-copy storm (game-wide per-descriptor mutex inserts in the Copy* hooks) is now gated to the InventoryMenu telemetry window and capped at 256/call, and `StoreResource`/`StoreDescriptor` are window-gated too. PSO/root-signature tracking stays game-wide by design (the item's PSO is created before the inventory opens).

Deployed for the next test: build `9f3a8c91530faa6df5a64dcb9c1fb4c4f644b3540ce6a5c5be8b0d36b87d2303`, `enabled=true`, `installDeviceHooks=true`, `installGraphicsHooks=true`, other probes disabled. Expected log: `EXECUTE` batches, `RESOURCE_CREATE`/`DESCRIPTOR`/`PSO_CREATE`/`ROOT_SIGNATURE_CREATE`, and per-list `DRAW`/`PSO_BIND` telemetry inside the inventory window.

## 19. FIRST FULL DRAW-LEVEL TELEMETRY RUN — 2026-09-10 00:09 (probe survived, no CTD)

Build `9f3a8c91…` ran clean through a full inventory session with every hook family armed: 21.4 MB log (preserved at `SF Starfield MCM/tools/ba2_extract/telemetry_run_20260910_0009.log`), 4000 capped `DRAW` records (cap hit), 292k dispatches, 41.6k list closes, 480 `EXECUTE` batches, no crash dumps. The in-place shared-vtable pattern with PostFX-proven slots is stable.

### The item-scene fingerprint (decisive Phase 3 result)

- The inventory item scene renders into a discrete **1280×720 offscreen render target** — 254 draws at `viewport=1280x720`, dominated by PSO `0x2E780A186D0` (250 draws: fullscreen-quad passes, `count=3`, topo=4) plus geometry passes through PSO `0x2E77E20CA80` with scissored rects at UI positions (`86,96-286,229`, `955,410-1224,546`) — i.e., multiple item rects (main preview + secondary views) composite into the same 720p scene target.
- A second item path writes directly at UI-rect coordinates: **400×266 viewport at (173,193)** with real mesh draws (indexed, 1350 indices, ~4 GB VB range / 16 MB VB stride-12 pools) writing to RT handle `0x2E6FA8DA3D0` + DS `0x2DB07160240`. This is the per-rect item render the PreviewSceneRect contract drives.
- The main screen pass is 3476 draws at 2560×1440 (the game's render resolution) — cleanly separable from the item paths by viewport.
- `PSO_CREATE` = 0 during the window: the item's pipelines are created **before** the menu opens, confirming that PSO/root-signature tracking must remain game-wide (as built).
- The resource backing RT `0x2E6FA8DA3D0` could not be resolved this run: its RTV creation happened before the telemetry window and descriptor stores are window-gated. One refinement needed: record RTV/DSV descriptor creations game-wide (log only in-window) so the item scene's **exact D3D12 resource** is captured next run.

### What this unlocks

1. **Path A (region copy) is now implementable with exact coordinates**: the item scene exists as a bounded offscreen 1280×720 target plus per-rect viewports; a Present-time copy of the PreviewSceneRect screen region no longer guesses — the probe gives the true rect geometry per resolution/UI scale.
2. **Direct texture capture (path A+)** is one probe refinement away: resolve RT `0x2E6FA8DA3D0`'s resource, then read/copy that texture at Present — full-fidelity item scene, no screen-region cropping, no Scaleform access.
3. The probe is **proven stable** in its final form (in-place patches, PostFX slots, window-gated tracking) and can be re-enabled any time for follow-up runs.

Probe restored to `enabled=false` after the run. Log preserved before overwrite.

## 20. Item render target RESOLVED to an exact D3D12 resource — 2026-09-10 00:20–00:26

Run with build `45ea7959…` (game-wide resource/descriptor stores, 8000 draw cap): clean, no crash dumps (the three `.dmp` files under `overwrite/Root` are the old 23:03–23:43 crashes from sections 17–18). Log 97.7 MB, preserved at `SF Starfield MCM/tools/ba2_extract/telemetry_run_20260910_0026.log`. Totals: 1,397,216 DISPATCH, 186,965 LIST_CLOSE, 12,000 DRAW (cap hit), 6,000 BARRIER, 2,840 DESCRIPTOR, 1,000 PSO_BIND, 640 EXECUTE, 160 RESOURCE_CREATE, 4 inventory windows (two short pre-init pairs, two real sessions).

### Decisive result — the item scene texture has a name

Session 2's item draws carry **resolved resource identity** (the session-1 game-wide descriptor store had captured the RTV before the window opened):

```
DRAW ... indexed=true count=1350 ... pipeline=0x20F7BFB9400
  rt0=0x20EFAA64D00:res=0x210EDFD6DA0 fmt=26 kind=3
  ds =0x20EFAAE2770:res=0x20FF5F6A880 fmt=20 kind=4
  viewport=400.000000x266.000000@173.000000,193.000000  scissor=173,193-573,459
```

- **Item render target resource = `0x210EDFD6DA0`**, format 26 = `DXGI_FORMAT_R11G11B10_FLOAT` (HDR), used with depth resource `0x20FF5F6A880`, format 20 = `X32_TYPELESS_G8X24_UINT` (32-bit depth).
- The identical mesh draw appears in **both sessions with the same vertex/index buffer virtual addresses** (`ibVA=0x221BD8F00`, `vb0VA=0x216DC0300`, 16,776,448-byte stride-12 VB pool, 1350 indices) but different pipelines and RT handles per session — confirming a per-session RT pool serving the same item-mesh path.
- Viewport census: main screen 2560×1440 = 10,514 draws; offscreen scene 1280×720 = 736; per-rect item viewports 400×266 = 24 and 572×274 = 20 across a small pool of RT handles (`0x…A8DA3D0/430/490/4B0`, `0x…A64D00/D60/DA0/DE0`) — multiple item rects (preview + secondary views) each own a target from the pool.
- Session-1 handles (`0x2E6FA8DA3D0` etc.) appear in 259 DRAW lines but zero DESCRIPTOR events: those RTVs were created before that session's stores existed; session-2 stores (game-wide after the section-19 fix) resolved them. Resource/RTV addresses are **per-session** — the capture module must resolve at runtime, not hardcode.

### What this changes

1. **Path A+ (direct texture capture) is now unblocked**: at Present time, resolve the current item RT by size/format signature (≈400×266…572×274, R11G11B10_FLOAT, HDR RT pool) via game-wide RTV tracking, `CopyResource` it into a shop-owned texture, create an SRV in the shop's heap, and draw it in the PREVIEW viewport. No Scaleform, no screen cropping, full HDR fidelity.
2. The capture module must track `CreateRenderTargetView`/`CreateDepthStencilView` game-wide (in-place vtable pattern, slots 20/21, PostFX-compatible) to keep the CPU-handle→resource map current across renderer resets.
3. Probe restored to `enabled=false`; boundary probe and ShopTestRender remain disabled; log preserved.

## 21. Shop capture module prepared (inert); verification run armed — 2026-09-10 01:22

### Shop-side implementation

New `ItemCapture` module in SF Shop Explorer (`src/ItemCapture.h/.cpp`), wired into the overlay and browser UI:

- **Hooking**: observes `ID3D12Device::CreateRenderTargetView` at the live-proven slot 20 via **in-place shared-vtable patching** (the only pattern this stack tolerates — sections 17–18). Chains over whatever occupies the slot, including PostFX's thunk; never repoints vptrs.
- **Candidate filter**: textures 256–1024 × 128–768, single-sample — the measured item-rect band (400×266, 572×274) — excluding the 1280×720 scene target and main screen. Bounded map (64).
- **Menu gating**: SFSE task thread polls `RE::UI::IsMenuOpen("InventoryMenu")`; game objects are only touched there, never on the render thread.
- **Mode 2 capture path**: on the shop's own command list each Present (after the engine's frame lists on the same direct queue), `CopyResource` from engine RT (read-only, no barrier against the engine resource; COMMON→COPY_SOURCE implicit promotion) into shop-owned textures, SRVs created in heap slots 12–15 (heap enlarged 8→16), drawn aspect-fit in the PREVIEW viewport.
- **INI**: `[itemcapture] mode=` in SFShopExplorer.ini — 0 off (default), 1 verify, 2 capture.

### Deployed state

- `SFShopExplorer.dll` build `1ca72cc3ff5f1ef9…` deployed, hashes match, INI set to **mode=1 (verify)**: RTV observer active + one `itemcapture session candidates=…` line per InventoryMenu session in `SFShopExplorer.log`, zero GPU work.
- All probes disabled (telemetry, boundary, ShopTestRender) — the run only exercises the shop + verify observer.

### Next test protocol

1. Launch, open InventoryMenu once, exit game.
2. Check `SFShopExplorer.log` for `itemcapture session candidates=` — the live candidate list confirms slot-20 observation inside the shop process and that the candidate filter matches the engine's item targets.
3. If candidates match the probe-measured rects → set `mode=2` → next run shows the live item scene in the shop PREVIEW viewport. If empty → widen the filter per the logged candidate sizes (no rebuild needed to change size band? — currently compiled; a rebuild is required if the band is wrong).

### Verify run result — 2026-09-10 01:24 (slot-20 observation CONFIRMED)

Clean session, no crashes: shop loaded (`renderMode=4`), RTV observer installed (`slot 20, mode 1`), InventoryMenu opened 01:26:08, exited via PauseMenu at 01:27:47.

The observer fired on menu open — but matched **640 candidates**, all generic UI textures (598×338 f60, 512×512 f26, 696×392 f33, 622×350 f39 …): the box-only size band was far too permissive. The probe data (section 20) already contained the discriminator, so the filter was tightened and rebuilt:

- `formatFilter` (INI, default 26 = R11G11B10_FLOAT): only HDR-float targets — the item-scene format the probe measured; none of the UI textures matched it.
- Size band tightened to the measured rects (180–640 × 120–330), aspect 1.40–2.20 (kills the square f26 textures), single-sample.
- Recency ring (16, newest kept): the probe showed the item RT pool is recreated per session, so newest = current session's pool. Capture feeds slots newest-first, so slot 0 (PREVIEW) shows the freshest item target.
- Deployed build `d69d11fb653b4a1b…` with `mode=2` — next run either shows the live item scene in the PREVIEW viewport or produces a decisive, logged failure.

If the PREVIEW stays empty, the next diagnostic is a one-session `mode=1` rerun: the candidates line now prints with the tightened filter, distinguishing "no targets match" from "capture path failed".

### Mode-2 run result — 2026-09-10 01:38 (candidates=0 → filter recalibrated on the wrong field)

Clean session, no crashes, shop fully browsed (all catalogs). Log: `itemcapture config mode=2`, observer installed, **`itemcapture session candidates=0`** at InventoryMenu open. The tightened filter matched nothing — root cause: it filtered on the **resource** creation format and assumed viewport-sized resources, but the probe's `fmt=26` was the **RTV view format**, and the resource behind a 400×266 viewport can be any size (viewports carve rects out of shared/atlased textures).

Fix (build `cb02078a636f759d…`, deployed `mode=1` for a ground-truth census run):

- Filter now checks the **RTV view format** (falls back to resource format when the view desc omits it); resource dims widened to 128–4096 (only extremes excluded).
- **RTV census**: first 48 RTVs after each menu-open edge are logged verbatim (`res=0x… WxH resFmt=… viewFmt=… samples=…`), plus a 2-second trickle (8 lines) while the menu stays open — covers RTVs created before the open edge. Budget refills per session only, so log cost is bounded.
- Session reset on menu-open: candidates cleared (stale resource pointers from a previous session are never reused — the probe proved per-session pool recreation).

Next run: `mode=1` census → the census lines name the exact item-pool signatures → recalibrate the filter on measured fields → `mode=2`. No further guessing.

### Run of 2026-09-10 01:50 — census not triggered; multi-item basket shipped

The 01:50 session never opened the native InventoryMenu (log shows shop browsing only, closed at 01:53:45), so the RTV census produced no lines — the capture side remains armed and awaits an inventory open. Separately, this run exposed a shop UX bug: the basket held only one item (`basketFormIndex` scalar), so ADD TO BASKET replaced the previous selection.

**Multi-item basket (build `e7eef14ffdd70cf9…`, deployed):**

- `BasketEntry {formID, edid, count}` vector replaces the scalar basket; ADD TO BASKET appends (merging duplicates), so any number of distinct items accumulates.
- Basket UI: per-line `-/+/x` controls, per-line and total credit pricing, ACCEPT transfers every entry in one pass on the task thread (all-or-nothing: every FormID is re-validated and the full cost checked before anything moves), CANCEL clears.
- Stale-FormID safety: entries resolve by FormID at accept time, so catalog refreshes between add and accept cannot transfer a dangling pointer.

Next capture run should combine both: open the native inventory once (census lines fire), then shop basket test.

### Census run of 2026-09-10 02:04 — item-RT geometry pinned; capture redesigned as direct SRV display

Clean session (no dumps), InventoryMenu opened 02:04:44, basket multi-add confirmed working. The census fired on the menu-open edge and delivered decisive ground truth:

- **The item scene renders into fullscreen-size HDR targets, not small per-rect textures.** The census burst (35 RTVs in ~3 ms on the render thread, ~80 ms AFTER the menu-open edge) shows the engine recreating its whole RT pool at inventory open: seven 2560×1440 fmt-26 (R11G11B10_FLOAT) targets, one 256×256 fmt-26 UI atlas (8 RTVs on one resource — atlas), 1280×720 fmt-33/39/60 targets, plus one 1024×5608 degenerate strip.
- This matches the preserved probe log exactly: RTV handle `0x20EFAA64D00` → resource `0x210EDFD6DA0` is used BOTH fullscreen (2560×1440 viewport) and with the item viewport `400×266 @(173,193)` — the item image is a **sub-rect of a shared fullscreen HDR chain**.
- The engine's own BARRIER data shows these targets repeatedly transitioned to `0xC0` (shader-resource-readable) every frame — sampling them adds no state writes.
- Census also explains `candidates=0` from the 01:39 run: the one-shot session log fired at the menu-open edge, but the fmt-26 pool RTVs were created milliseconds later on the render thread. Candidates were stored after that; nothing re-evaluated.

**Capture redesign (build `53a09f0a2f85e99d…`, deployed, `mode=2` armed):**

1. **Continuous candidate evaluation** — candidates stored as RTVs arrive; session logging moved to `Update` so it reports after the pool burst, not before it.
2. **Aspect band replaces size band** (INI `minAspect=1.45 maxAspect=2.15`, `maxDimension=8192`): fullscreen HDR chain (1.78) passes; the 256×256 UI atlas (1.0) and 1024×5608 strip fail. `formatFilter=26` unchanged.
3. **Mode 2 is now direct SRV display, zero copies** — SRVs bound straight onto the engine's candidate textures (recreated per session, rebind by resource identity). No CopyResource, no barriers, no state writes (engine keeps them SRV-readable; COMMON→read promotion is implicit). Slot 0 feeds PREVIEW.
4. **Sub-rect drawing** — PREVIEW crops to the probe-measured item region (UV 0.0676,0.1340 → 0.2239,0.3187; configurable `uvX/uvY/uvW/uvH`) instead of drawing the full HDR texture.

Test protocol unchanged: launch → open inventory (browse items) → open shop (Page Up) while inventory still open or after browsing → PREVIEW should show the live item scene region. If empty, the session `candidates=N` line now reports AFTER the pool burst and the census identifies what the aspect band missed.

### CTD at 02:26 (pre-main-menu) — root cause: un-owned engine resource pointers; fix: COM lifetime ownership

**Crash signature:** dump at 02:26:37, ~1 s after `first native draw submission`, right after the MainMenu callback — the first RT-pool burst of the session. The 02:26 build (`53a09f0a…`) had removed the menu-open gate from `Update`, so mode 2 immediately began creating SRVs on fmt-26 fullscreen candidates accumulated from startup — including **transient loading-time targets the engine had already freed** (Fader/Loading menu pools). A D3D12 descriptor does not keep a resource alive; `CreateShaderResourceView` on a freed resource = the AV.

**Fix (build `6fbe4cfc6e753497…`):**

1. **COM lifetime ownership** — every accepted candidate `AddRef`s inside the RTV hook (the one moment the pointer is provably alive) and `Release`s on session clear/ring eviction/shutdown. No candidate pointer can ever go stale afterward, regardless of what the engine frees. VRAM bounded by the 8-entry ring.
2. **Session clear also invalidates all display slots** — no descriptor can outlive the texture it points at.
3. **Menu-open gate restored** for mode-2 binding — GPU work only while InventoryMenu is open, as in the stable 01:39 generation.
4. **Explicit SRV descs** (typeless-safe; view format + MipLevels clamp + default component mapping) and per-loop pinned refs while binding.
5. **Slot state under the data mutex** — FramePoll (task thread) and Update (render thread) can no longer race on slot writes.

**Deployed `mode=1` (stability-proof run):** this exercises the identical code path that crashed — hook, AddRef, census, candidate accumulation across all menu pool bursts — with only the SRV creation step disabled. If it reaches main menu + inventory cleanly, flip `mode=2` for the live PREVIEW run.

## 22. Run of 2026-09-10 08:17 — capture pipeline VERIFIED end-to-end; failure isolated to target selection — 2026-09-10 12:57

The COM-ownership build (deployed 02:43) ran clean, no dumps. `SFShopExplorer.log` delivers the decisive evidence:

- Config/observer: `itemcapture config mode=2 menu=InventoryMenu formatFilter=26 aspect=1.45-2.15`, `RTV observer installed (slot 20, mode 2)`, `first native draw submission beginning` — the exact code path that CTD'd at 02:26 is stable.
- Census fired at the InventoryMenu open edge (08:17:36) and the session pool burst matches section 20 exactly: seven `2560x1440 resFmt=26 viewFmt=26` fullscreen HDR targets, the `256x256` UI atlas (8 RTVs, one resource), the `1024x5608` strip, and the fmt-33/39/60 UI textures — all correctly excluded by the aspect band. `StoreCandidate` accepted the seven HDR targets (bounded by the 8-slot recency ring).
- PREVIEW nevertheless showed the placeholder: **no GPU failure, no sampling error — a target-selection problem.** The seven fullscreen candidates are interchangeable to every filter field (dims, format, aspect); slot 0 = newest created = an arbitrary chain member, not provably the item image. The UI only ever drew slot 0.
- Secondary bug: the session `candidates=` line logged at the open edge (08:17:36.224, `candidates=0`) BEFORE the render-thread burst (first fmt-26 line at .219 on thread 15296, burst on thread 4692 at .302) — the one-line summary reported the pre-burst state.

### Fix (build `c6198cd5699428b64956e6ff211434ae905a58b8b798fe9c021eb968ec6727b4`, deployed 12:57 with hashes matching)

1. **All 8 slots bound** (was 4): overlay SRV heap enlarged 16→24 (slots 12..19 reserved), `ItemCapture::kMaxSlots = 8`. With only 7 candidates expected, every session target is now visible simultaneously.
2. **Diagnostic grid view** (INI `fullTexture=1`, default): PREVIEW draws every bound target's full [0,1] extent side by side, each labeled `slot:WxH fmt`, the primary slot outlined amber, the INI sub-rect outlined cyan on the primary. The item image is eye-identifiable in one run — the filters cannot distinguish the seven, but the eyes can. `fullTexture=0` restores the sub-rect crop view.
3. **INI slot pinning** (`slot=N`, default -1): pins the primary PREVIEW slot by index once the grid identifies the winner; -1 follows the newest candidate. Snapshot carries per-slot resource address + view format for labeling.
4. **Capture status line** in PREVIEW: `hook not installed` / `disabled (INI mode=0)` / `waiting for InventoryMenu (candidates=N)` / `menu open, 0 targets bound (candidates=N)` / `N target(s) bound, primary slot M` — an empty viewport is now self-explaining.
5. **Per-bind log lines** (`itemcapture slot N bound res=0x… WxH viewFmt=…`) plus **session summary timing fix**: the summary now fires ≥600 ms after the open edge (the burst lands ~80 ms in), so `candidates=N` reports the real pool.
6. Repo hygiene: the 09:29 reorganization had moved `Depends/commonlibsf-main` to `_archive`, breaking the `includes("../Depends/commonlibsf-main/xmake.lua")` build; restored (copied) into `SF-Shop/Depends/` and regenerated the xmake cache (stale absolute PCH paths from the pre-move layout).

### Next run protocol (INIs verified: telemetry/boundary/ShopTestRender all disabled; shop `mode=2`)

1. Launch through MO2, open the shop (Page Up), browse to an item, open the native InventoryMenu alongside, select items.
2. PREVIEW (grid view): find which slot shows the item scene; note its `slot:N` label and amber outline.
3. Set `slot=N` (and `fullTexture=0`) in SFShopExplorer.ini → next run shows the cropped live item scene.
4. If NO slot shows the item: the item viewport renders into a target the RTV hook never observes (e.g. created before the session pool burst and not recreated since) — rerun `mode=1` and diff the census against the probe's DRAW-line RTs (`400x266` viewport resource), then extend the census window (pre-open RTVs are currently only covered by the 2-second trickle).
5. If ALL slots show the same fullscreen image: the chain members are aliases of one texture; pinning any slot is equivalent and the UV sub-rect alone decides correctness — tune `uvX/uvY/uvW/uvH` while watching the cyan outline.

## 23. Run of 2026-09-10 13:06 — the format discriminator died; identity-based filter + edge-race fix (build `8dd4b837…`, deployed 22:52)

The user reported the same placeholder as 08:17. The log explained it — and this time the run differed from 08:17 in a way that invalidates the filter design itself:

- **The "fullscreen HDR chain" RTVs were the swap-chain back buffers.** The two fullscreen census entries (`0x2887B9275B0`, `0x2887B92C5F0`, 2560x1440, **resFmt=28 viewFmt=28**) are literally SFPostFX's logged back buffers (its log: `buffer[0]/[1] = 0x2887B9275B0/0x2887B92C5F0 2560x1440 R8G8B8A8_UNORM (28)`). Something in the display/config stack changed between 08:17 and 13:06 and the main chain moved from fmt 26 (R11G11B10) to fmt 28 (R8G8B8A8). `formatFilter=26` therefore matched **nothing** — `candidates=0` was correct this time, not a timing artifact.
- **The open-edge race is real too**: the burst landed at `.498` and the InventoryMenu open edge was polled at `.530` — the 02:04-style clear-on-open would have wiped the pool even if the filter had matched.
- **Census starvation**: 47 of 48 census lines were repeat RTVs on just 3 resources (PostFX recreates back-buffer RTVs per frame; one 256x256 atlas took 40). The burst crowd starved the budget before the item pool could log.

### Fixes (build `b5d5d6fc8b1ecd3578b5e2e80e60cb406684e278639a9eeb27fe855c07dd0b26`, deployed; the first deploy `8dd4b837…` preceded a header cleanup with identical behavior)

1. **Identity replaces format as the discriminator**: the overlay now calls `ItemCapture::RegisterSwapChainResources()` every Present with the swap chain's back buffers; the RTV hook excludes them by resource pointer (census still logs them). `formatFilter` default flipped to **0 (any)** — the INI was also corrected (it had 26, which would have kept rejecting everything).
2. **Open-edge race fix**: no clear-on-open. Clearing now happens only when the menu is observed **closed** past a 1.5 s grace window — a real session boundary. The pool that lands before the open edge survives and becomes the session's candidate set.
3. **Dedupe accounting**: repeat RTVs on already-tracked resources are counted (`duplicateRtvs`) instead of silently returning; rejected back buffers are counted (`rejectedSwapChain`). Both appear in the snapshot and in the PREVIEW status line (`cand=N dup=N bb=N`), so the next empty viewport is quantitatively self-explaining.

### Deployed state

- `SFShopExplorer.dll` `b5d5d6fc…` in `B:/MO2/Starfield/mods/Shop Test Render/SFSE/Plugins/` (hash matches build; prior DLL preserved as `SFShopExplorer.dll.pre-swapslot-fix-20260910-2252`).
- INI: `mode=2`, `formatFilter=0`, `fullTexture=1` (grid), `slot=-1`; telemetry/boundary/ShopTestRender probes all disabled.
- `build.cmd`'s stale hard-coded project path (D:\Projects\Address Library\...) replaced with `%~dp0`.

### Next run protocol

1. Launch through MO2, open the shop, browse an item, open the native InventoryMenu alongside, change items.
2. Expected: PREVIEW grid shows **up to 8 non-back-buffer HDR targets** (the session pool, whatever format it uses now). The status line reads `N target(s) bound, primary slot M`.
3. If a slot shows the item scene: set `slot=N` and `fullTexture=0`; the run after that is the production crop view.
4. If the grid shows only non-item fullscreen images (tonemapped frames, depth, normals): the item viewport sub-rect now has to be found inside those targets — tune `uvX/uvY/uvW/uvH` live against the cyan outline (the measured 400x266 @(173,193) rect is the starting point and is already the default).
5. If still `candidates=0` with `dup=0` and `bb>0`: the item pool RTVs are created before this hook ever sees them (pre-session) — extend the census trickle window (pre-open RTVs are covered only by the 2 s trickle).

## 24. Run of 2026-09-10 23:00 — capture VERIFIED with identity filter; close-edge clear regression fixed (build `5a9e2b87…`, deployed 23:10)

The user's screenshot showed `waiting for InventoryMenu (cand=0)` and placeholder. The log proved the identity-filter build works:

- 8 slots bound at the open edge, including the two **1280x720 offscreen targets** (fmt 61/34 — the probe-identified item-scene containers) plus five 2560x1440 HDR chains (fmt 26/28/61) — the full session pool, no back buffers, no atlas.
- The census fired cleanly with no starvation (one atlas burst, then the pool); `candidates=8`.
- **Root cause of the visible failure**: the user's screenshot was taken AFTER the inventory closed. The 8dd4b837 build's close-edge grace clear (added with the open-edge race fix) released all candidates and invalidated all slots 1.5 s after close — destroying exactly the frozen last-seen scene the shop is supposed to display. PREVIEW correctly reverted to placeholder; the user lost the visual before even seeing it in the grid.

### Fix (build `5a9e2b87f1109b04e0452ffd39fa21a4b6ca41bf2b4f3b845b21e65f733748ca`)

1. **No clear-on-close.** Candidates hold COM references, so the frozen scene stays valid and sample-safe indefinitely; only `Shutdown()` clears now. The recency ring (8 entries) naturally evicts the previous session's entries as the next session's pool arrives, which preserves the lifetime invariant (a descriptor never outlives its texture — ring eviction releases both together).
2. Status line gains a frozen-scene state: `frozen scene from last session (cand=N) — open InventoryMenu to refresh`, so a closed-menu PREVIEW with content is self-explaining instead of lying that it is "waiting".

### Next run expectations

- With InventoryMenu open: PREVIEW grid shows up to 8 tiles (`0:1280x720 f61`, `1:1280x720 f34`, five `2560x1440 f26/28/61`, …). The item scene is expected in one of the two 1280x720 tiles (probe section 19: item path = discrete 720p offscreen target) or inside a fullscreen chain tile.
- After closing InventoryMenu: the grid stays visible, labeled `frozen scene from last session`.
- Pin the winning tile with `slot=N` + `fullTexture=0`; if the winner is a fullscreen chain member, the measured UV sub-rect (400x266 @(173,193) → `uvX=0.0676 uvY=0.1340 uvW=0.1563 uvH=0.1847`) carves the item region.

## 25. 2026-09-11 — offline 3D preview (the shop's own renderer) + catalog name fix + UI fit (build `7f6c6534…`, deployed 01:05)

The architectural wall fell: the engine will only ever render an item scene for
something with a live handle (owned inventory, barter stock). The shop browses
arbitrary catalog forms — the capture pipeline, though fully working, can never
serve that. So the shop now renders items ITSELF.

### Offline 3D preview (`OfflineModel.cpp/.h`, INI `[offlinepreview]`)
- **Pipeline**: `tools/renderer_recon/build_model_cache.py` (outside the game)
  extracts NIF + LOD0 `.mesh` from the Meshes BA2s, converts via Blender
  addon's `MeshConverter.dll` (`ImportNif`/`ImportMesh` → JSON), writes compact
  `.sfmodel` (SFM1) files. The plugin reads ONLY the cache — no BA2s, no
  foreign DLLs, no new GPU pipelines inside Starfield (project rules kept).
- **Plugin side**: PREVIEW calls `RequestPreview(modelPath, label)` keyed by
  the form's model path basename → `<cacheDir>/<key>.sfmodel`. The task thread
  rasterizes (CPU, z-buffered, stylized per-mesh shading — textures deferred,
  materials live in `materialsbeta.cdb`) and the Present path uploads via the
  overlay's own command list to SRV heap slot **20** (heap sized 12+8+4+1).
- Cache shipped: ak_47 (5.4 MB), ar99 + 6 attach-parts (built 23:54, verified).
- INI: `enabled` (kill switch), `dir`, `width/height` (512×384), `pitch/yaw/zoom`.
- **Mods question (answer)**: mod-added items work when their NIF matches a
  cached key; the cache builder scans BA2s *and* MO2's loose `mods/` tree, so
  re-running it covers downloaded mods. True live in-game BA2/NIF reading is a
  deliberate NO (in-process foreign code / new GPU pipeline = the project's
  forbidden risk class).

### Catalog fixes (all from the SFDevilzDump ground truth the user pointed at)
- **Names**: `FormDisplayName` now RTTI-casts to `TESFullName::GetFullName()`
  (the dump's exact proven method). Aid/Keys/Books/Misc/Ammo were "(unnamed)"
  because they carry FULL names, not editor IDs — dump rows prove it
  ("Tranquilitea Classic", "Weapon Rack Key", "Digipick").
- **Ingredients empty is CORRECT**: zero INGR rows in the entire game database.
  The tab now says so and points to Aid. This was never a bug.
- **Form-array scan hardened** (the actual latent bug): scan now covers the
  full declared `formArrays` range (0x070–0x1498, the old scan started at 0x0E0
  and missed the first four arrays), and each array is verified by a
  homogeneous type-run over its first entries — a null element[0] no longer
  disqualifies an array (Ingredients' array would have been dropped by the old
  heuristic on any plugin with a null first slot).

### UI fit + crispness
- Window now opens **maximized** (`fullscreen=true` default; MAXIMIZE/WINDOWED
  toggle remains) — no more 1500×880 default clipping smaller screens.
- Crispness note: the "pixelated" UI is the single font-atlas rasterization
  ImGui uses; rasterization happens once at UI scale. Font atlas rebuild on
  scale change is the follow-up lever (ImGui_ImplDX12 has
  Create/InvalidateDeviceObjects) — not pulled yet; uiScale=1.0 keeps it crisp.

### Deployed state
- `SFShopExplorer.dll` `7f6c6534…` (source-hash-matched), backup
  `SFShopExplorer.dll.pre-offline-3d-20260911-0105`. INI: capture `mode=2`,
  grid view (`fullTexture=1`, `slot=-1`), `[offlinepreview] enabled=1`.
- Cache deployed to `SFSE/Plugins/SFShopExplorerAssets/models/`.

### Next run protocol
1. Launch via MO2 → open shop (Page Up). Window opens maximized.
2. Browse Weapons → Rattler/AK-47 etc: PREVIEW should show the offline 3D
   render ("offline 3D — <model> — N mesh(es), M tris" tag) within ~1 frame
   after selection (first request per model rasterizes once).
3. Aid/Keys/Books/Misc/Ammo: real names everywhere; EDID shown beneath.
4. Ingredients: shows the "no INGR exist in Starfield's data; use Aid" note.
5. If a weapon shows only "no cached model": its NIF isn't in the cache yet —
   re-run the builder with `TARGET=<nif-basename-substring>` (or raise
   LIMIT_NIFS) and copy the new .sfmodel files.

## 26. 2026-09-11 — HOTFIX: form-array scan reverted to proven heuristic (build `c8a7994a…`, deployed)

The 01:05 section-25 scan rewrite (declared-offset scan 0x070–0x1498 + strict
2-element type verification) **broke every catalog tab at runtime** — only
weapons/clothes survived. Lesson recorded: the declared TESDataHandler layout
does not match this runtime build's actual member origin/stride, so
offset-driven reading misaligned everything. The content-driven heuristic
(start 0x0E0, 0x18 grid, BSTArray-header validation, identify by first live
element) is the proven path and is restored verbatim, with two deltas:

1. Null element[0] no longer disqualifies an array (probe forward to the first
   live element). Element-level type filtering still happens per-row in
   BuildFormListForType, so a probe cannot inject wrong-typed rows.
2. One INFO line per discovered array (`form array for type N at offset 0x…`)
   so future regressions are diagnosable from the log instead of a blank tab.

Rule reinforced: change ONE variable at a time on layout-sensitive scans — the
old scan was validated in production; its replacement must be proven with
telemetry before displacing it.

## 27. 2026-09-12 — LiveModel .mesh layout root-caused and fixed (my bug, my fix)

### Symptom (my regression)

The offline 3D preview rendered **exploded/shredded geometry** for outfits,
spacesuits, and the silver ore chunk (screenshots 12/09), while the rigid
dataslate rendered correctly. Root cause was entirely in my
`LiveModel.cpp::ParseMeshLod0`, not the workspace, BA2s, NIF walk, rasterizer,
or GPU path.

### Ground-truth proof

Extracted the failing NIFs/.mesh files from the Meshes BA2s and compared
against MeshConverter `ImportMesh` OBJ output vertex-by-vertex:

- True `.mesh` LOD0 layout: `u32 unk, u32 nidx, u16 idx[nidx], f32 border,
  u32 weightsPerVert, u32 nv, s16 xyz[nv]` with
  `pos = s16/32767 * border` (border = max_border: 2.0 outfits/spacesuits,
  1.0 small rigid). UV stream (`u32 nuv + f16 pairs`) follows immediately.
- My old "two layouts" code (direct-nv vs +8 index shift for clothing) read
  the `border/weightsPerVert` header as indices/verts: every triangle was
  misconnected (outfit cached indices `[2,3,4,5,1,4,…]` vs true
  `[0,1,2,0,2,3,…]`, 19485 vs 19488) and positions missed the ×border scale.
- A Python replica of the old parser produced **byte-identical** corruption
  to the live-built `outfit_medical_doctor_lowerbody_01_f.sfmodel`.
- NIF walk, node transforms (outfit = translate z +1.6323 only), and the
  MeshConverter-based prebuilt weapon cache were all verified correct and
  are untouched. The DLL's own "Offset of Positions" printout was also
  shown to be unreliable (points inside the index stream for skinned
  meshes) — ground truth OBJ won over it.

### Fix (`LiveModel.cpp`, build `4C009B01…`)

Single layout, strict validation, fail closed: `nidx`/`nv` must match the
NIF LOD entry EXACTLY (mismatch = `return false`, no warn-and-continue);
border must be finite and sane (NaN/inf/zero rejected); every index is
range-checked before acceptance (any OOB = fail closed, no silent triangle
drops); positions scaled by border.

### Verification (all byte-checked vs MeshConverter)

outfit_medical (3903v, w=8), dataslate all 5 LODs, silver LOD0, suitzero
(16057v, w=7), trucker_tee — every vertex within 5.5e-5, every index exact.

### Deployed state

- `SFShopExplorer.dll` `4C009B01E5624DC990B995EEBBF10D6EF6A07AFE85053A22716B362571E55B1F`
  (1197568 bytes) in `Shop Test Render/SFSE/Plugins/` (hash matches build;
  prior DLL preserved as `SFShopExplorer.dll.pre-meshlayout-fix-20260912-1315`).
- **All 495 live-built `.sfmodel` files in MO2 `overwrite/.../models/` were
  deleted** — they were baked with the corrupt parser and would otherwise
  keep loading from disk cache ahead of the fixed builder. The prebuilt
  weapon cache in the mod dir is untouched (verified good). Caches rebuild
  correctly on next browse.
- INI unchanged (`[livemodel] enabled=1`, `[offlinepreview] enabled=1`).

### Next run protocol

Launch via MO2 → Page Up → browse the previously broken items (Medic Uniform,
Resort Wear, Mark I Spacesuit, Silver). Expect coherent solid renders with
`offline 3D — <key> — N mesh(es), M tris` tags. First view per item
rebuilds the cache (brief "building offline render..."), second view is
instant. If any item still shows shards, its `.sfmodel` in overwrite plus
the `SFShopExplorer live:` log lines (`mesh ... mismatch`, `index out of
range`) name the exact failing field — no guessing.

## 28. 2026-09-12 — orbit inspection + per-part model label (build `78E9E2BD…`)

### "Missing top half" — not a bug

The Neon Socialite Skirt preview is complete: its form points at
`..._skirt_lowerbody_01_f.nif`, which contains exactly one geometry
(`..._LowerBody_01_F:0`, 2163v). Starfield splits outfits into one NIF per
body part (`lowerbody` / `upperbody` / `sleeves` / hat NIFs side by side in
the same folder, separate forms). The reference full-set look = 3+ shop rows
worn together. To make this self-evident, the detail pane now shows
`Model: <path>` under Plugin, so `..._lowerbody...` vs `..._upperbody...`
reads directly off the selection.

### Drag-to-rotate preview

- `OfflineModel::{AddOrbit,AddZoom,ResetOrbit}`: atomic yaw/pitch offsets on
  top of the INI camera + zoom multiplier + a dirty flag. New model selection
  resets to the INI camera. Lock-free (Present thread never waits).
- `UploadGPU` consumes the dirty flag every Present: GPU path re-renders
  through the existing PSO/VB/RT (same texture/SRV, zero new resources);
  CPU fallback re-rasterizes inline. Snapshot stays valid throughout.
- Shop viewport: an `InvisibleButton` over the preview rect — left-drag
  rotates (0.45°/px), wheel zooms (×1.12/notch, clamped 0.35–4×), double-click
  resets. Hint line drawn top-left of the viewport when a render is shown.
- Deployed `78E9E2BD71633462D1575D99801F1AB9319D4AE60E4C683BFE59B66D6FFBF483`
  (prior DLL preserved as `.pre-orbit-20260912-1330`). No geometry changes —
  disk caches remain valid, no wipe.

## 29. 2026-09-12 — orbit diagnostics + middle-drag zoom (build `EC0E7124…`)

User report: rotation works on some models, others visibly "try but refuse";
wheel zoom asked to move to middle mouse. Shipped:

1. **Middle-drag zoom** (no wheel-event dependency): the orbit button now
   listens to left+middle (`ImGuiButtonFlags_MouseButtonLeft |
   ImGuiButtonFlags_MouseButtonMiddle`); middle-drag vertical zooms
   (up = in, `1 - dy*0.004` per frame, existing 0.35–4× clamp). Wheel kept.
2. **Live camera readout** in the viewport (`yaw +12  pitch -3  zoom x1.00`,
   second hint line): decisive split for the refusal — if the numbers move
   but the image doesn't, the render path ignores the camera; if numbers
   freeze, the drag never reaches `AddOrbit`. Report which + the item key.
3. Deployed `EC0E7124F6ECD1CF844D6EAA92E1025BC292D3F083EAAA441EFE3C17C005F663`
   (prior DLL preserved as `.pre-orbitdiag-20260912-1345`). View-only again —
   caches valid, no wipe.

## 30. 2026-09-12 — full-outfit assembly + texture prefetch + color-index fix (build `F38F6386…`)

User evidence: guns never leave `yaw +32 pitch -22` (render thread stalls),
outfits show one part only, everything renders flat grey.

### 1. Guns don't rotate — texture I/O was on the Present thread

`RenderGpuModel → UploadModelTextures` ran `FindDiffuse` (which builds the
color index by opening ~100 BA2s) plus per-material `ReadTexture`
(seek+decompress) **on the render thread**. A 28-mesh rifle stalled Presents
for seconds: drags went nowhere. Fix: split in two — task-thread
`PrefetchTextures` (runs in `ServicePending` after pixels publish, so first
paint never waits) decodes diffuses into a memory cache; render-thread
`UploadModelTextures` only creates GPU objects from memory and renders plain
until the prefetch lands (completion re-arms a frame, colors pop in alone).
The render thread now takes only the tiny prefetch mutex — never the
LiveModel I/O mutex, never the filesystem. A prefetch-request slot covers
revisits/evictions (cache capped at 24 keys, re-warmed on demand).

### 2. Missing colors — DX10 archives were invisible to the matcher

`EnsureColorIndex` scanned only `ar->entries` (GNRL archives), but every
`_color.dds` lives in DX10 texture archives (`texFiles`). The index was
empty, `FindDiffuse` never hit, all previews stayed steel-grey. One-spot
fix: index `texFiles` names too. Offline proof: exact family-scoped hits for
both test outfits (scores 2034+) with per-part maps (lower/upper/sleeves).

### 3. "Render all" — sibling outfit assembly

`LiveModel::BuildModel` now merges same-set body-part NIFs: when the key
contains `upperbody`/`lowerbody`/`sleeves`, the other two are built and
appended (same bind-pose body frame, so parts align). Single-NIF items
(weapons, suits, clutter) are untouched; misses stay silent + negative
cached. The cache file under the requested key holds the full set, so every
part row shows the whole outfit and the mesh-count tag grows accordingly.

### Deployed state

- `F38F638685E618732722B9091B272A521111FF9F681FD8E8C085D296E19CB5F9`
  (prior DLL preserved as `.pre-assemble-textures-20260912-1400`).
- **Wiped all live-built `.sfmodel` in MO2 overwrite again** — assembly
  changes cache contents (single-part files would mask the feature).
- INI unchanged.

### Next run protocol

Browse a gun first: it should paint instantly (plain), rotate freely WHILE
colors pop in a moment later. Then the Neon skirt row: full upper+lower+
sleeves in one view, textured. If a part misaligns (floating/disjoint), its
sibling NIF root transform differs from the bind frame — report the item + a
screenshot and the assembly becomes per-part-framed instead of merged.

## 31. 2026-09-12 — log found, per-key GPU ban, tiny-mesh fallback, hover-drag (build `71AD78B5…`)

### The log was here all along

`SFShopExplorer.log` lives in `Documents\My Games\Starfield\SFSE\Logs\`
(note the `Logs\` subdir — earlier searches only listed `SFSE\` itself).
Every REX line was landing there (1.5 MB from today's session). It proved:
color index = 28,913 maps (DX10 fix works), assembly firing (ecliptic 3
meshes, varuun 2, …), rescueaxe textured on GPU (2/2 prefetched) — and:

### One 12-tri cube killed GPU for the whole session

`desktop_testclutter01` (1 mesh, 12 tris) passed every size filter, so
`BuildGpuModel` produced zero ranges → `RenderGpuModel` false → the old
**global** `g_gpuFailed` banned GPU for everything after 14:13:45. That is
why later items (notes, books, coats) went flat/beige CPU fallback while
earlier ones (grenade, knife, axe) stayed textured GPU. Fixes:

1. **Per-key GPU ban** (`g_gpuBanned` session set): only the failing model
   falls back; everything else stays GPU. Logged per key.
2. **No-skip fallback for tiny meshes** (GPU `accumulate(false)` retry +
   CPU `anyBig` gate): a 12-tri item renders small instead of vanishing
   (Gabby's Plea empty viewport) or banning GPU.
3. **Hover-drag**: rotation gated on `IsItemActive`, but presses usually
   start on the list row and drag into the preview — the button never
   activates, so those drags silently did nothing (the axe at frozen
   `yaw +32`). Now gated on `IsItemHovered` for left and middle drags.
4. **Clear color unified**: CPU `0xFFB8C0D0` (warm) → `0xFFD1BFB8`, byte-equal
   to the GPU clear (0.72,0.75,0.82) — no more blue-vs-cream tell.
5. Reverted an accidental duplicate `SFSE_PLUGIN_VERSION` in main.cpp (the
   xmake `commonlibsf.plugin` rule already generates it from the
   `COMMONLIB_PLUGIN_*` configvars — LNK2005). No behavior change.

### DX10-vs-DX12 aside (user asked)

DX10 is only the BA2 *texture container* version (FO4-era toolchain name) —
the runtime is DX12 regardless. Nothing odd; the fix simply reads both
tables now. Keep the two layers distinct: the BA2 reader decodes source bytes;
the D3D12 resource/SRV format and shader sampling determine how those bytes
become pixels.

### DX12 texture-view color-space regression recovered from ENB notes (2026-10-14)

The user pointed to the adjacent `ENB/Starfield PostFX` project as relevant
prior art. Its `HANDOVER.md` records the BG/blue-fastest LUT upload axis and a
legacy R/B upload defect in that independent D3D12 renderer; more directly,
the October 2 renderer history in this project's notes records a runtime test
where mapping BC sRGB view formats through to sRGB darkened weapon colors, and
restoring UNORM views reverted that color change. Treat the latter as a useful
DX12 sampling/color-space precedent, not as an ENB source change or proof that
it causes the current screenshot's remaining dull/grainy look.

The current `MapTexFormat` intentionally maps DXGI BC1/2/3 sRGB IDs 72/75/78
to linear UNORM view IDs 71/74/77. `UploadTextureImage` uses the normalized
format both for the D3D12 resource and SRV, and the HLSL samples that SRV.
This is distinct from BA2's `DX10` archive label and its chunk decompression.
The previous build-100 sRGB-view experiment darkened weapons; build 101
restored the UNORM mapping. The Rattler's recorded DXGI 71 was already UNORM,
so this finding does not explain its separate missing decal lettering. Keep
color response, map ownership, alpha/channel behavior, and geometry/draw
submission as separate investigations.

Regression coverage now pins the sRGB-to-UNORM mappings and the resource/SRV
format handoff, so a future DX12-format change cannot be mistaken for a BA2
reader fix. See `tools/renderer_recon/test_weapon_material_diagnostics.py`.

ENB was inspected read-only; no files in that project were modified.

### Deployed state

- `71AD78B55B7B3D0F0CDDEC1A16364AAD08F32F7E40EB2CAB036B084744905FC3`
  (prior DLL preserved as `.pre-gpuban-hover-20260912-1440`).
- No wipe: geometry/cache format unchanged (ban + fallback are runtime-only;
  the clutter `.sfmodel` on disk is valid data).
- When reporting: `SFSE\Logs\SFShopExplorer.log` now has per-model lines
  (`texturing '<key>' (N ranges, M prefetched)`, `below size filter`,
  `'<key>' banned from GPU path`) — quote those plus the orbit readout.

### Next run protocol

Ax row first (was frozen `yaw +32`): press-drag from the row into the
preview should now rotate (hover gate). Then clutter/books rows (were empty
or beige): tiny items render small instead of vanishing. Then anything
post-14:13-gun: textured GPU again, since one bad model can no longer ban
the session. Lighting tuning (dark albedos, flat CPU shading) comes after
these readouts confirm all-GPU.

## 32. 2026-09-12 — spring-back root-caused: request compared against the resolved key (build `2A057365…`)

### Symptom

Guns drag-move then ping back to start; zoom too. Guns never leave the
default `yaw +32 pitch -22` while clothes rotate freely.

### Root cause (mine)

`RequestPreview` compared the incoming model-path key against `g_pendingKey`
— but `ServicePending` overwrites `g_pendingKey` with the RESOLVED key, which
differs for alias/EDID-fallback items (most guns: `BGSModelMaterialSwap`
model → generic shared NIF → `aliases.json`/EDID key). Every frame therefore
looked like a NEW request: `ResetOrbit()` + generation bump + full
re-resolve/re-rasterize, per frame, forever. Drags applied for one frame and
snapped back the next; the log spam (`request`/`resolve` pairs at 60 Hz for
hovered guns) confirms it. Clothes/books resolve directly
(resolved == request), which is why only guns sprang.

### Fix

Split what the shop asked for from what it resolved to: new `g_requestKey` /
`g_requestLabel` (written once per actual change, the stability comparison)
vs `g_pendingKey` (resolved cache key, written at publish). GPU/texture/
prefetch/ban keys all consistently use the resolved key; the snapshot label
still matches the per-frame preview label, so the current-item check is
stable and alias items resolve exactly once.

### Deployed state

- `2A057365D8C67540FD6009BC95F2EA43E16B6FA2766D4541787D6FCDC52DE2E8`
  (prior DLL preserved as `.pre-springfix-20260912-1450`).
- No wipe: cache format/contents unchanged (request plumbing is runtime-only).
- Log check: alias-resolved guns should now show ONE `request`/`resolve`
  pair per selection instead of per-frame spam.

### Next run protocol

Gun rows: drag rotates and STAYS (yaw readout holds after release), wheel/
MMB zoom holds, no per-frame log spam. If any item still springs, its log
lines will show repeated `request` pairs — quote them.

## 33. 2026-09-12 — armor addon assembly, variant-texture tiebreak (build `4614B707…`)

User report: Mantis backpack has no model, Deepseeker "suit" is gloves, some
parts missing, one helmet wears a teal variant texture, some items white.

### 1. Armor now assembles every addon NIF (fixes no-model + wrong-part + missing modules)

`FormModelPath` returned only the FIRST female addon model. Replaced with
`FormModelPaths`: every addon in `modelArray`, female slot first with male
fallback each, deduped. The request path is multi-NIF end to end
(`RequestPreview(models, …)` → `g_pendingPaths` → `ResolveModel` →
`LiveModel::BuildModels`, which builds each NIF + its outfit siblings
exactly once via a normalized-key used-set). Cache still filed under the
first key; snapshot/tag show merged counts. Expected: Mantis resolves via a
non-first/male slot, Deepseeker shows body + gloves together, partial
outfits gain their missing modules.

### 2. Variant-texture tiebreak (fixes teal helmet)

Log proof: `Outfit_WeldingHelmet.mat` picked
`outfit_weldinghelmet_01_teal_color.dds` over the base. The old tiebreak
compared base length against the FULL KEY length (meaningless units), so
archive order picked the variant. Ties now prefer the SHORTER base (fewer
variant tokens); exact matches still dominate by length bonus, and
explicitly-variant materials still hit their variant by exact/containment.
No `.mat` parsing: `.mat` files exist in no BA2 (only `materialsbeta.cdb`
holds the mapping — a later sub-project if fuzz ever misses again).

### Still open: white items

No white case in the supplied screenshots; likely untextured-bright or an
empty tiny render. Next report should name one item (EDID) + whether its
viewport tag shows meshes — the `texturing (N ranges, M prefetched)` /
`below size filter` / ban lines in `SFSE\Logs\SFShopExplorer.log` will say
which of the three it is.

### Deployed state

- `4614B7074185357E0B027E00C6F3D8BDBE167FCBAE3DB35ECED1ECD06A683FE3`
  (prior DLL preserved as `.pre-arma-multi-20260912-1515`).
- **Wiped live-built `.sfmodel` in MO2 overwrite again** — merged addon
  contents replace single-part files.
- INI unchanged.

### Next run protocol

Mantis backpack (was no-model), Deepseeker suit (was gloves-only), welding
helmet (was teal): full assemblies with base colors. Mesh-count tags grow
accordingly (body + addons). One named white-item example still wanted.

## 34. 2026-09-12 — all types covered, mod tolerance proven (build `BA8B73CA…`)

User: apply it to every item type; mods must work live or it can't ship.

### 1. Model resolution now covers every browsable type

`FormModelPaths` gains `kAMMO` (box + shell casing assembled; caseless
rounds carry no casing path and render the box alone), `kALCH` (aid) and
`kKEYM` (keys, via the `TESObjectMISC` carrier — `TESKey` derives it). All
headers were already included. Anything WITH a model renders; genuine
model-less records keep the honest placeholder. Also hardened `pushUnique`
against null/empty paths for all callers at once.

### 2. Mods work live — proven, plus one real tolerance fix

Proof with the 4-Eyes Star Wars outfit (loose mod NIF + named-factory
`.mesh`, never in a BA2): NIF walk syncs (Bethesda stamp), custom mesh
parses to 28,588 verts within 2e-5 of MeshConverter with all 81,156 indices
exact. A second mod NIF (lightsaber, third-party author stamp) syncs at
skip=34 through the wide window — a layout vanilla never uses. (Amusingly,
MeshConverter itself access-violates on that lightsaber NIF; ours doesn't.)
At runtime MO2's VFS presents loose mod files under `Data/`, which is
exactly what the live resolver scans — no prebuild, no INI needed.

Real gap found by the proof: the mod's NIF LOD entry says 30186i/18744v
while its loose `.mesh` holds 81156i/28588v (stale entry). The old strict
equality would brick it in-game, so mismatches now WARN and trust the file
— every index is still range-checked and the border sanity-checked, so a
genuinely wrong layout still fails closed. Vanilla behavior unchanged
(counts always matched there).

### Deployed state

- `BA8B73CA212C7EADCF64554A343C63EE809FFBFAAC5C7FEA9D6EA38A88A28F06`
  (prior DLL preserved as `.pre-alltypes-mods-20260912-1530`).
- No wipe: no cache format/content change for existing keys (new type paths
  only add previously-unbuilt keys).
- INI unchanged: mod assets resolve via the existing `Data/` + `Data/*.ba2`
  scan; `extradirs`/`extraba2` remain for non-VFS layouts.

### Next run protocol

Ammo/aid/keys rows with models now render (box + casing for ammo). Browse a
mod-added item (e.g. 4-Eyes outfit, lightsaber): it should live-build on
first view (slower first paint + `differs from NIF; trusting file` line in
`SFSE\Logs\SFShopExplorer.log`) then render/rotate/texture like vanilla.
Nexus story: ship the DLL + INI + prebuilt `models/` cache; mod items need
no prebuild — first view builds, later views hit disk.

## 35. 2026-09-12 — texture mismatch class-kill (build `9DFDA9DC…`)

User: ammo crate wears orange; broad "no image" worries; mod-file question.

### Wrong colors — cross-family containment (log-proven)

`prefetched` lines showed the disease: `AmmoBoxLarge_DarkGreen01.mat` wore
`bigbang laser` / `razorback ammo` / even `houdini clouds white`;
`PaintedMetalshine01_Orange` wore a **palm-tree** texture; the ammo counter
wore gunmetal. Mechanism: when nothing in the material's own family matched
by containment, ANY cross-family containment won instead of failing closed.
Fix: containment now requires family scope — only exact-base matches may
cross families (shared generics like glass). Plus a family-dirname fallback
candidate, so `DarkGreen01` (shares no words with `ammobox01_color`)
resolves to the family base. Offline proof on 7 cases: crate→base,
helmet→`outfit_weldinghelmet_01` (not teal), outfits unchanged exact,
counter/glass fail honest-flat. Textures aren't cached, so no wipe.

### "No image" census (log: 616 ready vs 10 failed keys)

All 10 empties are correct fail-closes, not bugs: `typea_*` / `fatman` /
`testspaceshiprocketprojectile` point at NIFs absent from every archive;
`bullpupdummyreceiver` is a geometry-less frame NIF (`notypes`, no
BSGeometry — invisible by design). Model-less forms (caseless ammo without a
box path, Heal Gel, some keys) keep the honest placeholder — the engine
shows icons, not 3D, for those too. Ingredients: zero INGR records exist in
the game database (long-standing note in the tab).

### Mod-file question (answered)

Mod users need NO extra render files. The `.sfmodel` cache is speed only:
first view of any item (vanilla or mod) live-builds from the game's own
bytes and writes the cache for later views. Mod NIFs/BA2s resolve through
the same `Data/` + `Data/*.ba2` scan (MO2's VFS presents them there), and
stale mod LOD counts are tolerated (section 34). Nexus ships DLL + INI +
prebuilt vanilla cache; everything else self-builds.

### Deployed state

- `9DFDA9DCFB5EAEDE87B48ABC7FB290F28AAA9A3AF01BDED3A799DA8EAD8C3017`
  (prior DLL preserved as `.pre-texmatch-20260912-1615`).
- No wipe. INI unchanged.

### Next run protocol

Ammo crate (was orange): family green/grey base. Welding helmet stays base
(not teal). Former cross-family absurdities (palm, clouds, soup) render
flat-shaded instead. Anything still wrong-colored: quote its `prefetched`
log line.

## 36. 2026-09-12 — screenshot batch: book / succulent / rattler texture class-kill (build `74B4CBD0…`)

User supplied the 4-item screenshot batch (in-game target vs offline grey):
Rattler (00040826, BullpupDummyReceiver), Succulent (0000DEC2, SM01),
Ramen Multipack (001EE440, ComboPack), Red Rover Come Over (0007FA37,
BookLarge02). Log + BA2 ground truth root-caused every one in
`LiveModel::FindDiffuse` (geometry was already correct for all four).

### 1. Rattler wore bigbang + industrial textures (prefix-fragment exact)
`BaseVariants` stripped `rattler_` -> `receiver`/`decals` fragments, and the
old exact-match rule let fragments match across families. Fix: prefix-derived
candidates are tracked separately and can NEVER match cross-family; full-base
cross-family exact is allowlisted to shared generics (`glass` only).
Proven on the full 34,819-entry color index: Receiver -> heater_receiver,
Slideandlower -> heater_slideandlower, Decals -> heater_decals (was bigbang /
industrial_tablekit01).

### 2. Succulent: variant + token fix (Plant01 -> sm01, Soil -> soil)
Two stacked bugs: (a) `NormalizeBase` only stripped `_01` with underscore, so
`plant01`/`soil01` kept their digits and never token-matched (`soil01` !=
`soil`); `StemTokens` + matToken now split trailing digits. (b) No
token-overlap path existed, so Soil01 fell through to the dir-fallback plant
map. Added family-scoped token-overlap scoring + 600 distinctive-token bonus
+ 400 variant-number bonus. Proven: Plant01 -> succulents_sm01_plants (was
sm02), Soil01 -> succulents_soil (was sm02), Plastic01 -> flat (was green
plant map; grey pot is correct — no plastic color exists).

### 3. BookLarge02: same-folder generic fallback (was 0 prefetched)
GenBookB01 has no B01 _color map in any BA2 (only genbooka01 exists), so both
cover + pages failed closed and the book rendered grey. New last-resort pass:
shortest family-scoped map sharing a 3-letter prefix (genbooka01). Cover may
still miss its green tint if the tint lives in materialsbeta.cdb/substance
rather than the diffuse — pages + cover art will at least be textured now.

### 4. Collateral hardening
Fail-closed for untextured kinds (plastic/glass/decal/overlay/blank/letter/
stripe/lines): the match must carry the material's own token, else flat.
Ramen glass -> flat (was wrong glassvisor); book letter/line overlays ->
flat (transparent decals, correct). Ramen body + decal unchanged and correct.

### Deployed state
- `74B4CBD08E88233DC5A08972371A0D65BCBC8836186590F3A842870FB190D308`
  (prior DLL preserved as `.pre-texmatch2-20260912-1645`).
- No wipe: textures resolve at runtime, disk caches hold geometry only.
- INI unchanged. Offline replica of the new matcher passes all 10 material
  cases (test_match.py) against the full BA2 color index.

### Next run protocol
Rattler (was white): wood grip + metal slide with decals. Succulent (was
pale): green sm01 plant, brown soil, grey pot. Red Rover (was grey): beige
GenBookA cover + aged pages — report if the cover still isn't green (then the
tint is a CDB/substance job). Ramen Multipack (was blocky): noodle + decal
maps. Anything still wrong: quote its `prefetched` log line.

## 37. 2026-09-12 — weapon side profile + soil + decal-sheet skip (build `1E4CD802…`)

User verdict on the section-36 build: Rattler textured ✓, Succulent textured
but **no soil**, plus a side-by-side: our Rattler stands barrel-up while the
inventory scene lays it barrel-left.

### 1. Weapon orientation (the side-by-side)
NIF weapons are authored barrel-up +Y, grip down -Z; our camera treated +Y as
up, so pistols stood vertical (rifles only escaped via the 1.8x lay-flat
heuristic, which the Rattler's deep grip defeats: extents x=0.04 y=0.21
z=0.17). `RequestPreview` gains a weapon flag (shop passes
`selectedType == kWeapon`); weapon models take the cyclic
`(x,y,z)->(y,z,x)` — barrel +Y onto +X, grip -Z onto -Y, width onto depth
(proper rotation, det +1, verified on rattler verts: x=0.21 y=0.17 z=0.04).
New weapon requests also start at yaw +180 so first paint points barrel-left
like the inventory scene. Runtime-only: no cache wipe, all paths (GPU +
CPU fallback) take the flag from request → publish.

### 2. Soil (18-tri disc vs the tiny filter)
The soil mesh (28v/18tris) died in the `<24 tris` attachment-point filter
while pot (268) + plant (2050) survived — log `2 ranges` told it all. New
`MeshSpread` exception: a tiny mesh survives when its bbox diagonal spans
>30% of the model radius (soil 0.283 vs 0.06 threshold). True attachment
points stay compact and are still dropped. Both GPU + CPU paths.

### 3. Decal sheets hid the book cover
`booklarge02.sfmodel` geometry showed why the fallback didn't visibly land:
the Letters/Lines/Blank overlay sheets span the same volume as the cover and
— rendered opaque flat-grey — cover the freshly textured GenBookA beneath.
New draw-time rule (both paths): an untextured decal/overlay/blank sheet is
skipped so the surface below shows; textured sheets (weapon heater_decals)
are kept. Glass/plastic are structural and untouched.

### Deployed state
- `1E4CD80212EB874077312605C9D23F260C5398DBAB75AFBD0416EB2C828F1F88`
  (prior DLL preserved as `.pre-orient-soil-20260912-1730`).
- No wipe (runtime-only + geometry caches unchanged). INI unchanged.

### Next run protocol
Rattler: horizontal, barrel left, grip down — compare against the reference
screenshot (if grip points up, the sign flips next build). Succulent: brown
soil ring between pot and plant. Red Rover: beige cover visible (letters
sheet hidden); green tint still CDB-side if absent. Ship-embedded weapons
(Vanguard Obliterator: no handheld model exists) still show the honest
placeholder — catalog-wide no-model census is the next work item.

## 38. 2026-09-12 — grip flip, key stand-in, no-model census (build `830546E8…`)

### 1. Rattler upside-down: grip is +Z, not -Z
Side-by-side proved the section-37 sign wrong: grip rendered up, so the grip
runs +Z in NIF space. Orient is now `(x,y,z)->(y,-z,-x)` (still det +1):
barrel +Y onto +X, grip +Z onto -Y, width onto depth. Same silhouette
(0.21 x 0.17), grip down. Yaw+180 start kept (barrel left).

### 2. No-model census (today's log, empty-path requests)
Dominant groups are genuinely model-less records, verified by EDID/keywords:
ship-embedded weapons (Supernova II, CE-19, Pulse Laser — Spaceship*Weapon,
no handheld mesh exists), creature attacks (Unarmed*, claws, mouth lasers),
creature ammo/grenades. These keep the honest placeholder — rendering a
wrong gun would be actively misleading. Keys are the exception below.

### 3. Keys: generic keycard stand-in
KEYM door keys/keycards carry no model string (TESKey derives TESObjectMISC,
so the carrier read works — the field is just empty), and no generic metal
key NIF exists anywhere in the BA2s (82 key*.nif hits: keypads, markers,
biometric setdressing, whiskey-turkey noise). Only keycard NIFs exist, so
empty KEYM forms now resolve `SetDressing\Keycards\KeycardGen01.nif`; the
detail pane's Model: line names the stand-in. Placeholder text clarified
accordingly ("this form carries no 3D model").

### 4. Olive Seed / Groat Meat verdicts (likely faithful, not bugs)
Olive Seed wears its EXACT family diffuse
(city_na_z_olivebranch_seed_color) with sane 0..1 UVs and no NaN — the
print-like look is the texture itself (printed packet), not a mismatch.
Groat-meat pack materials (MeatHam, MeatPlasticBag, chunks labels/packages,
tape) are all legit packaging maps. Counterexamples wanted: 2–3 named Aid /
Book / Misc rows whose preview visibly contradicts the inventory look, with
screenshots — the matcher can only fix proven mismatches.

### Deployed state
- `830546E85600703EB55754DFA9E041821D478A1B6AECBC5ABA868143426850D8`
  (prior DLL preserved as `.pre-gripflip-keys-20260912-1820`).
- No wipe (runtime-only; weapon orient re-renders from existing caches).

### Next run protocol
Rattler: grip DOWN this time (barrel left). Keys tab: every key shows the
generic keycard. Ship/creature rows: clear "carries no 3D model" message.
Named Aid/Book/Misc counterexamples still wanted for the texture pass.

## 39. 2026-09-12 — grip sign reverted, sibling dirs, tint forensics (build `4BA2A0C9…`)

### 1. Grip sign: (y,z,x) restored, (y,-z,-x) was wrong
The flip rendered grip-up, proving the grip runs -Z: the original cyclic
mapping stands. Lesson: one sign per build, screenshot decides.

### 2. Sibling-dir scope + token-prefix overlap (chimera + playing cards)
Two families split textures into neighbour dirs the strict scope rejected:
`menuitem_chimera/*` next to `menuitem/`, `exoticplayingcards/` next to
`exoticplayingcard/`. New extended scope accepts a family sharing the parent
dir whose dirname extends the material family's own dirname
(`menuitem_chimera`, `exoticplayingcards`); containment/token rules still
gate, so `bigbang` stays excluded from `rattler`. Plus token-prefix overlap
at half weight (`chimeracap` ~ `chimera`, `exoticplayingcardbox01` ~
`exoticplayingcard`). Dir-dirname candidates stay STRICT (books must not
hijack `booksailing` instead of the genbooka01 fallback). Fail-closed for
plastic/glass/decal now fires only on dir-loop wins or sibling+short-token
wins — strict hits (heater_decals) and sibling+long-token hits (card
plastic -> symbols sheet) survive. 19-case replica all green.

Proven resolutions: ChimeraCap/Water -> chimera_water, Glass -> flat;
card symbols/trim/plastic -> symbols_decal (bg ~219 white-pink);
poster back stays flat (no bridge to the 1024x1024 poster dirs yet).

### 3. Tint forensics: yellow ammo + green book are CDB-side
Decoded top-mip averages prove the diffuses carry no yellow/green:
ammobox01 (97,82,57 olive), ammobox01decals (55,42,10 dark),
ammoboxrevised (58,56,46 grey), genbooka01 (247,247,247 white). The
reference yellow crate and green cover come from material tints in
materialsbeta.cdb (mat_recon's DIFF-context format still undecoded) or
substance — the next sub-project. Diffuse mapping for both is now correct;
only the tint is missing.

### Deployed state
- `4BA2A0C92A7FCED8D3E4320254C2E41C495F206E0E45B0CBD14BB4AAEE576212`
  (prior DLL preserved as `.pre-sibling-prefix-20260912-1900`).
- No wipe (runtime-only).

### Next run protocol
Rattler grip down (barrel left) — final answer on orientation. Chimera can:
tan liquid + cap art, glassy top flat. Playing card: white faces with red
symbols (back may stay grey). Ammo crate + book: textured but tintless
(khaki crate, white book) until the CDB tint pass. Olive seed + groat pack
stand as faithful (exact diffuses, sane UVs) unless the in-game look
contradicts.

## 40. 2026-09-12 — dummy receivers + 339 missing weapon models (build `FFE5C54D…`)

User: Cutter + many weapons show nothing (BullpupDummyReceiver.nif?),
Rattler still wrong — "assuming guessing with no real effort". Answering
with ground truth, not guesses.

### 1. Rattler orientation: geometry proof, no more flips
`rattler.nif` bounds (MeshConverter ground truth): slide-top decals at +Z,
trigger just below center, grip mass toward -Z; trigger sits 39% from the
rear, so the muzzle is +Y. Mapping `(y,z,x)` + yaw-180 (barrel left, grip
down) is therefore CORRECT from the data. The grip-up screenshot came from
the one-off `(y,-z,-x)` experiment, already reverted; the 18:29–18:31
screenshots predate the 18:55 revert deploy. One sign per build stands.

### 2. BullpupDummyReceiver is a marker, not a model
Verified: it exists in the BA2s but parses as `notypes` (no NiNode, no
BSGeometry) — pure placement marker. Distinct weapons (Beowulf, Cutter,
Rattler, Eon...) all reading it means the base mesh comes from the weapon's
mod assembly, so `FormModelPaths` kWEAP now treats any `*dummy*` path as
empty and falls through to EDID/substring resolution.

### 3. BA2 census: 775 weapon NIFs, 298 built → 339 new models shipped
Offline builder only scanned 3 Meshes archives. Extended to SFBGS003/SFBGS050/
ShatteredSpace mains and built every missing base: magpulse/shear/shot/sniper,
microgun (+48-mesh receiver), razorback (47), tombstone(+rifle), regulator,
orionrifle, shotty, sidestar, urbaneagle, solstice, novablast, novalight,
maelstrom, magstorm, pacifier, pumpshotgun parts, russian rifles, blades,
teslamine, vortexgrenade, breechblock... 637 models deployed (339 new).
No-base families (dogfight: parts only, no receiver) stay placeholder.

### 4. Runtime resolution: aliases + substring fallback
- `aliases.json`: grendel -> bullpup_receiver (25k-vert rifle body, verified),
  eon -> stinger (12-mesh pistol body, AnimStinger keyword on Eon forms).
- Substring fallback over the cache dir (task thread, fs only): Cutter ->
  lasercutter, Lawgiver -> lawgiver1, Orion -> orionrifle, Mag* -> mag bases,
  Tombstone/Regulator/Razorback/Microgun -> bases. Attachment-only keys
  (scopes/barrels/mags...) never qualify, so Equinox (scopes only) stays
  honest-placeholder instead of rendering a bare sight.

### Deployed state
- `FFE5C54D6FB5EB5C7EB05BD9860FAF7C2B164451E5FCEE79EC3FD4C7A5B065B0`
  (prior DLL preserved as `.pre-dummyalias-20260912-2000`).
- 339 new `.sfmodel` + updated aliases/index.json in the mod dir.
- Builder `MESH_ARCHIVES` extended to the 3 DLC mains (kept).

### Next run protocol
RESTART the game first (the last screenshots predated the revert deploy).
Cutter/Mag/Grendel/Eon/Tombstone/Regulator/Razorback/Microgun/Shotty rows:
full renders. Rattler: grip-down barrel-left — geometry-proven; if it
still reads wrong, the question is viewing side (yaw 0 vs 180), not up/down.

## 42. 2026-09-12 — catalog UI pack: all-plugins, tighten, sort (build `10C5FAAC…`)

User asks: list the whole catalog at once; tighten ship parts / hair /
naked / body etc. by default; verify the right inventory items show; sort
by name / type / value / wt / formid.

### Texture-mixing question (answered before the features)
"Each texture has material same name" is the prebuilt cache's known scar:
`build_model_cache.py` writes the material path into BOTH the name and
matPath fields, so all 637 prebuilt models match on material evidence only
(node names are lost). Nothing crosses ITEMS (caches, ranges, SRV slots and
prefetches are all keyed per model) — but within an item, meshes sharing a
material can't use node evidence. Mitigations already live in the matcher
(token/variant/family scoping); recovering node names needs a builder-side
pass (MeshConverter tree names) and is queued behind the tint work.

### Shipped
- **★ All Plugins** pseudo-row (default on open): browses every loaded
  source at once; pick a single source to narrow. Counts + log name it.
- **Tighten, both default-on**: "Hide test/debug" (EDID NOCLUTTER / DUMMY /
  TEMPLATE / DEBUG, name NOT PLAYABLE) and "Hide ship/hair/etc" (keyword
  spaceship / hair / beard / naked / nude). "Body" deliberately excluded —
  it collides with legit armor keywords. Uncheck to reveal; log reports
  hidden test/tighten counts per rebuild. Test Room Keycard survives (no
  blanket TEST rule).
- **Sortable columns**: clickable NAME / TYPE / VALUE / WT / FORMID headers
  (▲▼, click flips). Name/type alphabetical, rest numeric, FormID tiebreak.
  Selection is by form index, so sorting never loses the selection.
- Correctness note: ship-embedded guns, creature attacks/ammo and model-less
  aid (Onion, Heal Gel...) keep the honest placeholder — the engine shows
  icons, not 3D, for those too.

### Deployed state
- `10C5FAAC398559031A9D22F394A6439D7E6F85D8B0F1164E90F7940746A5F8D4`
  (prior DLL preserved as `.pre-uipack-20260912-2100`).
- No wipe (UI-only).

## 43. 2026-09-12 — auto-render answer + muzzle fix + all-types (build `088AD06A…`)

User: "isn't this meant to auto render, hence the dll — why one by one?"
Fair. The record: the pipeline IS automatic — browse any row and the model
self-builds from game bytes (no per-item assets ship; the 339-model cache is
speed only). The "one by one" was test cases for systematic rules, each
covering hundreds: sibling dirs (all menuitem drinks + cards), substring
(dozens of guns), prefix fragments, decal skip. Remaining gaps are data, not
per-item labor: CDB tints, OMOD assembly.

### 1. Muzzle corrected from node evidence (no more flips)
`Decal_grip` sits at y~+0.11 (rear) with the trigger 61% from the -Y end, so
the muzzle is -Y and the rear/grip +Y — the old trigger-% read had the ends
backwards. Mapping is now `(x,y,z)->(-y,z,-x)` (det +1, verified): muzzle
onto +X, rear onto -X, top stays +Y; yaw-180 start reads muzzle left, grip
down. Geometry-anchored this time (node names + bounds), not screenshot-read.

### 2. All-Types view (what "all" meant)
New `BrowseType::kAllTypes` (toolbar combo + default on open): every
browsable record type merged. TYPE column + detail pane + sort now use the
per-row form type; weapon side-profile triggers per form, not per view.

### 3. "50 images then break": no crash on record
No dumps anywhere; shop opened 21:40, browsing cleanly at 21:48 through
hundreds of live builds. If previews stall after ~50 rows, say exactly what
stalls (frozen image? "building..."? rows empty?) and it gets traced — the
log already timestamps every resolve/texture per item.

### Deployed state
- `088AD06A2762D22FE4AD393D4D0008AB08B044972E3FD63B928BF152685CCCB5`
  (prior DLL preserved as `.pre-alltypes-20260912-2130`).
- No wipe (orientation is runtime-only).

## 41. 2026-09-12 — render-source line (build `BDDA9D6C…`)

User theory: the `BullpupDummyReceiver.nif` in the Model: line is a leftover
from an old render attempt. Checked both halves:
- Stale cache: no `bullpupdummyreceiver.sfmodel` exists anywhere (mod dir +
  overwrite, 1096 live-built models) — fail-closed parsing never bakes the
  marker, so there is nothing stale to poison resolution.
- Stale UI: disproven — the log prints the dummy path live per request
  (`request 'Rattler' <- '...BullpupDummyReceiver.nif'`), and the independent
  DevilzDump reader also finds no weapon model strings. The base record
  genuinely carries the marker; the engine assembles from mods.
- Real confusion: the Model: line names the FORM path, never the RENDER
  source. The detail pane now prints `Render: <key>` (gold) whenever the
  resolved snapshot key differs — Rattler shows `Render: rattler`, Cutter
  `Render: lasercutter`, keys `Render: keycardgen01`.

### Deployed state
- `BDDA9D6C20502BAD200EDBB7F695C606626B36A5BAFDCCCBE8D42D5E09C5AF2F`
  (prior DLL preserved as `.pre-rendersource-20260912-2030`).
- No wipe (UI-only change).

## 44. 2026-09-12 — session dating wins; token fallback (build `62A0B67B…`)

Two corrections, both mine:
- The stale-session theory was wrong: successful DLL copies prove the game
  was closed for every deploy. Withdrawn.
- Dating evidence from the log instead: the Rattler shot still showing the
  dummy Model: line predates the FFE5 dummy-skip (current code shows no
  Model: line + gold `Render: rattler`, and 22:25 logs
  `resolve '' -> ready ('rattler')`). Mixed-session screenshots are now
  dated by the startup tag (`build 44-tokens-rendersrc`), bumped per build.

### Shipped
- EDID-token fallback (task thread, fs only): `Companion_Solstice` ->
  `solstice` exact, `NOCLUTTER_ResearchUI_Grendel_*` -> `grendel` -> alias
  `bullpup_receiver`. Short/generic tokens fall through harmlessly.
- `Render:` provenance line moved out of the Model: block so dummy-model
  weapons (no Model: line at all) still show their render source.

### Deployed state
- `62A0B67BDE637A5E0B0EE5EE4901BC1298151E01AFED1F2016708E2E845B4DAA`
  (token + rendersrc fixes + tag).
- Game was closed; copy succeeded. No wipe.

## 45. 2026-09-12 — orientation settled by elimination (build `89BAC9D4…`)
"Opposite side" (muzzle-right) plus the full evidence table:
- Grip-up renders happen ONLY under mappings with newY=-oldZ
  (`(y,-z,-x)`, and the vertical era). Every grip-up screenshot traces to
  those builds.
- `(y,z,x)` and `(-y,z,-x)` both render wood-down/slide-up/muzzle-left and
  differ only in which flank faces out (depth mirror). No grip-up screenshot
  can come from either — and none is dated to a session running them.
- Baked mesh bounds (slide z +0.05..+0.08, wood mass z -0.09..+0.03,
  muzzle -Y, rear +Y) uniquely select `(-y,z,-x)` + yaw-180: muzzle left,
  slide up, wood down, rear right. det +1, proper — text and culling safe.
- The flank question (port side vs slab side) is genuinely 50/50 from the
  desk; if the silhouette is right but mirrored detail-for-detail, say
  "mirrored" (not "upside down") and the Z sign flips once, finally.

### Deployed state
- `89BAC9D4361271029DA651AA2C1BE4087991407CA0E903F6B3EF497852D73D64`
  (tag `45-orient-final`).
- Game was closed; copy succeeded. No wipe (runtime-only).

## 46. 2026-09-12 — user-calibrated weapon camera (build 19DCF39A)

User dialed the orbit to yaw +40 / pitch +20 and called it inventory
positioning, so that IS the weapon camera: [weaponview] wyaw=40 wpitch=20
(defaults), applied to weapon-oriented models in GPU + CPU paths, orbit
readout and drag clamp. The +180 start hack is removed. Tune in INI, no
rebuild ever needed for the camera again.

### Deployed state
- `19DCF39AC4CDFF10E6B785B34D4B345A24CD0D8A46F8D3B6B3195E4A92C44BE3`
  (tag `46-weaponview`).
- Game was closed; copy succeeded. No wipe.

## 47. 2026-09-12 — grip mirrored, not misplaced (built, awaiting deploy)

The +40/+20 comparison isolates the last defect exactly: muzzle left,
slide up, trigger bottom all match — only the wood sits bottom-LEFT
instead of bottom-RIGHT. So this is a left-right mirror of one axis, not
orientation: `(x,y,z)->(y,z,x)` keeps muzzle/slide/wood heights and moves
the +Y rear (and wood with it) onto +X right. det +1, proper — no
culling/text changes. The brown mass upper-left in the old shot is the
bronze receiver body (correctly placed per its bounds), not the grip.

### State
- `9F5DE4CD27B2CAF5D51D25448067B3333A24185D253D4A7AF3CE055EAE83F7C0`
  (tag `47-mirrorfix`).
- Game was closed; copy succeeded. No wipe.

## 48. 2026-09-12 — mirror was the mistake, muzzle proof (build 14D9E3B3)
47 mirrored newX chasing "bottom-right" from one read — wrong call: it put
the muzzle right ("opposite side", user). Proof the muzzle is +Y:
ProjectileNode spawns there (y=+0.17, max extent) with the front drum at
+0.13. So 48 restores the 46 mapping (-y,z,-x) the user had already
confirmed (muzzle +Y onto -X = left at yaw 40, slide +Z up, wood -Z down,
rear -Y right, det +1). Lesson: user confirmations outrank screenshot
reads.

### Deployed state
- `14D9E3B33D8169B85193E54D21FF89B5E59DC3D8B0B6131589659AFD4AEC82F5`
  (tag `48-muzzlefix`).
- Game was closed; copy succeeded. No wipe (runtime-only).

## 49. 2026-09-13 — stock-mag assembly (build 040408FA)

User called it: weapons are modular, the grip is a separate module. Bounds
proof: rattler_mag (1966v) sits y[-0.05,+0.01] z[-0.07,+0.01] — inside the
grip well; glowsight/reflex/longbarrel slot onto the slide/muzzle the same
way. Base NIFs ship mag-less (rattler/AK/lawgiver/microgun/coachman carry no
mag mesh), which is the hollow "poor AI 3D" look.

### Shipped
`AssembleDefaultMag` in the resolver: after any successful resolve, if the
base has no mag-like mesh (mag/magazine/magcover/drum/clip token) and
`<base>_mag` (else `<base>_magazine`) exists, its meshes merge in with
recomputed bounds (session-cache only, disk untouched). Bases already
carrying one (Breach/SideStar/M1919 Magazine, Beowulf MagCover) are skipped;
barrels/muzzles/sights never auto-attach (player's choice). Textures follow
automatically (prefetch iterates the merged meshes).

### Honest boundary
This renders STOCK. A customized gun in the player's inventory wears their
mods; matching an exact player build needs live instance data (the old
uHandleID native-scene problem), not the offline renderer.

### Deployed state
- `040408FA74E6253FC4BC1D024C6FD53D831B7105E20A6111B7A2B6C72045AC9E`
  (tag `49-stockmag`).
- Game was closed; copy succeeded. No wipe.

## 51. 2026-09-13 — archive walk desync + mag runaway + BC3 (build `F6844480…`)

User's cache-hidden experiment paid off double: live geometry verified,
and two real bugs surfaced from the log.

### 1. DX10 walk desync (the missing-textures root cause)
`OpenBa2` validated entries BEFORE consuming chunk bytes: any entry with
bad dims (or nchunks 0/>16) was skipped without consuming, desyncing every
later entry in that archive — dims/dxgi/chunk offsets parsed as garbage.
Symptom in log: `texture format 84 unsupported` for textures whose headers
really say 72 (chimera_water, dataslate screen, empty tray), plus silent
drops. Fix: consume-then-validate (chunks always read; validation after),
archives fail loud past 64 chunks. The "84" was never a real format — no
BC6H work needed.

### 2. Mag assembly runaway (log-proven 12 -> 14 -> 16 -> 18 -> 20)
Mag NIFs share the gun's main materials (rattler_mag wears Slideandlower),
so the material sniff never detected a prior merge. Guard is now a
per-session assembled-keys set. Same fix class as the request-key split.

### 3. BA2 code 77 = BC3_UNORM (safe map to the 74 path: same 16B blocks,
alpha ignored, color identical).

### Deployed state
- `F6844480FC48AD17555CBAD3D5125A0A64104F7FC8C8A3900E508868FBC90DDD`
  (tag `51-desync`).
- Game was closed (copy succeeded twice). No wipe.

## 52. 2026-09-13 — 8K cap + matcher batch (build `FCEE446E…`)

Log census of partial renders drove this batch (25-case replica, all green,
zero regressions on the 19 old cases).

### 1. One 8K texture hid ~5,500 (the big one)
`Starfield - Textures05.ba2` failed wholesale over entry 1512
(`textures/debug/debugmips.dds`, 8192x8192) tripping the 4K cap — and every
reported-broken texture sits sorted AFTER it: dataslateback (5176),
chimera_water (5495), ramen (5767), olive seed (5973). Cap is now 16384.
This alone restores dataslate/chimera/ramen/olive/ammo-decals.

### 2. Purged 27 mat-less prebuilt models (both dirs)
MeshConverter returns empty mat paths for whole NIFs (tombstone 18/18,
inflictorrifle, flamer parts, grenades...). Live parser reads NIF shader
strings directly, so purge forces the working path. Overwrite had none
(live writes good mats — nice validation).

### 3. Matcher: typo bridge, short tokens, underscore-blind siblings
- `comatknife` <-> `combatknife` canonicalized (Bethesda typo dir) — knife
  + decals resolve.
- Containment/token floor 4 -> 3 (leg/arm/eye tokens).
- Sibling-dirname compare ignores underscores
  (modelscrimsonfleet == models_crimsonfleet) — crimsonfleet LEG/BODY/Turret
  resolve. `>=` (equal-after-strip counts).

### Still tint-side (verified by scan, not guessable)
Keycard yellow (no keycard DDS in any archive), book green (genbooka01
averages white), ammo yellow (olive/grey/dark maps only). CDB pass next.

### Deployed state
- `FCEE446ECC849961ACC6F8B457DB0735227AF10063A3BC329D4A472F055DF81E`
  (tag `52-texturebatch`).
- Game was closed; copy succeeded. No wipe.

## 53. 2026-09-13 — decal alpha-test (build `A471086D…`)

User: wrong textures because TOO MANY applied (no screenshots possible —
image limit hit). Mechanism confirmed in code: transparent decal/label
sheets rendered OPAQUE, paving the surface beneath. New alpha-tested PSO
for decal-kind ranges (`clip(a-0.5)`; opaque maps read alpha 1 = no-op by
construction) + unconditional sheet skip on the texture-less CPU path.
Glass stays opaque deliberately.

## 54. 2026-09-13 — kind synonyms + score audit (build `33687490…`)

Partial-texture census drove it: MagPulse screens/counters wore the GRIP
map (family fallback over-generalizes). `KindNorm` functional synonyms
(screen/display/counter/monitor/readout/indicator, scope/sight/optic,
grip/handle, mag/clip/drum, barrel/muzzle, light/lamp/glow, button/knob,
stock) with compound-suffix matching (powerindicator -> screen). 28-case
replica all green, zero regressions. `FindDiffuse` reports match scores;
prefetch log prints them (cached/fallback tagged) for the next audit.

### Deployed state
- `336874902802EE4FA35C70539EC3638DC2BBE42C93976A9168FBEE1487EBC249`
  (tag `54-kinds`).
- Game was closed; copy succeeded. No wipe.

## 55. 2026-09-13 — matcher batch: dir-last, slash-strip, kind-keep (build `C2C6F072…`)

Partial-render census + 28-case battery (all green, zero regressions).

### 1. Generic-dir exact outbid specific matches
`Eon_Decals` wore the family generic instead of the decals map (dir-exact
scores ~2000 vs token ~1600). Dir-dirname candidates now run ONLY when
nothing specific matched — true last resort. Same fix lands Coachman (now
hits its decals map).

### 2. Leading-slash families were unmatchable
Some NIFs store `\materials\...` (dogfight decals): family computed empty,
total miss. Stripped.

### 3. Fail-closed rewritten as keep-rules
Fail iff dir/fallback win, else require: strict + (own token in pick OR
art-kind pick), sibling + (long token OR art-kind both sides). Plastic pot
stays flat, card plastic keeps the symbols sheet, weapon decals keep theirs,
glass stays flat.

### 4. Kind synonyms extended (suffix compounds)
`powerindicator` -> screen (compound suffix rule), `indicator` group added;
token bonus normalized on both sides. MagPulse screens/counters/indicators
all hit the display map now; labels fail flat (no label map exists).

### Still open (evidence-graded, no guessing)
- Tombstone-class: 27 mat-less prebuilt files purged (live path takes over).
- Tint-side: keycard/book/ammo (verified absent diffuses).
- v2 texture archives (DLC/mod): entry layout undecoded, parked.

## 50. 2026-09-13 — cache-hidden live-build experiment (no build)

User: hide the cache to force live calls, see what happens. Parked (not
deleted): mod-dir `models/` -> `models_prebuilt_backup/` (637 files),
overwrite live models -> `models_live_backup/` (1096 files). Fresh empty
`models/` holds only `aliases.json`. Every browse now live-builds from BA2s
via the in-game parser (first paint slower), which cross-checks the
MeshConverter pipeline + restores node-name evidence for matching. Repo
cache untouched. Restore = move both backups back (say the word).

### Verdict (same day): restored, cache is the throughput layer
Live parser WORKED (tanto/cutlass/mines/grenades all built, no failure
flood) — but throughput collapsed: request 12:22:45 resolved 12:25:13
(2.5-min task-thread lag behind cold BA2 scans + full NIF/.mesh extraction
+ texture decode), so texture prefetch starved and guns sat grey. The
prebuilt cache is load-bearing for browsing, not polish. Both backups
moved back whole (637 + 1320). Lesson: cache-hidden is a diagnostic, never
a mode.

## 56. 2026-09-22 � Clone3D mesh extraction (build pending deploy)

### Root cause (confirmed)
`ProbeGeometryNode` only scanned past +0x130 for material/texture **string
paths** � never located or read vertex/UV/index buffers. Result: every form
extracted `pos=0 uv=0 idx=0`, `tri=0`, while still reporting `loaded=true`
(BSGeometry itself was fine � vtable 498008 confirmed).

`WalkTree` also never incremented `a_triCount`, so the summary tri total was
always 0 even if a mesh had been found.

### Changes (src/NativeModel.cpp)
1. **Forward decls** for `ResolveRTTIName` / `DemangleRTTI` / `TryParseMeshData`
   near the top (they are defined after `ProbeGeometryNode`).
2. **Hex dump of +0x130..maxOff** on every geometry probe � captures the
   BSGeometry-specific fields that were previously invisible. If parsing
   fails on the first in-game run, this dump is enough to lock offsets.
3. **Object-pointer scan**: every 8-byte slot from +0x130 is checked for a
   heap pointer whose `*ptr` looks like a vtable (MSVC COL at vtable-8).
   Each candidate gets RTTI name + 0x100-byte hex dump, then
   `TryParseMeshData`.
4. **`TryParseMeshData`** (heuristic, empirical):
   - Scans object fields +0x08..+0x120 for (u32 count, ptr) pairs
     (count 3..2M, ptr readable).
   - Classifies pointed arrays: float3 positions (sane bbox / finite),
     float2 UVs ([-5,10]), u16/u32 index runs (all < vertCount, first
     indices in range, tri count multiple of 3).
   - Picks largest verts � inds pair whose indices validate against
     vertCount; copies verts (applies world matrix at +0x80), copies
     u16-then-u32 indices, copies matching UV array if present.
   - Logs every candidate (`verts?` / `idx16?` / `idx32?` / `uv?`) so a
     wrong pair is obvious in the next log.
5. **`WalkTree`**: `a_triCount += indices.size()/3` after probe.
6. String-only mat/tex path scan **kept** unchanged.

### Build
`run_build.bat` cd path fixed (`�\SF-Shop\SF Shop Explorer`). Build OK
(24.6s, 2 pre-existing warnings C4100/C4189). DLL not yet deployed or
tested in-game.

### Next run must show
- `geo+0x130` hex lines in the log (layout evidence even if parse fails).
- `obj at +0x� RTTI=�` + `meshdata` dumps for each pointer candidate.
- `TryParseMeshData OK` + non-zero `pos/uv/idx` on the preview line, or
  `verts?/idx16?/idx32?` lines that let us pin exact offsets.

### Still open
- Exact Starfield NiTriShapeData/NiGeometryData field offsets unknown �
  first in-game log decides.
- Garbled geometry leaf name (len 15 prints `''`) and zero bound sphere �
  cosmetic, parked.

### Test 1 result (2026-09-22 15:42) � false positives, then fix

Razorback `BullpupDummyReceiver.nif`. Probe range +0x130..+0x400 OK.
Hex dump captured. One heap object at +0x1C0 but **RTTI=""** (vtable not in
.rdata � heap false positive). `TryParseMeshData` then matched garbage:
`verts range=[-0,0]`, idx16 `max=65216/55136` (pointer low-bits, not indices),
hundreds of `verts?/idx16?/uv?` lines; log cut mid-scan (never finished).

Key dump facts:
- `geo+0x130`: `60 FE BA 37 F6 7F` = **0x7ff637bafe60** (.rdata � embedded object vtable).
- `geo+0x140`: `03 00 03 00 03 00 01 00` (three 16-bit counts + 1).
- `geo+0x2F0`: second vtable `0x7ff637bafe70` (.rdata).
- Object at +0x1C0 first qword is heap, not .rdata.

Fix (build pending test 2):
1. Object candidates now require **vtable inside game .rdata** (+ COL at vtable-8).
2. Every .rdata qword in +0x130..maxOff logs RTTI name (identifies embedded types).
3. Fallback: parse embedded object at base+0x130 directly when no pointer candidate works.
4. `TryParseMeshData`: vertex **extent > 0.01** (rejects [-0,0]), index count **% 3 == 0**,
   triangle/vert ratio sanity, full index range check, candidate logs gated off.

### Test 2 result + model-path hunt (2026-09-23) — offset correct, value is dummy

**Hunt conclusion: there is no wrong form offset.** CommonLib `TESObjectWEAP`
places `BGSModelMaterialSwap` at +0xF8 and `TESModel::model` at component+0x08
= **form+0x100**. All weapons sharing entry `0x21696948d38` →
`BullpupDummyReceiver.nif` is **by design**: modular weapons carry a shared
placement-marker base NIF (no geometry — see notes §42 dummy section). Real
mesh lives on OMOD assembly paths (`FormModelPaths` / `CollectWeaponAssemblyNifs`).

OMODs (`BGSMod::Attachment::Mod`) are `TESForm`, **not** `TESBoundObject` —
cannot `Clone3D` them. No CommonLib `ModelLoader`/`NiStream::Load(path)` body
exists (RTTI `ModelLoader` 845569 / vtable 399227 only).

**Fix shipped (test 3 candidate):**
1. `ScanForModelPath` candidates reordered: **0x100 first** (WEAP model),
   then 0xF8 (MISC/AMMO), then nearby.
2. `IsDummyModelPath()` — case-insensitive `dummy` in path.
3. `NativeModel::LoadFromForm(fid, a_modelPaths)` — ShopExplorer now computes
   `FormModelPaths` **before** the native call and passes the vector.
4. When base path is dummy: `Clone3DSwapPath()` per assembly path —
   save `BSFixedString` at form+0x100, assign real path, `Clone3D`, restore
   original, extract, merge all parts into one `PreviewResult`.
5. Non-dummy base keeps direct Clone3D (no form mutation).

Build OK (12.9s, same C4100/C4189/C4244 pre-existing).
Deployed SHA256 `1C3CF0D24E83721CEBDF714733BACF9754E07F50C1824F40CC52CBFEB31A55B8`.

### Test 3 next run must show
- `baseDummy=true assemblyPaths=N` for MagShot/Rattler/Razorback.
- `swap form+0x100 -> 'Weapons\...' (saved 'BullpupDummyReceiver.nif')` lines.
- Per-part `loaded/extracted/meshes/tri` lines; leaf `pos/uv/idx > 0`.
- After all parts: form+0x100 back to dummy entry `0x21696948d38` (restore OK).

### Test 3 result (2026-09-24 13:28) — path-swap works, mesh parse still empty

Log: 5717 lines. **Path-swap pipeline verified working end-to-end:**

- `baseDummy=true assemblyPaths=N` for all modular weapons.
- `swap form+0x100 -> 'Weapons\Heater\rattler.nif' (saved 'Weapons\Bullpup\BullpupDummyReceiver.nif')` — form+0x100 mutation + restore works.
- Real NIFs load: `loaded=true extracted=true meshes=1`.
- Geometry leaf found: `geometry leaf '…' at node 1`, vtable `0x7ff60a0ea920` (BSGeometry).
- Multi-part merge works: Razorback 5/5, Regulator 4/4, MagShot 3/3, Rattler 5/5 parts extracted.

**Remaining failure — mesh-data location:**

- `TryParseMeshData: verts=0 inds=0 uvs=0`, `no mesh data found in +0x130..+0x400`, `tri=0`, `geo leaf: pos=0 uv=0 idx=0`.
- Probe range was capped at **+0x400**; `TryParseMeshData` only scanned **+0x08..+0x120** of each candidate object — both too narrow for Starfield BSGeometry.
- Dump shows real counts at **geo+0x390..+0x3F0** (e.g. `0x1BFB` = 7163 verts, `0x1A4F`, `0x1BE6`, `0xF6E`, `0xF8B`, `0xF9B`, `0x13B5`) but **no adjacent pointers** — count and buffer are non-adjacent (buffer likely at geo+0x310/0x330/0x350 heap ptrs `0x19723d9d8f8` / `0x19723dddd40`).
- +0x130 = `NiTObjectArray` vtable (children), +0x150 = FLT_MAX bounds placeholder, +0x2F0 = second `NiTArray` vtable.
- RTTI strings confirmed in rdata: `BSGeometry` @ `0x7ff60a270308`, `NiTriShape` @ `0x7ff60a273928`, `NiTriShapeData`, `NiTriShapeDynamicData`, `BSGeometrySegmentData`.
- Garbled geometry leaf name (len 15) and zero bound persist — cosmetic, parked.

**Fix shipped (test 4 candidate):**
1. Probe range extended **+0x130..+0x800** (was +0x400).
2. `TryParseMeshData` scan window now a parameter (`a_scanStart`/`a_scanEnd`, default +0x08..+0x120) — RTTI objects scanned +0x08..+0x200, embedded +0x130 scanned +0x08..+0x1C0.
3. **Field cross-pair**: collect every heap pointer and every plausible u32 count in geo+0x130..maxOff; try every count×ptr combination with the same extent/%3/index-range validators (handles non-adjacent count@+0x390 ↔ buffer@+0x310 layouts).
4. `TryParseMeshFromFields(base, 0x130, maxOff, …)` — parse the geo object's own field region as one blob (adjacent count+ptr pairs only).
5. RTTI-object dumps increased to 8 × 0x180 bytes; field scan logs every count and every heap ptr with RTTI.

Build OK (35.5s, pre-existing C4100/C4189 only).
Deployed SHA256 `2B57D851E327794A4C22B6A499DCDD9AB9CA5519A26C37C428D9F2507CA4C0F9`.

### Test 4 next run must show
- `field scan: N heap ptrs, M count candidates` + per-entry `count @+0x… =` / `ptr @+0x… -> … rtti='…'` lines.
- `verts? count@+0x… extent=…` and/or `idx16?`/`idx32?` lines pinning the exact count/ptr offsets.
- `mesh from field cross-pair: pos=… uv=… idx=…` or `mesh from geo field blob: …` or `TryParseMeshData OK` on a pointed RTTI object.
- Preview line `pos/uv/idx > 0`, `tri>0`.
- If still empty: the field-scan log alone gives the full count/ptr inventory to pick the right pair manually.

### Test 4 result — hang crash; leftover RE code was the cause

Test-4 build (`2B57D851…`) hung/crashed on load. Root causes:

1. **Sample-only `CanRead`** before materializing garbage-sized buffers (64-vert sample passed, full read AV'd).
2. **Missing `maxIdx > 0`** on index candidates (all-zero indices accepted).
3. **Unbounded `strlen`** on non-terminated heap strings during field scan.
4. **Greedy count collection** (any u32 in 3..500k).
5. **Massive diagnostic logging** + accumulated RE probes from earlier sessions (see cleanup below) — shop got slower each test as dump code piled up.

### Cleanup shipped (test 5 candidate) — strip leftover RE/probe code

User directive: accumulated probing made the shop slower; strip old diagnostic code. `NativeModel.cpp` went **1824 → 1087 lines**. Removed entirely:

- `ScanForStrings` — full `.rdata` string hunt + entire-`.text` LEA-table build + xref deep-dumps (ran once on first `LoadFromForm`; multi-MB scan + thousands of log lines).
- `DumpEventSinks` / `DumpBarterLoadModelSinks` — sink walker + ProcessEvent code/LEA dumps after every vendor preview (also removed from `ShopExplorer.cpp` call site + `NativeModel.h`).
- Form layout dump (`form+0x000..+0x300` every load), 16× `DumpComponentVtable`, name/model area dump, `unk0E0` probe.
- `DumpRawObject` full hex dump on every geometry leaf; probe-region hex dump (`geo+0x130..`); `HexDump("meshdata"/"emb"/"source"/"sink"/"PE"/"deep")`.
- Per-entry field-scan inventory (`count @…` / `ptr @…` / `verts?` / `idx16?` / `idx32?`), `rdata@` RTTI loop, `probe range` line, `TryParseMeshData` failure spam (kept success logs only).
- Dead helpers: `VerifyRTTI`, `ModuleNameFor`, `HexDump`, `DumpComponentVtable`, `TryParseMeshFromFields` wrapper (was already gone — section-4 blob scan calls `TryParseMeshData` directly).

**Kept:** `CanRead`/full-buffer checks, count caps (`kMaxCount` 200k / `kMaxIdx` 600k / `kMaxVerts` 200k), bounded string peek, field cross-pair, `TryParseMeshData(scanStart, scanEnd)`, string-only mat/tex path scan, path-swap Clone3D, success logs (`mesh from field cross-pair` / `mesh from geo field blob` / `TryParseMeshData OK` with offsets).

Also fixed hang-AV issues in the mesh path before cleanup (full `CanRead` before materialize, `maxIdx > 0`, bounded peeks).

Build OK (40.9s, pre-existing C4100/C4189/C4244 only).
Deployed SHA256 `88825DB1A9681CBD5C7E6282CA07D94D159729B028B5CADABA4A9C00C101C19C`.

### Test 5 next run must show
- **No hang.** Shop opens, previews load without multi-second stalls or crash.
- Log is **much shorter** (no LEA/xref/deep-dump, no form layout, no hex dumps, no per-entry field inventory).
- Same pipeline lines as test 3: `baseDummy=true`, `swap form+0x100 -> …`, per-part `loaded/extracted/meshes/tri`.
- Mesh success (if offsets hit): `mesh from field cross-pair: pos=… uv=… idx=… (vCount@+0x… iCount@+0x…)` or `TryParseMeshData OK` or `mesh from geo field blob: …` → preview `pos/uv/idx > 0`, `tri>0`.
- If still empty: only `no mesh data found in +0x130..+0x…` per leaf — no inventory left; next step would be re-adding a **one-line** count/ptr summary (not per-entry) or reading offsets offline from test-3 hex dump (counts at +0x390.., heap ptrs at +0x310/330/350).

### Test 5 result — clean but very slow; leftover `ptr @` log survived

Razorback load completed, 5/5 parts `loaded=true extracted=true meshes=1`, but:

- **~20–50s per part.** Geometry leaf → embedded-obj gap ~35s (cross-pair cost).
- Mesh mostly empty: `no mesh data found in +0x130..+0x800`, `tri=0`.
- One false-positive: `mesh from field cross-pair: pos=33 uv=0 idx=27 (vCount@+0x300 iCount@+0x310 u16=true)` → tri=9 (garbage).
- **416 leftover `ptr @+0x…` lines** — one per-entry log survived the cleanup (`NativeModel.cpp` field-scan loop after collecting `heapPtrs`).

Root cause of slowness: cross-pair did **full-buffer `CanRead(ptr, count*stride)` on every count×ptr pair** (O(counts × ptrs) `VirtualQuery`), and `TryParseMeshData` did the same on every RTTI/blob candidate. Sample-only checks had been removed when fixing the test-4 hang, with no later re-add of the cheap path.

### Perf fix (test 6 candidate) — sample candidacy + attempt caps

Shipped edits in `NativeModel.cpp`:

1. **Removed leftover `ptr @+0x…` per-entry log loop.**
2. **Cross-pair + `TryParseMeshData`:** candidacy now uses **sample-sized `CanRead` only** (first 32 verts / 64 indices / 16 UVs). Full-buffer `CanRead` runs **once, immediately before materialize**. Index range validated against `v.count` **before** copying verts (garbage pairs fail cheap).
3. **Caps:** `kMaxCand=32` per candidate list, early-break when full; `kMaxAttempts=3` materialize attempts; one-line `cross-pair stopped after N attempts` if capped.

Safety (still present): full `CanRead` before any materialize, `maxIdx > 0`, count caps, bounded string peek — test-4 hang guards intact.

Build OK (27.5s, pre-existing C4189 only). Deployed SHA256 `194334C3C976E66D59F49AD665D3431366DDE4C8197BF5598CA6570E946BE5D1`.

**Offline preview OFF for test runs** (user directive: no offline raster until tests done). Runtime INI `B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.ini` → `[offlinepreview] enabled=0`. Viewport shows `offline preview disabled (INI enabled=0)`; only `NativeModel::LoadFromForm` runs. Re-enable with `enabled=1` after tests.

### Test 5b result — still hung 400+s (sample-candidacy fix was not enough)

Deployed `194334C3…` still took **~40s leaf→embedded + ~25s embedded→no-mesh per part**. Razorback 5 parts finished 14:50–14:56 then Regulator continued the same pattern — shop unusable for minutes. Root cause: deep heap probes (per-field string peeks with per-byte `CanRead`, RTTI demangle on every field, multi-object `TryParseMeshData` scans, 32-float samples through cold pages) → page-fault storms under a ~9GB working set. Caps alone did not bound wall-clock.

### Hang-kill fix (test 6) — 40ms wall-clock budget per geometry leaf

Shipped in `NativeModel.cpp`:

1. **`ProbeGeometryNode`: 40ms hard budget** (`std::chrono`); every stage checks `expired()` and bails. Finish log includes `{}ms fields= counts= expired=`.
2. **Field collect:** max 64 fields; string peek = one `CanRead(128)` + in-buffer scan (no per-byte VirtualQuery); RTTI demangle deferred (store `hasRtti` only); no `ResolveRTTIName` during collect.
3. **RTTI parse: first RTTI object only** (was: every RTTI heap ptr).
4. **Cross-pair:** 8 candidates/list, 2 attempts, sample 4 verts (was 32), index sample 16, skip materialize if `v.count>100k` / `i.count>300k`.
5. **`TryParseMeshData`: own 25ms budget**, same tighter caps (`kMaxVerts=100k`, `kMaxIdx=300k`, cand=8, attempts=2).

Build OK (23.8s, pre-existing C4189 only). Deployed after game closed.

### Test 6 result — hang killed (400s → ~17s); budget eaten by maxOff probe

Deployed `F76F0AE5…`, offline `enabled=0`. **Shop usable again** — full catalog browse ~17s of native work vs multi-minute freeze.

Per-leaf log: `no mesh data found … (2426–3671ms fields=0 counts=0 expired=true)` — **37/37 leaves**. Diagnosis:

1. `maxOff` probe loop (~108× `CanRead`/`VirtualQuery` on the geo object) ran **before** any `expired()` check. Under the game's fragmented address space each VirtualQuery ≈20ms → **~2.5s**, budget (40ms) already dead.
2. Field collect then never ran → `fields=0 counts=0` → RTTI/cross-pair/embedded/blob all skipped via `expired()`.
3. `GetGameSections()` re-parsed PE headers every leaf (minor).

Razorback 5 parts completed in ~13s (15:11:12–15:11:25), still `tri=0`. Offline tag absent from log (disabled correctly). Native preview lines: `loaded=true extracted=true … tri=0`.

### Budget-eaten fix (test 6b)

1. **Drop maxOff probe** — fixed `maxOff=0x800` (every observed leaf is readable there); single `CanRead(base+0x130, 0x6D0)` for the whole window, then raw field reads (no per-offset VirtualQuery on self).
2. **Cache `GetGameSections()`** in a function-local static.
3. Field collect / RTTI / cross-pair still budget-gated; should now actually run within 40ms and report `fields>0 counts>0 expired=false`.

Build OK (72.8s, pre-existing C4189 only). Deployed SHA256 `B64C8A96B8A7D2F2653C7EBE4783B6915B921614313FF12FAB27FF210B168929` @ 15:19:38.

### Test 6b result — hang still fixed (44–63ms/leaf); budget eaten by setup CanReads

Deployed `B64C8A96…`. Log 15:30–15:33: **82/82 leaves** `no mesh data found … (44–63ms fields=0 counts=0 expired=true)`.

Hang fix holds (avg ~45ms/leaf, Razorback 5 parts ~0.7s). But `fields=0 counts=0` again — field loop never ran. Diagnosis: `t0` started at function entry, so the three setup `CanRead`s (window 0x6D0, world 64, sections) burned the entire 40ms; first `expired()` check in the field loop returned true → body never executed → `counts=0` regardless of content.

### Budget-after-setup fix (test 6c)

1. **Budget starts after setup** — `tSetup` covers window/world/sections only; `t0`/`expired()` start immediately before field collect. `setupMs` added to finish log.
2. **Split field collect:**
   - **Phase A (counts):** pure u32 reads in the already-validated window, no `CanRead`, not budget-gated — always fills `counts[]` (cap 64).
   - **Phase B (heap ptrs + mat/tex peek):** budget-gated, one `CanRead(12)` per candidate + one bounded string peek.
3. RTTI / cross-pair / blob stages unchanged (still `!expired()` gated).

Build OK (21.7s, pre-existing C4189 only). Deployed SHA256 `33FF52AC67D74C3CC04E17DBBED519E4029AA9D3D5B84DF4EBF66677DD974671`. Game not running at deploy. Offline INI still `enabled=0`.

### Test 6c next run must show
- Finish log includes `setup=NNms`: if `setup` is large (>40ms), setup `CanRead`s are still the bottleneck (rare VirtualQuery stall) — move them fully outside any budget / log separately.
- **`counts>0` on every leaf** (Phase A has no VirtualQuery — if still 0, the window at +0x130..+0x800 has no u32 in [8, 200000], which would mean wrong base/size).
- `fields>0` (heap ptrs in Phase B) and ideally `expired=false` → RTTI/cross-pair/blob actually run.
- Mesh still optional until non-zero inventory; then pair counts@+0x390 with ptrs@+0x310/330/350 (nested `NiTriShapeData*` hypothesis).
- Shop stays responsive; Razorback 5 parts &lt;1s.

### Test 6c result — hang + inventory fixed; mesh stages starved by per-field VQ

Deployed `33FF52AC…`. **82/82 leaves** `no mesh data found … (40–63ms setup=25–31ms fields=5–11 counts=64 expired=true)`.

| Check | Result |
|-------|--------|
| Hang | ✅ 40–53ms/leaf |
| `counts>0` | ✅ always 64 (cap) |
| `fields>0` | ✅ 5–11 |
| Budget | ❌ `expired=true` 57/57 |
| Mesh | ❌ `tri=0` |

**Cause:** setup logs `setup=25–31ms` for just 2 `VirtualQuery`s → **each VQ ≈13ms** in this process. Phase B’s per-candidate `CanRead` + string peek burned the 40ms; RTTI/cross-pair/blob (all `!expired()`-gated) never started.

### Fix 6d — free Phase B + separate 80ms mesh budget

1. **Phase B = `IsValidPtr` only** (no VirtualQuery). Heap-ptr candidacy is free; vtable/RTTI and string peeks deferred.
2. **Mesh budget restarts after collect** — `tMesh` / `meshExpired` / `meshMs`, **80ms** (~6 VQs at 13ms). All RTTI/cross-pair/embedded/blob gates use `meshExpired`, not the setup budget.
3. **RTTI:** lazy vt check (one `CanRead` each), first 3 rdata-vt ptrs only.
4. **Mat/tex peek AFTER mesh**, max 4 ptrs, still under mesh budget.
5. Finish log adds `mesh=NNms`.

Build OK (6.1s then 12.9s / 2.8s).

### Direct live wire (live render test — NOT the old path)

Per user: *“if testing live it's the direct live not the old.”*

- **Direct live** = NativeModel Clone3D meshes → `OfflineModel` GPU rasterizer (`RequestPreviewDirect`).
- **Old path** = `.sfmodel` cache + `LiveModel` NIF parser (`RequestPreview`) — **not used** for this test.
- `ShopExplorer`: keeps `s_native` static across frames; builds `DirectMesh` list from non-empty pos/idx meshes; INI skiplist still forces empty direct request.
- `OfflineModel`: `g_pendingDirect` model; `ServicePending` rasterizes it without `ResolveModel`; log `direct-live '…' -> ready (N mesh(es), M tris)`.
- **Runtime INI** rewritten: `[offlinepreview] enabled=1`, `[livemodel] enabled=0`, `[itemcapture] mode=2`.
- Deployed SHA256 `03B57C8C85C3183555E2A0CA751A59A75B7D4BD11CBD155ABA3A68C75AECB894`.

### Test 6d + direct-live next run must show
- Finish log: `counts=64`, `fields≈64` (IsValidPtr cap), **`mesh=NNms`** with `expired=false` or at least RTTI/cross-pair lines actually attempted.
- If still `expired=true` on mesh stage: mesh budget still too small for the VQs it issues — raise `kMeshBudget` or cut sample `CanRead`s further.
- **`mesh from field cross-pair` / `mesh extracted` with `tri>0`** — then PREVIEW shows the model (direct-live).
- Log: `SFShopExplorer offline: direct-live '…' -> ready (N mesh(es), M tris)`.
- No `resolve '<key>' ->` lines while testing direct live (those mean the old path ran).
- Shop stays responsive; Razorback 5 parts &lt;1s.

### Test 6d result — inventory + direct-live OK; mesh stage still VQ-starved

Deployed `03B57C8C…`. **193/193 leaves**: `no mesh data found … (80–103ms setup=27–38ms mesh=80–103ms fields=64 counts=64 expired=true)`. Zero mesh/RTTI/cross-pair lines. Direct-live works: `direct-live 'Razorback' -> ready (0 mesh(es), 0 tris)` — PREVIEW correct (`0 mesh(es)`) because extraction still yields nothing.

| Check | Result |
|-------|--------|
| Hang / responsive | ✅ ~80–100ms/leaf |
| `fields=64 counts=64` | ✅ all 193 |
| Mesh stages | ❌ always `expired=true`, no attempt logs |
| Direct-live wire | ✅ (shows `direct:…`, not old `resolve`) |

**Cause:** mesh budget 80ms ÷ ~13ms/VQ ≈ **6 VirtualQueries**. Cross-pair was 64 counts × 64 ptrs with `CanRead` per pair; `TryParseMeshData` VQ'd every field offset; RTTI up to 3×. Budget died before any useful pairing.

### Fix 6e — VQ region cache + cut unique queries

1. **`CanRead` region cache** (thread_local, 64 slots): one `VirtualQuery` stores `[regionStart, regionEnd)`; sibling offsets in the same region are free. Scanning one geo object or re-probing one heap region no longer costs 13ms per offset.
2. **Cross-pair pre-filter (no VQ):** vert-like counts (8..100k) and index-like (`%3==0`) capped at 16 each; probe at most **16 heap ptrs**; one `CanRead(ptr,64)` per ptr then sample via cache.
3. **`TryParseMeshData`:** one window `CanRead(a_obj+scanStart, span+0x40)` then **raw field reads** (no per-offset VQ); candidate data ptrs still `CanRead(ptr,12)` once (cache hits on repeat).
4. **RTTI:** first rdata-vt object only (was ≤3).

Build OK (3.5s, pre-existing C4189 only).

### Test 6e next run must show
- Finish line still `fields=64 counts=64` but **`mesh=NNms` with much lower values** (cache hits) and ideally **`expired=false`**, OR actual stage lines: `mesh extracted` / `mesh from field cross-pair` / `embedded obj`.
- **`tri>0`** → PREVIEW `direct-live '…' -> ready (N mesh(es), M tris)` with N>0.
- If still always `expired=true` with no stage logs: budget still too tight — raise `kMeshBudget` to 150–200ms (still ≪ old 40s) or skip RTTI when cross-pair is the primary path.
- Shop stays responsive; no old-path `resolve '<key>'` lines.

### Test 6e result — PERF SOLVED; mesh stage runs; layout still wrong

Deployed `E6A46F37…`. **436/436 leaves**: `(0ms setup=0ms mesh=0ms fields=64 counts=64 expired=false)`.

| Check | Result |
|-------|--------|
| Hang | ✅ **0ms**/leaf (region cache + pre-filter) |
| `expired` | ✅ false 436/436 |
| Stages run | ✅ `embedded obj at base+0x130 RTTI='NiTObjectArray<NiPointer<NiAVObject>>'` every leaf |
| Cross-pair / RTTI mesh | ❌ no success lines |
| Direct-live | ✅ `direct-live '…' -> ready (0 mesh(es), 0 tris)` → white PREVIEW (correct empty) |

**Root cause of white screen (not a render bug):** `tri=0` → 0 meshes handed to GPU.

**Why extraction fails:** +0x130 is a **children array** (`NiTObjectArray`), not mesh data. RE notes already said real fields are **non-adjacent**: counts at **geo+0x390..+0x3F0**, buffers at **+0x310/330/350**. Old `kMaxFields=64` starting at +0x130 filled with header junk and **never collected** those offsets; cross-pair also only probed the **first 16** ptrs (all header).

### Fix 6f — reach the RE-confirmed bands + inventory dump

1. **`kMaxFields` 64 → 512** (collection is free pure-reads) so counts@+0x390 and ptrs@+0x310 are actually captured.
2. **Cross-pair prioritization:** counts in **+0x370..+0x420** first; probe ptrs in **+0x2E0..+0x370** first (then up to 32 others).
3. **One-shot field inventory** on first leaf: `field inventory: counts=N ptrs=M` + first 24 count/ptr offsets (RE aid).
4. **Cross-pair miss log:** `cross-pair empty: verts= inds= vCands= iCands= probes=`.

Build OK (4.5s, pre-existing C4189 only).

### Test 6f next run must show
- Still **instant** (`0ms`, `expired=false`).
- **`field inventory:`** line — confirm whether +0x390 counts and +0x310 ptrs appear.
- **`cross-pair empty:`** (or success) with cand/probe counts — if `vCands=0 iCands=0`, band filter is wrong; if `verts>0 inds>0` but no mesh, materialize validators reject.
- Ideally **`mesh from field cross-pair`** with `vCount@+0x390` → `tri>0` → PREVIEW shows model.
- White PREVIEW is expected while `tri=0`; success = non-zero `direct-live … (N mesh(es), M tris)`.

### Test 6f result — counts@+0x390 CONFIRMED; cross-pair builds verts+inds; materialize rejects all

Deployed `D6CE50B6…`. **150/150 leaves**: `(0ms … expired=false)`. Direct-live still `0 mesh(es)` (correct empty → white PREVIEW).

**Inventory (first leaf) — RE layout confirmed:**
```
count +0x390 = 2250
count +0x3A0 = 6912
count +0x3B0 = 2885
count +0x3C0 = 2917
… +0x3D0..+0x440 also real-looking vert/index counts
counts=90 ptrs=127
```

**Cross-pair now finds candidates** (was empty):
```
cross-pair empty: verts=8 inds=8 vCands=24 iCands=24 probes=32 (0ms)
```
Sometimes `verts=0 inds=8` (extent sample fails). **Zero materialize successes** — `verts=8 inds=8` but every pair rejected before `mesh from field cross-pair`.

**Why materialize fails / inventory junk:**
1. **Ptr collection stepped `off += 4`** → misaligned overlapping u64 reads: inventory shows `0x100000002`, `0x3f800000` (float 1.0), `0x7f7fffff` — packed u32 pairs and float bits, not heap ptrs. `IsValidPtr` accepted them (`>0x10000`).
2. Band ptrs (+0x2E0..+0x370) not visible in first-24 dump — may be fine real ptrs *or* also polluted.
3. Sample `CanRead(ptr,48)` passes (small region / lucky floats) but materialize `CanRead(ptr, count*12)` fails, or ratio/idx-range rejects.

### Fix 6g — aligned ptrs + materialize reject counters

1. **Ptr scan `off += 8` only** (8-byte aligned; object base is aligned).
2. **Tighter heap filter:** `val >= 0x100000` and `<= 0x00007FFFFFFFFFFF` (drop packed junk).
3. **Inventory:** dump **all band ptrs +0x2C0..+0x3A0** plus first 16 ptrs.
4. **Materialize diagnostics:** `rejV rejRatio rejI rejIdx attempts` + first vert/idx sample (`count`, `countOff`, `ptr`, `u16`).

Build OK (4.8s, pre-existing C4189 only).

### Test 6g next run must show
- `band ptr +0x310/330/350 = <heap>` — real buffers present.
- `cross-pair empty: … rejV= rejRatio= rejI= rejIdx=` — identifies which gate kills pairs.
- `vert sample` / `idx sample` lines — check whether `countOff` is in +0x390 band and `ptr` is a plausible heap address.
- Goal: one pair survives → `mesh from field cross-pair` → `tri>0` → PREVIEW non-empty.

### Test 6g result — band ptrs are OBJECT HEADERS, not buffers; verts=0 always

Deployed `4221C24F…`. Instant (`0ms`, `expired=false`). `verts=0 inds=8 attempts=0` on **every** leaf (materialize never runs — rej counters all 0 because no vert cand exists).

**Band inventory (first leaf):** all `+0x2C0..+0x3A0` ptrs target one **~344-byte heap cluster** `0x1feda19c000`–`0x1feda19c158` (pointers spaced 8–16 bytes apart — array of pointers / small objects, **not** a multi-KB vertex float buffer).

**Likely layout:** `geo+0x310/330/350` = pointers to small nested objects (`NiTriShapeData` / segment headers / `BSGeometrySegmentData`); actual pos/idx buffers live **one level deeper** inside those objects. Count@+0x390 is real but has no direct buffer pointer on the geo object.

**Also:** inventory dumped only first-24 counts → never showed band counts in this run (fix: dump all `+0x370..+0x420` counts explicitly).

### Fix 6h — one-level nested indirection + band-header TryParse + band count dump

1. **Inventory:** always dump every count in **+0x370..+0x420** (RE band), plus existing band ptr dump.
2. **Nested probe:** for each band ptr target (`+0x2C0..+0x3A0`), read first **0x80 bytes** and collect inner heap ptrs → these become the primary cross-pair probe list (expected: real vertex/index buffers).
3. **Probe order:** nested ptrs first → non-band geo ptrs → raw band headers last (deprioritized as vert candidates).
4. **Phase 1b:** `TryParseMeshData` on up to 6 band object headers (scan +0x08..+0x200 for adjacent count+ptr pairs) — handles nested `NiTriShapeData*` hypothesis directly.
5. Log `nested probe: N from band targets`.

Build OK (5.8s, pre-existing C4189 only).

### Test 6h next run must show
- `band count +0x390 = …` etc. in inventory.
- `nested probe: N from band targets` with N > 0.
- **`verts>0`** on cross-pair lines (nested buffers sample as float3).
- Or **`mesh from band header +0x310 …`** if TryParse finds pairs inside the nested object.
- Goal: `mesh from field cross-pair` / `mesh from band header` → `tri>0` → non-empty direct-live.

### Test 6h result — nested ptrs found but still verts=0; idx hits module rdata

Deployed `43565043…`. Instant. `verts=0 inds=8 attempts=0` every leaf.

**Inventory (this session, first leaf):**
- Band counts: `32758`, `501`, `505` (junk/repeats — **not** the 6f RE values; different object or layout varies).
- Band ptrs: mix of real heap (`0x1f55…`), **module rdata** (`0x7ff6e663…`), packed junk (`0x1f500000000`, `0x8400000769`).
- `nested probe: 19–48` from band targets — inner ptrs found but still no float3 verts.
- **idx sample always module rdata:** `ptr=0x7ff6e663de40` / `0x7ff6e5783ea0` — index sampler accepted **.rdata** as u16 buffer (module bytes look like valid indices). That is why inds=8 with garbage and verts=0.

**Root cause of 6h:** cross-pair probes included **module/rdata pointers**; sampleVertExtent correctly rejects them (not float3), but index sampling does not. Nested ptrs from band targets also need module filtering.

### Fix 6i — FO4 rendererData chase + heap-only probes + hex dump

**FO4 CommonLib fact (applies conceptually to Starfield BSGeometry):**
```
BSGeometry.rendererData @+0x148 → TriShape {
  VertexDesc @+00  (stride = (desc & 0xF) * 4)
  VertexBuffer* @+08
  IndexBuffer*  @+10
}
BSGraphics.Buffer {
  data* @+08, maxDataSize @+30, dataSize @+34
}
```
Actual CPU vertex/index bytes are **inside Buffer.data**, not pointed at by geo+0x310 band headers (those are object/array headers).

**Changes:**
1. **Phase 0:** Read slots `+0x140..+0x170` (FO4 `rendererData` at +0x148) → treat as TriShape → follow VB/IB → `Buffer.data@+08` / `dataSize@+34` → de-stride float3 + validate indices. Logs `tri-shape` / `VB data=…` / `extent=…`.
2. **Inventory hex dump:** one-shot `geo+0x140..+0x17F` as u64 lines (locks rendererData/vd slot offline).
3. **Heap-only probes:** `addProbe` rejects `.text`/`.rdata` module ranges + `inRdata` — stops fake idx8 from module bytes.
4. Nested probe / non-band / band order unchanged otherwise.

Build OK (4.3s, pre-existing C4189 only).

### Test 6i result — FO4 +0x148 is NULL; Starfield layout differs; heap probes work

- Hex dump: `geo+0x148 = 0` (no FO4 rendererData); `+0x150..+0x168 = 0x7f7fffff…` bound placeholders; `+0x140 = 0x0001000300030003` packed u16s.
- Tri-shape chase fired on many slots but first qword was a **heap ptr** (not VertexDesc) → `stride=0`, `VB data=0xffff000100000001` (packed junk). FO4 Buffer field offsets wrong for Starfield or wrong object.
- **Heap-only probes fixed idx samples:** now `ptr=0x1f2d…` (not `0x7ff6…` module). Found `idx count=498 @ +0x37C` (166 tris plausible) and `196608` (junk).
- Still **`verts=0`** — no vert-count candidate passed `sampleVertExtent`.

### Fix 6j — full RE dump + gated tri-shape + vert-count diagnostics

1. **One-shot inventory:** every count `+0x180..+0x420`, **full u64 hex `geo+0x130..+0x3C0`**, first 16 heap-ptr **targets** (16 bytes each at target).
2. **Tri-shape gate:** require `stride = (vd&0xF)*4` in `[12,64]` — rejects heap-ptr-as-vd; scan `+0x140..+0x1C0`.
3. **Cross-pair fail:** log first 8 `vertCount cand +0x… = …` (or `EMPTY`) — explains verts=0.

Build OK (4.2s). Pre-existing C4189 only.

### Test 6j result — hex locked layout; 615s are pointer high-halves; stride bug

**Full hex `geo+0x130..+0x3C0` (first leaf):**
- `+0x130` = rdata vtable `NiTObjectArray<NiPointer<NiAVObject>>` (embedded children — Phase 3 confirmed).
- `+0x148` = **0** (FO4 rendererData NULL).
- `+0x150..+0x168` = `0x7f7fffff…` bound placeholders.
- `+0x1B0: 0x3f8000000000168c` → **u32 +0x1B0 = 5772 (0x168c)** real count candidate.
- `+0x238: 0x0000008d0000009c` → counts 156 / 141.
- `+0x248..+0x3B8` = dense heap ptrs into one ~0x170-byte cluster `0x26797dba000` (object/array headers, self-referential — not buffers).

**Count scanner bug (root cause of verts=0):** every `count = 615` is the **high 32-bits of a heap pointer** (`0x00000267…` → 615). Scan steps `off+=4` so `off%8==4` hits pointer high-halves. Band `+0x370..+0x420` filled entirely with 615s → `vertCounts` (24 slots) never saw real `+0x1B0=5772`.

**idx samples (later leaves):** `count=5772 off=+0x370 ptr=0x2682965fa20 u16=true` — real-looking idx buffer. `count=615 ptr=0x26797dba000` is garbage.

**Tri-shape:** gated correctly — silent this run (no false fires).

**Stride bug:** `sampleVertExtent` assumed packed float3 (stride 12). Starfield verts are interleaved (pos+nrm+uv…, typically 24–40 bytes). Even a correct buffer would fail extent sampling at stride 12.

### Fix 6k — reject pointer high-halves + multi-stride sampling

1. **Count scan:** when `off%8==4`, read qword at `off-4`; if it `IsValidPtr` (≥1MB, canonical), skip — kills the 615 flood. Real counts (`5772`, `156`, `141`) now enter `counts[]` unfiltered.
2. **Multi-stride `sampleVertExtent`:** try strides `{12,16,20,24,28,32,36,40,44,48,52,56,64}`; first finite non-degenerate extent wins; store `stride` on `VCand` / `Candidate`.
3. **Materialize uses stored stride** (`flt = stride/4`) in both cross-pair and `TryParseMeshData`.
4. Full-buffer `CanRead` uses `count * stride` (was `* 12`).

Build OK (5.1s, pre-existing C4189 only).

### Test 6k result — counts clean; still verts=0; probes hit headers not buffers

Deployed `35FEBE56…`. Instant. `verts=0 attempts=0` still; `tri=0`.

**Counts (first leaf):** `+0x1B0 = 5336`, `+0x1BC = 196608` (0x30000 flags), `+0x1C8 = 63698`, `+0x238 = 33` — pointer-high-halves gone (`counts=6`).

**vertCount cands:** `65539@+0x144` (packed `0x00010003`), `5336`, `63698`, `33` — real values now enter the list.

**Still `verts=0`:** multi-stride `sampleVertExtent` fails every (count, probe) pair. All tgt dumps are cluster headers / bounds / RTTI — **no float3 data at any depth-1 target**.

**idx junk:** `count=196611 off=+0x140` = packed `0x00030003` flags read as u32; `196608@+0x37C` likewise. Both pass `%3==0` → fake idx candidates.

**+0x210:** object with rdata vtable `0x7ff6e794d0b8` (RTTI) — Phase 1 only tries first match and `break`s; may never reach this child.

### Fix 6l — 2-level nested probes + all-RTTI TryParse + packed-flag reject

1. **2-level nested probe:** non-band ptrs first (`+0x138`, `+0x210`, …) walked depth=2 (`std::function` walker), then band. Should reach actual buffer allocations behind the header cluster.
2. **Phase 1 RTTI:** try **every** non-band rdata-vt object (cap 4, budget-gated), log `RTTI obj +0x… vt=… name='…'` via `ResolveRTTIName`/`DemangleRTTI`. Scan window widened to `0x08..0x280`.
3. **Packed-flag reject:** both `indexCountOk` (cross-pair + TryParseMeshData) and count scan drop values where high-16 == low-16 (`0x00030003`, `0x00010003`) — kills `196611`/`65539` pseudo-counts.
4. `#include <functional>` for the recursive walker.

Build OK (4.34s, pre-existing C4189 only). Deployed SHA `304E96D04449D97210BC8964B30E5BEAB3F39256FBEEBCE185986248D8CB018A`.

### Test 6l next run must show
- `nested probe: N (2-level, non-band+band)` with N substantially larger than 6k's `36`.
- `RTTI obj +0x… -> … vt=… name='Ni…'` / `'BS…'` lines (up to 4).
- **No** `idx sample … count=196611` (packed flags rejected).
- Prefer: `mesh extracted at +0x210` / cross-pair verts>0 → `tri>0` → non-empty direct-live.
- If still `verts=0`: all 2-level targets also lack float3 → CPU vertex data may be GPU-only → escalate to NIF-file fallback (`LiveModel::BuildModels` → DirectMesh, render still direct-live).

### Test 6l result — false mesh (size/flag counts); real verts unpaired

Deployed `304E96D0…`. Instant.

**Wins:**
- `nested probe: 45–48 (2-level, non-band+band)` — depth-2 works.
- Packed `0x00030003` gone from idx samples.
- **First ever cross-pair mesh:** `pos=196608 idx=196608 (vCount@+0x304 iCount@+0x37C u16=true stride=48)` → `tri=65536` → direct-live rendered. **But it's garbage.**

**False-mesh root cause:**
- `vCount@+0x304 = 65536` (0x10000 — **allocation size field**, not vert count). `countJunk` missed `lo16==0`.
- `iCount@+0x37C = 196608` (0x30000 — **flags**). `%3==0` and `lo16==0` passed old `indexCountOk`.
- Ratio `tri=65536` vs `v=65536` passes; u16 indices `maxIdx < 65536` always true vs `v.count=65536` → full validate passes on any non-zero u16 blob.

**Real verts still unpaired:**
- `verts=8` (extent-ok candidates exist!) — first is `count=63698 off=+0x1C8`.
- `iCands=1` (only junk `196608`) → all8 verts failed `rejIdx` within `kMaxAttempts=2`.
- `RTTI obj +0x210/+0x3D0` always `VLooseFileLocation:BSResource` (same heap `0x2ad49655c20`) — not geometry data.

### Fix 6m — kill size/flag counts + more attempts + derived idx + diagnostics

1. **`countJunk` / `indexCountOk`:** also reject `hi!=0 && lo16==0` (0x000N0000 = 65536, 196608, …). Applied in cross-pair + TryParseMeshData.
2. **`kMaxAttempts`:** 2 → **16** (both cross-pair and TryParseMeshData).
3. **Derived idx probe:** if verts found but `inds < 4`, synthesize idx counts `{v, v+v/2, 2v, 3v}` (mod-3) from each vertCount and sample them as u16 across all probe ptrs. Logs `derived idx probe: N counts, inds now=M`.
4. **Diagnostics on empty fail:** log all `idxCount cand`, `vert sample … stride=`, first **6 floats** at top vert ptr (confirms real positions vs header), and first 3 `rejIdx` details (`vCount/iCount/u16/max`).

Build OK (7.4s, pre-existing C4189 only). Deployed SHA `3CBCFD9ED0B21D8ABF83ECF32C9F1E60B729D43ABE735A4FE2754FA1B3C906D6`.

### Test 6m next run must show
- **No** `mesh from field … vCount@+0x304` false mesh (65536 filtered).
- **No** `tri=65536` / `pos=196608` previews.
- `idxCount cands EMPTY` or only real values (not 196608).
- `derived idx probe: N counts, inds now=M` with M > 0.
- `vert floats: …` — if values look like model-space coords (tens–hundreds), verts are real.
- **Goal:** `mesh from field cross-pair` with `pos≈63698*3` (or similar real count) and matching `idx` → real `tri` → non-empty direct-live that looks like the item.
- If verts floats are real but all `rejIdx`: index buffer isn't among probes → next step is buffer-adjacent idx (scan `vertPtr + count*stride` region) or accept NIF-file fallback.

### Test 6m result — false mesh killed; verts junk; attempts burned on one pair

Deployed `3CBCFD9E…`. First PgUp CTD'd (transient); 2nd attempt ran clean. Full log captured.

**Wins:**
- **No** `tri=65536` / `pos=196608` false mesh — `countJunk`+`indexCountOk` `lo16==0` reject works.
- `derived idx probe: 12 counts, inds now=8` — derived path fills inds when field iCands=0.
- Diagnostics fired: `vert floats`, `rejIdx … derived=`, top-3 verts (not in this build — added in 6n).

**Failures (root causes for 6n):**
1. **`vert floats: -0.014 0.000 0.000 0.000 0.000 0.000`** — `extent>0.01` still accepts header junk (near-zero bbox). Top vert `count=63698 extent≈0.014` is garbage.
2. **Sort-by-count + shared derived inds:** all 16 `kMaxAttempts` burned on `v=63698` × derived `{63696, 95547, 127395}` (same junk family) before smaller real counts (`5336`, `5215`) were tried. `rejIdx=16` every pair.
3. **`iCands=0`** — no field idx count survives `indexCountOk`; only derived inds exist.
4. `idx sample: count=5358 off=+0x0` — a plausible u16 idx blob exists at a probe ptr but never pairs (wrong v.count / ratio).
5. Geometry leaf RTTI still garbage `'?T?…2 '` — node name/RTTI parse wrong on Starfield NiAVObject (cosmetic; mesh path doesn't need it).

### Fix 6n — vert quality scoring + per-vert attempt cap + buffer-adjacent idx

1. **`sampleVertExtent`:** require `hasMag` (any |coord| > 0.05) and **`extent > 0.1`** (was 0.01). Kills header-junk verts like `-0.014,0,0,…`.
2. **`VCand.extent` stored; sort verts by extent desc** (real mesh bbox first), then count.
3. **`ICand.derived` flag; stable_sort field inds before derived.**
4. **Per-vert attempt cap = 3** — one bad pair cannot exhaust `kMaxAttempts=16`; smaller verts get tried.
5. **Buffer-adjacent idx probe:** after cross-pair fails, for each vert try `idxPtr = vertPtr + count*stride + {0,4,8,16,32,64}` as u16 with derived+field counts. Logs `mesh from buffer-adjacent idx` or `buffer-adjacent idx: tried=N`.
6. **Diagnostics:** `extent=` on vert sample; top-3 `vert[i] count/extent/stride/off`; `rejIdx` now includes `derived=`.

Build OK (3.8s, pre-existing C4189 only). Deployed SHA `459ECFABE21E676AEE800828C2C30C880220BE1E381498961204F02886B484FB`.

### Test 6n next run must show
- Top verts: **no** `extent≈0.014` header junk; expect `extent` in 0.1–100 range if real coords found, or `verts=0` if all near-zero.
- `vert[0..2]` lines — if any has real-looking count (500–20k) + extent≥0.1, that's the real mesh.
- Prefer: `mesh from buffer-adjacent idx: pos=… idx=…` → `tri>0` → non-empty direct-live.
- Or: `mesh from field cross-pair` with non-derived `iCount` (not 63696/95547/127395).
- `rejIdx … derived=true` should be rare (field inds tried first).
- If `verts=0` after hasMag+extent>0.1: probe ptrs don't reach real vertex buffers → escalate to NIF-file fallback (`LiveModel::ParseNifGeos` → DirectMesh, still direct-live render).
- If verts real but buffer-adjacent `tried=0` or all fail: idx buffer not contiguous → try 2-level nested ptrs only for idx, or accept NIF fallback.

### Test 6n result — extent filter works; idx still missing

Deployed `459ECFAB…`. Ran clean, no CTD.

**Wins:**
- `vert[i] count/extent` diagnostics live. Real-looking candidates: `count=5363 extent=22.958 stride=52 off=+0x1B0` (5363 = the known +0x1B0 count), `count=257 extent=… off=+0x3C8`.
- Per-vert cap works: attempts rotate 63698→5363→257 (rejIdx=9 across multiple vCounts instead of 16 on one pair).
- `extent` in 12–229 range (real bbox-ish), not 0.014 header junk. `hasMag`+`extent>0.1` passing.

**Failures:**
1. **`vert floats` still zeros/garbage** at first 6 floats (`0.000…` or huge bit-pattern floats) despite extent ok — extent samples stride-interleaved positions, but first-3-floats-of-buffer aren't xyz (leading floats are 0/normals/other). Materialize would emit garbage pos even if idx found.
2. **`iCands=0` every leaf** — no field idx count survives `indexCountOk`. All inds are `derived=true` and sample wrong ptrs (`firstBad/max=4` or `781` = random heap, not the idx buffer).
3. **`buffer-adjacent idx: tried=12`** all fail — idx buffer not contiguous after vert end (+0..+64 pad).
4. Leaves with `verts=0 inds=0 attempts=0` — extent filter rejected everything there (stricter than before; acceptable).

### Fix 6o — content-based index discovery

1. **Idx-content scan:** for each vert cand, treat every probe ptr as a raw u16 sequence: sample 64 values; if all `< v.count` and `max>0`, walk forward to natural run end (cap `v.count*3` / 300k), round down to `/3`, full-validate, ratio-check → materialize. Logs `mesh from idx-content: pos=… idx=… run=… ptr=…` or `idx-content scan: tried=N`.
2. **World-matrix typo fix:** `wz` translation index `world[13]`→`world[14]` in new paths (rows 0-3 = x,y,z,w of transform; col3 = translation = +12,+13,+14).

Build OK (4.2s, pre-existing C4189 only). Deployed SHA `8FF5306AB194BFD6127E52158BCD574B7EDCD303663CBF038C5E216BC33FD62D`.

### Test 6o next run must show
- **Goal:** `mesh from idx-content: pos=5363*3 idx=run` → `tri>0` → non-empty direct-live that renders the item.
- Or `idx-content scan: tried=24` with no hit → probe ptrs don't contain the idx buffer at all → last native step is to scan **all heap ptrs in the object window** (not just 48 probes) as raw u16, or accept NIF fallback.
- If `run` found but `pos` looks wrong (zeros): fix materialize to skip leading non-pos floats (derive pos offset from stride by finding 3 consecutive floats with coord-like range) — only after idx works.

### Test 6o result — first meshes (false); real iCounts exist; probes miss idx data

Deployed `8FF5306A…`.

**Wins:**
- **First `mesh from …` lines ever:** `mesh from buffer-adjacent idx: pos=15231 idx=9` (Microgun, `tri=6`) and `pos=191094 idx=9` (UnarmedHuman, `tri=3`) → `direct-live ready (2 mesh(es), 6 tris)`. Path works end-to-end.
- **`iCands` no longer always 0:** `iCands=5..11` with real values (`3852`, `1446`, `859`, `455`) — field scan finds index counts.
- `idx-content scan: tried=24` runs clean (no hits — probe ptrs aren't idx buffers).

**Failures:**
1. **Buffer-adjacent false meshes:** `iCount=9` for `v=5077` (3 tris vs 5077 verts) — **no ratio check** in buffer-adjacent path. `iCount=9` is a header field, not indices.
2. **Field iCounts always `rejIdx`:** `vCount=5077 iCount=3852 firstBad/max=780` — count is real but sampled at a random probe ptr, not the count's own data buffer.
3. `idx-content` never hits: probe set doesn't contain the idx buffer.
4. `hi==1 && lo16==2` (65538) slipped through `indexCountOk` as fake iCount.

### Fix 6p — kill false meshes + field-local ptr pairing

1. **Ratio check in buffer-adjacent tryCounts loop:** `tri+1 < v/8 || tri > v*4` → skip (kills `iCount=9`).
2. **Adjacent content-run first:** at each pad, walk u16 while `< v.count` to natural end; materialize only if run passes ratio + `max>0`. Logs `mesh from adjacent-run: … run=N`.
3. **Field-local count↔ptr pairing:** for every idx/vert count field, probe `count±8`, `count±16` as likely `{count, ptr}` struct neighbors — added to **front** of `probePtrs` before nested walk. Logs `field-local probes now=N (idxCounts=… vertCounts=…)`.
4. **`countJunk`:** also reject `hi==1 && lo16<8` (65538).

Build OK (23.9s, pre-existing C4189 only). Deployed SHA `4524EBB04499AB52AFFC9C9AD6F0DC69C4C3748975CB40CB5C47A4AFC52D15FF`.

### Test 6p next run must show
- **No** `mesh from buffer-adjacent idx: … iCount=9` false meshes; **no** `tri=3`/`tri=6` junk previews.
- `field-local probes now=N` with N > 0 — local ptrs in probe front.
- **Goal:** `rejIdx` rate drops because field iCount (3852) now samples its own buffer → `mesh from field cross-pair` or `adjacent-run` with `idx` proportional to `v.count` (e.g. 5077 verts ↔ ~3k–15k idx) → real `tri` → direct-live that looks like the item.
- If local ptrs are object headers (extent fails): the count±8 fields aren't data ptrs → layout is `{count @ A, ptr @ A+0x80}`-class; next is probe `count` paired with heap ptrs at `count+0x80±8` or accept NIF fallback.

### Test 6p result — false meshes GONE (win), but verts=0 everywhere (regression)

Deployed `4524EBB0…` (SHA verified against deployed file). Log: session 20:59–21:02, 6793 lines.

**Wins:**
- **Zero false meshes** — no `iCount=9`, no `tri=3`/`tri=6`, no `mesh from buffer-adjacent idx` junk anywhere. Ratio check + adjacent-run + `countJunk hi==1&&lo16<8` all work. No hangs.

**Failures:**
1. **`verts=0 inds=0 attempts=0` on ~148/150 items** (6o had `verts=8`). `vCands=3 iCands=0` uniformly.
2. **`field-local probes now=5`** — all 5 resolve to band-cluster object headers (`0x1feb159bf10`/`0x1feb159bfc0`/`0x1feb159c000`, same `0x1feb159c0XX` cluster as band ptrs). count±8 pairing finds headers, not data buffers.
3. **`iCands` 5..11 → 0**: cannot be the `countJunk hi==1&&lo16<8` rule (kills only 65538); no other scan change in 6p. Likely data/session variance (different objects scanned) — 6o log was overwritten by game restart, cannot diff.
4. Two outliers: `verts=3 inds=8 attempts=6 rejIdx=6` and `verts=6 inds=8 attempts=12 rejRatio=24 rejIdx=12` — derived inds, all rejected (correctly, ratio broken).
5. One-shot inventory (UnarmedHuman): `counts=6 ptrs=194`; counts `+0x1B0=5287` (v-class), `+0x1BC=196608` junk, `+0x1C8=63698` (non-div-3), `+0x238=206`. geo+ dump: `+0x150..+0x168` = ±FLT_MAX **empty AABB** — scanned leaf may be a bounds proxy, not the mesh holder (suspicion, unconfirmed). Targets of `+0x138` = `0x1ff57c09fa8 → 0x1ff57cb9e30, 0x1ff57c01500` — 2nd-level heap region, already probed via nested walk.

**Root-cause hypothesis for verts=0:** field-local ptrs pushed INTO `probePtrs` before the nested walk → consumed 5 of the 48 cap → non-band nested truncated 32→27 → the extent-passing vertex-buffer ptr (tail of non-band group) never probed. 6o (no field-local) had verts=8.

### Fix 6q — field-local out of cap + probe-content dump

1. **Field-local → separate `localPtrs` vector** (own 16-cap + dedup, deltas `±8, ±16, ±0x80`) — never touches `probePtrs`, nested walk gets its full 48 again. Combined `allPtrs = locals + nested` (deduped) used by all three probe loops (main / derived-idx / idx-content).
2. **`allPtrs=N (locals=X nested=Y)`** log line.
3. **One-shot probe dump:** first 3 cross-pair empties dump first 10 probes' leading 6 floats — shows whether probes point at vertex buffers (coord-like floats) vs headers (0 / pointers-as-floats / ±FLT_MAX).
4. Ratio check + adjacent-run + all 6p junk rules **kept** (they work).

Build OK (7.0s, pre-existing C4189 only). Deployed SHA `1EBCF32477BD2E3DC83A3212546BFA8C183AA8F268846DF4F9F487850707B482`.

### Test 6q next run must show
- **`field-local probes=N`** (own count, ~4–12) and **`allPtrs=X (locals=N nested=48)`** — nested back to 48.
- **`verts>0` restored** on items where 6o had verts=8 (extent-passing VB ptr re-probed) → `buffer-adjacent`/`adjacent-run` with **ratio-passing** idx → first real `mesh from …` with `tri` proportional → direct-live render.
- **Probe dump (first 3 empties):** if leading floats are coord-like (e.g. `-12.345 3.2 0.1`) probes hit buffers → tune sampling; if 0/FLT_MAX/pointer-bit garbage → probe targets are wrong objects entirely → escalate.
- If verts restored but still no idx: next is `count+0x80±8` pairing (layout `{count @ A, ptr @ A+0x80}`) or, with user approval, NIF-file fallback (`LiveModel::ParseNifGeos` → DirectMesh, still direct-live).


### Test 6q result - nested restored, but 0 meshes (diagnosis complete)

Deployed `1EBCF324...`. Log: session 14:45, 7766 lines, 138 cross-pair items.

**Wins:**
- `allPtrs=50/52 (locals=6 nested=48)` on every item - displacement fix confirmed; nested walk back to full 48.
- `verts>0` restored: 70/138 items (63x verts=3, 5x8, 2x6); 68x verts=0.

**Failures:**
1. **`mesh from` lines = 0.** `iCands=0` on ALL 138 items, `vCands=3` always; `idx-content tried=24`, `buffer-adjacent tried=12`, `rejRatio`/`rejIdx` typical.
2. **Probe dump (first 3 empties):** `probe[0]=0x3f800000` (!CanRead - that is the float 1.0f read as a ptr: count+8 = 1.0f), probes 6/7 misaligned garbage, probes 1/2/3/5 = band-cluster headers with FLT_MAX floats, probe[9] all zeros -> **field-local pairing is useless** (count+8/+16 are sibling header fields, not ptrs).
3. **`verts>0` is a false positive:** `sampleVertExtent` samples only the first 4 verts at ptr start and ignores `count`, so the same shared buffer `0x1b586fd33e0` (stride=56, extent=6.743 constant) "passes" every count in 68 items. The real VB is not in the probe set.
4. **Leaf layout decoded (first item, geo+ dump):** `+0x1B0={5072, 1.0f}`, `+0x1BC=196608`, `+0x1C0` -> bounds object (also +/-FLT_MAX empty), `+0x1C8=63698`, `+0x210` -> `VLooseFileLocation:BSResource` (file reference!), `+0x150..+0x168` empty AABB, `+0x148=0` (FO4-style rendererData slot empty).
5. **Structural hole:** nested walk fills the 48-cap first -> the leaf's own 194 field ptrs are never probed directly (direct level-1 adds at lines 707-718 are no-ops).
6. **iCands=0 root:** the window has only 6 counts total (5072, 196608, 63698, 245, ...), none div-by-3; byte-size pairing does not exist anywhere in the code.

**Hypothesis:** geometry streams from disk (empty bounds + BSResource file loc) -> no resident VB in our detached Clone3D; and/or rendererData (+0x148) only materializes on scenes the game actually renders (the inventory preview scene), never on our detached clone.

### User decision after 6q

> "native only - create a probe for inventory not the shop so every item i view gives you the answers - also look at whats in B:\MO2\Starfield\mods\Shop Test Render delete all the stuff not needed"

NIF fallback: **declined** (native only). New deliverable: run the full diagnostic per **inventory view** (not shop hover), and clean the mod folder.

### Test 6r - inventory view probe (chain)

The pieces already existed from the earlier v1.0 experiment (`inventory_renderer_notes.md`); this test finally ships the missing native side:

1. **SWF:** `tools/ba2_extract/inventorymenu_probe_test.swf` (stock + one guarded AS call: `BGSCodeObj.DevilzCaptureItem(ItemList_mc.selectedEntry)` on selection change, before Bethesda's `EVENT_TRY_ON`/`EVENT_LOAD_3D_MODEL` dispatch; null-guarded -> inert without the native function) deployed as `Interface\inventorymenu.swf` in the test mod. Verified: decompressed SWF contains `DevilzCaptureItem` x1, stock does not.
2. **Native (`InventoryProbe.h` + `SFE::InventoryProbe` in ShopExplorer.cpp):**
   - `Tick()` on the SFSE task thread (main.cpp BrowserTask::Run, after `ItemCapture::FramePoll`): while InventoryMenu is open, installs **our own** `Scaleform::GFx::FunctionHandler` on the menu's `BGSCodeObj` via `asMovieRoot->CreateFunction(handler)` + `SetMember("DevilzCaptureItem", ...)` - dispatch goes to OUR `Call`, not the menu's idx dispatch (no collision). Installs once per movie (`menu->uiMovie` pointer compared); retries every tick; WARNs once after 300 ticks if `menuObj`/root never becomes ready.
   - `Call()` (movie thread): logs `capture callback fired form=... handle=... name=...` (the v1.0 success phrase), queues formID (cap 8). No game-object access.
   - `Tick()` pops one formID per tick, defers while the shop is open, dedupes per menu session, then runs the full answer set: `=== viewing 'name' (0xFID) type=... ===` -> `FormModelPaths` -> `NativeModel::LoadFromForm` (the whole 6q diagnostic: counts/ptrs/allPtrs/probe dump/cands/mesh lines) -> `result loaded=... extracted=... tri=...` + per-mesh `geo leaf` lines. Menu close logs `unique items probed this session=N`.
3. **Build 56-inventory-probe.** Deployed SHA `837FD4F5873FC16D61B6A3C22AF45729918A0B48F4FC4E177A50AC22B04F739F`.
4. **Mod cleanup done:** deleted 23 `SFShopExplorer.dll.pre-*` (25.8 MB), `InventoryRenderTelemetryProbe.*`, `InventorySceneBoundaryProbe.*`, `ShopTestRender.*` (v1.1 tracer, result already recorded), the 734-file `.sfmodel` cache (156.1 MB, `usecache=0`), and the empty `textures\` tree - **182 MB removed**. Kept: `SFShopExplorer.dll/ini`, 4 UI PNGs, `Interface\inventorymenu.swf`, `ShopRender.esm`/`- Main.ba2`/`meta.ini`.

### Test 6r next run must show (open inventory, hover items)

- `DevilzCaptureItem installed (movie=...)` once per menu open - if instead the 300-tick WARN appears, `menuObj`/root path differs for InventoryMenu -> fall back to `asMovieRoot->GetVariable("_root")` explicitly.
- `capture callback fired form=0x...` per hovered item - **this is the v1.0 success criterion.** If zero callbacks: SWF override not loaded (check loose-file name/case) or the AS guard found no function (install line missing from log).
- Per item: `=== viewing ... ===` + the full 6q answer set + `result ... tri=...`.
- **Key question this answers:** does the inventory item's window differ from the shop's (e.g. counts div-by-3, real ptrs, `verts>0` with honest extents, `rendererData +0x148` populated) -> tells us whether the buffers exist at all when the game renders the item.
### Test 6r result - install never succeeded + broken inventory UI (session 15:31-15:32, build 56, 1864 lines)

- Plugin loaded, `existing menu callback name=InventoryMenu` fired (menu created), but the ONLY probe line in the whole session is `DevilzCaptureItem install still failing after 300 ticks (menuObj/root not ready)` at 15:32:42.497 (last line of log). No callback, no item probe ran. Game not running after the test.
- User screenshot: inventory UI visibly broken - near-empty screen, `PLAYER 100` bar, repeated placeholder button hints `SLABEL [A]KEY`, no item list ("yeah not quite right"). Native side cannot blank the UI (TryInstall returns before touching the movie on failure) -> the SWF override was the prime suspect.
- **Override conflict:** four other mods also ship `Interface\inventorymenu.swf`: `120 FPS - Smooth UI` (+ inventorymenu_lrg.swf), `Compact Inventory UI`, `Star Wars Genesis - HUD Pack`, `Undelayed Menus (120 FPS)` (+ _lrg).
- **MO2 priority (profiles\*\modlist.txt, later line = higher priority):** in BOTH profiles where Shop Test Render is enabled it outranks all four UI mods - `Star Wars Genesis Compiler`: Shop Test Render 560 vs HUD Pack 119 / Compact 94 / Undelayed 92 / Smooth 91; `Test profile`: Shop Test Render 546, all four UI mods disabled there. (Default profile has Shop Test Render disabled entirely.) => **our patched SWF was the active inventorymenu.swf in both runnable profiles.**
- **AS re-check - why the install could never work:** `BGSCodeObj` appears in NO stock script; the only occurrence is our patch line in InventoryMenu.as (`var codeObj:Object = this["BGSCodeObj"]`). Stock InventoryMenu has no codeObj on the root -> `GetMember(CODE_OBJ_NAME)` fails -> TryInstall returns false forever; even a successful SetMember would have had no caller. Also: `selectedEntry` is an AS3 GETTER (`BSScrollingContainer.get selectedEntry` -> `entryList[selectedIndex]`), `entryList` is a protected getter -> `private var _visibleEntriesA`, and `_selectedIndex` is a private int.
- `SLABEL` grep hits were false positives (case-insensitive match inside `LegendaryModsLabel_tf`).

### Test 6r-2 - native GFx scrape + diagnostic dump (SWF removed)

1. **Deleted** `B:\MO2\Starfield\mods\Shop Test Render\Interface\inventorymenu.swf` (restores pre-test state: SWG Compiler profile -> HUD Pack wins again; Test profile -> BA2 stock).
2. **InventoryProbe rewritten** (build **57-gfx-scrape**, deployed SHA `E4A1C32E06F5F5FC6F222AD2F043CB9C279A95672181610BF94CC3E670B0B104`):
   - `DumpDiagnose` runs twice per menu movie (on open + at tick 150 ~5s later): menu RTTI name (local vtable resolver + demangler copied into ShopExplorer.cpp), `menuObj`/rootPath/root types, root member probes (BGSCodeObj, ItemList_mc, CategoryList_mc, ItemCard_mc, EquippedItemCard_mc, PreviewSceneRect_mc, ButtonBar_mc, PlayerStatus_mc), capped `VisitMembers` listings with `IncludeAS3PublicMembers=true` on root and on ItemList_mc (first 40 of total), `_visibleEntriesA` count via ArrVisitor + entries[0] field probes (uFormID/uHandleID/sName/uCount/iFilterFlag), dormant codeObj install result if a codeObj ever exists.
   - `ScrapeSelected` runs every tick while open: root -> ItemList_mc -> `selectedEntry` getter, else `_visibleEntriesA[_selectedIndex]` via ArrVisitor ElementVisitor -> uFormID/sName. New formID -> queue + log `selection form=0x... name='...' queued`. Probe chain unchanged (one queued formID per tick, defers while shop open, per-session dedupe, full LoadFromForm answer set).
   - WARN once at 300 ticks only if never scraped: `selection scrape unresolved after 300 ticks (state=N)` where 0=NoRoot, 1=NoList, 2=NoEntry, 3=NoForm.
   - InventoryProbe.h chain doc updated to the 6r-2 design.
3. **Mod folder now:** meta.ini, ShopRender.esm, ShopRender - Main.ba2, SFSE\Plugins\{SFShopExplorer.dll, SFShopExplorer.ini, SFShopExplorerAssets\*.png}. No Interface tree.

### Test 6r-2 next run must show (open inventory, hover items)

- **Inventory UI normal again.** If it is still broken with our SWF gone -> not the SWF; then investigate what else changed (deleted passive probes, ItemCapture mode=2).
- `dump1 menu rtti='...' menuObj=... rootPath='...' root=...` -> identifies the C++ class behind InventoryMenu (plain IMenu vs MenuPaperDoll/UICellRenderer subclass -> feasibility of the rendered-scene scrape route, incl. PreviewSceneRect_mc).
- `dump1 root probes: BGSCodeObj=absent ItemList_mc=dispobj ...` -> BGSCodeObj absence is expected (explains 6r); ItemList_mc readability is the key.
- `dump1/2 itemlist probes: selectedEntry=... _visibleEntriesA=... _selectedIndex=...` -> **decides whether AS3 getter/private members are externally readable through the GFx C++ API (the whole 6r-2 bet).** The VisitMembers listings show every name that IS reachable (publics + dynamic).
- `selection form=0x... name='...' queued` + `=== viewing ... ===` + full probe answer sets per item -> success criterion for the chain.
- If `selection scrape unresolved (state=2)` AND dumps show `selectedEntry=absent _visibleEntriesA=absent` -> AS3 sealing confirmed -> next candidates from the dump: reachable publics list, item-scrape via the game-rendered preview scene (PreviewSceneRect_mc native side / MenuPaperDoll route), or BSUIDataManager event sinks.
### Test 6r-2 result - CTD on inventory open (session 16:01, build 57)

- Log: plugin loaded, profile `Test profile` (all four UI mods disabled there -> stock BA2 SWF active after our Interface file removal; inventory UI restoration untested because...). InventoryMenu opened 16:01:16.160; `dump1` and `dump2` both ran to completion (all member listings logged); log ends 16:01:16.733; CTD within ~1s of menu open. No WER event 1000 and no dump file captured (LocalDumps for Starfield.exe not configured; HKLM write denied without elevation).
- **Dump findings (kept for the record):** `rtti='VInventoryMenu'`, `menuObj=dispobj rootPath='root1.Menu_mc'`, `BGSCodeObj=absent` (confirms 6r install could never work), `ItemList_mc=dispobj`; `selectedEntry`/`selectedIndex`/`entryCount` are AS3 GETTERS and returned values from the task thread (`selectedIndex=int=-1`, `entryCount=int=0`); private `_selectedIndex`/`_visibleEntriesA`/`_entriesA` all `absent` (sealed); VisitMembers listings = builtin display props only (no AS3 publics). entries never loaded before CTD (`entryCount=0`, no `selection ... queued` line).
- **CTD cause (high confidence):** executing AS3 getter bytecode and doing GFx Value acquire/release on the movie from the SFSE task thread at high frequency (~300 ticks/s scrape) while the movie thread runs concurrently. Build 56 never executed AS3 (only GetVariable path traversal) and survived 300 ticks; v0.9 read state a handful of times from menu-callback context and survived. 6r-2's per-tick `GetMember(selectedEntry)` = the new unsafe element. All cross-thread GFx access removed in 6r-3.

### Test 6r-3 - native event sinks (build 58-inventory-events)

The v1.1 sink mechanism (`BSTEventSink` on a game UI event source, proven with `BarterMenu_LoadModel` ID 91377) is the safe path: sinks fire on the game's own dispatch thread (during AS event dispatch), zero GFx access, passive subscription only.

1. **Root blocker found and solved:** CommonLib's `IDs.h` zeroes the InventoryMenu event IDs (`GetEventSource{ 0 }` - the old notes' "ID 0") but keeps the real Address Library IDs as comments. Verified against the installed `versionlib-1-16-244-0.bin` (V5 header=96 bytes, offsetCount=1274968; direct `offset(id)` at 96+id*4):
   - `InventoryMenu_SelectItem` = **139344** OK, `InventoryMenu_LoadModel` = **139341** OK, `InventoryMenu_PaperDollTryOn` = **132471** OK, `InventoryMenu_SetMouseOverModel` = **139345** OK (comment IDs), plus `BSTEventSource::RegisterSink`=123821 OK, `GetInventoryObjectSub`=63846 OK, `BGSInventoryInterface::Singleton`=883301 OK; control: `BarterMenu_LoadModel`=91377 (header-active value, v1.1's working ID) also OK.
2. **Implementation** (ShopExplorer.cpp InventoryProbe rewritten; old dump/scrape/RTTI/visitor/CaptureHandler/TryInstall code deleted):
   - `RegisterSinks()` called from main.cpp `kPostLoad`: own `REL::Relocation` to `Event::GetEventSource()` using the real comment IDs (bypassing CommonLib's zeroed constants), then `source->RegisterSink(sink)` (ID 123821). Null source -> WARN fail-closed.
   - `EventSink<Event>` x4 (`SelectItem`, `LoadModel`, `PaperDollTryOn`, `SetMouseOverModel`): logs `<event> fired bytes=` (16 raw payload bytes), then tries u32 candidates at payload +0/+4/+8/+12 as `uItemHandle` via `BGSInventoryInterface::GetInventoryObject` (self-validating: wrong candidate -> null). Success logs `payload+0x.. handle=0x... -> form=0x... 'EDID'` + `queued form=0x... for probe`.
   - `Tick()` now: native `IsMenuOpen` session bookkeeping + queue drain + `RunProbe` (unchanged full LoadFromForm answer set). No `uiMovie`/`menuObj`/GFx access at all.
   - Event structs are forward-declared only (`Events.h` is not self-contained; dispatch is vtable-based, so declarations suffice).
3. **Build 58-inventory-events**, deployed. `Interface\inventorymenu.swf` remains deleted.

### Test 6r-3 next run must show

- **No CTD**; inventory UI normal (stock SWF on Test profile).
- `inventory probe: InventoryMenu_* sink registered (source=0x...)` x4 at load (kPostLoad) - or WARNs if a source is null.
- In inventory: `InventoryMenu_* fired bytes=XX ...` per hover/selection - THE key line. Which events fire and how often tells which is the per-view signal.
- `payload+0x.. handle=... -> form=... '...' queued` x1 per view - if a +0/+4/+8/+12 candidate resolves, the payload ABI is settled; if all four miss, decode the offset from the `fired bytes=` dump next iteration.
- `=== viewing ... ===` + full 6q answer set per item + `menu closed; unique items probed this session=N`.
### Test 6r-3 build 58 RESULT - GAME CRASHES AT LAUNCH (16:29:30 + 16:30:33)

- WER 1000 x2: `Starfield.exe` AV `0xc0000005`, fault offset `0x28cb2e2` (identical both runs). SFShopExplorer.log ends at `kPostLoad received` -> crash inside the build-58 `RegisterSinks()` call at kPostLoad. No menu, no UI - game never starts.
- ID-resolution path verified correct: `REL::ID::address()` = `FModule::GetExecutingModule()` = `GetModuleHandleA(nullptr)` = Starfield.exe base + IDDB offset (lib/commonlib-shared FModule.cpp) - so we were CALLING exe addresses.
- **ID-series mismatch discovered in IDs.h:** entries carry an ACTIVE value and a COMMENT value from two different series - e.g. `BarterMenu_LoadModel GetEventSource{ 91377 }; // 138996`. The active 91377 is the proven-working ID (v1.1 used it on this exact install). All4 InventoryMenu GetEventSource entries are zeroed with comment-series IDs (139341/139344/132471/139345) = the current-series ID is unknown/unvalidated upstream -> likely stale for Starfield 1.16.244 -> we called wrong functions -> deterministic AV inside exe code at a fixed offset.
- Secondary suspect: kPostLoad runs before menu event sources/singletons are initialized (no project sink had ever registered that early; Barter pattern at ShopExplorer.cpp:456 resolves lazily at runtime).

### Test 6r-3 build 59-sink-guard (fix for launch crash)

1. Registration moved from kPostLoad to LAZY: first Tick where InventoryMenu is open; retry up to 5 menu opens; per-source skip flags (`g_okSelect` etc.) so successful sources are never registered twice.
2. Every stage SEH-guarded (C2712-safe leaf helpers): `CallGetterGuarded` wraps the GetEventSource call, `RegisterSinkGuarded` wraps RegisterSink - a stale ID now logs `... threw an exception - stale or wrong ID? failed closed` instead of killing the game.
3. Per-stage logs: `getter id=N -> 0xADDR` / `source=0x...` / `sink registered (source=0x...)`.
4. CONTROL sink added on the proven `BarterMenu_LoadModel = 91377` (active series) so at least one event stream is guaranteed to work and gives payload bytes + candidate resolution even if all InventoryMenu comment-series IDs fail.
5. Deployed (tag 59-sink-guard). InventoryProbe.h documents the whole chain.

### Test 6r-3b next run must show

- **Game launches** (primary requirement after build 58).
- On first inventory open: `registering sinks (attempt 1)` then per-source outcomes. Two scenarios:
  a) Comment-series IDs are fine (earlier crash was timing): `sink registered` x5 -> wait for `... fired bytes=` lines in inventory.
  b) Comment-series IDs are stale: 4x `GetEventSource threw` WARNs + `BarterMenu_LoadModel(control) sink registered` -> use the control stream: its `fired bytes=` + payload candidate resolution settles the ABI; next iteration hunts the current-series InventoryMenu IDs (pattern-scan from the known 91377 getter, or accept Barter-only probing).
- Any `fired bytes=` line + `payload+0x.. handle=... form=... queued` -> full probe answer sets follow.### Test 6r-3b RESULT (build 59, session 10:24:54-10:26:49, 145 lines) - launches, all 4 comment IDs stale, Barter control OK, no events

- Game launched fine (59's guard worked). On first inventory open: 4x `GetEventSource threw an exception - stale or wrong ID? failed closed` (IDs 139344/139341/132471/139345 - CONFIRMED STALE, no crash) + `BarterMenu_LoadModel(control): getter id=91377 -> 0x7FF7BD82A450` -> `sink registered (source=0x7FF7C246E770)`.
- Registration attempts 1-5 burned within 16ms across SFSE task threads (24360/22732/20696/20832/22900) - Tick runs multi-threaded; registration state was unguarded (race bug).
- Zero `fired bytes=` lines: the Barter control sink never fired (user did not use a barter container; InventoryMenu events never registered) -> no probes ran, no payload ABI yet.
- ID hunt closed: local CommonLib IDs.h, upstream osf-ui copy, and the maintained libxse/commonlibsf fork all keep InventoryMenu event IDs zeroed/comment-only. Comment-vs-active numbering conflict proven: comment 139340 = InventoryMenu_HideModel while ACTIVE 139340 = BSStringPool::Entry::Release (RVA comments 0x28CAEA0/0x28CBA80); the build-58 crash offset 0x28cb2e2 sits in that BSFixedString cluster -> comment IDs belong to a different numbering era, unusable. Current-series IDs unknown upstream -> STOP hunting IDs, extract source ADDRESSES instead.

### Test 6r-3c - source-address extraction path (offline analysis)

- Notify-caller scan (Python, temp rtti_walk.py / notify_scan.py): imagebase 0x140000000, sections .text va=0x1000, .rdata va=0x3A3F000 vsize=0x1E2210A, .data va=0x5862000 vsize=0x2D03040 (file raw ~9.5MB; the rest is bss); runtime base this session 0x7FF7BC250000 (bin offset(91377)=0x15DA450). Scanned .text for E8 rel32 calls to BSTEventSource::Notify (ID 123824) and extracted rcx-loading context -> 13 direct-lea .data candidate addresses + mov-cells.
- Offline typing of those candidates via their FILE vptrs WORKED: 0x590A240=BSTEventSource<WeaponFiredEvent>, 0x5923EA8=BSMusicEvent, 0x597A400=PlayerCharacterQuestEvent::Event, 0x591B8B8=AttachReference::Event, 0x597A1F8=LevelIncrease::Event, 0x5924FD0=BSSysMonFrameCaptured. bss candidates (0x61EFF30, 0x82D7BD8/1B0/270, 0x82FF0A0) + known Barter source 0x621E770 = beyond file raw (zero in file) -> objects constructed at runtime, vptrs written dynamically.
- CONCLUSION: a runtime RTTI scan of .data finds every BSTEventSource regardless of ID eras - no function calls, no stale IDs, just vptr->RTTI typing (vptr-8=COL, col+0x0C=TypeDescriptor RVA, td+0x10=name). Barter getter bytes at 0x15DA450 read: guarded getter (init check + lea rax,[rip+disp]) - not a plain lea+ret, so code-shape scanning was rejected in favor of the object scan.

### Test 6r-3d build 60-rttiscan (deployed SHA 8CE8C4C79B59275CD05DD8B1315D81DCC878F0CFB30CE789DE0F579EBEBAC357)

1. `GetScanRanges()`: parse PE of GetModuleHandleA(nullptr) -> .rdata/.data bounds (base+va .. +VirtualSize, incl. bss tail).
2. `ScanSources`: walk .data every 8 bytes under ONE __try (POD locals only, every read range-bounded against .rdata): vptr in .rdata -> col at vptr-8 -> tdRva at col+0x0C -> bounded name copy at td+0x10, keep `.?A` names containing `InventoryMenu_` or `BarterMenu_` (cap 16), count all typed names.
3. `RegisterFromMatches`: RTTI name containing `BSTEventSource` AND the exact event name -> `RegisterOn` (SEH-guarded RegisterSink, per-flag skip so no duplicates); non-source matches (e.g. the event struct's own RTTI) logged, not registered. Only the Barter control still uses the getter (active ID 91377).
4. `LogCalibration`: reads the KNOWN Barter source at RVA 0x621E770 and logs its runtime vptr + RTTI name - proves bss sources carry vptrs (if this logs `no .rdata vptr`, vptr scheme is dead and the AS3 listener bridge becomes next).
5. Race fixes: registration block now under `g_regMutex` on menu-open RISING EDGE only (or attempt 0), max 8 attempts; `g_lastOpen`/`justClosed` under the same lock; `g_probed` check+push fully locked in RunProbe.
6. Deployed, tag `60-rttiscan`; DLL strings verified (60-rttiscan present, 59-sink-guard absent).

### Test 6r-3d next run must show

- Game launches; first inventory open: `registering sinks (attempt 1)` ONCE (not 5x in 16ms).
- `calibration: barter source vptr=0x... rtti='.?AV...'` - decides whether runtime vptrs exist (expected yes).
- `rtti scan: typed=N matched=M InventoryMenu/BarterMenu types` + `rtti match addr=0x... type='...BSTEventSource@...'` + `sink registered (scanned source=0x...)` per event -> THE success criterion (expect up to 4 InventoryMenu matches; Barter may match too but flag already set).
- Then in inventory: `InventoryMenu_* fired bytes=XX ...` -> `payload+0x.. handle=... -> form=... queued` -> `=== viewing ... ===` + full 6q answer set per viewed item.
- If scan reports 0 matches / missing sinks (`inventory sources may be heap-allocated`): next build = AS3 addEventListener bridge via `uiMovie->asMovieRoot->CreateFunction` on InventoryMenu open (payload uItemHandle known from InventoryMenu.as line 1240 dispatchCustomEvent), or a heap scan for BSTEventSource objects.### Test 6r-3d RESULT (build 60-rttiscan, session 11:05-11:07, 112 lines) - launches + race fixed, scan faulted on bounds bugs

- Race fix proven: `registering sinks (attempt 1)` fired ONCE (11:07:00.541), not 5x across threads. Barter control registered (source=0x7FF7C246E770). Inventory UI normal (screenshot: GREEN CUP item + game's own green 3D preview rendered).
- `calibration: barter source bad typedesc` + `rtti scan raised an exception after 0/0 slots` + `typed=0 matched=0`. Two bugs:
  1. FAULT: range checks used `col + 0x10 >= rdataE` - a garbage col near 0xFFFF...FF WRAPS col+0x10 to a small value, passes both checks, then derefs ~0xFFFFFFFFFFFFFFFC -> AV at the first such .data slot (caught by __try, scan aborted at 0 typed).
  2. `bad typedesc`: vptr/col/td were all checked against .rdata ONLY, but offline evidence shows COLs live outside .rdata (WeaponFired COL RVA 0x84045216, inside .data's VA range 0x5862000..0x8565040) -> even REAL objects fail narrow checks.
- Scan ran BEFORE any InventoryMenu event could fire (registration happens on menu open; events only fire during interaction) - expected; zero `fired bytes` lines this session.

### Test 6r-3e build 61-rttiscan2 (deployed SHA BD7C1F71A33AE0F2027359F6F96E8D9B780E274671711816D7FEA23D4BD6760E)

1. All RTTI bounds (vptr, col, td, name copy) are now IMAGE-WIDE (`[g_imgB, g_imgE)` from `OptionalHeader.SizeOfImage`; whole module is committed memory so any in-image read is safe) and OVERFLOW-SAFE (comparisons `vptr > g_imgE - 8`, `col > g_imgE - 0x10`, `td > g_imgE - 0x11` - no addition can wrap).
2. Registration filter tightened: exact `.?AV?$BSTEventSource@` prefix required (guarantees the CommonLib BSTEventSource<T> layout our RegisterSink assumes); other InventoryMenu-matching types are logged, not registered.
3. `LogCalibration` rewritten as `LogChain` stage dumps with ranges: logs `calibration: ranges img=... rdata=... data=...`, then q0/col/tdRva/td/rtti for BOTH the offline-known-good WeaponFired source (rva 0x590A240 - proves the runtime chain works) AND the Barter source (rva 0x621E770 - reveals whether it is a real vptr object or not).
4. Tag `61-rttiscan2`.

### Test 6r-3e next run must show

- `calibration: ranges img=[...]`, `calibration: known WeaponFired source ... rtti='.?AV?$BSTEventSource@VWeaponFiredEvent@@@@'` (if this resolves, the chain + bounds are correct; if it fails, dump stages lead).
- `calibration: barter source ... rtti='...'` - barter object's real type (or `q0 is not an image vptr` if it holds a non-vptr first member).
- `rtti scan: typed=N matched=M` (typed should now be in the hundreds/thousands, NOT 0) + `rtti match addr=... type=...` + `sink registered (scanned source=...)` for each InventoryMenu event found.
- Then: `InventoryMenu_* fired bytes=` per item view -> `payload+0x.. handle=... queued` -> `=== viewing ... ===` + full 6q answer set.
- If typed>0 but matched=0 for InventoryMenu events -> sources are heap-allocated or a different class name; next step: log all match names / broaden scan to heap or AS3 listener bridge.### Test 6r-3e RESULT (build 61-rttiscan2, session 11:16-11:19, 142 lines) - SCAN WORKS; game architecture revealed; cap-16 truncation

- Chain fully resolved: `calibration: ranges img=[0x7FF7BC250000,0x7FF7C4BD8000) rdata=[0x7FF7BFC8F000,0x7FF7C1AB110A) data=[0x7FF7C1AB2000,0x7FF7C47B5040)`.
- WeaponFired known object: q0=0x7FF7C0DD8FF0 col=0x7FF7C1276DA0 tdRva=0x5A5F1D0 -> `rtti='.?AV?$BSTEventSource@VWeaponFiredEvent@@@@'` (runtime chain + image-wide bounds proven).
- **Barter source real type: `.?AU?$EventSource@VBarterMenu_LoadModel@@@BSTGlobalEvent@@`** - the game dispatches these UI events through `BSTGlobalEvent::EventSource<T>` (CommonLib calls the same layout `BSTEventSource<T>`; our RegisterSink on it works - proven by v1.1 + the control sink).
- `rtti scan: typed=56348 matched=16` - matched = EXACTLY the cap (16). Contents: 8x `AutoRegisterEvent@V?$TUIEventDispatcher@VBarterMenu_*` (CloseMenu/LoadModel/HideModel/ShowFailureMessage/ViewedItem/BuyItem/SellItem/SetMouseOverModel) at 0x7FF7C21816F8-0x7FF7C2181768 (0x10-apart, 16-byte metadata objects) + 8x InventoryMenu counterparts (LoadModel/HideModel/SelectItem/DropItem/OpenCargoHold/ToggleFavorite/OpenAlmanac/SetMouseOverModel) at 0x7FF7C2181E10-0x7FF7C2181E80. NO PaperDollTryOn in the first 16 - list continues beyond cap; the REAL `EventSource@VInventoryMenu_*` statics (like barter's at 0x7FF7C246E770, ABOVE 0x7FF7C2181xxx in address order... actually later) were truncated by the cap.
- Registered: none (all 16 were AutoRegisterEvent = metadata, correctly rejected by the exact-BSTEventSource filter) -> `missing select=...` WARN expected. Zero fired lines (no sinks on InventoryMenu sources yet).

### Test 6r-3f build 62-eventsource (deployed SHA 1FA036AF22157143EF22434F67ACC3E5744AB3B19D1284AD37C12440480DDADC)

1. Match cap 16 -> 128 (stack ~21KB, fine) + `match cap N reached; results truncated` log when hit.
2. Registration filter changed to `EventSource@V<EventName>`: matches BOTH real forms (`.?AV?$BSTEventSource@VInventoryMenu_SelectItem@@@@` and `.?AU?$EventSource@VInventoryMenu_SelectItem@@@BSTGlobalEvent@@` - the game's actual class) while excluding `AutoRegisterEvent@V?$TUIEventDispatcher@V...` metadata (no `EventSource@V` substring). Layout safety: game `BSTGlobalEvent::EventSource<T>` == CommonLib `BSTEventSource<T>` layout (barter proof).
3. Every match still logged; non-source matches -> `not an EventSource type (dispatcher/metadata?)`; source matches for untracked events (DropItem, ToggleFavorite, OpenCargoHold, OpenAlmanac, HideModel, BarterMenu_ViewedItem/BuyItem/SellItem/...) -> `EventSource for an untracked event`.
4. Tag `62-eventsource`.

### Test 6r-3f next run must show

- `rtti scan: typed=56348 matched=N` with N > 16 (cap-not-hit) and among them `rtti match addr=... type='.?AU?$EventSource@VInventoryMenu_SelectItem@@@BSTGlobalEvent@@'` (or BSTEventSource form) -> `sink registered (scanned source=0x...)` per tracked event.
- +4 more lines about missing flags if PaperDollTryOn's source is absent -> non-fatal; the other 3 matter (SelectItem + LoadModel are the per-view signals).
- In inventory: `InventoryMenu_SelectItem fired bytes=XX ...` / `InventoryMenu_LoadModel fired bytes=...` + `payload+0x.. handle=0x... -> form=0x... 'EDID' queued` -> `=== viewing ... ===` + full 6q answer set per item viewed.
- If still NO `EventSource@VInventoryMenu_*` match: the InventoryMenu EventSource statics are heap-created -> next step: type the AutoRegisterEvent/TUIEventDispatcher q1 pointers (dump + LogChain) or call the (stale-ID) getters AFTER the menu is open and read the returned pointer's RTTI instead.
### Test 6r-3f RESULT (build 62-eventsource, session 11:28-11:31, 4751 lines) - EventSource statics found; SelectItem/LoadModel NOT in .data

- `rtti scan: typed=56349 matched=30` (cap not hit). Calibration identical to 61 (chain proven).
- `.data` EventSource statics found: `EventSource@VInventoryMenu_ResetPaperDollInv` (0x7FF7C243EEB8), `PaperDollTryOn` (0x7FF7C243EEE8 -> REGISTERED, paper=true), `Change3DView` (0x7FF7C243EF18), `ToggleSuit` (0x7FF7C243F338), `ToggleHelmet` (0x7FF7C243F3C0), `BarterMenu_LoadModel` (0x7FF7C246E770, already set), `BarterMenu_CloseMenu` (0x7FF7C246E7A0) - all BSTGlobalEvent globals.
- **NO `EventSource@VInventoryMenu_SelectItem` / `_LoadModel` / `_SetMouseOverModel` anywhere in .data** -> these three are PER-MENU-INSTANCE (heap) sources; only their `AutoRegisterEvent<TUIEventDispatcher<...>>` metadata (0x10-strided, 16-byte {vptr, q1}) sits in .data at 0x7FF7C2181E10-0x7FF7C2181EF0 (now 15 InventoryMenu entries incl PaperDollTryOn, OnEnterCategory, StartCloseAnim, Change3DView, ResetPaperDollInv, ToggleHelmet, ToggleSuit).
- Zero `InventoryMenu_* fired` lines (select/load/mouse unregistered) -> `missing select=false load=false paper=true mouse=false`.
- 12 `BarterMenu_LoadModel(control) fired bytes=` lines: these were triggered by the mod's EXISTING `TryNativeVendorPreview` (ShopExplorer.cpp:451 - ItemCapture hover poll -> synthetic `src->Notify(RawVendorEvent{formID})`) plus one game-real event (`01 08 00 0C ...`). Our TryQueueFromHandle silently dropped them: the synthetic events carry a FORMID (0x1F4 'UnarmedHuman'), not an inventory handle -> GetInventoryObject null at all four offsets -> no payload lines. Existing feature ran fine (Clone3D OK root=... per fire, iCands pattern as before).
- Inventory session: `menu closed; unique items probed this session=0`.

### Test 6r-3g build 63-menuscan (deployed SHA CF2FFB0F0991E1E57E2FC35C75EDB8356039D3807251F870BB7D6DADF99D5F16)

1. `ShopMenu::OnMenu` (fires per menu creation via the SFSE menu callback) now calls `InventoryProbe::NoteMenu(a_menu)` -> captures the live `RE::IMenu*` when `GetName()=="InventoryMenu"` (`InventoryMenu instance captured at 0x...` log); pointer cleared on menu close.
2. New `ScanMenuInstance` (runs in every registration attempt after the .data scan): `LogChain("menu instance")` prints the instance RTTI (expect `VInventoryMenu`), then walks the instance from `a_inst` to `min(+0x1000, VirtualQuery region end)` under `__try` with the shared `ReadTypeName` (image-wide bounds, bounded copy). Every polymorphic member whose type contains InventoryMenu/EventSource/TUIEventDispatcher/AutoRegister is logged as `menu member off=+0xX type='...'` (cap 64 + truncation marker); TUIEventDispatcher members additionally dump `fields[+8..+48]` as 6 raw qwords (next-iteration layout analysis: look for heap/image pointers to EventSource-like objects or a BSTArray triple).
3. EventSource-typed members are registered with the same `EventSource@V<Event>` match and flagged `g_menuDerived*`. On menu close: those flags reset their `g_ok*`, `g_allRegistered=false`, `g_regAttempts=0` (`menu-instance sinks released on close; will re-register on next open`) because the next open creates a NEW instance with NEW sources; `.data`-static registrations (paper, barter) persist and are skipped by their flags.
4. `LogChain` now accepts heap objects (obj-range image check removed; __try guards the reads). `RegisterFromMatches` takes a prefix arg (`rtti match` for .data) and logs `q1=0x...` for non-source matches (AutoRegisterEvent second qword = likely dispatcher/registration pointer).
5. Missing-flags WARN reworded: `still missing ... after .data + menu instance scans`. Tag `63-menuscan`.

### Test 6r-3g next run must show

- `InventoryMenu instance captured at 0x...` (after the SFSE `existing menu callback name=InventoryMenu` line) -> then `menu instance scan: typed=N structural=M`.
- `menu member off=+0xX type='.?AV?$TUIEventDispatcher@VInventoryMenu_SelectItem@@@@' fields[+8..+48]=...` and ideally `menu member off=... type='.?AU?$EventSource@VInventoryMenu_SelectItem@@@BSTGlobalEvent@@'` -> `InventoryMenu_SelectItem sink registered (scanned source=0x...)` (+load +mouse). THE success criterion for this build.
- If dispatcher members appear WITHOUT EventSource members: the 6 field qwords decide next step (field1 = heap ptr -> follow/type it; BSTArray triple {ptr,len,cap} at +8 -> sinks live IN the dispatcher -> reverse its layout from those values + the game's dispatch code).
- Then in inventory: `InventoryMenu_SelectItem fired bytes=...` / `InventoryMenu_LoadModel fired bytes=...` -> `payload+0x.. handle=... -> form=... queued` -> `=== viewing ... ===` + full 6q answer set per viewed item.
- On close: `menu-instance sinks released on close` (only if menu-derived ones registered) + re-register on reopen.
### Test 6r-3g RESULT (build 63-menuscan, session 11:48-11:52, 216 lines) - capture works; menu scan raced by 130ms

- Race: `registering sinks (attempt 1)` at 11:49:41.725 (open edge consumed, .data scan ran, paper=true) but `existing menu callback name=InventoryMenu` + `InventoryMenu instance captured at 0x29138CB7AA0` (heap) arrived at .855 - 130ms LATER. Attempt 1 saw g_invMenu==null -> `menu instance not captured yet` and, edge already consumed, NO second attempt -> `ScanMenuInstance` NEVER RAN this session (unknown whether InventoryMenu instances carry EventSource members).
- `.data` results identical to build 62 (typed=56345, matched=30, AutoRegisterEvent q1=0x0 for all).
- **PaperDollTryOn sink FIRED 16x** (11:52:16-11:52:29) - end-to-end proof that RegisterSink on `EventSource@V<Event>` statics works: `fired bytes=60 35 ED C0 F7 7F 00 00 E0 6D/69/9C 2F 86 DD 00 00 00` -> payload = {qword0=0x7FF7C0ED3560 (.rdata ptr), qword1=0xDD862F6DE0/-862F6F61E0 packed u64}; TryQueueFromHandle tried all four u32 offsets as handles -> all null -> no payload lines (correct: PaperDollTryOn is a try-on action, NOT the per-view SelectItem/LoadModel event).
- Menu closed clean: `menu closed; unique items probed this session=0`; no release log (nothing menu-derived registered - correct).

### Test 6r-3h build 64-latescan (deployed SHA 1A6185450622503A37D5DC7AED739E4D1649881B51F7D1E0A94BF678EC8791C7)

1. Attempt condition extended: `(risingEdge || g_regAttempts==0 || lateInst)` where `lateInst = g_invMenu!=nullptr && !g_invScanned`. When the SFSE callback delivers the instance mid-open (after attempt 1 burned the edge with g_invMenu null), attempt 2 fires tagged `, late menu-instance capture` and runs `ScanMenuInstance` on the captured heap instance.
2. `g_invScanned` set in TryRegisterSinks when the instance is present; reset on menu close (new instance each open). Cap 8 unchanged (edge attempt + late attempt = 2 in the observed ordering).

### Test 6r-3h next run must show

- `registering sinks (attempt 1)` then `InventoryMenu instance captured` then `registering sinks (attempt 2, late menu-instance capture)`.
- `menu instance scan: typed=N structural=M` + `menu member off=+0xX type='...'` lines - THE data this build exists for:
  a) `'.?AU?$EventSource@VInventoryMenu_SelectItem@@@BSTGlobalEvent@@'` members -> `InventoryMenu_SelectItem sink registered (scanned source=0x...)` (+load/mouse) -> full per-view chain (fired -> payload handle -> form -> === viewing ===).
  b) dispatcher-only members -> `fields[+8..+48]=` qwords are the next lead: heap ptr -> follow/type it in build 65; BSTArray triple -> sinks inside the dispatcher -> reverse layout.
  c) NO structural members -> sources are NOT in the IMenu object (deeper subclass member? separate singleton reached by the dispatcher's q1?) -> next: type the AutoRegisterEvent q1 (0x0 here - then follow the TUIEventDispatcher members' field qwords instead).
### Test 6r-3g/6r-3h RESULT (build 64-latescan, two sessions: 12:03-12:07 and 12:11-12:13) - REGISTRATION SOLVED; payloads have no handle in +0..+12

- Late-capture retry works: `attempt 2, late menu-instance capture` -> `menu instance scan: typed=42/37 structural=15` ran with the live instance (0x287B8B3C5B0 / 0x198BB5F4360 / 0x198BB61E470).
- KEY DISCOVERY: the InventoryMenu per-event EventSource statics live at 0x7FF7C246EA10-EBC0 (0x30-strided: StartCloseAnim EA10, OnEnterCategory EA40, SetMouseOverModel EA70, OpenAlmanac EAA0, ToggleFavorite EAD0, OpenCargoHold EB00, DropItem EB30, SelectItem EB60, HideModel EB90, LoadModel EBC0) and **only BECOME RTTI-visible after InventoryMenu first initializes them**: attempt 1 scan `matched=30` (stopped at BarterMenu_CloseMenu 0x246E7A0), attempt 2 scan 130ms later `matched=40` (+10) -> vptrs appeared. `.data` statics, NOT per-instance members.
- Result: `InventoryMenu_SelectItem sink registered (scanned source=0x7FF7C246EB60)`, `InventoryMenu_LoadModel ... 0x7FF7C246EBC0`, `InventoryMenu_SetMouseOverModel ... 0x7FF7C246EA70`, paper=true -> WARN line now only `still missing select=false...` on attempt 1 (attempt 2 completes).
- Menu instance carries NO EventSource members: `menu instance rtti='.?AVInventoryMenu@@'`, typed=37, structural=15 = subobject pointers `.?AVInventoryMenu@@` at +0x0,+0x10,+0x50,+0x138..+0x190 (8-strided) - harmless; the .data statics are the real sources.
- Events FIRE: 12:03 session SetMouseOverModel x12 + LoadModel x1; 12:12 session PaperDollTryOn x4. Payload shape (16 bytes): {q0=0x7FF7C0ED3xxx (.rdata static: 0xED3460 mouse, 0xED2FA8 load, 0xED3560 paper), q1=heap ptr (0x6A73EFCEB0 mouse, 0x6A73EF6800 load, varies paper)}. TryQueueFromHandle tried u32 +0/+4/+8/+12 -> all null -> **zero payload/queued/viewing lines**. SelectItem never fired (hover triggers SetMouseOverModel; LoadModel on category/3D-load; SelectItem needs an actual item CLICK).
- The 0x7FF7C0ED3xxx q0 values are static-only (per-event, in .rdata) - likely a BSFixedString or event descriptor, not the handle.

### Test 6r-3i build 65-payload (deployed SHA 22867133FAD3DD3497B62390B8D326651391FF5E66BA903EC2685C31986B7F7C)

1. `ProcessEvent` now: guarded 48-byte read (`ReadEventBytes`, SEH-safe if event < 48), `fired bytes=` shows all 48, `TypePayloadSlot` RTTI-types every qword slot (heap vptr read in __try + shared ReadTypeName) logging `payload+0xN at 0x... rtti='...'`, and `TryQueueFromHandle` runs for EVERY u32 offset 0..44 (success logs `payload+0xN handle=... -> form=...` with the offset that worked).
2. Guard against false handles: only GetInventoryObject successes log (handles are small sequential ids; .rdata/heap low-u32s will not resolve).

### Test 6r-3i next run must show

- For each `fired bytes=` (now 48 bytes): `payload+0x8 at 0x... rtti='...'` lines identifying q1 (if it is e.g. `.?AVInventoryMenu@@`-adjacent, BGSEntryPoint, TESObjectREFR, or an item wrapper type) - this names the payload object.
- `payload+0xN handle=0x... -> form=...` IF a u32 in 0..44 is a real inventory handle -> `queued form=...` -> `=== viewing ... ===` + full 6q set. **User must CLICK items (SelectItem) as well as hover** - SelectItem never fired in 6r-3h sessions.
- If rtti lines show a heap object type but no handle resolves: next build reads fields of THAT type (e.g. TESBoundObject* at known offset -> GetFormID; or item wrapper -> inner handle field) - the offset comes from its CommonLib definition / another dump.
### Test 6r-3i RESULT (build 65-payload, session 12:24-12:25) - THE DISPATCH ENVELOPE DECODED

- `fired bytes=` (now 48) shows a 6-qword envelope identical in shape for every event:
  - +0x00 per-event `.rdata` descriptor (load=0x7FF7C0ED2FA8, mouse=0x7FF7C0ED3460, paper=0x7FF7C0ED3560)
  - +0x08 `BSGFxObject*` heap (menu's GFx object; rtti logged `'.?AVBSGFxObject@@'`; varies: 0x7E95146C10, 0x7E95149F10, 0x7E95149FC0...)
  - +0x10 per-event `.rdata` descriptor (load=0x7FF7C0ED2978, paper=0x7FF7C0ED2A30)
  - **+0x18 heap object typed `'.?AVInventoryMenu_LoadModel@@'` / `'.?AVInventoryMenu_PaperDollTryOn@@'` / `'.?AVInventoryMenu_SetMouseOverModel@@'` - VARIES per fire (LoadModel: 0x7E95146BA0 -> 0x7E95149F30 -> 0x7E95149EA0 -> 0x7E95149F50) = THE REAL EVENT INSTANCE (polymorphic heap object)**
  - +0x20 the EventSource itself (0x7FF7C246EBC0 load / 0x7FF7C243EEE8 paper / 0x7FF7C246EA70 mouse)
  - +0x28 = 2
- `env+0x..` handle tries at all u32 offsets 0..44: NO resolution (expected - envelope has no handle).
- Volume/behavior: LoadModel spams (~60 fires in 8s during 3D preview + category changes), PaperDollTryOn on try-on, SetMouseOverModel 3x on hover, **SelectItem STILL never fires** (its trigger is still unknown - possibly item click needs focus; keep prompting user to click items).
- `Still missing` WARN gone from attempt 1 output? (attempt1 registered select? - see session: attempt 1 matched=40 already this time; instance captured before attempt1? no - `menu instance not captured yet` then attempt2 did menu scan; select/load/mouse registered by attempt1's .data scan this session since statics already initialized from previous InventoryMenu use.)

### Test 6r-3j build 66-payloadobj (deployed SHA FC2D5737647A60833C67B9821FC75C24FADABE2B1F070C27BCB6CD17D2790E12)

1. After the envelope dump, ProcessEvent takes the pointer at env+0x18 (the typed event instance), dumps ITS 64 bytes as `payloadobj@0x... bytes=`, RTTI-types its qword slots (`... obj+0xN at 0x... rtti='...'`), and runs TryQueueFromHandle over its u32 offsets 0..60 (`... obj+0xN handle=... -> form=...` on success).
2. Labels: envelope lines now use `env+0x..`, object lines `obj+0x..`.

### Test 6r-3j next run must show

- `payloadobj@0x... bytes=` line right after each `fired bytes=` - the REAL event fields (look for: a u32 that resolves as handle `obj+0x.. handle=0x... -> form=...`; a TESBoundObject/REFR pointer `obj+0x.. rtti='.?AVTES...` to read formID from next; BSFixedString model path; AS3 arg arrays).
- `queued form=... for probe` -> `=== viewing ... ===` + the full 6q answer set (iCands=0 expected, still diagnostic-only).
- If obj slots are all non-pointer scalars: the u32 at the resolving offset IS the item handle. If nothing resolves: read `InventoryMenu.as` EVENT_LOAD_3D_MODEL field order and map to obj offsets.
- **Ask user to CLICK items too** - SelectItem (event with `uItemHandle` per AS3) never fired in 6r-3i.
### Test 6r-3j RESULT (build 66-payloadobj, session 12:40-12:42) - statics init too late; retries needed

- Registration timing regressed this session: attempt 1 `matched=30` (cluster not yet typed), capture, attempt 2 (late) `matched=32` - ONLY HideModel EB90 + LoadModel EBC0 had become RTTI-typed by then (menu-open code path touches those two). **SetMouseOverModel EA70 and SelectItem EB60 were still uninitialized at attempt 2** -> `still missing select=false load=true paper=true mouse=false` and NO attempt 3 (lateInst one-shot consumed) -> user viewing GREEN CUP (3D preview + detail card works, screenshot verified) produced ZERO events: hover needs mouse (unregistered), LoadModel registered but didn't fire (no model reload), SelectItem unregistered.
- Conclusion: cluster statics at 0x7FF7C246EA10-EBC0 initialize LAZILY on first use of each event type (LoadModel+HideModel on menu init; MouseOver on first hover; SelectItem on first select) - attempts must RESCAN periodically while flags are missing, not stop after 2.

### Test 6r-3k build 67-retry (deployed SHA 7D7BAE61350572CF92A8E2B3D7B2914A5460290C501D1E641C8933A2CCBB1B7C)

1. Periodic retry: while menu open && !g_allRegistered && attempts<32, re-scan every 1000ms (`due` via GetTickCount64, `g_lastAttemptTick` reset on close) - catches statics as soon as they initialize (they stay initialized once touched). Attempt line tags: `, late menu-instance capture` / `, rescan for late .data statics`.
2. Log spam gated: calibration lines once (`g_calibLogged`), match/non-source/untracked lines once (`g_matchLogDone`), menu-member dump once per open (`g_invScanned` gate inside TryRegisterSinks now skips re-scan). Per-attempt `rtti scan: typed=N matched=M` STILL logs each attempt (shows cluster growth) + registration lines always.
3. WARN moved out of attempt block: fires ONCE at attempt 32 (`g_regWarned`); attempts reset to 0 on menu close.

### Test 6r-3k next run must show

- Retries: `registering sinks (attempt 3, rescan for late .data statics)` etc. until `typed=` grows to include EA70/EB60 -> `InventoryMenu_SetMouseOverModel sink registered` + `InventoryMenu_SelectItem sink registered` (possibly attempt 3-6, ~1-3s after first hover).
- THEN on hover: `SetMouseOverModel fired bytes=` (64 bytes envelope) + `payloadobj@0x... bytes=` + `obj+0xN rtti=` / `obj+0xN handle=... -> form=...`.
- CLICK items for SelectItem. Success = `queued form=... for probe` -> `=== viewing ... ===`.
### Test 6r-3k RESULT (build 67-retry, session 12:53-13:00) - REGISTRATION SOLVED; payload still handle-less

- Retry loop worked exactly as designed: attempt 1 (menu open edge, matched=30) registered Barter control + PaperDollTryOn, WARN `still missing select=false load=false paper=true mouse=false`; attempt 2 (+150ms, late menu-instance capture, matched=32) registered LoadModel EBC0; attempt 3 (+1s rescan, matched=40 once first hover touched the cluster) registered **SetMouseOverModel EA70 + SelectItem EB60** - ALL FIVE inventory sinks live ~1.4s after open, no WARN after attempt 3, no cap hit.
- Fire behavior: LoadModel spams (~70 fires, 3D preview/category churn), SetMouseOverModel fires on every hover (~35), PaperDollTryOn 6x on try-on, **SelectItem 0 fires - user hovered but never CLICKED an item** (menu closed twice at 12:58:09/12:58:42 with `unique items probed this session=0`).
- payloadobj proven **BYTE-IDENTICAL to fired envelope first 48 bytes** -> envelope == event object (env+0x18 is a SELF-pointer; obj@0x8467F39E70 with obj+0x18=0x8467F39E70). RTTI chain: obj+0x18 value = obj itself -> vptr at obj+0x00 = per-event vtable (load 0x7FF7C0ED2FA8 / mouse 0x7FF7C0ED3460 / paper 0x7FF7C0ED3560) -> `'.?AVInventoryMenu_<Event>@@'`.
- Event object layout (>=0x40 seen): +0x00 vptr (.rdata), +0x08 BSGFxObject* (heap, = obj+0x70 in same allocation), +0x10 .rdata ptr (load 0x7FF7C0ED2978 / mouse 0x7FF7C0ED2A70 / paper 0x7FF7C0ED2A30 - untyped, STRING? read next), +0x18 self*, +0x20 EventSource*, +0x28 = 2, +0x30 heap ptr = obj+0xC0 (same allocation tail, untyped), +0x38 .text fn 0x7FF7BE7B15AE (CONSTANT across events/instances).
- Handles: obj+0..+60 and env+0..+44 ALL failed to resolve (zero `handle=` lines across 112 fires) -> uItemHandle is beyond +0x3C, in the +0x30 tail object, or AS3-side only.
### Test 6r-3l build 68-payloadwide (deployed SHA 82A820250FD977206C5A886822075004D417C0F1D5BAD1C5DE8260661D29EE1D)

1. payloadobj dump widened 64 -> 256 bytes (`payloadobj@0x... bytes=` now 0x100 long) - covers the whole same-allocation block including the +0x30 tail object at obj+0xC0.
2. Handle probes widened: TryQueueFromHandle over obj u32 offsets 0..252 (was 0..60), env unchanged (0..44).
3. New slot reads per qword of the wide dump: `TryStrAt` - if slot points into .rdata, reads a NUL-bounded printable string (>=3 chars) -> `obj+0xN str='...'` (catches +0x10 event name / model path / BSFixedString content). Slots pointing into .text ([imgB, rdataB)) -> `obj+0xN fn=0x...` (identifies +0x38 etc; offline: rva vs IDA).
4. EventSink/registration/retry logic unchanged from build 67 (all five sinks still register via 1s rescan).

### Test 6r-3l next run must show

- Wide `payloadobj@0x... bytes=` (256 hex bytes) after each fire; new `obj+0xN str='...'` and `obj+0xN fn=0x...` lines; handle probes to +0xFC -> `obj+0x.. handle=0x... -> form=...` if the uItemHandle sits anywhere in the block.
- **CLICK items (left-click, not just hover)** - SelectItem must fire (`InventoryMenu_SelectItem fired bytes=`) since it is now registered; its payload is the most likely to carry uItemHandle per InventoryMenu.as line 1209.
- Success chain: `queued form=... for probe` -> `=== viewing ... ===` + full 6q answer set.
- If still no handle: compare `str='...'` + `fn=` values, then walk the BSGFxObject at obj+0x08 (CommonLib layout) or fall back to the AS3 listener bridge (InventoryMenu.as EVENT_LOAD_3D_MODEL uItemHandle, line 1240).
### Test 6r-3l RESULT (build 68-payloadwide, sessions 16:24 + 19:59) - event object decoded; AS3 Event inside

- 16:24 session: registration only (all five sinks by attempt 3, ~1.4s), user quit before interacting -> zero fires. 19:59 session: wide dump live; PaperDollTryOn fired 4x, mouse/load/select STILL 0 (user in try-on flow, not hovering/clicking inventory grid).
- payloadobj 256-byte layout (PaperDollTryOn @0xB61E0F9E60; per-session base differs): +0x00 vptr (per-event .rdata vtable), +0x08 BSGFxObject* (= obj+0x70/0x80 same allocation), +0x10 .rdata (not a printable string), +0x18 self*, +0x20 EventSource*, +0x28 = 2, +0x30/+0x40 heap (obj+0xC0/+0x60), +0x38 .text fn (0x...B5515AE constant), +0x48/+0x60 AS3-heap ptrs (0x28D3FF9F820, 0x28087D5B2D0), +0x68/+0xB8/+0xF8 more .text fns, +0x70 .rdata vptr-ish, +0x88 '.?AVObjectInterface@Value@GFx@Scaleform@@' (same across fires = singleton), +0x90 {u32 0x48, u32 0xB6}, +0x98 **'.?AVEvent@fl_events@Instances@AS3@GFx@Scaleform@@'** = the AS3 Event instance (heap 0x28D40433680), +0xA0 double 1.0, +0xE0 AS3 MovieRoot, +0xE8 AS3 UserDefinedFunction (closure fn).
- Handles: u32 probes 0..0xFC on the wide obj - ZERO resolve (0x48/0xB6/0xC0/0x28D etc are not inventory handles). No str= lines (rdata slot +0x10 not printable; heap strings not yet attempted - build 68 TryStrAt was rdata-only).

### Test 6r-3m build 69-hop2 (deployed SHA ACCB8B99DD06D5EB92C8AA16BEE5DC0DD1018FFC4A4BEF1F9920F5F38D021A2A)

1. Second hop: obj slots that RTTI-type as BSGFxObject or *AS3* (skip ObjectInterface singletons, cap 4 per fire) get a 256-byte dump: `hop@0x.. rtti='..' bytes=` (hex line once per address via g_hopDumped set; typing/handles/strings every fire).
2. Hop2 slots typed as `obj+0xN/h2+0xM rtti=`, string-read, fn-logged, handles probed u32 0..0xFC with region `obj+0xN/h2`.
3. TryStrAt now ALSO reads heap pointers (>=4GB non-image): ASCII NUL-bounded >=4 printable chars, else UTF-16LE >=4 chars -> `str='...'` on obj and hop2 slots.

### Test 6r-3m next run must show

- `hop@0x.. rtti='.?AVEvent@fl_events...' bytes=` (AS3 Event internals) + `hop@0x.. rtti='.?AVBSGFxObject@@' bytes=` (GFx wrapper + its Values).
- `obj+0xN/h2+0xM rtti=` / `str='...'` (model path / item name / event name) / `handle=0x.. -> form=` -> `queued form=` -> `=== viewing ===`.
- **MOUSE/LOAD/SELECT STILL NEVER FIRED**: user must HOVER inventory grid items (SetMouseOverModel) and LEFT-CLICK one (SelectItem = the event most likely carrying uItemHandle per InventoryMenu.as:1209). Try-on alone is not enough.

### Test 6r-3n build 70-getmember (deployed SHA 98C86835B8D9FB3608243AB79A60E73B151C709B8E88E77A172A1A0EA0CC686D)

1. Build-69 RESULT context: all five sinks register every session; str= reads the event-name string at obj+0x48; hop dumps decode BSGFxObject + AS3 Event instances; ZERO raw u32 handles in obj or hop windows across ~93 fires. GFx Value layout solved from the dumps: the event object embeds a GFx::Value at +0x78 (ObjectInterface* at +0x88, _type at +0x90 = 0x48 = kObject|kManaged, _value at +0x98 = the AS3 CustomEvent instance).
2. AS3 delivery chain (InventoryMenu.as + BSUIDataManager): dispatchCustomEvent(name, params) constructs "new CustomEvent(name, params)"; CustomEvent extends Event with "public var params:Object"; params = {uItemHandle: selectedEntry.uHandleID} (SelectItem :1209, TryOn :1236, LoadModel :1240). The handle therefore lives as an AS3 member, not as a raw u32 in the native struct - that is why every numeric probe so far was silent.
3. New in build 70: ProcessEvent captures obj+0x88 (ObjectInterface), obj+0x98 (AS3 Event) and up to 4 Instances@AS3 candidates (obj + hop2 slots), then calls Scaleform ObjectInterface::GetMember (REL ID 169748, CommonLib RE/S/ScaleformGFxValue.h) under SEH:
   - primary: event -> GetMember("params") -> GetMember(data, "uItemHandle");
   - fallbacks: direct "uItemHandle" on the event, then on each candidate;
   - results decoded as kInt (i32), kUInt (u32), kNumber (double -> u32), kString (logged text); managed Values released via ObjectRelease.
4. Expected log lines: "gfm params type=0x.. data=0x..", "gfm via=params uItemHandle=### (0x..) type=0x.." then "handle=0x.. -> form=0x.." -> "queued form=0x.. for probe" -> "=== viewing ===". Misses show as "gfm miss params on event=..." / "gfm miss uItemHandle ..." / "gfm params not an object".
5. User must HOVER grid items AND LEFT-CLICK one (SelectItem) this run - SelectItem still has never fired in any session; try-on alone does not produce a selectedEntry.uHandleID.

### Test 6r-3o RESULT (build 70-getmember, live 10:01) - HANDLE CHAIN SOLVED; NativeModel exposed

- All five sinks registered (attempt 2, ~1.7s). One PaperDollTryOn fired: gfm params type=0x48 data=0x156BBE9DB40 -> gfm via=params uItemHandle=4207674169 (0xFACC0339) type=0x4 (kUInt) -> handle=0xFACC0339 -> form=0x0010A25D Spacesuit_UC_XenoSpecialist_Backpack_UC08_Reward -> queued form=0x0010A25D -> === viewing ===. Direct event getmember missed as expected; a hop candidate also held the same handle (dedupe absorbed it). SelectItem/mouse/load did not fire (try-on-only session).
- Probe ran but extraction failed: MODEL PATH = UC AntiXeno (fullName at form+0xF0 committed before validation -> short-circuited every later candidate AND the heap scan), root clone geometry leaf with garbage name at node 1, tri=0 and no mesh data found in +0x130..+0x800 - the ROOT NiNode was misclassified as geometry so the scanner chewed node fields (vert cand +0x658=16398 was child-array junk) instead of descending to the real children.
- Live dump solved the layout: children at +0x130 = embedded NiTObjectArray with VPTR {vptr@+0x130 -> RTTI NiTObjectArray<NiPointer<NiAVObject>>, data@+0x138 = child ptr buffer (children[0]=0x152B21582A0), u16 size@+0x140 = 3, u16 cap@+0x142 = 3}. CommonLib NiNode.h claims BSTArray {u32 size, u32 cap, ptr data} at +0x130 - STALE, do not trust it. NiObjectNET::name at +0x10 confirmed by CommonLib static_assert.

### Test 6r-3p build 71-ninodewalk (deployed SHA 6117624E444D903334588B3E0725C38D017BDBB122E3F9727D13DF2477D8ADCA)

1. LooksLikeNiNode/ChildrenData/ChildrenCount: RTTI-gated vptr layout (data@+0x138, size u16@+0x140, cap u16@+0x142, first child must be an AV-class RTTI object), legacy heuristic kept as fallback.
2. GetNodeName reads the inline entry string (len@+8, flags@+0x14, str@+0x18, printable-ASCII validated) - the old code followed the entry _left pointer and produced garbage names.
3. ScanForModelPath commits a candidate only on LooksLikeModelPath/HasNifExtension (temp string), so fullName/keyword strings no longer block the real path search.
4. WalkTree: NiNode and geometry leaf log lines carry the RTTI type name; meshes with empty indices are not pushed (geometry leaf EMPTY (pos= idx=) instead of a misleading 1 meshes).
5. Next run must show: a NiNode line with children=N and rti=.?AVNiNode@@ (or similar), geometry leaf lines with real BSGeometry rti, a real model path (FOUND MODEL PATH ... meshes\...) or no model path found, and hopefully a mesh-from line with pos/uv/idx and tri>0 inside === viewing ===. Hover AND LEFT-CLICK items too (SelectItem still never fired).

### Test 6r-3q build 72-armorload (deployed SHA B65CB61F5ED57CD26C9246EEFABC7CF830B0F63932564DD8E213D2194C82F6FE)

1. ESM walk (esm_walk.py, sequential, adaptive 20/24 header, zlib flag 00040000) proved the ARMO record: TARGET 0010A25D has exactly one model string, MOD2 = Clothes\spacesuit_ucmarine\UCMarine_Backpack_GO.nif, plus INDX 0000 + MODL u32=00257800 (link to ARMA 00257800) and RNAM u32=0000347D (race). Sibling ARMO 000EF9AC has the same MOD2; suit 0007B2B9 MOD2/MOD4 = SetDressing\SpaceSuitContainer_Green.nif. ARMA 00257800 itself was skipped by the resync (5 records dumped) - worn mesh taken from the folder listing (UCMarine_Backpack_m.nif).
2. RACE 0000347D (63042 bytes) carries ANAM = Actors\Human\CharacterAssets\skeleton.nif (+ female variant, NAM5 rig paths). Verdict: Clone3D on an ARMO builds the worn-context RACE skeleton and never reads form+0x1A8 - the build-71 clone (203 nodes, HumanExportRoot with camera/bumper/controller, zero BSGeometry, 123 bone leaves) matches skeleton.nif, while GO.nif extracted from Meshes01.ba2 has BSGeometry x2.
3. LoadFromForm_Clone3D additions for the non-dummy direct branch with no geometry: (1) formstr map - every readable BSFixedString at form+0x100..0x378 logged as formstr form+0xNNN; (2) stage A - the same Clone3D with nullptr requester, logged null requester Clone3D nodes geo tri, wins as meshes via null requester; (3) stage B - FindHelperWeapon (content-driven BSTArray scan 0x0E0..0x1510 step 18, same proven heuristic as RefreshRuntimeFormArrays because CommonLib formArrays offsets are stale) picks any kWEAP, its +0x100 model string is swapped per candidate path (caller assembly paths first, then the scanned form path), Clone3D, restore - the exact dummy-weapon loader proven for guns; first path with geometry wins so worn and world pieces never double-add; wins as meshes via helper swap.
4. Empty after both stages logs candidate count or helper WEAP not found. SelectItem still never fired - LEFT-CLICK a grid item this run plus hover plus try-on; weapon rows are the control (they must keep extracting on the direct stage)

### Test 6r-3r build 73-geomesh (deployed SHA 6F17C5DECA6CE99217DF3D177A445D5418D9928C648B34FC03800C6FD477DEEB) - geometry from the external .mesh stream (stage A2)

1. Root cause locked: Starfield stores vertex/index data OUTSIDE the NIF. Each BSGeo LOD entry is a 41-byte GUID key (raw hex\hex, backslash) resolved by LowerAscii (lowercase AND backslash -> forward-slash, LiveModel.cpp:76) to geometries/<guid>.mesh - a separate archive stream: 'Starfield - Meshes01.ba2' name table holds 'geometries/5c3ff5c00818d2ac6057/8ed75bc587841837ea66.mesh' for the UC backpack (geos: 40098v/13600i and 336v/128i, counts matching the runtime probe fields exactly). The engine loads these through MeshLODDB stream entries (BSGeo +0x1C8/+0x3C8), which is why the buffer probe reads candidates (vCands=20, iCands=4, counts 40098/13600/336) but can never pair CPU arrays: those bytes are not on the cloned object at all.
2. Weapon NIFs use the identical key scheme (rattler.nif: 20 LOD keys, all backslash) - the same path BuildSingleNif already validated byte-for-byte against MeshConverter ground truth. ResolveAsset's BA2 exact-match find works because LowerAscii rewrites the separator before lookup.
3. Shipped stage A2: when Clone3D meshes are empty and the form path is real (non-dummy), NativeModel calls the new ungated SFE::LiveModel::ParseNifMeshes(modelPath) - LoadConfig + NormalizeNifKey + BuildSingleNif only (no [livemodel] enabled gate, no cache write, no sibling assembly) - converts LiveMesh -> NativeMesh (name/matPath/pos/uv/indices), computes center/radius from file bounds, message 'N meshes (geometry files)'. Stage B (helper swap) now runs only if the file parse fails. LiveModel.h carries the API contract comment.
4. Deployed SHA256 6F17C5DECA6CE99217DF3D177A445D5418D9928C648B34FC03800C6FD477DEEB; DLL strings 'build 73-geomesh', 'geometry files:', 'ParseNifMeshes failed' verified. Test: relaunch, view UC/Miner backpack - expect 'geometry files: N meshes, T tris from ...' in the log and tri>0 in the preview message. If 'ParseNifMeshes failed' appears instead, grep the paired WARN from BuildSingleNif (asset not found / NIF parse failed / no usable geometry).

### Test 6r-3r RESULT (build 73-geomesh, live 11:29-11:34, 2419 lines, 1 warn 0 err) - GEOMETRY EXTRACTED, tri>0 FIRST TIME

1. Four loads with real meshes: UC backpack 2 meshes/13478 tri (geometry files), Miner backpack 1/16245 (geometry files), Miner helmet 1/16245 (null requester - the walk extracted this time where it was empty for both backpacks, unexplained; buffers may become pairable once the engine streams that family's .mesh), SpaceSuitContainer01 2/4964 (geometry files). Geo leaf detail lines match NIF advisory counts exactly: UCMarine_Backpack_GO:0 pos=40800 uv=27200 idx=40098 and :1 384/256/336; SSMiner_Backpack_GO:0 32691/21794/48735; container SpaceSuitContainer01_Mesh:0 10380/6920/14868 + Object17457:0 27/18/24.
2. Texture path is live too: the first load indexed every texture BA2 (FindDiffuse/ReadTexture) between the null-requester walk and the geometry-files result (~1.2s including archive index; later loads ~180ms each).
3. result center/radius is log-only - OfflineModel recomputes bounds from mesh pos (center/radius with 1e-4 guard, camDist = radius*5), so the helmet's r=0.0 log cannot affect framing. nodes=203 geo=1 in result lines are stale base-walk counts when stage A2 adopts (cosmetic, fold into next build).
4. SelectItem still never fires; PaperDollTryOn carries every load (uItemHandle 0xFACC0339 -> form 0x0005278E, dedup 'already probed this session, skip' works). Lone WARN is the known select=false registration line. Next: visual verdict on materials/textures/orientation, weapon-row control check, multi-part outfit assembly.

### Test 6r-3s RESULT (build 73 crash session, live 11:45:20-11:46:56, 7611 lines) + build 74-meshguard - CTD replicated in shop hover

1. Sequence: inventory opened clean; shop hover ran native vendor preview for UnarmedHuman (0 meshes), CreatureRangedAttack (0), Microgun (dummy base, 6 assembly paths, ALL parts walk-empty), RescueAxe (GEOMETRY FILES 2 meshes 8414 tris - shop rendering proven before the crash), Razorback (dummy, 5 paths, all empty), Regulator (dummy, 4 paths, all empty), MagShot - and MagShot.nif walk ADOPTED GARBAGE: the ':346' leaf rti=.?AVNiCamera@@ was treated as geometry, ProbeGeometryNode pair-accepted pos=196632 uv=0 idx=65544 from adjacent memory -> '1 meshes from 3 path(s)' -> 'direct-live MagShot -> ready (1 mesh, 21848 tris)' at 11:46:56.927 = THE LAST LINE. Both threads silent after it.
2. Crash forensics: NO WER event (Application+System empty 11:43+, WerSvc Stopped/Manual never triggered), NO dump, no System/TDR events -> no Windows-visible exception: either hang-then-kill or a handled clean exit (user asked, hang-vs-instant answer pending). Outstanding 26/09 10:23:50 crash DID report: WER 1001 bucket P7=c0000005 P8=00000000028cb2e2 (build-58 stale-ID signature family), no dump that day. HKCU LocalDumps\Starfield.exe (DumpType=1, DumpCount=10, CrashDumps folder) was added THIS session - next real exception will capture a dump. Devilz ESM Shop is DISABLED in Test profile (probe not involved); SFDevilzDump/StarPatcher ran benign (dump wrote 11:46:12, 216445 forms).
3. Build 74-meshguard: (a) WalkTree RTTI gate - only rti containing BSGeometry is probed, everything else logs non-geometry leaf '...' rti='...' - skipped (kills the NiCamera garbage class); (b) index-range validation at adoption - idxMax >= vertCount drops the mesh with a mesh dropped log; (c) dummy-branch fallback - each non-dummy assembly path that walk-fails now runs the shared AdoptFileMeshes (ParseNifMeshes) so Microgun/Razorback/Regulator/MagShot parts get real geometry files meshes; (d) shared AdoptFileMeshes + ComputeMeshBounds helpers, honest geoCount on Stage A2 adopt, radius fallback for null-requester adopt (helmet r=0.0 fixed).
4. Deployed SHA256 DFD2A09399432351B3D5009D2C9FC808F41EE13F3397B06D63EC9E816D71F904; strings build 74-meshguard / non-geometry leaf / mesh dropped / geometry files: verified. Next run: inventory control first, then shop hover over MagShot + dummy weapons - expect per-part geometry files lines, non-geometry leaf skipped for cameras, NO crash; if it still dies, HKCU LocalDumps should now produce a dump (do not relaunch before the dump/log are collected).
### Test 6r-3t RESULT (build 74-meshguard, live 12:07-12:13, log 119326 lines, 55 warn 0 err) - SHOP PASSES, NO CRASH

1. No CTD: user hovered the vendor grid (inventory first, then shop) and closed the game manually (log last write 12:13:30). The RTTI guard fired on EVERY dispatch - non-geometry leaf ':346' at node 201 rti=.?AVNiCamera@@ - skipped, the exact adoption that was the last line of the 6r-3s crash session. Zero other non-geometry leaf classes appeared; mesh dropped count=0 (index-range validation never needed to fire - every geometry file passed). Root cause of the 6r-3s crash is confirmed fixed by 74a.
2. Dummy-branch file fallback (74c) proven end to end: MagShot = 3-part OMOD assembly, every part loaded via AdoptFileMeshes - 33+1+2 = 36 meshes, 11312+50+1508 = 12870 tris -> direct-live 'MagShot' -> ready (36 mesh(es), 12870 tris); user screenshot shows the assembled pistol rendering and rotating in the shop preview (yaw +40 pitch +20 zoom x2.05). Drum Beat likewise 18+1+5 = 24 meshes / 33071 tris. Before 74 every dummy weapon was walk-empty (0 meshes).
3. Warnings (55 total, 0 err), all benign: 52x offline GPU banned (CPU fallback for this model only) - every one is a 0-candidate record (UnarmedTerrormorph_*, UnarmedHopperA_*, UnarmedQuadrupedC_*, creature-attack pseudo-weapons) with msg 'no geometry after null requester and helper swap (0 candidate path(s))' = the record carries no model path at all, correct emptiness not a loader failure; 2x live NIF parse failed 'stage notypes' (lawgiver/regulator_standardbarrel - skeleton-only NIFs containing just NiNode/extra data/BSConnectPoint, no geometry block types); 1x known select=false probe line. Everything else INFO.
4. Cosmetic leftovers for the next build: the per-part line still logs the swap result ('part ... loaded=true extracted=false meshes=0 tri=0') BEFORE the file fallback adopts (its 'geometry files:' line follows immediately) - harmless but confusing; 74d radius/geoCount honesty verified on the null-requester adopt. SelectItem STILL never fired in any session - PaperDollTryOn remains the proven dispatch path.
5. Next: visual verdict pass (materials/textures/orientation) in inventory + shop now that geometry is stable, multi-part outfit assembly for worn ARMO, then the deferred queue (clothes/body/items docs, ENB findings doc, CommonLibSF corrections).

## 6r-3u RESULT -- build 75-outfit (multi-part outfit assembly), deployed 27/09/2026 13:00
- SHA256 19ABF80E1C334AC08E50309A4F5337DECA6FAE906B034FE0805B9D7F814B980E, 3018240 bytes, tag 'build 75-outfit' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Binary strings verified present: 'build 75-outfit', 'proxy world ', 'replaced by ', 'addon path(s)', 'meshes from ', 'path(s) (geometry files)', 'no geometry from ', 'candidate path(s)', 'part walk ', 'swapMeshes=', 'setdressing\'; legacy 'build 74-meshguard' gone.
- WHY (ESM ground truth, armor_data.json merged two-pass walk): 484 clothes/suit ARMOs carry a SetDressing PROXY world model (SetDressing\OutfitContainer01.nif / SpaceSuitContainer01.nif / SpaceSuitContainer_Green.nif = a dressing box). The real garment lives on the ARMA addon links (2248 resolved MODL links, formid range 0<v<0x01000000; 437 ARMOs have >1 addon). Clothes = 3 parts (UpperBody+LowerBody+Sleeves), spacesuits up to 9. FormModelPaths already collected those worn paths (ARMA bipedModel[FEMALE].model, male only if female empty) but Stage A2 adopted ONLY the scanned proxy world string, so the shop preview rendered the dressing box instead of the garment.
- CHANGED (NativeModel.cpp, non-dummy branch):
  1. New IsProxyModelPath() (lowercase contains 'setdressing\' AND 'container'), placed next to IsDummyModelPath (~line 1692).
  2. Stage A2 rewrite (~line 2118): if the scanned world model is a PROXY and caller addon paths exist -> clear any walk-cloned box meshes (info line "proxy world '<path>' (N meshes) replaced by K addon path(s)"), then adopt ALL addon paths merged (case-insensitive dedup, skip empty/dummy/proxy strings). Proxy forms NEVER fall back to the box while addons exist; box fallback only when there are zero addon candidates.
  3. Real (non-proxy) world models keep first-wins: adopt the scanned path alone first (approved GO.nif look, e.g. backpack 0010A25D = UCMarine_Backpack_GO.nif); addon parts only fill in when the world parse yields nothing. Empty world path (creature pseudo-weapons etc.) also tries addon candidates now.
  4. ComputeMeshBounds runs ONCE after the candidate loop; honest message: single path -> "N meshes (geometry files)", multiple -> "N meshes from K path(s) (geometry files)". Per-path logs: "geometry files: X meshes, Y tris from '<p>'" / "geometry files: ParseNifMeshes failed for '<p>'" / "geometry files: no geometry from K candidate path(s)".
  5. Cosmetic (carried from 6r-3t): dummy-branch per-part line renamed "part walk '<p>' loaded=... extracted=... swapMeshes=... swapTri=..." (it reports the Clone3D swap attempt only; the file-fallback 'geometry files:' line follows it).
- NOT CHANGED (deliberate): AssembleSiblings still NOT wired into ParseNifMeshes (FormModelPaths already lists every part of the outfit; sibling auto-merge would double the meshes - deferred edge case). Stage B helper-WEAP swap kept as the last resort only.
- ESM walker knowledge kept for reuse: GRUP header = 24 bytes and the GRUP size field INCLUDES its own header; mode 'a' (GRUP consumed as a record) finds ARMO=1017 but misses ARMA; mode 'b' (if sig==GRUP: pos+=24) finds ARMA=1106 and WEAP=406 but misses ARMO (early false-record jumps); run BOTH walks and merge. Chunked raw scans must not f.seek inside the per-chunk loop (breaks chunk offsets) and must f.seek(0) after seek(0,2). Raw byte hits on 'ARMA' are mostly the string 'TARMAC' - no ARMA record sig false-positives.
- TEST PLAN (build 75): launch, open shop, hover a clothes/space suit that previously showed the OutfitContainer box -> expect the real garment mesh(es) in preview, tag "... (N / M textured)", message "N meshes from K path(s) (geometry files)", one "geometry files: X meshes, Y tris" line per addon part, and an optional "proxy world ... replaced by ..." info line in the log. Re-hover single-path weapons (MagShot / Drum Beat) as regression: message must stay "N meshes (geometry files)" with counts unchanged from build 74 (36/12870 and 24/33071). Then give the texture eyeball verdict (flat vs textured surfaces). SelectItem still never fires without a LEFT-CLICK on a grid item; PaperDollTryOn remains the proven dispatch.
- Build notes: first build failed with C2447 'missing function header' (one stray '}' left after the rewritten Stage A2 block, cascading C2059/C2143 down the file) - removed the extra brace; rebuild OK (4.4s incremental; benign C4189 'left' NativeModel.cpp:1667 unchanged).

## 6r-3v RESULT -- build 76-sanity (CTD root cause + grey-texture root cause), deployed 27/09/2026 13:24
- SHA256 2E52329D85DB38CFF7AE365DD71AA2AB9E25ADA8B7D53ACC46D8A1141BF095E1, 3024896 bytes, tag 'build 76-sanity' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Strings verified: build 76-sanity / insane position / null requester bounds rejected / bounds sanity rejected / diffuse unresolved for / mesh dropped / proxy world; legacy 'build 75-outfit' gone.
- SESSION 75 ANALYSIS (13:06:46-13:08:21, 10334 lines, CTD at end): user hovered weapons only (Razorback = 5 paths -> "54 meshes from 5 path(s)" 45520 tris; MagPulse = 4 paths -> 33/63672; Frag Grenade; Equinox) - NO clothes/suits hovered, so the build-75 PROXY OUTFIT PATH IS STILL UNTESTED (zero 'proxy world' lines). Warns=8 (GPU bans: pseudo-weapons + Equinox). Equinox = 0 meshes + GPU-ban (its LoadFromForm found no model path and no assembly; same family as build-74's 52 zero-candidate bans - capture its 'native preview' msg next session to confirm).
- CTD ROOT CAUSE (fixed in 76): the null-requester Stage A walk (NativeModel.cpp ~2088) adopted PROBE GARBAGE. Two events at 13:08:21: "2 meshes (null requester)" center=(1.5e37,...) r=inf tri=6380, then "3 meshes (null requester)" center=(-1.06e37,...) r=inf tri=36855 (log ends right after - CTD). The build-74 guard checked only RTTI + index range: garbage passed because pos=171348 vertices with idx<verts, and the floats are FINITE (isfinite passes) but absurd (~1e37). No new dump in C:\Users\mrlma\AppData\Local\CrashDumps (25/09 dumps only).
- GREY ROOT CAUSE (fixed in 76): LiveModel::FindMatAlbedo (2014), FindDiffuse (2150) and ReadTexture (1681) ALL gated on `!g_configLoaded || !g_enabled`. The shop runs [livemodel] enabled=0 -> every resolver returned false on entry -> PrefetchTextures silently `continue`d every mesh (that skip had NO log) -> completed with an EMPTY cache but set g_texReady -> TakePrefetched returned true+empty -> log "texturing 'direct:Razorback' (44 ranges, 0 prefetched)" -> every range "UNTEXTURED (no prefetch)" -> items render grey. Evidence: 0 'prefetched' lines, 0 'texture unreadable' warns, 5 'texturing ... 0 prefetched' lines. User verdict: "items are still grey but i think are correct" = GEOMETRY CONFIRMED CORRECT. The directory lists LoadConfig builds are unconditional, and ParseNifMeshes already proves ResolveAsset works at enabled=0 - the texture gate was a blanket copy of the BuildModels gate.
- CHANGED (build 76):
  1. NativeModel.cpp: kMaxCoord=100000.f + SaneBounds(center, radius) helper (finite, |center|<=1e5, 0<radius<=1e5) defined before WalkTree.
  2. WalkTree: per-mesh position check after the index check - any non-finite or |v|>kMaxCoord -> "mesh dropped '<name>' (insane position)".
  3. Null-requester adopt: if !SaneBounds after ReadBounding/ComputeMeshBounds -> log "null requester bounds rejected (center=...) - N mesh(es) discarded", clear meshes/counts, do NOT set extracted/message/g_cachedObj -> Stage A2 (geometry files) then gets its chance (Frag Grenade would fall back to Weapons\FragGrenade\GrenadeFrag.nif, proven earlier that session: 6 meshes/1989 tris).
  4. Requester Stage A: after WalkTree, recompute bounds when !SaneBounds (stored NIF bounding can be garbage while positions are fine).
  5. LoadFromForm wrapper = final safety net for ALL paths: !SaneBounds -> ComputeMeshBounds once, still insane -> clear + message 'bounds sanity rejected'. Garbage can never reach the preview renderer.
  6. LiveModel.cpp: ReadTexture/FindMatAlbedo/FindDiffuse gates reduced to !g_configLoaded only (comments explain: pure file IO, needed by offline prefetch at enabled=0). BuildModels/ReadAsset/DiscoverNif KEEP the enabled gate (live-build paths).
  7. OfflineModel.cpp: the silent diffuse skip now logs WARN "diffuse unresolved for <mat> (<node>)" so an all-empty prefetch is diagnosable next time.
- TEST PLAN (build 76): 1. Hover guns repeatedly (Razorback, Frag Grenade - the CTD trigger) -> expect 'prefetched' INFO lines, "texturing ... N prefetched" with N>0, colored/textured preview, and either a sane null-requester adopt or a "bounds rejected" line instead of r=inf. 2. HOVER A SUIT/CLOTHES - the build-75 proxy outfit test is still pending: expect "proxy world ... replaced by K addon path(s)" + "N meshes from K path(s) (geometry files)" and the real garment, not the SetDressing box. 3. Confirm grey is gone (texture eyeball verdict). 4. Note Equinox's 'native preview' msg line. Report any CTD with the time so the log tail can be cut before relaunch.
## 6r-3w RESULT -- build 77-weapons (family-A keyword gate + B1 factory normalize), deployed 27/09/2026 16:15
- SHA256 79025F634F425EF8782185D83976B8D043ED353BC7229106BA58AD2F0FE94DEE, 3030528 bytes, tag 'build 77-weapons' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Strings verified: build 77-weapons / omod probe / family ( / geometries/; legacy 'build 76-sanity' gone.
- FAMILY A ROOT CAUSE (proven): BGSMod::Attachment::Mod has NO BGSEditorID member (TESObjectWEAP has one at 0x238, BGSKeyword at 0x38) so form->GetFormEditorID() returns null for every OMOD at runtime - the folded identity (ShopExplorer.cpp ~879) carries only display name + model basename, and those words miss most guns (Eon family=1, Grendel family=1). sim_assembly.py noEDID variant reproduced the observed family counts for all 10 weapons exactly.
- FAMILY CARRIER: OMOD MNAM = the game's own mod-association keyword ma_<Weapon> (census probe_mnam.py: 1627/2682 OMODs carry MNAM, exactly 1 distinct MNAM per family, 0 multi-MNAM; carriers = that weapon plus crew/UI variants; WEAP KWDA lists NO ap_* slot keywords, so the membership test cannot leak across slot layouts). MNAM runtime member unknown (attachParents @0xA0 vs attachPoint @0xD8) - the gate reads BOTH.
- CHANGED (ShopExplorer.cpp CollectWeaponAssemblyNifs):
  1. Part gains bool kwKin; per owner-OMOD compute kwKin = weapon KWDA contains attachPoint formID OR any attachParents formID (formID compare over weapKW->keywords, not pointer compare). Counters nAp/nPar land in the log.
  2. Model dedup now UNIONS kwKin: OLD_mod_Stinger_Receiver_Standard (stinger.nif, no MNAM) wins first-by-fid dedup over mod_Eon_Receiver_SemiAuto_Standard (ma_Eon); without the union the Eon receiver was silently dropped and anchor came out empty.
  3. Family gate: kin = token-kin OR p.kwKin (union, never AND); token path unchanged, keyword path adds.
  4. Anchor tie-break: receiver votes, then family part presence in dir, then lexicographic. Kills the 1-1 flips MagPulse->magstorm and EquiNOX->pirate (offline sim picked a random winner per unordered_map iteration order).
  5. Subdir filter (~line 1004): old compare used anchor + "/" while dirs are backslash and never matched; now accepts both separators so anchor subdirs join the assembly again.
  6. Log reads "... N family (K kw, ap A par P), anchor 'dir'" plus ONE-TIME "omod probe fid=.. ap=.. par0=.. c0=.. c8=.. d0=.." (raw unkC0/unkC8/unkD0 words, no deref) on the first OMOD with any keyword-ish field; par0!=0 -> MNAM sits in attachParents, ap!=0 -> attachPoint, only c0/c8/d0 nonzero -> KWDA-style vector there.
- EXPECTED (offline kin2, exact weapon match): Grendel 1 -> 17 parts, anchor weapons\bullpup, FULL slots; Equinox 2 -> 10, anchor weapons\laser_rifle (presence 8 vs 2 over pirate); Eon 10 with receiver restored, anchor weapons\stingerpistol; MagPulse 8 but anchor stays weapons\magpulse (presence 7 vs 1 over magstorm); Rattler 10, Tombstone 13; Microgun/Razorback/MagShot/Solstice unchanged counts.
- CHANGED (LiveModel.cpp BuildSingleNif ~1412, family B1): factory -> mkey normalization: lowercase, backslash->slash, strip trailing '.mesh', strip leading 'geometries/', then "geometries/" + factory + ".mesh". If the transform changed the key and the normalized lookup misses, retry the raw legacy key; vanilla factories (bare guid/guid) normalize to themselves, so their single lookup stays byte-identical to build 76. Root cause was BubBow's plugin-authored factory string 'BubBow\bbow_body.mesh' (mixed separators + .mesh suffix) producing mkey 'geometries/bubbow\bbow_body.mesh.mesh' against BA2 entry 'geometries/bubbow/bbow_body.mesh'.
- B2 (ammo crates / missiles / fatman): NO CODE FIX - SetDressing\ammo_crates\type{a,b,d,f}_xlarge.nif and ammofatman.nif are absent from all 2967962 ba2 name-table entries AND the loose dirs; ammo777mm.nif resolves and renders, so the resolver is correct. Honest missing content.
- Build notes: two truncated attempts first (tool timeout killed the batch mid-compile; xmake cache timestamps showed config ran, compiles were incremental), final run build ok in 402.984s; only benign C4244 warnings. run_build.bat stalls have NOTHING to do with the code - give the batch 8+ minutes.
- TEST PLAN (build 77): 1. Hover Grendel, Equinox, Eon, MagPulse -> expect assembly lines with 4-7 parts, anchor at the expected dir above, and the new "(K kw, ap A par P)" counts (A+P > 0 for every one). 2. Read the FIRST 'omod probe' line and report it verbatim (ap/par0/c0/c8/d0 reveal the MNAM member). 3. Talon IV Crossbow (BubBow) -> geometry from geometries/bubbow/bbow_body.mesh instead of 'no geometry'. 4. Rattler/Tombstone regression: family/anchor unchanged vs build 76. 5. Ammo crates/missile racks still empty - expected, see B2. 6. Left-click grid items; report CTDs with time.
## 6r-3x RESULT -- build 78-ctdguard (ammo CTD guard + MNAM probe v2), deployed 27/09/2026 17:10
- SHA256 F9FC53D9B86D880F1DC1A1AF313C96BFC61917BEF0C894B16645F7BC09936209, 3033600 bytes, tag 'build 78-ctdguard' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Strings verified: build 78-ctdguard / omod probe / omod mem / mnamPtr / probe caught SEH / wk; legacy 'build 77-weapons' gone.
- SESSION 77 ANALYSIS (16:24:44-16:31:42, 86332 lines): 11 weapons hovered. Token-gated families unchanged and assembling (Razorback 12 anchor razorback asm 5; Regulator 11 asm 4; MagShot 7 asm 3; Rattler 9 anchor heater asm 5; MagPulse 7 anchor magpulse asm 4; Tombstone 12 asm 5; Lawgiver 11 asm 4; MagShear 8 asm 3) but Eon 1 / EquiNOX 2 / Grendel 1, all anchor '' and assembly 0 - the build-77 keyword gate NEVER FIRED: every line printed "(0 kw, ap 0 par 0)". Ammo items .50 Caliber and 7.77mm rendered 3 meshes + 3 prefetched textures.
- ROOT-CAUSE DATA (build-77 probe line + offline ESM decode):
  1. Runtime probe said: `omod probe fid=000079CD ap=0002249C par0=00035799 c0=0 c8=0 d0=0` - OMOD mod_Shotty_Muzzle_MuzzleBrake. Its ESM MNAM = 001F70BB (ma_Shorty): NOT the attachPoint, NOT attachParents[0], NOT any c0/c8/d0 raw word.
  2. OMOD DATA blob holds BOTH members (validated: exact runtime match). Layout after the 'TESObjectWEAP_InstanceData' string: 2 pad bytes + u32 attachPoint + u32 parentCount + parents[]. Shotty: ap=0002249C count=2 parents=[00035799, 0004B6E1]. Eon 00088BCD: ap=00024004 count=8 parents=[0005D4D7, 00022499, 0002249C, 0002EE28, 0001BC46, 0002ECC0, 0002249F, ...] - NO 001F70DD. Equinox 0006D5BA: ap=00024004 count=9 parents=[0002249D, 0002249F, 0014D08A, 00022499, 0005D4D7, 0002EE28, 0001BC46, 0002ECC0, ...] - NO 00297DE0.
  3. Keywords 0002249C / 00035799 appear in the KWDA of ZERO WEAPs (slot/ap constraints) - a formID gate over attachPoint/attachParents can only ever match if MNAM APPENDS to attachParents at runtime (parN test), or the weapon-side store is empty (wk test), or MNAM lives in an undiscovered Mod member (mnamPtr / mnamRaw / omod mem hex test).
  4. Eon census reconfirmed: 27 OMODs MNAM=001F70DD; modgroups/quality have MNAM=none. MNAM is one distinct ma_<Weapon> per family as established in 6r-3w.
- CTD ROOT CAUSE (user: "ctd on ammo in shop"): log ends mid-walk on form 00230559 `SetDressing\ammo_crates\AmmoLaserSmall.nif` (type=49, Clone3D OK, 203-node scene). Leaf 'AmmoLaserSmall:1' ended EMPTY; leaf ':2' deep probe dereferenced unrelated heap objects (RTTI +0x750 -> VPathingCell, +0x760 -> VBGSSpatialGraphExteriorCell) and faulted inside the idx-content scan - the 6th rejIdx line is the last log line. NativeModel.cpp contained NO __try anywhere. BuildSingleNif/ShopExplorer assembly code did NOT run on this path (assembly is WEAP-only; the crash precedes ParseNifMeshes) - NOT a build-77 regression: pre-existing latent AV in ProbeGeometryNode's candidate-pointer content scans, first hit on this particular NIF.
- CHANGED (build 78):
  1. NativeModel.cpp: POD trampoline ProbeGeometryNodeGuarded wraps ProbeGeometryNode in __try/__except (C2712-safe: no unwinding locals in the trampoline). On fault: WARN `geometry leaf '<n>' probe caught SEH 0x<code> - skipped`, partial mesh cleared, walk continues - hovering ammo can no longer CTD.
  2. ShopExplorer.cpp: probe v2 (same first-OMOD trigger) logs fid, ap, parN + par0..2, c0/c8/d0, mnamPtr/mnamRaw (POD __try scan of Mod slots 0x30..0x108 for a pointer whose +0x28 TESForm::formID == 001F70BB, or a raw u32 == 001F70BB), plus `omod mem <hex>` = full member bytes 0x30..0x108 for offline decode.
  3. Family line gains `wk <n>` = weapon keyword count read through BGSKeywordForm::keywords. wk=0 means that store is empty or misoffset at runtime - the other candidate reason ap/par stayed 0 despite a correct gate.
- INTERPRET NEXT LOG: parN==3 && par2=001F70BB -> MNAM appends to attachParents (check wk next); wk>0 && parN==2 -> MNAM not in parents -> read mnamPtr/mnamRaw, else decode omod mem hex against the known ap/par pointer values (formID at +0x28; BGSKeyword slots identified by the ap/par pointers); mnamPtr=+0xE0/E8/F0 -> gate that member in build 79.
- TEST PLAN (build 78): 1. Hover ammo boxes (typea_xlarge etc.) -> expect WARN `probe caught SEH ... - skipped` instead of a CTD; crates stay empty (B2, honest). 2. Hover ANY weapon once -> report the `omod probe` and `omod mem` lines verbatim plus one family line (wk count). 3. Regression: Razorback/Tombstone assemblies unchanged. Left-click grid items; report CTDs with time.
## 6r-3y RESULT -- build 79-famtable (OMOD->MNAM census table family gate), deployed 27/09/2026 18:43
- SHA256 E89D49618DBD22D96072D8EDE70A35515C4152D0C11AB42341B20629E0C0E706, 3047424 bytes (census table adds 13824 over build 78), tag 'build 79-famtable' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Strings verified: build 79-famtable / mnam {}; legacy 'build 78-ctdguard' gone. Generator archived at tools/gen_omod_mnam_table.py.
- SESSION 78 ANALYSIS (18:00:07-18:07:10, 129596 lines): NO CTD (user: "test ran no crash"). AmmoLaserSmall hovered twice, full walk both times, ZERO 'probe caught SEH' lines - the guarded probe did not fault this session (fault is garbage-dependent; the trampoline stays as insurance). 16 weapons hovered; token families assembling unchanged (Microgun asm 6, Razorback 5, Regulator 4, HardTarget 6, Tombstone 5, Lawgiver 4, Rattler 5 ...); Eon 1 / EquiNOX 2 / Grendel 1, all anchor '' - family line still "(0 kw, ap 0 par 0)".
- PROBE VERDICT (decode of the build-78 lines): `omod probe fid=000079CD ap=0002249C parN=2 par0=00035799 par1=0004B6E1 c0=0 c8=0 d0=0 mnamPtr=+0xFFFFFFFF mnamRaw=+0xFFFFFFFF`. `omod mem` = 432 hex chars: attachParents control block sits at +0xB0 ({begin,end,cap} at 0xB0/0xB8/0xC0 = exactly 2 elements), +0xD8 = 0 while runtime ap=0002249C (live Mod layout differs from CommonLib past ~0xB0), +0xE0 heap ptr, +0xF8 name string. MNAM 001F70BB is in NO direct slot: no raw u32 over 0x30..0x108, no pointer member whose +0x28 formID matches - MNAM is not kept as a direct runtime Mod member. Member hunt CLOSED, no more slot probes.
- WEAPON SIDE PROVEN WORKING: wk = 14-18 on every weapon (Eon 16, Microgun 18, MagShot 14, EquiNOX 17, Grendel 17) = BGSKeywordForm::keywords is populated and formID-comparable. Only the OMOD side lacked MNAM.
- SLOT TOKEN DISCOVERY (probe_fam3.py): the OMOD attachPoint KEYWORD IS the slot token - EDIDs: 00024004 ap_gun_receiver, 0002249C ap_gun_Muzzle, 00022499 ap_gun_Scope, 0005D4D7 ap_gun_Mag, 0002249D ap_gun_Barrel, 0001BC46 ap_gun_Cover, 0002EE28 ap_gun_Battery, 0002249F ap_gun_Grip, 00147AFE ap_gun_InternalMod, 000191EE ap_gun_Laser. ShopExplorer appends the attachPoint EDID to folded (line ~936) BEFORE WeaponPartSlot, so stinger.nif and pirate_laserrifle_rev1.nif receivers classify as 'receiver' at runtime (every working assembly line contains receiver+..., proving the append works). Offline sims that lacked this token under-predicted (Eon anchor '' was a sim artifact); runtime slotting is strictly better than the sim.
- CARRIER DATA (probe_fam3.py): ma_Eon 001F70DD = 37 carriers (mod_Stinger_* + OLD_mod_Stinger_Receiver_Base), ma_Equinox 00297DE0 = 30 (incl. OLD/mod_LaserRifle_* rows), ma_Grendel 0005164B = 29; every receiver OMOD carries ap=00024004 ap_gun_receiver; Eon WEAP 000476C4 kw=16, EquiNOX 0001BC4F kw=17, Grendel 00028A02 kw=17 all carry their ma_*. Model-less OMODs (no MODL) are dropped by the model check - correct, they change stats only.
- CHANGED (build 79):
  1. NEW src/OmodMnamTable.h: 1627-entry sorted {omod, mnam} pairs generated from Starfield.esm (ASCII, binary-search LookupOmodMnam). OMODs without MNAM (1055/2682) and every plugin OMOD return 0 -> token gate fallback exactly as before.
  2. ShopExplorer.cpp gate rewritten: onWeaponId(u32) lambda; kwKin = LookupOmodMnam(form->GetFormID()) (new nMnam counter) OR attachPoint formID (nAp) OR attachParents formID (nPar) - the last two kept as plugin-form fallbacks.
  3. Family line: "... N family (K kw, ap A par P mnam M wk W), anchor 'dir'".
- EXPECTED NEXT LOG: Eon family ~10-13 anchor 'weapons\stingerpistol' assembly ~5 {receiver+mag+muzzle+scope+laser}; EquiNOX ~10+ anchor in the laser_rifle dir assembly >= 2; Grendel ~17-20 anchor 'weapons\bullpup' assembly ~6 {receiver+barrel+mag+grip+scope+muzzle}; MagPulse anchor STAYS 'weapons\magpulse' (tie-break presence beats the single magstorm receiver); Rattler ~10, Tombstone ~13; Microgun/Razorback/MagShot/Solstice counts unchanged or +1 (the union only adds).
- TEST PLAN (build 79): 1. Hover Eon, EquiNOX, Grendel -> mnam > 0 in the family line, non-empty anchor, assembly lines with 4-6 parts, real gun mesh in the preview. 2. Regression: MagPulse anchor stays weapons\magpulse; Razorback/Tombstone family+anchor unchanged; Microgun asm 6. 3. Hover ammo boxes again (guard still armed). 4. Left-click grid items; report any CTD with its time.
## 6r-3z RESULT -- build 80-laserfix (slot classification: mesh+attachPoint before display name), deployed 27/09/2026 19:40
- SHA256 0567C7649260977111399821A3A9B79DC59C1124ADFF55049D6287F070306E19, 3047424 bytes, tag 'build 80-laserfix' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Strings verified: build 80-laserfix; legacy 'build 79-famtable' gone.
- SESSION 79 VERDICT (family-A FIXED, no CTD, SEH 0): Eon 1 -> 10 family, anchor 'weapons\stingerpistol', assembly 4 [receiver+muzzle+mag+scope], mnam 22; EquiNOX 2 -> 9, anchor 'weapons\laserrifle', assembly 4 [receiver+barrel+mag+scope], mnam 20; Grendel 1 -> 17, anchor 'weapons\bullpup', assembly 7 [receiver+barrel+muzzle+mag+grip+scope+laser], mnam 25. kw == family on every weapon (table fires 100%, ap 0 par 0 - fallbacks never needed for base game). Regressions clean: MagPulse anchor STAYS 'weapons\magpulse' (fam 8, tie-break beat magstorm), Razorback 12/asm 5, Tombstone 13/asm 5, Microgun 10/asm 6, MagShot 7/asm 3, MagShear 8/asm 3, Lawgiver 12/asm 4, MagSniper 12/asm 5, Beowulf 16/asm 6.
- RESIDUAL (build 80 target): Eon and EquiNOX assemblies lack the laser slot although their families contain the lightlaser parts (fam arithmetic proves it: Eon 10 = receiver 1 + mag 3 + muzzle 3 + scope 3 where scope 3 = stinger_sights + reflex + lightlaser). Grendel's laser survived by luck: dedup keeps the FIRST row per nif and its display string has no scope word.
- LASER-SLOT ROOT CAUSE: folded = EDID(null) + LOCALIZED display name + mesh basename + attachPoint EDID, and WeaponPartSlot scans it receiver-first, scope words (scope/sight/optic/reflex/iron) BEFORE laser. FormDisplayName resolves the OMOD FULL subrecord (a formID into the localized string table, e.g. 0013134D -> 0x9820) to text like "Recon Sight" - so display scope words preempt the base filename's 'laser': lightlaser.nif classified 'scope', first-seen scope pick kept stinger_sights, laser never assembled. Dir is NOT in folded (only in p.dir for the token gate), ruling that out.
- CHANGED (build 80, ShopExplorer.cpp CollectWeaponAssemblyNifs):
  1. meshId = mesh basename + attachPoint EDID, built alongside folded.
  2. slot = WeaponPartSlot(meshId) first; only when that has no slot word, WeaponPartSlot(folded) (display-name fallback preserved for plugin parts whose mesh name is generic). Filename + attach point are the strongest identity (ap_gun_receiver/ap_gun_Muzzle/... cover every receiver/muzzle whose base name lacks the word, e.g. stinger.nif and supressor.nif); display text is a weak signal that must not hijack the group order.
  3. Note 'supressor.nif' (ESM typo) matches no muzzle word in WeaponPartSlot - attachPoint 'ap_gun_muzzle' is what classifies it; unchanged behavior, documented here so nobody "fixes" the typo into a regression.
- EXPECTED NEXT LOG: Eon assembly 5 [receiver+muzzle+mag+scope+laser]; EquiNOX 5 [receiver+barrel+mag+scope+laser]; Grendel stays 7; every other weapon's composition byte-identical (display-only classifications survive via the pass-2 fallback).
- TEST PLAN (build 80): 1. Eon + EquiNOX hover -> assembly lines gain '+laser', laser module visible on the preview. 2. Grendel/MagPulse/Razorback/Tombstone/Microgun compositions unchanged vs build 79. 3. Left-click grid items; report CTDs with time.
## 6r-4a RESULT -- build 81-connect (BSConnectPoint C-/P- part placement), deployed 27/09/2026 23:19
- SHA256 11872CB2FAE3860BB9A78B5DB8A050EC7C6BB9E12BC7FADCE145CCC22CD563C0, 3082240 bytes, tag 'build 81-connect' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Strings verified: build 81-connect / connect place / connect root / no host for / BSConnectPoint; legacy 'build 80-laserfix' gone.
- SESSION 79 VERDICT (build 80): composition now correct on all 13 weapons (user: "they render but not quite right", 4 screenshots) - every part was root-merged at identity, so origin-centered part geometry buried itself inside the receiver body. Build 80 placed nothing by design; the NIFs themselves carry the mounting graph, which is the correct fix.
- OFFLINE VALIDATION (all before any C++):
  1. Extracted all 657 base-game weapon NIFs from Starfield - Meshes01.ba2 (prefix meshes/weapons/) to %TEMP%\opencode\weapon\.
  2. Rewrote the header parse as C++-style trial-sync (nif_connect.py): the 8 type-sync failures of the first scan are gone - 0 parse failures across 657 (tombstone_longbarrel was ours and now parses).
  3. BSConnectPoint layouts decoded from bytes. Parents = u32 ver(3), u32 count, entries {u32 len host node name, u32 len "P-<Slot>", 8f quat(w,x,y,z)+translation+scale}. Children = u32 ver(2), 5 pad bytes, then {u32 len "C-<Slot>"} until block end (no count; stop on first non-ASCII).
  4. Placement dry-run over the exact build-80 part-walk orders (13 assemblies): ALL PLACE CLEAN, zero identity fallbacks, zero missing files. Chains resolve (Rattler suppressor mounts on the long barrel's P-Muzzle which mounts on the receiver's P-Barrel). MagShot ships a lowercase C-receiver - root detection must be case-insensitive.
  5. Bounds cross-check (world-space geo bounds): magsniper barrel attach y=0.665 vs receiver body tip 0.666; tombstone 0.317 vs 0.318; lawgiver 0.707 vs body 0.706 - attach points sit on the geometry within 1mm.
  6. lawgiver_standardbarrel.nif is a 518-byte geometry-free stub (barrel visuals live in the receiver body) yet carries P-Muzzle@root - the connect parse must accept geo-less files, which is why ParseNifGeos relaxes its notypes/noroot stages for connect callers.
- MATH CONVENTION (locked): Mat4 is row-major 4x4, column-vector (p' = M*p, translation at m[3],m[7],m[11]); compose = parent * local. Entry matrix = quat(w,x,y,z) to R, scale folded into the 3x3, t in column 3. Connect world = place[parent] * nodeWorld(host node, or root frame when the host name is empty) * entry. First matching host wins (hosts keyed in placement order); multi-pass loops grow chains (receiver -> barrel -> muzzle/scope); the C-Receiver holder (else path 0) stays identity.
- CHANGED (build 81):
  1. LiveModel.h: ConnectPt {node, name, m[16]} and ConnectInfo {parents, childs, nodeWorlds, rootWorld} plus ParseNifConnects declaration.
  2. LiveModel.cpp: ParseNifGeos gains an optional ConnectInfo*. New helpers QuatEntry (8f -> 4x4, fail-closed on non-finite fields or degenerate scale) and ParseConnectBlocks (scans the block table by type name "BSConnectPoint::Parents" / "BSConnectPoint::Children"; a bad Parents block is skipped, a bad Children run stops exactly like the offline validator). notypes/noroot stage failures relax when a_conn is set (geo-less stubs parse). The NiNode walk records node worlds (later same-named node overwrites) and the root frame world. New public ParseNifConnects = LoadConfig + g_mutex + NormalizeNifKey + ResolveAsset + ParseNifGeos(..., &conn); returns whether any connect data was found.
  3. NativeModel.cpp: new helpers PlaceMat/MulPlace/IsPlaceIdent (1e-5), TransformMeshes (p' = M*p over positions; normals rotate without translation then renormalize; identity no-op), BaseName, ResolveAssemblyPlaces (parses every path's connects, finds the case-insensitive C-Receiver root, first-wins host map, multi-pass resolve, per-part INFO logs).
  4. Hooks: the dummy branch (all 13 weapons) resolves placements once before the part loop and transforms each appended range (Clone3DSwapPath meshes before the merge, AdoptFileMeshes range [before, after)); the Stage A2 !any && !proxy addon block resolves over its candidates and transforms adopted ranges; proxy/armor (bind pose) is never placed = identity. Bounds: the dummy branch recomputes ComputeMeshBounds when any place is non-identity (the first part's stored bounds are stale after moves); Stage A2 already recomputes.
  5. main.cpp:60 tag -> build 81-connect. xmake release 57s, zero new warnings (only pre-existing C4244/C4189/C4996).
- EXPECTED NEXT LOG: per weapon one `connect root '<receiver>' (N paths)` line plus one `connect place '<part>' C-x -> P-x@node t=(x,y,z)` line per mounted part, t matching the offline dry-run (e.g. lawgiver suppressor t=(0.000,0.707,0.091)). ZERO `no host for` lines on base-game weapons - any such line means the connect parse regressed and that part fell back to identity.
- TEST PLAN (build 81): 1. Hover all 13 weapons (Microgun, Razorback, EquiNOX, Eon, Grendel, MagPulse, HardTarget, Rattler, MagSniper, Tombstone, MagShot, Lawgiver, MagShear): barrels extend forward from the receiver, scope/muzzle/grip sit on the body, mags sit in their wells (MagShot shell-side, Grendel mag at the top-rear Magazine02), no part floating at origin or sunk inside the receiver. 2. Report any `no host for` lines verbatim. 3. Regression: armor/clothes previews unchanged (proxy path untouched, identity). 4. Left-click grid items; report CTDs with time.
## 6r-4b RESULT -- build 82-details (weapon detail meshes + diffuse resolution fixes), deployed 28/09/2026 12:54
- SHA256 D08B439AF1CF7C09237A60984A3BC60CD3DC43B9899A86FE4C66269F020FD842, 3101696 bytes, tag 'build 82-details' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Strings verified: build 82-details / opacity index; legacy 'build 81-connect' gone. Build: xmake release 39.9s, zero new warnings (only pre-existing C4244/C4189/C4996).
- USER REPORT (build 81): placement correct on all weapons, but 'small detail meshes, some render grey as well'.
- ROOT CAUSE 1 (missing details): OfflineModel.cpp range builder line ~961 skipped every mesh with (tri < 24 || vert < 12) unless MeshSpread diagonal >= 0.3 * model radius. Screws, latches, sight dots and decal quads are tiny AND compact, so both conditions always failed: razorback 54 -> 43 ranges, microgun 59 -> 49 (about 11 meshes per gun silently dropped). The 64-range cap was NOT the cause (max 49 ranges observed; cap unchanged).
- ROOT CAUSE 2 (grey parts): 103 unique diffuse-unresolved pairs in the build-81 log. FindDiffuse failed on: AWP_Master_Shareable (mat family textures/weapons/awp_master_shareable has 0 textures; real art at awp_shareable), e-ink AmmoCounters (families ship only _opacity.dds glyph maps, zero _color), Equinox (art under dlc001/varuunequinox + sfbgs045 parents), BubBow (mat paths lack the materials/ prefix -> empty family), reflex glass (flat-kind gate refused the sight map), Grendel_ScopeDecals (tiebreak picked barrel_color over decal match), MagPulse label/display glass (gate keep rules).
- CHANGED (build 82):
  1. OfflineModel.cpp line ~961: weapons now skip only near-degenerate meshes (tri < 4 || vert < 3), no spread test - tiny detail meshes are real geometry. Non-weapons keep the old tiny+compact filter (attachment points, effect quads). 64-range cap unchanged.
  2. LiveModel.cpp FindDiffuse fix A: matPaths without the materials/ prefix (BubBow\bbow_body.mat) derive family = textures/<dirname>.
  3. Fix B: familyScopedExt gains B1 (same-parent underscore-token subset after dropping generic dir words master/shared/common/variant - AWP finds awp_shareable) and B2 (segment-prefix parent, depth difference <= 1, dirname containment - dlc/pack subdirs like dlc001/varuunequinox and sfbgs045/equinox enter scope; the depth gate deliberately rejects Test_Herb -> textures/effects/explosions).
  4. Scope weighting: strict family +1000, extended scope +900. Regression testing showed camo-pack dirs sort earlier by path and hijacked every equal-score tie (21 of 119 resolved materials would change: SideStar/UrbanEagle/Solstice all wearing desert-camo maps). With the 100-point gap the material's own family wins ties; extended scope still wins when the plain family has no entry.
  5. Fix C: bonus tokens = material last token (+600, equality or >= 4-char suffix containment) plus kind-normalized node stem tokens (+300). Grendel_ScopeDecals -> bullpup_decals_color; MagPulse_MagPowerLabel -> mag_pulse_display_color instead of the grip map.
  6. Fix D: new *_opacity.dds index (EnsureOpacityIndex, sorted by key for determinism) with a fallback loop strictly family-scoped, scored as loop 1 so the flat-kind gate still judges the pick - rescues CombaTech/Allied/ThinFont e-ink digits.
  7. Fix E: glass materials whose path carries an optic/readout context word (sight/scope/optic/reflex/lens/display/screen/counter/meter) skip the fail-closed gate - reflex glass wears the scope map instead of staying grey.
  8. Fix F: the strict gate also keeps tokMax >= 8 (label accepts mag_pulse_display).
- SIMULATION PROOF (before any C++): Python mirror of FindDiffuse over all 103 unresolved pairs: 0 -> 89 resolve. The 14 that stay grey have NO texture in any of 494 archives (AnimatedGlow_SolidBar bullet bars x8, AnimatedLetterLineGlow01 x4, Kraken counter digits x2, Test_Herb test material x1) - shader-driven/test mats, fail-closed is the correct outcome. Regression over the 119 currently-resolved material paths: 1 diff, an improvement (LaserRifle_AmmoGauge -> laserrifleammogauge_color instead of laserrifle_fuse_color).
- EXPECTED NEXT LOG: 'opacity index (N _opacity maps)' once per session; diffuse-unresolved warnings drop from ~130 to ~14 (only the no-texture families remain); weapons gain ranges (razorback 43 -> 54, microgun 49 -> 59), still under the 64 cap.
- TEST PLAN (build 82): 1. Hover weapons: small details present (sight dots, latches, screws, decals) - compare razorback/microgun to build 81. 2. Fewer grey parts: AWP shareable, Equinox, BubBow, ammo-counter digits, reflex glass, Grendel decals, MagPulse display. 3. Accepted grey (no texture exists): glowing bullet bars, Kraken counter digits, microgun test belt. 4. No regressions on previously correct textures (camo packs must NOT appear on plain guns). 5. Left-click grid items; report CTDs with time.
## 6r-4c RESULT -- build 83-tokmatch (CTD fix: size_t underflow in bonus-token suffix match), deployed 28/09/2026 13:15
- SHA256 BF604C33CE3FB663871EBF5EC47F0BA436AF928698FD2D1D650ED16F62D9DBBA, 3101696 bytes, tag 'build 83-tokmatch' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Strings verified: build 83-tokmatch; legacy 'build 82-details' gone.
- SESSION 82 VERDICT: CTD on first hover. Log ends at 'color index (41287 _color maps)' with no FindDiffuse output - the crash is inside the FIRST FindDiffuse call, right where the new bonus-token loop runs.
- ROOT CAUSE: port bug in the build-82 bonus-token suffix match (LiveModel.cpp scoreOne). bt.second.size() - kn.size() and kn.size() - bt.second.size() are size_t subtractions with no length guard: whenever the two tokens differ in length the subtraction underflows to a huge position and std::string::compare throws std::out_of_range, which is uncaught - the process terminates. The Python sim used .endswith() (length-safe), so simulation never showed this; only the C++ port had it. Every scope hit with a length-mismatched token pair reached it, hence crash on the very first material.
- FIX (build 83, LiveModel.cpp): local endsWith(a, b) helper with explicit a.size() >= b.size() guard before the subtraction; the suffix test is endsWith(bt.second, kn) || endsWith(kn, bt.second). Same semantics as the sim's a.endswith(b) or b.endswith(a).
- AUDIT: grepped every .size() - in LiveModel.cpp/OfflineModel.cpp. KindNorm (line 2208) already guards a_tok.size() > w.size(); NormalizeBase guards a_s.size() > n + 3; Fix A/B/D arithmetic all guarded (mp.size() >= 4, name.size() < 13, segPrefix length checks before indexing). The bonus-token compare was the only unguarded subtraction - no other underflow class remains.
- Build: xmake release 5.1s, zero new warnings.
- EXPECTED NEXT LOG: no CTD; 'opacity index (N _opacity maps)' appears after the first unresolved material; the 6r-4b expectations hold (89/103 pairs resolve, razorback 54 / microgun 59 ranges, warnings drop to ~14).
- TEST PLAN (build 83): repeat the 6r-4b plan - details present, fewer grey parts, camo packs absent, left-click grid items, report CTDs with time.
## 6r-4d RESULT -- build 84-keyhash (grey render fix: content-keyed preview cache), deployed 28/09/2026 14:05
- SHA256 18C8179BB80FDEE711D6B999969869873DC781892F7253D3765036E569E6CB0D, 3103232 bytes, tag 'build 84-keyhash' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Strings verified: build 84-keyhash; legacy 'build 83-tokmatch' gone. Build 50.9s, zero new warnings.
- SESSION 83 VERDICT: no CTD (the 6r-4c tokmatch fix held), FindDiffuse fixes working - only 12 unresolved pairs left (all the documented no-texture-exists set: AnimatedGlow/LetterLine counter bars, Test_Herb belt), opacity index live (13070 maps), 333 prefetched draws. Remaining complaint: MagShot renders grey with warning decals/screens missing.
- ROOT CAUSE (stale prefetch key): RequestPreviewDirect keyed the texture cache as 'direct:' + label. MagShot and SFBGS003_MagShot_Roach BOTH arrive with label 'MagShot' (NIF basename), so the Roach preview at 08:56:01 marked 'direct:MagShot' ready with ITS 13 texture entries; the real MagShot assembly (36 meshes, 33 ranges) at 08:56:04 then took the Roach's entries - zero (matPath,nodeName) matches against its own ranges - all 33 ranges UNTEXTURED, every hover, forever (5/5 MagShot events logged pre=13 untext=33). 177 of the session's UNTEXTURED draws came from this. The decal/warning-writing meshes resolve fine (never in the unresolved list) - they were victims of the stale key, not the resolver.
- ALSO FIXED (same edit): RequestPreviewDirect dedup compared only label + meshCount, so two different assemblies with the same part count silently dropped the second one (stale geometry). The key now FNV-1a-hashes the full content - name, matPath, positions, uvs, indices - making request identity, dedup, texture cache and GPU-ban keys all exact-content identity. UI 'Render:' line now shows 'direct:MagShot#<16 hex>' - by design (proves which content won).
- ALSO FIXED (same edit): direct models now insert into g_cache at publish (ServicePending), so the texture warm path (QueuePrefetch -> ServicePending -> g_cache) can re-decode after the 24-entry g_texCache eviction. Before this, path models were cacheable but direct models were not - a revisit after eviction had no way back to textured.
- DIAGNOSIS of the user's report, in parts: (1) 'writing warnings missing' = decal meshes untextured = stale key, fixed here; (2) 'some like screen also' = ammo-counter readout bars = AnimatedLetterLineGlow01/AnimatedGlow_SolidBar, no _color or _opacity map exists in any of 494 archives (shader emissive in game) - stays grey, fail-closed by design; (3) 'different versions custom rare epic legendary' = tier prefixes change stats only, never meshes/materials - NOT a render cause; (4) OMOD assembly (3 parts) composition works - placement was verified in 6r-4a.
- EXPECTED NEXT LOG: MagShot hover logs 'direct:MagShot#<hex>' with ranges==prefetched (33/33) and ZERO 'UNTEXTURED (no prefetch)' for it; every weapon's untextured count drops to only the genuine no-texture families; Roach and MagShot previews texture independently.
- TEST PLAN (build 84): 1. Hover SFBGS003_MagShot_Roach FIRST, then plain MagShot - MagShot must now show body decals, warning writings, serial labels (the grey must be gone). Order must not matter. 2. Razorback/Microgun/Equinox unchanged-textured regression. 3. Accepted grey: glowing bullet bars, kraken digits, microgun test belt. 4. Left-click grid items; report CTDs with time.
## 6r-4e RESULT -- build 85-modspike (purchase-with-mods spike: drop -> AttachMod -> pick up), deployed 29/09/2026 10:11
- SHA256 ED5F0110E633070C8F939374F64438886119D6A535A8CBB039A96DC985F18316, 3168768 bytes, tag 'build 85-modspike' (main.cpp:60), deployed to B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll. Strings verified: build 85-modspike present, spike strings present, legacy build 84-keyhash gone.
- GOAL: the parts-selector feature needs a purchased weapon carrying the selected OMODs. Before any UI, build 85 spikes the hard unknown - applying mods to the bought instance - using the game's own documented flow (the .amod console path) driven directly through SCRIPT_FUNCTION, so no console string is executed (no achievement penalty).
- MECHANICS (ShopExplorer.cpp): ConfirmBasket, for dummy-model WEAPs only (same gate as the preview assembly in FormModelPaths), warms CollectWeaponAssemblyNifs and reads the new g_assemblyPicks cache (slot + OMOD formID recorded beside g_assemblyCache; Part gained fid). SpikeDeliverWithMods then: (1) locates the game's AttachMod SCRIPT_FUNCTION via Script::LocateConsoleCommand - fail fast, before any mutation; (2) snapshots cell refs already holding this base; (3) adds the stack to the player; (4) drops it at the player's feet (RemoveItemRequest, reason kDropping) which creates a world ref; (5) finds the new ref by cell scan - pointers come straight from the cell's own list, the raw RemoveItem handle is logged but never dereferenced; (6) calls executeFunction for every pick with param '%08X' (the exact code behind '.amod'); (7) carries the ref's extraDataList back into the player, purges the world copy, SetDelete(true) on the temp ref. Every step logs [spike].
- FAILURE LADDER: failure before the drop = plain add (old behavior); attach returns false = item still delivered without mods ('mods failed'); unrecoverable drop = item stays on the floor, Notify says so, and no second copy is added (dup guard). Notify now appends ' +mods' / ' (mods failed)' / ' ON FLOOR (see log)'.
- OPEN UNTIL FIRST TEST: AttachMod's SCRIPT_PARAMETER paramType (does '%08X' parse as hex or decimal - logged 'param0Type=' on first purchase); tolerance of executeFunction for a null script context; whether drop-ref creation is synchronous with RemoveItem (cell scan runs immediately after); AddObjectToContainer source-removal semantics (the purge step makes either behavior safe).
- EXPECTED LOG buying an assembled weapon: '[spike] AttachMod name=...' once, then one 'attach slot=... ok=...' per pick, 'dropped/ref/recovered/temp ref deleted' sequence, Notify 'added x1 ... +mods'.
- TEST PLAN (build 85): 1. BUY a weapon the preview assembles (Model line shows a receiver+ composition); it briefly drops at your feet and is picked back up - expected. 2. Inventory model/name must show the mods; a weapon workbench must list them as attached. 3. Save + reload - mods persist. 4. Report the first ~40 [spike] lines on any deviation (ok=false, floor stranding, CTD with time). 5. Plain non-dummy guns purchase exactly as before.
## 6r-4f RESULT -- build 86-spikehard (build-85 purchase CTD: staged SEH hardening + param forensics), deployed 29/09/2026 11:45
- SESSION 85 VERDICT: CTD the instant a basket purchase ran. Log captured exactly ONE new line - '[spike] AttachMod name='AttachMod' short='amod' nparams=3 refFn=0 param0Type=3D' - then the process died inside SpikeDeliverWithMods between that log and the 'dropped x' log. The unlogged span = parentCell null-check, the pre-drop ForEachReference snapshot, the proven inventory AddObjectToContainer, and the kDropping RemoveItem. No crash dumps exist anywhere (no crashgen/WER artifacts).
- BUILD 86 RESPONSE (same flow, diagnostic-hardened): every stage now runs in its own POD-only __try helper (the proven ScanForMnam/LogChain pattern) returning the SEH code; the orchestrator logs '[spike] <stage> stage AV=0x...' so an access violation degrades the purchase gracefully INSTEAD of CTDing, and the log names the exact guilty stage. Stages: parentCell VirtualQuery plausibility; guarded direct references[] snapshot walk (replaces ForEachReference); inventory add (unguarded, proven); kDropping RemoveItem; dropped-ref cell scan; per-pick executeFunction; extraDataList raw read + writability gate before any AddRef; recovery add; purge; SetDelete. Pre-mutation failures abort to plain add (purchase completes without mods); post-drop failures notify ON FLOOR / mods failed - never a duplicate add.
- PARAM FORENSICS: the log now dumps AttachMod's full header plus every SCRIPT_PARAMETER (name/type/optional, capped 8) and probes 'AttachModToInventoryItem' (found in Starfield.exe rdata next to 'amod'/'RemoveMod' - a possible no-drop inventory route for the next iteration). Offline mining was attempted and ruled out: Address Library versionlib-1-16-244-0.bin resolves ID 896666 (GetConsoleCommands) to RVA 0x5EB1050, which sits in .data's zero-filled tail (file-backed .data ends 0x5D60800) - the console table is constructed at runtime, so only the live log can report the parameter layout. Observed so far: refFn=0, nparams=3, param0Type=0x3D.
- ALSO FIXED: two new C4267 warnings (size_t index into BSTArray::operator[]) - loop index is now decltype(arr.size()). Build 3.6s, only the pre-existing C4244 remains. SHA256 985B89B9BD01D6F36C7AF83506C0BE98ADCABB1F5AED32E9C0AE277CE7832430, 3177472 bytes, tag 'build 86-spikehard' (main.cpp:60), legacy 'build 85-modspike' gone.
- EXPECTED NEXT LOG on purchase: full [spike] param tables for both commands, then 'pre-drop litter matches=' -> 'inventory add' -> 'dropped ... handle=' -> 'dropped ref ptr=' -> per-pick 'attach ... ok=' -> 'recovered' -> 'purge stage' -> 'delete stage'. If a stage AVs, '<stage> stage AV=0x...' appears and the purchase STILL completes (with the ON FLOOR / mods failed Notify) instead of a CTD.
- TEST PLAN (build 86): 1. Buy an assembled dummy-weapon again and report which of: (a) no CTD and mods attached, (b) no CTD but floor/plain delivery, (c) still CTD - if (c), the LAST [spike] line names the stage. 2. Paste the first ~60 [spike] lines. 3. State where the item ended up (inventory / floor) whenever Notify is not '+mods'.
6r-4g - build 87-spikeresolve: handle-route ref resolution (snapshot walk removed)

Status: shipped. Deployed DLL SHA256 69F86C017922B7010F1872D1C17042D903D8D9B511EDDB5253DF8B11AE37FBB0 (3180544 bytes).
Changes vs 86:
- Removed the snapshot/refscan cell-walk stages (SpikeSnapshotRefs, SpikeFindDroppedRef) entirely: the CommonLib references[] walk AVs at cell+0x80 (86 log: snapshot stage AV=0xC0000005) and was the 85-CTD root cause. Cell is now diagnostics-only: SpikeDumpCellLayout hex-dumps cell+0x70..+0xB0, log-only, never gates the purchase.
- New dropped-ref route: after the guarded drop, RemoveItem's outHandle goes to Misc::LookupReferenceByHandle (Address Library ID 72986, versionlib-1-16-244-0 resolves it to RVA 0xEE7870 in .text). Signature unknown, so results are hedged: pointer-sized RAX return taken as viaRet, bool-style out param as viaOut; the whole call runs under SEH. The magic-static REL::Relocation lives in accessor SpikeHandleRel() outside the __try region (C2712 otherwise).
- Candidate validation (SpikeValidateRef): hex-dump +0x00..+0xD8 of the candidate (one log line) plus a slot hunt for a pointee whose +0x28 (TESForm::formID, project-verified) equals the weapon base form. Slot 0x98 (expected data.objectReference at OBJ_REFR+0x80+0x18) is checked first, then a full-object scan. A slot < 0xD8 accepts the candidate; no match => kOnFloor fallback with full diagnostics in the log.
- Attach / recover / purge / delete stages unchanged from 86. Dropped the "AttachModToInventoryItem command not found" probe (proven absent in 86).
- Console usage confirmed by docs: "(Ref ID).amod (OMOD ID)" - AttachMod targets the ground item's REFERENCE, exactly the drop route. Params from the 86 test log: Form (type 0x3D, required), two Int (Optional) (type 0x01) - the %08X token format is correct.
- Test expects: [spike] dropped ... handle= -> handle resolve ... viaRet/viaOut seh= -> candidate ... slot=0x98 (or other) -> attach ... ok -> recovered -> delete stage. If resolve fails the item lands on the floor; next fallback ladder if needed: VM DispatchMethodCall route B, console string route C, raw instance extra route D.
6r-4h - build 87 test + build 88-idhunt: multi-signal identity + cell-walk fallback

Build 87 test (3 purchases, no CTD, all ON FLOOR - weapons landed at the player's feet):
- Handle route worked: RemoveItem returned handles (00168FF6/00169018/0016901A); LookupReferenceByHandle (REL::ID 72986) returned a plausible pointer every time via RAX (viaRet set, viaOut=0, seh=0) - so the signature looks like TESObjectREFR*(handle) (single param, out ignored).
- The candidate hex dump shows vptr-like image pointers at +0x00/+0x10, u32 formID-like values at +0x28 (FF07AB9E / FF07ABE1 / FF07ABF4 - temp-range, growing per purchase), but the base-form hunt (pointee +0x28 == 00040826) found NOTHING - Starfield evidently does not store the base as a raw pointer at any 8-byte slot in the first 0xD8 bytes, or the returned object is a handle entry rather than the ref itself. Full dumps + level-1 pointee formIDs are now logged for offline decoding.
- Cell dump from 87 decoded the real references array: count u32 @cell+0x80, capacity u32 @+0x84 (0x400), data* @+0x88; count tracked 571 (0x23B) pre-purchase, 572 (0x23C) with a floor weapon present - consistent with the user picking up 87's first floor weapon between purchases. CommonLib's BSTArray@0x80 claim is confirmed wrong; this layout is the replacement.
Build 88 changes:
- SpikeValidateRef now returns a signal mask: bit0 base formID INLINE as u32 anywhere in +0x00..+0xD8, bit1 drop-position float triple within 5 units (data.location), bit2 pointer slot whose pointee carries base formID at +0x28 (all offsets still logged per slot as "slot:fid" line), bit3 reserved (cell match folded into pointee dump). Non-zero = accept and continue to attach. Full 0x00..0xD8 hex (520-byte buffer) + pointee dump logged every candidate.
- New SpikeWalkCell fallback when both handle candidates are rejected: iterates cell+0x88[count] entries (guarded, POD, whole loop under SEH) and returns the first readable ref containing the requested drop-position float triple (the fresh drop sits exactly at dropLocation; litter is excluded by position, not by base). Diagnostics: "count=N idx=I posAt=0xOO" / "count=N no match" / "seh=0x...".
- Accept chain now: handle candidate (sig!=0) -> cellwalk (position match) -> kOnFloor. Attach/recover/purge/delete unchanged.
- Deployed DLL SHA256 A97A8217D831CB1E5B6F90BFCA187F82C373538BA57889E0F11687176587D819 (3184128 bytes).
- Next test expects: candidate ... sig=0x? (bit0/bit1/bit2), or cellwalk ptr=... posAt=..., then attach ... ok -> recovered -> delete. If attach runs but ok=false, the a_object may need to be the ref handle entry rather than the ref - decode the logged dumps first.
6r-4i - build 89-direct: AttachMod impl called directly (Address Library ID 40714), hang tail removed

Build 88 test (1 purchase): cellwalk FOUND the fresh ref (ptr set, count=572, idx=569, posAt=0x8C - data.location sits at ref+0x8C), the attach stage executed with NO access violation but ok=false opcode=3 (parse failure), recovery read extra=1, purge AV'd 0xC0000005, delete returned code=0x0, purchase logged 'added x1 00000FD6 free (mods failed)' - then the GAME HUNG. Ref identity is solved; two problems remained: the executeFunction buffer format, and the recovery tail.
Hang root cause (build-89 response): the tail shared ONE raw extraDataList between the inventory copy and the world ref, then RemoveItem-purged and SetDelete'd the world copy - the inventory item kept a dangling pointer into freed ref state. Build 89 deletes the whole recover/purge/delete tail: the ref simply stays on the floor and the user picks it up manually; the mods ride the ref's instance across.
AttachMod exe decode (offline, capstone 5.0.7; exec RVA 0xC78BB0 taken from sfse.txt 'reloc mgr imagebase' 0x7FF635A20000):
- Address Library versionlib-1-16-244-0.bin layout: u32 version=5, u32 count at file offset 92 = 0x137458 (1274968), u32 RVA table from offset 96 (id N reads offset 96+N*4; control check: ID 72986 -> 0xEE7870, the known LookupReferenceByHandle RVA).
- The executor's work function is Address Library ID 40714 -> RVA 0x4024F0: AttachMod(TESObjectREFR* ref, TESForm* omod, int32 index, int32 arg4), verified at the call site 0x140C78CF8.
- Executor gates before that call (RVA 0xC78BB0): parse false -> return false; a_object null -> global fallback (player); [ref+0x98]==0 -> return true WITHOUT attaching; parsed form null -> return true; byte at form+0x2E != 0x98 -> logs 'Parameter must be object mod type' (string pulled from exe rdata) and returns true; index >0xFF and != -1 -> logs 'Invalid attach index, defaulting to 1'. Console one-arg form = index 0, arg4 1 (executor defaults: Form slot -> [rsp+0x58], Int0 -> +0x50 = 0, Int1 -> +0x54 = 1).
- Parse root cause of ok=false: executeFunction reads a u16 param count from the buffer start, then a marker byte per param; type 0x3D has table byte 1 at imagebase+0x58D28B5+type*8 (offline reads: 0x3D->1, 0x01->0, 0x3C->1), so it demands byte 0x72 ('r') + u16 + a scriptLocals variable resolve - a compiled script buffer, not the raw '%08X' string (we passed '00044C40': count read as 0x3030, marker byte '0' at offset 2 rejected -> jne fail). Console lines are compiled first; producing that buffer from a plugin is unreliable, so 89 calls the impl directly.
Build 89 changes (ShopExplorer.cpp):
- SpikeAttachRel(): magic-static REL::Relocation<fn>{REL::ID{40714}}; unresolved -> kNotAttempted plain add (fail fast before any mutation). SpikeAttachStage now mirrors the executor gates (ref+0x98 non-null = gate98, form+0x2E==0x98 = gateType, raw values logged), takes a 4-qword before/after window at ref+0x90..+0xA8, then calls impl(ref, omod, 0, 1) under SEH; report = {code, called, gate98, gateType, formType, window}.
- The resolved ref now gets a full 0x00..+0xD8 hex dump + pointee list (the 88 cellwalk hit was never dumped).
- Zero handle no longer aborts the flow: it falls through to the cellwalk route.
- Tail removed (SpikeReadExtraRaw/SpikeDeleteStage/purge gone). Returns the new kOnFloorMods outcome when the impl was entered without AV, else kOnFloor. Notify: ' ON FLOOR (mods attached, see log)' / ' ON FLOOR (see log)'.
- The old executeFunction call site is gone; LocateConsoleCommand + param dump kept as diagnostics only.
Deployed DLL SHA256 57B6FE9CB3AF13D9C6E61E65621CF2C13DA6D2A6605DEA774F6B2F088D519679 (3184640 bytes), tag 'build 89-direct' (main.cpp:60), legacy 'build 88-idhunt' gone. Build 33.2s, only the pre-existing C4244.
Expected next log: 'resolved sig=0x...' hex + pointees, then per pick "attach slot=... omod=... called=1 av=0x0 gate98=1 formType=0x98 gateType=1 diff=?" plus window lines, 'left x1 ... on floor ... (pick it up)', Notify ' ON FLOOR (mods attached, see log)'.
Test plan (build 89): 1. Buy an assembled dummy weapon - expect NO hang this time, the weapon on the floor with mods; pick it up and check the mods in inventory/workbench. 2. If any attach line shows called=0, report gate98/formType values (which gate refused). 3. Save + reload - mods persist. 4. If only some parts stick (window diff on the first pick only), the index param is a slot number - next build decodes it from mnam.
6r-4j - build 89 test verdict (attach works, floor pickup works); build 90-veridump: extra33 ground truth + EDIDs + wording

Build 89 test (session 1: 4 purchases; session 2: 5 purchases - fids 00000FD6 0001BC4F 0002CB5F 0002EB36 00040826; no hang, zero errors across both):
- Every purchase: drop -> handle resolve -> cellwalk hit (idx=569 each time; cell count 571 pre first drop and +1 per purchase, each cellwalk sees the fresh drop present) -> gates all pass (gate98=1 formType=0x98 gateType=1 called=1 av=0x0) -> impl entered for every pick -> 'left x1 ... on floor'. The build-89 tail removal fixed the hang.
- User verdict: floor items visible, picked up manually, land in the inventory. Mods DO attach - the user received a LEGENDARY Equinox (a tier-receiver OMOD only reaches an item by attaching) and its preview matched the delivery.
- One mismatch: the Rattler's delivered mods did not match its preview ('Didn't match preview'). Preview and purchase read the SAME g_assemblyPicks (built together in one pass inside CollectWeaponAssemblyNifs: the kSlots loop pushes pick->nif and AssemblyPick{slot, pick->fid} side by side), so the divergence happens inside the attach itself (index semantics or default-mod interplay), not in pick selection.
- User also reported preview words / warning labels are all still missing - Phase 2 UI requirement (per-slot part-name words plus warning labels in the preview).
- The always-zero diff window is EXPECTED: mods live in the object-instance extra, never in the ref+0x90..+0xA8 window.

AttachMod impl decode round 2 (capstone, offline):
- Impl 0x4024F0 has exactly ONE caller in .text: the console executor at 0x140C78D03 (every E8 rel32 in .text scanned). The workbench path goes elsewhere, so the impl itself is the only source of index semantics.
- Impl entry: dword at [omod+0x20]; bit7 clear -> jumps straight to worker 0x140403110(ref, omod, index, arg4); bit7 set -> 0x14043F9A0 explodes the omod into a list and the worker runs once per entry with the SAME index/arg4 (r12b ANDs the per-entry results).
- Worker 0x403110 first instructions are the ground-truth hook: locks extraDataList at dl+0x20, then FindExtraByType(dl, 0x33) = RVA 0x347170 = Address Library ID 37878 (found by scanning the versionlib table); if absent it ALLOCS a 0x30-byte extra (type byte 0x33 at +0x12, matching the executor's [rbp+0x12] read on the -1 path). It also reads [ref+0x98] (the object instance the executor gates on) into a local.
- Impl return is void-ish: the bit7-clear tail does 'call 0x140403110; ret' with al never set; the executor discards it anyway. Per-entry success lives in r12b inside.
- Executor index recap: 0..255 passes through; -1 does an extra-0x33 lookup + list removal dance, then forces index:=1; other >255 logs 'Invalid attach index' then index:=1. Our call (index=0, arg4=1) equals the plain one-arg console form.

Build 90 changes (veridump - diagnostics only, attach params unchanged at index=0 arg4=1):
- SpikeCollectMods + SpikeLogMods: two [spike] groups per purchase, 'pre' (before the first attach = defaults) and 'post' (after the loop): e33/inst pointers + SEH code, 0x80-byte hex of the extra33 blob and of the [ref+0x98] instance, per-pick 4-byte presence in each blob (logged 'E' = in extra33, 'I' = in instance, '-' = absent), plus a pointer scan of both blobs that accepts qwords reading back as forms with formType 0x98 at +0x2E and logs their formID at +0x28 - mods stored as form pointers are visible even without a raw formID match. POD-only under __try; string building after it (C2712); the FindExtraByType relocation (magic-static, REL::ID{37878}) resolves outside the __try.
- The inventory-add line logs weapon='EDID'; every attach line logs ed='EDID' - the log identifies Rattler/Equinox directly next run.
- Notify wording: floor outcomes now read 'bought x1 ... - left on floor (mods attached, pick it up)' (the old 'added' implied inventory delivery that never happened).
- Deployed DLL SHA256 25506D46C19D41616AFD4B210F2D86E5DCA096FDDFAC12A3E741E7FC091FC471 (3196416 bytes), tag 'build 90-veridump' (main.cpp:60), stale 'build 89-direct' gone. Build 59.0s, only the pre-existing C4244.
Expected next log per purchase: inventory add ... weapon='EDID' -> resolved hex/pointees -> 'mods pre e33=0x... seh=0' + hex33 + hexInst + pick=00044C40:E- ... live=[...] -> attach lines with ed=... -> same four lines with 'post' -> 'left x1 ... on floor' -> Notify 'bought x1 ... - left on floor (mods attached, pick it up)'.
Test plan (build 90): 1. Buy the mismatching dummy (Rattler) - post pick flags + live list show exactly what went wrong: pick absent from both blobs = attach failed; present already in 'pre' = it was a default; present in 'post' but visually different from preview = index/slot replacement issue (index param is then the fix: the -1 auto path or per-slot indices decoded from mnam). 2. weapon=EDID confirms which purchase mapped to which gun. 3. Pickup + workbench list remains the human proof. 4. Save + reload check for persistence.
6r-4k - build 90 test verdict (mods stored=0 mystery); RE of store/sync; build 91-veridump2: count trajectory + slot dump

Build 90 test verdict (session 12:38:46, Rattler purchase 12:41:40 fid 00040826, weapon='Rattler', 5 attaches all called=1 av=0x0 gates pass; OMOD ed='' - those forms carry no editorIDs, so mods are identified by formID only):
- pre: e33=0x0, inst=0x1E5AA083C70, all 5 picks absent, live=[].
- post: e33=0x1EA2D1EA660 (created by the attaches), type byte 0x33 at +0x12 confirmed, the store list at extra+0x18 reads count=0 / cap=4 / data*=0x1EA478C7BC0, the instance hex is byte-identical pre/post, no picks in the 0x80 window.
- Inventory card after the manual pickup: MODS 0/8 - nothing surfaced, which fully explains 'Rattler did not match preview'. The Equinox 'legendary' claim is still unexplained if no mod ever stores; its session had no build-90 log lines.

RE decode since (offline on Starfield.exe, files under Temp\opencode\store_disasm.txt, sync_disasm2.txt, insert_disasm.txt, hashtbl_disasm.txt):
- Store 0x140403530(extra rcx, omod r8, byte r9b, stack flag1, stack byte2): the flag1 path first calls 0x1404063E0 which is a DEDUPE REMOVE over the list (entry match = [form+0xE0] equal AND byte8 equal AND NOT (form+0x20 & 0x180), dec count on hit), then calls find 0x1422CC0D0, then writes the entry {omod form ptr, byte from r9b slot, byte2, 0} at the returned index. Store itself never touches count.
- Find 0x1422CC0D0(set, key, param=[list+4], stride 0x10): if count@[list+0] < param it takes the fast path (count++, return old index); otherwise the key-vtable grow path (count++ on success, -1 on failure). count@[list+0] is the same field every consumer iterates (sync 0x403DAD/0x403E40, worker dedupe loop 0x40326E).
- Consequence: 5 attaches should leave count=5 (or 4 if one dedupe hit); the post dump shows 0, so the count was never raised or was raised then reset. cap=4 plus a live data* prove the insert machinery ran at least once.
- Sync 0x140403B00(extra, ref, pair): the pair phase runs only when pair.first != 0 (instance already has a mod store), and BOTH reconcile loops are gated on count@[extra+0x18] - with count=0 everything is skipped, which is why the instance hex never changes and the card stays 0/8 while the count reads 0.
- extra33 is a 0x30-byte allocation: hex33 offsets +0x30.. are heap-neighbor noise; the apparent second {count=1, cap=4, ptr} triple at +0x50 is NOT another list.
- Stack args of our worker call: flag1=1 always on, byte2=arg4=1, entry byte8 comes from worker [rbp+0x50] (not yet pinned to a real parameter).

Build 91-veridump2 (diagnostics only, attach unchanged at index=0 arg4=1):
- Per-attach trajectory: after every attach one line 'mods step#N slot=... e33=... cnt18=... cnt1C=... data1=0x...' - it shows whether count climbs 1,2,3.. or falls back after each step. If it drops, the resetter is one of: the dedupe remove, the sync (0x403B00/0x404160), or the worker-tail extraDataList ops 0x140501450 (dl=0xB6) / 0x140533AC0.
- Slot dump: pre/post lines gain 'cnt18= cnt1C= data1= slots=[fid b8b9bA]...' - the physical 0x10-byte entries behind data*, bounded by cap (clamped to 8), each guarded, formID resolved from form+0x28; stale slots still name the forms even when the live count reads 0.
- Deployed DLL SHA256 31EE60C1870C147083AE09F158225B615F4C7D3430C6F50E34F8F6F5F8ACE0A3 (3202048 bytes), tag 'build 91-veridump2' (main.cpp:60), stale 'build 90-veridump' gone. Build 373.3s, only the pre-existing C4244.
Test plan (build 91): buy the Rattler again and grep 'mods step#' plus 'cnt18='. A climbing count that zeroes only at post points at the sync or the worker-tail ops; a count that reads 0 after every single step points at the find fast path never running (param=[list+4] mismatch) or an immediate dedupe remove. Build 92 then carries exactly one fix. After count>0: pickup persistence (does extra33 travel into the inventory entry, or must the instance be synced) and the instance-sync gate, then Phase 2 selector UI with per-slot part words plus warning labels (Equinox feedback recorded in 6r-4j).
6r-4l - build 92-partsui: Phase 2 selector UI (parts dropdowns + rarity), preview choice reload, purchase gate widened

RE follow-up since 6r-4k (offline, files under Temp\opencode\):
- phase4_disasm.txt (0x140404160): the NOT-FOUND branch ([form+0xE0] absent from the local set) calls 0x140403440 = remove-at-index over the store list (dec count; count==1 zeroes slot0 and count) then sets a break flag; the FOUND branch migrates the entry. So 0x404160 is a real count-resetter - but the build-91 post dump contradicts its count==1 path (slot0 still held the real form while count read 0), so either it never ran or the final zero comes from elsewhere.
- collect_disasm.txt (0x140406390): phase3 collect gate = 0x402C7B0 plus the instance-pair lookup 0x1405DDBD0 (keys at rva 0x5dda2e3 / 0x5dd7153; computed abs target 0x1461DD500 -> rva 0x61DD500 -> file offset 0x61DBD00 > .data EOF = BSS, runtime-initialized, not file-readable; the key value is irrelevant to the count question).
- removeat_disasm.txt (0x140403440): remove-by-index with slot compaction, called by the dedupe remove - a resetter on its own only if it ran post-attach, which the dump disputes.
- Remaining suspects: the worker-tail ops 0x140533AC0 / 0x140501450. Disasm files generated (tail533_disasm.txt, tail501_disasm.txt) but NOT yet read. The count/persistence fix is deferred to its own single-variable build after 92.

User pivot: Phase 2 selector UI now (sub-mods menu + rarity dropdown so preview and delivery render correctly), count resetter next.

Build 92-partsui changes (ShopExplorer.cpp unless noted):
- Catalog: CollectWeaponAssemblyNifs snapshots g_choiceVersion at entry and rebuilds only when the cached version mismatches (g_assemblyBuiltVer). No-model OMODs of the weapon family (folded contains no 'no mod'/'nomod'/'modgroup') no longer drop out silently: they land in tierRaw, kin-flagged by kwKin (MNAM census) or famTok match, fallback owner plugin < 150 OMODs -> accept all. The tail builds WeaponCatalog{slotOrder, slots, defaultFid, tiers} beside cache/picks: SlotOption{label, nif, fid, standard} per slot (anchor dir unchanged), TierOption{label, fid} per tier (kin first). Stored with insert_or_assign + g_assemblyBuiltVer[wid]=ver. Part gained a label field set at push_back from OptionLabel(form, mesh) (FullName -> EDID -> mesh basename -> hex fid; OMOD EDIDs are empty at runtime).
- Choices: g_weaponChoice holds WeaponChoice{slotFid map, tierFid} per weapon, written only by SetWeaponSlotChoice / SetWeaponTierChoice, which bump the atomic g_choiceVersion after unlocking - one bump per click, one rebuild per click. Negative cache: owner-empty early-return now stores an empty catalog + builtVer so unresolvable owners never rescan per frame.
- Resolution: chosen slot fid replaces that slot default; dummy-base weapons fill untouched slots from defaults (old behavior); stale choice falls back to the default for dummy bases; non-dummy weapons resolve ONLY explicit choices (defaults on a real model would stack parts it already renders). Tier rides as AssemblyPick{'tier', fid} - SpikeDeliverWithMods consumes picks generically, 'tier' is for logs and preview only. 8-NIF cap unchanged. New logs: extended assembly line '(N slots, M tiers, dummy|base)' and 'tier options 0xXXXXXXXX: [label=fid, ...]' - the empirical rarity-discovery check.
- UI (details pane, after Keywords, before TRANSFER; stays on the clicked selection while the viewport follows hover): for WEAP call the collector first (cache-hit is cheap), snapshot catalog+choice under g_assemblyMutex, draw unlocked. Per slot: muted uppercase label + full-width combo, default option suffixed ' (default)', PushID(slot), Selectable fires SetWeaponSlotChoice only when the fid differs. RARITY combo when tiers non-empty: 'Standard (no tier)' (fid 0) plus each TierOption; chosen tier adds the muted note 'rarity attaches on purchase (no mesh change)'. One accentBright 'PARTS' header. Parts gated on FormModelsAssembled(form) && slotOrder - real-model weapons show RARITY only.
- Preview: RarityWord (first space token, else after last '_') prefixes the title when a tier is chosen; the native reload gate gained g_choiceVersion (fid alone cannot see a parts change) so a click re-clones next frame. RequestPreviewDirect stays content-keyed; label is in the key - identical content dedups, tier-only changes re-raster (orbit reset is expected).
- Purchase: ConfirmBasket dropped the 'model contains dummy' gate - every WEAP warms the collector and spikes whenever resolved picks are non-empty (tier choices deliver on real-model weapons; untouched non-dummy weapons get no picks and keep the exact plain-add path).
- Build/deploy: XMAKE ok 7.3s, only the pre-existing C4244 (EnsureSkipModels :423). First compile failed on const char* iteration over vector<string> slotOrder (C2440 -> const std::string&) plus two C4456 lock shadows (-> choiceLock). Deployed DLL SHA256 3EB17D2721FDAACCF274549BBABC2B1D289EAD1A097996ADF02737BAA12CD078 (3337216 bytes), tag 'build 92-partsui' (main.cpp:60), stale 'build 91-veridump2' gone. Deployed strings verified: build tag, PARTS, RARITY, 'rarity attaches on purchase', 'tier options', 'Standard (no tier)'.
Test plan (build 92): 1. Click a dummy weapon - PARTS header, one combo per slot (defaults labeled), RARITY row; changing a combo re-renders the preview within a frame. 2. Grep 'tier options' - empty means family detection needs a wider kin window (the empirical question). 3. Pick a rarity, buy: [spike] must include the 'tier' pick and the title must read e.g. 'Legendary <weapon>'. 4. Real-model weapons show RARITY only, no PARTS; no-choice purchases stay on the plain-add path (no picks, no spike). 5. Preview words / warning labels were NOT in this build - still open from 6r-4j. Then build 93 carries the single count-resetter fix once tail533/tail501 are read.

### 6r-4m - build 93-recdump: pin the reconcile keep/drop (record-source dump)

DEPLOYED: build 93-recdump, bytes 3350016, sha256 88D82D247EE9CFC93FDFB0D3FE3410740272EC3588C597CB98A6916F1C98714C, tag at main.cpp:60.

VERDICT FROM BUILD 92 TESTS (user report + log grep): Phase 2 UI works - PARTS and RARITY combos render, previews re-render, all 17 attaches called=1 av=0x0 - but attachments store nothing. Every purchase shows cnt18=0 after each attach (cnt1C=4, data1=0x1859B7E0840 byte-identical across three purchases: 09:22:40 Rattler 00040826 6 picks, 09:25:28 ParticleMG 010AC60B 5 picks, 09:25:59 Tombstone 0002EB36 6 picks). Inventory cards show spawn-rolled mods (Rattler = Armor-Piercing Rounds only), not the purchased picks. RARITY discovery too broad (receivers/grips/mags/skins/Quality Level/Med Theft, zero real Calibrated/Refined/Advanced tiers; 28 "tier options" hits) - filtering deferred to a later build.

RE CONCLUSION - THE COUNT RESETTER (this section, disassembly of sync 0x403B00 / phase4 0x404160 / removeat 0x403440):
1. Worker order (unconditional): store 0x403530(extra33, omod, index, arg4) -> res 0x42C7B0() -> instance-record lookup 0x5DDBD0(instance, &pair, key1, res) -> sync 0x403B00(extra33, ref, &pair) -> containment loop -> tail (0x501450, 0x533AC0 -> 0x4048D0).
2. Store find 0x1422CC0D0 is CORRECT: all success paths bump count (fast count<param -> count++, slow vfunc0 -> count=1, slow vfunc8 -> count++). Observed 0->1->0 means the bump happens and something removes it right after.
3. Sync pair-insert phase is gated on pair.first != 0; the collect loop (lines 188-218) and the phase4 loop (lines 229-246) run whenever extra33 count > 0, regardless of pair validity.
4. Collect loop: 0x406390(entry_form, &local) = 0x5DDBD0(form, KEY2 same word RVA 0x61DD4FF) returns a record; 0x4067E0(record+8 ...) merges the record vector [rec+8, rec+0x10) stride 8 into the SET (flat hashmap: count@+0, cap@+4, data@+8/+0x10).
5. Phase4 0x404160(ctx, entry): ctx = {+0=&set, +8=extra33, +0x10=ref, +0x18=&counter, +0x20=&flag, +0x28=&map}; form=[entry+0]; skip (return 1 = continue) if form null, [form+0x20]&0x180, or [form+0xE0]==0. Set lookup of key [form+0xE0]: FOUND -> consume + map dance + cleanup + fire mod-list-changed events (vfunc +0x60/+0x58 on globals 0x5a5c240/0x5a5c1ec), returns 1, does NOT touch extra33. NOT-FOUND -> removeat on [ctx+8]=extra33 (dec/zero count), sets flag, returns 0 -> sync BREAKS after ONE removal.
6. Net per attach: store inserts 1 (count 0->1) -> phase4 NOT-FOUND removes it (1->0). That is exactly the observed cnt18=0 trajectory. Root cause: our dropped ref's instance record (union source A) is empty/missing for a fresh spawn, and [form+0xE0] of the fresh OMOD is absent from the union of (instance record vector) + (each entry form's record vector via collect).
7. Ruled out as resetters: sync itself (writes no count), dedupe 0x4063E0 (runs before insert, list empty on first attach), 0x4048D0 (post-containment migration/reconcile), impl phase4 post-phases (buffer free + event dispatch cosmetics only).

WHAT BUILD 93 ADDS (diagnostic only, no behavior change):
- SpikeRecordDump/SpikeRecordProbe/SpikeLogRecord (after SpikeLogMods, ShopExplorer.cpp ~:2045): replicates the worker's two record fetches on the game thread pre- and post-attach-loop via raw image-base+RVA fn pointers (res 0x42C7B0, lookup 0x5DDBD0, key u16 at image+0x61DD4FF), dumps: res, key word, instance out pair + 6 header qwords + parsed [begin,end) qword range (vec/vecOk/vecN), per pick: fid, [form+0xE0], [form+0x20], [form+0x28], omod out pair, pick-0 header, omod vec count, and both membership verdicts (e0InInst, e0InOmod). POD-only under __try; strings built in SpikeLogRecord outside the guard.
- Call sites: SpikeLogRecord(SpikeRecordProbe(dropped, pickFids, nPicks, 0)) after the pre SpikeLogMods (~:2393), phase=1 after the post SpikeLogMods (~:2429).
- Grep targets: "[spike] rec pre", "[spike] rec post", "vecOk=", "inInst=", "inOmod=".

WHAT THE LOG MUST ANSWER (drives build 94, the actual fix):
a. Is instVec populated at all (vecOk=1 vecN>0) with the spawn card's mod keys? If yes, the instance record IS the card list and the OMODs' [form+0xE0] values simply are not in it -> build 94 seeds each picked OMOD's [form+0xE0] into that record (transient: seed -> call impl -> unseed) so phase4 goes FOUND and keeps the entry.
b. If the instance record is empty AND omod records are empty (vecOk=0 everywhere, out pairs null or non-range), the set union is empty by construction for fresh refs -> build 94 must seed a record (either source) before the attach, or write [form+0xE0] into whichever object the set actually reads.
c. If e0InOmod=1 but e0InInst=0 (or vice versa), only one source holds the key -> seed the missing one; also check whether inInst/post changes vs inInst/pre (did our own attach populate anything?).
d. Confirm [form+0xE0] is non-zero for the picked OMODs (phase4 skips entries with [form+0xE0]==0; a zero here would mean a different bug class).
## Section 6r-4n -- build 94-rarity (RARITY v2 + cursor + double-click)

Post-93 user triage: "missing common rare epic legendary and writing and
warning stickers a few ui bugs 2 mouse cursors double click should add to
basket" + "add items to inventory so not drop them on ground". Question
tool: "writing and warning stickers" = in-game guns show barrel/label
decals but the preview renders none (3D decal gap, not UI); rarity action
= attach quality + roll legendary effects.

esm dump (tools/dump_omod_classes.py, tools/gen_omod_class_table.py):
OMODs carry EDID in file (runtime GetFormEditorID still null). Quality
OMODs mod_<Weapon>_Quality_01..04 (204 weapon entries; 01=Common no
bonus, 02=Calibrated, 03=Refined, 04=Advanced) have NO MNAM and no model,
so the MNAM census never sees them and the nOwner>=150 gate dropped them
from RARITY -- root cause of the missing entries. Legendary weapon
effects = mod_Legendary_Wep_1/2/3_* (36 entries: 13/13/10), universal
(no MNAM, no weapon binding, TESObjectWEAPON_InstanceData); armor/helmet/
pack quality and effects excluded by EDID pattern. High Velocity and
friends are MNAM-bearing no-model internal parts (census-kin) that
polluted the old tier list.

Build 94 changes:
- src/OmodClassTable.h (generated): kOmodQualityTable 204 x
  {omod, weapon, rank} (EDID token -> WEAP fid, all resolved; 16
  unresolved are armor), kOmodLegendaryTable 36 x {omod, rank}, binary
  search Lookups.
- RARITY list = table quality entries for the own weapon (rank-sorted:
  Common, Calibrated, Refined, Advanced) + fixed sentinels Rare
  (0xFF000001), Epic (0xFF000002), Legendary (0xFF000003) + Standard at
  top. Legacy kin tierRaw entries only when the table has no quality hit
  (plugin weapons), preserving the old fallback.
- Sentinel resolution rolls 1/2/3 distinct effects from cumulative rank
  pools (Rare=rank1, Epic=1+2, Legendary=1+3), std::shuffle into the
  cached AssemblyPick list as slot="legendary" so preview and purchase
  consume the same rolls until the choice changes (cache keyed by
  g_choiceVersion). Log: "SFShopExplorer: rarity roll <wid> -> N effects
  [fids]". Assembly log line now reports effects pool size.
- Double cursor: ImguiOverlay.cpp no longer calls ShowCursor(TRUE) on
  input-enable or per frame; new HideHwCursor() settles the display
  count at hidden without per-frame drift (bare FALSE loops would
  decrement below the game's own shows and could hide the OS cursor in
  menus). The ImGui virtual cursor (raw-input) is the only arrow.
- Double-click: manual same-row tracker (press times, MouseDoubleClickTime)
  after the grid Selectable calls ConfirmAdd() -- ImGui's
  IsMouseDoubleClicked never fires on a Selectable that activates on
  release.

Build 94-rarity: 12.828s, only pre-existing C4244. Deployed
SFShopExplorer.dll 3389440 bytes SHA256
6CE1445381A628CF1687A2769BC22F060F2E3DC7C037C4F63120DA32AA8B7D8D;
strings "build 94-rarity", "rarity roll", "effects, ", "Legendary"
verified in the DLL.

Still open: (1) attach correctness -- build-93 proved the reconcile
instance record stays empty (phase4 NOT-FOUND -> removeat), so quality
and legendary attaches will not persist until the record seeding fix;
(2) inventory delivery (build-88 double-own hang RE); (3) preview decals
(writing/warning stickers) -- Rattler decal ranges build and upload,
suspect draw-side alpha clip / depth bias; Cutter never previewed.
Next section: 6r-4o.

## Section 6r-4o -- build 95-persist (reconcile repair: mod persistence)

Post-94 triage: "selected legendary i got common" - the roll fired
(00000FD6 -> 3 effects [000FFA3D,0031996B,000F7321], all 8 attaches
called=1 gate98=1) but the delivered item carried no effects. The
build-94 `rec pre` / `mods step#` lines pin the mechanism exactly:

- The attach worker (RVA 0x403110) runs store (Address Library ID
  1015131 = RVA 0x403530) then sync (ID 40721 = RVA 0x403B00) once
  PER PICK, with the pair built from the object instance (lookFn RVA
  0x5DDBD0). Phase 4 (RVA 0x404160) Source A is that pair's vector
  [first+8, first+0x10) stride 8 holding raw [form+0xE0] keys - and
  on a fresh drop it is EMPTY: `rec pre` shows inst vecOk=0 vecN=0,
  inInst=0 inOmod=0 for every pick (image 0x7FF70F2F0000, inst
  pair.first = inst+0x2F8, form pair vecs 6/4/2/0... are the omod
  records, not Source A).
- Phase 4 semantics (full disasm): e0 = [form+0xE0]; e0==NULL ->
  early keep; reconcile-set count 0 -> remove; linear scan
  NOT-FOUND -> remove-at (RVA 0x403440 = predicate find over the
  record + memmove/dec count - erases the entry FROM the record);
  FOUND -> erase the key from the set (consume once) + category-
  group prune (group map built from Source A, global registry at
  rip+0x5a7dcc2 keyed by source obj; miss -> keep), sets the flag,
  increments the counter; sync breaks on the first entry that does
  not return 1.
- Observed trajectory is that exactly: step#1..#6 cnt18=0 (each
  fresh store pruned by its own sync), step#7 (0031996B e0=0) kept
  via the null early-keep -> cnt18=1, step#8 prunes the new pick
  again -> `mods post cnt18=1 cnt1C=4 data1=0x1CB5F9EC2C0`,
  `slots=[0031996B 000100]`, hexInst unchanged pre->post, live=[].

Record entry layout confirmed: {+0 form ptr, +8 index byte, +9 arg4,
+0xA 0} - the worker saves arg3 at [rsp+0x18] (= [rbp+0x50] after its
frame) and reads it back into r9 for store, so the +8 byte always
matches the entry itself. sync ctx (built at lea rcx,[rbp-0x28]):
+0 &setB, +8 RECORD, +0x10 REF, +0x18 &counter, +0x20 &flag,
+0x28 &groupmap; the pair arrives in r8 and r9 is never read. sync
reads pair.first only -> a fake pair is safe.

Store decoded (RVA 0x403530): signature (record, rdx, form, index,
arg4, replace). rdx is spilled in the prologue and never read (the
slot is overwritten by a local before any use), the worker passes
index=0, arg4=1, replace=1 at [rsp+0x20]/[rsp+0x28]. Body:
replace=1 erases the {form.e0, index} key (RVA 0x4063E0), then RVA
0x22CC0D0 appends (count < cap) or grows through the allocator
vfunc (cap at record+0x1C, stride 0x10) - the growth path exists,
so 8 entries from cap 4 is fine; -1 (grow failure) silently skips
the write, which cntMid would expose.

Tail decode: mark (ID 44741 = RVA 0x501450, (DL, 0xB6, REF)) is a
pure existence check - presence bitmap + list walk + vfunc on a
match, creates NOTHING, return value unused; flush (ID 45140 = RVA
0x533AC0, (DL, REF, instance, flag)) locks DL+0x20 itself and, when
extra 0xB6 does NOT exist, resolves the record (RVA 0x4048D0,
smart-ptr resolve, no record read), creates 0xB6 {+0x18 instance,
+0x20 out} and with flag=1 runs the extra-0x96 builder; when 0xB6
ALREADY exists it early-returns at RVA 0x533D38 skipping the
builder. The builder (ID 45153 = RVA 0x534D20, (DL, REF, extra33,
instance, out on stack)) locks, FindExtra 0x96, RVA 0x404F90
(record+0x28 word == 0xFFFF -> null; our records are always 0xFFFF)
-> pair fallback keyed at rip+0x5CA8541 -> RVA 0x9EB8D0 -> creates
extra 0x96 (size 0x50) + RVA 0x54EB30 payload write; idempotent
(rewrites when the extra already exists).

FIX (ShopExplorer.cpp SpikeRepairMods, invoked after the post-loop
probes unless attachAv): under the same DL+0x20 lock the worker
holds across these very calls (ID 123863 LockWrite / 35983
UnlockWrite - recursive, mark and flush re-lock the same word):
record = FindExtra(dl,0x33); re-store every pick with the worker's
exact arguments; one sync over a fake pair {&s_fake, 0} where
s_fake = {0, e0s, e0s+n} carries every non-null [form+0xE0] (the
Source A layout verbatim, elements are raw e0 values - same parse
as SpikeRecordProbe's instVec); mark 0xB6; flush(..., 1); the
builder called DIRECTLY (covers the b6-exists skip; a second run is
an idempotent rewrite); unlock (best-effort in the SEH handler).
Fake storage is function-static (lives for the process) so no
registry entry keyed by the fake source address can dangle.
Diagnostics: `[spike] repair seh=.. dl=.. inst=.. rec=.. stores=
e0s= cnt18=before->mid->after cnt1C= b6=-> e96=-> out=` plus the
full `mods repair` dump.

replace=1 collision check: within-item e0 keys are distinct
(build-94 `rec pre`: 7 unique + one null for the 8-pick item, 5
unique for the 5-pick item), so the e0+index erase cannot drop a
sibling; null e0s are skipped in the fake vector and early-keep
anyway. NOTE e0 is NOT unique across items (00044C40 and 00057B44
share 000001CB26709180) - only within-item distinctness matters
here. A ref carrying a foreign leftover entry (e.g. a prior
purchase's null-e0 survivor) early-keeps it - cntMid/cntAfter
include it.

ID table (versionlib-1-16-244-0; reverse tool:
_archive/tools/rva_lookup.py <rva>, or struct read at 96+4*id):
store 1015131, sync 40721, mark 44741, flush 45140, builder 45153,
lock 123863, unlock 35983, FindExtra 37878 (0x347170), impl 40714.

Build 95 shipped: tag `build 95-persist`, DLL 3399168 bytes SHA256
FE265F0D072FFCEE9632829F133E26B0F818519594F99D7F82B1FA62338A548D.
Test expectation: fresh ref `repair ... stores=8 e0s=7 cnt18=0->8->8`
(5-pick item: 0->5->5), b6 0->1, e96 0->1, `mods repair cnt18=8`
with all 8 picks showing E; pick the item up and the inspected item
must show the selected quality prefix and the rolled legendary
effects (re-verify BOTH purchases). cntMid < pre+stores means a
grow failure or an e0 collision - read cap/out before changing the
plan. Open: which consumer reads the record entries vs extra 0x96
for the applied state (instance vec stayed empty in 94; if repair
keeps cnt18=8 but the item is still common, the next probe targets
the 0x96 payload consumer).
(2) inventory delivery (build-88 double-own hang RE); (3) preview
decals (writing/warning stickers) + barrel artifact (clip(tex.a -
0.5) OfflineModel.cpp:535, coplanar z-fight, decal skip :1535 /
:1607-1639).
Next section: 6r-4p.

### 6r-4p - build 96-reffix: correct world-ref resolution (multi-item baskets) + repair e0-collision (delivered parts)

Feedback after build 95 (4 screenshots): "pretty close but the parts given are wrong and writing on weapons are missing". Writing = the queued offline-preview decal gap (unchanged here). Parts analysis of the 09:16 session log (2 purchases: Rattler 00040826, Tombstone 0002EB36, 2.9 s apart, same feet position):

- Wrong-ref (root cause): both purchases' cellwalk returned the SAME pre-existing floor ref (ptr=0x29913e7db30; count=571 idx=569 then count=572 idx=569 - the first match at the shared drop position). The fresh refs were idx 570/571 (append-on-drop) and never received the attaches. Purchase 2's attach loop then wiped purchase 1's repaired record on the shared ref (mods pre cnt18=7 -> post 0) before repairing it with Tombstone's picks, so one floor item mixed two deliveries and the other was plain. All attach/repair logs show inst=0x29D0A070120 for both - one instance, one ref.
- Handle route was rejected both times: Misc::LookupReferenceByHandle returned a distinct plausible object per drop (0x2A175CCE000 / 0x2A175CCE1E0, temp-formID-like FF07B334/FF07B336 sequential +2, +0x98 instance set) but SpikeValidateRef found sig=0x0 - its hunt only scanned 0xD8 bytes and neither position nor base formID sits in that window on the fresh object.
- e0 collision: repair ran with replace=1 (e0+index erase then append); [form+0xE0] is a shared slot descriptor, not per-form - Frenzy 000FC8A4 and Titanium Build 000FFA3D both key ...4580, Handloading 000EA0BA and Swashbuckling 002F97E1 both key ...4600. Each repair stored 8 and kept 7: the earlier same-slot legendary was erased (slots= shows Frenzy/Handloading missing).
- Positives: every attached fid matched the preview exactly (CSV-checked); repair persistence works (record entries drive name/effects; extra 0x96 still not built - instance vec empty, out=0x0, non-blocking); RARITY prefix "Professional's" present in game.

Changes (tag build 96-reffix, main.cpp:60):

- SpikeValidateRef: hunt range 0xD8 -> 0x150 (inline fid, position triple, pointee scan) + hex dump follows the same range; caller hex buffers 520 -> 780 (42 qwords).
- New SpikeCellCount: snapshots cell+0x80 reference count BEFORE AddObjectToContainer/drop, logged as [spike] cellcount pre=N.
- SpikeWalkCell rewritten (+a_preCount arg): (a) tail route - entries[preCount..count) were appended by OUR drop, so the OLDEST new gated entry (index preCount upward) wins; gate = readable + ref+0x98 non-null (the attach executor's own gate); (b) position scan only when count did not grow, now keeps the LAST gated match (newest) instead of the first; position alone no longer qualifies. Diagnostics: route tail|pos, idx, count/pre, inst, posAt (0x150 range).
- Resolution logs: dropped ref line gains eqHandleRet (1 = handle candidate won the extended hunt; 0 = cellwalk route) and preCount - next run shows which source produced the ref and whether the two purchases resolved to DIFFERENT ptrs.
- SpikeRepairMods (a): store replace 1 -> 0 (append only) so same-slot legendary keys cannot erase each other; (c) comment notes the sync set may still collapse a duplicate key - cnt18 mid vs after in the log decides whether a follow-up must skip sync. Call-site comment updated.

Deploy: SFShopExplorer.dll 3401216 bytes, SHA256 2BE86DA07202A9F5F7BC6D4F1FA59DF2979BCA02E1C544AB9DEAC73E1C7C44F9; byte-search verified build 96-reffix, cellcount pre=, tail idx=, eqHandleRet, pos idx=, repair seh=, cnt18={}->{}->{}.

Test watch (next session): two purchases in a row without moving - expect cellcount pre=N then N+1, cellwalk "tail idx=" at the pre-count (distinct per purchase), dropped ptr DIFFERENT per purchase, eqHandleRet per route; each floor item carries only its own preview picks; repair cnt18=0->8->8 (both legendaries kept; 0->8->7 means the sync set collapsed the duplicate key - follow-up build skips sync then); mods pre for purchase 2 must be 0 (no shared-ref leftovers). Screenshot mod names from build-95 feedback (Recon Scope, Reflex Sight, Suppressor, Armor-Piercing Rounds, Professional's) were never attached in that session - retest on fresh floor items should make them appear only if selected.

Open queue: preview decal/writing gap + barrel artifact; skip-sync follow-up if cntAfter < cntMid; extra 0x96 still unbuilt (instance vec empty).

### 6r-4q - build 98-inventory-delivery: use the native actor pickup path

Latest user log (`Documents/My Games/Starfield/SFSE/Logs/SFShopExplorer.log`, 2026-10-02 10:57) loads `build 96-reffix`. It explicitly says `left ... on floor` for Rattler (00040826), EquiNOX (0001BC4F), and Tombstone (0002EB36), each with `attachedAny=1 attachAv=0`; therefore the old DLL intentionally left all three world refs for manual pickup. Screenshots of the inventory are consistent with the user subsequently picking them up, but the latest plugin log does not show delivery into inventory. No log from build 97 was present.

Source change (not deployed or runtime-tested): after attaching/repairing the OMODs, call `Actor::PickUpObject(ref, count, false)` so the game uses its own player-pickup path to transfer the modified instance/extras. This avoids the unsafe raw `ExtraDataList` sharing that caused the build-88 hang. The function call is SEH-guarded; log the request and schedule a next-main-thread-tick inventory count verification. A normal return from this void call is not proof of transfer: immediate count growth reports success, otherwise the purchase toast says verification is pending. Build 98 log tag added. `xmake build -y` passes; only the existing wchar-to-char C4244 warning remains. Deploy the rebuilt DLL and confirm a purchased modified weapon appears in inventory without a manual floor pickup; inspect the `inventory pickup verify ... confirmed=1` line and workbench mods. If pickup verification fails, check both floor and inventory before retrying.

### 6r-4r - build 99-pickup-depth: test confirmed the pickup call faults; labels still unresolved

Correction to the pre-test note above: the user explicitly deployed build 99 and restarted; the runtime log confirms `build 99-pickup-depth` loaded. This is a valid test, not a stale-DLL test. Three-plus purchases still ended on the ground. For each purchase, the `Actor::PickUpObject` stage logged SEH `0xC0000005`, player inventory count did not rise, and deferred verification confirmed 0. The user also reports no crash; the access violation was caught inside the plugin's SEH guard and did not terminate Starfield. Do not retry this virtual call.

The item world refs are resolved uniquely and OMOD attach/repair executes without AV, although the separate mod-persistence logs still show `mutated=0`/empty live mod lists in some cases. CommonLib confirms `PickUpObject` is virtual slot 0x15B, but it faults in the current call/context. The raw ExtraDataList sharing route remains retired (build 88 hung). Build 100 later tried the engine-owned `RemoveItemRequest.moveTo=player` path from the modified dropped ref; the user tested it and reported purchases still went to the ground. No new crash was reported. Build 101 is a local candidate that changes the transfer reason to `kTakeFromActivator`; this has not been runtime-tested, and a safe transfer is not yet established.

Decal evidence from build 99 is only partial: Rattler's material resolves the expected `heater_decals_color.dds`, its mesh UV ranges are populated, and the texture is uploaded/bound, but that does not prove the GPU pass draws or that the correct alpha channel is used. Build 100 added detailed decal bind/draw logs and preserved DXGI sRGB formats as sRGB for sampling; the user reported the weapons became darker and lettering was still absent. Build 101 restores the prior UNORM mapping to address the color regression and logs material ranges named `Rattler_Decals.mat` even if the name classifier misses them. The warning lettering is still unresolved; collect build-100 bind/draw lines and then test build 101 only after deployment is explicitly approved.

Build 100 was deployed and tested; user reports purchased weapons still landed on the floor, colors became darker, and gun writing remained absent. Thus `RemoveItemRequest.moveTo` with `reason=kNone` did not solve the transfer, while converting sRGB formats back to UNORM did cause the color regression. Build 101 restores UNORM behavior, sets the transfer reason to `kTakeFromActivator`, and logs material ranges named `Rattler_Decals.mat` even if the name classifier misses them. `xmake build -y` passed with existing wchar-to-char, unused variable, and strncpy warnings. The user explicitly selected deployment. Starfield was stopped before replacement; backup `SFShopExplorer.dll.build100-backup-20261002-192236` was created. Build 101 is deployed at `B:/MO2/Starfield/mods/Shop Test Render/SFSE/Plugins/SFShopExplorer.dll`; source and destination are 3,423,232 bytes, SHA-256 `5da8c8086ad770018ce80de32f3003028ca5347d3f9e871ecde37d2140a869c5`. Expected test marker is `build 101-take-reason-unorm`; check `inventory move verify ... confirmed=1` for delivery, and inspect `rattler decal-material range` alongside decal binding/draw lines. The adjusted reason and restored colors remain untested until the next run.

Deployment/test history: Build 100 (`build 100-moveto-bc-srgb`) was deployed to `B:/MO2/Starfield/mods/Shop Test Render/SFSE/Plugins/` after the user authorized it on 2026-10-02; backup `SFShopExplorer.dll.build99-backup-20261002-190512`; DLL size 3,421,184; SHA-256 `327925a48aa9b6e29e8ab9e36a127e76f0c6c81d55fb36d32859ab6a56e1d115`. User report on the subsequent run establishes runtime test, though latest log has not yet been collected; request/verify lines and decal bind/draw lines are needed to distinguish whether moveTo faults, returns without moving, rows are not bound, or the image data lacks markings. Clean/no-crash status is separate from delivery and appearance outcomes.

### 6r-4s - audit corrections and remaining proof gaps (build 101 not yet tested)

Source/history audit found several assumptions that must not be treated as established fixes:

- `RemoveItemRequest.moveTo=player` in ItemPreview is a container-to-player example, not proof that a placed weapon ref transfers this way. Build 100 already tested the placed-ref route with `reason=kNone` and it stayed on the floor; build 101's `kTakeFromActivator` is still a test hypothesis. Current request also leaves `instanceData`, `extra`, and `inventoryIndex` at defaults. Do not populate raw extra pointers or share an `ExtraDataList` without confirming ownership/refcount semantics (build 88 hung).
- Current “mods delivered” evidence is weaker than the names imply: `SpikeAttachStage.called` only means the void AttachMod implementation returned without SEH; its `mutated` probe watches ref+0x90..+0xA8, already known not to contain mod-list state. `SpikeRepairMods` outcome is logged but not used to gate the success outcome. Inventory count is only base-form/count evidence: it does not prove the selected OMOD stack, object-instance data, or extra 0x33 survived. Confirm the workbench/card mod list and quality/effects after pickup/transfer. Test quantity >1 separately; current flow drops and attaches against a count-sized stack, not individually verified instances.
- Basket credits are removed before the per-item spike; failed/floor delivery currently has no refund/rollback. If any transfer fails, check both floor and inventory before retrying to avoid duplicate purchases.
- Decal path has an untested whole-renderer fallback: CPU `Rasterize` intentionally skips every `IsDecalSheet` mesh and has no texture sampling. GPU render failure/banning therefore guarantees missing decals. Pipeline-ready or a post-render material-range summary is not proof the GPU actually issued a draw. The build-101 `rattler decal-material range` message enumerates ranges after the draw loops; it is not an interception/count of successful `DrawIndexedInstanced` calls. More specifically, the loop skips a decal range when `texSlot < 0`, but the later summary still logs it; a `texSlot=-1` line means the decal was NOT drawn. The bind log's `alpha=1` is only `IsDecalSheet(...)` classification, not a measured texture-alpha result. Look for per-model GPU-ban warnings and actual GPU texture/pipeline/bind lines before diagnosing alpha alone.
- A concrete texture-channel blind spot remains: prefetch resolves only the `.mat` Albedo `File`, and `FindDiffuse` tries `_opacity` maps only when no color candidate was found. It never combines an albedo with a separate opacity/mask map. The alpha PS samples `tex.a`; Rattler's earlier texture log was BC1/ DXGI 71 (which can encode 1-bit alpha), but no evidence yet proves the chosen color DDS contains the lettering mask or any non-opaque alpha. Inspect the actual material channels plus the texture's alpha/UV coverage; do not infer transparency from the filename.
- Build-100's sRGB-to-sRGB texture view caused the reported darker look, and build 101 restores the prior UNORM view mapping. However Rattler's logged DXGI 71 texture was already BC1_UNORM, so that format change cannot by itself explain its decal alpha/lettering failure. Keep appearance, alpha, and whether the GPU path ran as separate questions.

### 6r-4t - build 101 test result: transfer still fails; Breach decals submit but static text is absent (2026-10-02)

Latest `SFShopExplorer.log` session loaded **`build 101-take-reason-unorm`** at 19:23:34, with MO2 profile `Test profile` and `itemcapture mode=2`. This is a valid test of build 101, not an old-DLL run.

**Delivery: conclusively still on the ground.** At 19:28 the shop processed four one-item purchases: Rattler `00040826`, EquiNOX `0001BC4F`, Tombstone `0002EB36`, and Breach `000547A3`. Each log sequence shows `dropped x1`, `inventory move request code=0x0 out=00000000`, `inventory move immediate ... confirmed=0`, and `delivery ... destination=pending/floor`; the deferred verification also reports unchanged inventory counts (`0->0`, `1->1`, `0->0`, `0->0`, expected count +1) and warns `inventory move not confirmed`. `code=0` is only the request call's return, not success. Build 101's `kTakeFromActivator` reason therefore does **not** transfer these placed refs. The plugin first adds a base-form item, then drops/removes that item to create the ref; since the measured inventory count returns to its pre-purchase count, manually picking up the floor copy is not shown to create a duplicate. Still check the floor and inventory before re-buying.

**Breach text/engravings: model and decal path are present, but the screenshot confirms the visible markings still are not reproduced.** The preview uses the assembled Breach NIFs (`Breach_Receiver`, standard barrel, choke, tactical stock, reflex sight), producing 81 ranges. The GPU path prefetched 80 textures; `Materials\\Weapons\\Breach\\Decals.mat` resolved to `textures/weapons/breach/decals_color.dds` (1024×1024 BC1_UNORM). It logged decal draw submissions for ranges 6, 14, 50, and 73 with valid texture slots 7, 15, 50, and 73. Thus the static decal geometry wasn't wholly omitted and isn't a generic renderer-ban/fallback failure in this preview. However, `AnimatedLetterLineGlow01.mat` for Breach `AmmoType_A:2` is unresolved and has `texSlot=-1`, so that animated ammo lettering range is explicitly skipped. Existing alpha PS samples texture alpha; current lines do not inspect actual texture pixels/channels or establish that the color DDS contains the BREACH/ALLIED/CAUTION print or alpha mask. Root cause of the static missing lettering is therefore narrowed to material/texture-channel content or decal UV/depth/blend behavior, not yet proven. The shop renderer also assembles an offline NIF model, not the live game weapon scene.

**OMOD persistence remains suspect.** On the Breach purchase, five AttachMod calls returned without SEH, but `diff=0`; repair logged `cnt18=16->21->17`, `e96=1->0`, and no selected IDs found by its post-repair pick probe (`live=[]`, each listed as `--`). This is not proof of what Starfield ultimately serializes, but it is not evidence that the requested parts/quality/effects survived. Test workbench state only after confirming a safe delivery path; do not treat `attachedAny=1` as confirmation.

**Crash dumps checked at the user-specified overwrite.** `B:\\MO2\\Starfield\\overwrite\\Root` contains 38 Starfield dumps, with the newest named `Starfield_09-29-09-35.dmp` (2026-09-29 09:35 UTC), before this Oct 2 build-101 test. Its minidump exception stream reports access violation `0xC0000005` at `SFShopExplorer.dll+0x6103E`; without the exact matching DLL/PDB and a complete symbolized stack, that offset cannot be assigned to a source function. No Oct 2 dump is present in that overwrite listing, so this is an older plugin crash and cannot be tied to today's purchase/preview result. `AppData\\Local\\CrashDumps` likewise has no Starfield dump newer than 2026-09-25. No build or deployment was performed during this log/dump review.

### 6r-4u - build 102 source change: no ground drops (2026-10-02)

At the user's request, weapon purchases with selected assembly picks now skip the experimental world-drop/AttachMod/RemoveItem(moveTo) route and call `PlayerCharacter::AddObjectToContainer` directly. This guarantees the code no longer intentionally creates a ground drop; inventory count before/after is logged, and a weapon notification warns that preview-selected mods are not applied. A failed count check reports delivery unconfirmed rather than claiming success. This does **not** solve OMOD/quality/effect persistence: there is no verified inventory-instance attach API yet. `xmake build -y` passed after this source change (existing C4244 wchar-to-char warning only); build 102 was deployed to Shop Test Render and its marker confirmed on launch.

**Build-102 runtime test — 2026-10-02 19:56–20:00:** SFSE log and plugin log confirm build `102-inventory-only-weapons` loaded in MO2 profile `Test profile`. Basket acceptance directly added five one-count entries to inventory, all verified by increased counts: credits item `00000FD6` 0→1; Rattler `00040826` 0→1; EquiNOX `0001BC4F` 1→2; Tombstone `0002EB36` 0→1; Breach `000547A3` 0→1. Each weapon toast explicitly says “inventory only; selected mods not applied.” There are no `dropped x` lines in this test; this confirms the user-observed no-ground-drop result. No new Starfield dumps appeared in `B:\\MO2\\Starfield\\overwrite\\Root` (the previous newest remains 2026-09-29). This test validates no-drop inventory delivery only. It does not verify OMODs/quality/effects, and the later Breach preview in this log still submits the same decal ranges; its missing engraving/static writing remains unresolved.

Next engineering work is to find and validate a supported way to apply selected OMODs directly to an inventory stack/instance, then verify parts, quality, and effects in the workbench. Any new DLL deployment still requires explicit approval.

### 6r-4v - build 103 runtime result and build 104 follow-up (2026-10-02)

The user reported the build-103 test ran. `SFShopExplorer.log` confirms the 20:23:20 startup marker `build 103-bc1-decal-alpha-audit`; Breach was resolved as 81 meshes/60,945 tris at 20:24:55. The texture prefetch audited all three decal-geometry paths with the same result: `textures/weapons/breach/decals_color.dds`, BC1 1024×1024, **0/1,048,576 transparent texels** for `Decals3:0`, `Declas6:0`, `Decals1:0`, and `decal_barrel:0`. All four decal ranges were bound with texture slots (7, 15, 50, 73) and appear in the decal-range submission summary. This rules out BC1 punch-through alpha as the transparency mask in the selected color DDS, but does not alone prove the rendered marks should be visible: the chosen map's RGB content, UV locations, rasterized geometry, depth/blend and possible separate opacity/material inputs remain to check. The static Breach markings should therefore not be attributed to depth or the fuzzy map matcher without inspecting the actual companion/material data.

Build 104 was tested. Log marker `build 104-breach-opacity-companion-audit` loaded at 20:37:50. The exact companion exists: `textures/weapons/breach/decals_opacity.dds`, DXGI format 80 (BC4_UNORM), 1024×1024, 524288 bytes. The four decal geometry paths still use the same opaque BC1 color map. This shows there is a separate same-size opacity texture; it does not prove whether Starfield's material shader consumes it as decal alpha.

Build 105 was tested, and the user confirmed the Breach preview looks good with its same-stem BC4 opacity map combined into the color map. At the user's request, build 106 generalizes this only to decal-classified geometry: for any resolved `*_color.dds`, it checks for the exact sibling `*_opacity.dds` in the same directory; only matching-size BC1_UNORM color + BC4_UNORM opacity pairs are decoded and combined to RGBA8. It does not fuzzy-match, and missing, dimension-mismatched, or unsupported pairs leave the original texture untouched. This applies the proven approach across the preview's decal materials without changing non-decal texture sampling, inventory, or OMOD handling. The opacity cache is limited to unique maps per prefetch; renderer pipeline version was bumped so the new resources are uploaded.


### 6r-4w - build 113-preview-expand: click-to-expand preview viewport (2026-10-04)

The weapons fix shipped after build 112 was undocumented in this file; it was reconstructed by diffing current source against the deployed build-112 DLL. OfflineModel.cpp gained a direct GPU renderer for NativeModel Clone3D meshes: exact .mat albedo first with fuzzy FindDiffuse fallback, per-range SRV slots 1-64, same-stem BC4 _opacity companions uploaded to slots 65+, a runtime g_opacity mask shader, and the new logs direct GPU: range and direct GPU: paired opacity with the snapshot messages building direct GPU preview, direct GPU preview unavailable for this model, direct GPU renderer failed for this model, and this form carries no direct mesh data. The direct request key became direct:{label}#{fnv1a of mesh bytes} in place of the bare direct: prefix, and the request/ready logs were renamed to SFShopExplorer direct: with mesh and tri counts. LiveModel.cpp gained exact material map overrides for the setdressing items probed on 2026-10-03 (SFBGS00D action-figure packaging and UC Army Soldier, celery produce, exoticplayingcard symbols, office plant swaps, and the numbered poster) plus the card non-symbol neutral-surface rule. The user also rebuilt a logic-only OfflineModel.cpp edit at 10:14 on 2026-10-04 with no new string literals. Builds 107-112 are still not written up here.

Build 113 change: clicking the preview viewport (press and release both on the viewport, travel under 4 px) toggles between the compact strip (34 percent of the details column, clamped 120-260 px exactly as before) and the expanded viewport (72 percent of the column, leaving room for the item details below). A double-click toggles twice so the net height is unchanged and the orbit reset keeps working; a press that began on a list row and merely releases over the viewport never toggles. The hint line now reports click: expand or click: collapse. New members previewExpanded and previewPress live in ShopBrowser (ShopExplorer.h). Deployed at 3350016 bytes after byte checks for build 113-preview-expand, both click hints, the direct hash key, and direct GPU: paired opacity. Not yet tested in game.

### 6r-4x - build 114-ui-quality: vector font, asset minification, uiScale layout (2026-10-04)

Diagnosis of the pixelated-UI and off-looking-logo report: the shop drew every label with ImGui's default ProggyClean bitmap (fixed 13px, picked up implicitly because CreateContext had no AddFont call anywhere) while [overlay] uiScale only scaled style paddings through ScaleAllSizes; text never followed the scale. The brand logo, chest art, and credit icon uploaded at native size (1024x1024, 1024x1024, 1034x514) and drew at 96/160/40 pixels with MipLevels=1, so the GPU minified without mips and the edges aliased.

Build 114 change: InitImGui now loads Segoe UI (fallbacks Segoe UI Semibold, then Arial) at 16px * uiScale with oversampling and the symbol glyph ranges the shop draws (arrows, box drawing, geometric shapes for the sort triangles, miscellaneous symbols for the star and skull); a log line reports UI font loaded at N px or warns when no system TTF exists. LoadTexturePixels takes a per-asset long-edge cap applied through the WIC HighQualityCubic bitmap scaler before the RGBA conversion: ShopLogo 256, Chest 320, CreditIcon 128, space untouched. The new Overlay::UiScale() accessor scales the fixed shop heights (header 102, toolbar 48, status reserve 32, logo 96, credit 40x20) so text, padding, and panels grow together when uiScale differs from 1.

Deployed at 3354624 bytes after byte checks for build 114-ui-quality, UI font loaded at, the no-system-TTF warning, and the retained build 113 click hints. Not yet tested in game.

### 6r-4y - build 115-theme-ini: persist the shop theme (2026-10-04)

The theme combo only wrote the in-memory member, so the choice reset on every launch. ShopExplorer.ini (the same file the [overlay] and [shop] skipmodels reads use) gained [shop] theme: EnsureThemeIni loads it once before the first ActiveTheme pick in DrawImGui, clamped to Theme::kTotal-1 with a log line when the saved index is out of range, and SaveThemeIni writes on every combo change, logging theme saved to INI or a warning with GetLastError on failure. The new ShopIniPath/EnsureThemeIni/SaveThemeIni helpers sit next to EnsureSkipModels and reuse its module-directory pattern.

Deployed at 3356672 bytes after byte checks for build 115-theme-ini, the save log, the failure warning, and the retained build 113/114 strings. Not yet tested in game.

### 6r-4z - build 116-archive-warm: kill the selection-to-render freeze (2026-10-04)

Log-timestamp analysis of the build-112 session pinned the lag to two lazy-init costs paid at
click time: (1) ParseNifMeshes/ParseNifConnects run inside LoadFromForm on the PRESENT thread
during the selection, and ResolveAsset indexed each not-yet-opened BA2 there - 46ms for a
20k-entry archive up to 678ms for Meshes01 (320k entries). The Digipick click shows three such
archives back to back: about 770ms frozen between the click and the native-preview log line.
(2) The first FindDiffuse holds g_mutex across EnsureColorIndex, whose loop opens every
archive - a ~6s scan in the same session (36.8s to 43.0s) - so any selection made during that
window blocks on g_mutex in ParseNifMeshes for seconds.

Fix: LiveModel::WarmTick(), called every task tick from ShopBrowser::FrameUpdate after
ServicePending. One BA2 per tick under a short g_mutex hold (mesh archives first, the
EnsureBa2List partition that matches ResolveAsset walk order), then color+opacity indexes
built once at the end while every archive is already cached - the same work FindDiffuse would
otherwise do under the lock. After warm completes (~100 ticks from plugin load, long before a
shop opens) the click path only ever does cached map lookups: no Present-thread indexing, no
multi-second g_mutex hold to collide with. Progress logs reuse OpenBa2 per-archive lines plus
warm N of M done and archive warm complete summaries; post-completion it is a no-op.

Deployed at 3359232 bytes after byte checks for build 116-archive-warm, both warm summaries,
and retained build 113/114/115 strings. Not yet tested in game.

### 6r-5a - theme persistence confirmed in game (2026-10-04)

User relaunched after build 115-theme-ini and confirmed the chosen theme stays selected across
relaunch - the [shop] theme round-trip through SFShopExplorer.ini works end to end.

### 6r-5b - build 117-preview-lightbox: expand preview shows a real full-workspace render (2026-10-04)

User report: expanding the preview did not grow the image, it only "drops from top right to
middle right". Cause: the details column is a fixed 340px, so the ~1.5-aspect snapshot is
width-capped in both states - the old expand grew the child box (34% to 72% of column height)
but the aspect-fit image stayed about 244x160 and merely re-centered inside it.

Fix: expanded preview is now a lightbox over the whole workspace. Frame-level preview work
(TryNativeVendorPreview, FormModelPaths + native load gate, SkipModelRequested +
RequestPreviewDirect, GetSnapshot, SetShopWantsCapture) was hoisted out of the old viewport
child so it runs every frame no matter where the viewport draws, and the draw path became a
drawViewport(center, size) lambda shared by both layouts. Compact (default) keeps the old
34% / 120-260px in-column strip with its child name item_render_viewport. When previewExpanded
the strip is replaced by a muted placeholder line, and after EndTable a preview_lightbox child
covers the workspace rect (wsMin/wsSize captured just before BeginTable) with a dim backdrop
(4,4,8,200) and a centered 76%x90% box - drawViewport(boxMin, boxSize) - so the render finally
gets real width; the cursor is restored to the table footer afterwards. Auto-collapse: if (!form)
previewExpanded = false, so a filter change that empties the selection while the lightbox is
open cannot park an empty overlay over the catalog. The lightbox child is created last so it
takes input over the table; clicking it collapses through the existing press+release gate
(no drag movement), same as the old expand. The old metadata { } wrapper was removed because
the lambda has to stay in scope until the lightbox after EndTable.

Deployed at 3362304 bytes after byte checks for 0.8.117, preview_lightbox, the placeholder
line, and retained build 116/115 strings (archive warm complete, theme saved to INI). Not yet
tested in game.

### 6r-5c - toolbar/preview labels and material-database follow-up (2026-10-04)

Narrow-toolbar follow-up: CATALOG title, category combo, optional subcategory combo, quantity/free controls, theme combo, and maximize/close controls now occupy separate rows. Each combo takes the available inner width instead of retaining a 72px minimum that could push into adjacent controls. The toolbar child height is 240 * uiScale to accommodate the stacked rows including a subcategory row. Preview overlays are drawn after the image; when capture status is absent, the direct-GPU mesh/triangle/texture summary still receives its own bottom strip. When capture status exists, the summary sits on the line immediately above it. This is source/build verified, not yet runtime-tested in game.

CDB schema reconnaissance: materialsbeta.cdb's TYPE section has 97 class layouts; the class definition for BSMaterial::EffectSettingsComponent explicitly contains IsGlass, Frosting, MaterialOverallAlpha, ZTest, ZWrite and BlendingMode, while BSMaterial::AlphaSettingsComponent contains HasOpacity, AlphaTestThreshold and OpacitySourceLayer. Texture paths are present as inline serialized data in DIFF records (e.g. Data\\textures\\...dds); this is not merely a string-table-offset lookup. The typed field layouts are now partly understood, but the owning material/object record walk and field association are not yet validated, so no glass alpha value or playing-card reverse texture is claimed. Leave the correct card-back texture unresolved and do not add an unmeasured glass-opacity renderer hack. No celery/plant map overrides were changed.

The unfinished local CDB probe was removed after proving its initial chunk-scanning assumptions incorrect. The in-game glass composition and authentic card-back material remain open research items; no game/MO2 deployment was performed.

### 6r-5d - materialsbeta.cdb decoded far enough to settle the playing-card back (2026-10-04)

The CDB does not need a full object walk to answer the texture question. Texture sets are stored as a name record immediately followed by its texture-path records, in slot order (colour first, then opacity, then normal/rough/metal/mask), which is directly readable in the byte stream.

Proven associations, all from `tools/renderer_recon/extracted/materialsbeta.cdb` (105,037,616 bytes):

- `ExoticPlayingCard_symbols_TextureSet1` -> `Data\Textures\Setdressing\Exotic_Clutter\ExoticPlayingCards\ExoticPlayingCard_symbols_Decal_Color.dds` and `..._Decal_Opacity.dds`. These are exactly the two files previously found in `Starfield - Textures09.ba2`, so the existing symbol-sheet override and the same-stem opacity pairing are confirmed by the game's own material data. There is no other texture in that set, which is why the symbol atlas is a decal-only material.
- `ExoticPlayingCard_Letters01_TextureSet1` -> `astralcolorgradient02_color.dds` plus `decallabelsletters01` normal/opacity/rough/metal. It is a letters decal, not a card back.
- `ExoticPlayingCardBox01_Trim_Color02_TextureSet1` and `ExoticPlayingCardBox01_Trim_Color_TextureSet1` carry no texture paths of their own (the next record is another asset name), so the card trim has no authored map and stays neutral.
- `GlassCleanTransparent02_TextureSet1` carries no texture paths at all. The blister glass is therefore not texture-driven; nothing in the archives can supply an opacity map for it, and an unmeasured blend constant would be a guess.
- `Abstract_Poster2x1_TextureSet1` -> `Data\Textures\Setdressing\posters\AbstractPaintings\512x1024\Poster_512x1024_02_color.DDS` + `plasticplainsmooth03_normal.DDS` + `metaldullgeneric01_rough.DDS`. The numbered variants such as `Abstract_Poster2x1_13_TextureSet1` override the same slot with their own map, which is the inheritance pattern for `Abstract_Poster2x1_02`.
- `actionfigure` occurs zero times in the whole CDB, so the SFBGS00D packaging materials are not in `materialsbeta.cdb` at all; the existing exact atlas override there can be neither confirmed nor contradicted from this file, and the `blister` hits in the CDB are unrelated (Inhailer_BlisterPaste, OctopedeA_BlisterCrab).

Change: the card NIF's back surface uses `Materials\SetDressing\Posters\Abstract_Paintings\Abstract_Poster2x1\Abstract_Poster2x1_02.mat` on range :1, and the CDB base texture set for that material names `Poster_512x1024_02_color.DDS`. The abstract-poster exact mapping in FindDiffuse was moved above the card-neutral rule so the card back now gets the game's own authored albedo instead of a neutral surface; the plastic body and box trim stay neutral because their texture sets are empty. No render path, blending or celery/plant mapping was touched.

### 6r-5e - build deployed and runtime-confirmed (2026-10-04)

`xmake build -y` succeeded and the result was deployed to `B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll` at 3,367,936 bytes, SHA-256 `a522432ce0232a4c46de60a236a4204ac225698000649ee191beb13c32306565`; the previous file was preserved as `SFShopExplorer.dll.backup-20261004-161036` (3,362,304 bytes). Byte checks confirmed the direct-GPU metadata string and capture-status strings in the deployed image.

Runtime evidence from the live session (`Documents\My Games\Starfield\SFSE\Logs\SFShopExplorer.log`): the marker `SFShopExplorer v0.8.117` loaded at 16:14:26, matching the deployed image. The 2,013-line session contains no `[W]` or `[E]` entries and no ImGui cursor/bounds warnings, which is the check that previously produced the `imgui.cpp:10789` SetCursorPos warning from the toolbar hit target. The narrow-toolbar and preview-label changes are therefore runtime-clean; their visual layout is still only confirmed by source inspection.

The subsequent build carrying the card-back mapping could not be deployed: Starfield.exe was running and held the destination DLL (`Device or resource busy`). It stays local until the game is closed. No process was terminated.

### 6r-5f - preview expand is in place, toolbar is a compact vertical list (2026-10-04)

User screenshots of build 0.8.117 showed both remaining UI faults. Expanding the preview opened `preview_lightbox`, a child window over the whole workspace table with a dim backdrop, so a click replaced the entire shop with one huge image. The toolbar had been given full-width combos, so at 2000px window width each combo became a 2000px bar and the header stack looked like empty shelves instead of a control list.

Fix: the lightbox is deleted outright, together with the now-unused `wsMin`/`wsAvail`/`wsSize` workspace-rect capture and the placeholder text. `drawViewport` is called from the same `item_render_viewport` child in both states; only the height changes - compact keeps `clamp(availY*0.34, 120, 260)`, expanded uses `clamp(availY*0.62, 300, 560)`. Because that box lives in the details column, the column itself now widens from 340 to 470 while expanded so the aspect-fit render gains real pixels instead of being hidden behind an overlay. Clicking still collapses through the unchanged press+release gate, and double-click still resets the orbit.

The toolbar keeps one control per row but stops stretching: a `comboWidth()` helper gives the category, subcategory and theme combos `clamp(avail*0.5, 110*uiScale, 260*uiScale)`, so they render as a short left-aligned vertical list at any window width and still shrink rather than overflow on a narrow panel. The Qty/Free and MAXIMIZE/Close rows keep their wrap checks. Child height reduced from 240 to 210 * uiScale now that no row is a full-width bar.

`xmake build -y` succeeded (only the pre-existing wchar-to-char warning). Deployed to `B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll`, 3,367,936 bytes, SHA-256 `e4ba3c57694ce04e5472a818976d187d71fd462b5ae68d14d5f583da746dc5b3`, previous file kept as `SFShopExplorer.dll.backup-20261004-162537`. Byte checks confirm the card-back build marker is live and `preview_lightbox` is gone. This deploy also carries the 6r-5d card-back mapping that was blocked earlier. Not yet tested in game.

### 6r-5g - toolbar is horizontal again, with wrapping (2026-10-04)

Correction to 6r-5f: "vertical" was wrong - the user wants the top toolbar controls listed left to right, not stacked. The stacked version also had no room for the CATALOG title next to the first combo.

The toolbar is now one left-to-right flow. `placeInline(width)` calls `ImGui::SameLine()` while the requested control still fits in the remaining content width and falls back to `ImGui::NewLine()` when it does not, so the sequence CATALOG | category | subcategory | Qty | Free | theme | MAXIMIZE | Close stays on one row at normal window widths and wraps to a second row on a narrow panel instead of overflowing or stretching to full width. Combo widths are `clamp(avail*0.30, 110*uiScale, 210*uiScale)`, bounded so a 2000px window does not produce 600px dropdowns. The child is 72 * uiScale, which holds two wrapped rows; the per-control wrap thresholds (60/72/90/64 * uiScale) are what decide the breaks.

`xmake build -y` succeeded (only the pre-existing wchar-to-char warning). Deployed to `B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll`, 3,368,448 bytes, SHA-256 `c4894a97f3d11b1821e8610ff239d549f6fa1609d8d2c44bf7b42959ae9ca6c6`, previous file kept as `SFShopExplorer.dll.backup-20261004-162728`. Byte checks confirm `reference_toolbar` and `item_render_viewport` are present and `preview_lightbox` is absent. Not yet tested in game.

### 6r-5h - docs refreshed before the 6r-5g test session (2026-10-04)

Artifact under test: `B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll`, 3,368,448 bytes, SHA-256 `c4894a97f3d11b1821e8610ff239d549f6fa1609d8d2c44bf7b42959ae9ca6c6`. That build carries three independent changes: the horizontal wrapping toolbar, the in-place preview expansion, and the CDB-verified playing-card back mapping. All three are in the deployed image; `preview_lightbox` is absent.

Identify this build by size plus hash, not by the log marker: `SFE::kVersion` in `src/ShopExplorer.h` is still `0.8.117`, which several consecutive builds share. Version bookkeeping is also inconsistent in the build files - `xmake.lua` still declares `set_version("0.8.116")` and `COMMONLIB_PLUGIN_VERSION_PATCH 116` against the runtime `0.8.117`. That was left alone deliberately so the deployed DLL stays byte-identical to the verified build; bump all three together in a later change.

Doc updates made for this session:

- `README.md`: controls table now covers the preview interactions (click expand, drag rotate, wheel zoom, double-click reset) and the toolbar combos; the stale claim that the PREVIEW pane is metadata-only is replaced by the real direct-GPU and inventory-capture behavior, the in-place expansion, and the texture resolution order. The build output path was corrected from `releasedbg` to the `build/windows/x64/release/` directory actually in use, with a note that `build.cmd` configures `releasedbg` mode only on first run.
- `SHOP-CATALOG-REFERENCE.md`: documented the toolbar left-to-right wrapping rule and the preview sizing rule; replaced the now-answered "poster card-back bridge" open question with the resolved CDB evidence and added the blister-glass question; removed a duplicated copy of sections 7 and 8 that had been appended twice; refreshed the rendering-notes row in the document inventory.
- Celery, succulent and plant-pot overrides remain untouched; no render path, blending or camera behavior changed in this session.

### 6r-5i - CDB-backed material maps, deck box fix, pop-out preview (2026-10-04)

User test report on the 6r-5g build: cards now render correctly, the deck box is wrong, celery is right but the rest of the produce and aid items are still wrong, and expanding the preview should pop out into its own window instead of growing in place. The live session log identified all three causes.

**Produce/aid (grey, `0/1 textured`).** `Food_Produce_Tomato.mat` resolved to nothing: its map is `textures/items/fooddrink_singleitems/all_packaged_consumables/producebox_produceonly/tomato_base_color.dds`, a sibling directory the fuzzy scorer cannot reach from the material family. The material database has the answer for every base-game material - the run log showed the same failure class across the other produce and aid items.

Fix: `EnsureCdbIndex()` in LiveModel.cpp reads `materials/materialsbeta.cdb` once (~105MB, task thread, blob released afterwards) and builds a stem -> colour/opacity table. Each texture path belongs to the most recent `<name>_TextureSetN` record, so assignment is by order rather than a fixed window; that is immune to the field-name strings (`UniqueMap`) and record tags interleaved between entries, which is what made a window-based scan mis-attribute paths. Two defects were caught by replicating the exact algorithm against the real file before shipping: a case-sensitive `_TextureSet` match found **zero** of 42,934 sets, and the windowed variant bled `Abstract_Poster2x1`'s colour from `_18` and dropped Onion/Letters entirely. The validated version indexes 36,438 sets and resolves tomato, celery, onion, potato, the card symbol decal (colour + opacity), the card letters decal (`astralcolorgradient02_color` with `decallabelsletters01_opacity` - a cross-stem pair the same-stem pairing could never find), and the solvent decal.

Sets with no texture path at all (blister glass, card plastic, card box trim) are dropped so callers fall through to neutral instead of borrowing art. `FindDiffuse` consults the table after the exact overrides and before the fuzzy scorer, with `FindCdbMaterialLocked` used inside it because `g_mutex` is a plain `std::mutex` and a locking call from a locked path would deadlock. `FindDiffuse` gained an optional `a_outOpacity` out-parameter so the database's companion mask reaches the prefetcher.

**Deck box.** The log showed three of the box's six ranges - `ExoticPlayingCardBox01_Trim_Color`, `ExoticPlayingCard_plastic01` and `ExoticPlayingCard_Letters01` - all fuzzy-matched to the card symbol atlas. The card-neutral rule keyed on the node containing `exoticplayingcard_` with a trailing underscore, and the box meshes are named `ExoticPlayingCardBox01:*`, so they slipped through. The rule now covers the whole `materials/setdressing/exotic_clutter/exoticplayingcard/` material family plus any node containing `exoticplayingcard`, and it runs *after* the database lookup so the letters decal keeps its authored texture while trim and plastic stay neutral.

**Pop-out preview.** Clicking the inline viewport opens `##preview_popout`, a sized window (34% of the display width clamped 420-720, 62% of height clamped 420-820) anchored near the shop window. Clicking anywhere outside it returns the preview to the inline strip, which keeps its height and shows a "click away to bring it back" hint. Only one `drawViewport` exists per frame because it owns a fixed ImGui item id. The details column is back to a fixed 340px in both states.

`xmake build -y` passed (only the pre-existing wchar-to-char warning). Deployed to `B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll`, 3,385,344 bytes, SHA-256 `d91d44258b719670e041f5ae622d692dda35435b668010e19eb09426feebd0e9`, previous file kept as `SFShopExplorer.dll.backup-20261004-173327`. Not yet tested in game.

Caveat worth stating: the database maps Solvent_Liquid01 and Solvent_Metal01 to the same decal map the fuzzy scorer already picked, so the solvent preview is unchanged, not fixed; and CDB sets that are empty (Solvent_Metal01 among them) still fall through to the fuzzy scorer.

### 6r-5j - the CDB never loaded: BA2 entry size is the DECOMPRESSED length (2026-10-04)

The 6r-5i test session reported the same three symptoms as before (preview expands in place, deck box wrong, produce/aid still wrong) with celery as the sole correct item. The session log settled all three at once, and the first two turned out to be the *same* bug.

**Root cause: `Ba2Read` read the uncompressed length off disk.** The log carried exactly one line about the material database:

    [17:36:40.550] [W] SFShopExplorer cdb: materialsbeta.cdb unavailable (0 bytes)

Zero bytes, and no `cdb material map` line anywhere in the session - so every base-game material fell through to the fuzzy scorer exactly as before the CDB work. Celery looked correct only because it has a hardcoded exact override.

`Starfield - Materials.ba2` was opened successfully (`archive Starfield - Materials.ba2 (1 entries)`), and its single entry name is exactly `materials/materialsbeta.cdb`, so neither the archive nor the lookup key was at fault. Byte-level parse of the BA2:

    header    hver=2 type=GNRL num=1 nameTableOffset=17576655
    entry     ext='cdb'  e+12 (offset)=68  e+20=17576587  e+24=105037616

`offset + e+20` lands exactly on the name table (68 + 17,576,587 = 17,576,655), which proves `e+20` is the **on-disk** length. `e+24` = 105,037,616 is the **decompressed** length - byte-identical to the extracted file. `Ba2Read` was reading `e+24` bytes from a 17MB file, the short read set the failbit, and the handler's `a_out.clear()` turned that into a 0-byte buffer, which the CDB loader then reported verbatim.

Fix: `Ba2Archive` now carries `dataEnd` (the name-table offset, which bounds the bulk region for any entry) and `Ba2Read` reads `min(size, dataEnd - off)`. This is correct under both conventions - where `size` is the compressed length the `min` is a no-op and behaviour is byte-for-byte unchanged. When `size > streamLen` the field is provably the decompressed length, so the inflate buffer is sized exactly once instead of starting at `size * 4` (a 420MB allocation for the CDB).

This is why the deck box, the produce and the aid items were all still wrong: one reader bug, not three material bugs. With the database actually loading, the ground truth it holds fixes them together - `Food_Produce_Tomato` -> `Tomato_Base_color.dds`, `ExoticPlayingCardBox01_Trim_Color` -> `common/metal/metalpainted01_color.dds` (the white painted box body), `RubberBare01` -> `RubberTexturedCracks01_color.dds`, `ExoticPlayingCard_Letters01` -> `astralcolorgradient02_color.dds` + `decallabelsletters01_opacity.dds`. The card-neutral rule already runs *after* the database lookup, so the box trim and letters keep their authored maps while `ExoticPlayingCard_plastic01` (empty set) still falls through to neutral.

**CDB index moved to startup warm-up.** `EnsureCdbIndex()` is now the last step of `WarmTick()`, so the ~105MB read and parse is paid on the task thread during warm-up instead of holding `g_mutex` on the Present thread during the first material lookup. It stays under the lock because `g_cdbSets` is read by `FindDiffuse`; `g_cdbIndexed` is set on entry so the later first-lookup call is a no-op.

**Pop-out preview.** Two real defects beyond wording:
- Clicking *inside* the popped-out window toggled `previewExpanded` off, so every rotate-click dismissed the preview. The click-to-open gesture is now inline-only; returning it is the click-away handler's sole job, which is what "click away returns it" means.
- The window had an ID-only title (`##preview_popout`), so ImGui gave it `NoTitleBar|SkipItems` and it could not be dragged. It is now `Preview##preview_popout` - visible title, stable id, draggable - keeping `NoCollapse|NoSavedSettings`.

The hints now say what the buttons do ("Click to pop out" inline, "Click outside to bring it back" in the window), and both transitions log (`ui: preview popped out (WxH) for '...'`, `ui: preview returned inline (click away)`) so the next session gives ground truth instead of inference from a screenshot.

`xmake build -y` exit 0 (only the pre-existing wchar-to-char warnings). Deployed 3,387,904 bytes, SHA-256 `b8d5f58f316f2713949a6cc97d0b0720a81bb9e74f98ffea3f471afcb0b326ca`, previous file kept as `SFShopExplorer.dll.backup-20261004-175751`. Verify with `grep -a "cdb material map" SFShopExplorer.log` - that line existing at all is the regression test for this fix.

### 6r-5k - DX10 audit, offline BA2 harness, and the path-key CDB miss (2026-10-04)

Three follow-ups to 6r-5j, driven by the offline harness in `tools/ba2_audit/`.

**1. The DX10 texture reader does NOT have the GNRL size bug.** `Ba2Read` reads entry+24, which for `Starfield - Materials.ba2` is the decompressed length. The DX10 chunk path is already correct: it reads `packed` (on-disk) and inflates to `size` (decompressed) - the opposite convention, used correctly. Byte proof from a 1024x1024 BC1 entry: `packed=11354 size=524288`, and 524288 is exactly 1024*1024/2. v3 archives declare `method=3` and use LZ4, which the plugin already handles.

The harness confirmed both generations end to end, with a pure-Python LZ4 block decoder so the v3 path is genuinely exercised rather than assumed: 4 v2 GNRL archives (126 probes), 4 v2 DX10 archives (40 probes) and 3 v3 DX10/LZ4 archives (30 probes) all decode, and the decoded length matches the stored `size` field exactly every time. Chunk offsets are correct for both generations - v2's `bulk_base = name_table_offset - sum(packed or size)` resolves to real offsets and v3's absolute offsets all land inside the bulk region.

The audit did find one genuine asymmetry: the DX10 chunk read had no `dataEnd` bound, so a corrupt chunk record could read past the bulk into the name table, while `Ba2Read` was already bounded. `ReadTexture` now refuses a chunk whose `off + want` exceeds `dataEnd`. A second known gap, left alone as a performance question rather than a correctness one: there is no negative cache on texture misses, so each failed texture re-walks all 383 archives.

**2. The database is keyed by FULL PATH for two thirds of its materials.** This is the reason "all fruit and veg and aid items" stayed wrong even with the CDB loading. Of 36,438 mappable materials, **26,748 are keyed by full path and only 9,690 by bare stem - and for 26,745 of the path keys the bare stem does not exist as a key at all.** `FindCdbMaterialLocked` reduced the material path to its filename, so it could only ever reach the 9,690. Every packaged food and drink is in the missed group:

    data\materials\items\fooddrink_singleitems\all_packaged_consumables\producebox\packagedfood_tomato.mat
        -> textures/.../producebox/produceboxes..._color.dds
    ...\all_crafted_consumables\aliennuggets.mat, ...\menuitem\menuitem_runnerrush.mat, ...

That is precisely the reported symptom: celery looked right only because it has a hardcoded override, while tomato, onion, potato, the crafted consumables and the menu items all missed. The lookup now tries the normalized full path, then a `data/`-prefixed form, then the bare stem, then the numbered family base. `EnsureCdbIndex` stores path keys slash-normalized so the two sides match. Verified against the real database: 7 of 9 representative materials now resolve by path where stem-only reached 1. `GlassCleanTransparent02` still misses, correctly - its texture set holds only rough/normal maps, so it has no colour to find.

**3. Opacity masks were rejected for being a different size.** `PrefetchTextures` required the mask to match the colour map's width and height exactly. UV sampling is resolution-independent, so that test was stricter than the game's own. The material sweep found 11 of 259 paired colour/opacity sets in a 400-material sample shipping the mask at a different resolution (`decalleavespineneedles01` 2048 vs 1024, `gl_dirt_light01`, `decalwet09`, both UC Marine hand sets), each losing its cutout entirely. The test is now aspect-ratio agreement plus a degenerate-mask guard: a mask under a quarter of the colour map in either axis (4x4 against a 2048 map) is still refused, since it would render the range uniformly transparent or opaque. Format acceptance widened from BC4 only to BC4 or BC3.

**The material sweep** (`tools/ba2_audit/material_sweep.py`) walks every material in the database, resolves each authored colour map through the archive index, and reports unresolved colour maps, missing or mismatched companions and same-stem fallbacks. On a 400-material sample: **2 unresolved colour maps (99.5% resolve)**, 245 correctly paired companions, 11 aspect mismatches (now accepted), 3 companions not found at all, 152 materials with no companion and no same-stem fallback (correctly opaque). `--report` writes the full per-material table.

**CDB component values: not decoded, deliberately.** The schema is now fully mapped, and it corrects two assumptions worth recording. The file is a `BSMaterial::Internal::CompiledDB` (BETH v8): a null-terminated field-name schema to offset 34,436,483, a 16-byte component/edge record table, then 559,897 differentially-encoded strings (`ff ff ff ff` + `DIFF` + base reference + replacement) to end of file. `BSMaterial::EffectSettingsComponent` does carry `IsGlass`, `Frosting`, `FrostingUnblurredBackgroundAlphaBlend`, `FrostingBlurBias`, `ZTest`, `ZWrite`, `BlendingMode` as expected - but `MaterialOverallAlpha` is in **DecalSettingsComponent**, and glass translucency lives in **`BSMaterial::TranslucencySettingsComponent`** (`Thin`, `UseSSS`, `SSSWidth`, `SSSStrength`, `TransmissiveScale`, `TransmittanceWidth`, `TransmittanceSourceLayer`), not in the effect settings. Reading the *values* requires reversing the CompiledDB record framing and each field's width and type, which is not done. No blend constant, alpha value or glass flag has been invented, so blister packaging and the solvent remain exactly as they were.

`xmake build -y` exit 0. Deployed 3,390,464 bytes, SHA-256 `b81aa136b689f0f3c7dd6068acc7f4722f23ee5fa25b29b75675a5fb08e545aa`, previous file kept as `SFShopExplorer.dll.backup-20261004-183324`. The first deploy attempt correctly refused because Starfield was running (PID 9000); it went out once the game closed.

### 6r-5l - texture negative cache, build-time archive gate (2026-10-04)

Runtime test confirmation first: **produce and vegetables now render correctly**, which validates the 6r-5k path-key lookup (7 of 9 representative materials resolve by path where stem-only reached 1).

**Texture negative cache.** `ReadTexture` had its own resolution loop and never consulted `g_failed`, the session negative cache `BuildSingleNif` already used. Every unresolved texture therefore re-walked all 383 archives - re-opening and walking each entry table - on every hover, on the Present thread. It now checks `g_failed` first and records a miss only after every loose dir and every archive has been tried, bounded at 16,384 entries so a pathological content set cannot grow it without limit. Sound because the miss is recorded only once the full `g_ba2List` has been searched and `g_configLoaded` is true, so the directory list is final; a texture that exists is found on the first walk and never enters the cache. The payoff is mostly the opacity companions, since absent masks outnumber absent colour maps.

**Archive gate now runs before every build.** The harness is wired into `build.cmd`, not into an xmake `before_build` hook, because this xmake build (v3.0.9+HEAD) sandboxes the build script: `os.iorunv` raises even for `cmd /c echo`, and neither `task.runv` nor `pcall` is exposed, so a Lua gate cannot spawn the harness at all. Attempting it cost four iterations before that was established; the reason is now recorded in xmake.lua so nobody repeats it. `build.cmd` runs `ba2_audit.py` against one archive of each generation and kind (`Starfield - Materials.ba2` v2 GNRL, `Starfield - Textures11.ba2` v3 DX10/LZ4) and aborts on non-zero exit.

The gate was verified in **both** directions, which matters more than the passing case: it passes on the real archives (exit 0, 4 checks, 0 failures) and, with the exact 6r-5j defect re-injected into `ba2lib.py` (`stream_len = min(ent.size, avail)` reverted to `stream_len = ent.size`), it reports 1 failure and exits 1 without building. One batch trap worth recording: `%VAR%` inside a parenthesised block expands when the block is *parsed*, so the original `set "AUDIT_PROBE=..."` and its `if not exist "%AUDIT_PROBE%"` in the same block read back empty and silently skipped the gate. All variables are now set above the blocks and read with `!VAR!`. Skips cleanly when Starfield is absent (`SFSE_SKIP_BA2AUDIT=1` forces it off, `SFSE_STARFIELD_DATA_OVERRIDE` repoints it).

**CompiledDB value decode: investigated and not feasible in this pass, with evidence.** The schema and layout are mapped, and the structure is now characterised precisely rather than assumed: the file is null-terminated schema strings to offset 34,436,483, then a record region of 559,897 differentially-encoded strings (`ff ff ff ff` + `DIFF` + base reference + replacement) spanning 34,436,505..105,037,293, interleaved with 16-byte record headers (a constant 0x69 field that is presumably a component-type index). A printable-fraction scan over the whole file shows **no separable binary value region** - binary and strings are interleaved roughly 1:1 throughout, at 65.4% printable across the record region. So there is no bulk blob to carve values out of; each component value sits inline between names, reachable only by fully reversing the record framing plus each field's width and type. That is a multi-session reverse-engineering effort, not a lookup. **No blend constant, alpha value or glass flag has been invented**, so blister packaging and the solvent remain exactly as they were, and this is recorded as a known limitation rather than papered over.

`xmake build -y` exit 0. Deployed 3,389,952 bytes, SHA-256 `7e67653bba856bf512f08430bdf32926872a77065bc283540663030421fb470d`, previous file kept as `SFShopExplorer.dll.backup-20261004-190435`.

### 6r-5m - full material sweep, and a real translucency pass (2026-10-04)

**Full sweep, not a sample.** The offline sweep now runs over the whole database rather than 400 materials: **36,373 of 36,438 materials (99%) resolve their colour map**, and only **65 fail**. Companions: 23,070 correctly paired, 679 dimension/format mismatches (accepted by the 6r-5k relaxed rule), 50 companions absent, 13,318 materials with no companion and no same-stem fallback (correctly opaque). Two bugs in the sweep itself were found and fixed while doing this - it indexed the colour map for the opacity comparison without first checking the colour had resolved, so the full run crashed on the first unresolved material with a `KeyError`. Both were latent in the 400-sample runs only because that sample happened to contain no unresolved colour map in the paired path.

So the material system is essentially complete, and the sweep is now cheap enough to run as a standing check rather than an investigation.

**Why clear glass and plastic "did not render".** The session log settles it: 249 `cdb material map` hits and only **11 UNTEXTURED ranges** in the whole session, of which four are glass/plastic (`GlassCleanTransparent02` under two path spellings, `CraftedFood_Container_PlasticPlain01/02`) and the rest are decal sheets that are correctly skipped. So nothing was failing to resolve - the ranges were drawing, as a **flat opaque blob**, because:

1. The database gives these materials an **empty texture set** (`GlassCleanTransparent02_TextureSet1` names only `GlassNoise_rough`, `GlassNoise02_rough`, `GrungeGrime02_rough` - no colour map exists to sample, by design), so there is no albedo.
2. All three existing pipelines are **opaque** (`BlendState.RenderTarget[0].BlendEnable` was never set, and every pixel shader returns `float4(..., 1.0)`).

A display case with no albedo and no blending cannot look like anything but a grey lump. That is the real defect, and it was never a lookup failure.

**Fix: a second, alpha-blended draw pass.** New `kPsBlendTexSrc` pixel shader with the same two-sided lighting as the existing textured shader, a stronger specular term for glass, and a fractional output alpha. It samples the albedo slot, which means it covers both cases at once: an untextured range already binds slot 0, which is the white texture. New `psoBlend` pipeline state uses `SRC_ALPHA`/`INV_SRC_ALPHA` with `BlendOp = ADD` and **`DepthWriteMask = ZERO`**, so a glass shell shows what is behind it and does not occlude later translucent surfaces. The descriptor is reset to opaque defaults after creation. The draw loop is now two passes over the ranges - opaque first, then translucent - with a shared constant-buffer slot counter that is deliberately *not* reset, so every range keeps a unique slot and the 65-slot ring stays within budget. `GpuPipe::kVersion` is 4 so an existing session rebuilds its pipelines.

**This is an approximation, and is labelled as one in the source.** The material's real opacity lives in `BSMaterial::TranslucencySettingsComponent` (`Thin`/`SSSWidth`/`TransmissiveScale`) and `DecalSettingsComponent` (`MaterialOverallAlpha`), whose values are still undecoded. So detection is by material-name token (`glass`, `clear`, `translucent`, `transpar`) and the 0.30 alpha is a chosen constant, not a recovered value. Two deliberate restraints: **plain plastic stays opaque** - only the "transparent"/"translucent" variants blend, because making every plastic container see-through would be wrong far more often than right; and the alpha is a compile-time constant rather than a per-range value, because the vertex constant buffer carries only a `float3` colour plus pad and plumbing a fourth component would have meant editing the interpolation signature of all three existing shaders. `BuildGpuModel` logs `translucent ranges N of M (e.g. '<material>')` once per model, counted after `accumulate()` runs rather than inside the mesh loop (it can run twice - filtered, then unfiltered - so an in-loop tally would double-count).

Two verification notes. The build gate caught a genuine compile error here (`const char* const[]` iterated as `const char**`), and `build.cmd`'s exit status is unreliable when invoked from Git Bash via `cmd //c` - the compile failure printed while the wrapper reported 0 - so final builds are confirmed with `xmake build -y` directly and by checking no source is newer than the DLL. A manual `touch` also makes that check report a false "stale", because xmake keys on content rather than timestamp; `xmake build -r -y` restores a consistent state.

`xmake build -r -y` exit 0. Deployed 3,391,392 bytes, SHA-256 `06721e252ebf3fb269126e0a4290cba22937435fab6b3ee8dd348a4845f38115`, previous file kept as `SFShopExplorer.dll.backup-20261004-193323`.

### 6r-5n - an empty texture set is an ANSWER, not a miss (2026-10-04)

Reported wrong, from one screenshot batch: Action Figure (Sealed UC Army Soldier) front cover, Deck of Cards, Tomato (`0/1 textured`), Solvent (Vital Fluids), Empty Drink Pack, Plant: Starfruit Shrub, Crate of Medical Treatments (black/red noise), London Snow Globe (glass confirmed working).

**The root cause behind most of them.** `EnsureCdbIndex` ended by **erasing every texture set that named no colour map**, on the theory that such a set "carries an empty colour and should fall through to neutral". That conflated two different things. In `materialsbeta.cdb` an empty set is the game's own statement that **the material is flat-shaded and has no albedo** - and the erasure made those materials indistinguishable from "not in the database at all", so `FindDiffuse` ran on and **the fuzzy scorer returned whatever unrelated `_color.dds` it scored highest**. The index goes from **36,438 to 42,933 sets, of which 6,495 are textureless by design**. Empty sets are now kept, `FindCdbMaterialLocked` reports the hit with an empty colour, and `FindDiffuse` stops there with a new log line `cdb flat material '<mat>' (textureless set{, opacity only})`.

Verified offenders, all confirmed empty in the CDB (the texture-set record is immediately followed by the *next* set's name, with no texture path between):

| material | CDB set | before | after |
|---|---|---|---|
| `Supplycontainer_Labels_Green` | empty | fuzzy -> BC4 -> black/red | flat, name says `_Green` |
| `Supplycontainer_Generic_Metal_White` | empty | fuzzy -> BC4 fmt=80 at 512x512 | flat, name says `_White` |
| `Supplycontainer_Generic_Metal_Green` | empty | fuzzy -> BC4 fmt=80 | flat, name says `_Green` |
| `Isotopic_Coolant_Container` | empty | fuzzy -> grey cylinder | flat steel, lid stays textured |
| `WaterCaustic01_IdleAnimLoop` | empty | fuzzy -> unrelated gobo | flat |

This is a deliberate trade-off: 6,495 materials now stop at the database instead of trying a guess. The database is the game's own table and a wrong-family guess is what produced the corruption; the new log line names every such material so any regression is visible in one grep.

**A single-channel map is never albedo.** New `IsSingleChannelFormat()` refuses BC4 (fmt 80, one channel) and BC5 (fmt 83, two) as a diffuse source, logged as `rejected single-channel map as albedo <key> (fmt=80) for <mat>`. This is independent of the change above and would have caught the crate's metal panels on its own.

**A textureless material's colour is in its name.** `FlatBaseColor()` reads the **last** name token (`_Metal_White` -> `white`, `_Labels_Green` -> `green`) and overrides `MeshColor`'s index-cycled grey at range-build time. Last-token matching, not substring: a substring test calls "Predator" red. Sixteen tokens (white/silver/grey/gray/green/red/bronze/gold/black/blue/yellow/orange/brown/tan/beige/purple); no match keeps the neutral grey. `UploadModelTextures` still resets the colour to white for anything that does get an albedo, and no longer does so for a promoted cutout.

**An opacity-only set is a cutout.** A material the database gives no colour map but *does* give a mask is exactly that: a white/tinted shape clipped by the alpha pipeline. `PrefetchTextures` now promotes the mask to the albedo (`maskCutout`, `score -11`) instead of dropping the range. Scoped by construction, not by name test - a set with no colour but a mask can only mean cutout. This is what puts the crate's medical cross and small text back: `Supplycontainer_Decal_Icon_white` and `Supplycontainer_Generic_Letters01` both resolve to `SupplyContainerIcons_opacity.dds` (512x512, BC4, `Starfield - Textures10.ba2`), and `Isotopic_Coolant_Decals` to the lid's own opacity.

**Action figure front cover - root-caused, not yet fixed.** The atlas override is right and the mask pairing was added, but the cover still samples the wrong sub-rectangle, and that is a UV problem, not a texture-assignment one. Evidence, all from decoding the shipped files (`tools/ba2_audit/dump_tex.py`):

* `SFBGS00D - Textures.ba2` contains **exactly one** packaging texture, `actionfigurepackagingart_color.dds` 1024x1024 BC1 - so the existing override names the only possible source. (Listing every `*actionfigure*` texture in the archive rules out a separate front-cover map.)
* The atlas is mostly padding: a grid of soldier variants across the top and the box artwork in a band below. The companion `actionfigurepackagingart_opacity.dds` (1024x1024 BC4) confines the live area to a **small sub-rectangle in the top-left corner**; everything outside is transparent.
* Our render shows the **soldier grid** on the front cover, i.e. the upper part of that live sub-rect instead of the box artwork at the bottom of it.

**A global V flip was considered and ruled out.** `snowglobela_color.dds` was decoded for exactly this: the London globe's `Snowglobe_Landmark_Buildings` mesh samples it with `u=[0,1] v=[0,1]` (full range, from the session log), the skyline sits in the **bottom ~15%** of the texture, and the user's screenshot shows the skyline in the **lower** part of the globe. Sampling V unflipped is therefore correct, and flipping would break the snow globe. (`ParseMeshLod0`'s "preserve the UV convention" comment is vindicated.) The London globe is a poor test of this in general - its interior is 3D geometry, not a plate - which is why the texture was decoded rather than eyeballed.

So the remaining defect is per-mesh UV extraction for the packaging faces (plausibly the per-material UV offset/scale Starfield bakes into the shader properties, or the wrong UV set being taken). **Do not attempt it without a real `BSTriShape` parse**: the packaged NIF is only 18,766 bytes for 24 meshes and 9,313 triangles, which is impossible, so the geometry is not inline and the extracted `actionfigurepackaging_uc_armysoldier.nif` carries no UV stream and no `.mesh` sibling reference. A scan for plausible UV half-float streams in that file found zero candidates. **Next session, paste the log for the action figure:** `PrefetchTextures` already logs `uv '<mat>' v=<n> u=[..] v=[..] tex=<w>x<h>` per textured mesh, which gives the real rects directly.

**The atlas mask pairing is kept** despite the front cover still being wrong, because it cannot make the cover worse: the mask is white exactly where the current wrong content is sampled, so the pairing removes padding spill at the sub-rectangle edges and routes these ranges through the alpha pipeline. It is the game's authored pairing, and it is verified to pair - both maps are 1024x1024, BC4 accepted as a mask.

**Two report items that turned out not to be bugs.** *Empty Drink Pack*: `FoodDrink_Set_DrinkPack02_TextureSet1` is **not** empty - the CDB names `fooddrink_set_drinkpack_decals_color.dds` (1024x1024, `Starfield - Textures05.ba2`) as its colour map, so the small white pouch wearing the shared drink-pack print is correct. *Tomato*: both candidate NIFs resolve cleanly - `food_produce_tomato.nif` -> `Food_Produce_Tomato.mat` -> `Tomato_Base_color.dds` (512x1024), `packagedfood_tomato.nif` -> `PackagedFood_Tomato.mat` -> `ProduceBoxesTomato_color.dds` + opacity. The `0/1 textured` in the screenshot cannot be reproduced from source, and the CDB index log line (`cdb: 36438 material texture sets indexed`) confirms the index built in that session. **Needs a fresh look in-game before assuming a regression.** Plant: Starfruit Shrub is still open - no `starfruit` record exists in the CDB at all, and its NIF is from the `tankgirlsxenologyexpanded` mod, so it needs its own investigation.

**Tooling added** (all in `tools/`, none of it ships in the DLL): `renderer_recon/cdb_strings.py` caches the CDB's 9.7M printable runs to a TSV so a 105 MB scan becomes a grep; `ba2_audit/probe_material.py` runs a NIF material path through the plugin's exact four-step CDB lookup and reports the resulting map, its archive and its DXGI format; `ba2_audit/find_nif.py` searches archive name tables only; `ba2_audit/nif_materials.py` extracts one `.nif` and lists the material strings it references; `ba2_audit/dump_tex.py` decodes BC1/BC4 to PNG so an atlas can be *looked at* instead of guessed at. `material_sweep.py` now mirrors the kept-empty-sets rule (`--drop-empty` restores the old behaviour for A/B) and reports the textureless count.

One bug in the new tooling is worth recording because it nearly sent this the wrong way: `probe_material.py` first indexed only `*[Tt]extures.ba2`, which Git Bash matches case-sensitively, silently covering 10 archives instead of 25 and reporting the tomato's colour map as `missing` when it exists in `Starfield - Textures05.ba2`.

`xmake build -r -y` exit 0. Deployed 3,399,168 bytes, SHA-256 `d27694a6760621d325b5041b29ffab6cbeadcd3f4b557a49fb7125e55b96bbbc`, previous file kept as `SFShopExplorer.dll.backup-20261004-203539`. `SFE::kVersion` and `xmake.lua` are now both `0.8.118` (they were deliberately left inconsistent at 117/116 so the previous deployed DLL stayed byte-identical to its verified build; that constraint is void now that a new build is shipped).

## 6r-6 - the CompiledDB is decoded; per-material alpha ships; UV streams get walked

Three things closed in one session, and one of them invalidates an assumption
the last three sessions were working around.

### 6r-6a - materialsbeta.cdb: the record framing is solved

`materialsbeta.cdb` is a **BETH** container, and it is fully walked now. The
prior note's "CompiledDB binary record framing still undecoded" is closed.

    0x00  "BETH" u32 8, u32 4, u32 <string table end>
    0x10  "STRT" u32 count, then `count` NUL-terminated names (456 of them:
          the reflection name pool - every BSMaterial::* component and field)
    ...   "TYPE" u32 version, u32 classCount(97), then classCount blobs, each
          "CLAS" u32 size + `size` bytes. A blob is a list of 12-byte field
          descriptors: u32 fieldId, u16 typeCode, u16 flags(0xffff),
          u16 byteOffset, u16 byteSize.
    ...   "OBJT" ... then 1,228,218 records.

A record is:

    "DIFF" u32 len, u32 fieldId, u16 kind, u16 count, <elements>

with **`len` counting from the field id at offset 8, not from the tag**, and a
record ending in a `0xffff` / `0xffffffff` terminator. Records for one object
are contiguous and separated by a 2-byte gap, and exactly one of them carries
field id `0x1f4` = `BSComponentDB::CTName::Name`. That name sits in the
*MIDDLE* of its own object - a material stores its float fields, then the
name, then the persistent id - which is why grouping by "records before the
name" is the correct object boundary, not "records after the name".

Element encodings, all confirmed against named objects:

* floats: `(f32 value, u16 indexOfNext)` at stride 6, terminated by a `u16`
  index of `0xffff`. A `float4` therefore arrives as
  `00 00 80 3f 01 00 | 00 00 80 3f 02 00 | 00 00 80 3f 03 00 | a7 e8 e8 3d
  ff ff` = `(1, 1, 1, 0.1097)` - which is exactly
  `ComTextileBlindsTranslucent05_Material1`'s colour.
* strings: `count` bytes, NUL-terminated.
* sub-structs: `(u16 bitOffset, NUL-terminated name)` then `0xffffffff`, e.g.
  `04 00 52 65 64 00 ff ff ff ff` = "channel Red at bit 4".

**The census that matters: across all 105 MB only 43 distinct field ids ever
appear**, and a `_Material` object ever carries exactly two besides its name -
`0x25a` and its own id. Everything else rides on `_TextureSet`, `_Layer`,
`_Blender`, `_UVStream` and `_Cell` objects. Identified members:

| id | object | meaning |
|---|---|---|
| `0x038a` | TextureSet | texture file path (212,877 records) |
| `0x025a` | **Material** | **`MaterialOverrideColorTypeComponent::Color`, XMFLOAT4** |
| `0x022b` | Material | blender name, `Multiply`/`Lerp` |
| `0x0472` | TextureSet | shader route, `HighResUniqueMap`/`UniqueMap`/`DetailMapTiling`/`Tiling` |
| `0x0543` | other | `ShaderRouteComponent::Route`, `Effect`/`Deferred`/`Water`/`PrecomputedScattering` |
| `0x03f9` | Blender | `AlphaBlenderSettings::Mode`, `PositionContrast`/`Linear`/`Additive`/`CharacterCombine` |
| `0x0e39` | other | list of blender names, `Additive`/`Multiply`/`Subtract` |
| `0x02ab` | PermutationLayer | `TextureAddressModeComponent::Mode`, `Clamp`/`Mirror`/`Border` |
| `0x0418`/`0x064d` | Blender/Cell | channel selector, `Red`/`Green`/`Blue`/`Alpha` |
| `0x0376` | PermutationLayer | texture-set kind, `Two`/`One` |
| `0x01f4`/`0x020f` | all | object name, persistent id |

### 6r-6b - there is no translucency number to recover, and that is the finding

The prior note guessed the opacity lived in
`BSMaterial::TranslucencySettingsComponent` (Thin/SSSWidth/TransmissiveScale)
or `DecalSettingsComponent::MaterialOverallAlpha`. **Those components are
declared in the reflection name pool but no field id belonging to either ever
appears in a single record.** Glass materials carry *no* non-default fields at
all: `GlassSmooth01_Base01_Material1`, `DeskGen_Kit_Transparent_Glass_01_Material1`,
`SciBedGlassOverlay01_Material2`, `TranslucentTwoSided1Layer_Material1` and
`TheShard_GlassBroken_Material1` among 170-odd others have nothing but their
name and id. The game's see-through-ness is a property of the **shader
route**, not a stored scalar, so no amount of further decoding will produce a
translucency value.

What the database *does* hold per material is `0x25a`, and its **w channel is
a real override alpha**. Proof that it is the override colour and not a UV
scale or a tiling: the median value tracks the colour token in the material's
own name, which nothing else in the schema does.

| token | n | median R | median G | median B | median A |
|---|---|---|---|---|---|
| snow | 14 | 0.898 | 0.930 | 0.888 | 0.314 |
| white | 137 | 0.744 | 0.656 | 0.592 | 0.922 |
| gold | 32 | 0.694 | 0.552 | 0.614 | 1.000 |
| black | 129 | 0.541 | 0.565 | 0.457 | 1.000 |
| gray | 116 | 0.622 | 0.622 | 0.622 | 1.000 |

Across the 15,226 materials that carry a colour, alpha is 1.0 for 7,010, 0.95
for 1,372, then 0.91, 0.80, 0.12, 0.08, 0.04, and the **decal family sits at a
median of 0.21** - a whole family that used to render as an opaque sheet.

**Shipped.** `EnsureCdbIndex` now runs a second pass over the same blob that
walks the DIFF framing and keeps the first `0x25a` per object (first, not
last: a layered material stores more than one - `ExoticsShell05_Material1` has
two - and the first is the base layer). `FindCdbAlpha` looks it up with the
same four candidate keys `FindCdbMaterialLocked` uses and returns false when
the database has none, so "absent" is never read as "opaque".
`MaterialBlendAlpha` uses that value whenever it is below 0.995, and falls
back to the old name-token heuristic - and still its documented 0.30 - only
for the 82% of materials that store no colour at all, glass included.

Per-range alpha costs nothing: the frame constant buffer already carried
`float3 g_color; float g_pad;` and that fourth float was being written as a
literal `0.0f`. It is now `range.alpha`, renamed `g_alpha` in the shader, and
the blend PS reads it instead of the hardcoded 0.30. No new buffer, no new
bind, no new permutation.

`xmake build -r -y` exit 0. Deployed 3,409,408 bytes, SHA-256
`f78ca8bbccfeb97606b957ef963890fc29c33cadc0cc890b3d314d9d3ec43f53`, previous
file kept as `SFShopExplorer.dll.backup-20261004-213213`. Version 0.8.119 in
both `SFE::kVersion` and `xmake.lua`; `GpuPipe::kVersion` 4 to 5.

**Two bugs the verification caught, both of which would have shipped silent.**
The in-record bounds were first written as if `len` counted from the `"DIFF"`
tag rather than from the field id, so every bound was 8 too large and the
scan returned **zero** keys while looking healthy. And a material with two
override colours took the wrong one. Both were found by
`renderer_recon/cdb_alpha_replica.py`, which re-implements the plugin's walk
in Python and compares it against `cdb_records.py`'s independent parse of the
same file (grouping by name record instead of scanning for DIFF): **10,673 of
10,673 keys agree, 0 mismatches, 0 keys on one side only.** That harness is
the reason to trust the shipped number; rerun it after any change to the
scan.

### 6r-6c - UV streams: the front cover has no UVs to find

The last session's "do not attempt it without a real BSTriShape parse" was
right to demand a parse, and the parse says something different from expected.
The shader block is settled: `BSLightingShaderProperty` is **12 bytes** -
u32 material index, u32, i32 - for all 24 shapes. There are **no UV tiling
floats in the shader block**, so "the per-material UV offset/scale baked into
the shader properties" does not exist and that hypothesis is closed.

Triangles were never the problem either: `ParseMeshLod0` already reads the
u16 index buffer and range-checks every index against the vertex count.

All 24 `BSGeometry` shapes of `actionfigurepackaging_uc_armysoldier.nif`,
swept with `ba2_audit/uv_sweep.py`:

    17 shapes   one UV stream, non-degenerate  -> stream 0, unchanged
     6 shapes   ONE UV stream, fully degenerate (every vertex u=0, v=1)
     1 shape    no UV stream at all

The six are all `actionfigurepackaging_maincolor` - `Planet_logoNewFront025`,
`Planet_logoNewBack030` and `Circle.009`, each a `:0` and a `:1` LOD. Their
stream is a single point, so the old code sampled **one texel of the 1024x1024
atlas** and painted the whole plane with it. That is the actual reason the
front cover looked wrong, and it is not a wrong-sub-rectangle problem: the
mesh carries no rectangle.

**Shipped.** `ParseMeshLod0` now walks the stream list and accepts the first
set whose u or v actually spans a range, skipping degenerate ones, so those
six planes become honestly untextured (and take `FlatBaseColor`) instead of
one arbitrary atlas texel, and log
`skipped N degenerate UV stream(s), using stream M`. The 17 healthy shapes are
unchanged.

What this does **not** do: it does not make the front cover show its artwork.
A plane with no UV data has no rectangle to recover. The packaging texture is
a full-atlas `UniqueMap` that needs per-face rects from somewhere this
session did not find, and the next place to look is
`BSMaterial::GlobalLayerDataComponent::TexcoordScale_XY/YZ/XZ` (in the name
pool, and adjacent to `0x25a` in the schema) rather than the shader block.

### 6r-6d - full offline material sweep: the remaining exposure is 65 materials

`ba2_audit/material_report.py` over all 42,933 texture sets in the database,
against the 133,175 texture names in the 25 texture archives:

    OK               36,373  (84%)   colour map resolves, colour-capable format
    FLAT              5,568  (12%)   database gives no colour map by design
    CUTOUT             927   (2%)   opacity mask only, promoted to tinted cutout
    MASK_AS_ALBEDO        0   (0%)   BC4/BC5 misread as albedo: fully closed
    MISSING              65   (0%)   the work queue

`MASK_AS_ALBEDO` at **zero** is the 6r-5n single-channel refusal measured at
full scale: the black-and-red crate panels class is gone.

`MISSING` is 65 materials in 28 families, and the families are the story -
they are overwhelmingly **not shippable**:

* **20 materials across 7 families are test/debug assets** and never appear on
  a shop shelf: `textures/test/*` (donotuse, test_transmissive01,
  testdebugmat01, utilitydebugchecker01, macbethchart, texeldensitychecker,
  metalroughnessrefgrid, testtextureload_base_01, testmatboy1).
* 4 `textures/lod/generated/composite/cydoniacity` - LOD-swapped Cydonia
  geometry that is not streamed in as a material.
* 4 `textures/effects/*` - VFX sheets (mudsprayblobs, pureblack,
  smokebursttankexplosion, smoketrail ribbons).
* 3 `textures/landscape/plants/shrubs/shrubfringecup` (stems) and
  3 `textures/setdressing/dishtowels`.
* 10 genuine singletons, one each: `outfit_formal_halfdress_bodysuit_dress_01yellow`,
  `nasaposter05`, `shipsigns_01`, `towelrack7-9`, `equinoxlaserimpact_2x2`,
  `bigbolt_chroma_01`, `menuitem_departuretime_liquid`,
  `testgym_flipbookmotionvectorblending`, `explosiontankcryo_smokepillar`,
  `layeredfirev2gasadaptive`.

So **98.5% of base-game materials resolve to a real texture or a correct
intentional fallback, and 99.8% of the residue is unreachable content.** The
only fixable shelf families left are `dishtowels` (3), `towelrack7-9`,
`nasaposter05`, `shipsigns_01` and the one formal half-dress - nine
materials, each needing its own answer. Full table:
`reports/material-verdicts.tsv` (42,933 rows), grouped summary:
`reports/material-report.txt`.

### Tooling added

`renderer_recon/cdb_schema.py` (header, string pool, 97 class blobs, field
descriptors), `renderer_recon/cdb_records.py` (DIFF walk, object grouping,
field-id census), `renderer_recon/cdb_alpha_replica.py` (the plugin's walk
re-implemented in Python, cross-checked against the independent parse),
`ba2_audit/uv_stream_check.py` and `ba2_audit/uv_sweep.py` (per-shape UV
stream tables), plus `ba2_audit/extracted/ba2nameindex.tsv` (name to archive
for 1,196,451 entries, built in 6 s - resolving one `.mesh` without it means
opening 110 archives, which is what made the earlier `--first-uv` run take
twenty minutes and print one shape).

One note on `nif_uv.py`: its `--first-uv` output drove an earlier draft of the
UV work, and the crash in that run's log was a half-applied patch, not a
parser bug. `uv_sweep.py` supersedes it for per-shape questions and reuses its
`Nif` and `parse_mesh_streams`, so there is still one implementation of each.

## 6r-7 - the shader route does not decide translucency; the override colour does

Prompted by 6r-6b's suggestion that the decoded shader-route fields could
replace the material-name heuristic. **They cannot, and shipping them would
have made rendering worse.** Both candidate replacements were measured and
rejected, and the same measurement turned up a change that did work.

### 6r-7a - the shader route has no coverage and no discriminating power

`renderer_recon/route_census.py` joins each `_Material` base name onto its
`_TextureSet` objects and reads the two decoded route fields:

    translucent-word materials (381)     opaque-word materials (2,563)
    <no route record>            349      <no route record>          2,469
    0472:UniqueMap                17      0472:UniqueMap                 46
    0543:Effect                    6      0472:HighResUniqueMap          24
    0472:Tiling                    4      0472:DetailMapTiling          17
    0472:HighResUniqueMap           2      0472:Tiling                    7
    0543:Deferred                  2
    0472:DetailMapTiling           1

Three numbers kill it:

* **4% coverage.** 1,973 of 49,212 material bases carry a route record at
  all, and 349 of the 381 translucent-word materials carry none. A rule that
  is silent on 92% of its target cannot replace a fallback.
* **No separation.** `UniqueMap` tops both lists and `HighResUniqueMap`,
  `DetailMapTiling` and `Tiling` appear in both. The only routes exclusive to
  translucent-word materials are `Effect` (6) and `Deferred` (2) - eight
  materials in the whole database.
* **The fields are not per material.** They ride on `_TextureSet` objects and
  name which *shader* a texture set compiles to, not how opaque the surface
  is. `UniqueMap` is what a fully opaque wall uses.

### 6r-7b - "has an opacity map" is far worse

The other obvious upgrade, and the one with the best coverage story, is worse
than useless. `renderer_recon/translucency_rule.py` scores every candidate
against ground truth - the 4,019 materials the database itself marks
see-through by storing an override alpha below 0.995:

| rule | recall on the see-through set | fires on opaque materials |
|---|---|---|
| A  name tokens (shipped fallback) | 34 / 4,019 = 0.8% | 346 / 45,193 = 0.8% |
| B  texture set names an opacity map | 513 / 4,019 = 12.8% | 23,600 / 45,193 = 52.2% |
| C  A or B | 533 / 4,019 = 13.3% | 23,877 / 45,193 = 52.8% |

B would have put **more than half of all opaque materials** into the blended
pass: an opacity map also means "this is a decal" or "this is a mask", which
is most of the database. It is rejected outright.

The useful reading of this table is about **A**. The name tokens are
high-precision and low-recall - they fire on only 0.8% of opaque materials -
which is exactly the right shape for a fallback that runs only when the
database has nothing at all. They stay, with the reasoning now written down
next to them in `MaterialBlendAlpha` so the next session does not re-propose
this.

The enriched-token analysis in the same script is also worth recording as a
negative result: the name words most associated with see-through materials are
`surfaceb` (848), `surfacea` (832), `ocean` (149) and `ground` (12). Those are
atlas slot names, not translucency words. There is no token set to be had.

### 6r-7c - what the same decode was already throwing away

`0x25a` is a float4 and 6r-6b shipped only `w`. The **rgb is the material's
own tint** and was being discarded - the flat, textureless materials were
taking their colour from a guess at the last token of the material name, or
from neutral grey when the name had no colour word.

`renderer_recon/override_tint_census.py`:

    material bases                49,212
    no colour map (render flat)    6,256  (12.7%)
    carry a CDB override colour   10,666  (21.7%)
    override is visibly tinted     8,421  (79.0% of those that carry one)
    flat AND tinted                4,014  <- rendered as a blob until now

**Shipped.** The index now stores the whole float4 (`CdbOverride`), and
`FindCdbTint` reports the rgb only when the max channel is below 0.97, so a
near-white override cannot flatten a material that would otherwise keep a good
name-derived colour. It is applied after `FlatBaseColor`, so the database wins,
and `UploadModelTextures` still resets `range.color` to white when a real
albedo lands - which means the tint survives on exactly the ranges it is for,
the untextured ones, and never double-tints a textured range. Logged per model
as `N range(s) tinted from the CDB override colour`.

This is the change that should move the most pixels: 4,014 materials, 8.2% of
the database, that render as an untextured blob today.

`xmake build -r -y` exit 0. Deployed 3,412,992 bytes, SHA-256
`eb51ce294b8a68b679c3ec27caf9674d910bd5dd82f55c39d0b51340c80cdefb`, previous
file kept as `SFShopExplorer.dll.backup-20261004-215517`. Version 0.8.120;
`GpuPipe::kVersion` 5 to 6.

`cdb_alpha_replica.py` was extended to compare **all four channels**, not just
the alpha, so the newly shipped rgb is covered by the same harness as the
alpha: **10,673 of 10,673 keys agree on rgba, 0 mismatches.** The spot checks
read the way they should - `agriculturaltools_greenpaint.mat` is
(1.0, 0.932, 0.148), a yellow-green, and `ak_bottle_plastic_grey01.mat` is
(0.517, 0.623, 0.667), a cool grey.

### Tooling added

`renderer_recon/route_census.py`, `renderer_recon/translucency_rule.py` and
`renderer_recon/override_tint_census.py`. All three are measurement tools with
no shipped counterpart, kept because the negative results are the expensive
part: without them the next session re-runs the same three experiments.

## 6r-8 - four diagnoses; two ship nothing, and that is the finding

Four follow-ups taken together. None of them changed the plugin: three
produced a diagnosis that closes the question, and the fourth found a real
general gap whose fix is a different piece of work. Recorded because the
negative results are what stop the next session re-running them.

### 6r-8a - Plushie Mancubus: two independent causes, neither fixable in the preview

Traced end to end with `ba2_audit/mod_material_trace.py`.

1. The NIF is `meshes/kgcdoom/clutter/plush/doom_mancubus_plush_01.nif`,
   inside `Official CK/kgcdoom - main.ba2`. It names one material:
   `\materials\clutter\kgcdoom\plush\doom_mancubus_plush.mat`.
2. **That mod ships its own material database**:
   `materials/creations/kgcdoom/materialsbeta.cdb`, 1,190,891 bytes. It
   contains `doom_mancubus_plush_TextureSet1`, which names
   `Data\Textures\setdressing\KGCDOOM\Plush\Doom_Mancubus_Plush_Color.dds`.
3. **Cause A, general:** `EnsureCdbIndex` resolves exactly one database, the
   base game's `materials/materialsbeta.cdb`. **23 mod archives ship their own**
   - the whole `Official CK` set, `materials/creations/<name>/materialsbeta.cdb`.
   Every material a CK mod invents is therefore "not in the database", which
   routes it to the fuzzy scorer, which only indexes base-game texture archives.
   This is a real plugin gap and it affects all 23, not just this item.
4. **Cause B, specific:** `Doom_Mancubus_Plush_Color.dds` is **not installed**.
   Not in any of the 384 mod archives, not loose. The only kgcdoom texture
   present is `textures/terrain/overlaymasks/kgcdoomdr023world.dds`.

So even with Cause A fixed and `[livemodel] extraba2` pointed at the mod
archive, this item still has no albedo to load: **0/1 textured is the correct
behaviour for what is on this machine.** It is not a regression and there is
no plugin change that would fix it.

Also worth recording, because it explains why the trace looked impossible at
first: the deployed ini has `[livemodel] enabled=0`, `extradirs=` and
`extraba2=` empty. The preview runs the **NativeModel Clone3D** path, reading
the engine's already-loaded mesh - which is how 14,144 triangles were obtained
for a file that no base-game archive contains.

**The actionable item is Cause A**, and it is a separate piece of work:
`EnsureCdbIndex` should read `materials/creations/*/materialsbeta.cdb` from
`extradirs`/`extraba2` in addition to the base game. The scan code is already
generic over the record framing - 6r-6a decoded it - so this is a loop over
more blobs, not a new parser. It will not help this item, only CK mods whose
textures are actually installed.

### 6r-8b - the flat shelf materials are base-game content gaps

`ba2_audit/flat_explain.py`, cross-checking the sweep's work queue against the
override census. Of the shelf items the 6r-6d sweep flagged:

| material | CDB texture set names | CDB override | verdict |
|---|---|---|---|
| `dishtowels01`, `towelrack8`, `towelrack9`, `menuitem_departuretime_liquid` | **none at all** | none | nothing to read |
| `dishtowels03` | `Dish_Towels04_color.DDS` | none | map not shipped |
| `dishtowels04` | `Dish_Towels01_color.DDS` | none | map not shipped |
| `towelrack7` | `HangingTowels+Bar06+08_Base_Color.DDS` | none | map not shipped |
| `nasaposter05` | `ChemPoster04_color.DDS` | 0.886 0.876 0.806, **a=0.0** | tint usable, alpha 0 |
| `shipsigns_01` | `LightningVerticalTile_opacity.DDS` only | none | opacity-only, no colour map |
| `outfit_formal_halfdress_..._01yellow` | `AkilaApex_ScalesandBits_Alpha_color.dds` | none | map not shipped |

**Not one of those textures exists anywhere in the 110 base-game archives.**
There is no `dish_towel*` texture in the entire game, and `chemposter04`
exists as a mesh but has no texture. So the tint cannot explain any of them -
they are missing textures, not missing tints - and no offline change can
produce them. `nasaposter05` is the only one the override reaches, and its
alpha is 0.0.

Two oddities that are the game's, not ours, and are worth not "fixing":
`dishtowels03` points at the **04** map and `dishtowels04` at the **01** map,
and the yellow formal half-dress points at the **scale armour** colour map.
Those are base-game authoring.

Across the whole work queue the override tint explains 1,978 of 5,568 FLAT
materials (36%); 3,590 remain name-word or neutral grey. CUTOUT is mostly
untouched by design - a cutout is mask-driven, not tint-driven.

### 6r-8c - texture address mode cannot fix atlas bleed, and there is nothing to ship

Measured `0x02ab` across the whole database:

    Clamp    12,999 records
    Mirror        10 records   (permutationlayer1/3, uvstream a/b)
    Border         3 records   (uvstream a)
    total     13,012 records, of which 13 are not Clamp

Two reasons not to ship this:

1. **It would change 13 records**, and those live on `_UVStream` and
   `_PermutationLayer` objects, not on materials.
2. **It cannot do what it was proposed for.** Address mode governs what
   happens outside [0,1] or at an edge. Clamp is already what every atlas in
   this game uses and already what the preview effectively gets, so there is no
   wrap-around bleeding to stop. The packaging art problem is that the mesh
   carries **no rectangle at all** (6r-6c: the six `maincolor` faces have one
   degenerate UV stream, u=0 v=1), and Mirror or Border would smear the
   neighbouring sub-rectangle across the plane - making it worse.

The real fix remains finding the per-face rect, i.e.
`GlobalLayerDataComponent::TexcoordScale_XY/YZ/XZ`.

### 6r-8d - naming all 43 field IDs is blocked, with the blocker identified

Not done. The blocker is specific and worth recording so it is not re-attempted
blind: **the header string pool is a globally deduplicated name pool, not a
positional dump.** Zipping the 97 class blobs against it by order fails for
exactly that reason - `BlendModeComponent` is followed immediately by
`ColorChannelTypeComponent`, and `BlendMode` appears 1,500 names later at the
point where the pool happens to introduce it. The earlier count mismatch
(95 reflection types vs 97 blobs, almost all field counts disagreeing) is the
same fact seen from the other side.

The 12-byte class descriptor is `(u32 fieldId, u16 typeCode, u16 flags,
u16 byteOffset, u16 byteSize)`. The `0xffff` in the flags slot means "no
default recorded", and the byteOffset/byteSize pair is a confirmed packed
struct layout - verified against blob 60, whose nine fields sit at byte offsets
0,1,4,8,12,16,20,21,24 with sizes 1,1,4,4,4,4,1,1,4 and type codes 0xff10
(bool) / 0xff11 (u32). **There is no name index anywhere in the descriptor**,
so an id-to-name map cannot be read out of the file.

What would unblock it, in order of cost: (a) find the game's own reflection
serialisation and read the ids the engine assigns, which is what the ids
almost certainly are - `0x25a` decodes to something sensible only because its
values looked like a colour; (b) find a newer CK database whose pool is
per-type rather than deduplicated; (c) accept value-based inference per field,
which is what produced the 14 of 43 named so far.

Until then, 41 of 43 ids stay inferred from their values, and the notes table
in 6r-6a records which ones and how confident each is.

---

## 6r-9 - mod material databases: Cause A shipped

§6r-8a closed on the Mancubus plush with two separate faults: the mod ships its
own CDB naming a texture that is not installed anywhere (Cause B, unfixable and
correctly left alone), and **the plugin never read mod CDBs at all** (Cause A,
general, and now fixed). Cause A was the whole gap: `EnsureCdbIndex` resolved
exactly one hardcoded key, `materials/materialsbeta.cdb`, so a Creation Kit mod's
own material database - the only statement that mod makes about its own
materials - was invisible.

The sub-problem was worth checking before writing any C++, and it passed:
`tools/renderer_recon/cdb_mods.py` extracts a mod database and runs the same
two passes the plugin uses. It is the same BETH/DIFF framing at a smaller scale,
so this is a loop over more blobs, not a second parser.

### What shipped

`IndexCdbBlob(base, size)` now holds both passes, verbatim, and returns the
counts it produced. Neither pass overwrites an existing value - pass 1 uses
`try_emplace` for the key and fills `color`/`opacity` only while they are empty,
pass 2 uses `emplace`. So "index the base game first, then the mods" *is* the
entire precedence policy: a mod cannot shadow the game's own answer for a
material the game already knows, and can only fill a gap. No separate merge
logic, no flag on the indexer.

`EnsureModCdbIndex()` is deliberately **lazy**, and this is the one design
choice in the change. Indexing the 23 mod databases eagerly means opening every
mod archive on the first material lookup of the session, which is precisely the
cold-start cost the mesh-path work went to lengths to avoid. Instead it fires
from `FindCdbMaterialLocked` on the first material the base game does **not**
know - for a base-game item that essentially never happens, and for a mod item
it is exactly the miss that needs answering. The candidate-key resolution is
factored into a `resolve()` lambda so the retry is one extra line rather than a
duplicated ladder. One-shot via `g_cdbModsScanned`, on the task thread, under
the lock the caller already holds. Capped at 32 databases, which is 23 real
ones plus headroom; the cap is insurance against a pathological modlist turning
one miss into unbounded multi-megabyte decompressions, and it logs if it bites.

It looks in both places a mod database can live: loose
`<root>/materials/creations/<mod>/materialsbeta.cdb` (one directory listing per
mod root, immediate children only - not a tree walk) and archived, by scanning
`g_ba2List` entry names for `materialsbeta.cdb` under `/creations/`. Archive
names are already `LowerAscii`'d with `\` folded to `/` by `OpenBa2`, so case
and separator variants match. `g_ba2List` is mod-first, so a mod's own database
is always seen before anything it replaces.

### Measured, on this host

`python cdb_mods.py --merge`, after making the replica's texture-key test
byte-faithful to `CdbTextureKey`:

    23 mod databases        850 texture sets        0 overrides
    501 keys new to the index
      423   name a colour map
      385   ...and that colour map IS installed   <- these go from grey to textured
       38   name a colour map that is not installed anywhere
       78   name no colour map at all (40 have an opacity companion only)

`cdb_alpha_replica.py` re-run after the refactor: 10,673 keys, 0 mismatches,
exit 0 - the base-game path is byte-for-byte unchanged.

Two measurement notes, both of which changed the answer:

- **Overrides are structurally absent from mod databases.** All 23 decode 0
  `0x025a` records. So `FindCdbAlpha` and `FindCdbTint` gain nothing from this
  change, and deliberately do not trigger the scan - alpha misses are the common
  case (82% of base-game materials store no colour) and paying a full archive
  scan for the first of them would have reintroduced the eager cost.
- **Checking a mod's colour map in the archive that named it reads 0 present,
  and that is a false negative.** Creation Kit splits `main.ba2` from
  `textures.ba2`, so `cyberthornmantis_color.dds` is named by
  `kgcdoom - main.ba2` and ships in `kgcdoom - textures.ba2`. Checking across
  all 352 mod archives gives **389 of 458 distinct colour maps present, 0 in
  base game, 69 nowhere**. `cdb_mods.py --merge` now prints that warning, since
  it is the failure mode that would make this change look pointless.

The 38 that name an uninstalled texture are Cause B and stay grey - correctly.
`doom_mancubus_plush` is one of them; §6r-8a's conclusion is unchanged, Cause A
never was going to fix that specific item. What it fixes is the other 385.

No loose mod CDB exists on this host, so the loose-directory branch is untested
by live data - it is exercised by the same `ReadLoose` path the mesh lookup
already uses, and it is the branch that would catch a hand-installed CK export.

### Still open

Cause B (mod textures that the mod names but does not ship) is not a plugin
problem and not fixable here. The 41 of 43 unnamed field ids are unchanged from
6r-8d. And no appearance claim in this section is confirmed: this host cannot run
Starfield, so 0.8.121 is verified by build, by the byte-level replicas above,
and by the deployed DLL's markers only.

### End-to-end reachability (measured, not assumed)

Indexing a material is only half the path - the colour map it names still has
to be findable by `ResolveAsset`. Two links in that chain were checked rather
than trusted:

- **`g_moBa2` really contains the mod texture archives.** `DiscoverMoMods`
  collects `*.ba2` **non-recursively**, one level inside each mod folder, so an
  archive in a subdirectory would be invisible to the whole plugin. On this
  host all 384 mod archives are at `<mods>\<ModName>\<file>.ba2`, i.e. exactly
  the depth `useDir` walks, and all 389 of the resolved colour maps live in
  those. Zero unreachable.
- **`g_ba2List` cannot be cached before MO2 discovery runs.**
  `LoadConfig` sets `g_configLoaded = true` at line 1359 but calls
  `DiscoverMoMods` at 1380, and `EnsureBa2List` latches its result for the
  session. That window looks dangerous and is not: `LoadConfig` holds
  `g_mutex` for its entire body, and every public entry point takes
  `std::scoped_lock` on the same mutex *before* testing `g_configLoaded`, so no
  other thread can observe the flag before discovery has populated `g_moBa2`.
  Confirmed by reading both sides rather than by assuming.
---

## 6r-10 — opacity verdict memo, persisted CDB index, and a dead end on field names

Three pieces of work in one version (0.8.122).

### A. `g_diffuseMemo` was discarding the opacity verdict

The 40 mod opacity-only keys flagged at the end of 6r-9 turned out **not** to be
a new lookup gap. Measured first, before changing anything:

- base game: 42,934 keys, 927 opacity-only (448 installed), 5,569 truly flat;
- mods (new keys only): 501 keys, 40 opacity-only (28 installed), 38 truly flat.

The 40 are 20 blood-decal materials (`kgcdoomblooddecal1..9` plus `_glow`
variants), 4 paint-splatter decals and ~10 VFX/particle materials (flame,
tesla arc, energyflame, coldsmoke, force field, simpleglow) — **not** the
e-ink counter family 6r-4b described. My prediction was wrong; the measurement
was not.

**The promoted-mask path already handles them.** It is generic code in
`OfflineModel::PrefetchTextures`, not a name test, and the real log proves it
end to end:

```
cdb flat material 'Materials\Common\Decal\DecalMetalBevels03n.mat' (textureless set, opacity only)
prefetched '...\decalmetalbevels03n.mat' <- textures/common/decal/decalmetalbevels03_opacity.dds (score fallback)
```

Only **1 of 58** `cdb flat material` lines in the whole session carried an
opacity map — the base-game case is rare, which is exactly why the bug below
survived three turns of log reading.

#### The real defect (order-dependent, and therefore invisible)

`FindDiffuse` memoised **only the colour verdict** and threw the opacity away:

```cpp
std::unordered_map<std::string, std::string> g_diffuseMemo;   // was
```

So for a textureless-set material that ships a mask, the **first** lookup
promoted the mask correctly (the memo was still empty, so the live path ran),
and every **later** lookup of the same material+node hit the memo, got an empty
colour, and returned `false` **with an empty opacity** — silently dropping the
mask and leaving the range flat. In a log that resolves each material exactly
once, this is invisible by construction.

Fixed by memoising the verdict as a pair:

```cpp
struct DiffuseVerdict
{
    std::string color;    // resolved albedo key, "" = none
    std::string opacity;  // companion mask, "" = none
};
```

and re-emitting `*a_outOpacity = it->second.opacity` even on the flat verdict.
Eight call sites changed (declaration, memo hit, `exactTexture` lambda, CDB
colour branch, CDB flat branch, card-neutral branch, and two fuzzy-tail sites).
All eight verified by reading back the edited regions after the build.

### B. Persisted CDB index (`cdbindex.bin`)

Building the index parses a 105 MB database and, on first miss, opens every mod
archive. That is a fixed per-session cost landing on the first hover. The result
is a pure function of *(plugin build, game archives, mod set)*, so it is now
written to `<cacheDir>/cdbindex.bin`.

**Validation is stat-only and deliberately cheap.** `CdbIndexStamp()` encodes
`v<kVersion>;s<schema>;a<archive count>;t<total archive bytes>;d<dataDir>`.
It fingerprints the archive *list* by `file_size()` and **never opens an
archive** — validating 494 archives by opening them would cost everything the
cache saves. A mod added, removed or re-downloaded moves the count or the byte
total, the stamp moves, and the cache is rebuilt. That is the failure mode we
want; the one we must never have is serving a stale index.

Fail-closed throughout: bad magic, wrong stamp, truncation, or an absurd count
(all > 1,000,000) all return false and the caller parses as before. Records are
read into **local** containers and only assigned to `g_cdbSets`/`g_cdbAlpha`
after every check passes, so a partial read can never land. Every length read
from disk is bounded by `kCdbMaxStr = 4096` before it is trusted, so a corrupt
or hostile file cannot over-allocate. Writes go to `.tmp` then rename.

The `modsMerged` flag is written into the header: a warm session that loads an
already-merged index skips `EnsureModCdbIndex` entirely (`g_cdbCacheHasMods`),
which is what delivers the "skips the mod archive scan entirely" half of the
goal. The base-only save still happens if the mod merge never runs, so a
session that never misses a base-game material leaves a valid, smaller cache.

`kVersion` is in the stamp, so **the version bump is load-bearing**: a wrong
stamp would make the very first warm session a miss. That is why the bump is
`0.8.122` and not left at `.121`.

#### Verification

The round-trip was exercised outside the game by
`tools/renderer_recon/run_cdb_cache_harness.sh`, which **slices the cache block
out of `LiveModel.cpp` at build time** (marker to end of
`SaveCdbIndexCacheIfCurrent`) and compiles it against stub declarations. The
harness therefore tests the shipped bytes and cannot drift from the source.
**34 cases, all pass**: miss, hit, deep per-record compare of all 42,934 sets
and 10,673 overrides, merged-flag survival, stale stamp from an archive-count
change, older plugin version, stamp sensitivity to the archive list (add /
resize / determinism), corrupt magic at two offsets, **truncation at 7
points** (never yields a partial index), absurd set and alpha counts, over-long
stamp and key lengths, empty file, zero-record file, save into a missing
directory tree, and the empty-cache-dir no-op.

Note one thing the harness deliberately does **not** claim: a single flipped
payload byte is accepted, because the format has no per-field checksum. What is
guaranteed is that *structural* damage is rejected and never produces a partial
index.

### C. Naming the 41 field ids — **dead end, reported honestly**

6r-8d's blocker was that the header name pool is globally deduplicated and the
12-byte class descriptor `(u32 fieldId, u16 typeCode, u16 flags, u16
byteOffset, u16 byteSize)` carries no name index. I tested that conclusion
hard rather than assuming it, and it **survives** — but the investigation turned
up a real bug in our own tooling on the way.

#### The bug: `cdb_schema.py` counts a class-header record as a field

Raw-byte probe (`cdb_clas_probe.py --raw`): **every** CLAS blob begins with a
record shaped `(typeCode=0x0000, flags=0x0000, byteOffset=0, byteSize=n-1)`
where `n` is the blob's record count. **78/97** blobs match that shape exactly
and **zero** later records in any blob have it. The leading `u32` is the class's
own id — it is what a DIFF record names to pick its component type.

`cdb_schema.py` has been returning that header as "field 0", so:

- every class's field list was offset by one ("field k" was really field k-1);
- 97 class ids were counted as field ids;
- the distinct-id census was **456**; the corrected figure is **359 distinct
  field ids + 97 distinct class ids = 456** with **zero overlap**, which is
  exactly the pool size.

Field slots: **562 before, 465 after** removing the headers. Field-id reuse:
305 ids used once, 38 twice, 8 three times, and one id (`0x0150`) used by 16
classes — surplus 106, matching by construction.

#### The map hypothesis, and why it fails

If the pool is the concatenation of per-class runs in class order, then
`(class name, then its field names, deduplicated)` emits exactly **456** ids —
and the pool has exactly **456** names. That is either the map or a
coincidence, so it got a falsification test rather than a shrug
(`cdb_field_map.py`):

- **Positional pairing**: qualified names (`BSMaterial::Foo`, 88 of 456) must
  land on the 97 class positions. Observed **20**, chance expectation **18.7**,
  p(shuffle >= observed) = **0.41** against 2000 draws. `MAP NOT SUPPORTED`.
- **Raw index into the full STRT table** (`cdb_strindex.py`): ids run
  0x0000..0x2175 (8565) and STRT declares 8573 strings, which looked very
  promising. It is not: class ids land on type-shaped names **1/97 (1.0%)**
  against a **1.06%** chance rate, and 456 ids collide onto 345 distinct
  strings. `INDEX MAP NOT SUPPORTED`.
- **Constant offset / affine / XOR** (`cdb_stroffset.py`): swept 6001 additive
  offsets, 5 multipliers, and 14-bit XOR masks. Best result **8/97**. That
  beats the *single*-offset null p99.9 — and is still noise: 6001 offsets were
  tried, `P(single draw >= 8)` is about 1e-5, so ~0.1 offsets are expected to
  reach 8 by chance, and **a real map would score near 97/97, not 8**. An
  earlier version of that script printed `OFFSET MAP FOUND` from a single-draw
  comparison; that verdict was wrong and is corrected in the script.

Also checked: the STRT count field (8573) covers the *whole* table, of which
only the first 456 are schema names — the rest are material paths, so it
cannot be used as the schema pool bound. And no `.cdb` file exists anywhere
loose on this host (walked both the MO2 mods root and the game `Data` tree,
0 hits), so there is **no newer CK database on this machine** to compare
against.

**Honest status: Task C is not solved.** 14 of 43 fields remain named by value
inference; the rest plus the 97 class names still have no mapping. What did
come out of it is the corrected schema census above, which matters because any
future field-id work has to start from the corrected counts, not the ones in
6r-8d. Unblocking would still need the engine's own reflection serialisation or
a CK database not present here; neither was reachable from this host.

### Verification for 0.8.122

- `xmake build -r -y` clean (12.06s), only the pre-existing LiveModel warnings.
- `cdb_alpha_replica.py` exit 0.
- `cdb_mods.py --all` 850 sets / 23 databases, exit 0; `--merge` exit 0.
  New-key count is **508** with **429** colour maps, against 501/423 recorded
  in 6r-9 — 850 sets and 23 databases match exactly, so this is drift in the
  installed mod set, not a regression in the plugin.
- Cache harness: 34/34.
- Deployed `dc598ce315a8f7c5b0c2bf04227c6e3ae9bda7dc2cda2cc073dbdb4f669cf318`
  (3,429,888 bytes) == build SHA-256; markers `0.8.122`, `index loaded from
  cache`, `index cached`, `mod materialsbeta.cdb indexed`, `cdb flat material`
  all present via `grep -aqF`; backup `SFShopExplorer.dll.backup-20261005-143837`;
  Starfield closed at deploy time.
- **The DLL hash is not reproducible across rebuilds.** Three builds of
  unchanged source gave `c814225e...`, `4fe98083...`, `dc598ce3...` at an
  identical 3,429,888 bytes — the link embeds a build timestamp / PDB GUID. So
  "build == deployed" has to mean "deployed from the current build", not "hash
  equality across runs"; verify with the marker strings and the byte count,
  and never treat a hash mismatch alone as evidence of a source change.

**Not verified: anything visual.** No appearance claim here is confirmed in
game — everything above is source, CDB-bytes and DDS-decode only. The 40
mod materials now resolve their mask on every lookup instead of only the first,
and the first warm session should show `index loaded from cache` in the log
after one cold run.
#### First live 0.8.122 session — Task A confirmed by the log

The user's own session (plugin load `16:34:56`, base index `16:35:37`) closed
the biggest gap above. It resolved the *same* material three times in one
session, which is exactly the case the old `g_diffuseMemo` broke:

```
[16:37:18.498] live: cdb flat material 'Materials\Effects\Weapons\AmmoCounters\AnimatedLetterLineGlow01.mat' (textureless set, opacity only)
[16:37:18.502] offline GPU: prefetched '...' <- textures/common/decal/decalletterlineoverlay01_opacity.dds (score fallback)
[16:37:18.513] live: cdb flat material '...' (textureless set, opacity only)
[16:37:18.515] offline GPU: prefetched '...' <- ..._opacity.dds (score fallback)
[16:37:18.520] live: cdb flat material '...' (textureless set, opacity only)
[16:37:18.522] offline GPU: prefetched '...' <- ..._opacity.dds (score fallback)
```

3 lookups, 3 opacity resolutions. Under the old `unordered_map<string,string>`
memo lookups 2 and 3 returned an empty opacity; the `DiffuseVerdict` fix
resolves all three. Note this is a cross-stem pairing again — stem
`AnimatedLetterLineGlow01` vs texture stem `decalletterlineoverlay01` — which
same-stem pairing could never have found. Still **no pixel-level confirmation**;
the log proves the mask path resolves, not what it looks like.

Corrections to numbers quoted from this host's tool runs (the live mod set has
drifted since 6r-9/6r-10 — these are the session's real values, not a regression):

- `42934 material texture sets indexed (6496 flat/textureless)` — flat is
  **6,496**, not the 5,569 in the 6r-10 census; base override colours are
  **11,536**, not 10,673.
- `32 mod materialsbeta.cdb indexed (6648 texture sets, 15268 override colours)`
  — **32 databases**, not 23; the 23-database figure predates the drift.
- Cache saved twice by design: base-only at `16:35:37`
  (`42934 texture sets, 11536 override colours`), then re-saved merged at
  `16:40:07` (`49582 texture sets, 14158 override colours`). 42934 + 6648 =
  49582 exactly.
- Cold base parse cost **~41 s** of session time (load `16:34:56` -> index
  `16:35:37`). That is the number the warm run has to beat.
- The opacity-mask marker in the log is **`(score fallback)`**, not
  `(score -11)`; `score -11` occurs 0 times. Score values on other prefetch
  lines are large positive numbers (482 at `score 1900`).

`index loaded from cache` has not appeared yet — only one launch so far, so the
warm path is still unexercised in game.


---



This section records what source and retained data establish; it does not claim
that every game asset renders correctly. The strict acceptance bar is per asset:
identify the selected model and mesh ranges, resolve each material's intended
maps, verify each read/format/channel and blend rule, submit the draw, then
compare visible pixels against the game. A successful earlier stage is not a
substitute for a later one.

### Verified renderer path

The shop can display a direct NativeModel GPU preview or the separate captured
inventory-scene route; do not conflate their evidence. In the retained
2026-10-10 v0.8.123 Razorback direct-GPU log, NativeModel produced 52 meshes /
39,849 triangles from three paths; the task thread published the model; Present
matched 41/52 ranges, set up the 512x384 pipeline, submitted draws and bound the
preview SRV. This proves submission, not visible pixels. No screenshot/frame
was attached to that session. The direct-model path and ItemCapture path are
separate (see §6r-12).

In source, texture reads and prefetch happen on the task thread before GPU
upload; Present consumes prefetched data. The GPU renderer uses opaque and
translucent passes, uses an alpha-aware PSO for decal sheets or ranges with an
opacity slot, and binds colour, opacity and optional detail SRVs. A missing
texture/read fails closed (untextured/base-colour range, or skipped mesh as
applicable); it is not evidence that the resulting appearance is correct.

### Material and mask policy supported by evidence

- CDB texture sets and loose material albedo are separate resolution inputs.
  Base-game CDB is indexed first; mod material databases are lazily merged on a
  base-index miss, preserving base answers. The mod CDB path is supported by
  source and the recorded 0.8.122 log, including repeated opacity-only
  resolution after the memo was changed to retain both colour and opacity.
- An opacity-only CDB material can promote its mask to a tinted cutout. This is
  generic (not a material-name allowlist), and the memo regression is covered by
  the three repeated live log resolutions recorded in §6r-10.
- BC4 format 80 reads the default red component; BC3 format 77 selects alpha.
  Source, HLSL selector and `test_decal_mask_channels.py` agree. BC4 masks are
  not required to equal colour dimensions, but the current aspect-ratio check
  rejects some otherwise readable pairs.
- Guessed `_opacity.dds` siblings must not be presumed authored. In the retained
  Razorback session, 36 reads failed as `fmt=0 0x0` while 12 pairs succeeded;
  the wider 0.8.122 census logged 508 empty reads among 513 rejected attempts.
  The follow-up live probe establishes a narrower result: the installed BC4
  Tombstone map decodes through the plugin, while the deliberately absent key
  and the Razorback receiver sibling both return `fmt=0 0x0`. The base-game
  census found that sibling absent from 25 texture archives; MO2 overrides did
  not supply it. Thus this specific miss is a guessed path absent from the
  installed sources, not a general BC4/DX10 read failure. This does not prove
  all other failed siblings are absent or unnecessary. The incomplete
  `ba2nameindex.tsv` is not authoritative for texture existence.

### Razorback grip detail: source-backed, not yet visually established

The Razorback grip and barrel decals share `materials/weapons/razorback/decals.mat`.
The current offline prefetch recognizes the `decals_grip` node, chooses the
standard wood atlas and coverage mask before consulting the shared loose .mat
albedo, and separately attempts `textures/weapons/razorback/fancy/decals_color.dds`
as an engraving signal. It attaches that detail map only when the read succeeds,
format is BC1/BC4-family-supported by the code (format 71 or 28), and dimensions
match the wood atlas. A shader mode composites sampled detail as grayscale
relief while retaining standard wood colour and coverage.

The extracted texture-review artifacts and UV analysis establish that the
Fancy colour atlas has horse-mark content at the grip UVs while the Fancy
opacity atlas is blank there. This supports the authored-detail choice and
explains why using Fancy colour as the base map would be wrong. However, the
retained Razorback selection predates this detail-map change: no later
engraving-detail load log, mode-2 binding, or post-change screenshot is in the
retained evidence. Successful detail lookup and visible engraving are therefore
unverified. Require a fresh selection, the load log and mode-2 draw, plus an
in-game comparison before claiming it renders.

### Coverage statement and remaining unknowns

There is no defensible claim that the renderer can render *everything*
correctly. The retained session validates one weapon's geometry-to-submission
chain; screenshots in other notes validate only their named items, views and
routes. Unverified classes include arbitrary mod assets, all opacity-only or
translucent materials, channel conventions outside BC3/BC4, missing/guessed
texture companions, all CDB tint/substance overrides, unusual UV/atlas layouts,
and visual equivalence between direct previews and inventory captures. The
material matcher can also lack node names in prebuilt `.sfmodel` caches, limiting
within-item disambiguation when multiple meshes share a material.

The retained evidence includes the completed positive/negative reader probe
and a user-supplied direct Razorback screenshot. These confirm the known BC4
read and that this direct preview reached the child window. They are bounded to
one tested sibling and one item/session, not a complete fidelity test: there is
no matching inventory reference, post-detail-map capture, mask-cutout
comparison, or representative mod-asset matrix. Conclusions apply only to the
specific probe outcomes and displayed preview, not universal render
correctness.

### Source and test anchors

- `src/OfflineModel.cpp`: texture selection/prefetch, Razorback grip detail,
  opacity pairing and GPU draw submission/shader modes.
- `src/LiveModel.cpp`: archive texture lookup/decode, material/CDB resolution
  and texture index/cache.
- `tools/renderer_recon/test_decal_mask_channels.py`: BC3-alpha / BC4-red
  shader/source regression checks.
- `tools/renderer_recon/test_live_texture_probe.py`: positive/negative reader
  probe regression checks.
- §6r-10 through §6r-12 above: CDB memo evidence, reader probe and supplied
  preview screenshot with remaining visual caveats.

*Status: investigation report only; no renderer source or runtime configuration
was changed.*

## 6r-11 - UI pass: theme legibility, banner layout, text input, and a deploy that shipped nothing

A presentation-only pass over the shop chrome. No renderer or material work.
Two findings are worth more than the changes themselves: a contrast bug that
predated this work, and a build/deploy path error that made most of the pass
unobservable in game.

### 6r-11a - the deployed DLL was stale for an entire session

`build.cmd` configures the `releasedbg` mode, so xmake writes to
`build\windows\x64\releasedbg\`. The deploys in this pass copied from
`build\windows\x64\release\`, a directory left over from an earlier
configuration. It stopped being written at 20:35 and kept serving a frozen
3.5 MB binary while the real build was 1.9 MB.

Every deploy in the pass *reported* success and every build *reported* clean,
because both were true. The mismatch was only visible by searching the
deployed binary for a string the change introduced:

```
bgg  utf16 at -1     in the deployed DLL
bgg  utf16 at 741616 in the releasedbg build
```

So the banner work, the search-typing fix, the double-typing fix, the backdrop
change and the `InkOn` button contrast all built correctly and were all absent
from the running game. What the user actually saw after each of those was the
unmodified UI.

Fixed three ways, because a single fix would leave the trap armed:

1. Deploy from `releasedbg`, hash-verified against the build.
2. Deleted the stale `release` directory so the wrong path cannot be used.
3. `build.cmd` now fails if the `releasedbg` output is missing and echoes the
   built DLL's SHA-256, so a stale deploy announces itself at build time
   instead of at screenshot time.

**Lesson: a successful build is not a deployed build.** The verification that
would have caught this — searching the *deployed* artefact for a marker string
from the change — is cheap and belongs in the deploy step, not just the log.

### 6r-11b - Hazard body text was near-black on a dark panel (pre-existing)

Not caused by this pass. Measured from the user's screenshot rather than
assumed:

| element | palette contrast | measured in shot |
|---|---|---|
| item names (`ink`) | 2.59:1 | 2.22:1 |
| `muted` | 1.93:1 | — |
| TYPE/VALUE/WT/FORMID (`gold`) | 1.35:1 | — |

`gold` was `0.20,0.22,0.25` — dark steel on a `0.24` panel, which is why the
numeric columns had effectively vanished. That came from the 15:58 repalette
that made Hazard a "light steel-grey shell", where `gold` was repurposed from
gold to steel but left at a value indistinguishable from the panel.

Two rounds of ink tweaking were the wrong lever: on a mid-grey panel no red
reaches 4.5:1 without going neon, because red is intrinsically dim. Darkening
the panel is what bought the room:

| | before | after |
|---|---|---|
| panel | 0.285 | 0.135 |
| `ink` | 2.59:1 | 5.64:1 |
| `muted` | 1.93:1 | 4.48:1 |
| `gold` | 1.35:1 | 8.77:1 |

`gold` is now pale steel at ~8:1 and deliberately *not* red, so the numeric
columns stay scannable and do not compete with the names.

A mid-course correction worth recording: the first "calmer red" proposal
(`0.85,0.35,0.32`) was offered as *dimmer*, but it holds the same 0.62
saturation as the rejected hot red and only cuts luminance — it would have
reintroduced the readability problem it was meant to fix. Saturation, not
brightness, was the complaint.

### 6r-11c - InkOn(): contrast on filled widgets, once, for every theme

Widgets filled with a theme accent were drawing `ImGuiCol_Text`, which is body
ink. On Hazard that is red on hazard yellow. Rather than special-case one
theme, `InkOn(fill)` picks black or white from the fill's own luminance:

| fill | luma | picks | new | was |
|---|---|---|---|---|
| Hazard button | 0.731 | black | 11.16:1 | 1.55:1 |
| Devilz Red button | 0.110 | white | 14.96:1 | 5.80:1 |
| Hell button | 0.353 | white | 4.58:1 | 1.78:1 |

Hell was also unreadable at 1.78:1 and self-corrects. Applied to Refresh,
MAXIMIZE/WINDOWED and the NAME/TYPE/VALUE/WT/FORMID sort headers, which were
accent-filled too.

### 6r-11d - banner: three layout bugs, each from a wrong assumption

The header went through three iterations. All three failures came from
assuming rather than checking, so they are recorded as failures:

1. **Logo off-centre.** One fixed column plus one stretch meant the logo
   drifted right by half the difference between them. Computed equal-width side
   columns fixed the centring and broke the layout: on a 2559px window two
   ~1130px fixed columns plus inter-column padding exceeded the available width,
   and ImGui resolved the overflow by *collapsing the middle column* — which
   dragged the actions column back under the title and shoved the logo against
   the right edge. Correct construction is two **equal-weight stretch** columns
   around one fixed centre column: stretch cannot overflow.
2. **Title size had no effect.** `ImGui::PushFont(font, 0.0f)` does not mean
   "use the font's baked size" — in ImGui 1.92+ `0.0f` means *keep the current
   size*, so the larger atlas entry was loaded correctly and then rendered at
   body size. Correct call is `PushFont(font, font->LegacySize)`.
3. **`TableSetupColumn` 4th argument.** It is `ImGuiID`, not a weight; the
   stretch weight is the 3rd. Passing `1.0f` there was a float→ImGuiID
   narrowing that also happened to be redundant (stretch already defaults to
   weight 1, which is what the centring relies on).

Search is right-aligned by measuring the block and shifting the cursor, because
ImGui has no right-to-left flow and the pieces must be emitted left-to-right to
keep their order. Verified from the screenshot: title right edge 598, logo
centre 1266 against window centre 1279, search and tagline sharing a 13px right
margin.

The logo is drawn from the window draw list after `EndTable` rather than as a
column, so it can be any size at no cost in row height and can overhang both
sides without being clipped.

### 6r-11e - text input needed synthesised WM_CHAR, then de-duplication

Symptom: the search box took focus but nothing typed. Arrow keys, Enter and
Escape worked, which is the signature — ImGui was receiving key *events* but
never key *characters*.

`WM_CHAR` is produced by `TranslateMessage`, not by the keyboard. Nothing in
this codebase calls it, and (re-derived 2026-10-17) the engine calls it at only
two sites - the message pump at 0x189df20 (PeekMessage -> TranslateMessage ->
DispatchMessage, 8 callers) and a second loop at 0x189d520 (1 caller) - neither
of which is on the path the shop is open on. See
reports/engine-claims-reverification-2026-10-17.md; the latch below stays
correct. The overlay therefore now synthesises characters from `WM_KEYDOWN` via
`ToUnicodeEx`, which is the correct call because the character depends on
layout, Shift, CapsLock and AltGr rather than the virtual key alone. It runs
only on the make side (`ToUnicode` consumes dead-key state) and skips control,
function and navigation keys, which are already handled as events.

That produced **double typing**: the game *does* call `TranslateMessage` on some
other path, so both sides produced a character. Two guards:

- `g_hostTranslatesChars` latches on the first genuine `WM_CHAR`; synthesis then
  stops for the session. One real character proves the host translates.
- Per-batch de-duplication, because the latch cannot help the *first*
  keystroke: a synthesized character followed in the same batch by a real one
  with the same code point is dropped. `InputMessage` carries a `synthesized`
  flag rather than comparing timestamps, so the pairing is exact.

Untested: dead-key layouts. `ToUnicodeEx` can return multiple code points for
one AltGr keystroke and each is passed through separately, which will be wrong
for compose sequences.

### 6r-11f - pop-out preview could not be moved

`SetNextWindowPos` and `SetNextWindowSize` were both called with
`ImGuiCond_Always`, so ImGui applied the drag and then immediately overwrote it
on the same frame. Position is now forced only on the frame the window opens
(new `popJustOpened` flag) and neither is set afterwards, which also required
dropping `NoSavedSettings` — otherwise ImGui's ini has nothing to persist.

### 6r-11g - model preview backdrop

The clear colour was hardcoded `0.72,0.75,0.82` — a light blue-grey that read
as a product photo and fought every dark theme. Now a dark slate
(`0.055,0.070,0.095`) close to the game's own inventory background, overridable
per channel via `[offlinepreview] bg/bgg/bgb` as 0–255. Any channel left unset
keeps its default, so a partial override tints rather than replaces.

Not a gradient: the model's lighting is unchanged, so on a much darker field
some dark materials lose definition against it. If a model reads murky, raise
the value rather than shifting the hue.

### 6r-11h - backdrop strength became a setting

The themed `space_*.png` art ranges from a near-black nebula to a bright
inferno — measured mean luminance 0.048 for the old shared backdrop against
0.443 for the new `space_hell.png`, p95 0.921. Drawn at a fixed alpha 190,
the bright art washed the 72%-alpha panels to a mid-orange and `muted` text had
nothing to sit against. `[overlay] backdropAlpha` (default 150) exposes it so
bright themes can be dialled back without a rebuild.

**Not verified: visual confirmation of anything in 6r-11b through 6r-11h.** All
of it shipped in the same session as the deploy-path failure, so none of it has
been seen in game yet. The first launch after `03A26CD0…` should confirm the
banner geometry, search typing, button contrast and preview backdrop together.

### Deployed build

`03A26CD0CF3DCE13AF173E0E7BC7C7578EE079DB86C2F9BDE17A0A889DBCD07B`, 1,878 KB,
from `build\windows\x64\releasedbg\`, hash-matched to the deployed copy,
Starfield closed at deploy time.

Note the contradiction with the 0.8.122 record above, which says the hash is
not reproducible across rebuilds because the link embeds a build timestamp.
Both are true: the hash still cannot prove *which source* produced a DLL across
runs, but comparing build output to deployed copy is still exact and is the
check that would have caught 6r-11a.

## 6r-12 - resume audit: direct GPU preview reaches the display path; opacity reads fail (2026-10-10)

This is a log/source audit only. Starfield was not running during this pass,
so there is no new screenshot or live visual confirmation. The latest retained
plugin log is from 2026-10-10 00:30; the last actual selection activity is at
00:25:59. The game process is now stopped.

### What is positively verified in the retained session

At 00:23:57 the plugin loaded as v0.8.123. At 00:24:15 it read the game's
installed INI with `renderMode=4`, input enabled, and item-capture mode 2.
There is one complete selection-to-GPU-submission chain for Razorback:

- NativeModel extracted 52 meshes / 39,849 triangles from 3 model paths.
- The task thread published that direct model as ready.
- The Present thread built the 512x384 GPU pipeline, matched textures for 41
  ranges out of 52 prefetched meshes, submitted the draws, and bound preview
  SRV slot 20. There is no logged renderer/pipeline failure in that chain.
- An UnarmedHuman selection shortly before it resolved to zero meshes, which
  is consistent with a record that has no static model and is not evidence of a
  GPU failure.

This proves geometry-to-texture-prefetch-to-render-target setup and command
submission. It does **not** prove the pixels visible to the user are correct:
there is no screenshot/frame capture attached to this log, so orientation,
framing, clipping, material appearance, and whether the custom child actually
shows that SRV still need visual confirmation.

### Current concrete render-texture defect: guessed opacity maps are often absent

For the single Razorback selection, the task log recorded 36 opacity rejections
and 12 successful pairs. Every rejection was `fmt=0 0x0`; successful pairs
reported actual BC4/BC3 dimensions. The rejected names include mechanically
derived siblings such as `razorback_receiver_opacity.dds`,
`cylinder_opacity.dds`, and `ammo_opacity.dds`, while the corresponding
`*_color.dds` albedos loaded. Since `PrefetchTextures` derives
`_opacity.dds` whenever a color key ends in `_color.dds`, the missing read
alone does not show that a material authored or needs that mask.

The code fails safely today: a failed mask read is not bound as an opacity
texture, and an ordinary albedo range still draws through the regular textured
shader. But if a material genuinely requires that mask for cutout coverage,
its holes/decal edges will be filled. This matches the broader earlier census
in §6r-10 (508 empty reads among 513 rejected pairing attempts); do not change
the reader or weaken its validation based only on a filename guess. Next
renderer task should distinguish actual missing files from packed-decoding
failures using a known installed mask with positive control: verify its archive
entry in the plugin's warm index, call `ReadTexture`, record archive/key,
format, dimensions and decode result; then compare a deliberately nonexistent
sibling. Preserve the current valid BC4 red / BC3 alpha channel checks.

### Other display-path caveat and next test

`ItemCapture` is configured in mode 2 but that proves only the diagnostic
capture module is enabled; its session summary and selected candidate are not
in the retained tail excerpt, so capture success is not established here. Do
not infer that the shop preview uses it: direct NativeModel rendering and the
item-scene capture are separate routes. The UI source selects the captured
scene only under its own display condition; otherwise its live direct-model
SRV is the preview route.

Resume with a known static weapon (Razorback) and one simple misc model. Save a
screenshot of the shop preview and the inventory reference at the same camera
orientation. For the render path, check that the preview is ready and compare
orientation/framing and visible texture detail; for mask work, test a known
BC4 opacity texture plus a deliberately absent derived path and record exact
archive/decode outcomes. Do not use an NPC/unarmed zero-geometry row as the
geometry test. Keep `OfflineModel::ServicePending` doing file/BA2 work off the
Present thread, and avoid guessed engine vtable calls or resource-state writes.

The next step is investigation and evidence capture, not another broad
renderer rewrite. Rebuild/deploy only after a source-level defect is isolated
and regression coverage is added.

### 6r-12 follow-up probe — fresh launch and archive positive control (00:43-00:47)

A new launch was available during this continuation. The plugin again loaded
v0.8.123 with `renderMode=4`, warmed all 383 BA2s, and indexed 13,070 opacity
maps. Cache rebuild hit the known `empty texture-set key` miss, then indexed
42,977 material texture sets and decoded 15,243 override alphas; that cache
issue is separate from rendering and is not addressed here.

At 00:47:14 the Razorback chain reproduced: 52 prefetched meshes, 41 ranges,
then preview SRV slot 20. All missing Razorback receiver/cylinder/ammo/sights/
barrel/grips opacity siblings were logged `fmt=0 0x0`; known BC4 decals were
successfully paired. The missing lookups repeated for duplicate material
ranges, so rejection-line counts are not unique-file counts. This continuation
ended with Starfield closed and no screenshot saved; pixels and actual UI
presentation therefore remain unverified.

I also ran the repository's Python BA2 replica against the installed game's
25 base `*Textures*.ba2` archives. A decisive positive control was found in
`Starfield - Textures11.ba2`: `textures/weapons/tombstone/tombstone_decals_opacity.dds`
is present as BC4/DXGI 80 at 1024x1024, and the reader replica decoded the
selected 0-mip chunk to 524,288 bytes without error. This control is a separate
Tombstone asset, not a mask required by Razorback. The guessed
`textures/weapons/razorback/razorback_receiver_opacity.dds` is absent from
those base-game archive indexes. This strongly supports the current
filename-derived miss diagnosis rather than a blanket DX10 decode failure.

A fresh live run now confirms the plugin reader itself: after the 383-archive
warmup, it loaded the positive control from `Starfield - Textures11.ba2` as
BC4/80, 1024x1024, pitch 2048, 524,288 bytes. Both the definitely-absent key
and the Razorback-derived receiver sibling returned false with zero format,
dimensions, pitch, and bytes. This agrees with the base archive name-table
probe: no broad DX10 failure is indicated; the Razorback receiver sibling is
missing in the base game. MO2 loose-file overrides were still traversed by the
live reader but none supplied that candidate, as shown by its complete miss.

### Live positive-control results (02:05:24)

- `known-bc4` — `textures/weapons/tombstone/tombstone_decals_opacity.dds`:
  `ok=true`, source `Starfield - Textures11.ba2`, fmt 80, 1024x1024, pitch
  2048, 524,288 bytes.
- `definitely-absent` — the diagnostic-only absent key: `ok=false`, source
  reports no successful source, fmt 0, 0x0, 0 bytes.
- `razorback-derived` — `textures/weapons/razorback/razorback_receiver_opacity.dds`:
  `ok=false`, same no-source/empty result. The archive positive control and
  negative cases therefore distinguish a functioning BC4 DX10 read from these
  missing-key cases.

### Reproduced Razorback GPU submission (02:06:40-02:06:41)

In the same live session, NativeModel extracted 52 meshes / 39,849 triangles
from three model paths. The Present-side direct GPU path prefetched 52 textures,
reported 41 rendered ranges, and bound preview SRV slot 20. BC4 opacity maps
were accepted for `glowsolidline`, Novablast counter numbers, and the Solstice
Decals material; guessed Razorback receiver/cylinder/ammo/sights/barrel/grips
siblings still missed (`fmt=0 0x0`). Thus geometry, texture read, and SRV
submission reproduced with the deployed probe build. The two zero-mesh native
preview messages were separate NPC/unarmed selections and not evidence of a
general geometry failure.

### Attached screenshot evidence (02:14 capture)

The user supplied [the live Razorback preview screenshot](tools/renderer_recon/extracted/razorback_live_preview_20261010.png) from the current run.
It visibly shows the Razorback centered in the popped-out Preview panel, in a
horizontal side view with the barrel to the left and grip to the right. The
preview status reads `direct 3D | 52 meshes | 39849 tris | 41/41 textured`,
and the details pane identifies Razorback FormID `00000FD6`. This is direct
visual confirmation that the submitted geometry and preview SRV appear on
screen, closing the earlier unknown about whether the SRV reached the child
preview window. The captured model is recognizable, but the screenshot does
not establish inventory-camera parity or full material fidelity: some surfaces
look muted/flat, and the repeated receiver alpha-sibling misses remain. No
inventory reference appears in this frame, so final comparison and opacity
cutout verification are still open.

The startup probe runs once after archive warm-up. The screenshot and log now
confirm the geometry-to-preview display path, but not full material correctness
or inventory parity. Next compare Razorback and one simple misc preview against
their inventory views, checking framing, texture fidelity, and for filled
cutouts. Keep file/archive I/O on the task thread and do not rewrite renderer
code based on submission evidence alone.


## 6r-12 original evidence sources

- The original 00:30 log supplied the first Razorback submission trace.
- `src/OfflineModel.cpp`: texture prefetch/mask admission, material pass and
  D3D12 SRV submission.
- `src/ImguiOverlay.cpp`: shared/theme art upload and preview texture selection.
- `src/ItemCapture.cpp`: mode-2 candidates bind direct SRVs only; capture is
  distinct from the OfflineModel render target.
- `src/NativeModel.cpp`: source of the 52-mesh Razorback geometry used above.
- The attached [Razorback preview screenshot](tools/renderer_recon/extracted/razorback_live_preview_20261010.png) verifies it is displayed; inventory-view parity, full material fidelity, and mask-cutout correctness remain unverified.

- `42934 material texture sets indexed (6496 flat/textureless)` — flat is
  **6,496**, not the 5,569 in the 6r-10 census; base override colours are
  **11,536**, not 10,673.
- `32 mod materialsbeta.cdb indexed (6648 texture sets, 15268 override colours)`
  — **32 databases**, not 23; the 23-database figure predates the drift.
- Cache saved twice by design: base-only at `16:35:37`
  (`42934 texture sets, 11536 override colours`), then re-saved merged at
  `16:40:07` (`49582 texture sets, 14158 override colours`). 42934 + 6648 =
  49582 exactly.
- Cold base parse cost **~41 s** of session time (load `16:34:56` -> index
  `16:35:37`). That is the number the warm run has to beat.
- The opacity-mask marker in the log is **`(score fallback)`**, not
  `(score -11)`; `score -11` occurs 0 times. Score values on other prefetch
  lines are large positive numbers (482 at `score 1900`).

`index loaded from cache` has not appeared yet — only one launch so far, so the
warm path is still unexercised in game.


---



#### Opacity pairing: partial success, and a large empty-read class (0.8.122 session)

**Correction first.** An earlier note in this file claimed opacity pairing had
"0 successes". That was wrong, and the cause was my own shell pipeline, not
the plugin: `grep -ao "SFShopExplorer[^\"]*opacity[^\"]*" | sort -u | head -20`
truncated the output, and `opacity map rejected` sorts before `paired opacity`,
so the successes were cut. The success branch at `OfflineModel.cpp:1565` has
been firing since `16:37:18.505`. Lesson: never put `head` after a `sort -u` over a mixed-result grep.

Live counts (log still growing, taken ~17:0x):

| outcome | lines | distinct textures |
|---|---|---|
| `paired opacity` (success, `:1565`) | 346 | 66 |
| `opacity map rejected` (`:1568`) | 513 | 151 |

So pairing succeeds for ~40% of attempts by line count, ~30% by distinct
texture. It is **not** broken wholesale.

Rejection breakdown (513): **508 `fmt=0 0x0`** and **5 `fmt=80 1024x512 vs
1024x1024`**. An earlier chat claim of "498 fmt=0" was hand-added from a
`head`-truncated histogram; the true count at that moment was 500, and 508 as
the log grew.

The real finding is the **508 empty reads**: `fmt=0 0x0` is not an unsupported
format (that would still report real dimensions), it is a total read failure.
150 distinct opacity textures fail this way. User-visible instance: **Giant
Heart** (`Crafted_InHailer`) loads its colour map at `score 1900` but rejects
`inhaler_opacity.dds (fmt=0 0x0)`, so it renders with no cutout.

The 5 `fmt=80` cases are a different, milder class: a valid BC4 mask refused
purely by the aspect check (1024x512 against a 1024x1024 colour).

**Cause still NOT determined.** `:1524` derives `_opacity.dds` from a
`_color.dds` key when the CDB supplies none, and a guessed path is
indistinguishable in the log from a real one. Do not try to settle it with
`tools/ba2_audit/extracted/ba2nameindex.tsv` — that file is **not authoritative
for textures**: 0 hits for `inhaler` even though `inhaler_color.dds`
demonstrably loads in-game, and only 7,882 `.dds` rows in 106 MB. Use the
plugin's own archive warm index (13,070 `_opacity` maps found at `16:35:37`).

#### Two visual corrections from user screenshots

- **London Snow Globe is fine.** The shop preview showed a pale sphere at
  "4/10 textured" and the log logs
  `Snowglobe_Landmark_ApolloLandingSite.nif` at 9 meshes / 8,981 tris while the
  preview read 10 meshes / 6,152 tris — a different NIF. The inventory render
  shows a fully textured globe (London skyline, Ferris wheel, snow, trees,
  gold-rimmed wooden base). The shop preview was showing a different or not yet
  resolved model. Do not "fix" this.
- **Captain Galaxy's Note** renders as a flat untextured light-blue book in the
  inventory view while the shop preview showed a dark cover. Untriaged.

#### Warm-cache relaunch and ammo screenshots (17:42-17:45)

The user closed Starfield, then relaunched at `17:42:34`. The cache did **not**
hit: base CDB parsed at `17:43:07`, mod CDBs merged at `17:45:26`, and no
`index loaded from cache` line appeared. This is a cold run, not a warm-pass.

The current run then saved the merged cache. Verified file:
`B:\MO2\Starfield\overwrite\SFSE\Plugins\SFShopExplorerAssets\models\cdbindex.bin`,
10,014,479 bytes, mtime `17:45:26`. Header is schema 1, `modsMerged=1`,
49,582 texture sets, 14,158 override colours, stamp
`v0.8.122;s1;a383;t195221793475;dB:\\SteamLibrary\\steamapps\\common\\Starfield\\Data`.
The startup log reports active MO2 Test profile 36/560 dirs, 141 mod BA2s,
383 total BA2s. `offlinepreview usecache=0` only disables `.sfmodel` disk
caching; it does not disable the separate CDB index cache.

**Likely cache miss cause, not conclusively proven:** the previous cache was
built from a different archive list/profile; notes from its build record 494
archive inputs, while the current profile indexes 383. The old cache was
replaced by this run's save, so its stamp cannot now be read to confirm the
mismatch. Current cache stamp and merged contents are verified; next launch
should be the real warm test and should log `index loaded from cache`.

The new inventory screenshots confirm the **40mm XPL** and **Light Particle
Fuse** models and their labels/textures render correctly. (Talon Bolt was also
confirmed in the preceding screenshot.) These are visual checks of ordinary ammo
previews, not the separate weapon HUD ammo-counter mask test.

---

## 6r-13 — audit of 2026-10-17: the CDB cache never hit, and five warning-class defects

Scope: current log, the three dump CSVs, the docs, and the plugin sources
(10 translation units, 24,974 lines). Five defects fixed, one duplicate left
in place and documented, one visual defect fixed by approximation and not yet
seen in game.

### 1. The persisted CDB index was written, then rejected on every launch

The session log at `07:37:43.884` carried the only warning in 448 lines:

```
[W] SFShopExplorer cdb: cache miss (empty texture-set key) at
    ...\SFShopExplorerAssets\models\cdbindex.bin
[I] SFShopExplorer cdb: cache miss; rebuilding index (381 BA2 archives, ...)
[I] SFShopExplorer cdb: 42977 material texture sets indexed (6547 flat/textureless)
[I] SFShopExplorer cdb: index cached at ... (42977 texture sets, ...)
```

So the same run wrote the file and the next run threw it away. Both shipped
files were parsed directly (magic `SFECDB1\0`, schema 5, string records):

| file | stamp | sets | alphas | empty set keys | trailing bytes |
|---|---|---|---|---|---|
| `MO2/overwrite/.../models/cdbindex.bin` | `a381` | 42,977 | 43,543 | **1** (record 33,375) | 0 |
| `Data/SFSE/Plugins/.../models/cdbindex.bin` | `a250` | 44,582 | 52,834 | **1** (record 33,942) | 0 |

Cause, in `IndexCdbBlob` (`LiveModel.cpp`): a CDB object name ending in
`_textureset<digits>` starts a texture set, and the key is
`objectName.substr(0, suffixAt)`. For a name that is *exactly*
`_textureset1`, `suffixAt` is 0 and the key is the empty string.
`try_emplace("")` inserted it, `SaveCdbIndexCache` serialised it verbatim, and
`LoadCdbIndexCache` treats an empty key as corruption and discards all 42,977
records — the whole file, for one nameless entry. The measured cold parse was
~41 s on the 42,934-set run (notes 6r-12); after this fix the stamp and the
payload are the same on every launch, so that cost is paid once per archive
change instead of once per launch.

Fix: skip `suffixAt == 0`; the writer also drops empty keys and derives the
header counts from the records it actually writes, so a header can never
promise more records than the file holds. Four new cases in
`cdb_cache_harness.cpp` (§12) cover it: the cache must load with an empty key
present, the real entry must survive, and neither map may persist an empty key.
They fail against the pre-fix writer.

Not yet exercised warm in game: the next launch should log
`index loaded from cache`. Until then this is a source-and-harness result.

### 2. Four `wchar_t` -> `char` narrowings and two dead locals

A clean rebuild with `/W3` reported six warnings in plugin code, all real at
the source level:

- `LiveModel.cpp` — the MO2 profile name was narrowed for a log line, so a
  Unicode profile name printed mojibake.
- `ShopExplorer.cpp` — `[shop] skipmodels` entries were narrowed, so a
  non-ASCII model path silently stopped matching.
- `ItemCapture.cpp`, `OfflineModel.cpp` — the INI number parsers narrowed
  before `std::stod`, which the `std::wstring` overload makes unnecessary.
- `ItemCapture.cpp` — the capture `menuName` was narrowed the same way.
- `NativeModel.cpp` — an unused `left` read of the pool entry; `LiveModel.cpp`
  — an unused `hb[48]` buffer in the typesync diagnostic.

Fix: one `NarrowUtf8` in `PCH.h` used by all four sites, `std::stod` on the
wide string in both parsers, both dead locals removed. `PCH.h` now includes
`<Windows.h>` *after* SFSE.h — CommonLib raises C1189 on the reverse order —
and `#undef ERROR`, because `wingdi.h` defines `ERROR` as 0 and every
`REX::ERROR` call then fails with C2589. The full rebuild is warning-free.

### 3. `build.cmd` hashed the DLL before building it

The deploy guard and the SHA-256 both ran before `xmake`, so:

- every successful build printed the **previous** DLL's hash under the label
  `built DLL sha256` — exactly the stale-binary trap README.md warns about;
- a fresh checkout, or any tree after `xmake clean -a`, failed with
  `no build output` and never compiled at all.

Fix: build first, then check and hash. The config step now also re-runs
`xmake f -p windows --toolchain=msvc` when the build tree is missing, because
after `xmake clean -a` xmake falls back to the mingw toolchain and zlib then
fails to compile against the MSVC headers (`error C2143` in `zlib.h`).
Verified from a deleted `build/`: configure, full 23 s rebuild, exit 0, hash
printed equals the DLL on disk.

### 4. Planter soil rendered as a neutral grey mound

`DirtPacked01_SoilBowl_Soil01.mat` is a textureless CDB set, so
`FlatBaseColor` had no colour word to recover from the name and fell through
to `MeshColor`'s grey palette; the log shows the range `UNTEXTURED` and the HUD
`1/3 textured`. `soil`, `dirt` and `earth` are now in the name-colour table
(0.34/0.26/0.18). Like the wood veneer entry, this is an approximation from the
name, not a value read out of the game.

### 5. Duplicate left in place

`LowerAscii` exists twice, in `LiveModel.cpp` and `OfflineModel.cpp`, and the
two differ: the live copy also maps `\` to `/`. Consolidating them needs a flag
at every call site, which buys nothing behaviourally, so both stay. Everything
else flagged as a duplicate by name (`Install`, `Tick`, and the `Cdb*` cache
helpers) is either a distinct class method or a single definition in one file.

### Verification for this pass

- CLEAN rebuild of all 10 TUs: **0 warnings, 0 errors** (was 6 warnings).
- `tools/ba2_audit` gate: OK (`archives: 1 checks: 3 failures: 0`).
- 10 Python suites exit 0: catalog filters, audio lifecycle, weapon mod
  selection, decal blend policy, decal mask channels, DXGI colour space, live
  texture probe, razorback badge composite, weapon material diagnostics,
  CDB material sets (13 tests).
- `run_cdb_cache_harness.sh`: **ALL PASS**, including the four new empty-key
  cases.
- Deployed to `B:\MO2\Starfield\mods\DevilzDad's ESM Shop Mod Explorer\SFSE\Plugins\`,
  SHA-256 `A4867ACA34D58E4437604A32C8C4A7F776AB1A2E03C35F3A46F4161D2609C032`,
  identical to the build output.

### Still open after this pass

- Warm-cache launch unverified in game (see §1).
- The soil colour and the wood veneer colour are approximations, not
  game-derived; neither has been seen on screen since the change.
- BreechBlock's "missing" opacity maps do not exist in
  `SFBGS050 - Textures.ba2`; the log lines are correct and the grey unmasked
  look still has no established cause.
- This audit covered the cache, the text conversions, the build script and the
  flat-colour table. It is not a line-by-line review of the 5,131-line
  `LiveModel.cpp` matcher or the 3,665-line `OfflineModel.cpp` renderer.

- Matcher review: **MATERIAL-MATCHER-AUDIT.md** lists all twelve
  resolution paths and seven open findings. The two with demonstrated
  impact are a 4x4 placeholder albedo accepted as art on Coachman decals
  (renders one flat pink-mauve) and the CDB cross-family gate rejecting
  the game's own Coachman maps on every attempt (17 log lines, 5 materials).

### CDB family gate: family-extension directories (F2 of the matcher audit)

The gate required a CDB weapon map to sit in `/weapons/<family>/` exactly. The
game files the Coachman materials under `Materials\Weapons\Coachman\` but ships
their maps in `textures/weapons/coachmanshotgun/`, so all five Coachman
materials were rejected on every resolution (17 log lines in the audited
session, colour *and* opacity) and each node re-scanned the archives for a map
the database had already named. `WeaponDirNamesFamily` (`LiveModel.cpp:4036`,
called at `:4142`) now accepts a `/weapons/` directory segment that equals the
material's family token or continues it by at least three characters. Directory
segments only - part names in a file name are still not evidence - and the
generic-token blocklist is unchanged, so `magstorm` still cannot serve
`regulator`.

- Every shipped `coachman_*` CDB set points into `textures/weapons/coachmanshotgun/`,
  and all 13 maps they name (12 colours plus `decals_opacity.dds`) exist in
  `Starfield - Textures10.ba2`: the change cannot trade a fuzzy hit for a missing
  texture, and the decal set regains its authored mask.
- Shipped-database census: the CDB contains exactly 10 family-token to weapon-directory
  prefix relations and all are the same weapon (`coachman`/`coachmanshotgun`,
  `orion`/`orionrifle`, `pacifier`/`pacifiershotgun`, `inflictor`/`inflictorpistol`
  and `rifle`, `arboron`/`arborondagger`, `side`/`sidestar`, `hardta`/`hardtarget`,
  `ammo`/`ammocounters`, `lasercutter`/`lasercutter_worn`). Nine flip from reject
  to accept; `lasercutter_worn` already passed via the existing `/weapons/<family>_`
  test. The three recorded cross-family traps are rejected before and after.
- `tools/ba2_audit/test_cdb_material_sets.py`: 16 tests, OK, three of them new.
  27 matcher-adjacent tests, OK. The `ba2_audit` gate ran clean on
  `Starfield - Materials.ba2` (1 check) and `Textures11.ba2` (3 checks).
- New `tools/ba2_audit/family_gate_diff.py` compiles the gate **out of the
  source** and checks the Python replica against it over 4,957 pairs: 0
  disagreements. On its first run it found a real replica bug (a material with
  no directory under `materials/weapons/`), which was fixed in the replica.

#### The SHA-256 identifies a build, not a source revision

Deployed to `B:\MO2\Starfield\mods\DevilzDad's ESM Shop Mod Explorer\SFSE\Plugins\`,
SHA-256 `B78D4BC64265764A...` (2,032,640 bytes), byte-identical to the current
build output (`cmp`). Starfield was closed at deploy time.

While restoring a rollback copy, rebuilding the *pre-change* source produced
`a3a4fbc746678f80`, **not** the `a4867aca34d58e44` recorded for the previous
deploy of that same source. This build is therefore not byte-reproducible
(embedded linker metadata), which has two consequences for this repository's
deploy rule:

- "verify the built DLL's SHA-256 against the deployment record" only works for
  the build output that was actually deployed. A later rebuild of identical
  source will legitimately hash differently, and treating that as a stale-binary
  failure would be a false alarm.
- No `.bak-20261017` copy was left. The previous binary could not be reproduced
  byte-exactly, so a backup under that name would not match its recorded hash;
  the rollback path is reverting the one documented function and rebuilding
  (5 s). The 08:13 deploy of `a4867aca` this morning kept no backup either.

#### Still open in this pass

- In-game confirmation: no session has run since the deploy, so the expected
  log change (CDB score 1900 plus `paired opacity` for the Coachman materials
  instead of fuzzy scores 1223-2513) is a prediction, not an observation.
- F1 is untouched: the Coachman decal still wears the 4x4 flat pink-mauve
  placeholder, now via the CDB and with its mask restored. The colour-map
  minimum-dimension floor is still the open fix.

---

### Confirmed in game: Coachman resolves through the CDB (09:07 session)

The Coachman was previewed in the shop with the rebuilt plugin. All five
materials now take their authored maps from the database at score 1900 instead of
being rejected and recovered by the fuzzy scorer:

```
live: cdb material map 'Materials\Weapons\Coachman\Coachman_BarrelStandard.mat' -> textures/weapons/coachmanshotgun/coachman_barrelstandart_color.dds
live: cdb material map 'Materials\Weapons\Coachman\Coachman_Sight.mat'          -> textures/weapons/coachmanshotgun/coachman_sight_2_color.dds
live: cdb material map 'Materials\Weapons\Coachman\Coachman_Grip.mat'           -> textures/weapons/coachmanshotgun/coachman_grip_color.dds
live: cdb material map 'Materials\Weapons\Coachman\Coachman_StStock.mat'        -> textures/weapons/coachmanshotgun/coachman_stdstock_color.dds
live: cdb material map 'Materials\Weapons\Coachman\Coachman_Decals.mat'         -> textures/weapons/coachmanshotgun/decals_color.dds
direct GPU: paired opacity textures/weapons/coachmanshotgun/decals_opacity.dds fmt=80 channel=red (2048x2048 vs colour 4x4)
offline GPU: uv '...Coachman_Decals.mat' v=8 u=[0.045,0.677] v=[0.048,0.328] tex=4x4
```

The decal set's authored mask is the concrete win: it was discarded on every
resolution before this pass. Zero Coachman entries appear among the session's 19
`rejected cross-family` lines (10 RocketLauncher\Decals, 7
AWP_Master_Shareable, 2 ComatKnife), and AWP is untouched because the
`awpSharedMaterial` branch returns before the new rule. The live `tex=4x4` line
still shows F1 unresolved.

### Two corrections from the same session

- **F1's colour was wrong.** The placeholder decal is a flat **pale grey
  (197,190,180)**, not the "pink-mauve (197,123,180)" recorded in
  `MATERIAL-MATCHER-AUDIT.md`. The structural finding stands - 4x4, one uniform
  block, no authored art - but the number came from an ad-hoc RGB555 unpack;
  BC1 endpoints are RGB565. The audit is corrected.
- **`dump_tex.py` decoded a quarter of every block.** `bc1_block` and `bc4_block`
  read the four selector bytes as four pixels instead of sixteen, leaving 12 of
  every 16 pixels black, and `bc1_block` unpacked endpoints as 555. Fixed and
  verified against recorded bytes (the 4x4 block now decodes 100% uniform). The
  earlier conclusions in these notes that leaned on that tool - "the action
  figure packaging atlas is mostly padding", "the London snow globe's skyline
  plate is sampled unflipped in V" - were drawn from PNGs with three-quarters of
  every block blacked out and need re-deriving before they are relied on.

**The preview's colours are the albedo.** A first pass at this section claimed
the Coachman colour maps decode as ~94% black and therefore could not produce the
brown wood on screen. That measurement was an artefact of the `dump_tex.py`
decoder bug below, which blacked out most of every texture; the claim is
withdrawn. With the decoder fixed the maps are ordinary mid-tone albedos, which
explains the capture exactly:

| map | mean RGB | what the capture shows |
|---|---|---|
| `coachman_barrelstandart_color` | (152,140,132) | the near-white barrel |
| `coachman_sight_2_color` | (182,183,183) | the bright sight body |
| `coachman_compensator_color` | (175,170,167) | the bright muzzle break |
| `coachman_stdstock_color` | (86,59,51) | the brown wood stock |
| `coachman_grip_color` | (91,85,85) | the dark grip |
| `decals_color` (4x4) | (197,190,180) | the flat placeholder |

0% of the pixels in any of them are black. So the bright barrel is the authored
texture seen through the preview's own shading, not a wrong pick and not a
tint/mask effect; the matcher hands over the files the database names, and the
fuzzy route had been landing on the same ones. Whether the preview should look
*closer* to the game is the separate colour-pipeline question already probed by
`tools/renderer_recon/dxgi_color_space_comparison.py`, whose notes decline to
switch the preview away from its established UNORM path on that evidence alone.

**Decision 2026-10-17: leave the preview's shading as it is.** The user chose
not to change the colour pipeline. The preview shows the database's own albedos
at their real values, the decoder bug that suggested otherwise is fixed and
tested, and an sRGB/tonemap or exposure change would be visual tuning without a
defect behind it. Do not re-open this without a capture that shows a texture
decoding to different values than the ones tabulated here.

## 6r-14 — how the game actually renders an item preview, and a build gate that lied (2026-10-17)

### The engine's own preview mechanism (read out of Starfield.exe, not inferred)

Previewing an item is a Bethesda subsystem, not something a plugin has to
reproduce. Its parts, with the evidence each rests on:

- The exe carries a well-formed TypeDescriptor `. ?AVSceneManager@BSMenu3D@@`
  at RVA `0x84CE760` (name string at `0x84CE770`, preceded by a valid
  `pVFTable` and a zero spare field), so `BSMenu3D::SceneManager` is a real
  class with RTTI.
- `BSMenu3D::SceneManager` is a Bethesda singleton: the exe's RTTI name table
  carries `BSTSingletonSDMBase<BSTSDMTraits<SceneManager@BSMenu3D@@,
  BSTSingletonSDMOpStaticBuffer<...>>>`, i.e. the instance lives in a static
  buffer in `.data`. Reaching it is a data address, so it needs an Address
  Library ID before a plugin may touch it; none has been identified yet.
- **REFUTED 2026-10-17 - see the bullet below and

  [reports/engine-claims-reverification-2026-10-17.md].** `SceneManager@BSMenu3D`

  (td `0x5ccb720`) **does** have a COL at `0x5300148`, and that COL heads vtable

  `0x4d7e670` with one entry, the dtor at `0x2551340`. Verified structurally, not

  by a bare 4-byte match: the class-hierarchy descriptor's `BaseClass[0].pTD`

  equals the COL's `pTD`. So a SceneManager instance can be typed by vptr

  `image_base + 0x4d7e670` in `.data` - the plugin's RTTI scan mechanism does

  support it. It still exposes no virtual scene API, so this unblocks *finding*

  the manager, not driving it.

- **[original text, now refuted]** **Open, and it decides the approach:** no 4-byte value in the image equals
  that descriptor's RVA, so no `_RTTICompleteObjectLocator` references it. A
  vtable was therefore not located, which means the plugin's proven RTTI-vtable
  scan cannot be assumed to find this manager. Before any plan depends on
  driving `SceneManager`, either find its COL/vtable by another route or obtain
  the singleton's static-buffer address from the Address Library.
- `BSMenu3D::Scene` has **no** standalone TypeDescriptor (`?AVScene@BSMenu3D@@`
  occurs zero times as a descriptor; the name appears only inside template
  arguments in the RTTI name table). It is non-polymorphic and handed out
  through `std::shared_ptr` (`_Ref_count_obj_alloc3<Scene,
  BSTHeapSTLAllocator<Scene,1>>`, `_Ref_count_resource<Scene*,
  AlignedDelete<Scene>>`, `unique_function_impl<..., const Scene*>`). Two
  consequences that close off the obvious shortcuts: a Scene has no vtable to
  call, and it has no RTTI string, so the plugin's RTTI scan cannot find a
  Scene. The manager is the only handle.
- The menus drive that subsystem through version-safe events that are already
  mapped in the SDK: `RE::InventoryMenu_Change3DView` (Address Library
  `856181`) and `RE::CraftingMenu_Highlight3D` (`858734`), both in
  `RE/E/Events.h`. Those are the supported observation points for 3D-view
  transitions; no vtable guessing is needed for them.
- The authored framing data lives on the form, not in the menu:
  `TESBoundObject::previewTransform` (offset `0x88`) holds eight
  `BGSTransform*` slots, and each `BGSTransform::data` is
  position / rotation / scale / minZoom / maxZoom. `BGSPreviewTransform.h`
  forward-declares `BGSTransform` only, so `RE/B/BGSTransform.h` must be
  included to read the record.
- `BGSMenuDisplayObject` (`menuObject`) is a component of `SpellItem` and
  `EffectSetting` only - not of weapons, misc or armor - so it plays no part in
  the shop path.

What the plugin does today: `TryNativeVendorPreview` dispatches the
`BarterMenu_LoadModel` event (getter ID `91377`), which makes the *game's own*
barter scene load the selected form, and the panel picture is the plugin's
separate direct render (the split already recorded in 6r-12).

What the plugin never did: read `TESBoundObject::previewTransform`. Framing has
always been derived from model bounds, so the panel has been guessing values the
form already carries. Added this session, in `src/ShopExplorer.cpp`: a
`LogPreviewPlacement` reader called once per selection, emitting one line per
non-null context, e.g.

    SFShopExplorer preview-placement: 00XXXXXX ctx=0 pos=(0.0,0.0,0.0) rot=(0.0,0.0,0.0) scale=1.000 zoom=[0.500,2.000]

with explicit fallbacks (`has no BGSTransform entries`, `is not a
TESBoundObject`) so an absent record is never mistaken for a zero one.

Next fidelity step, and it needs the log from a shop session first: compare the
authored context values against the framing the direct render computes, then
drive the camera from the authored data instead of the bounds. Do **not** resume
capture schemes for this. The scene renders into the engine's own HDR chain,
and driving `BSMenu3D::SceneManager` - not copying its pixels - is the native
route to the game's own appearance.

### build.cmd reported a failed compile as a successful deploy

Measured twice on this tree: with a hard `#error` at the top of
`src/ShopExplorer.cpp`, `xmake build -y` returned 0 in one run and -1 in the
next. In the 0 case the script went on to print
`[deploy] built DLL sha256=B78D4BC6...` - the hash of the *previous* DLL - and
exited 0. A broken build was therefore indistinguishable from a deploy, which is
the stale-binary trap 6r-11a exists to prevent.

`build.cmd` now captures xmake's output to `%TEMP%\sfse_xmake_build.log`, checks
xmake's exit code, then greps the captured log for `error C` / `error LNK` /
`fatal error` and aborts with status 1 before any hash is printed. Re-running
the same probe through the new script exits 1 with
`[deploy] FAILED: xmake exited -1`. `tools/test_build_gate.py` guards the
ordering statically (the behavioural probe mutates `src/`, so it stays manual).

Also worth knowing before trusting a deploy line: two consecutive builds of
identical sources produced `D8D8BE0E...` and `B08A562F...`. A DLL hash
identifies a file, not a source state. Confirm a deploy carries a change by
mtime plus a string present only in the new code - here
`preview-placement` occurs in the rebuilt `SFShopExplorer.dll`.

### 6r-14a — the pre-native preview can be switched off, and why one INI key was not enough

Before a native-render test, every plugin-side painter of the PREVIEW rectangle
has to be off, or a picture the plugin drew can be read as the native result.
Two routes paint that rectangle (`src/ShopExplorer.cpp`, `drawViewport`):

- the direct GPU render - `OfflineModel`'s SRV, drawn when `offlineCurrent`;
- the captured-scene route - `ItemCapture`'s SRVs, drawn in the *else* arm,
  i.e. exactly when the direct render is not current.

That second arm is why `[offlinepreview] enabled=0` alone is not a safe test
configuration: blanking the direct render promotes the captured game frame into
the same rectangle, where it is indistinguishable from a native result. The
capture route is also re-armed by `[itemcapture] mode=1|2` - and the reference
INI in this repo ships `mode=1` - so a config-only disable can come back on
without anyone noticing.

One switch now covers both, plus the status text that accompanies them:

    [preview]
    legacyRenderer=0

With it off, and only with it off, the selection path skips `FormModelPaths`,
`NativeModel::LoadFromForm` (the Clone3D geometry extraction),
`OfflineModel::RequestPreviewDirect` and `GetSnapshot`, forces
`ItemCapture::SetShopWantsCapture(false)`, and suppresses the capture grid, the
capture sub-rect, the capture status line and the "building direct GPU
preview..." branch. In their place the viewport prints a notice naming the key
and the native dispatch result for the selected form
(`native dispatch: ok|FAILED for 00XXXXXX`), so the panel reports rather than
illustrates. The native route itself - the per-form placement reader above and
the `BarterMenu_LoadModel` dispatch - stays live, as does the shop UI.

Code default is `1` (previous behaviour); the value is read once per process
through a function-local static, so changing the INI needs a game restart.
Restore with `legacyRenderer=1`.

Verified 2026-10-17: rebuilt clean, the DLL carries the new strings, and the
suite is 111 tests / 13 suites green, including `tools/test_legacy_preview_gate.py`
(static assertions pinning both painters, the capture spinner and the status
branch behind the flag). **Not** verified: the gate in a running shop session -
it needs the rebuilt DLL deployed, and at the time of writing the deployed copy
is still the pre-gate build.

### 6r-14b - first in-game run of the gate, and the cast that read nothing

Session 2026-10-17 10:09 (SFShopExplorer.log, plugin 22144): the gate engaged.
`preview: legacy renderer DISABLED ([preview] legacyRenderer=0)` is logged once
at 10:09:35.518, the viewport shows the notice and
`native dispatch: ok for 00000FD6` instead of an image, and the log contains
zero `native preview: loaded=` and zero `direct 3D` lines - the geometry
extraction, the GPU request and the offline status footer were all skipped as
designed. The `ItemCapture` config line (mode=0) is the only other capture
mention. So: gate confirmed working, legacy path confirmed silent.

The same run also caught a bug in the placement reader, which is what the gate
was for:

    preview-placement: 00000FD6 is not a TESBoundObject; no authored placement

00000FD6 is the Razorback. Cause, in the SDK rather than the game:
`TESForm::Is<T>()` is `Is(T::FORMTYPE)`, a form-type comparison, and
`TESBoundObject` declares `SF_RTTI_VTABLE` but **no** `SF_FORMTYPE`. So
`As<RE::TESBoundObject>()` is nullptr for *every* concrete bound form - the
reader could never have worked on any item. Fixed by resolving the base through
the concrete class (`BoundObjectOf`), with two cases that a naive fix still gets
wrong: a KEYM form reports KEYM although `TESKey` derives `TESObjectMISC`, so it
needs `As<TESKey>()`; and INGR has no mapped class at all, so it cannot be cast
and is named in the log (`form type {} has no mapped bound-object class`) rather
than reported as absent data.

Consequence for the next session: the two `is not a TESBoundObject` lines above
are **not** evidence that these forms lack authored placement data - the values
were never read. The fixed build (deployed at 10:2x) will emit the eight
`preview-placement` context lines per form, which is the input the framing work
in 6r-14 needs. Suite after the fix and its guard
(`tools/test_preview_placement_reader.py`): 116 tests / 14 suites green.

### 6r-14c - the authored placement values, first read (2026-10-17, session 23992)

With the cast fixed, 22 selected forms produced placement data. Logged verbatim
in SFShopExplorer.log lines 4572-10094; summarised here because these numbers
are the input to the framing work and should not be re-derived by guesswork.

| form | records | ctx=0 pos | ctx=0 rot | ctx=4 pos/rot | ctx=5 zoom |
|---|---|---|---|---|---|
| 00000FD6 Razorback | 0,4,5,6 | (-0.02, -4.15, 0.03) | (0, 0, 4.712) | (0, 1, 0) / (6.248, 0.087, 4.189) | [1.0, -10.0] |
| 0001BC4F Equinox | 0,4,5,6 | (-0.08, -4.11, 0.00) | (0, 0, 4.712) | (0, 1, 0) / (6.248, 0.087, 4.189) | [1.0, -10.0] |
| 00028A02 | 0,4,5,6 | (-0.02, -4.15, 0.03) | (0, 0, 4.712) | (0, 1, 0) / (6.248, 0.087, 4.189) | [1.0, -10.0] |
| 00016758 | 0,4,5,6 | (0.08, -3.69, 0.02) | (0, 0, 4.712) | (0, 1, 0) / (6.248, 0.087, 4.189) | [1.0, -10.0] |
| 0000000A, 0000000F, 00005568, 0000556F, 00014CBC, 00091157 | 4 only | - | - | rot varies per form | - |
| 000028A5, 00003C4E | 0 only | (0, -0.20, 0.12) | (-1.524, -0.013, 0) | - | - |
| 00003A77 | 0,3,5 | (0, -0.20, 0.12) | (-1.524, -0.013, 0) | - | - |
| 0004AD3E | 4,5 | - | - | (0, 1, 0) / (5.672, 6.213, 3.037) | [1.0, 0.0] |

Records are sparse and per-form: contexts seen are {4} (6 forms), {0,4,5,6} (4),
{0} (2), {0,3,5} (1), {4,5} (1). **Eight of the 22 forms carry no records at all**
(000001F4, 00000D64, 00003BD3, 00013D5C, 000177E6, 00030B2F, 0020BA5E, 00215ED6) -
for those the engine has nothing to apply either, so the existing auto-fit is
not a shortcut there, it is the only option.

Two readings, and the data separates them:

- **ctx=4 is not model-specific.** All four weapon forms share a byte-identical
  ctx=4 record, (0, 1, 0) / (6.248, 0.087, 4.189) / scale 1. Two of them are
  named and are different models (00000FD6 Razorback, 0001BC4F Equinox); the
  mesh identity of 00016758 and 00028A02 is not established, so the claim
  argued here is only that the record is shared across distinct weapon forms.
  A per-model orientation fix-up would have to differ between a Razorback and
  an Equinox; it does not. So ctx=4 reads as a category-level object/placement
  transform.
- **ctx=0 tracks the model.** Rotation is constant across those same four
  (0, 0, 4.712 rad = 270 deg) while the Y distance moves with the item:
  -4.15, -4.15, -4.11, -3.69. A constant rotation with a size-dependent
  distance is what a framing camera looks like. So ctx=0 is the candidate
  preview camera, and our auto-fit approximation of *distance* is less wrong
  than the orientation, which is the part we never had.

Also settled: **zoom is a dead field in shipped data** - every record has either
[0, 0] or max &lt; min ([1.0, -10.0], [1.0, 0.0]), i.e. no usable range. Do not
build framing on it.

What would falsify the ctx=0-camera reading: a form whose ctx=0 rotation is not
(0, 0, 270 deg), or whose ctx=0 distance does not track its own model size. The
two non-weapon forms (000028A5, 00003C4E) already break the first - their ctx=0
is (0, -0.20, 0.12) at -87.3 deg, a different rig - so the reading is typed as
"weapon/menu context", not universal.

Next: apply ctx=0's rotation and ctx=4's object transform when rendering a
weapon, and compare that against the game's own preview side by side. That needs
the reference renderer switched back on for the comparison ([preview]
legacyRenderer=1), which is why the gate above is one INI key and not a code
deletion.

### 6r-15 - the gate now selects the native source instead of blanking the viewport

`[preview] legacyRenderer=0` used to mean "paint nothing", with a notice naming
the native dispatch result. That left no way to see whether the native route
worked, and the dispatch result itself only certified "no crash". Both halves
changed:

- The gate now selects the **source**, not the presence, of a preview. Legacy
  mode (`1`) is unchanged: `FormModelPaths` → Clone3D → NIF fallback. Native
  mode (`0`) asks `SFE::NativeModel::LoadFromBarterMenu`, which reads the
  `NiAVObject` the engine loaded for the barter menu's own preview.
- Both sources feed the same `RequestPreviewDirect` call, and the bottom strip
  names which one produced the picture (`direct 3D` vs `engine model
  (barter)`), so a screenshot cannot be attributed to the wrong route.
- If the read-back yields nothing the viewport is still empty and the notice
  carries the read-back's own message. There is no silent fallback.

How the model is found, without calling engine code:

1. `REL::ID(91377)` → `0x15da450`, the `BarterMenu_LoadModel` source getter.
2. Walk the source's sink array — SDK layout: sinks at `+0x08`, `BSTArray`
   size@+0, capacity@+4, data@+8.
3. `BarterMenu` has 13 RTTI vtables, one per event-sink base. The base at
   `menu+0x150` is the LoadModel sink: vtable `0x4c808e8` (COL `0x51226e0`)
   opens with the adjustor thunk `sub rcx, 0x150; jmp 0x15d0ea0`, and the
   teardown at `0x15d0c76` unregisters `rdx = rsi + 0x150` from the source
   returned by `0x15da450`. Its ProcessEvent is `0x15d24f0` → deferred body
   `0x15d25e0` / `0x15db520`. So `menu = sink - 0x150` is a base-class offset.
4. Scan the menu's first `0x4000` bytes for aligned pointers whose RTTI is a
   `NiNode`/`BSFadeNode`/`BSModelNode` root, walk each with the existing
   `WalkTree`, keep the root with the most triangles (the menu holds its own
   scene root too), then `SaneBounds` before it reaches the GPU.

`tools/renderer_recon/verify_barter_sink.py --exe <Starfield.exe>` re-derives
claims 1-3 from the image and exits non-zero if the offsets move; it prints
five PASS lines and `verified` on 1.16.244. The tool
`tools/renderer_recon/func_probe.py` was written for this hunt and is generally
useful: it disassembles one function with direct calls resolved to RVAs,
RIP-relative data/string loads annotated, and struct writes marked.

Unproven until a live run: whether the item model is a direct `NiAVObject*`
member inside the scanned window or is only reachable through the scene root.
The log line `barter read-back ok: N meshes T tris root 0x...` answers it; a
`barter read-back ... none carried geometry` line means the root is the scene
and the item is one hop further in.

#### First live run (12:49, session 17644) - the dispatch was a no-op all along

Every one of the 12 read-backs that session reported:

```
barter source 0x7FF6A882E770 has no readable sink array (count=0)
```

The source pointer is valid and the plugin dispatched 12 `BarterMenu_LoadModel`
notifications to it; **none had a sink**. `BarterMenu` registers its LoadModel
sink as a base subobject, and only while the menu exists, so this session had no
barter menu open at all. That is consistent with how the shop is opened: the
catalog is built from the game's global form arrays (`form array for type 4 ...
7217 entries` at 12:49:31.370), not from a vendor's inventory, so the hotkey
path needs no menu.

The consequence is larger than this feature: `TryNativeVendorPreview`'s "ok"
has never meant "a menu consumed the event". It means `Notify` returned without
faulting on an empty sink list. Any earlier reading of a native test that
treated that line as progress was reading nothing. The read-back now states the
case explicitly (`barter source ... sink count=0 (BarterMenu open: no)`) and
reports whether `UI::IsMenuOpen("BarterMenu")` agrees, so a vendor-open session
and a hotkey-open session can be told apart in one line.

Open decision this creates: the native route needs a barter menu to exist. Two
ways forward -- open the shop from a vendor so the menu is up (the route taken),
or instantiate the menu ourselves, which is the class of menu-state manipulation
the project has avoided.

#### Menu state is now a cache key, so vendor-after-shop works

Both caches keyed on the form alone were wrong for this order of events. The
shop UI opens from the global catalog with no menu, the first dispatch lands on
an empty sink list, and the cache then suppressed every later attempt even after
a vendor was opened. Now:

- `TryNativeVendorPreview` keys on `(form, BarterMenuAvailable())`, logs
  `native dispatch deferred ... BarterMenu is not open` instead of claiming an
  `ok`, and re-dispatches when the menu appears.
- The read-back cache keys on `(form, choiceVersion, BarterMenuAvailable())`, so
  the same form is re-read once the engine has a model for it.
- The failure path prints a UI census rather than a bare count:
  `barter source 0x... sink count=0 (registered=true instance=false open=no)`.
  `registered` true with `instance` false means the menu type is known to the UI
  but no object exists; that is the state to expect before any vendor is opened.

With a vendor open the order is: dispatch -> menu's own LoadModel sink runs ->
engine assembles and loads the item preview -> read-back scans the menu and
finds the `NiNode`. Nothing about that path has run yet; the next vendor session
is the test.

## Preview-transform context: engine code (supersedes the data-only reading above)

The context index is not stored anywhere in the record - it is chosen by each
engine caller. Full details and reproduction commands:
[reports/preview-transform-context-engine-code.md] (repo `reports/`).

- `BGSPreviewTransform` has RTTI 844574 and VTABLE 396233; resolving RTTI gives
  `td=0x59db458 col=0x4f8be40 vtable=0x4b0e8e8` with 11 slots, which are the
  destructor (frees `0x48`, the header's `sizeof`), `GetFormComponentType`,
  `InitializeDataComponent` and eight No-Op `ret 0`s. **There is no accessor
  virtual**, so no virtual returns `transforms[k]`.
- Displacement search over the corrected scan (`.text` decoded per `.pdata`
  function, 14.1M instructions) finds 80,115 eight-byte loads from
  `[reg + 0x90..0xC8]`, 310 indexed ones, 1,408 `lea/add reg, [x+0x88]` component
  bases, and 2,740 `TransformData`-range reads - 1,643 (function, slot) pairs,
  92 of which meet the block-read rule. Those 92 are **candidates, not
  identifications** (some are unrelated structs at the same offsets).
  The earlier "zero consumers" result came from a linear `md.disasm` pass that
  silently stopped after 753k of ~14M instructions, and is withdrawn.
- The consumer path is `TESForm` virtual slot `0x5A`/`0x5B`
  (`AsPreviewTransform1/2`, vtable `+0x2D0`/`+0x2D8`). 46 call sites exist; the
  `...1` form is called **with a context argument in `edx`**, e.g.
  `mov edx, r15d / mov rcx, r14 / call qword ptr [rax+0x2d0]`.
- Consequence for this project: `TESForm.h` declaring `AsPreviewTransform1()`
  with no parameters is wrong - it takes the context. Calling it as declared
  would pass garbage in `edx`.
- Of the 46 sites, **16 go through `+0x2D0`** (the context-taking form) and
  **30 through the const `+0x2D8`**; 8 resolve to an owning RTTI class. The
  plugin's "has no BGSTransform entries; engine falls back to auto-fit" case is
  the const-form path.
- Still open: the per-menu index table. At most sites `edx` is a register
  threaded in as a parameter, so it needs interprocedural propagation from each
  menu's own vtable methods down to those 46 sites (`context_index_attribution.py`
  already walks the call graph upward; resolved-constant rate 5/46).

## Live run 2026-10-17 21:40-21:43: gate + authored placement, no vendor

First in-game run of build `d733f794` (`legacyRenderer=0`, shop opened with Page
Up away from any vendor). Seven forms selected; **no crash**. What the run
settles:

- The dispatch gate fires before the sink walk: seven
  `native dispatch deferred for <id>; BarterMenu is not open` lines, one per
  selected form, and every read-back reports the same census
  `registered=true instance=false open=no`. So `IsMenuRegistered` and
  `IsMenuOpen` disagree in exactly the way the no-vendor case needs, and the
  crash path is never entered. **Still unproven: the vendor-open branch** - no
  run has ever had `instance=true`.
- The authored placement reader works and is per-context, not per-form:
  `0029B050` Boba Alien Tea and `0029B052` Alien Tea carry ctx 0/4/5 with
  `scale=4.200` at ctx 0 against `1.000` at ctx 4/5; `00000FD6` Razorback has
  ctx 0/4/5/6 with ctx 0 at `pos y=-4.150` and ctx 5 at `pos y=-0.530`;
  `002B558D` 7.62x39mm has only ctx 4/5; `0000000A` Digipick only ctx 4;
  `00050340` Playing Card only ctx 4. `000001F4` UnarmedHuman (the default
  selection on open) has none, so the auto-fit fallback branch is real too.
- **The `[-10.000,-10.000]` zoom is authored, not a misread.** It comes from
  TRNS `00162788` (`Inv_Guns_Workbench3D_04`), whose own `DATA` holds exactly
  that pair; the other 13 records genuinely store `0,0`. Verified offline by
  reading the record, see the tally below.
- Context coverage across seven unrelated forms is 4 (6 forms), 5 (4), 0 (3),
  6 (1). That is consistent with 4/5 being generic item-view slots, but it does
  not identify the barter slot - the per-menu index table is still open, and
  only a vendor-open run can correlate a dispatch with the context the engine
  actually applies.

## Offline preview-transform tally, 2026-10-17 (no playtest)

The same data the game logs is in the plugin files, so the catalog is measured
without a launch: `PTT2` on each object record is the eight-slot array (one
formID per context) and each entry points at a `TRNS` form whose `DATA` is the
9-float `TransformData`. `tools/preview_transform_coverage.py` reads it;
`tools/test_preview_transform_coverage.py` pins it to the live run by
reproducing all six logged forms slot-for-slot and value-for-value.

Test profile, 75 plugins, 7289 candidate forms, 6217 with at least one slot:

| slot | forms | dominant transforms |
| --- | --- | --- |
| 4 | 5486 (88% of authored) | `Inv_DefaultTransform_Z015`, `..._DataSlates`, `..._KeyCards` |
| 0 | 1967 | `Inv_IconDefault`, `Inv_Spacesuit_InventoryIcon` |
| 5 | 1590 | food-craft bin transforms, `..._UP_X090_Y172_Z180` |
| 3 | 562 | `Inv_Helmets_PreviewTransform`, `Inv_Backpacks_PreviewTransform` |
| 6 | 130 | `Inv_GunsLarge_DataMenu`, `Inv_Guns_DataMenu`, `Inv_Melee_DataMenu` |
| 1 / 2 / 7 | 17 / 1 / 4 | effectively unused |

- 1072 forms (14.7%) author nothing and fall back to auto-fit. By type the
  coverage gap is not uniform: weapons 169/467 authored, armor 703/1216, ammo
  72/155, against books 1426/1463 and alchemy 1132/1141. A framing fix that
  relies on the record alone will be wrong for most weapons, so the auto-fit
  fallback is load-bearing, not an edge case.
- Slot 4 is the general item transform, slot 0 is icon-shaped, slot 3 is
  body-slot specific and slots 6/7 are `DataMenu`-shaped. So the framing code
  can pick slot 4, then 0, then auto-fit, instead of deriving from bounds.
- Cost: one 1.7 MB TSV in `reports/`, 10 seconds to produce, no per-item files,
  nothing added to the mod directory.

Still runtime-only: which slot the *barter* preview applies. The tally says what
is authored, not what the menu consumes; slots 1/2/7 being near-empty means the
answer is almost certainly 4, but that needs a vendor-open run to confirm.

## Two armor-preview defects from the 2026-10-18 screens

### The Chunks cap drew only its logo

The screenshot showed the red Chunks decal floating with no cap around it, and
the log named the fabric as a translucent range:

```text
offline GPU: 'direct:Chunks Cap [FormID 00185A6F]#…' translucent ranges 1 of 2
  (e.g. 'Materials\Clothes\Outfit_Baseball_Cap_01\Outfit_Baseball_Cap_01.mat')
```

`MaterialBlendAlpha` already ignores a garment's stored `MaterialOverrideColor`
weight - that value is a composite layer weight, not surface opacity, and the
cap material stores `(1,1,1,8/255)` - but its garment test is written
`materials/clothes/` while NIF material strings use backslashes. `LowerAscii` in
`LiveModel.cpp` normalizes `\` to `/`; the copy in `OfflineModel.cpp` did not, so
`garment` was false for every real garment and the 3% alpha reached the surface.
The fix normalizes `ml` in `MaterialBlendAlpha` before the path test. The 12:35
build had this rule in place and still drew an invisible cap, which is what
identified the separator as the cause rather than the rule.

### Detached forearms in apparel previews

Azami, the Navy crew uniform and the fashion suit all previewed a body plus a
pair of forearms floating beside it. The log shows the part and why:

```text
geo leaf: 'Hands_3rd_F:0' bounds=(-0.734..-0.437, -0.077..0.122, -0.526..0.526)
geo leaf: 'Outfit_Azami_UpperBody_F:0' bounds=(-0.340..0.328, …)
```

`Hands_3rd_*` is a skinned attachment stored in bind space around the rig
origin; the engine attaches it to the skeleton at draw time. Reading its raw
vertices puts it off the body. `PruneSkinnedAttachmentParts` now drops leaves
whose name contains `hands` **only when the same load also carries rest-pose
geometry**, so a hand-only item still renders instead of going empty. Gloves are
full rest-pose meshes (`Gloves_F_LOD1`, bounds at hand height) and are untouched.

Deployed 2026-10-18 13:52, `4843b880…81CB6C`, previous DLL kept as
`SFShopExplorer.dll.backup-before-armor-categories-20261018-135202`. Suites
green: 110 tests in `tools/`, 33 in `ba2_audit/`, `renderer_recon/` OK, including
the new `tools/test_armor_preview_parts.py`.

Still open from those screens: outfit previews whose ARMA set includes a `Skin_*`
head render the atlas's bald scalp (grey) because hair is a separate part the
set does not carry, and compact items (the Mark I pack, helmets) fill and clip
at the panel edge at the default `zoom=2.05`. Neither is changed here.

## Armor/Other is actor skins, not items - 2026-10-18 (no playtest)

The Armor/Other list was not a classification miss, it was the wrong record
type: every row in it was an actor skin. The captured screen shows Terrormorph,
`SkinModelA GenericHeavyCombat`, `Skin_BipedA_Orchid`,
`Skin_QuadrupedA_Groat_Male`, `SFBGS001_Skin_*`, `Skin_MinorBotA_Security` and
`Skin_Turret_Laser_01_ShortCorpse`, all value 0 / weight 0.000, and the preview
of the Terrormorph is a bare ragdoll rig.

Two reasons the four hide toggles could not remove them, even with all four
pink ticks on:

- **No identity word.** The crew/creature filter matches crew, companion,
  follower, enemy, hostile, opponent, creature, animal, beast, monster. A
  creature's own name ("Terrormorph", "Groat", "MiniBot", "SkinModelA") carries
  none of those, and neither does its keyword set (`AnimClaw`,
  `DisplayObjectTypeMiscMedium`).
- **No display name to gate on.** The turret corpses have no `TESFullName`, so
  `FormDisplayName` falls back to the editor ID and `HasPlayerVisibleName` is
  false. Every identity toggle is gated on that name, so
  `Skin_Turret_Laser_01_ShortCorpse` was skipped before the "turret" test ran.
  Its row in the table is the editor ID, which is what told us the gate, not the
  match, was the problem.

The fix is structural, not another word list. A wearable ARMO in Starfield
carries `InventoryCategory*` and/or `ArmorType*`; an actor skin carries neither.
`IsActorSkinArmor` tests exactly that and `BuildFormList` drops the record before
the subcategory classifier, with no toggle in the way - a skin is never an item
in any view. Split on the 75-plugin profile: 332 records without either family,
all skins; 881 with one, all wearable (longest keyword list 360 chars, so the
1024-char read cannot truncate into a false positive). Four `SFBGS001_
Spacesuit_Redeemed_*` rows resolve to Spacesuits by name but are marked NON
PLAYABLE and carry only `DestructibleArmorKeyword`; they are hidden with the
rest.

Armor/Other now reports 0 rows and the header says why instead of showing a
blank list. New coverage: `tools/fixtures/armo_catalog.csv` (all 1213 ARMO rows,
three columns) built by `tools/gen_armo_fixture.py`, and three tests in
`tools/test_catalog_filters.py` that replay the classifier - no wearable row is
ever hidden, Armor/Other contains only skins, and every hidden row is
recognisable as a non-item.

## ESC-close killed search, plus preview framing and bald heads - 2026-10-18

Three reports after the actor-skin build: search stopped accepting characters
when the shop had been closed with ESC (PgUp and the window X were fine), the
Mark I backpack filled and clipped at the preview border, and Andreja's outfit
still showed a bald scalp.

### ESC close -> dead search box

The log pins the chain. `existing menu callback name=PauseMenu` appears exactly
once in the whole session, 0.8 s after the only ESC close: the game opened its
pause menu, so the ESC close had leaked into engine menu handling. While a game
menu is up, the engine's own pump (`0x189df20`, the site that calls
`TranslateMessage`) is running, so a genuine `WM_CHAR` reached the overlay.
That tripped the session-wide `g_hostTranslatesChars` latch, which disabled the
overlay's synthesized characters for the rest of the process. When the menu
closed the pump stopped, the host produced no more characters, and typing
produced nothing - exactly the reported asymmetry, since only the ESC path opens
a game menu.

Fixes, all in `ImguiOverlay.cpp` / `ShopExplorer.cpp`:

- **The latch decays.** `g_lastHostCharTick` records the last real `WM_CHAR`;
  the host counts as translating for 500 ms after it. Continuous host
  translation still suppresses synthesis (no double typing), and synthesis comes
  back the moment the host goes quiet, instead of staying dead for the session.
- **Swallowed key-downs keep their key-up.** `g_swallowedKeyDowns` is a 256-bit
  bitset set when a key-down is eaten and consumed by the matching key-up *before*
  the capture check, so the shop closing mid-press can no longer hand the game an
  orphan ESC up edge - the thing that opened PauseMenu.
- **Every open resets ImGui input.** Capture-enable sets `g_resetImGuiInput`;
  the render thread consumes it before `NewFrame()` and calls
  `io.ClearInputKeys()` (which also drops the character queue), clears the queued
  messages and the host-char timestamp. Stale keys from any close path are now
  harmless.
- **ESC leaves the search box first.** The handler now requires
  `!ImGui::GetIO().WantTextInput`, so typing then ESC cancels the edit instead of
  exiting the shop; the next ESC goes back a page and, from the plugin page,
  closes.
- **ESC close tears down like Close()**: `Back()` now calls
  `SetShopWantsCapture(false)`, which it previously skipped.

### Mark I backpack clipping

`camDist` was `radius*5/zoom`, which frames the bounding *sphere*. At the
default `zoom=2.05` that is `2.44r`, which puts a sphere-bounded silhouette at
0.99 of the frame half-extent - touching the edge - and an item whose near face
is closer than its centre (a deep pack) projects well past it. `FitCamDist` now
projects the eight bounding-box corners at the active orbit and pulls the camera
back until all of them fit inside a 10% margin, never closer than the old
distance; the wheel still scales the fitted framing (`/ zoomMul`). Applied to
both the GPU path (`BuildMvp`) and the CPU rasterizer, so the two agree.

### Andreja's outfit: bald scalp and bare hands

The outfit's ARMA set carries `Outfit_Civilian_FormalHalfDress_01_Skin_F` - the
player's bare body skin, stored in bind space like `Hands_3rd_F`. Rendered
statically it is a hairless scalp taken from the skin atlas plus detached
forearms; hair/eyes are separate parts the set does not include, so the head can
never look right. `PruneSkinnedAttachmentParts` now drops whole-token `hands`
*and* `skin` leaves when the same load carries rest-pose geometry, and still
keeps everything when a skin part is the item's only mesh.

Deployed `7F2AB5F6…3FD4DF` to the MO2 mod, previous DLL kept as
`SFShopExplorer.dll.backup-before-framing-input-20261018-144418`. Suites green:
127 tests in `tools/` (including new `test_overlay_input_recovery.py` and the
extended `test_armor_preview_parts.py`), 69 in `renderer_recon/`. Not verified in
game - no launch from here - so the ESC/typing sequence, the pack framing and the
outfit preview all still need one live look.

## Popped-out preview: dead arrows, dead ESC, dead search - 2026-10-18

Report after the framing/input build, this time on the armor screen with the
preview popped out: arrow keys did nothing, ESC did not dismiss the pop-out, and
search was still dead afterwards. The retained log
(`SFShopExplorer.log`, session 14:59-15:09) separates the three.

### The pop-out had no keyboard exit at all

`ui: preview popped out (290x192) for 'Mark I [FormID 0001754E]'` at 15:02:36 and
`ui: preview returned inline (click away)` at 15:03:38 bracket the whole
complaint: in those 62 seconds there is not one `catalog key` line, and the only
way the preview came back was a mouse click. ESC meanwhile fell through to the
page-back handler, which is why the pop-out stayed put while the page (and
eventually the shop) changed underneath it - and the shop closing on that path
is what the earlier "search is dead" report was about. ESC now dismisses the
pop-out first, consumes that press (one shared `escapePressed`/`escapeConsumed`
per frame), and the hint reads "ESC or click outside to bring it back".

### Keyboard was lost while the mouse kept working

Worth recording as the actual mechanism candidate, because it explains a symptom
pair that looks contradictory: arrows *and* typed characters dead, mouse fine.
`ImGui::UpdateKeyboardInputs()` calls `io.ClearInputKeys()` on every frame while
`io.AppFocusLost` is set, and that also empties the character queue. This overlay
drives its pointer from raw input and its own virtual cursor, so the mouse keeps
working while every key is silently discarded - exactly the reported split. A
focus loss whose `WM_SETFOCUS` never reached the subclass poisons the rest of the
session.

Fixes:

- `RenderImGuiFrame` re-syncs a stale focus loss against the real foreground
  window (`GetForegroundWindow() == g_hwnd` -> `io.AddFocusEvent(true)`) before
  `NewFrame()`. It never invents focus: while another application owns the
  keyboard ImGui keeps dropping input.
- `WM_SETFOCUS`/`WM_KILLFOCUS` are logged with the drawing/capture state, and the
  `catalog key` diagnostic now prints `popout`, `itemActive` and `textInput`, so
  the next session distinguishes "key never arrived" from "key arrived and was
  gated" without another guess.

### Catalog arrows while the pop-out is open

`keyboardReady` required `!ImGui::IsAnyItemActive()`. The pop-out's orbit button
becomes the active item as soon as it is clicked or focused, so the arrows were
disabled for exactly as long as the preview was popped out. Only things that
genuinely own keys block now: a text field (`WantTextInput`) and an open combo
popup. The pop-out is also created with `NoNav` so it cannot take keyboard
navigation from the catalog, and `previewPress` is cleared on both close paths.

Deployed `5B8A86DB…D12430B6`, previous DLL kept as
`SFShopExplorer.dll.backup-before-popout-keys-20261018-151632`. 134 tests in
`tools/` (was 127; `test_overlay_input_recovery.py` gained focus-loss and
pop-out key guards) and 69 in `renderer_recon/` pass; the new strings are present
in the deployed binary. Not verified in game - the ESC-dismiss, arrow navigation
with the pop-out open, and typing after a pop-out session each still need one
live confirmation, and the focus log is the evidence to read first if the keys
are still dead.

## The pop-out still ate the keys: ImGui never saw them - 2026-10-18 (second pass)

Report on the build that added the ESC path, arrows-with-pop-out and the focus
re-sync: still no arrows, still no ESC, backpack still clipped. The new session
log (`SFShopExplorer.log`, 15:19-15:21) is what settles it, because the new
instrumentation was in the deployed DLL and the hint text on screen proves the
new build was running.

### The keys never reached ImGui

```
15:20:20.781  ui: preview popped out (290x192) for 'Mark I [FormID 0001754E]'
15:20:23.965  game window focus lost (drawing=true capture=true)
15:20:49.022  game window focus gained (drawing=true capture=true)
15:20:49.033  ui: preview returned inline (click away)
15:20:52.606  ui: catalog key ... ready=true ... popout=false
```

Between the pop-out opening and the focus loss there is not one `catalog key`
line, and no line at all while the pop-out was up. So the gating added last pass
(`WantTextInput`, `IsAnyItemActive`, `NoNav`) was never the blocker: ImGui
received no key events to gate. The mouse kept working throughout - the
click-away at 15:20:49 proves it - which is the signature of this overlay's raw
input cursor running while the key path is dead.

Two candidate causes remain, and the log now names either: the window really was
unfocused (the `focus lost` line), or ImGui dropped the events it did get. The
fix therefore stops asking ImGui for these keys.

### Escape and the catalog arrows now come from the window's own key stream

`Overlay::ConsumeKeyPress(vk)` reads edge-triggered bits set in `WindowProc` from
the same `WM_KEYDOWN` messages the ImGui backend parses, before any ImGui state
is involved. Reading a bit clears it, so one press is one action; auto-repeat is
kept for the arrows (holding one walks the list) and ignored for Escape (holding
it must not walk back through the pages). Escape, Up/Down/Left/Right and Enter in
the shop use it instead of `ImGui::IsKeyPressed`.

The first 12 key-downs of a session are logged (`overlay key down vk=27 ...`), so
the next run answers the remaining question directly: if `vk=27` (Escape) is in
the log and nothing happens, the shop's handler is at fault; if it is absent, the
game window really is not receiving keys and no plugin-side fix can help.

### Backpack: the 10% margin was inside the HUD, not the visible area

`FitCamDist` fitted the bounding box to 0.90 of the render target. The viewport
paints two text lines over its top edge and a caption line over its bottom edge,
so a deep pack's lower face still ended up behind the caption strip - the "still
clipped" in the screenshot, while the geometry itself fits. The margin is now
0.80, i.e. ~10% clear at each edge for that chrome.

Deployed `D0F85DF9…E90464B` (one rebuild after a no-op cleanup: Win32 reports
keypad enter as `VK_RETURN`, so the separate `VK_SEPARATOR` lookup was dead),
previous DLL kept as `SFShopExplorer.dll.backup-before-rawkeys2-*` (and the
pre-raw-key build as `SFShopExplorer.dll.backup-before-rawkeys-20261018-152536`). 140 tests in
`tools/` (new `RawKeyPathTests`, new margin guard) and 69 in `renderer_recon/`
pass. Not verified in game: the arrows/ESC path and the wider framing each still
need one live look, and this time the `overlay key down` lines say whether the
key arrived at all.

## The Mark I backpack drew flat grey: one wrapped UV rejected the set - 2026-10-18

Keys confirmed working; the pack stayed grey (the game's pack is white with a
red band and blue canisters), and the ammo crates still showed the wrong paint.

### The pack had UVs all along

```
15:31:02.571  geo leaf: 'SpaceSuit_SuitZero_Backpack_GO:0' (pos=7491 uv=0 idx=12060)
15:31:02.581  direct GPU: range 0 '…\Spacesuit_SuitZero_Backpack.mat' <- slot 1 1024x1024
```

The atlas resolved, so `uv=0` was the whole defect: with no UV array every face
samples texel (0,0) and the pack is one colour. The `.mesh` is
`geometries/657478c21f1e48a01425/52b4455363dc68a70393.mesh` (2497v/12060i, the
NIF's own LOD entry) and its UV stream is a normal f16 set - except 72 vertices
carry whole-tile wrap offsets (u to -11.30, v to 26.17). `ParseMeshLod0`'s
"is this a UV stream?" bound was `+/-8` and rejected the *entire* set on the
first such vertex. The renderer already binds `ADDRESS_MODE_WRAP`, so those
offsets fold back into ordinary atlas coordinates: wrapped, the set covers all
64 cells of an 8x8 split of the atlas, which is what a real UV layout looks
like.

The bound is now `kMaxAbsUv = 4096` (finite and wide enough for wrap offsets
and texel-space UVs), which is all it has to be: `count == nv` is what actually
identifies the stream. `nif_uv.py`, the Python replica the audit tools use,
carries the same constant so the tools and the plugin agree.

Pinned by `tools/ba2_audit/test_mesh_uv_wrap.py`: a synthetic LOD0 with the
measured wrap vertex must keep its set, NaN and 9000 must still reject, and the
C++ source keeps the wide bound and the measurement note. Re-run against the
real archive:

```
python -c "…parse_mesh_streams(blob)"   # uv_sets [(-11.296, 1.991, 0.009, 26.172)]
```

Deployed `D65B6886…F8DABF64`. The binary it replaced (`D0F85DF9…E90464B`) was
not archived first; the nearest kept binary is
`SFShopExplorer.dll.backup-before-rawkeys2-20261018-152802` (the pre-rebuild
15:28 build, no source difference apart from this bound). 140 tests in `tools/`,
39 in `ba2_audit/` and the `renderer_recon/` suite pass, and `build.cmd` (with
its BA2 gate) is green. Not verified in game: the pack still needs one look.

### Ammo crates: the database paint colour is multiplied back in (measured)

Files from the same session. `.27 CAL` drew a white lid and a yellow band where
the game draws a yellow lid and a blue-grey band, and every ammo model logs
`2 range(s) have a CDB override tint candidate` - the database colour is found
and then dropped, because `UploadModelTextures` resets a range's colour to
white as soon as an albedo uploads unless the range is in the
`cdbOverrideTint` allow-list (today only the crafted drink bags).

The `ammo_paint_probe.py` measurement (new tool) resolves the question the
screenshots could not: it walks the crate's NIF, samples each shape's real
albedo under its UV footprint, and prints the composites.

```
shape PaintedMetalshine01_Orange   albedo (197,198,197)   <- neutral paint
      x tint raw                   (167,134,78)
      x tint linear->sRGB          (183,167,130)
game  .27 CAL lid                  (201,186,118) lit / (189,173,102) front
```

So the arithmetic is **multiply**, not replace (the grunge and stencils survive,
which replacing would erase), and the database colour is **linear** while this
pipe multiplies sRGB bytes - the raw value is visibly too dark, the converted
one is the paint. The database's names cannot stand in for it either:
`ammoboxprimer_lid` is `(0.584, 0.800, 0.320)` (the 1.5KV lime body) and
`ammoboxlarge_darkgreen01` is `(0.750, 0.609, 0.188)` (the .27 CAL yellow body).

The rule, and why it is narrow:

- "Has a colour map" cannot be the qualifier: the database **does** name
  `paint01_color` for `PaintedMetalShine01_Orange`. The map is present and
  neutral; the colour lives beside it.
- What separates a paint from a layered composite is the layer kind. The ammo
  and painted-metal materials record none; the Razorback receiver records
  `Multiply` with `(0.67, 0.697, 1.0, 0.635)`, and applying that would tint a
  solid rifle. `FindCdbAlpha(..., &cdbKind)` gates on `cdbKind == 0`.
- The resolved map must then measure as a neutral paint base
  (`Bc1MeanRgb` + `IsNeutralPaintBase`: bright, `(max-min)/max < 0.12`).
  `paint01_color` measures (`197,198,197`) and passes; the crate's dark
  interior atlas `ammobox01_color` measures (`113,90,53`) and does not, so its
  grunge is left alone.
- Everything else keeps the existing white reset, and the drink bags keep
  their raw (unconverted) override via `cdbOverrideTint`.

Pinned by `tools/test_ammo_paint_tint.py` (six guards: the kind gate, the
neutral gate and its bounds, the sRGB conversion inside the paint branch only,
its constants, the BC-only measurement, and the untouched drink-bag path).
146 tests in `tools/`, 39 in `ba2_audit/` pass; `build.cmd` green.

Breadth check (`cdb_alpha_replica.scan()` / `scan_kinds()`): 8,760 distinct
materials carry a visible override with layer kind 0, but only 158 of them sit
under `setdressing/` (where the crates live), 139 `composite/`, 57
`architecture/` - and **none** under `weapons/`. The rest are bare stems, so the
neutral-map gate is what keeps the change honest there: a map that already has
its colour is untouched. Every range the rule fires on is now named in the log
(`paint tint '<mat>' kept (...) over neutral base (...)`), which is the line to
check first if an item comes back tinted that should not be.

Deployed `1F33B2D2…AF27904` (previous `CA8451D7…`, and before that
`D65B6886…`, replaced without archived copies). Not verified in game: the ammo
colours, like the pack, still need one live look.

## Ammo: hiding the rows a shop cannot sell, and where the crate's colour lives - 2026-10-18 (third pass)

### Hide turrets also hides unnamed, model-less and ship ammo

Starfield.esm holds **150 AMMO records**, and 105 of them do not belong in a
shop catalog:

- **51 unnamed** - one dummy AMMO form per crate NIF (`(unnamed) [002184A8] ·
  typee_xlarge`). They have a model but no player-facing name.
- **18 with an empty `MODL`** - the creature rounds (`Creature Fire Ammo`,
  `Vortex Horror Ranged Ammo`, ...). The engine draws them from the creature
  that fires them, so the record has no mesh at all.
- **9 whose only model is `testspaceshiprocketprojectile.nif`** - the debug
  projectile, not a crate.
- **27 the Ammo category already files as `kMissile`** - `Missile, CE-13`,
  `Gravity Torpedo Ammo`, `Missile, Atlatl 280C`, ... These do carry crate
  meshes, so "no model" alone would have kept them.

**Trap that cost a wrong first rule:** the audit dump's `modelPath` column for an
AMMO record is `NAM1`, the spent **shell casing**, not `MODL` (the model). Reading
it as "the model" made 61 named rounds look model-less - including `Missile,
CE-13`, which owns `typeb_xlarge.nif`. `tests/fixtures/ammo_catalog.csv` is now
built from the plugin files themselves (`MODL`, `NAM1`, name, EDID, keywords, 150
rows) instead of that column.

`IsUnbuyableAmmo` (unnamed, empty `MODL`, test-rocket model, or
`ClassifyAmmoSub` = `kMissile`/`kShipCreature`) is gated by `hideTurrets`: these
are ship and crate hardware and none carries a test/debug marker. The checkbox
reads **"Hide turrets/ship ammo"** so the toggle says what it does.

45 rows survive: 44 crate NIFs plus `Mine`'s `Weapons\spacemine\SpaceMine.nif` -
the calibres, the energy cartridges and the player-thrown mines/grenade. Pinned
by `tools/test_catalog_filters.py` (105 hidden / 45 kept, the NAM1-vs-MODL
regression, mines kept). 148 tests in `tools/`.

### Why the crate preview cannot match the crate in the world

The comparison screenshots (`.27 CAL` / `1.5kV LZR`) put the pop-out preview
next to a crate standing in the world. Measured off the captures:

| | preview | world crate |
|---|---|---|
| .27 CAL body | (144,128,96) tan | (192,176,112) yellow |
| .27 CAL band | warm, orange-dominant | (64,80,96) blue-grey |
| 1.5kV body | dark olive | (192,224,160) lime |
| 1.5kV band | orange | (64,80,96) blue-grey |

Both are the **same mesh** - `.27 Caliber`'s `MODL` is
`SetDressing\ammo_crates\Ammo27Caliber.nif` (4,578 tris, three parts:
`AmmoBoxLarge_DarkGreen01` 2,336 v on the dark `ammobox01_color` atlas, measured
(113,90,53); `AmmoBox01_Revised` 84 v / 58 tris, the stencil band;
`PaintedMetalshine01_Orange` 1,951 v on the white paint atlas `paint01_color`,
measured (197,198,197)). `ammobox_01.nif` (13,630 tris) is the **`Mag Cartridge`**
crate, not this one. So the difference was colour resolution inside the same
NIF, not mesh identity - and the single cause is which **map** each paint part
multiplied its override with:

| material | CDB override (linear) | as sRGB | is the game's |
|---|---|---|---|
| `AmmoBoxLarge_DarkGreen01` | (0.750,0.609,0.188) | (224,206,120) | yellow body |
| `PaintedMetalShine01_Green` | (0.623,0.663,0.670) | (200,205,207) | blue-grey skirt |
| `PaintedMetalshine01_Orange` | (0.846,0.676,0.395) | (237,214,168) | tan lid |

`DarkGreen01` and `_Green` both carry an **empty** CDB texture set, so
`FindCdbMaterialLocked` falls through to the containing folder and takes the
crate's dark `ammobox01_color` atlas (113,90,53); the overrides are then dropped
by the neutral-base gate and the dark atlas is drawn bare. `_Orange` has its own
set naming `paint01_color`, which is why exactly one paint slot looks right.

**The obvious fix was built, deployed and seen in game - and rejected.**
Routing every empty-set part under `setdressing/ammobox01/` to the shared paint
atlas gives each override a neutral base to multiply, and the result was every
ammo crate in the catalog rendering the same yellow body as `.27 Caliber`,
including `1.5kV LZR Cartridge`. The reason is in the table:
`AmmoBoxLarge_DarkGreen01` is the **body** material of all 22 per-calibre crate
NIFs, so painting it repaints the whole shelf one colour. Whatever decides the
wildly different crate colours in the world, it is not this shared material
(and it is not a material swap: no LMSW in `Starfield.esm` - 9,822 of them -
renames any `AmmoBox01` material).

The rule was reverted and is kept as a commented-out "rejected rule" in
`FindCdbMaterialLocked` with the measured reason, plus
`test_the_crate_folder_is_not_repainted_wholesale` so it is not rebuilt by
accident. What stayed from that pass is the audit line: every uploaded range now
logs `atlas … tint=(r,g,b) mask=… uv=[..]x[..]` on one `direct GPU: range …`
line. That is the instrument for the next attempt - it names, per part, the map
and colour the preview actually used, which is what three passes of this
question kept costing.

The band is the third open question. Its `AmmoBox01_Revised` `.mesh` UVs span
the **entire** stencil sheet (measured u `[0.007, 1.000]`, v `[0.002, 0.718]` of
a 1024² atlas of every calibre's lettering), so at preview size the orange glyph
rows merge into a warm band; the crate in the world shows one clean grey-blue
`Caution` plate over the same material. Either the game applies a texture-set UV
transform this preview has no support for, or its opacity mask (paired
`ammoboxrevised_opacity.dds`, red channel) selects a different subset. Not
decided - it needs a live material/UV dump of that one part.

Recorded here so the "why is the band orange" question starts from these
numbers instead of from scratch.

Deployed `41050E52…0146EC8` (atlas rule reverted, audit line kept; the previous
`E4CC8DAD…96D6D` is the build that turned every crate yellow, `20B4FA1E…938EF9`
corrected the filter rule, and `1463291A…6B0D7` shipped the NAM1 misreading).
150 tests in `tools/`, `build.cmd` green.

**Verified in game 2026-10-18**: on `1463291A…` the Ammo page with Hide turrets
on dropped the `(unnamed) … typee_xlarge` rows and the creature rounds; on
`E4CC8DAD…` every crate rendered the same yellow body, which is why that rule is
gone. Still unverified: the missile rows the filter now also hides, and whether
`.27 Caliber` / `1.5kV LZR Cartridge` are back to their previous (distinct)
looks under `41050E52…`.

## 2026-10-18 17:20 session - the two crates, part by part

The audit line now carries the atlas path (`range N '<mat>' <- slot S '<albedo>'`),
not just the slot number, so a session log answers "which map did this part get"
without cross-reading the prefetch lines. Read from that session:

    .27 Caliber        (Ammo27Caliber.nif, 4572 tris)
      DarkGreen01 2336 v  -> ammobox01_color        tint 1        atlas mean (113,90,53)
      Revised       84 v  -> ammoboxrevised_color   + red mask    uv [0.007,1.000]x[0.002,0.718]
      Orange      1951 v  -> paint01_color          paint tint kept (0.93,0.84,0.66)
                                                               -> (183,167,130)
    1.5kV LZR          (AmmoLaserSmall.nif, 4568 tris)
      Green       2053 v  -> ammobox01_color        tint 1        atlas mean (106,87,57)
      DarkGreen01 2224 v  -> ammobox01_color        tint 1        atlas mean (100,83,53)
      Revised       88 v  -> ammoboxrevised_color   + red mask    uv [-0.057,1.000]x[0.000,0.999]

The two paint slots are resolved by two different rules, and that is the measured
cause of the asymmetry, not a guess:

- `PaintedMetalshine01_Orange` has its own CDB texture set naming `paint01_color`,
  so its override survives the neutral-base gate: neutral (197,198,197) x
  (0.93,0.84,0.66) = (183,167,130), against the game's (189,173,102) front face /
  (201,186,118) lit. **This one is numerically right.**
- `PaintedMetalShine01_Green`'s CDB set is **empty**, so `FindCdbMaterialLocked`
  falls back to the containing folder and returns the crate atlas, whose mean
  under its UV footprint is (106,87,57). No neutral base means no paint tint, so
  its override is never applied.

Atlas contents were measured rather than eyeballed (`ammobox01_color` is dark
metal plus amber paint only - no lime, no blue-grey; `paint01_color` is uniform
(197,198,197)). That matters because the 1.5kV preview shows a **lime** body
(~200,235,150, which is `paint01` x (0.584,0.800,0.320)) and a grey-blue `Caution`
plate - neither of which any of the three ranges above can produce. So the next
measurement is one fresh log on `BD7E83B8…1008910`, where the audit line names
the atlas and the disagreement becomes readable instead of inferred.

## The per-crate colour is a CDB *variant*, not a material

The crate family's colours are authored as three names, and they spell the
intended design exactly - white base, coloured lid:

    AmmoBoxLarge_Orange01_Material2         (0.857, 0.857, 0.857)      white base
    AmmoBoxLarge_DarkGreen01_Material2      (0.750, 0.609, 0.188)      yellow lid  -> sRGB (224,206,120)
    AmmoBoxPrimer_Lid_Material1             (0.584, 0.800, 0.320)      green lid   -> sRGB (201,231,152)
    PaintedMetalshine01_Orange_Material1    (0.846, 0.676, 0.395)      tan
    PaintedMetalShine01_Green_Material1     (0.623, 0.663, 0.670)      blue-grey

The decisive detail is that **the same object name appears more than once**.
`AmmoBoxLarge_DarkGreen01_Material2` was found three times, each with its own
0x020F id and its own 0x025A colour:

    id 0x68  (0.750, 0.609, 0.188, a=1)   yellow
    id 0x66  (0.891, 0.729, 0.162, a=0)   brighter yellow, weight 0
    id 0x??  (0.600, 0.500, 0.299, a=1)   tan / olive

`cdb_alpha_replica.scan()` keeps the first with `setdefault`, so every crate that
names this material receives the same colour. That is the measured reason a
whole-shelf rule turned all 22 crates one yellow: not a shared body material -
a shared *record*, with the variant never chosen. It is also why `.27` currently
draws tan: it is picking the tan variant while the crate in the inventory draws
the yellow one.

The chooser is per form: `BGSModelMaterialSwap::colorRemappingIndex` (the field
Starfield uses to select a colour variant). No code in this plugin reads it yet
(`grep -rn RemappingIndex src` is empty), so nothing can pick a variant today.

Next: read `colorRemappingIndex` from the ammo form when the preview request is
built, log it next to the model path, then index the CDB variants by that value
instead of `setdefault`. One screenshot of `.27` and `1.5kV` in the inventory plus
their two indices settles which variant is which.

## Probing the inventory's own render (2026-10-18, build 529C982A)

The shop preview is the render under investigation, so it cannot be the source of
truth for itself. The inventory's preview is; the plugin already locates its
objects (`preview-chain: menu=InventoryMenu … renderer=… refr=…`, measured at
17:59:54 in the 17:58 session). New probe:

    NativeModel::ProbePreviewMaterials(chain.refr, chain.renderer)

called once per previewed reference from the chain tick. It walks the engine's
live preview tree with the same fault-gated walk the Barter read-back uses and
logs, per geometry:

    preview material: 'Ammo27Caliber:2' mat='Materials\SetDressing\AmmoBox01\AmmoBoxLarge_DarkGreen01.mat' tex='…'

Raw offsets and CanRead/IsValidPtr only - no CommonLibSF types. The runtime
material object is a component-DB entry that CommonLib does not map (and whose
ids are stale for this build), so the colour itself is still not read; what this
establishes first is whether the engine resolves a *different albedo* for the
crates than the plugin does. If it does, the variant question is about texture
resolution, not about a colour float.

## Probing moved out of the shop (2026-10-18)

Investigating a render with a probe that lives inside that same render is a
closed loop, and it left three throwaway probes in the shop plugin: the colour
remap index read, the inventory preview-material walk, and the atlas name added
to the per-range audit line. All three are gone (`9D56E474…013A4412`), and so is
the test that pinned the audit-line format; back to 150 tests.

The probe now lives in the dedicated dump plugin (`SFDevilzDump`, `0f03D0DC…`),
where it belongs: press F10 with the item on screen and the capture gains a
`### InventoryMenu preview renderer` section listing the tree under the
`UICellRenderer_InventoryItem`, each object's pointer and RTTI, and every
`.mat` / `.dds` path that object holds.

That is the readable answer to "what is the engine drawing this crate with",
obtained from the engine's own preview instead of re-derived from the database.

## Aid previews ghosted: the layer kind never joined (2026-10-18, build 0.8.304)

Twelve Aid screenshots (`Pictures/Screenshots`, 22:34-22:45) all show the same
thing: the engine's own preview beside the plugin's, and the plugin's item
translucent, over-bright and wearing colours the game never draws. The session
log says why in one line:

    offline GPU: 'direct:Velocity Yellow [FormID 001EBE9E]#b44a732e4cd02c7f'
                 translucent ranges 6 of 6 (e.g. 'Materials\Common\Metal\MetalAluminum01.mat')

Six of six ranges on a metal can, and not one
`override alpha ... ignored ... (Multiply layer, not surface opacity)` line in
26,000 lines of log. `MetalAluminum01` is a Multiply layer (field 0x22b, kind 1)
carrying a 0.392 blend weight, so it should have been drawn opaque. The
Tranquilitea cup (0.608, no kind) and the Blend can body (0.025) were drawn
at those values as well: a 61%-transparent can and a 97%-transparent printed
can, next to the engine's fully opaque versions.

The cause is an alias asymmetry, not the rule. `EnsureCdbIndex` stores each
override colour under four spellings - the bare key plus `<key>.mat`,
`materials/<key>.mat` and `data/materials/<key>.mat` - because
`FindCdbAlpha` looks up a material as a path and as a stem, and both end in
`.mat`. The kind map stored only the full key and, when it had a slash, the
bare name. Every runtime lookup therefore landed on a `..._mat` spelling the
kind map did not have, so `layerKind` came back 0 and the blend weight was
applied as surface opacity. The replica scans hid it: `scan()` compares bare
names, so the invariant they pinned was true while the game disagreed.

Fix: `addKindAliases` records the same four spellings for the kind, so the join
survives whichever spelling the lookup uses. 3,131 override alphas below 0.995
now resolve to a Multiply/Lerp layer and draw opaque (measured over
`materialsbeta.cdb`).

Second, the alphas that are not layer kinds and still cannot be coverage:
`MenuItem_Blend.mat` stores 0.025 and a large family of painted ship metal,
cave floor and hardwood materials store 0.0039 (1/255) - the same signature as
the Chunks cap's 8/255 garment weight. Below `kMinSurfaceAlpha = 0.05` the value
is a layer weight even without a kind record, so the branch no longer returns it
as opacity; control falls through to the token scan, which is what still gives
glass its documented 0.30 fallback (2,096 materials store alpha 0.0 and rely on
it).

Superseded in part: the same release left the 4,458 no-kind alphas as opacity,
and that was wrong for consumables - see the 0.8.306 section below (the
Tranquilitea cup and the Velocity beverage are byte-exact weights, and they were
the two items that did not move).

Tests: 10 in `test_cdb_layer_kind.py` (new: kind readable under every alpha
spelling, surface floor, both pinned against the shipped database) and 15 in
`test_decal_blend_policy.py`. Suite: every file green except
`test_ammo_nif_audit.py`, which reads the live game log and needs an ammo
preview in it - this session opened only Aid items (0 ammo previews in the log).

Build 0.8.304, `releasedbg`, sha256 `1B8694C5…27685D`, deployed to
`DevilzDad's ESM Shop Mod Explorer/SFSE/Plugins/`; the previous DLL is kept as
`SFShopExplorer.dll.backup-before-kind-aliases-20261018-234731`. Schema is 9,
so the first launch rejects the v8 index and rebuilds it - expected, and the
rebuild is what puts the new aliases in the cache.

## Six Aid items that stayed the same: a stand-in map, a weight read as opacity, and a grunge overlay (2026-10-19, build 0.8.306)

After the layer-kind join shipped (0.8.304), six items were reported unchanged:
Tranquilitea, Hypergiant Heart, Velocity Yellow, Cheese, Erdebrau Light Can and
Captain Galaxy's Note 02. Re-measured from the game's own NIFs and the shipped
database, they are three separate causes, none of them the kind join.

**The models first.** `FormModelPaths` reads the form's `BGSModelMaterialSwap`
model string, so the preview and the game do load the same file - the catalog
CSV simply does not carry those strings:

| item | model (from the record's MODL) |
|---|---|
| Tranquilitea "To Go" Cup | `SetDressing\Tranquilitea\TranquiliteaToGoCupFull01.nif` |
| Hypergiant Heart | `Items\Drugs_Or_Medical\crafted_inhailer_amp_01.nif` |
| Velocity Yellow | `Items\FoodDrink_SingleItems\Velocity_Beverage\Food_RS_MSauvage_VelocityYellow.nif` |
| Cheese | `Itemsooddrink_singleitems/all_packaged_consumables\PackagedFood_Cheese01.nif` |
| Erdebrau Light Can | `Items\FoodDrink_SingleItems\Erdebrau_Beers\Erdebrau_Light01.nif` |
| Captain Galaxy's Note 02 | `SetDressing\Books\BookLarge02.nif` |

**Cause 1 - a stand-in albedo erased the authored colour.** The database names
no colour map for `Tranquilitea_ToGoCup_Base01` (teal 0.139/0.764/0.810) or for
`Inhailer_Amp` (yellow-green 0.983/1.0/0.0). `FindDiffuse` therefore fell to the
family scorer, which returned a neighbour's atlas, and `UploadModelTextures`
reset the range to white because "an albedo landed". The map was never this
material's art. The upload now compares the bound map with the material's own
database set: only its OWN map may carry the colour.

**Cause 2 - a byte-exact alpha read as surface opacity.** The cup's base and lid
both store 0.607843 (= 155/255) and `Velocity_Beverage_Yellow` stores 0.501961
(= 128/255), with no layer-kind record. Two unrelated surfaces sharing one
byte-exact value is a layer weight; the engine draws all three opaque. Consumable
materials (`/fooddrink_singleitems/`, `/drugs_or_medical/`) now take the same
branch garments do, with the token fallback still giving glass its 0.30.

**Cause 3 - the grunge overlay multiplied the authored colour away.** The
Velocity beverage's only map is `grungeoilslick01_color` - decoded with
`dump_tex.py` it is a blue/purple oil slick - while the database authors the
surface (1.0,0.917,0.0). A saturated authored colour on a `textures/common/grunge/`
map now leaves the range untextured so the colour shows.

**Cause 4 - the Erdebrau label lives on a file named after the decal material.**
`ErdeBrau_Can` only has the neutral `paint01_color`, and the database has no set
for `Erdebrau_Light_Decal`, but `Starfield - Textures05.ba2` ships
`erdebrau light_decal_color.dds` (and the pilsner one). Both now have exact
routes, like the Tranquilitea sleeve and the book covers before them.

**Not fixed, and why.** Cheese's own map IS `cheese_color.dds` (verified by
decoding it: the Bostaurus package art), so its remaining difference is lighting,
not a wrong texture. Note 02's cover swap runs in the Clone3D path
(`ApplyCaptainNoteMaterialSwaps`, B01 -> B04); if it still reads light blue, the
next measurement is the `book:` log line naming the material the preview used.

Tests: 9 in `tools/test_aid_preview_tints.py` (source rules plus the shipped
values), and the two tests that pinned the old garment-only log text updated.

Build 0.8.306, `releasedbg`, sha256 `3BDBA858…CA3ADE`, deployed to
`DevilzDad's ESM Shop Mod Explorer/SFSE/Plugins/`; the previous DLL kept as
`SFShopExplorer.dll.backup-before-aid-tints-20261019-0949`.

## Every shelf in one table, and the alpha that was never opacity (2026-10-19, build 0.8.307)

The Aid audit answered colour questions for one shelf. The same resolver now
covers the rest: `aid_colour_audit.py --shelf ammo|books|misc` (and `--aid`,
unchanged) selects a shelf by the form type ShopExplorer's own tab maps to
(`BrowseFormType` in src/ShopExplorer.h), never by category keyword - the dump
carries a keyword string for 1,530 of its 75,999 rows and none of those name a
shelf. `test_aid_colour_audit.py` reads that switch and fails if a shelf's form
type drifts.

| shelf | items | material rows | table |
|---|---|---|---|
| Ammo | 154 | 236 | `reports/ammo-colour-audit.html` |
| Books | 1,368 | 6,120 | `reports/books-colour-audit.html` |
| Misc | 1,482 | 5,540 | `reports/misc-colour-audit.html` |

The Ammo table also documents what cannot be previewed at all: 21 of its 154
records carry no MODL (projectiles, creature ammo) and 91 name a mesh that is
not in any archive on this machine - `SetDressingmmo_crates	ypea_xlarge.nif`
and `Ammo\FatMan\AmmoFatMan.nif` are cut content, not a lookup failure, since
the folder itself ships 26 other crates. Those rows say which path was tried;
the TSV carries it in the `model` column too.

**The override alpha is not surface opacity unless the name says so.** Two
items were reported unchanged after 0.8.306: the Tranquilitea to-go cup drew
ghosted and the Isotopic Coolant jar's lid was see-through. Both store a
database alpha - 0.607843 on the cup's base *and* lid, 0.251774 on the lid -
and neither names itself glass, clear, translucent, transpar, visor or lens.

The audit made the scale of this measurable: across the Aid and Ammo shelves
(exactly 1,295 material rows, 519 items) **only four distinct materials ever
reach that branch**. Three are solid on screen - the two Tranquilitea parts and
the coolant lid - and the fourth is `GlassHelmets01` at 0.686, which does name
itself glass and is the body of every inhaler. So the database value now
survives only where the material's own name agrees with it; everywhere else the
range stays opaque and the token scan below still gets its say. The inhalers
keep 0.686 rather than dropping to the 0.30 guess, which is why they were
already "close enough".

**Glass the database does not mark.** Alien Genetic Material's jar body,
`Isotopic_Coolant_Container`, has an *empty* database set - no colour, no
opacity, no maps - so it fell through every rule and drew as a solid grey tub
with the specimen buried inside. It is glass on screen. `kAuthoredSheer` in
`OfflineModel.cpp` now names it (with `kAuthoredSheerAlpha = 0.30`, the same
documented stand-in the token fallback uses), keyed by material because every
Legendary Catalyst reuses the coolant jar; the audit mirrors the list and the
value.

**Still open.** Erdebrau Pils: unchanged, the mask's black region still covers
the green wrap and nothing in the files decides whether the engine treats that
mask as coverage or as a cutout. Sparkling Wine: `Starfield - Textures09.ba2`
holds only `genericspacewine_decal_normal.dds` and
`genericspacewine_decal_opacity.dds` - there is no colour map anywhere, so the
bottle's dark body is a glass-shader tint and the black net in the preview is
that colourless decal drawn with its opacity mask. Books and inhalers were
called close enough.

Tests: `test_aid_colour_audit.py` (38) pins the new gate, the authored-glass
list and the shelf mapping to the C++; tools 176, renderer_recon 126.

Build 0.8.307, `releasedbg`, sha256 `7B096548…C05B7E`, deployed to
`DevilzDad's ESM Shop Mod Explorer/SFSE/Plugins/`; the previous DLL kept as
`SFShopExplorer.dll.backup-before-sheer-alpha-20261019-1131`.

## Contact sheets: the shelves, drawn offline (2026-10-19)

The colour audit says why a material resolves the way it does; it never drew
anything, so "does it look right" still cost a game launch. `contact_sheet.py`
closes that loop: MODL -> NIF -> BSGeometry -> LOD0 `.mesh`, every material
resolved by importing the audit's own swap/route/CDB/alpha decisions, one PNG
tile per item plus a filterable HTML grid per shelf.

    python tools/renderer_recon/contact_sheet.py --shelf aid --out reports/contact-aid.html

What the first pass had to get right, in the order the tiles exposed it:

**One fit for the whole item.** Framing each geometry independently scaled the
Isotopic Coolant jar's lid up to the full tile and drew it across the middle of
the glass. The camera now frames the union of every part, then projects.

**Additive overlays are not surfaces.** The same jar's water-caustic gobo is a
full jar-shaped shell; drawn opaque it blacked out the jar and the specimen.
Caustic/gobo/glow materials are skipped, and the jar now reads as clear glass
with the specimen inside - the in-game look the audit had predicted.

**A colourless decal is a mask.** Sparkling Wine's black net and the grey panels
wrapping the coolant jar are decal sheets whose set names an opacity map and no
colour. Drawing them flat grey invents a surface; they are skipped too.

Sizes: Aid 365 tiles / Ammo 154 / Books 1,368 / Misc 1,482, at 128 px each, with
a TSV twin (geoms, tris, textured count, status) for diffing two runs. Test:
`test_contact_sheet.py` (14) pins the BC1/BC4 decoders, the `.mesh` layout
against ParseMeshLod0, the camera fit, and the jar-glass/cup-opaque split using
the real archives.

## The white sleeve: closed shells and one-sided Fresnel (2026-10-21)

The 2026-10-20 pass stopped the four items being dark; the 10:17-10:20 captures
show they came back pale. All three (Alien Genetic Material, Sparkling Wine,
Erdebrau Pils Can) share one cause, and it is in `kPsBlendTexSrc`, not in any
material resolution:

    float fres = pow(1.0 - saturate(dot(n, viewDir)), 5.0);   // one-sided
    float a = saturate(g_alpha + fres * 0.85);                // 0.30 as coverage

**The far wall.** Every other lighting term in the file is two-sided via
`abs(dot(...))`; this one saturated. A closed shell is rasterised with
`CULL_MODE_NONE`, so its far wall presents normals pointing away from the eye,
`saturate` clamps the dot to 0, Fresnel goes to 1 and the alpha to 1 over that
whole surface. The far wall alone painted the item with the room gradient
(0.72,0.77,0.88 at the top of the env lerp). That is the opaque white cup
inside every glass item.

**The 0.30.** `kAuthoredSheerAlpha` and the glass-name fallback were documented
in the source itself as stand-ins, "not a recovered surface value". Used as
coverage they composite two walls to 1-(1-0.30)^2 = 51% of a 0.39-grey body.
Both are 0 now: a clear dielectric occludes only by its Schlick reflectance
(F0 0.04 head-on, 1.0 at the silhouette), and the shader derives coverage from
that. Authored opacity survives as absorption (`body = albedo * lit *
(1-F) * g_alpha`), so GlassHelmets01 at 0.686 still reads as smoked glass.

Measured on the analytic one-wall model, two walls of a clear shell: old
alpha 0.51 / rgb 0.39 (item 49% visible, ~0.20 of grey added), new alpha 0.078
/ rgb 0.011 (item 92% visible). The rim still reaches ~0.96 alpha where the
silhouette turns.

The can also settled a false lead. Its NIF (`Erdebrau Pilsner01.nif`, 7
geometries) wears exactly two materials: `Erdebrau_Pilsner_Decal.mat` on the
five label/body meshes and `GlassCleanTransparent01.mat` on the two largest
shells - a printed sleeve inside a clear plastic one. The atlas is green where
the label ranges sample it (80% saturated green under range 0, 49% under the
body wrap), so the colour was always right and simply sat under the glass. No
material work was needed for it.

Tests: two-sided Fresnel, Schlick coverage, `body + reflection` and both
stand-in constants are pinned in `test_glass_and_wrap_blend.py`; the three
assertions that pinned the old form were rewritten with their intent kept
(glass stays translucent; the audit still mirrors the C++). Full suite 358 OK,
fxc compiles every literal. Build sha256 `62989803…2750512`, deployed.

## The jar's green and its bubbles: an authored tint and a real additive pass (2026-10-22)

The wine and the inhalers were signed off; this capture pair left the Alien
Genetic Material as the only item still visibly unlike the game. Shape, lid,
specimen and framing all matched. Two things did not: the glass was blue-grey
where the game's is green, and the jar had no fluid at all where the game's is
full of bright bubbles.

**The green cannot be looked up, and that is settled.** `cdb_records.py --name
Isotopic_Coolant` prints the whole family: `Isotopic_Coolant_Container_Material1`
is an id record and its name, and nothing else. No colour map exists in the set
(`Isotopic_Coolant_Container_TextureSet1` names only `paintpeeling01_rough`),
no override colour is stored, and the same emptiness covers the `_Fluids`
variants. So the glass had the generic pale tint every untextured translucent
range gets - the same one the wine and the snow globe want - and the only
difference between the preview and the game was colour.

It is authored, exactly like the alpha in the section above, and measured the
same way. In the 2026-10-22 12:04 capture the left glass wall averages
(27,43,44) over an (8,23,31) backdrop: it ADDS (19,20,13)/255, green above red,
blue lowest. At the same framing this renderer's pale tint (0.62,0.66,0.72) was
adding (35,36,37). Each channel is the pale tint scaled by that ratio:
`kAuthoredGlassTint = {0.34, 0.37, 0.25}`, keyed to the same material list as
`kAuthoredSheer` so no other glass moves. `aid_colour_audit.py` mirrors the
constant; `test_aid_colour_audit.py` fails if the two drift.

**The bubbles are the fluid shell, and they were being skipped.** The jar's
`Node02_A:74` wears `Materials\SetDressing\Animated\WaterCaustic01_IdleAnimLoop.mat`,
a full-body caustic gobo. The renderer has skipped effect shells since the
contact sheet needed that shell out of the specimen's way - correct while the
glass was opaque, wrong once it was clear and the game showed bubbling green
through it. An effect shell is not a surface, but it is not nothing either: the
engine draws it as light ADDED to the frame, which is exactly why its map is
mostly black.

The fix is a third draw pass. `psoAdditive` blends ONE/ONE with no depth write,
and `kPsAdditiveSrc` returns `ink * i.color * 0.80` - a black texel adds
nothing, a bright caustic ribbon adds light over whatever is behind it. The
pass runs after the opaque and blended passes so the specimen and the glass are
already on the target. The shell wears the jar's own colour when the model
wears the authored glass (the fluid inside is what the green is made of), and
stays white for any caustic gobo elsewhere.

The glow shell is still skipped. `LegendaryCatalyst_Glow02` is an emissive
companion of the artwork; the only map this renderer can bind for it is that
artwork's full-colour specimen atlas, and adding that at full strength would
wash the specimen out. It keeps the old skip; the caustic loop is the only
material that becomes additive.

Tests: `test_glass_and_wrap_blend.py` pins the tint values, both application
sites, the ONE/ONE depth-write-zero PSO, the shader's emission line, the
three-pass routing and that a shell with no map is not drawn;
`test_film_material_blend.py`'s shader census now expects five pixel shaders and
exempts the additive one from the diffuse exposure anchor (it has no lighting
rig by design). Suites: tools 226 OK, renderer_recon 126 OK, fxc compiles every
literal. Build 0.8.317, `releasedbg`, sha256 `67D7196F…D0ECCC80`, deployed with
the previous DLL kept as `…backup-before-jar-green-20261022-1330-v0.8.316`.

## The Sparkling Wine's web is a Blender mask (2026-10-22)

The wine has been the one Aid item on every "still wrong" list: a dark grey tube
in the preview against a white bottle with a black net in the game (2026-10-22
inventory capture). The close-out said the bottle's colour "is a glass-shader
tint the database does not carry", which was true of the texture set and wrong
about the material.

`GenericSpaceWine_Glass`'s set is empty, but its **Blender layer** is not:
materialsbeta.cdb object `GenericSpaceWine_Glass_Blender1` carries one texture,
`Data\TEXTURES\Common\Detail\Manmade\DetailAbstractPattern06_mask.DDS`
(field 0x038a). Decoded, that map is white cells under a black web - the bottle
in the capture. The other map the audit had been reading,
`genericspacewine_decal_opacity.dds`, is the ornate crest label (a white
silhouette on black) belonging to the *decal* material, not the body.

So the body is an opaque printed surface, not clear glass. `kAuthoredPattern`
binds the Blender mask as the range's albedo, the build takes that range out of
the blend pass (opaque, alpha 1, white tint) and the upload flags it as a
single-channel albedo, which routes it to mode 11 - the greyscale-ink arm that
was already there for `maskInkMode`. White cells shade as white body, the web's
black cells shade dark; nothing is clipped away.

Scoped to the one material that authors it, like `kAuthoredSheer` and
`kAuthoredGlassTint`: a general pass would have to carry the Blender map for
every stem through the CDB index and its cache, and no other shelf item has
been measured against that yet.

Tests: `test_glass_and_wrap_blend.py::SparklingWinePatternTests` pins the table
and its map, the prefetch arm (and that a single-channel albedo is otherwise
still rejected), the opaque/white build override and the white tint the ink arm
needs. Suites: tools 230 OK, renderer_recon 126 OK. Build 0.8.318, `releasedbg`,
sha256 `20FAE375…B9909057`, deployed with the previous DLL kept as
`…backup-before-wine-pattern-20261022-1420-v0.8.317`.


## The wine's print was zoomed in: the island is a quarter of the sheet (2026-10-23)

The 0.8.318 bind gave the right material and the wrong size. The capture of the
popped-out preview showed two or three web cells across the bottle where the
game's own inventory view has five or six, and strands as thick as the cells.

**The map is a tiling sheet, not an atlas, and it was sampled through an
atlas-sized island.** `detailabstractpattern06_mask.dds` decoded whole holds
88 cells with 22-texel strands, while `nif_uv.py` reports the glass shell it is
bound to at `u=[0.010,0.262] v=[0.749,0.997]` - a quarter of the sheet in each
axis. Straight through that island the bottle's front covers 0.126 of the
sheet, i.e. ~1.6 cells on screen. The in-game capture (`wine_game.png`) counts
33 white cells on the visible half, which is what one repeat of the sheet over
the shell's surface predicts (~44) and four times what the raw island draws.
A scale-free edge-density comparison of the two captures puts the gap at
3.0-3.5x, the same order; the difference between that and 4.0 is the capture's
own framing and threshold, not the mapping.

The island is square (0.252 x 0.248) and the bottle's body height and
circumference are within a few percent of each other in the capture, so
mapping it onto [0,1] stretches neither axis.

`accumulate()` therefore normalises the UV island of an authored-pattern mesh
as it writes the vertex buffer - `(uv - min) / span`, x3.97 u and x4.02 v on
the wine - and logs the factor once per mesh:

    pattern '...\GenericSpaceWine_Glass.mat' island u=[0.010,0.262] v=[0.749,0.997] -> x3.97/x4.02

Scoped to `kAuthoredPattern`, like every other authored rule: no other range
sees different UVs. Because this happens at vertex emission the relief arm
samples the sheet's `_normal` companion at the same density, and no shader or
constant-buffer layout moved.

Checked offline against the same capture before shipping:
`_wine_pattern_density_probe.py` rasterizes the shell per pixel through the
normalized UVs (the contact sheet's own renderer shades one mean colour per
triangle and cannot show a texture's scale). Its edge density over a fixed
fraction of body height is 21,169 against the game's 18,407 - within 15%, so
the print is now the game's, not merely a finer one. The old raw-island
mapping measured 2,084 by the same yardstick.

Tests: `SparklingWinePatternTests` pins the conditional transform and parses
the shipped `.mesh` out of `Starfield - Meshes02.ba2` to prove the island is
0.252 x 0.248. Suites: tools 236 OK, renderer_recon 130 OK. Build 0.8.321,
`releasedbg`, sha256 `7826BF7E...D9A769DB1`, deployed with the previous DLL
kept as `...backup-before-wine-uv-density-20261023-1242-v0.8.320`.

**Not yet seen in game.** The count above is the offline sheet, the shipped
mesh and the 2026-10-22 capture; the next 1553361 capture should show the net
at the game's cell count.

## A-300 selection CTD: allocator reset before the preview frame completed (2026-10-24)

Selecting **A-300** (`00000FD6`, vanilla Razorback) in the shop closes the game.
The last shop line is `preview SRV bound (slot 20)`, about 0.7 s before the
process ends. The item's five assembly NIFs, its receiver, `a300.mat` and
`a300/color.dds` are all owned by **Star Wars Blasters and Melee** (the loose
`Razorback.nif` is from *Star Wars Blasters - 1st Person Sights*), and the
preview is the first one in the session to upload a mod GNRL DDS texture
(BC7 sRGB, 1024x1024, 11 mips). The part walk extracts no geometry from the
mod's five NIFs (`ReconScope.nif` logs `stage nogeo`), so the two drawn meshes
come from the engine-cloned receiver.

**Candidate root cause, not yet proven in game.** The overlay waited only
1000 ms for its fence and ignored `WAIT_TIMEOUT`. The next Present then reset
`g_allocator` and `g_commandList` while the previous preview command list was
still in flight; D3D12 removes the device and the game closes. The first
textured preview after 0.8.321 is the longest first draw, which is why it
exposed the path. There is no WER event or Starfield dump from either run, so
there is no faulting stack to confirm this; both scenarios (slow first draw
vs. a genuinely removed device) end at the same line.

Fix in 0.8.322: `WaitForGpu` returns false on timeout, the caller skips the
frame without resetting the allocator, the wait is 5 s, the timeout logs the
device-removed reason and elapsed time, a synchronous removal after submit is
logged, and `Shutdown` drains the queue before releasing preview/capture
resources. A wait over 100 ms is logged as well, so the next run shows the
first-draw cost.

Tests: 44 OK (`test_overlay_input_recovery`, `test_build_gate`,
`test_legacy_preview_gate`). Build 0.8.322, `releasedbg`, md5
`93d89c2c76c20eb143095a9a24007462`, deployed with the previous DLL kept as
`...backup-before-gpu-wait-20261024-0920-v0.8.321`. **Not yet verified in
game.** Relaunch and select A-300: if it survives, the timeout was the trigger;
if it still closes, the log now carries `GPU wait present timed out ... device
0x...` or `device removed after overlay submit: 0x...`, which names the
device-removal code instead of leaving the run silent.

## 0.8.323: the waited-on crash was not the wait

The 0.8.322 build was tested on the same profile. The log is explicit:

```
[09:25:12.469] offline GPU: texturing 'direct:A-300 ...' (1 ranges, 2 prefetched)
[09:25:12.472] direct GPU: range 0 'materials\weapons\swweapons\a300.mat'
              <- slot 1 fmt=97 1024x1024 pitch=4096 tint=(1.00,1.00,1.00) mask=none
[09:25:12.472] offline: preview SRV bound (slot 20)
[09:25:12.472] device removed after overlay submit: 0x887A0001
```

There is **no** `GPU wait present timed out` line, so the 5 s wait and the
frame-skip did not run. The device is already removed when the overlay's
command list is submitted, i.e. one of the recorded commands is invalid, and
DXGI reports `DXGI_ERROR_INVALID_CALL` (0x887A0001) rather than a hang or a
page fault. The A-300 request is the first textured GPU preview of the session:
pipeline ready, one body range with a BC7 1024x1024 albedo, the 12-triangle
`Lawgiver_aim` mesh filtered out, no opacity/normal/detail maps.

Build 0.8.323 is a **diagnostic** build, not a fix:

- `main.cpp` enables the D3D12 debug layer at plugin load, before the game
  creates its device.
- `ImguiOverlay.cpp` attaches `ID3D12InfoQueue` to that device and drains
  warnings/errors after every overlay submit (and at frame start), logging the
  message severity, ID and description.
- Break-on-severity is disabled so the message reaches the log instead of
  taking the process down in the debug layer.

Expected next log line, immediately before the `device removed` warning:

```
SFShopExplorer D3D12 after overlay submit: severity=1 id=... <description>
```

That description names the invalid call. If instead the log says
`info queue unavailable (device predates debug layer)`, the game created its
D3D12 device before SFSE plugins load and the debug layer path cannot see it;
the next step is then to split the preview's command list from the overlay's
and log the debug layer output from a dedicated probe. Remove the debug-layer
block from `main.cpp` once the cause is fixed: it is a diagnostic, not a
shipping setting.

Build 0.8.323, `releasedbg`, SHA-256
`ae7680f2e50631dfe22dcb9232e4b129f6fb6e96a64b691fc1d3dbce4a2375b8`, deployed
with the previous DLL kept as
`...backup-before-d3d12debug-20261024-0930-v0.8.322`. **Not yet verified in
game.**

## 0.8.323 result: the debug layer cost the launch

0.8.323 enabled the D3D12 debug layer at plugin load. The log proves the
device was created after the plugin (so the timing was right) and the info
queue attached:

```
[09:52:30.687] SFShopExplorer diagnostics: D3D12 info queue attached (0 stored messages)
[09:52:30.755] SFShopExplorer: first native draw submission beginning
[09:52:30.755] SFShopExplorer D3D12 frame start: severity=2 id=1424
              ID3D12CommandQueue1::Wait: Fence values can never be less than zero ...
```

The process then closed before the shop opened. The debug layer validates the
**game's** command lists too; the game makes calls it tolerates without the
layer, and one of them takes the process down once validation is on. It is not
usable as a diagnostic here.

## 0.8.324: DRED instead of validation

- The debug layer is gone; `D3D12Debug::EnableDebugLayer` is not called.
- `main.cpp` enables DRED auto-breadcrumbs and page-fault recording before the
  game creates its device. DRED only records operations; it does not validate
  or break.
- When the overlay sees a removed device, `LogDred` queries
  `ID3D12DeviceRemovedExtendedData` and logs the breadcrumb count, the last
  completed index, the operation codes around it, and any page-fault VA:
  `D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION (8)`,
  `RESOURCEBARRIER (15)`, `DRAWINDEXEDINSTANCED (4)`, etc.

Expected A-300 log if it still crashes:

```
SFShopExplorer: device removed after overlay submit: 0x887A0001
SFShopExplorer DRED after overlay submit: breadcrumbs=N lastCompleted=...
SFShopExplorer DRED after overlay submit: [i] op=...
```

The op code at `lastCompleted + 1` is the command that was executing when the
device was removed, which distinguishes the texture upload (copy), the
barriers, and the model draw without guessing.

Build 0.8.324, SHA-256
`0183a7372007614d7576d85504733414a7bc68b82bdce68db6fe13c84b7e8aa8`, deployed
with the debug-layer DLL kept as
`...backup-before-dred-20261024-0955-v0.8.323-debuglayer`. 44 tests OK.
**Not yet verified in game.**

## 0.8.324 result and 0.8.325 mitigation

0.8.324 launched and reached the shop, so removing the debug layer fixed the
launch crash. A-300 still removed the device at submit with 0x887A0001, and
the DRED dump was **silent**: the `device removed` warning is the last line.
Two reasons were possible: the settings call sat before `SFSE::Init`, where
REX has no sink, and `LogDred` logged only when it had something to say.

0.8.325 does two things:

1. DRED setup moved after `SFSE::Init` (still before the game creates its
   device) so the enable/failure line is in the log, and `LogDred` now logs
   every step: QueryInterface HRESULT, `GetAutoBreadcrumbsOutput` HRESULT and
   head pointer, breadcrumb count/last index/op codes, and the page-fault
   HRESULT/VA. If DRED is unsupported or empty, the log says so.
2. BC7 textures with more than one mip are uploaded as the **base level only**
   for now. A-300 is the first preview to carry a 1024x1024 BC7 atlas with 11
   mips; the per-mip `CopyTextureRegion` chain is the only command difference
   from the previews that survive. The texture still samples correctly at the
   preview's scale and the DDS chain is not touched on disk.

This is a diagnostic **and** a candidate fix. If A-300 now opens, the mip
chain is implicated and the upload should be fixed properly (copy all mips or
keep base-only with a comment); if it still crashes, the DRED op at the
`lastCompleted + 1` index names the remaining command.

Build 0.8.325, SHA-256
`d6acfd13c87b45d742c5e5abe1364c504ace398457d38ba6ad5f2015b72c2f58`, deployed
with the 0.8.324 DLL kept as
`...backup-before-topmip-20261024-1018-v0.8.324`. 44 tests OK. **Not yet
verified in game.**

## 0.8.325 result: the mip chain was not the cause

The log confirms the mitigation ran and did not help:

```
[W] SFShopExplorer offline GPU: BC7 1024x1024 has 11 mips; uploading base level only (diagnostic)
[W] SFShopExplorer DRED after overlay submit: auto breadcrumbs hr=0x00000000 head=0x0
```

DRED reported no breadcrumbs at all, and the device was still removed. The
BC7 atlas was uploaded as a single level, so the per-mip copy chain is
cleared; the DRED opacity is empty because the removal happens on the GPU
side after submit, not in a command D3D12 recorded on the CPU path.

## 0.8.327: BC7 was uploaded as BC7_TYPELESS

The real defect is in `MapTexFormat` in `src/LiveModel.cpp`. DXGI formats
97/98/99 are **BC7_TYPELESS / BC7_UNORM / BC7_UNORM_SRGB**; the old mapping
returned **97** for all three. A typeless texture resource is legal, so
`CreateCommittedResource` succeeded and `GetCopyableFootprints` sized the
upload correctly - which is why nothing looked wrong. But `UploadTextureImage`
then built the SRV with the same format:

```cpp
srv.Format = static_cast<DXGI_FORMAT>(a_format);  // 97 = BC7_TYPELESS
```

A typeless **view** format is invalid in D3D12. `CreateShaderResourceView`
returns void and was not checked, so the descriptor slot kept whatever bytes
were there before. The first draw that sampled the A-300 atlas read that
garbage descriptor and the GPU faulted; the next submit reported
device-removed 0x887A0001. That also explains why the failure is item-specific:
A-300 is the first preview to carry a BC7 atlas (1024x1024, 11 mips). Other
previews use BC1/BC2/BC3/BC4/BC5, whose concrete formats were already correct.

Changes:

- `MapTexFormat` now returns **98 (BC7_UNORM)** for 97, 98 and 99, matching the
  existing linear-sampling policy for sRGB variants.
- The 0.8.325 base-mip-only upload is reverted: the full chain is uploaded
  again, since the mip copies were never implicated.
- `tools/ba2_audit/ba2lib.py` and
  `tools/ba2_audit/test_ba2_v1_archives.py` follow the same mapping, and the
  version-declaration gate is updated.

The `previewdebug.ini` bisect switches (`render`, `upload`, `draw`) stay in
`UploadGPU`. They are read once from the DLL folder, default to 1, and are
useful for the next renderer fault; the deployed file is now all-ones.

Build 0.8.327, SHA-256
`b00edc7a064391ab5adb660556445ce491df65d30dbc5dbc95a1fdb47c6fbdc1`, deployed
with the 0.8.326 DLL kept as
`...backup-before-bc7-typeless-20261024-1035-v0.8.326`. 48 tests OK (build
gate, overlay input, legacy preview, BA2 v1 reader). **Not yet verified in
game.**

## 0.8.328: the Blender-material albedo scan was unreachable

Items that render textured in the engine but drew grey in the preview (Combat
Knife, E-5, Republic General Armor, the Republic backpack) all share one shape
of `.mat`: a **Blender export with no `Summary`/`Albedo` block**. The maps are
named only by `BSMaterial::MRTextureFile` components:

```json
{"Data": {"FileName": "Data\\\\textures\\\\clothes\\\\obiwan\\\\body_color.dds"},
 "Index": 0, "Type": "BSMaterial::MRTextureFile"}
```

`FindMatAlbedo` (src/LiveModel.cpp) already had a component scan for exactly
this case, but an earlier `if (alb == std::string::npos) break;` exited the
do-while before reaching it. So the scan was dead code: the material resolved
nothing, `FindDiffuse` guessed nothing for the mod-only families
(`swweapons`, `bodeknife`, `clonewars`), and the range logged
`UNTEXTURED (no prefetch)` - visible in the 2026-10-24 log for all four items.
The engine was unaffected because the game reads the `.mat` itself.

Changes:

- The Summary/Albedo block is now optional; a missing key falls through to the
  `"FileName"` scan instead of aborting the parse.
- The scan also accepts the generated SGTexSet naming (`*_d.DDS`, `_n`/`_r`/`_l`
  siblings excluded), kept as a **second** choice so a colour-word map always
  wins.
- `tools/mo2_overlay.py` mirrors both rules (it already had the correct
  fall-through, so the C++ was the divergent side), and
  `tools/test_mo2_overlay.py` pins the `_d` case plus a source assertion that
  the fallback is reachable.

Offline verification against the installed mods: `obiwan_body.mat` ->
`textures/clothes/obiwan/body_color.dds`, `obiwan_jetpack.mat` ->
`.../jetpack_color.dds`, `obiwan_gloves.mat` -> `.../basegloves_03_color.dds`,
`obiwan_skirt.mat` -> `.../cloth_color.dds`; `bodeknife.mat` ->
`textures/weapons/bodeknife/color.dds`; `e5.mat` -> `textures/weapons/e5/color.dds`.
The four outfit maps are BC7 in a v1 BTDX archive (`Textures09.ba2`,
2048x2048 / 12 mips, `cloth` also 2048 / 12, `jetpack` 1024 / 11); the two
weapon maps are BC7 DDS files in the GNRL archive `Star Wars Blasters -
Main.ba2`. Both paths are now supported by the 0.8.327 format fix and the v1
reader.

Build 0.8.328, SHA-256
`bf56f6bdb132ed5d469f261b2493bb051421f0eff14e3ddd4681b87a6f1f6b5c`, deployed
with the 0.8.327 DLL kept as
`...backup-before-blender-mat-albedo-20261024-1052-v0.8.327`. 77 tests OK
(build gate, mo2 overlay/precedence, glass/wrap blend, material swaps).
**Not yet verified in game.**


## 0.8.329: the 0.8.328 fallback did not fire in game

0.8.328's parsing change was verified against the real bytes with a throwaway standalone
MSVC harness that copies the parse loop: `e5.mat`, `bodeknife.mat` and
all four `clonewars` materials return their colour map there, and the files
resolve through the same MO2 overlay the plugin uses. The 11:00 run on 0.8.328
still logged `0/1`, `0/4` and `UNTEXTURED (no prefetch)` for the same items, and
**no** non-Summary material has ever produced a `score albedo` prefetch line
(the only three `score albedo` hits - `frag.mat`, `a300.mat`, `jnd41.mat` - all
carry a Summary/Albedo block). So the failure is in the in-game material
lookup, not in the offline parse.

0.8.329 adds diagnostics only (no behaviour change) so the failing stage is
named by the log instead of inferred:

- `offline GPU: prefetch '<key>' (N meshes) mats=[...]` - the material paths
  the prefetch actually sees;
- `live: mat lookup miss '<path>' (archives=N)` - `ResolveAsset` did not return
  the `.mat`;
- `live: mat is not JSON '<path>' (size, first byte)` - the bytes read are not
  a JSON .mat (wrong archive copy, bad decompression, binary format);
- `live: mat names no colour map '<path>'` - parse ran and found no DDS;
- `live: mat albedo '<path>' <- '<dds>'` - successful parse (new, so the log
  shows whether the component fallback ran at all);
- `offline GPU: no colour map for '<mat>' node '<node>'` - the chain reached
  the final `FindDiffuse` failure.

Build 0.8.329, SHA-256
`9d48473bb01816d06bb5d78d63fa3e19956dbf949ae299879c8662efc96dea96`, deployed
with the 0.8.328 DLL kept as
`...backup-before-prematlog-20261024-1113-v0.8.328`. **Awaiting one in-game
reproduction of the four rows.**

## 0.8.330: memo-miss probe added to the same diagnostic build

0.8.329 could still say nothing if `FindMatAlbedo` returned a **memoized**
empty result: an early call (e.g. a material probe before the archive index
was warm) stores the miss for the session, and every later call short-circuits
before any of the new log lines. 0.8.330 adds

- `live: mat memo miss '<path>'` - a cached empty result was returned, with no
  lookup attempted at all.

That distinguishes "the archive lookup failed" from "a poisoned cache entry
from an early call". No behaviour change.

Build 0.8.330, SHA-256
`4300f2fbe6dee7ef62beff550442391245688f8ce9335c630557bb800083259c`, deployed
with the 0.8.329 DLL kept as
`...backup-before-matlog-20261024-1150-v0.8.328`. 48 gate tests OK.
**Awaiting one in-game reproduction of the four rows.**

## 0.8.331: the texture side of the chain is proven; prefetch now reports itself

Offline, the Clone Wars chain is not broken:

- `materials/clonewars/obiwan_body.mat` names
  `Data\textures\clothes\obiwan\body_color.dds`, and the plugin's own component
  scan (the Blender `"FileName": "Data\Textures\..."` rule) turns that into
  `textures/clothes/obiwan/body_color.dds` for all four outfit materials
  (body/gloves/skirt/jetpack) - checked by replaying the exact scan over the
  shipped files.
- Those maps exist: `StarWarsGenesisArmorCompendium - Textures09.ba2` (v1 DX10,
  73 entries) holds `textures\clothes\obiwan\*_color.dds` at 2048x2048,
  DXGI 99 (BC7 UNORM_SRGB) - a format `MapTexFormat` has accepted since 0.8.327.
- The archive is in the session index (`Textures09.ba2 (73 entries)` in the
  11:00 log).

So the flat result is a lookup failing in-game, not missing art. 0.8.331 adds
the last two lines needed to name it:

- `offline GPU: prefetch '<key>' resolved X of N textures` - every prefetch,
  including a total miss (previously a silent 0);
- `offline GPU: prefetch '<key>' skipped (M textures cached)` - a cached
  answer was reused. M=0 is the poisoned case: the first attempt failed and
  the key is never retried for the session.

Build 0.8.331, SHA-256
`bd657cfb33362542a064d6de6131b3173de56ccd35f188ce414a527f0a757f5d`, deployed
with the 0.8.330 DLL kept as
`...backup-before-prefetchlog-20261024-1125-v0.8.330`. **Awaiting one in-game
reproduction.**

## 0.8.332: the tested log was produced by a binary older than the DLL

The 11:57-12:01 session produced no probe lines at all. Every line added
between 11:20 and 11:24 is absent from the log (`prefetch '<key>' (N meshes)`,
`resolved X of N textures`, `skipped (M textures cached)`, `mat lookup miss`,
`mat memo miss`), while code that predates 11:16 is present (`prefetched
'<mat>' <- ... (score ...)`, `texturing ... (N ranges, M prefetched)`,
`UNTEXTURED (no prefetch)`). The DLL in the mod folder is byte-identical to
`build/windows/x64/releasedbg` and does contain every missing format string,
so the file on disk was new while the code running in the process was not:
the session was still executing a previously mapped image.

That also explains the repeated "looks the same" cycles. 0.8.329 was built
five times, so its version line could not distinguish them. 0.8.332 bumps the
version (header and xmake) and adds three lines that fire on paths nothing
can skip:

- `prefetch enter '<key>' (N meshes)` - the first statement of
  PrefetchTextures, before the `alreadyReady` early return;
- `warm request '<key>'` - logged only when QueuePrefetch actually stages a
  key, so a render thread that never asks is visible;
- `serving warm '<key>' cached=0|1` - the task thread stating whether the
  staged key resolved to a model at all. `cached=0` is the silent dead end
  that leaves a preview permanently flat.

The material parser needed no change for these items: the Clone Wars .mats
are Blender exports whose `BSMaterial::MRTextureFile` components carry
`Data\textures\clothes\obiwan\body_color.dds`, which the scan already
normalizes to `textures/clothes/obiwan/body_color.dds`, and that key exists
in `StarWarsGenesisArmorCompendium - Textures09.ba2` (73 entries, DXGI 99 =
BC7_UNORM_SRGB, 2048x2048).

Build 0.8.332, SHA-256
`a64ca70757f44ed534bb181b33bbce08503f7fef6c5418d85324756f77b3bb65`,
deployed with 0.8.329 kept as
`...backup-before-warmtrace-20261024-1210-v0.8.329`. Build gate, overlay
input, legacy preview, BA2 v1 and MO2 overlay tests all pass.

## 0.8.332 correction: the image was new, the LOGGER was dropping lines

The 0.8.332 session disproves the "old image" reading above: the log opens with
`SFShopExplorer v0.8.332.0` (that string comes from xmake's version
configvars and ShopExplorer.h was bumped in the same build), and the deployed
DLL is byte-identical to the freshly linked artefact whose
`OfflineModel.cpp.obj` contains every probe literal.

What actually happens is narrower and worse: the plugin log is an async
spdlog sink, and it drops the first line of a burst. `FindMatAlbedo` logs
`mat albedo '<mat>' <- '<key>'` immediately before returning true; the very
next line, `prefetched ... (score albedo)`, proves that function returned
true, yet the albedo line is absent. Inside `PrefetchTextures` the entry line
and the `resolved X of N` summary are missing while the per-material lines
emitted a few statements later are present. So every diagnostic added with
REX::INFO was being swallowed exactly where it was needed.

0.8.333 therefore writes the prefetch trace straight to
`%TEMP%\SFShopExplorer-prefetch.txt` with `std::ofstream`, next to the REX
lines: function entry, per-material verdict (`ok`, `nocolour`, `unreadable`,
`rejected`, `limit`, `nomat`) and the per-model summary. It is append-only and
survives the session, so a flat preview can be interrogated after the game
closes without a debugger.

Build 0.8.333, SHA-256
`d0b40090c6d6142ad814f0aa2bf8ed64669559ba86993bc6f6f51ae909d5c7fd`,
deployed with 0.8.332 kept as
`...backup-before-filetrace-20261024-1235-v0.8.332`.

## Root cause of "no change": SFSE was loading three plugin instances

`Documents\My Games\Starfield\SFSE\Logs\sfse.txt` from the 12:24 session:

```
checking plugin SFShopExplorer.backup-before-blender-mat-albedo-...v0.8.327.dll
checking plugin SFShopExplorer.backup-before-prematlog-...v0.8.328.dll
checking plugin SFShopExplorer.dll
...
loading plugin "SFShopExplorer"   (three times)
plugin ...v0.8.327.dll loaded correctly (handle 22)
plugin ...v0.8.328.dll loaded correctly (handle 23)
plugin SFShopExplorer.dll loaded correctly (handle 24)
```

SFSE loads every `*.dll` in `Data\SFSE\Plugins`, and two backups were named
with a trailing `.dll`. Both old builds therefore initialised in-process:
three renderers, three `kPostLoad` lines, three audio workers, three archive
indexes. Whichever instance installed its Present/preview hooks first owned
the preview, so the preview kept being drawn by 0.8.327/0.8.328 no matter
what was deployed - and the plugin's own diagnostics were written by a
mixture of instances, which is why new lines appeared inconsistently and why
`%TEMP%\SFShopExplorer-prefetch.txt` (a side effect only 0.8.333 performs)
was never created.

Fix: those two files moved to `SFSE\Plugins\Backups\` and renamed to `.bin`;
the plugins folder now holds exactly one `SFShopExplorer.dll` (0.8.333).

Rule for every future deploy: a backup kept inside `SFSE\Plugins` must NOT end
in `.dll`. Use `SFShopExplorer.dll.backup-before-<label>-<stamp>-v<version>`
(which SFSE ignores) or move it to `Backups\` with a non-.dll extension.

## 0.8.334: grey lightsabers were inline geometry, not a texture miss

With the duplicate-instance problem out of the way the 12:49 session finally
reported the hilt honestly:

```
SFShopExplorer live: no usable geometry for meshes/weapons/lightsaber/
  engravedlightsaber.nif (3 geos, 3 mesh misses, 0 mesh parse fails)
```

Three geometries parsed, zero of the three `.mesh` lookups hit. The NIF is a
Blender export ("Blender Mesh Plugin" in its export-info block) and it names
**no** `geometries/<guid>/<guid>.mesh` entry at all: `grep` for "geometries" in
`EngravedLightsaber.nif` returns nothing, while its BSGeometry blocks weigh
10 KB, 22 KB and 514 KB. Those blocks carry the LOD0 `.mesh` payload *inline*,
right where a Bethesda file writes `u32 pathLen` + the printable factory path:

```
01 | 1140 | 357 | 0x40 | 02 00 00 00 | 74 04 00 00 | cb 00 24 01 ...
flag  nidx   nv    unk    mesh u32 2    nidx again     u16 indices
```

`ParseNifGeos` read the `2` as a path length, swallowed two bytes of the index
stream as a "path", then asked the archives for a geometry that has never
existed - so every lightsaber (both lightsaber packs, 3-18 geos each) fell back
to nothing and the preview drew empty.

Fix, in `LiveModel.cpp`:

* `NifGeo` keeps `inlineMesh`.
* `ParseNifGeos` now validates that the bytes following the length really are a
  printable path before consuming them. Non-printable means the Blender layout:
  the rest of the block is a `.mesh` LOD0 blob and is kept verbatim.
* `BuildSingleNif` parses `inlineMesh` with the existing `ParseMeshLod0`, before
  any archive lookup.

Verified: each block's own header matches its LOD entry (1140/357, 1140/905,
81120/19032 indices/verts) and every blob yields a UV stream inside 0..1, so the
reading is byte-exact rather than plausible. `tools/test_inline_nif_geometry.py`
pins both branches against the real files (the Bethesda `razorback_grip.nif`
still resolves a `guid\guid` factory path).

## 0.8.334 UI: report-as-bug rows and the keyword wall

The ESM shop names every record it could not resolve `"<item> (report this as a
bug)"`, which buried the weapon catalog under placeholder rows. They now have
their own `Hide 'report as a bug' items` toggle (default on), run before the
test/debug filter because those rows carry ordinary names.

The detail panel listed 15+ keywords inline, pushing PARTS below the fold. The
list now lives behind a `Keywords (N)` header that starts closed for each newly
selected form; `SetNextItemOpen(false, ImGuiCond_Always)` runs only when the
form changes, so the user can still open it without it re-closing every frame.

Build 0.8.334, SHA-256
`a72ed245ea66a33e9c42f3c5e3c083f5365720e90e10d88517eee71ea9559465`,
deployed with 0.8.333 kept as
`...backup-before-lightsaber-inline-20261024-1318-v0.8.333`.

## 0.8.335/0.8.336: the freeze is real, and the log now says so

The 13:25 session (0.8.334) left three lines in the log and a doubt:

```
[W] game-audio duck found no settings; shop audio only
[I] world frozen request (UI+0x4B4 floor=1, diag=false)
[I] world resumed (... cleared ...)          <- 2m45s later
```

No `freeze check` line, and the ambience kept playing under the shop. Both had
the same shape: something was looking in the wrong place.

**Audio.** `fAudioVolumeMaster / Music / Voice / Effects` are not game
settings. They are written to `StarfieldPrefs.ini` under `[Audio]` (this
profile: master 1.0, music 0.0, effects 1.0), so they belong to
`INIPrefSettingCollection`; `GameSettingCollection::GetSetting` returns null for
all four, which is exactly what 0.8.334 logged. The duck now walks
`INIPrefSettingCollection` and then `INISettingCollection`, captures the
floats it finds and writes 0 back, restoring the captured values on close. The
resolved key is logged per channel, so a future miss names the channel that
stayed live. The write is in memory only; the plugin never saves the profile,
though a game exit while the shop is open is the one case where the game could
save the ducked values itself. Whether the audio engine re-reads these prefs
live is not proven: the next session's `game audio duck '<key>' ...` lines and
a quiet shop are the test.

**Freeze.** The check was scheduled from the SFSE task (`FrameUpdate`), and that
task is one of the things the freeze stops. The retained counter is why the
task stopped, so the check could never fire - the absence of the line for 2m45s
is itself consistent with a paused world, but it is not proof. 0.8.336 adds
`ShopPause::TickRender()` at the top of `RenderOnPresent`, before the render
mode early-outs: Present keeps running while the world is paused, so it can
re-assert the floor and report. The check now takes two samples, ~1.5s (frame
90) and ~2.5s (frame 150) after the freeze, and logs both `worldDelta` values
plus `moved=`. `freeze check t1 world clock held` means the world clock did not
advance between the samples; `still advancing` means the counter hold is not
what stops this build's world and the address needs revisiting. Both lines also
carry the counter, `isGameMenuPaused`, mode byte and blocking gate.

Sound continuing is not evidence either way: Wwise ignores the pause counter,
which is why the duck is separate from the freeze.

Guarded by `tools/test_shop_pause_freeze.py`: the collections searched, the
Present call site sitting before the early-outs, the two-sample check, the
per-open reset, and the `BuildGate::Verified()` guard ahead of the counter
write.

Build 0.8.336, SHA-256
`9eb952d2255c367b79c144cc8a4758b28da8c93d75977a39933ce1a80367a44b`.

## 0.8.337: the freeze works, the sound comes from another layer

The 14:04 session (0.8.336) settled the pause question:

```
[I] world frozen request (UI+0x4B4 floor=1, diag=false)
[I] freeze check t0 ctr=1 isGameMenuPaused=0x01 worldDelta=0.000000
[I] freeze check t1 world clock held ctr=1 (t0 1) worldDelta=0.000000 (moved=0.000000)
[I] world resumed ...
```

`isGameMenuPaused` is 1, the counter holds at 1 and the world delta does not
advance between the samples: the world is paused. Present-driven `TickRender`
is what made the check reachable; the task tick stops with the world.

Sound, however, is a different layer. Wwise never consults that counter, and
the INI-pref duck still resolved no keys:

```
[W] audio setting 'fAudioVolumeMaster'/'fAudioMasterVolume' not found
```

So neither `INIPrefSettingCollection` nor `INISettingCollection` resolved a
key on this build either, which is why the ambience kept playing. The new
probe line settles whether that is an empty/null collection or a spelling
difference; until then the setting write is only a fallback. Two changes:

* **Real silence.** The shop's WASAPI stream now initialises with its own
  session GUID instead of NULL. That puts it in a Core Audio session separate
  from the game's process default session, so `ShopAudio::SetGameSessionMuted`
  can mute the default session (the game) while the shop keeps playing. Applied
  on the audio worker, which already owns the COM apartment and endpoint, and
  released on shop close (`SFShopExplorer: game audio session muted=1/0`).
* **Diagnosis for the setting path.** On the first mute attempt, one line each
  reports the prefs and INI collection sizes and any keys containing "udio".
  If a future build does expose the keys, the next log names them; the setting
  write stays as a best-effort fallback, not the mechanism.

No console host: `ShopMenu.cpp/.h` are deleted, the menu callback registration
is gone, and closing the shop no longer sends a hide message to the Console
menu. The shop is a standalone overlay, as it has been in practice.

Delivery: the basket used to drop every configured item on the floor and try to
move the world reference back into inventory. The 14:12 purchases show that
move never lands on 1.16.244:

```
[I] [spike] inventory move request code=0x0 ref=0x29a19571c00
[I] [spike] inventory move verify ... playerCount=0->0 expected=1 confirmed=0
[I] destination=pending/floor
```

`attachMods` now defaults to 0: purchases go straight to inventory with a
count check, so nothing is dropped and nothing is lost. Selected OMODs and
rarity rolls are not applied in that mode (the confirmation says so); setting
`[shop] attachMods=1` restores the experimental world route for development.
The mods UI is unchanged.

Probes and log volume were already handled in 0.8.335/0.336 and are visible in
the same log: `logging level=info`, zero `[D]` lines in 2,090 lines / 281 KB
for a nine-minute session (the 11:25 session wrote 35,199 lines / 3.8 MB).

Build 0.8.337, SHA-256
`2adda3dfeced15c8903167293386abe749bcbc4298205da5b369135ca315e959`.
## 0.8.338: rarity reaches inventory through the game's own take path

0.8.337 made the basket safe by skipping OMODs. That was the wrong end of the
problem: a weapon's quality tier, its parts and an armor's legendary effect
only exist as OMODs, and an OMOD only attaches to a world reference. So the
route has to pass through one - add, drop, attach - and the open question was
never the attach, it was the delivery.

The old delivery asked the dropped ref to move itself:

```
[I] [spike] inventory move request code=0x0 ref=0x29a19571c00
[I] [spike] inventory move verify ... playerCount=0->0 expected=1 confirmed=0
[I] destination=pending/floor
```

`RemoveItem` looks for an inventory entry on the SOURCE. A dropped stack is not
a container and has none, so the request returned cleanly having moved nothing -
which is why the item was always at the player's feet. The count check was
telling the truth, the call was not.

**The fix: activate the ref.** `SpikeActivateStage` calls
`TESForm::Activate(OBJ_ACTIVATE_DATA)` with the player as `actionRef` - the
entry the game itself runs when the player presses the take prompt. The ref's
activate handler runs, and the container receives the INSTANCE the ref holds,
so the attached OMODs come with it instead of being lost. No raw address is
involved (it is a vtable call), so this half needs no new verification.

Order and safety:

* activation runs first; the legacy `RemoveItem` request only runs if the
  activation *faults*, because a clean activation may free the ref inside the
  handler and touching it afterwards would be a use-after-free,
* success is the inventory count, not either call's return: `[spike] activate
  pickup`, `[spike] delivery immediate` and the queued `[spike] delivery verify`
  all carry the before/after counts,
* `[shop] attachMods` now defaults to 1. 0 still means "plain inventory add,
  OMODs not applied", and the confirmation line says which ran.

What the next session has to answer: does activation move a loose item on
1.16.244? `activate pickup code=0x0 ... confirmed=1` and `+mods` in the notice
means yes; `activate=0x0` with `destination=pending/floor` means the handler
accepted and moved nothing, and the item is at the player's feet exactly as
before.

Guarded by `tools/test_rarity_delivery.py` (activation before the move, the
move only on a fault, count as ground truth, pending not reported as floor) and
the updated default in `tools/test_armor_rarity.py` and
`tools/test_shop_pause_freeze.py`.

Build 0.8.338, SHA-256
`48b337872ae4bffe090c6a7738d356d94fa380edfbe616ec670095b3d8f5ce37`.
## 0.8.339: the activation was right, the reference was not

The 15:24 session (0.8.338) bought three items and left all three on the floor
as Common. The log says why in four lines:

```
[spike] candidate ptr=0x21adbe85ab8 sig=0x2 hex=...
        53454D5C41544144 6F7463615C534548 ...      <- "DATA\MES" + "HSE\acto"
[spike] mods pre e33=0x0 inst=0x0 seh=0xC0000005
[spike] attach slot='selected' omod=000F437E called=0 av=0xC0000005 gate98=1 formType=0x98 gateType=1 diff=1
[spike] activate pickup code=0x0 playerCount=1->1 expected=2 confirmed=0
```

`Misc::LookupReferenceByHandle` (ID 72986) has an unverified signature, and its
return was accepted by `SpikeValidateRef` on `sig=0x2` - the position-float
signal alone. That pointer is not a reference: its bytes at +0x88 are ASCII
paths. Everything after it ran against a string table, which is why the attach
faulted (`called=0`, AV), why the "mods" dump faulted, and why activation
returned cleanly having done nothing. The real dropped ref was never touched,
so the plain Common item was left where the drop put it.

The fix is the resolution step only:

* **cell walk first.** `cell+0x80` count / `cell+0x88` array is structural, and
  with the pre-drop snapshot it names the entry the drop appended. No unverified
  call participates.
* **every candidate must carry the dropped form as its base.** `TESObjectREFR`
  puts `data` (OBJ_REFR) at +0x80; angle +0x00, location +0x0C, so
  `objectReference` is at **ref+0x98**. That is also the qword the attach
  executor gates on, which makes it a natural identity test. (The first version
  of this check read +0x18, a TESForm field, which rejects every real ref; it
  never shipped.)
* **the handle route is a fallback** and must pass the same test. A candidate
  that does not name the dropped form is logged as `bound=0` and ignored.
* per-pick record dumps are behind `[shop] debugSpike` (default 0); a purchase
  now logs resolution, one line per attach, and the delivery verdict.

Still unproven: the attach has never been called with a real reference. The next
session's `attach ... called=1 av=0x0`, `activate pickup ... confirmed=1` and the
`+mods` notice are the test. If the walk reports `tail idx=... inst=0x...` with
no `count=...->...` growth, the drop's ref is being appended to a different cell
and the position scan (same identity test) is the next thing to read.

Build 0.8.339, SHA-256
`8cf4ff6a62e2c506134058fb284e34c28cf3671906ad3a08b037bead34f6dde8`.

## 0.8.340: the take request now names the stack, so nothing stays on the floor

The 15:59 session (0.8.339) proved the resolution fix and then exposed the last
delivery gap. Every modded purchase found its ref, attached every pick, repaired
the record - and the item still stayed on the ground:

```
[spike] cellwalk ptr=0x2108048ea80 count=741 pre=740 tail idx=740 inst=0x210AB556F00
[spike] attach slot='selected' omod=000191EF called=1 av=0x0 diff=0
[spike] repair seh=0x0 ... stores=10 cnt18=13->23->19
[spike] activate pickup code=0x0 playerCount=1->1 expected=2 confirmed=0
```

`Activate` returns clean and does nothing for a loose item ref, so the modified
instance only reached inventory when the player picked it up by hand.

**Cause.** Build 85's move request was `{object, count, moveTo}`. `RemoveItem`
locates the stack to take on the SOURCE, and a dropped ref's stack is identified
by its instance data and its extra data list - neither of which the request
carried. The lookup therefore matched nothing and the request was a no-op, which
is the "a dropped stack has no inventory entry" conclusion in §6r-4g. A loose ref
does own an inventory list; the request was simply asking for the wrong entry.

**Fix (5a).** `SpikeTakeToInventory` fills the whole request: the base object,
the ref's own instance data (ref+0x98), its extra data list (ref+0xC8 - the one
carrying extra 0x33 and the attached OMODs), `inventoryIndex = -1`, the count,
`kTakeFromActivator`, and the player as `moveTo`. RemoveItem then performs the
same transfer the take prompt does, so the modified instance lands in the
player's inventory in the same frame as the attach - no manual pickup, no
rarity loss.

The request is assembled in a flat 0x40 buffer and handed over as a
`RemoveItemRequest&`: the instance-data field is a one-word smart pointer, and
the ref holds that reference for the duration of the synchronous call, so
nothing increments or releases a refcount and no non-POD local lives across the
`__try` (C2712). It is SEH-guarded like the other stages and logs

```
[spike] take request code=0x0 inst=1 extra=1 out=... playerCount=1->2 expected=2 confirmed=1
```

**Order.** take request -> activate handler (only while the count says the stack
did not move) -> legacy move request (only if activation faulted) -> floor
outcome. The activation can free the ref inside its handler, so it is never
called after a confirmed take.

**Guarded by** `tools/test_rarity_delivery.py`: the two identity reads, the flat
request's field offsets, `-1` index, `moveTo` = player, take-before-activate, and
the count gate on activation.

Build 0.8.340, SHA-256
`a89828e07cb1789585edf02d089f4c733c2349f50e9ece8d7b3788409cc95594`.

## 0.8.341: neither a move request nor activation takes a loose item - PickUpObject does

The 16:18 session (0.8.340) settled the transfer question. The complete take
request ran against the right ref and did nothing:

```
[spike] repair seh=0x0 ... stores=7 cnt18=10->17->14
[spike] take request code=0x0 inst=1 extra=1 out=00000000 playerCount=0->0 expected=1 confirmed=0
[spike] activate pickup code=0x0 playerCount=0->0 expected=1 confirmed=0
```

`inst=1 extra=1` proves the request carried the ref's instance data and extra
list, `code=0x0` proves it did not fault, and `out=00000000` proves `RemoveItem`
found **no list entry to remove**. A loose ref is not a container holding its
own item in an inventory list; it is a ref whose instance data and extra list
*are* the item. That is why every move request since build 85 has been a no-op,
independent of how completely it was filled. `Activate` likewise returns clean
and moves nothing for a loose item - the take prompt's action is not routed
through the ref's activate handler on this build.

**Fix (5a).** `Actor::PickUpObject(ref, count, playSound)` - a commonlib virtual
(`RE/A/Actor.h`, slot 0x15B) - is the call the take prompt performs. It builds
the inventory stack from the ref's instance data + extra list and removes the
ref, so the attached OMODs and the rolled rarity ride along. Being a virtual,
no unlisted address is involved and the BuildGate's raw-address caveat does not
apply to it. It is SEH-guarded like the other stages:

```
[spike] pickup stage code=0x0 playerCount=1->2 expected=2 confirmed=1
```

**Order now.** pickup -> take request -> activate handler -> legacy move
request. Each later stage runs only while the inventory count says the stack did
not move AND `SpikeRefIsBound(dropped, ...)` still holds: a successful pickup
frees the ref, so nothing touches it after the count moves.

Build 0.8.341, SHA-256
`74b21d98ed620cc004aac11671df4c8e6dfb19f9bda524a2bb53970fee5e66b2`.

## 0.8.342: add the instance, retire the ref

The 16:28 session (0.8.341) showed the actor virtual is not callable on this
build:

```
[spike] repair seh=0x0 ... stores=7 cnt18=11->18->14
[spike] pickup stage code=0xC0000005 playerCount=0->0 expected=1 confirmed=0
[spike] take request code=0x0 inst=1 extra=1 out=00000000 playerCount=0->0
[spike] activate pickup code=0x0 playerCount=0->0 expected=1 confirmed=0
```

`Actor::PickUpObject` faults immediately, so the vtable slot commonlib models at
0x15B does not hold that function on 1.16.244. Combined with the 16:18 result
(complete `RemoveItemRequest` -> handle 0; `Activate` -> no-op), all three
engine take entries are now ruled out.

**What works is the add call the plugin already uses.** A dropped stack's
`ExtraDataList` is the ref's own, at ref+0xC8 - the very list the attach wrote
extra 0x33 into - and `AddObjectToContainer` accepts one. 0.8.342 therefore adds
the item with `BSTSmartPointer<ExtraDataList>(ref_dl)`: a refcounted copy, which
is exactly what build 88 got wrong when it shared a raw pointer and then freed
the ref, leaving the inventory item dangling (that was the hang).

The world ref is retired only when the ref's instance data pointer (ref+0x98)
differs from the inventory item's; if the two share one `TBO_InstanceData`,
deleting the ref would free it out from under the inventory entry, so the ref is
kept and logged instead. The log also proves the mods arrived:

```
[spike] instance add code=0x0 refDl=0x... playerCount=0->1 expected=1 confirmed=1 invDl=0x... invInst=0x...
[spike] inventory mods=7 world ref retired code=0x0
```

The take request, activate handler and legacy move remain as fallbacks, each
gated on the count and on `SpikeRefIsBound`.

Build 0.8.342, SHA-256
`3f3152ac25197b5a843681b8c4ada302d5025a07f7c5e33aa28c240ba74e7f59`.

**Verified in game, 16:43 session (0.8.342).** Two purchases, both clean:

```
[spike] instance add code=0x0 refDl=0x184C29AAF50 playerCount=0->1 expected=1 confirmed=1 invDl=0x184C29AAF50 invInst=0x184B61FDAD0
[spike] inventory mods=14 world ref retired code=0x0
[spike] delivery immediate instance=0x0 take=0x0 activate=0x0 move=0x0 playerCount=0->1 confirmed=1
[spike] delivery x1 00023606 destination=inventory ref=0x184c2a0fdc0 attachedAny=1
added x1 00023606 free +mods

[spike] instance add code=0x0 refDl=0x184ABE9A320 playerCount=1->2 expected=2 confirmed=1 invDl=0x184ABE9A320 invInst=0x184B61FD600
[spike] inventory mods=2 world ref retired code=0x0
added x1 00035A48 free +mods
```

`invDl` equals the ref's own list pointer, the mod count reads back from the
inventory entry, no fallback stage ran (`take=0 activate=0 move=0`), and the ref
retired without a crash. The purchased items are modded inventory entries with
their rarity, and nothing is left on the floor. This closes the item-delivery
saga: 0.8.334 through 0.8.341 each removed one wrong assumption, and the last
one was that the take has to go through an engine entry at all.

## A mask is a mask whatever it is compressed as (2026-10-24, build 0.8.348)

Two Aid items came back wrong in one capture: the Alien Genetic Material jar had
the coolant lid's cutout stamped inside it as a black star, and the Erdebrau Pils
can printed the black surround of its label atlas over the whole body. Both are
the same miss, and neither is about a material or a colour.

The renderer accepted an opacity mask only as BC4 (80) or BC3 (77). The Test
profile's texture packs repack both masks as **BC7_UNORM_SRGB (98)**:

```
direct GPU: opacity map rejected .../erdebrau pilsner_decal_opacity.dds (fmt=98 1024x1024 vs 1024x1024)
range 3 '...Isotopic_Coolant_Decals.mat' <- slot 4 fmt=98 2048x2048 ... mask=yes
```

The base game ships them as BC4 (`dxgi=80`, `Starfield - Textures05/09.ba2`); the
Erdebrau file is overridden by *Star Wars - Environmental Immersion Project* (its
atlas is the "Deathstick Premium Pilsener" sheet, BC3) and the jar's lid by
*Starfield HD Overhaul* (2048, BC7, 4 mips). Decoded with texconv, both BC7 masks
are greyscale ink in RGB - R=G=B over 178 and 183 values - with alpha pinned at
254-255. So the coverage channel is **red**, the same as BC4, not BC3's alpha.

What the two failures actually were:

- **Can**: with the mask rejected, `mask=none` and the decal fell to mode 4 with
  `g_opacity` still the white 1x1 texture, so coverage was `min(atlas alpha, 1)`
  and the BC3 atlas' alpha is opaque. The label painted as a solid sheet,
  including the black band and surround the mask exists to cut away.
- **Jar**: the label sleeve (`Isotopic_Coolant_Decals.mat`) names the *lid's*
  mask and no colour. 0.8.315 fixed that by skipping a decal whose albedo is
  another material's cutout, but the test was gated on the decal's own map being
  single-channel - and a BC7 mask is not. The star came back.

Changes, in `src/OfflineModel.cpp` only:

1. `maskFmtOk` accepts 98 beside 80 and 77.
2. `range.opacityRedChannel` is true for 80 or 98.
3. The borrowed-mask skip no longer requires the decal's own format to be
   single-channel: `borrowedMask || (IsSingleChannelFormat && !promotedMask)`.
   `borrowedMask` itself is unchanged - the lender's albedo must still carry
   colour - so the Sparkling Wine's two mask-only prints keep drawing.

The Python audit and contact sheet needed no change: they never filtered on mask
format, which is why the offline tile for `001FF69A` always showed the label with
its cutout and the GPU did not. That tile is the reference for the in-game check.

Tests: `test_repacked_bc7_mask_keeps_its_cutout` (new), and the mask-format
strings in `test_decal_mask_channels.py`, `test_decal_blend_policy.py` and
`test_glass_and_wrap_blend.py` updated with their intent kept. Tools 343 OK.
