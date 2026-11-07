Parcel Fix 1.0.7
================

1.0.7 marks the parcels deleted as well as hiding them. The case for it is the
1.0.5 and 1.0.6 logs, read one after the other:

    20:19  1.0.5  cell 000125AC 'NewAtlantisSpaceport03' - 8 candidate(s)
           FF02FB6B 'Amara Boots' distance 1 persistent=0 deleted=0 disabled=0
           ... Amara Gloves, Jeans, Necklace, Sash, Shirt, Vest ...
           sweep done - 8 hidden

    20:37  1.0.6  cell 000125AC - 15 runtime-created, 8 candidate(s)
           FF02FBD0 'Amara Boots' distance 5 persistent=0 deleted=0 disabled=0
           ... the same seven pieces, new runtime ids ...
           sweep done - 8 hidden

Same seven parcels, new ids, and the disabled flag 1.0.5 had set was gone. The
save rebuilds these references on load and the copy in the file wins: a flag
cleared while the cell is live is not written back, and the park 1.0.6 wrote did
not survive either. The delete bit is the one the file respects - a deleted
reference is not drawn at all, and clicking the parcel and typing
`markfordelete` in the console is the manual fix for exactly this leftover. So
hiding now parks the reference 20,000 units below the cell, clears persistence,
sets delete, and disables it: the same set `markfordelete` leaves, reached from
inside the plugin.

Every hidden reference is logged twice now:

    candidate cell ... ref FF02FBD0 ... 'Amara Boots' distance 5
        at 12,-480,96 persistent=0 deleted=0 disabled=0
    hidden ref FF02FBD0 now at 12,-480,-19904 persistent=0 deleted=1 disabled=1

The second line is a read-back of the live object, so the next run's log can
tell a write that did not happen from a save that did not keep it. If a parcel
still comes back, compare its `at` position with the previous `now at`: same z
means the file re-created it from its own record, further down means our write
survived and something else is putting a copy back.

How to run it

  1. Load the affected save and stand where the parcels are.
  2. Wait a few seconds after the loading screen closes; the sweep runs by
     itself and writes ParcelFix.log.
  3. Save, quit, reload. The parcels should not be there.
  4. Remove the mod once the save is clean.

Nothing is ever destroyed by the plugin: the references are parked, flagged and
marked, not freed. If one was a mistake, `FFxxxxxx.enable` in the console brings
it back - run `FFxxxxxx.moveto player` first, because 1.0.6 and later moved it
twenty thousand units down.

1.0.6 parked what it hid; 1.0.5 dropped the persistent/deleted/disabled test
that had been rejecting the parcels since 1.0.2 and swept the worldspace's
persistent cells too (`bFlagsOnly=1` puts the test back); 1.0.4 narrowed the
diagnostic listing so the runtime-created references were no longer cut off by
planted scenery; 1.0.3 added the walk's duration and rejection tally; 1.0.2
fixed the crashes on save load in 1.0.0 and 1.0.1.
