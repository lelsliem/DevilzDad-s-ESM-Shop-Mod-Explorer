Parcel Fix 1.0.0
================

What it does

Clears the parcels that SF Shop Explorer left on the ground in saves made before
0.9.0. Every purchase on those builds dropped a real object at your feet, moved
the item into your inventory, and left the object behind; the game wrote it into
the save, so it came back as a green parcel after the next load.

This plugin finds those leftovers in the save you are standing in and clears them
out of the world: each one is moved below the cell, unmarked as persistent, marked
deleted and disabled, and shrunk through the game's own scale call so the box on
screen goes as it runs. Nothing is deleted from your inventory and nothing is
removed from the save's records - every parcel is hidden, not destroyed, so it can
be put back.

It only ever touches objects the game created at runtime, whose base object is a
type the shop sells, within about 20 m of where you stand. Planted scenery, quest
objects and anything a plugin placed with its own form id are never touched.

It is a one-shot tool. Run it once per affected save, then disable or remove it.

How to use it

1. Install the mod, then load the save that has the parcels.
2. Stand near them and wait a few seconds. The scan starts after the loading
   screen closes, and the boxes shrink away as it runs.
3. Check Documents\My Games\Starfield\SFSE\Logs\ParcelFix.log. It ends with:

       ParcelFix: N reference(s) hidden. Save now, then the plugin can be removed.

   Every parcel is listed above that line with its id and name.
4. Save the game.
5. Disable or remove Parcel Fix - see below. It has nothing left to do.

If you would rather see what it would hide before it touches anything, set
bAct=0 in SFSE\Plugins\ParcelFix.ini, load the save once, read the log, then set
it back to 1 and load again.

About your save - read this

* Back up the save first. Copy the .sfs file out of your saves folder before you
  run this, so you have something to fall back to. The plugin writes to references
  inside the save when you save after it has run; that is the point of it, but it
  means a save it has touched is a changed save.
* Do not overwrite your only good save with the result until you have loaded it
  once and seen the parcels gone and everything else normal.
* If a save ever comes back damaged or will not load, restore the backup you made
  and report it with ParcelFix.log attached. The log names every reference it
  changed, which is what makes a bad run recoverable - do not discard it.
* The parcels are only hidden, never deleted, so nothing here can take an item
  with it. Anything hidden can be brought back from the console with its id from
  the log: FFxxxxxx.enable and FFxxxxxx.setscale 1.

Disable or remove it when you are done

Parcel Fix is not meant to stay installed. Once the log says the references are
hidden and you have saved, do one of these:

* Disable it: set bEnabled=0 in SFSE\Plugins\ParcelFix.ini. The plugin loads and
  does nothing.
* Remove it: delete the Parcel Fix folder from your mod manager, or move it out
  of the mods folder. Nothing else depends on it and it leaves nothing behind
  that the game needs.

Either way, remove it before you start a new playthrough. New purchases on 0.9.0
and later never leave a parcel, so there is nothing for this to clean up, and a
scan left installed in a different save can only ever be a risk you do not need.

Settings, in SFSE\Plugins\ParcelFix.ini

    bEnabled=1    set to 0 to disable without removing
    bAct=1        set to 0 for a dry run: logs the list, changes nothing
    bDiagnose=1   also lists every runtime-created reference the scan saw
    bFlagsOnly=0  set to 1 to also require the old persistent/deleted/disabled test
    iRadius=1500  how far from the player to look, in engine units
