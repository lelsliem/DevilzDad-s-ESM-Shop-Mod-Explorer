# Shop Explorer — Shop Rendering Notes

**Session note:** started 2026-09-07; model-path resolution addendum 2026-10-16  
**Purpose:** authoritative handoff for Shop Explorer rendering and asset resolution

> This file is a chronological investigation log, not a single-date snapshot.
> Later corrections and current-state addenda supersede earlier statements; read
> the latest dated entries and source before relying on older section summaries.

> **Trimmed current-state file.** Sections 1–6 and 10–11 are kept here. The full dated build log (sections 7–40+) is in [archive/SHOP-RENDERING-NOTES-full-history.md](archive/SHOP-RENDERING-NOTES-full-history.md).

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

