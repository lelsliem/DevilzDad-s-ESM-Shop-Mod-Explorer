DevilzDad's ESM Shop + Explorer 0.8.300
=======================================

Release build: 2026-10-18  (x64, releasedbg)
Plugin SHA-256: 6E0F0931077C00F274ACD3151367CE5C3A8D1B8D71E26E4CC11657743E24B9ED
Plugin file:    SFSE\Plugins\SFShopExplorer.dll  (2,080,768 bytes)

Install the archive with MO2 or Vortex and keep the folder structure:

    SFSE\Plugins\SFShopExplorer.dll
    SFSE\Plugins\SFShopExplorer.ini
    SFSE\Plugins\Audio\*.wav
    SFSE\Plugins\SFShopExplorerAssets\*
    src\            (source for this build)

Launch Starfield through SFSE and press Page Up (change [overlay] toggleKey in
the ini) to open the shop.

Requirements
------------
  * Starfield 1.16.244
  * SFSE
  * Address Library for SFSE Plugins

No other plugin dependency, and nothing to install by hand: the shop draws its
own interface, so the older OSF UI / MCM requirements no longer apply to this
build.

Notes for this build
--------------------
  * Version 0.8.300 is the first build after the 0.8.123 upload. Item rendering
    is still work in progress: books, keys and misc items are next, along with
    the filter pass and better music.
  * Addresses this plugin needs that are not in Address Library are now checked
    against the running game build at load. If you are not on Starfield 1.16.244
    the shop still works, but two features disable themselves and say so in the
    log: the world freeze while the shop is open, and the item-mod delivery
    route in the basket. That is deliberate - those addresses were read from
    1.16.244 and would be wrong elsewhere. The log line names both versions.
  * Purchases with mods selected can still leave an item on the floor next to
    you. Check the ground before rebuying.
  * The shipped SFShopExplorer.ini is a clean default. If you already have one,
    keep yours; nothing in this build needs a new key.

Settings live in SFSE\Plugins\SFShopExplorer.ini: the open/close key, overlay
scale, preview resolution, audio volumes and the junk filter ([shop] skipmodels).

Log: Documents\My Games\Starfield\SFSE\Logs\SFShopExplorer.log
