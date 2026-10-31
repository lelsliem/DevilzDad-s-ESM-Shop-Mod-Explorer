# DevilzDad's ESM Shop Mod Explorer

An in-game shop and mod explorer for **Starfield**. It reads every `ESM`, `ESP` and `ESL` in the
load order and lists every record that can actually go into a player's inventory — weapons,
armour, ammunition, aid, misc items, books and keys — with a 3D preview, a real basket and a
working purchase path. No console commands, no FormID hunting, no xEdit.

One source tree builds **two front ends**:

| Front end | DLL | Interface | Opens with |
|---|---|---|---|
| **ImGui / DirectX 12 overlay** | `SFShopExplorer.dll` | Native Dear ImGui window drawn into the game's own back buffer | <kbd>Page&nbsp;Up</kbd> |
| **OSF UI page** | `DevilzShopOSF.dll` | HTML/CSS/JS view rendered by OSF UI's WebView2 | <kbd>F10</kbd> → Mod Settings → **INTERFACES**, or Page Up |

Both front ends run the same catalog, filters, pricing, basket and purchase code
(`ShopBrowser` in `src/ShopExplorer.cpp`). Only the presentation layer differs.

Source snapshot: **0.9.1** (`SFShopExplorer` / ImGui) and **0.9.1-OSF** (`DevilzShopOSF` / OSF UI).

## What is in this repository

This repository publishes the **C++ sources only**. It intentionally does not contain:

- prebuilt DLLs, INI files or packaged releases;
- audio (`Audio\*.wav`), backdrop art or preview caches;
- the OSF UI view page (`index.html`, `app.js`, `shop.css`, `manifest.json` and its assets);
- the offline Python tooling and test harnesses used during development.

The OSF DLL is only half of the OSF front end — the page itself ships in the OSF release
archive. A build from this repository produces the native plugin; it does not recreate the
web page. The ImGui front end is complete from these sources.

## Source layout

```text
src/
├── main.cpp              SFSE plugin entry point and the per-frame task
├── ShopExplorer.cpp/.h   ShopBrowser: the catalog, filters, prices, basket, transaction,
│                         themes, favourites and the shared state both front ends read
├── ImguiOverlay.cpp/.h   ImGui front end: DXGI Present hook, Win32 input, D3D12 draw
├── OsfFrontend.cpp/.h    OSF front end: attaches to OSF UI and answers the page's requests
├── PreviewHost.cpp/.h    OSF only: renders preview frames offscreen and returns PNG data URLs
├── BackdropArt.cpp/.h    OSF only: player-supplied backdrop images as JPEG data URLs
├── LiveModel.cpp/.h      Runtime NIF / .mesh / BA2 parsers; builds preview geometry from the
│                         player's own install, including mod-added items
├── NativeModel.cpp/.h    Engine-native model loading through TESBoundObject::Clone3D
├── OfflineModel.cpp/.h   D3D12 preview renderer (shaders, PSOs, materials, textures, readback)
├── ItemCapture.cpp/.h    Optional: borrows the engine's own inventory item render target
├── ShopAudio.cpp/.h      WASAPI shared-mode music and effects, decoded from the player's WAVs
├── ShopPause.cpp/.h      Holds the game's UI pause counter while the shop is open
├── BuildGate.cpp/.h      Refuses unlisted engine addresses on builds they were not verified on
├── OmodClassTable.h      GENERATED — quality/rarity OMOD table
├── OmodMnamTable.h       GENERATED — OMOD form ID → M_AMMO / weapon keyword table
├── OSFUI_API.h           Vendored copy of the OSF UI native bridge API (ABI 1.7)
├── PngWrite.h            Minimal PNG encoder used by the OSF preview path
└── PCH.h                 Shared precompiled header
```

The two `Omod*` headers are generated from game data and must not be hand-edited.

## Dependencies

### Build dependencies

| Dependency | Link | Notes |
|---|---|---|
| **CommonLibSF** | <https://github.com/libxse/commonlibsf> | **The project has moved.** The old `Starfield-Reverse-Engineering/CommonLibSF` repository is archived; `libxse/commonlibsf` is the maintained home. Clone with `--recurse-submodules`. Verified with commit `9e8333a` (2026-08-23). |
| **Dear ImGui** | <https://github.com/ocornut/imgui> | The tree was built against **1.92.6 WIP** (`IMGUI_VERSION_NUM 19256`), including `backends/imgui_impl_dx12.cpp` and `backends/imgui_impl_win32.cpp`. Use the 1.92.x line; do not jump to 1.93+ without checking the API and the two backends. |
| **xmake** | <https://xmake.io> | 3.0.0 or newer. The project is configured for MSVC. |
| **Visual Studio 2022** | <https://visualstudio.microsoft.com/vs/> | MSVC v143, Windows 10/11 SDK, C++23. The D3D12, DXGI, Media Foundation and WASAPI headers/libs come from the Windows SDK. |
| **zlib** | <https://github.com/madler/zlib> | Fetched by xmake (`add_requires("zlib")`). Used by the PNG encoder. |
| **lz4** | <https://github.com/lz4/lz4> | Fetched by xmake (`add_requires("lz4")`). Used by the BA2 reader. |

CommonLibSF is licensed GPL-3.0-or-later with its Modding Exception and GPL-3.0 Linking
Exception. Read its `COPYING` and `EXCEPTIONS` before redistributing binaries built from this
source.

### Runtime dependencies

| Dependency | Link | Applies to |
|---|---|---|
| **Starfield 1.16.244** | — | Both builds. `BuildGate.cpp` was verified against `1.16.244.0`; on any other build the world freeze and the modified-item delivery path refuse themselves instead of writing to addresses that may have moved. |
| **SFSE — Starfield Script Extender** | <https://github.com/ianpatt/sfse> · <https://www.nexusmods.com/starfield/mods/106> | Both builds. |
| **Address Library for SFSE Plugins** | <https://www.nexusmods.com/starfield/mods/3256> | Both builds. Both xmake targets set `address_library = true`. |
| **OSF UI 2.0** | <https://www.nexusmods.com/starfield/mods/17711> | OSF build only. |
| **OSF Settings (MCM)** | <https://www.nexusmods.com/starfield/mods/18326> | OSF build only; required by OSF UI. |
| **Microsoft Edge WebView2 Runtime** | <https://developer.microsoft.com/microsoft-edge/webview2/> | OSF build only. Ships with Windows 10/11. |

## Building

### 1. Lay out the tree

Only `src/` is published, so the build script is reproduced at the end of this README. Create
this layout:

```text
DevilzDad-s-ESM-Shop-Mod-Explorer/
├── src/                     ← this repository
├── xmake.lua                ← copy the block from "Build script" below
└── Depends/
    ├── commonlibsf/         ← libxse/commonlibsf, with submodules
    └── imgui/               ← Dear ImGui 1.92.x, with backends/
```

Clone the dependencies:

```bat
git clone https://github.com/lelsliem/DevilzDad-s-ESM-Shop-Mod-Explorer
cd DevilzDad-s-ESM-Shop-Mod-Explorer
mkdir Depends

git clone --recurse-submodules https://github.com/libxse/commonlibsf Depends/commonlibsf
git clone https://github.com/ocornut/imgui Depends/imgui
```

Save the [build script](#build-script) as `xmake.lua` in the repository root.

### 2. Configure once

```bat
xmake f -p windows --toolchain=msvc -m releasedbg -y
```

`releasedbg` is the configuration the project is developed and deployed in: release code with
debug symbols, so a crash dump can be resolved against the PDB.

If xmake ever configures itself for MinGW (a `xmake clean -a` or a fresh checkout can do this),
delete `.xmake` and `build`, then re-run the command above. zlib does not compile in the MinGW
toolchain used here.

### 3. Build the ImGui version

```bat
xmake build -y SFShopExplorer
```

Output:

```text
build\windows\x64\releasedbg\SFShopExplorer.dll
build\windows\x64\releasedbg\SFShopExplorer.pdb
```

### 4. Build the OSF version

```bat
xmake build -y DevilzShopOSF
```

Output:

```text
build\windows\x64\releasedbg\DevilzShopOSF.dll
build\windows\x64\releasedbg\DevilzShopOSF.pdb
```

`DevilzShopOSF` is `set_default(false)`, so a bare `xmake build -y` builds **only** the ImGui
target. Name the target explicitly, as above.

### 5. Check the build actually produced a DLL

xmake can return exit code 0 even when a translation unit failed. Do not trust the exit code
alone — search the build log for compiler and linker diagnostics:

```bat
xmake build -y DevilzShopOSF > "%TEMP%\devilz-build.log" 2>&1
findstr /C:"error C" /C:"error LNK" /C:"fatal error" "%TEMP%\devilz-build.log"
```

If `findstr` prints a match, the DLL was not rebuilt, even if an older one is still sitting in
the output directory. Check the DLL's timestamp and hash before deploying:

```bat
powershell -NoProfile -Command "Get-Item build\windows\x64\releasedbg\DevilzShopOSF.dll | Select-Object Length,LastWriteTime; Get-FileHash build\windows\x64\releasedbg\DevilzShopOSF.dll -Algorithm SHA256"
```

> **Deploy from `releasedbg`, never from a sibling `release` directory.** An old `release`
> output can survive from an earlier configuration and is not updated by these commands.
> Deploying from it ships a stale binary that behaves in game like a change that did nothing.

The instructions above were verified with xmake 3.0.9, MSVC 2022, CommonLibSF `9e8333a` and the
vendored Dear ImGui 1.92.6 WIP; both targets link successfully from a clean tree.

## Deploying

### ImGui build

Copy beside the DLL:

```text
SF Shop Explorer/
└── SFSE/
    └── Plugins/
        ├── SFShopExplorer.dll
        ├── SFShopExplorer.ini
        ├── SFShopExplorerAssets\
        └── Audio\
```

The plugin reads `SFShopExplorer.ini` and `SFShopExplorerAssets\` from **its own directory**, so
they must sit next to the DLL. The asset folder holds the themed backdrop/chest art and the
model index; the `Audio\` folder holds the six `ui_*.wav` stings and any music tracks. Without
the asset folder the shop still opens, but previews and themed art degrade.

Remove or disable other shop DLLs that register the same feature — especially `SFStarfieldMCM.dll`
or `DevilzESMShop.dll` — to avoid two overlays competing for the same key.

### OSF build

Copy beside the DLL:

```text
OSF DevilzDad's ESM Shop/
└── SFSE/
    └── Plugins/
        ├── DevilzShopOSF.dll
        ├── DevilzShopOSF.ini
        ├── DevilzShopOSFAssets\
        ├── Audio\
        └── OSFUI\
            └── views\
                └── devilzdad.osfshop\
                    └── shop\
                        ├── index.html
                        ├── app.js
                        ├── shop.css
                        ├── manifest.json
                        └── assets\
```

The `OSFUI\views\devilzdad.osfshop\shop\` page is **not** part of this repository; it ships in
the OSF release archive. The DLL registers the view and answers its bridge requests, but there
is nothing to render without those files. Keep the view in OSF UI's legacy `OSFUI\views` root
(that is the root OSF UI registers as a launcher) and ship one copy only.

`DevilzShopOSFAssets\models\` is created next to the DLL on first launch. The OSF build uses a
different INI, asset folder and model cache from the ImGui build on purpose, so both mods can be
installed and enabled side by side without sharing or overwriting each other's state.

Open the page through OSF: <kbd>F10</kbd> → **Mod Settings** → **INTERFACES**, or press
<kbd>Page&nbsp;Up</kbd>. Escape, the page's close button, or pad B closes it.

### Controls (ImGui build)

| Action | Control |
|---|---|
| Open / close | <kbd>Page&nbsp;Up</kbd> |
| Select a plugin, type or form | Click the row |
| Add an item | Select it, set **Qty**, click **ADD TO BASKET**, then **ACCEPT** |
| Search | Search box, top right |
| Rotate / zoom the preview | Drag / mouse wheel over the viewport |
| Reset the preview camera | Double-click the viewport |
| Pop the preview out | Click the viewport; click outside it to dock it again |

## How it works, briefly

- **Catalog.** `ShopBrowser` enumerates the game's `TESDataHandler` at runtime, maps every
  ESM/ESP/ESL owner (including light and medium plugins), and filters the result by record type,
  keywords, names and model paths. Nothing is pre-baked from a particular load order.
- **Duplicates.** Records a player cannot tell apart — same type, name, value and weight
  (weight is ignored for ammunition), and a non-zero price — are grouped into one row with a
  `copies` count. The survivor is the earliest plugin's newest record. The **Hide duplicate
  names** toggle collapses the group; the item pane lists the sibling FormIDs.
- **Preview.** `LiveModel` reads the item's NIF out of loose files or BA2 archives with its own
  NIF 20.2.0.7 / `.mesh` parsers, `NativeModel` can borrow the engine's own loader, and
  `OfflineModel` rasterises the result on D3D12. The OSF build renders the same geometry on a
  private device (`PreviewHost`) and hands the page a PNG data URL, because a WebView cannot
  sample a GPU descriptor.
- **Purchase.** Plain items go straight to the inventory with `AddObjectToContainer`. A modded
  item (OMODs, quality tier, armour rarity) has to pass through a world reference: the basket
  drops the stack, attaches the picks, then adds the stack with the reference's own extra-data
  list, so the modified instance is what lands in inventory. The world reference is retired
  immediately — disabled, marked deleted, persistence cleared and parked below the cell — so a
  new purchase leaves nothing on the floor for the save to keep.
- **Pause.** The shop is not a game menu, so `ShopPause` holds the UI pause counter at `UI+0x4B4`
  directly and ducks the game's audio. That offset is not in the Address Library, which is why
  `BuildGate` refuses the feature on any build other than 1.16.244.

## Notes and limitations

- This is a runtime-sensitive renderer and engine hook. Test with a save you can restore, and
  keep the previous plugin disabled while validating a new build.
- Starfield 1.16.244 is the verified build. On other builds the shop still browses and previews,
  but the world freeze and the modified-item delivery path are disabled by `BuildGate`.
- Some records ship no model, or name a material the game's material database leaves empty.
  Those preview blank or neutral rather than borrowing another item's art.
- Saves written before the 0.9.0 purchase fix can still contain parcel references left by older
  builds. Those are already in the save file; `Parcel Fix`, the separate support plugin, exists
  to clean them up.
- The ImGui build implements its own text input translation and key-up handling. If typing
  stops working, confirm `input=1` in `SFShopExplorer.ini` and that no other overlay is eating
  the key events.

## Credits

- Inspired by **perchik71**'s P71 In Game Shop Mod Explorer.
- **OSF UI** by **ozooma10** — the OSF build is a content plugin for it.
- **Address Library** by **meh321**.
- **CommonLibSF** by the libxse contributors; based on **CommonLibSSE** by Ryan-rsm-McKenzie.
- **SFSE** by **Ian Patterson**.
- **Dear ImGui** by **Omar Cornut** and contributors.

## Build script

Save this as `xmake.lua` in the repository root. It is the build definition the sources in
`src/` are written against, with the dependency paths pointed at `Depends/` inside the
repository.

```lua
set_xmakever("3.0.0")

set_project("SFShopExplorer")
set_version("0.9.1")
set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- CommonLibSF (https://github.com/libxse/commonlibsf) has to sit in
-- Depends/commonlibsf; Dear ImGui in Depends/imgui.
includes("Depends/commonlibsf/xmake.lua")

-- force MSVC toolchain
set_toolchains("msvc")

add_requires("zlib", "lz4")

target("SFShopExplorer", function()
    set_kind("shared")
    set_default(true)
    set_symbols("debug")

    add_rules("commonlibsf.plugin", {
        options = {
            address_library = true,
            sig_scanning = true,
            layout_dependent = true,
            no_struct_use = false,
        },
    })

    add_deps("commonlibsf", { public = true })
    add_includedirs("src", "Depends/imgui", "Depends/commonlibsf/lib/commonlib-shared/include")

    add_files("src/ShopExplorer.cpp", "src/ImguiOverlay.cpp", "src/ItemCapture.cpp", "src/ShopAudio.cpp", "src/ShopPause.cpp", "src/BuildGate.cpp", "src/OfflineModel.cpp", "src/LiveModel.cpp", "src/NativeModel.cpp", "src/main.cpp")
    add_files("Depends/imgui/imgui.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_draw.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_tables.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_widgets.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_dx12.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_win32.cpp", { pcxheader = false })
    add_headerfiles("src/**.h")
    set_pcxxheader("src/PCH.h")

    add_links("d3d12", "dxgi", "d3dcompiler", "user32", "ole32", "uuid", "mfplat", "mfreadwrite", "mfuuid", "shlwapi")
    add_packages("zlib", "lz4")
    add_defines("IMGUI_DISABLE_OBSOLETE_FUNCTIONS", "IMGUI_DISABLE_DEMO_WINDOWS")

    set_configvar("COMMONLIB_PLUGIN_NAME", "SFShopExplorer")
    set_configvar("COMMONLIB_PLUGIN_AUTHOR", "lelsliem")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MAJOR", "0")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MINOR", "9")
    set_configvar("COMMONLIB_PLUGIN_VERSION_PATCH", "1")
end)

-- The OSF front end: the same engine and sources, one extra translation unit.
target("DevilzShopOSF", function()
    set_kind("shared")
    set_default(false)
    set_symbols("debug")

    add_rules("commonlibsf.plugin", {
        options = {
            address_library = true,
            sig_scanning = true,
            layout_dependent = true,
            no_struct_use = false,
        },
    })

    add_deps("commonlibsf", { public = true })
    add_includedirs("src", "Depends/imgui", "Depends/commonlibsf/lib/commonlib-shared/include")

    add_files("src/ShopExplorer.cpp", "src/ImguiOverlay.cpp", "src/ItemCapture.cpp", "src/ShopAudio.cpp", "src/ShopPause.cpp", "src/BuildGate.cpp", "src/OfflineModel.cpp", "src/LiveModel.cpp", "src/NativeModel.cpp", "src/OsfFrontend.cpp", "src/PreviewHost.cpp", "src/BackdropArt.cpp", "src/main.cpp")
    add_files("Depends/imgui/imgui.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_draw.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_tables.cpp", { pcxheader = false })
    add_files("Depends/imgui/imgui_widgets.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_dx12.cpp", { pcxheader = false })
    add_files("Depends/imgui/backends/imgui_impl_win32.cpp", { pcxheader = false })
    add_headerfiles("src/**.h")
    set_pcxxheader("src/PCH.h")

    add_links("d3d12", "dxgi", "d3dcompiler", "user32", "ole32", "uuid", "mfplat", "mfreadwrite", "mfuuid", "shlwapi")
    add_packages("zlib", "lz4")
    add_defines("IMGUI_DISABLE_OBSOLETE_FUNCTIONS", "IMGUI_DISABLE_DEMO_WINDOWS", "SFE_OSF_FRONTEND")

    set_configvar("COMMONLIB_PLUGIN_NAME", "SFShopExplorer")
    set_configvar("COMMONLIB_PLUGIN_AUTHOR", "lelsliem")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MAJOR", "0")
    set_configvar("COMMONLIB_PLUGIN_VERSION_MINOR", "9")
    set_configvar("COMMONLIB_PLUGIN_VERSION_PATCH", "1")
end)
```
