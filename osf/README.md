# OSF UI build

This folder is the install tree for the OSF UI page: the INI, audio, and the view itself
(`OSFUI\views\devilzdad.osfshop\shop`). The plugin DLL is built from [`../src`](../src) and is
not committed.

## Build and add the DLL

From the repository root:

```bat
xmake build -y DevilzShopOSF
copy build\windows\x64\releasedbg\DevilzShopOSF.dll osf\SFSE\Plugins\
```

See the [root README](../README.md#building) for the dependency setup and the xmake configure
step.

## Install

Copy the contents of `osf\` into `<Starfield>\Data\` (so this folder's `SFSE\Plugins` lands
under `Data\SFSE\Plugins`), or install `osf\` as a mod in MO2 or Vortex.

Required next to the DLL:

```text
SFSE\Plugins\
├── DevilzShopOSF.dll
├── DevilzShopOSF.ini
├── Audio\
└── OSFUI\views\devilzdad.osfshop\shop\
```

The page must stay in OSF UI's legacy `OSFUI\views` root — that is the root OSF UI registers as
a launcher. Do not add a second copy under `OSF\UI\views`: that root carries a 2.0 manifest
which the legacy loader rejects.

`DevilzShopOSFAssets\models\` is created next to the DLL on first launch and holds this build's
own model index. It is deliberately separate from the ImGui build's `SFShopExplorerAssets`, so
both mods can be installed and enabled side by side.

## Requirements

- Starfield **1.16.244**
- SFSE
- Address Library for SFSE Plugins
- **OSF UI 2.0** (<https://www.nexusmods.com/starfield/mods/17711>)
- **OSF Settings** (<https://www.nexusmods.com/starfield/mods/18326>), required by OSF UI
- Microsoft Edge WebView2 Runtime (ships with Windows 10/11)

Run [Starfield Shader Cache Reset](https://www.nexusmods.com/starfield/mods/15632) once before
the first install to clear stale shader data.

## Open and close

Press <kbd>F10</kbd> → **Mod Settings** → **INTERFACES**, then pick the shop. <kbd>Page&nbsp;Up</kbd>
also opens it through the key OSF UI binds. Escape, the page's close button, or pad B closes it.

## Controls

Mouse and keyboard work as expected: click a row, set the quantity, add to the basket, then
accept. Raw gamepad input is supported too — left stick / d-pad moves the cursor, A clicks, X
adds, LB/RB page, B closes, Start accepts, Back clears.

## Settings

`DevilzShopOSF.ini` sits beside the DLL. It carries the logging switch to the preview size; the
open key belongs to OSF UI, not this file. The page stores theme choice, audio levels and unlock
progress in the same INI.
