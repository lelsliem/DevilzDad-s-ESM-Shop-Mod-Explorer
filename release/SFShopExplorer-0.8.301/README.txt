DevilzDad's ESM Shop + Explorer 0.8.301
=======================================

Release build: 2026-10-18  (x64, releasedbg)
Plugin SHA-256: F43DF0656F9FE05C2071299DC36A632F1F4E05BA390B756D10F26CEE6B67363A
Plugin file:    SFSE\Plugins\SFShopExplorer.dll  (2,081,792 bytes)

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

Notes for this build  (0.8.301)
-------------------------------
  * Books now has its own sub-filters: Slates / Posters / Notes-Reports /
    Logs-Journals / Guides-Books / Misc-Other. Posters sorts ahead of notes
    because the two names overlap ("Wanted Poster Note" is art, not a note).
  * Item art now follows the record's own material swaps for books, notes,
    posters and misc props, not just armor. The preview reads a generated
    SFShopExplorerAssets\models\materialswaps.tsv (6,521 overrides for 1,834
    forms, up from 1,273 / 337). A poster NIF is a blank sheet in the archive -
    the picture only exists as a record swap - so this is what makes different
    posters show different art.
  * Audio pass. Every UI sting was regenerated softer and quieter: real
    fade-ins instead of a step, roughly half the brightness (the click's
    spectral centre drops from 8.2 kHz to 3.5 kHz) and peaks between 0.40 and
    0.62 instead of 0.55-0.95. Runtime defaults are now music 0.45 / SFX 0.55.
  * Both shop music beds were replaced. The elevator theme is a 76 bpm lounge
    bed with no drum kit, the dark themes use a 54 bpm ambient pad with no
    percussion; both are reverbed, loop-wrapped and mastered lower.
  * Addresses this plugin needs that are not in Address Library are still
    checked against the running game build at load. On anything other than
    Starfield 1.16.244 the shop still works, but two features disable
    themselves and say so in the log: the world freeze while the shop is open,
    and the item-mod delivery route in the basket.
  * Still known, and next on the list: a note previews as its authored hardback
    because that is the record's own model; some poster families may still
    repeat if their record swap names a sibling model's material; and purchases
    with mods selected can still be left on the floor if the engine refuses the
    transfer back into inventory. Check the ground before rebuying.
  * The shipped SFShopExplorer.ini is a clean default. If you already have one,
    keep yours; nothing in this build needs a new key.

Settings live in SFSE\Plugins\SFShopExplorer.ini: the open/close key, overlay
scale, preview resolution, audio volumes ([audio] musicVolume / sfxVolume) and
the junk filter ([shop] skipmodels).

Log: Documents\My Games\Starfield\SFSE\Logs\SFShopExplorer.log
