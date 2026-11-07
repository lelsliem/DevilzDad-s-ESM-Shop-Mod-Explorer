DevilzDad's ESM Shop + Explorer - OSF UI build
==============================================

Version 0.9.1-OSF
Plugin SHA-256: 46C66AD3C2D704C8BD1F66EC9ED5A22F530521E1DBAD19648378610BF207426E
(1,804,288 bytes), built 2026-10-31 13:31.
app.js SHA-256: 472CC2F5B0B1795B80B8C4836BA30E55C23699261FC7BD5D72AB858E55F09675
(73,239 bytes), file timestamp 2026-10-31 14:06:30 UTC.
index.html SHA-256: 57460F9671D805A08DF138591C7897173EDE00C586890B370E3E068F1B0A503E
(9,832 bytes), file timestamp 2026-10-31 14:05:33 UTC.
shop.css SHA-256: 081EE0536B9FD7AED1A1136495A3A782DA189322E1514BCC773C9C8D7F8CA085
(21,286 bytes), file timestamp 2026-10-31 09:55:48 UTC.

Source and releases: https://github.com/lelsliem/DevilzDad-s-ESM-Shop-Mod-Explorer

This download contains this build only. The 0.9.0-OSF download also shipped
SFShopExplorer.dll, so installing it loaded the standalone overlay as well:
two catalogs, two audio sessions on the game's audio, two bridges. If you
installed 0.9.0-OSF, delete Data\SFSE\Plugins\SFShopExplorer.dll first.

Same catalog engine as the standalone shop, presented inside OSF UI instead of
its own overlay window. Everything the overlay does, this page does - including
the 3D item preview, the Audio popup and all nine themes.

What it is
----------
An in-game shop and mod explorer that reads every ESM, ESP and ESL in your load
order and lets you browse and buy anything that can actually go in a player's
inventory. No console commands, no FormID hunting, no alt-tabbing to xEdit.

This build uses OSF UI's WebView2 interface: the shop is a page, opened from
OSF Settings (Mod Settings) like any other OSF UI menu. It does not hook the
game's Present chain and does not draw an overlay of its own.

What works in this build
------------------------
* Every loaded plugin, with ESM / ESP / ESL badges, or All Plugins at once.
* Weapons, Armor, Ammo, Misc, Aid, Books and Keys, each with its own
  sub-shelves - weapon class, armour slot, misc type and so on. The same
  classifiers the standalone shop uses decide every row.
* Favourites: the star at the left of any row marks it, and the Favourites
  toggle in the shop bar lists every starred record across all plugins and
  categories. The list is kept in SFShopExplorerFavourites.txt beside the
  plugin - the same file name, format and directory the standalone build uses,
  so a reinstall that keeps the INI keeps the stars, and copying that one file
  carries them to the other build.
* The five Hide toggles: test/debug records, unnamed "report this as a bug"
  forms, ships/hair, crew/enemy/creature leftovers, and turrets with their
  dummy ammo. They live behind the Hide button in the shop bar.
* Search over item names, editor IDs, model file names and FormIDs for the
  whole catalog, not just the current page.
* 3D preview of the selected item, drawn from its own model and materials by
  the same renderer the overlay uses. Drag to rotate (frames stream while you
  drag, they do not wait for you to let go), wheel to zoom, double-click to
  reset. The view is handed to the page as a PNG, so it works in the WebView
  without a second renderer. Pop out lifts it into a floating window you can
  move and resize, and Dock puts it back in the panel.
* A sorter on the column headings. Click Name, Shelf, Type, Value, Weight or
  FormID and the whole catalog is ordered that way - not just the hundred rows
  on screen - with an arrow in the heading; click the active one again to
  reverse it. The first click on a column sorts ascending, exactly as the
  standalone build's headers do, and the arrow is drawn into the label so the
  column keeps its width in every state.
* Item detail: name, FormID, editor ID, type, shelf, plugin, value, weight,
  keywords and model path.
* Weapon parts and rarity, and armour rarity: pick the receiver, barrel, optic,
  magazine, muzzle, grip and so on, the quality tier, and Rare / Epic /
  Legendary. What you configure is what the basket line carries.
* A real basket: up to 25 lines, any quantity of each, adjust or remove lines,
  then Accept. Free mode ignores credits; with it off the total comes out of
  your real balance, shown live at the top right.
* Audio popup: music and sound each have a volume slider and a mute, plus a
  track picker. The track is independent of the theme, and your own .wav files
  work - drop one in Audio\ and pick it (anything starting with ui_ is a sound
  effect, not a track). Settings are stored in DevilzShopOSF.ini.
* All nine themes - six free, three earned by shopping - with the overlay's own
  palettes. Three of them also swap the page's backdrop art, and the theme runs
  through the whole shop: the panes are glass, so the art sits behind every
  panel instead of stopping at the edges of the window.
* The world pauses while the page is open (the view asks OSF UI for it).
* Mouse, keyboard (arrows walk the page, Enter adds) and controller: raw
  gamepad input, left stick / d-pad moves a cursor, A clicks, X adds, LB/RB
  page, B closes, Start accepts, Back clears.

Build note (rebuilt 2026-10-26 21:12, sha256 a87c7806...)
---------------------------------------------------------
The overlay and this package were not the same binary again for part of the
day: `OSF DevilzDad's ESM Shop` still held the 20:32 DLL, which is the one
without the UTF-8 fix below, and a pre-fix `app.js`. Both are now the bytes in
this package (verified by hash), so replace the DLL *and* `app.js` when
updating. Added on top of the 20:45 build: the page's sort column is written to
`[shop] osfSortColumn` / `osfSortAscending` and handed back with the catalog,
so reopening the shop returns to the column and direction last clicked - the
overlay has persisted its own `[shop] sortColumn` since 0.9.0.

Two fixes on top of the 20:10 sorter build:

  * NAME and TYPE sorting said "Catalog query failed: legacy handler returned
    invalid JSON" and left the list as it was. The reply was not valid UTF-8:
    Starfield's string tables hand back Windows-1252, so a name with an accent
    arrives as one 0xE9 rather than the two-byte UTF-8 form, and the JSON
    writer copied those bytes straight through. Only those two columns failed
    because only they put such a record on the first page - an "Action Figure:
    ... Francois ..." under NAME and the Aid shelf's "Cafe et the Prime" under
    TYPE. Every text field of every reply is now converted to UTF-8 first (and
    still escaped afterwards), which also covers names in the item detail, the
    basket and the plugin list.
  * Double-clicking a row did not add to the basket. Selecting a row rebuilt
    every row's markup, so the second click of a double click landed on a new
    node and the browser never fired a double click at all. Selection now
    repaints the highlight in place - the same reason the star buttons repaint
    rather than rebuild - which fixes the double click and keeps the list's
    scroll position.

The package also carries the standalone overlay binary from the same sources -
`SFShopExplorer.dll`, 2026-10-26 21:29, SHA-256 4C360299... - so enabling either
folder gives the same engine; its own behaviour is unchanged from 18:40.

Replace DevilzShopOSF.dll and app.js in OSFUI\views\devilzdad.osfshop\shop\;
index.html and shop.css are unchanged from the 20:10 build.

Build note (rebuilt 2026-10-26 20:10)
-----------------------------------
Adds the column sorter described above. The sort runs in the plugin, not the
page: the page only ever holds one window of a hundred rows, so ordering there
would leave page 2 unsorted, and the order is cached until the column, the
direction or the item list changes. Replace DevilzShopOSF.dll and the three
view files (app.js, index.html, shop.css) in OSFUI\; the INI, the audio and
the assets are unchanged.

Build note (rebuilt 2026-10-26 19:47)
-----------------------------------
This package was rebuilt from the same sources as the 0.9.0 overlay build, so
the two now carry the same code. The earlier OSF binary here was 3.5 hours
older and did not include the save/load fix: a purchased item's drop reference
is now made non-persistent and disabled once the item is in the inventory, and a
second pass re-applies that after the engine's own end-of-frame cell update.
Without it, a purchase can come back as a parcel on the floor after a reload.
If you are updating from the previous download, replace DevilzShopOSF.dll; the
views in OSFUI\ are unchanged (verified byte-identical).

What changed in this build (0.9.1-OSF)
------------------------------------
  0.9.0-OSF was pulled after crash reports. 0.9.1-OSF is the same engine with
  every defect found since fixed, and one packaging mistake corrected:

  * Sounds work. Click, hover, select and the refusal click were posted on the
    command transport while the audio endpoint is a request handler, so every
    one was dropped at the bridge with no error on either side. They are now
    sent as requests. The open and close stings were never affected - the
    plugin plays those itself - which is why only they could be heard.

  * Taking a line out of the basket has its own effect. Remove, clear and the
    page's data-act="rm" choose ui_remove.wav (a seventh file in Audio\) while
    add and accept keep the select sting. An Audio\ copied from an older build
    has no ui_remove.wav, so the bank borrows the click rather than falling
    silent for a sound the player never knew existed.

  * Closing the page stops the music. The view-hidden path now closes the
    session, unmutes the game and lets the track end instead of leaving it
    playing over the pause menu. Muting silences the effects as well as the
    music, from the page's Audio popup or from the INI.

  * Asset discovery no longer reads your mod manager. The plugin used to parse
    ModOrganizer.ini, guess the active profile, read its modlist.txt and walk
    the manager's mods folder, with B:\MO2\Starfield\mods hardcoded as a
    fallback. It now reads the Data directory the game is running. Under MO2
    or Vortex the VFS has already merged and priority-resolved that tree, so
    one file exists per path and it is the one the game loads. Nothing in your
    profile or mod list is opened, and nothing of yours is written to.

  * Removing a basket line has its own sound (ui_remove.wav, requested as
    'remove'), instead of the same click every button makes. An Audio\ folder
    from an older build falls back to the click.

  * Opening the shop counts as a visit every time. The OSF build counted
    at most one open per five minutes, on the belief that the page could
    ask for a session more often than a player opens it - but the session
    only starts on a real shown/hidden transition, so the throttle could
    only ever undercount. Open it six times in five minutes and the
    Starfield theme sat at "1 / 8 opens" no matter what you did.

  * The log is small again. The per-item preview lines - prefetch, texturing,
    per-range verdicts, the model build, the material lookups - are DEBUG and
    sit behind [logging] verbose=1, which also gates the DRED breadcrumbs and
    the %TEMP% prefetch trace. A browsing session writes a few dozen lines.
    If you report a preview problem, set verbose=1, reproduce, and send the
    log: that is the version with the detail in it.

  * This download carries DevilzShopOSF.dll only. The overlay build is a
    separate download; install one or the other, never both.

  Unchanged: the page still opens from OSF Settings (Mod Settings) or the OSF
  key you bound to it (Page Up by default), and Escape, pad B or the page's
  close button closes it.

  If 0.9.1-OSF still crashes for you at load, post
  Documents\My Games\Starfield\SFSE\Logs\sfse.txt with
  DevilzShopOSF.log. sfse.txt names the game build and every plugin SFSE
  loaded, which is what a crash on load has to be read from.
What changed in 0.9.0-OSF (withdrawn - superseded by 0.9.1-OSF above)
------------------------------------
  * The first search keystroke after changing category or plugin no longer
    stalls. It took 450 ms because the search key resolved a model path through
    FormModelPath, and for each of the 202 dummy-base weapons in the catalog
    that runs the whole OMOD assembly scan. The key now reads the model path
    the record declares, so the build is a string pass over the rows.
  * Selecting a plugin shows that plugin's records. Light (ESL-flagged) and
    medium files were looked up in each other's tables: 0xFE indexes the light
    table and 0xFD the medium one, and with the two crossed every light plugin
    resolved to no owner - All Plugins listed all 3,284 armors while
    AmaraArmor.esm showed 0 - and a medium plugin showed whichever light file
    sat at its index. AmaraArmor.esm's armors are FE06Exxx, slot 0x06E, well
    past the 13-entry medium table.
  * NAME sorting no longer re-folds the catalog on every click. The key was
    rebuilt for all 9,161 rows per header click - 4.4-5.0 ms, against 0.5-1.1
    ms for the numeric columns - and is now cached until the list itself is
    rebuilt. The item search keys are gated on the same counter, so sorting a
    filtered list is no longer a 9,161-key rebuild either. The rebuild logs its
    two halves: "list phases: gather X.XX ms, filter X.XX ms".
  * A NAME sort reads the name, not the bytes hiding in front of it. Five
    records in the catalog carry an invisible prefix: a leading space
    (" Golden Glow") or U+F225, the game's food/drink icon glyph no UI face can
    draw ("\uF225 Hydrated", "\uF225 Fed"). Sorted as bytes those rows rose
    above the whole list - the two marked Aid records at the top of a
    descending sort, " Golden Glow" at the top of an ascending one - which is
    the scramble you saw after clicking the header. The padding is trimmed from
    the name the shop shows and sorts on, and the key starts at the first
    character that can begin a word, so '"The Great Serpent" Lie' files under T.
  * Favourites. A star column on every row, and a Favourites toggle that lists
    the starred records from every plugin and category. The list lives in
    SFShopExplorerFavourites.txt beside the plugin - one FormID per line, the
    file the standalone build reads and writes too. The star is an SVG shape,
    not a character: none of Windows' UI fonts carries U+2605, which is why an
    earlier pass drew the notdef diamond on every row.
  * The five Hide filters moved from the shop bar into a Hide popup, and the
    Favourites toggle took their place.
  * The basket carries weight. Each line reads "Nc - W.WW wt" and the total
    reads "N credits - W.WW wt" (weight alone while Free is ticked), so a
    load-out that would not fit in the ship's cargo no longer looks free. The
    weight comes from the item's own record, multiplied by the line's count.
  * Weapons that copy a vanilla gun no longer preview blank. 202 records in a
    full load order ship a placeholder model instead of a mesh -
    BullpupDummyReceiver for firearms, MeleeDummyHousing for melee. The base
    game's own ones assemble from their mod parts, but a plugin that copies one
    without any model parts of its own ("MagPulse" -> MagPulse Fanisha,
    "Razorback" -> Razorback Trophy, Avarice, Custom Old Earth Revolver,
    Arboron Novastrike Sniper) resolved nothing and drew an empty preview.
    When a weapon owns no model part at all, the preview now assembles the
    vanilla stock parts its own mod-association keyword binds it to, and the
    log says so. A plugin with its own art is never repainted vanilla.
  * The item list sorts in about a millisecond on the overlay side of the
    engine: one key per row instead of a display name per comparison (9,154
    rows in 0.87 ms, logged as "view order ..."). This page shows the list in
    catalog order, so nothing changes here beyond the speed of the list the
    plugin builds for it.
  * No change to purchase, delivery, filters, pause or audio.

What changed in 0.8.350-OSF
---------------------------------------
  * The preview can be popped out. "Pop out" in the ITEM header lifts the
    preview into a floating window; drag its title bar to move it, the corner
    grip to resize it, and click the x (or Dock) to put it back. It is the same
    preview, not a copy, so the camera, the zoom and the frame on screen come
    along with it.
  * The preview rotates continuously. A drag now asks for a new frame on every
    mouse sample instead of one frame after you let go, so the item turns under
    the cursor. The plugin still renders one frame at a time - a sample that
    arrives mid-render is folded into the newest camera instead of queueing.
  * Drag works on the input path OSF UI actually forwards. The page accepts
    pointer events or plain mouse events for a press, whichever arrives, and
    keeps the other from doubling the rotation. Releasing outside the panel
    ends the drag instead of leaving the camera following the cursor.
  * The theme covers the whole shop. Panes, the header bar, the toolbar, the
    item preview and the popups are translucent with a light blur, so a theme's
    backdrop art reads behind every panel rather than only around the window.
  * Fixed: "Select an item." was painted over a delivered preview frame. The
    empty-state label is now properly hidden once a frame arrives.

Requirements
------------
* Starfield 1.16.244
* SFSE
* Address Library for SFSE Plugins
* OSF UI 2.0 (with OSF Settings, which OSF UI requires)
* Microsoft Edge WebView2 Evergreen Runtime (already on most Windows 10/11)

Install
-------
Install with MO2 or Vortex, or copy this folder's contents into
Starfield\Data\SFSE\Plugins:

    DevilzShopOSF.dll
    DevilzShopOSF.ini
    Audio\*.wav
    OSFUI\views\devilzdad.osfshop\shop\...

DevilzShopOSFAssets\models\ is created next to the DLL on the first launch and
holds this build's own model index. It is deliberately a different folder from
the standalone shop's SFShopExplorerAssets, so the two mods can be installed and
enabled side by side without sharing a cache.

The view sits in OSFUI\views (OSF UI's legacy root) on purpose. OSF UI 2.0
reads both roots, but only the legacy one is registered as a launcher, and a
launcher is what gives the shop a card under INTERFACES in Mod Settings. A
second copy in OSF\UI\views is not just redundant - it carries a 2.0
manifest, which the legacy loader rejects with a warning, so ship one copy.

Open it through OSF, the way a view is meant to be opened: press F10 for Mod
Settings, then pick the shop under INTERFACES. Page Up also opens it directly -
that binding is declared to OSF UI by the plugin as a key setting ("openKey",
"PageUp") when it attaches - and it never fires while you are typing in the
page or while another menu is up.

Escape (or the page's close button, or pad B) closes it. The plugin watches
which state OSF actually put the view in and starts or stops its session with
it, so the music, the muted game audio and the shop's own state all close with
the window: one Page Up opens, one Escape closes. If the plugin list looks
stale, press Refresh at the top right.

First preview of an item takes a moment: the model is parsed out of your own
install and rendered once. The rendered frame rides back to the page inside the
reply as a PNG data URL - nothing is written next to the view, because OSF UI
serves a view from a snapshot it takes at startup, so a file saved there would
never load (and, in developer mode, would make OSF UI reload the interface).
After the first render each camera stop is kept in memory for the session (the
newest 48), so going back to an angle you already looked at is instant.

Updating
--------
Keep your existing DevilzShopOSF.ini. Theme choice, unlock progress, text and
audio settings live there and are not overwritten.

Settings
--------
The open key is not read from this file any more. The plugin declares
"openKey" (Page Up) to OSF UI as a key setting when it attaches, so the
binding belongs to OSF; a leftover [overlay] toggleKey from 0.8.349 is
ignored. Open the shop from Mod Settings -> INTERFACES to avoid the key
altogether.

DevilzShopOSF.ini next to the DLL:

    [logging]
    verbose=0

Set verbose=1 for a diagnostic session if a preview or a purchase misbehaves.

Do not add a file under OSF\Settings\schemas\ for this mod. The plugin hands its
settings schema to OSF UI at run time (the same path OSF UI uses for every
view's open key), and a file with the same id is read as a second declaration
that competes with it - an empty one is rejected outright with "group controls
must be an array", which lands in Mod Issues.

    [offlinepreview]
    width=512
    height=512

Preview render size. The page scales whatever comes back, so leave it unless
you want sharper or cheaper frames.

    [audio]
    musicVolume / sfxVolume / musicMuted / sfxMuted / musicTrack

Written by the Audio popup. Music stays muted if it was muted before an update
- untick Mute in the popup.

    [premium]
    opens / spent / free

Unlock progress for the three earned themes. Deleting these lines re-locks
them; the shop is not otherwise affected.

Known issues
------------
* The page waits up to two minutes for the game to finish loading plugins; if
  it opens empty, press Refresh once the save is in.
* The preview is a stream of stills, not a live viewport: each frame is
  rendered on the game thread, encoded to a PNG and handed to the page, so a
  fast drag moves in whole degrees and slows down while a model is being
  parsed. Frames after the first are cached per camera stop (newest 48), so
  returning to an angle you already looked at is instant.
* One line per drag lands in SFSE\Logs\DevilzShopOSF.log ("preview drag: yaw=...
  samples=..."). If a drag produces no line at all, the press never reached the
  page - that is the first thing to check if rotation ever stops working.
* Absolute Control (flight control and menu suite) is unconfirmed on this
  build, as on the standalone one.
* Some records ship no model, or a material Starfield's database leaves empty.
  Those preview blank or neutral rather than borrowing another item's art.
* The three earned themes count shop sessions, at most one per five minutes.
  OSF does not raise a lifecycle event this plugin can subscribe to, so the
  count comes from the page watching OSF's own `views` state; the rate limit
  keeps a re-shown view from farming the earned themes.

Credits
-------
Inspired by perchik71's P71 In Game Shop Mod Explorer.
OSF UI by ozooma10 - this build is a content plugin for it.
Address Library by meh321.

Warning
-------
Still in development. Make a new save before adding items to your inventory
and double-check they are there. If you do not recognise an item, you probably
do not need it. I take no responsibility for issues caused by carelessness.
