DevilzDad's ESM Shop + Explorer - OSF UI build
==============================================

Version 0.8.350-OSF
Release build: 2026-10-25 (x64, releasedbg)
Plugin SHA-256: 2FA5BA6C654C04C6E8B14057C2E6FFBDF50D5067B9F880005BD3EEB936FFD7BB
Plugin file:    SFSE\Plugins\DevilzShopOSF.dll  (1,735,680 bytes)

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
* The five Hide toggles: test/debug records, unnamed "report this as a bug"
  forms, ships/hair, crew/enemy/creature leftovers, and turrets with their
  dummy ammo.
* Search over item names, editor IDs, model file names and FormIDs for the
  whole catalog, not just the current page.
* 3D preview of the selected item, drawn from its own model and materials by
  the same renderer the overlay uses. Drag to rotate (frames stream while you
  drag, they do not wait for you to let go), wheel to zoom, double-click to
  reset. The view is handed to the page as a PNG, so it works in the WebView
  without a second renderer. Pop out lifts it into a floating window you can
  move and resize, and Dock puts it back in the panel.
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

What changed in this build (0.8.350-OSF)
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
