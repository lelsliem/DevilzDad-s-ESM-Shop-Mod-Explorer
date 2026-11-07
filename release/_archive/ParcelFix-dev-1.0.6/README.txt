Parcel Fix 1.0.6
================

1.0.6 parks what it hides. 1.0.5 found the parcels and set the right flags, and
they were still on the pad when the player looked:

    cell 000125AC 'NewAtlantisSpaceport03' walked 946 slot(s) in 2055 ms
        - 11 runtime-created, 0 unreadable, 0 slot fault(s), 8 candidate(s)
    candidate cell 000125AC ref FF02FB6B base FE06CA10 type=34 'Amara Boots'
        distance 1 persistent=0 deleted=0 disabled=0
    ... six more Amara pieces ...
    sweep done - 1 cell(s), 946 slot(s), 11 runtime-created, 0 fault(s),
        8 candidate(s), 8 hidden

The list was right - seven Amara pieces and the deleted 'Artifact ETA'
activator, all within one unit of the player - and all eight were flagged. A
disabled reference stops being drawn when the cell next builds its scene, not
the moment the flag is set, so a flag-only run reads as "nothing happened".
That is also why the reference list matters more than the flags did: these
carried no persistence at all, and the engine had still written them into the
save, which is exactly what 1.0.5's looser rule was for.

So hiding now does what the shop's own retire does: the reference is moved
20,000 units below the cell, made non-persistent, and disabled. The move is a
plain write to the reference's position - no engine call, nothing freed - and
it takes the parcel off the screen immediately, before any cell update, and
keeps it away whether or not the disabled flag is written to the save. It is
still never deleted: a parcel whose instance the inventory holds would take
that instance with it.

Read the log the same way as 1.0.5. Every hidden reference is a line with its
id, the cell it was in and its base form; the run ends with

    N reference(s) hidden. Save now, then the plugin can be removed.

Then save, quit, remove the mod. `FFxxxxxx.enable` in the console puts anything
back, and it comes back where it was standing before 1.0.6 moved it - twenty
thousand units down - so `FFxxxxxx.moveto player` first if a reference was
hidden by mistake.

1.0.5 dropped the persistent/deleted/disabled test, which had been rejecting
the parcels since 1.0.2, and swept the worldspace's persistent cells as well as
the player's own cell. `bFlagsOnly=1` puts the old test back. 1.0.4 narrowed the
diagnostic listing; 1.0.3 added its duration and rejection tally; 1.0.2 fixed
the crashes 1.0.0 and 1.0.1 had on save load by waiting for the loading screen
to close and by reading the reference array directly instead of through
CommonLibSF's `ForEachReferenceInRange`.
