Parcel Fix 1.0.9
================

1.0.8 closed the game the moment it hid its first parcel. 1.0.9 is 1.0.8 with
that one call removed, so the hide finishes and the log runs to the end.

What killed 1.0.8

The log stopped on the first candidate and ended with:

    [C] ParcelFix.dll

    Failed to find offset for Address Library ID!
    The plugin needs to be updated and recompiled for the current game version.
    Invalid ID: 0
    Game Version: 1.16.244.0

That is not a crash this code could catch - it is CommonLibSF's address lookup
giving up and ending the process. It came from the line that read the parcel's
scale back out after hiding it:

    const auto scale = candidate.ref->GetScale();

`GetScale()` looks like a field read, but it is an out-of-line stub that calls the
engine through `REL::Relocation{ ID::TESObjectREFR::GetScale }`. The Address
Library installed here has no entry for that id, so the lookup raised a fatal
error. A structured exception handler never sees a fatal error, which is why the
plug-in had no "could not be hidden" line to explain itself: the process was gone
before it could write one.

The hide itself was fine. The `hidden ref ...` line never appeared, which means
HideRef had returned - position park, SetScale, persistent clear, delete bit,
disabled flag all done - and only the read-back after it died.

What 1.0.9 does

Same five writes as 1.0.8, in this order:

    park 20,000 units below the cell
    ref->SetScale(0.001f)            only if the ref still has a 3D node
    clear the persistent flag
    set the delete bit
    set the disabled flag

and no read-back. The log line now reports `shrunk=1` or `shrunk=0` instead of a
scale value, so a ref that had no node to shrink is visible in the log rather
than silently skipped:

    candidate cell 000125AC ref FF02FBF3 base FE06CA10 type=34 'Amara Boots'
        distance 5 at -907,1618,-161 ... persistent=0 deleted=0 disabled=0
    hidden ref FF02FBF3 now at -907,1618,-20161 persistent=0 deleted=1
        disabled=1 shrunk=1

`shrunk=1` means the mesh was scaled on the spot and the box is gone from the
screen. `shrunk=0` means the reference had no attached node - nothing was drawn
for it anyway, so there is nothing to see.

Why the visible boxes are still worth running

The refs left over from earlier versions are flagged deleted but still drawn in
your save. 1.0.9 does not skip those: the node check is the only guard, so a
leftover that is still being drawn still gets shrunk.

Everything is reversible

The log gives every id. In the console:

    FFxxxxxx.enable         bring it back into the world
    FFxxxxxx.setscale 1     restore its size

How to run it

1. Load the save that has the parcels and stand near them.
2. Wait for `ParcelFix: N reference(s) hidden. Save now, then the plugin can be
   removed.` in Documents\My Games\Starfield\SFSE\Logs\ParcelFix.log - the scan
   waits for the loading screen to close and the cell to settle, so give it a few
   seconds after you get control. The boxes should go as it runs.
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
