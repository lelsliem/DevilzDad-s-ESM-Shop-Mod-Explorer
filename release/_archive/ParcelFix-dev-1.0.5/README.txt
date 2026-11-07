Parcel Fix 1.0.5
================

1.0.5 changes two things: what counts as a leftover, and where the sweep
looks. What it does to a leftover is unchanged - set non-persistent, set
disabled, never deleted.

Why the flag test went away

1.0.2 to 1.0.4 only acted on a reference that was persistent, deleted or
disabled, on the theory that only a reference a save keeps can be a leftover.
Three runs against the affected save killed that:

    walk done in 3145 ms - 946 slot(s), 946 in range, 0 out of range,
                           0 unreadable, 0 slot fault(s), 1 candidate(s)
    rejected - not runtime-created 934, no persistent/deleted/disabled flag 7,
               base type not sold 3

946 slots is the whole cell. Eleven of them were created at runtime; exactly
one carried a save flag, and it was the deleted `Artifact ETA` activator. The
four parcels were still on the pad. So the flag test was rejecting the very
references it was written to find, and every build from 1.0.2 on reported
"nothing to hide" while the parcels stayed.

The rule is now: created at runtime, base object of a type the shop sells,
within iRadius. The three flags are still printed for every candidate, so the
log shows whether a hidden parcel was persistent - it is data now, not a
filter. `bFlagsOnly=1` in ParcelFix.ini restores the old test if this ever
picks up something you want left alone.

Where it looks

1.0.4 walked the player's own cell only. A layer of Starfield keeps the
references it treats as persistent in a cell of the worldspace rather than in
the local cell - commonlib marks both `TESWorldSpace::unk0D0` and `unk1D0`
"persistent cell?" - so a reference promoted out of the local cell would never
be walked there, however well the filters matched it. A sweep now walks the
player's cell and both of those worldspace cells, deduplicates by reference id,
and names the cell on every line:

    cell 000125AC 'Beacon Point Landing Pad' (player cell) - 946 slot(s)
    cell 0002A1F0 '' (worldspace persistent cell (0x0D0)) - 812 slot(s)

A worldspace cell is skipped past 60,000 slots, because one of them can hold a
whole worldspace and walking it would freeze the game for minutes rather than
seconds. That is reported, not silent.

The diagnostic listing was also reordered

1.0.3 and 1.0.4 wrote their listing in array order and stopped at a fixed
number of lines, which in a cell full of planted scenery filled up before the
runtime-created references were reached - so the log never showed what the
parcels actually were. The created references are now collected during the walk
and written first, up to 200 of them, then up to 200 references nearest the
player. If the created list is longer than 200, the count line says so.

How to run it

  1. Load the affected save and stand where the parcels are.
  2. Wait a few seconds after the loading screen closes; the sweep runs on its
     own and writes ParcelFix.log.
  3. Read the log: every hidden reference is a line with its id. The last line
     reads `N reference(s) hidden. Save now, then the plugin can be removed.`
  4. Save, quit, remove the mod.

If a parcel survives that, the log will now say why: the cell it is in is named
on every line, the created references in each walked cell are all listed, and
`bAct=0` plus `bDiagnose=1` shows the candidate list without changing anything.
Nothing is deleted, so `FFxxxxxx.enable` in the console puts a hidden reference
back.

1.0.4 is 1.0.3 with a narrower listing: runtime-created, flagged, or within
400 units, written with slot faults and its own duration. 1.0.3 added the
duration and rejection tally. 1.0.2 hid the same things as before and added
the settle timer. 1.0.0 and 1.0.1 crashed on save load; 1.0.2 fixed that by
waiting for the loading screen to close and by reading the reference array
directly instead of through CommonLibSF's `ForEachReferenceInRange`, with every
slot and every reference behind its own guard.
