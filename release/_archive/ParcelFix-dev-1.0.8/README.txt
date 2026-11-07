Parcel Fix 1.0.8
================

1.0.8 makes the parcels actually leave the screen. 1.0.7 marked them deleted and
disabled, and in game they were still drawn - you could walk around five green
boxes that could not be clicked. That is the clue: the state was being written to
the reference's record, and the record was not what the renderer was drawing.

Why the old hides read as "nothing happened"

A reference's 3D node is built once from the record and then keeps its own world
transform. `data.location` is the value the node was built from, not a handle to
it, so:

    1.0.5   cleared persistence, set disabled      -> drawn, still clickable
    1.0.6   also moved the record 20,000 units down -> still drawn where it was
    1.0.7   also set the delete bit                 -> drawn, no longer clickable

Each build moved one step further into the record while the thing on the pad was
a mesh that had never been told anything. The delete flag is still what stops the
next load from re-creating it; the scale is what takes it off the screen now.

What 1.0.8 does

Same four changes as 1.0.7 - park 20,000 units below the cell, clear
persistence, mark deleted, disable - and then one engine call:

    ref->SetScale(0.001f)

SetScale is called on the reference, so the engine walks the attached node and
moves it with the record. At 0.1% a parcel is a fraction of a millimetre: gone on
the spot, no cell rebuild, no waiting for the scene to stream, and nothing freed,
so no other list can be left holding a dangling reference or inventory instance.

Scale is a record field too, so a save keeps it. The read-back line in the log now
carries it:

    candidate cell 000125AC ref FF02FBF3 base FE06CA10 type=34 'Amara Boots'
        distance 5 at -907,1618,-161 ... persistent=0 deleted=0 disabled=0
    hidden ref FF02FBF3 now at -907,1618,-20161 persistent=0 deleted=1
        disabled=1 scale=0.001

If a parcel comes back after a save and load, compare the `at` position and the
scale with the previous `now at`: the same position means the file re-created it
from its own record, further down means the write survived and something else is
putting a copy back. A `scale=0.001` on the next run's candidate line means the
scale was kept but the object was re-created visible, which is a different
problem - post that log.

Everything is reversible

The log gives every id. In the console:

    FFxxxxxx.enable         bring it back into the world
    FFxxxxxx.setscale 1     restore its size

How to run it

1. Load the save that has the parcels and stand near them.
2. Wait for `ParcelFix: 8 reference(s) hidden. Save now, then the plugin can be
   removed.` in Documents\My Games\Starfield\SFSE\Logs\ParcelFix.log - the scan
   waits for the loading screen to close and the cell to settle, so give it a few
   seconds after you get control.
3. Save.
4. Remove the mod folder. It has nothing left to do.

Settings, in SFSE\Plugins\ParcelFix.ini:

    bEnabled=0    stop the plugin without removing it
    bAct=0        scan and log, change nothing (dry run)
    bDiagnose=1   list every runtime-created reference it saw
    bFlagsOnly=1  restore the old persistent/deleted/disabled test
    iRadius=1500  how far from the player to look, in engine units

It only touches references created at runtime, whose base object is a type the
shop sells (weapon, armour, ammo, misc, aid, book, key), within iRadius of the
player. Planted scenery, quest objects and anything a plugin placed with its own
form id are never candidates.
