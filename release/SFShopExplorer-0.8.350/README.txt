SF Shop Explorer 0.8.350
========================

Release build: 2026-10-24 (x64, releasedbg)
Plugin SHA-256: 6AE15596A5D369594C6968B671DF5FC6A6BE0D523CC58132AA29231642921B0E
Plugin file:    SFSE\Plugins\SFShopExplorer.dll  (2,260,480 bytes)

Requirements
------------
  * Starfield 1.16.244
  * SFSE
  * Address Library for SFSE Plugins

Run Starfield Shader Cache Reset
(https://www.nexusmods.com/starfield/mods/15632) once before installing this
build. It clears the game's shader caches; skipping it can cause a CTD when this
version first loads. It is a separate tool and is not part of this download.

Install
-------
Install the archive with MO2 or Vortex and keep the folder structure:

    SFSE\Plugins\SFShopExplorer.dll
    SFSE\Plugins\SFShopExplorer.ini
    SFSE\Plugins\Audio\*.wav
    SFSE\Plugins\SFShopExplorerAssets\*
    src\            (source for this build)

To install by hand, copy those folders into Starfield\Data\.

Launch Starfield through SFSE and press Page Up to open and close the shop.

Updating: replace the plugin files and keep SFShopExplorer.ini - themes, unlock
progress, text size and audio settings live there.

Controls
--------
Page Up opens and closes the shop. Arrow keys move the list, Enter adds the
highlighted item, double-click adds too, Esc goes back a page and closes the
shop from the plugin list.

To change the key, edit SFShopExplorer.ini: under [overlay] set toggleKey=33 to
another code. F1-F12 are 112-123, PgDn 34, Home 36, End 35, Insert 45, 1-9 are
49-57.

What changed in this build (0.8.350)
------------------------------------
  * Audio has its own controls. The toolbar's single Music checkbox is now an
    Audio popup with a volume slider and a mute checkbox for music, and the same
    pair for sound effects, plus a track picker. Levels apply live and are saved
    under [audio] musicVolume / sfxVolume / musicMuted / sfxMuted / musicTrack.
  * Music is chosen independently of the visual theme. "(theme track)" keeps
    each theme's own score; picking any other entry overrides it, so a track the
    player drops in is not replaced when they switch themes.
  * One flat Audio\ folder is the whole contract: ui_*.wav (case-insensitive) is
    the six-effect sound bank and is never offered as music; any other .wav in
    Audio\ or Audio\Music\ appears in the track list. No manifest, no rename, no
    second install step - drop a file in and reopen the popup.
  * An ini from an older build keeps its setting: when [audio] musicMuted is
    absent the old [shop] music toggle is read, so an update does not start
    playing over someone who had music off.
  * Misc and Aid sort by the item's model as well as its name. Misc / Other fell
    from 1,417 items to 416, Aid / Other from 58% to 4%, and Sabacc cards went
    from 0 to 108.
  * Lab glass draws as a vessel again, vial caps and liquid are solid and
    correctly coloured, and two texture-pack masks no longer print black.

Earlier builds
--------------
  0.8.349  Bounty boards, ammo primers, Old Earth PCs and Inorg containers moved
           to the right categories (Misc / Other 456 -> 416).
  0.8.348  BC7 opacity masks accepted - Erdebrau Pils label, Alien Genetic
           Material jar.
  0.8.347  Vial liquid is green instead of grey.
  0.8.346  Vial caps, DNA tissue and the Vortex Ichor casing draw as solids.
  0.8.345  Beakers and graduated cylinders preview as vessels.
  0.8.344  Eight new Misc categories; Misc / Other 1,417 -> 456.
  0.8.343  Misc and Aid read the model path; Sabacc cards 0 -> 108.

Notes
-----
  * The game pauses while the shop is open, and its own audio is muted and
    restored when you close it.
  * Known leftovers: three Seed Vault PCs still file under Flora, and packaged
    produce that ships on the xenology mesh still files under Creature Parts.
  * Debugging: [shop] debugSpike stays 0 for normal play; set it to 1 only when
    chasing a specific purchase in the log.
  * src\ is the C++ source for this build (xmake project, MSVC toolchain),
    included for reference and modding.
  * Runtime-sensitive DX12/SFSE plugin. Keep a backup and never replace the DLL
    while Starfield is running.
