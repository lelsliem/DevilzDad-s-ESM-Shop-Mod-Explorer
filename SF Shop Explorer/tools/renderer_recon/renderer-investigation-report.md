# Renderer investigation — verified paths and remaining visual gaps

**Date:** 2026-10-11  
**Scope:** source, extracted assets, retained game logs and screenshots. The Razorback badge shader was corrected for dark engraving extraction, exercised with an offline atlas probe, regression-tested, rebuilt and deployed to the MO2 Test profile. The user’s latest post-deployment screenshots, a crop derived from the new Razorback screenshot, and the matching 18:40-18:42 game log now show the dark emblem rendering; it is no longer the near-white disk seen in the older pre-fix capture. Inventory-view parity is encouraging but not a controlled same-camera comparison.

## What “correct” means here

There is no evidence that every game or mod asset renders correctly. Treat correctness per item: identify the model actually selected; resolve its intended materials and maps; confirm the texture reads, formats, channels and blend state; verify GPU submission; and compare the visible image with an inventory/game reference. A successful stage does not prove the next one.

## Confirmed: direct preview pipeline reaches the visible window

The retained 2026-10-10 v0.8.123 session records Razorback geometry extraction (52 meshes, 39,849 triangles, three model paths), task-thread publication, GPU prefetch and Present-thread draw submission. The preview used a 512×384 pipeline, reported 41 textured ranges and bound preview SRV slot 20.

The user-supplied [Razorback direct-preview screenshot](extracted/razorback_live_preview_20261010.png) confirms the direct preview appeared in the popped-out child window: FormID `00000FD6`, 52 meshes, 39,849 triangles and `41/41 textured`, weapon horizontal and barrel left. This is evidence for that item/session, not full material fidelity.

Do not conflate this route with `ItemCapture`: the direct NativeModel preview and inventory-scene capture are distinct. Other visual observations apply only to their named item and view. The latest supplied Razorback and Breach screenshots are discussed below.

## Confirmed: texture reader positive and negative controls

A retained follow-up run warmed 383 BA2 archives and indexed 13,070 opacity maps. Its one-shot reader probe found:

- `textures/weapons/tombstone/tombstone_decals_opacity.dds`: read successfully from `Starfield - Textures11.ba2`, BC4/DXGI format 80, 1024×1024, row pitch 2048, 524,288 bytes.
- A deliberately nonexistent diagnostic key: failed with no successful source and zero format/dimensions/bytes.
- `textures/weapons/razorback/razorback_receiver_opacity.dds`: same empty failure. A separate census found this receiver sibling absent from 25 base-game texture archives; the traversed MO2 loose overrides did not supply it.

This is strong evidence that the tested Razorback receiver miss is a filename-derived path absent from the installed sources, not a general BC4/DX10 decoder failure. It says nothing conclusive about every other rejected opacity guess or whether those materials need masks. The archive census is separate from the incomplete `tools/ba2_audit/extracted/ba2nameindex.tsv`, which should not be treated as authoritative for texture entries.

## Source-verified material and mask behavior

`src/OfflineModel.cpp` selects/prefetches textures on the task thread and submits opaque and translucent draw passes on Present. It binds colour, opacity and optional detail SRVs. Failed reads do not become opacity textures; depending on the mesh/material path, a range may render without a mask or a mesh may be skipped. That is a safe failure mode, not proof of correct appearance.

The source, regression checks and latest live log establish these specific policies:

- Base-game CDB entries precede lazily scanned mod material databases; mod CDBs fill missing answers rather than shadowing known base entries. The live notes record repeated opacity-only lookups after the diffuse memo retained both colour and opacity verdicts.
- A texture-set entry with only opacity can promote the mask to tinted cutout content; this is generic behavior, not a name allowlist.
- BC4 format 80 selects red; BC3 format 77 selects alpha. The HLSL selector and `tools/renderer_recon/test_decal_mask_channels.py` agree. Some readable aspect-ratio-mismatched masks are still rejected.
- A guessed `_opacity.dds` sibling is not evidence that the map exists. The broader 0.8.122 census recorded 508 empty reads among 513 rejected attempts, while the later Razorback-specific run showed 36 empty opacity reads and 12 successful pairs. Duplicate material ranges make these log counts different from distinct-file counts.

## Razorback grip medallion: post-deployment screenshot confirms dark engraving, with parity still open

The earlier pre-fix shop and inventory grip crops establish the original mismatch: a bright, nearly featureless disk versus a dark, relief-like horse medallion. The newest post-deployment capture, documented below, shows the dark emblem after the shader correction.

The source/texture audit narrows the mismatch:

- `Decals_Grip_R1` is an 8-triangle, separately rendered badge face (not simply a patch of the handle's wood surface).
- At its UVs, the standard `decals_color` atlas is plain wood and its `decals_opacity` atlas supplies circular coverage. The Fancy colour crop shows the horse as **dark lines on a light field**; its opacity crop is blank there. The earlier bright-ink extraction used the wrong polarity and selected bright surrounding paper/grime.
- The grip face is the separate 8-triangle badge mesh; its sampled pixels should preserve wood and shade the dark horse linework. The older direct preview showed an almost featureless bright disk; the new post-deployment capture instead shows dark emblem linework.
- **Evidence correction:** the earlier archived composite probe used the wrong polarity and measured the whole crop rather than the mesh's screen footprint and coverage. Its pixel counts and image are superseded by the screen-scale extraction probe below.

**Focused source fix implemented:** keep the standard wood albedo and BC4 circular coverage, and sample the Fancy colour map only for its **dark engraving**. A dedicated badge mode (shader alpha/mode 3) is separate from the wood-emboss mode (mode 2); both select the BC4 red mask channel, while only mode 2 perturbs lighting normals. In badge mode the shader estimates local paper luminance from eight UV-derivative-scaled neighbors and darkens linework rather than compositing the near-white paper. The mask polarity was initially reversed; reading the dark-on-light crop and sampling screen-scale sizes exposed the error, and the shader now computes `neighborhood - detailLuma`. Do not use the blank Fancy opacity crop or invent a receiver mask.

The corrected offline probe downscales the matching authored atlas crop to representative badge footprints and applies the shader's local 8-neighbor dark-line extraction. Pixels above 0.5 response within standard-mask coverage: **8/80 (10.0%) at 16×11, 37/189 (19.6%) at 24×17, 79/332 (23.8%) at 32×23, 173/739 (23.4%) at 48×34, 275/1,334 (20.6%) at 64×46, and 429/3,001 (14.3%) at 96×69**. It wrote a labeled [minification sweep probe](extracted/razorback_badge_minification_probe.png). This is a source-equivalent atlas approximation (bilinear minification and repeat-addressed local neighbors), not a GPU sample of the warped triangles or a game screenshot.

### Latest supplied screenshots and comparison

Saved the newly attached images as [Razorback shop screenshot](extracted/current_shop_screenshot_1.png) and [Breach shop screenshot](extracted/current_shop_screenshot_2.png). The Razorback capture identifies FormID `00000FD6`; status reports 52 meshes, 39,849 triangles, 43/43 textured, yaw −171°, pitch −9°, zoom ×2.57. The Breach screenshot is now the decal regression case (75 meshes, 56,091 triangles, 69/69 textured) paired with the supplied [Breach inventory reference](extracted/breach_inventory_reference_small.png). The [contact sheet](extracted/screenshot_comparison_contact_sheet_small.png) includes both shop captures and retained grip crops.

A crop from the new Razorback capture is [here](extracted/current_shop_screenshot_1_grip_nearest.png), with a larger [side-by-side grip sheet](extracted/razorback_latest_grip_comparison.png). The post-fix capture shows a dark circular emblem/linework on the wood, unlike the [pre-fix shop crop](extracted/current_shop_grip_crop.png)'s white blank medallion; the [inventory crop](extracted/current_inventory_grip_crop.png) also shows a dark horse engraving. This visually confirms that the deployed correction removes the white-disk defect and restores visible dark emblem detail. The inventory image is a different capture with different scale, orientation and lighting, so this is not quantitative or same-view parity; its relief/rim definition remains stronger.

The contemporaneous [SFSE log](C:/Users/mrlma/Documents/My%20Games/Starfield/SFSE/Logs/SFShopExplorer.log) corroborates the post-deploy Razorback route: it loaded `textures/weapons/razorback/fancy/decals_color.dds` as badge artwork, paired `decals_opacity.dds` as BC4/DXGI 80/red at matching 1024², then submitted Razorback's 43 ranges with 52 textures prefetched. Guessed `_opacity.dds` siblings on receiver, cylinder, ammo, sights, barrel and grips remain absent-file misses.

The Breach atlas decoded from `Starfield - Textures10.ba2` is linked as [BC1 decal color](extracted/decals_color_probe.png) and [BC4 coverage mask](extracted/decals_opacity_probe.png). This asset-level probe corroborates the channel diagnosis but is not a rendered GPU screenshot. Verification after the selector fix: the 20-test renderer suite passes and `xmake build SFShopExplorer` succeeds (existing C4244 wchar-to-char warning in `ParseFloat`); in-game confirmation is still pending.

The earlier Razorback shader build was successful and deployed; this Breach channel-selection edit was made after that deployment and still needs a new build and in-game test. The screenshot/log evidence above is for the previous Razorback run only, not this Breach fix.


A separate path check found the two Fancy colour/mask DDS files are present in `Starfield - Textures11.ba2` and decode via the established reader; main-game records report DXGI 72 / BC1 1024² for each colour atlas and DXGI 80 / BC4 1024² for masks. This confirms asset availability, not the final shader appearance.

## Breach decal fix: BC4 coverage was sampled from alpha instead of red

The new Breach shop screenshot shows the assembled weapon geometry but lacks its authored BREACH/ALLIED/CAUTION labels and warning shapes that appear in the supplied inventory reference. The game assets confirm the decal atlas has a BC1 color map (`textures/weapons/breach/decals_color.dds`, DXGI 72) paired with a BC4 coverage mask (`textures/weapons/breach/decals_opacity.dds`, DXGI 80), both 1024² and located in `Starfield - Textures10.ba2`. A decoded crop shows the expected visible print, while the mask carries nonzero coverage only over the atlas's upper-left 259×257 art area. The exact Breach material route and mask read are already present.

The root cause was the selector sent to the alpha-test pixel shader: `range.opacityAlphaChannel` was set true for DXGI 77 (BC3), leaving BC4 (80) in mode 0. But the shader defines mode 0 as `maskTex.a` and every other mode as `maskTex.r`; BC4 component mapping supplies its data in red, so it sampled opaque constant alpha and the transparent decal sheets painted their background over/behind the weapon. The earlier note that “BC4 selects red” was false for the actual draw state despite the HLSL expression looking correct.

**Fix:** track whether a mask is BC4 (`opacityRedChannel = opacityFormat == 80`) so BC4 uses mode 1/red and BC3 remains mode 0/alpha. Detail modes 2 and 3 remain distinct. A focused regression test now ties the exact Breach atlas/mask route to the format-to-selector mapping. The renderer suite now runs 20 tests.

The attached inventory screenshot has a different camera/render path and weapon configuration than the shop sample, but the missing static Breach decals match the channel failure: this change targets that reproducible content gap. Runtime parity awaits rebuilding/rerunning the game preview.

## Breach tactical stock: preview-only no-wood override implemented

The supplied preview shows the Breach Tactical Stock with obvious brown wood grain, unlike the inventory reference's neutral light-grey finish. Asset evidence confirms this is not caused by the Breach decal/opacity path: `meshes/weapons/breach/breach_stock_tactical.nif` is the stock part, and `Breach_Stock_Tactical_Material1` references `Breach_Stock_Tactical_TextureSet1`, whose authored `Stock_Tactical_color.dds` (DXGI 72 / BC1) is itself brown. The shared texture remains untouched.

**Correction:** the direct-preview model range is recognized by the exact normalized material path `materials/weapons/breach/breach_stock_tactical.mat`. Its name check accepts the actual stock node, the same material path in either slash form (the cached `.sfmodel` writer stores material path as its mesh display-name), or an empty name. The range receives flat neutral grey `(0.60, 0.62, 0.64)`, bypasses CDB tint, and remains untextured during GPU texture upload, so its wood albedo cannot appear. The focused regression parses the actual `breach_stock_tactical.sfmodel` cache and verifies that exact name/path representation matches the classifier. The textureless draw explicitly exempts this override from the generic decal-sheet skip, and the CPU fallback applies the same neutral grey. Other Breach materials and uses of the shared stock texture are not recolored. The regression checks cache matching, neutral color, texture bypass, and CPU fallback.


The current user screenshot predates this correction and still shows wood; it is not evidence of the new output. The focused renderer regression set passes (22 tests), including the final cached-model assertion. Since the prior report, the direct renderer test set now passes (24 tests, including a guard that no global stock recolor was added), and the releasedbg build compiles. No live preview capture yet verifies the new DLL output. The user-supplied capture identifies the same rusty/dark/brown patchy look on both Breach and Regulator; this is broader than the Tactical Stock's intended neutral grey and motivated the cross-family lookup investigation below.

## Absolute Control: reproduced file-open failure remains unresolved

The user reports the same error after enabling Absolute Control. The new `AbsoluteControlPanel.log` entry at 06:50:04 again names `B:\\SteamLibrary\\steamapps\\common\\Starfield\\Data\\SFSE\\Plugins\\versionlib-1-16-244-0.bin`. That exact physical file exists, is 5,099,968 bytes, has SHA-256 `299ea1b4da35b42e9bf1b8ed94fa980694a4dcbebfc7693201619c4c08fa49d8`, and an ordinary process can open/read it. The installed Plugins Address Library is metadata-confirmed as Nexus mod 3256 v22 for Starfield 1.16.244.0 and also has the identical file. At the time of the new report the user says Absolute Control was enabled; the latest Test profile file was rewritten shortly afterward and now also shows it enabled with the Plugins package enabled. A fresh USVFS log shows the relevant MO2-launched `sfse_loader.exe` and `Starfield.exe` PIDs were injected successfully; it does not trace the specific Address Library open. Note the saved profile now shows Absolute Control disabled even though the user reports it was enabled for the new launch, so the profile file is not a trustworthy record of that run. The bundled CommonLibSF IDDB source confirms how the error line is emitted and that it is specifically the initial binary stream open.

The DLL is now tied conclusively to the bundled CommonLibSF `REL::IDDB::STREAM::STREAM` loader by embedded source-path and diagnostic strings; the corresponding source is also present under `Depends/commonlibsf-main`. It proves this exact message is emitted only when `std::ifstream(a_path, binary)` reports `!is_open()` in the stream constructor (the message is not the later mmap error). The path is built from the plugin directory and executable version and chosen only if `filesystem::exists` returns true. Therefore a stale shared-memory map is not the cause of this precise line. The user has reproduced it again with Absolute Control enabled while the target file exists at that path, has the expected 5,099,968-byte size and matching package hash, and opens normally outside the game. This narrows it to a difference in the actual game process's C++ file-open path, path view, or access context, despite the same pathname. The on-disk profile and successful USVFS injection do not reveal why `ifstream` fails in that process. No copy, rename, permissions change, profile toggle, or other risky workaround was applied. **Absolute Control remains broken and this change does not claim to fix it.** Nothing here changes Absolute Control's Address Library resolution or file-access integration; a Process Monitor open trace or an instrumented `errno`/`GetLastError` result is still needed to establish the cause. The user has since closed Starfield and the newly built shop DLL was deployed to `Shop Test Render`; source/output/deployed SHA-256 all match at `91e8b8ca9ebd4436b39612e2b097391b499fabda9a3d3b8c0f5c7ae1b43757ce`.



MO2 launch settings configure SFSE with the Starfield runtime directory as the working directory. Both normal and Plugins Address Library package folders contain the same binary but have no conflicting contents for this version. The target plugin package is installed. The currently saved Test profile has the older `Address Library for SFSE` package disabled, `Address Library for SFSE Plugins` enabled, and Absolute Control enabled.


The initial 10-minute `build.cmd` attempt timed out. The later direct `xmake build -y` completed successfully in 4.203 seconds. The post-fix targeted Breach test passed within the 22-test renderer suite. Starfield was closed for the final deployment; the final DLL hash now matches in the build output and `Shop Test Render` MO2 folder.

The attached screenshot shows the previous brown stock, not a result from the neutral override.

---

*Visual validation status: this screenshot predates both the direct no-wood stock override and the new CDB weapon-map family check. The refreshed build is deployed with matching source/output/mod hashes; verify Breach and Regulator using new in-game screenshots. Absolute Control status: unresolved. The plugin's Address Library file-open error is separate from these texture corrections and must be diagnosed with a live-process file-open trace.*

---

## Weapon texture-family regression and current deployment

The latest SFSE log records Regulator's `barrel.mat` resolving to `textures/actors/modela/.../magstorm/barrel_color.dds`; that is a crimson-fleet MagStorm barrel, not the Regulator's authored map. The same preview-path CDB resolver can pick this because the CDB uses bare material stems and its first generic `barrel` set belongs to that other weapon. Regulator grip and decal routes resolve under `textures/weapons/regulator/`, and the Breach stock path has its authored `textures/weapons/breach/stock_tactical_color.dds`. Thus this is an actual wrong-family albedo risk beyond the already targeted Breach stock color, not evidence that all weapons need wood/neutral overrides.

**Initial correction:** before accepting a CDB color for a `materials/weapons/<family>/...` map, check for a distinctive material-family token in the candidate texture path. That first version did not treat weapon-part words like `barrel` as generic, so the Regulator case could still match on the shared part name. It should not be considered a complete fix on its own.

**Follow-up for the multi-weapon appearance report:** the screenshot shows the same patchy/brown rendering on Breach and Regulator, while the retained Regulator log specifically confirms one wrong-family barrel map. The family gate now excludes generic weapon-part tokens (barrel, grip, receiver, stock, sight, etc.) from the match evidence. Thus `materials/weapons/regulator/barrel.mat` no longer accepts a MagStorm `.../magstorm/barrel_color.dds` just because both say “barrel”; it can still accept a real Regulator map via the `regulator` family token. The regression exercises that rejection and acceptance along with Breach's authored stock-map path. The previous build was successful, but the screenshot is from before this follow-up and the current in-game pixels remain unverified; other rendering causes may also contribute to the shared look.

The earlier build's `releasedbg` DLL was deployed to `B:\\MO2\\Starfield\\mods\\Shop Test Render\\SFSE\\Plugins\\SFShopExplorer.dll`; this latest generic-part-token follow-up has been built locally but not deployed or visually verified in-game.

## Other display and material questions

The latest Razorback direct-preview and inventory crops are not a controlled same-camera pair, so they do not establish camera parity or full material fidelity. Remaining unknowns include other mask cutouts, unusual UVs/atlases, CDB tint/substance overrides, arbitrary mod materials, and conventions beyond tested BC3/BC4. A successful model parse, texture read, or GPU submit remains a narrower fact than correct pixels.

For a high-confidence item result, capture the selected form/model and per-range material/map keys, source/decode/format/channel, draw state, and preview plus comparable inventory screenshots. Broaden to representative opaque, translucent, opacity-only and mod assets before making any cross-item claim.

## Source and test references

- `src/OfflineModel.cpp` — texture prefetch, mask pairing, Razorback badge/engraving shader modes, GPU submission, and exact Breach Tactical Stock preview-only neutral override.
- `src/ShopExplorer.cpp` / `src/ShopExplorer.h` / `src/ImguiOverlay.cpp` / `src/ImguiOverlay.h` — persisted toolbar Small/Medium/Large body-text selector and atlas faces.
- `src/LiveModel.cpp` — BA2/loose texture reads, CDB resolution and live texture probe.
- `tools/renderer_recon/test_decal_mask_channels.py` — BC3-alpha / BC4-red channel selection and dedicated shader mode mapping.
- `tools/renderer_recon/test_decal_blend_policy.py` — Breach decal/stock renderer regressions, authored stock map inventory, and guard against generalized stock recoloring.
- `tools/ba2_audit/test_cdb_material_sets.py` — shipped CDB texture-set ownership and CDB weapon-family mismatch regression.
- `tools/renderer_recon/extracted/breach_stock_tactical_color_probe.png` — decoded Breach Tactical Stock base-colour map (shows the authored brown finish).
- `tools/renderer_recon/extracted/breach_stock_tactical_metal_probe.png`, [roughness](extracted/breach_stock_tactical_rough_probe.png), and [inventory reference](extracted/breach_inventory_reference_small.png).
- `tools/renderer_recon/test_razorback_badge_composite.py` — minification-aware offline dark-linework extraction and probe image generation.
- `tools/renderer_recon/test_live_texture_probe.py` — positive, absent and Razorback-derived reader probe coverage.
- `SHOP-RENDERING-NOTES.md`, §§6r-10–6r-12 — CDB memo evidence, reader probe and preview history.

*Prior Razorback correction: previously built/deployed and visually checked in-game. Breach decal-channel correction: source-level regression passes and was deployed; the latest supplied screenshot predates the neutral-stock build and still shows missing labels, so visual decal parity remains unconfirmed. Breach Tactical Stock now has a preview-scoped neutral-grey/untextured override, but the new color has not yet been visually checked in-game. The historic Absolute Control file-open failure awaits a fresh launch with the intended dependencies active; the current Test profile list has Absolute Control and Address Library for SFSE disabled, though the physical library file is currently present and readable.*

The earlier comparison images are [pre-fix shop grip crop](extracted/current_shop_grip_crop.png) and [inventory grip crop](extracted/current_inventory_grip_crop.png); the latest [post-deploy shop grip crop](extracted/current_shop_screenshot_1_grip_nearest.png), [full Razorback capture](extracted/current_shop_screenshot_1.png), [Breach capture](extracted/current_shop_screenshot_2.png) and [comparison contact sheet](extracted/screenshot_comparison_contact_sheet_small.png) are documented above.

## Coverage and next evidence

The evidence confirms one direct preview reached the user-visible window and the texture reader passes a known BC4 positive control while correctly missing the tested absent sibling. It does **not** support “everything renders correctly.” Still open: controlled inventory/direct-preview parity, other mask cutout appearance, other guessed opacity paths, arbitrary mod materials, unusual UV/atlas layouts, all tint/substance overrides, and any channel convention beyond the tested BC3/BC4 paths. Prebuilt `.sfmodel` caches can also omit node names, reducing mesh-level material disambiguation.

For a 100%-confidence claim about an individual item, capture its selected form/model, per-range material and resolved map keys, archive/source and decode results, format/dimensions/channel, draw state, and preview plus inventory screenshots from comparable views. Test representative opaque, translucent, opacity-only and modded assets; broaden to an exhaustive matrix only if the user needs an “everything” claim. The current evidence is bounded and useful, but it is not that exhaustive verification.

## Source and test references

- `src/OfflineModel.cpp` — texture prefetch, mask pairing, Razorback badge/engraving shader modes, and GPU draw submission.
- `src/LiveModel.cpp` — BA2/loose texture reads, CDB resolution and live texture probe.
- `tools/renderer_recon/test_decal_mask_channels.py` — BC3-alpha / BC4-red source/HLSL assertions.
- `tools/renderer_recon/test_live_texture_probe.py` — positive, absent and Razorback-derived reader probe coverage.
- `SHOP-RENDERING-NOTES.md`, §§6r-10–6r-12 — CDB memo evidence, opacity counts, follow-up archive probe and screenshot record.


## Follow-up: weapon preview map audit and material inspector

### Current Breach screenshot and the separate Extra Large selector issue (2026-10-13)

The newest supplied screenshot is a full shop window with the text-size combo open; it is not a close crop of the Breach. It shows the menu offering only Small, Medium, and Large. The current source adds a fourth `Extra Large` item and clamps/persists the `[shop] textSize` value through 3, but font selection happens only when the plugin is compiled into the process. The live SFSE log for this screenshot reports `UI font loaded at 16.0px` but no Extra Large face (the previous log message reports only base/title/caption sizes), and the selected MO2 Test profile enables `Shop Test Render`. The installed `Shop Test Render` DLL is older than the current build output. Therefore the missing menu entry is explained by the in-game instance loading a stale deployed DLL: source support exists, but it was not installed in the profile used for this capture. Restarting Starfield with the updated DLL is required; no deployment was attempted while the game was running.

The in-game Breach log supplies per-mesh draw proof: the exact Breach color atlas and BC4 red mask are resolved and paired, and both Breach `Decals.mat` draw ranges are present in the submitted GPU ranges. However, the running plugin loaded an older build; the currently built source includes the later tiny-decal exemption. The screenshot itself only shows a small thumbnail, not enough pixels to confirm label fidelity. Runtime diagnostics from this capture cannot validate the later source change.

### Breach tiny atlas patch regression correction

The Breach cache has decal-atlas meshes with 4–12 triangles as well as an 8-triangle compact label island. The generic `triCount < 24 || vertCount < 12` prefilter was being applied before range creation and texture setup, so merely pairing the correct BC4 map could not make those geometries appear. The GPU model builder now exempts decal-sheet materials from that generic tiny-geometry drop while leaving unrelated attachment/effect meshes subject to the size filter. This diagnosis is supported by the Breach cached mesh counts and the screenshot-session log: the older plugin did submit some decal ranges, but the current source retains every small decal patch rather than only those that pass the filter. Focused regression covers the exemption. It still needs an in-game screenshot using a freshly built and installed DLL before claiming the weapon looks right.

## Follow-up: weapon preview map audit and material inspector

The supplied 2026-10-13 screenshots show direct previews for Regulator (17 meshes / 17,487 tris), Razorback (52 / 39,849), Breach (75 / 56,091), and Tombstone (18 / 28,605), alongside inventory captures for Razorback, Regulator, Breach, and Tombstone. They establish a repeatable difference in perceived finish: direct previews are much lighter/duller and visibly grainier than the inventory models for the same named weapons. They do not alone identify whether albedo selection, CDB tint/substance data, missing material parameters, texture mip/UV sampling, or lighting is responsible. BA2 metadata confirms the sample weapon albedos carry 11–12 mips split across contiguous primary and lower-mip chunks; the preview was reading and uploading mip 0 only even though its sampler already requests anisotropic filtering and permits the full LOD range. That concrete data-path gap plausibly explains minification shimmer/patchiness at the screenshot sizes, but not the broader material/tone difference. The PBR inventory image is much brighter but more contrasty than the shop image, so simple brightness comparisons are not a controlled diagnosis.

A CDB/texture-archive audit indexed 607 weapon-related sets with authored weapon texture paths. It found 581 fully auditable sets, 26 opacity-only sets (reticles, e-ink UI and similar cutouts; not missing albedo bugs), and no weapon color paths that resolve to a single-channel mask. Eight color/mask pairs have 4x4 BC4 opacity maps. Inspection of all eight yielded the exact same payload `ff00499224499224` (one BC4 block, decoded into 0/145/218/255 values). These are placeholder masks, not authored full-resolution weapon coverage; the Rattler, Breach magazine, Tombstone scope, Microgun and sight examples confirm that a 4x4 block is not meaningful spatial detail for the associated 1024-2048px color map. The prior requirement that masks be at least one-quarter of color resolution was also too strict: the shipped Magshear frame legitimately has a 2048x2048 albedo paired with a 512x512 BC4 mask. The correct data gate is matching aspect ratio, BC3/BC4 mask format, non-empty texture bytes, and at least one 4x4 BC block; the inspector makes the actual map size/status visible rather than overclaiming coverage quality.

The bare CDB key `barrel` resolves to `textures/actors/modela/modela_crimsonfleet_01/modelacrimsonfleet_brute/magstorm/barrel_color.dds` (1024x1024 BC1), an unrelated MagStorm texture; `receiver` resolves to the generic `textures/weapons/awp_shareable/receiver_color.dds` (2048x2048 BC1), and `stock` resolves to Bridger's stock texture. These are concrete lookup hazards if a weapon material has only the bare stem. The existing family guard now rejects these when the material family is identifiable, but each supplied direct screenshot must be checked against its newly shown mesh/material -> map list at runtime; the retained log from the attached capture session has no per-range data, so it cannot prove which of these keys were actually selected for those pixels. The specifically authored Breach stock/receiver maps and Regulator decal/grip maps exist and resolve in the installed texture archives.

**Correction to the mask pairing gate:** the old `bigEnough` quarter-resolution condition rejected legitimate non-degenerate masks. It has been removed; maps below one BC4 block in either dimension are treated as degenerate placeholders, while 4x4+ masks are paired if aspect ratio, format, and data are valid. The CDB audit helper `opacity_pair_issue` now uses the same rule and reports lower-resolution valid pairs separately from invalid format/shape and missing maps.

**Diagnostic added:** weapon details now expose an expandable Resolved weapon materials list, showing each extracted mesh's node/material, resolved color and opacity keys, dimensions/DXGI formats, plus resolver verdict (exact, fuzzy/CDB resolved, flat, unreadable, rejected single-channel, paired, rejected mask, or no mask). Reports are cached alongside the texture prefetch and published for the active model so revisiting a weapon does not rescan textures just to repopulate the panel. The UI is source/test verified and compiled; in-game interactive validation and fresh screenshot comparison remain pending.

**Audit/test/build results:** the full `material_report.py` run covered 42,933 texture sets across 25 installed texture archives (133,175 unique texture names): 36,331 color maps resolve, 5,596 are intentionally flat, 941 are opacity-only cutouts, 65 texture paths are unresolved (mostly Test/LOD assets), and 0 color maps are BC4/BC5 single-channel masks. Follow-up mip-chain decoding verified contiguous 11-mip 1024² weapon maps and a 12-mip 2048² Razorback receiver. The direct renderer now decodes each sequential BA2 chunk through the full contiguous chain, uploads each mip with its D3D12 placed footprint, and exposes the levels in the SRV; the existing one-level loose-DDS path remains compatible and valid loose DDS chains are retained when present. Focused regression tests validate the real weapon BA2 chunk continuity plus the runtime reader/upload path. `python -X utf8 -m unittest tools.renderer_recon.test_live_texture_probe tools.renderer_recon.test_weapon_material_diagnostics tools.renderer_recon.test_decal_mask_channels tools.renderer_recon.test_decal_blend_policy tools.renderer_recon.test_dxgi_color_space_comparison` passes (35 tests), and `xmake build -y` passes. The report’s earlier stock-override paragraph and old captures identify a prior state; fresh in-game validation after restart/build is still required before claiming visual parity.
