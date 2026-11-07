Parcel Fix 1.0.0
================

Clears the parcels that SF Shop Explorer left on the ground in saves made
before 0.9.0. One job, done once per save, then the plugin can go.

Why those parcels exist
-----------------------
Before 0.9.0 a purchase was delivered by dropping a real object at the
player's feet and moving its contents into the inventory. The dropped object
stayed behind. The game makes anything the player can pick up persistent, and a
persistent object is written into the save, so every purchase from those builds
left one parcel in the file. 0.9.0 hides its own drop before the save is
written, so purchases from that build onwards leave nothing - but the objects
already inside an old save are save data, and no later version of the shop can
take them out of there. That is all this plugin does.

What it looks for
-----------------
A leftover has to match all of these, because each one is a property of the
shop's own drop and not of ordinary scenery:

  * the object was created while the game was running (the game gives those
    FFxxxxxx ids; anything a plugin placed keeps its own id);
  * it is persistent, deleted or disabled - in other words, something the save
    would bring back;
  * what it is made of is a type the shop sells: weapon, armour, misc, aid,
    ammo, book or key;
  * it is near the player when the scan runs (iRadius, default 1500 units,
    roughly 20 m).

What it does to them
--------------------
It writes one log line per candidate first - id, base object, name, distance,
and the three flags - and then makes the object non-persistent and disabled: not
drawn, no collision, cannot be activated. It never deletes anything, so nothing
else that holds that object can be left with a stale pointer. Disabling is also
reversible (see Undo).

Objects that are not persistent, deleted or disabled are left alone even if
everything else matches: they are not something a save would resurrect, so they
are not this plugin's business.

Install
-------
1. Install with MO2/Vortex and keep the SFSE folder structure:
     SFSE\Plugins\ParcelFix.dll
     SFSE\Plugins\ParcelFix.ini
2. Launch Starfield through SFSE and load the save you want cleaned.
3. Stand where the parcels are (or anywhere on the same pad - the scan runs
   where you are when the cell loads, and again whenever you enter a new cell),
   wait a second, then save.
4. Check the log: Documents\My Games\Starfield\SFSE\Logs\ParcelFix.log
   It will show, per cell:
     ParcelFix: cell ... - walked N reference(s) within 1500 units, M candidate(s)
     ParcelFix: candidate ref FFxxxxxx base xxxxxxxx WEAP 'name' distance ...
     ParcelFix: M candidate(s) hidden, 0 left as they were.
5. Save, quit, and remove the mod. The save no longer carries those objects.

Want to look before it acts? Set bAct=0 in ParcelFix.ini, load the save, read
the candidate list in the log, then set bAct=1 and load again. Nothing is
changed on the dry run.

Undo
----
The log gives the reference id of everything it hid. In the console:
     FFxxxxxx.enable
re-enables that object where it stands. A hidden object is still in the cell
until the cell resets, so this works during the same session or after a reload
of a save made before the reset.

To stop the plugin without uninstalling it: bEnabled=0 in ParcelFix.ini.

Requirements
------------
Starfield Script Extender and the Address Library for SFSE Plugins. No
signature scanning: this plugin only reads the player's cell and the objects in
it through CommonLibSF's own declarations, so a game update should break it at
worst by refusing to load rather than by crashing.

Notes
-----
* It runs one scan per cell per session, a second and a half after the cell
  becomes the player's, so it sees a fully loaded cell and not a half-built one.
* It only looks in the cell the player is in. Parcels dropped in another cell
  are cleaned the next time you go there.
* Purchases from 0.9.0 onwards never create these objects, so this plugin finds
  nothing in a save that was started after that build.
