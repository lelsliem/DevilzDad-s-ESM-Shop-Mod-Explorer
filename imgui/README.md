# ImGui / DirectX 12 build

This folder is the install tree for the standalone overlay: the INI, theme art, preview material
table and audio. The plugin DLL is built from [`../src`](../src) and is not committed.

## Build and add the DLL

From the repository root:

```bat
xmake build -y SFShopExplorer
copy build\windows\x64\releasedbg\SFShopExplorer.dll imgui\SFSE\Plugins\
```

See the [root README](../README.md#building) for the dependency setup and the xmake configure
step.

## Install

Copy the contents of `imgui\` into `<Starfield>\Data\` (so this folder's `SFSE\Plugins` lands
under `Data\SFSE\Plugins`), or install `imgui\` as a mod in MO2 or Vortex.

Required next to the DLL:

```text
SFSE\Plugins\
├── SFShopExplorer.dll
├── SFShopExplorer.ini
├── SFShopExplorerAssets\
└── Audio\
```

`SFShopExplorerAssets\` holds the themed backdrop and chest art, the logo, the credit icon and
`models\materialswaps.tsv` (the garment material overrides used by the preview). `Audio\` holds
the three theme tracks, the two shop beds and the seven `ui_*.wav` effects. Any other supported
audio file dropped into `Audio\` appears in the track picker.

## Requirements

- Starfield **1.16.244**
- SFSE
- Address Library for SFSE Plugins

Run [Starfield Shader Cache Reset](https://www.nexusmods.com/starfield/mods/15632) once before
the first install to clear stale shader data.

## Controls

| Action | Control |
|---|---|
| Open / close | <kbd>Page&nbsp;Up</kbd> |
| Select a plugin, type or form | Click the row |
| Add an item | Select it, set **Qty**, click **ADD TO BASKET**, then **ACCEPT** |
| Search | Search box, top right |
| Rotate / zoom the preview | Drag / mouse wheel over the viewport |
| Reset the preview camera | Double-click the viewport |
| Pop the preview out | Click the viewport; click outside it to dock it again |

Escape or the title-bar X also closes the shop.

## Settings

`SFShopExplorer.ini` is read from the folder the DLL sits in, not from
`Documents\My Games\Starfield`. Every key is documented inline. The shop writes back the values
it changes (volume, theme, text size, sort column, favourites and unlock progress), so deleting
a line resets it to the documented default.

Do not install this plugin and the OSF build into the same load order unless you intend to run
both; they are separate front ends for the same shop and each opens its own session.
