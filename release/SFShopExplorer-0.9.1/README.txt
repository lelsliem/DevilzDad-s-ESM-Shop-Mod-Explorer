SF Shop Explorer 0.9.1
======================

Live install build: 2026-10-29 17:29 (x64, releasedbg).
Plugin SHA-256: 25E4FCDA017E052E525C3AC38ABC21F4263FF74C319EC997EDD9B80B2C44A45F
(2,263,040 bytes).

This download contains this build only. The 0.9.0 downloads shipped both
DLLs by mistake - SFShopExplorer.dll and DevilzShopOSF.dll together - so
installing either one made SFSE load both builds at once. If you installed
0.9.0, delete Data\SFSE\Plugins\DevilzShopOSF.dll before you start 0.9.1.

Updating: replace the mod folder and keep your SFShopExplorer.ini - themes,
unlock progress and audio settings live there.

Before installing, run Starfield Shader Cache Reset
(https://www.nexusmods.com/starfield/mods/15632) to clear the game's shader
caches; skipping it can cause a CTD when this build first loads. It is a
separate tool, not part of this download.

Install the folder with MO2/Vortex; keep the SFSE structure. Launch Starfield through SFSE and press Page Up to open/close the shop.

This build is compiled from the same sources as the 0.9.1-OSF download, so the
two now carry the same engine - the column sorter's stored order, the
purchase-time drop retire and the Windows-1252 to UTF-8 reply fix are all in
both. The overlay's own behaviour is unchanged from its 18:40 build; the sorter
work is in the code the OSF page drives.

What changed in this build (0.9.1):
  0.9.0 was withdrawn after crash reports. 0.9.1 is the same engine with the
  four defects found since, and one packaging mistake corrected:

  * Each download carries one plugin again. Both 0.9.0 archives held
    SFShopExplorer.dll AND DevilzShopOSF.dll, so installing the overlay also
    loaded the OSF front end: two catalogs, two audio sessions fighting over
    the game's audio, two bridges, and the overlay's Present hook installed
    whether or not you wanted it. This archive holds SFShopExplorer.dll only;
    the OSF download holds DevilzShopOSF.dll only. Installing 0.9.0 is the
    one install path that needs cleaning up by hand - remove the other DLL.

  * Asset discovery no longer reads your mod manager. The plugin used to parse
    ModOrganizer.ini, guess the selected profile, read its modlist.txt and walk
    the manager's mods folder, with B:\MO2\Starfield\mods as a hardcoded
    fallback. It now reads the Data directory the game itself is running. Under
    MO2 or Vortex the VFS has already merged and priority-resolved that tree,
    so exactly one file exists per path and it is the one the game loads. The
    manager's own tree is never opened, so a renamed profile, a different
    profile, or a different manager cannot change what the shop reads - and
    nothing of yours is written to.

  * The page's sounds work. Click, hover, select and the refusal click were
    sent on the command transport while the audio endpoint is a request
    handler, so every one was dropped at the bridge with no error on either
    side. They are now sent as requests. The open and close stings were never
    affected, because the plugin plays those itself.

  * Closing the shop stops the music, and muting silences the effects as well
    as the music.

  * The basket is not silent any more. Add, the +/- steppers, remove and
    accept played nothing in the overlay - only the catalog row and the star
    made a sound. All four do now, and removal has its own: a seventh effect,
    ui_remove.wav, written in the same voice as the click with the tone falling
    away. An Audio\ folder carried over from an older build has no such file,
    so the bank borrows the click instead of going quiet. Enter, which adds the
    selected row to the basket like a double-click does, now sounds that too.

  * Opening the shop counts as a visit every time, on both front ends. The
    OSF build throttled the count to one per five minutes; the overlay was
    always one per open. The throttle is gone.

  * The log is a few dozen lines again. The per-item preview chatter - the
    prefetch lines, the texturing and per-range verdicts, the model build and
    the material lookups - is DEBUG now and only appears with
    [logging] verbose=1. The DRED breadcrumb block at plugin load and the
    %TEMP%\SFShopExplorer-prefetch.txt trace are gated on the same switch, so
    a normal session writes neither of them.

  Still true, and worth repeating: run Starfield Shader Cache Reset once
  before the first launch of a new build. If 0.9.1 still crashes for you at
  load, post Documents\My Games\Starfield\SFSE\Logs\sfse.txt with your
  SFShopExplorer.log - sfse.txt names the game build and the plugin load
  order, which is what a crash on load has to be diagnosed from.
What changed in 0.9.0 (withdrawn - superseded by 0.9.1 above):
  * The first search keystroke after switching category or plugin no longer
    stalls. It took 450 ms because the search key asked FormModelPath for a
    model path, and for each of the 202 dummy-base weapons in the catalog that
    runs the whole OMOD assembly scan (the "dummy weapon ... OMODs: 264 cast"
    work). The key now reads the model path the record declares
    (RawModelPath): no assembly scan, no INI skip filter. Searching "dummy"
    still finds those weapons, by the NIF they actually carry.
  * Backspace no longer sticks. One press in the search box or the quantity
    field deleted the text and kept deleting until the shop was reopened. The
    overlay hides every key-up from the game so it cannot leave an orphan ESC
    edge behind, but it was hiding it from ImGui too, and ImGui only releases a
    key on its matching up - a key it still believes is held auto-repeats. The
    up now reaches the overlay and is still withheld from the game. Measured
    with tools/renderer_recon/backspace_latch_harness.cpp on the vendored
    ImGui: 1 character deleted with the up, all 15 without it.
  * Plugin filtering shows the plugin's records. Light (ESL-flagged) and medium
    files were looked up in each other's tables: 0xFE indexes the light table
    and 0xFD the medium one, and with them crossed every light plugin resolved
    to no owner - "all plugins" listed all 3,284 armors, clicking AmaraArmor.esm
    showed 0 - while a medium plugin showed whichever light file sat at its
    index. AmaraArmor.esm's armors are FE06Exxx, slot 0x06E, far past the
    13-entry medium table.
  * Toggling NAME no longer re-folds the whole catalog. The name sort key was
    rebuilt for all 9,161 rows on every header click - 4.4-5.0 ms in the 21:41
    log, against 0.5-1.1 ms for the numeric columns - because the key loop sat
    inside the sort. The keys are now cached until the list itself is rebuilt,
    so a click only sorts; the one rebuild that pays for them logs its own line
    ("N name sort key(s) built in X.XX ms"). The item search keys are gated the
    same way, so sorting a filtered list no longer rebuilds 9,161 search keys.
  * The rebuild reports its two halves: "list phases: gather X.XX ms, filter
    X.XX ms". The runtime-array walk used to sit outside every timer, so the
    total alone could not say which half a change moved.
  * The item list sorts in about a millisecond. The column used to build each
    row's display name - and case-fold it - inside the sort comparator, so the
    9,155-row All Types list meant roughly 120,000 comparisons and two heap
    strings per click. One key is now computed per row when the list is rebuilt
    (name and type text; value, weight and FormID as numbers, so a 32-bit
    FormID stays exact) and the comparator only reads it. Every header click
    logs the result: "view order N row(s) by column C asc|desc in X.XX ms" -
    9,161 rows measure 0.49-1.08 ms on the numeric columns.
  * A NAME sort reads the name, not the bytes hiding in front of it. Five
    records in the catalog carry an invisible prefix: a leading space (" Golden
    Glow", tankgirlsxenologyexpanded) or U+F225, the game's food/drink icon
    glyph no UI face can draw ("\uF225 Hydrated", "\uF225 Fed", both Aid).
    Sorted as bytes that put those two Aid records above the whole Z block in
    descending order and " Golden Glow" above the A block in ascending order,
    so the list looked scrambled the moment a header was clicked. The padding
    is now trimmed from the name the shop shows and sorts on, and the key starts
    at the first character that can begin a word - '"The Great Serpent" Lie'
    files under T and "(unnamed) [000127BE]" under U, where a reader looking for
    them would go.
  * The basket carries weight. Each line reads "xN - N cr - W.WW wt" and the
    total reads "Total: N credits  -  W.WW wt" (weight alone in free mode), so
    a load-out that would not fit in the ship's cargo no longer looks free.
  * The sort survives a restart. Clicking a header writes [shop] sortColumn and
    [shop] sortAscending to SFShopExplorer.ini and both are restored next
    session, so a deliberate VALUE or WT sort is no longer reset every visit.
    sortColumn is 1-based in the file (1 NAME, 2 TYPE, 3 VALUE, 4 WT, 5
    FORMID); commented defaults in the ini document it.
  * Weapons that copy a vanilla gun no longer preview blank. 202 records in a
    full load order ship a placeholder model instead of a mesh -
    BullpupDummyReceiver for firearms, MeleeDummyHousing for melee. The base
    game's own ones assemble from their mod parts, but a plugin that copies one
    without any model parts of its own ("MagPulse" -> MagPulse Fanisha,
    "Razorback" -> Razorback Trophy, Avarice, Custom Old Earth Revolver,
    Arboron Novastrike Sniper) resolved nothing and drew an empty viewport.
    When a weapon owns no model part at all, the preview now assembles the
    vanilla stock parts its own mod-association keyword binds it to, and the
    log says so. A plugin with its own art is never repainted vanilla.
  * Favourites (added after this build was packaged, same version): a star at
    the left of every row marks a record, and the Favourites toggle in the
    PLUGINS pane lists every starred record across all plugins and categories.
    The list lives in SFShopExplorerFavourites.txt beside the plugin, one FormID
    per line, so it survives an update that keeps the INI and can be edited or
    copied by hand. The five Hide filters moved from that pane into a Hide
    button on the CATALOG row, where the favourites toggle used to have no room
    to sit.
  * The star and the row label share one cell. With the star in a table column
    of its own the two were independent cells, and after a NAME sort they could
    land on different rows: a star alone on a blank line with its name one row
    below, and that row twice as tall as every other. A name carrying a control
    character (a newline measures two lines but draws only the first) produced
    the same blank, tall row and sorted to the top, so names now have control
    bytes folded to spaces before they are drawn, sorted or searched.
  * No change to purchase, delivery, filters, pause or audio.

What changed in 0.8.350:
  * Audio has its own controls. The toolbar's single Music checkbox is now an
    Audio popup with a volume slider and a mute checkbox for music, and the same
    pair for sound effects, plus a track picker. Levels are applied live and
    saved on release under [audio] musicVolume / sfxVolume / musicMuted /
    sfxMuted / musicTrack.
  * Music is chosen independently of the visual theme. "(theme track)" keeps
    each theme's own score; picking any other entry overrides it for every
    theme, so a track the player drops in is not replaced when they switch
    themes. Open still applies the level before the fade starts.
  * One flat Audio\ folder is the whole contract: ui_*.wav (case-insensitive)
    is the seven-effect sound bank and is never offered as music; any other .wav
    in Audio\ or Audio\Music\ shows up in the track list. No manifest, no
    rename, no second install step - drop a file in and reopen the popup.
  * An ini from an older build keeps its setting: when [audio] musicMuted is
    absent, the old [shop] music toggle is read, so an update does not start
    playing over someone who had music off. The log's flat musicPeak=0.0000 in
    the 0.8.349 test was this: [shop] music=0, not a playback fault.

What changed in 0.8.349:
  * Three Misc shelves that were still landing in the wrong bucket now read the
    NIF sub-folder instead of the whole folder:
      - SetDressing\SFBGS003: the 22 mission board terminals (Bounty: The
        Vulture, Guild Network) file under Decor. The four ammo primer boxes in
        the sibling AmmoPrimerBox folder - Large/Small Round Primer, Charged
        Diodes, Magnetic Diodes - file under Tech, so the boards and the diodes
        no longer share a bucket or a row.
      - tankgirlck\xenologymesh: the seven Old Earth PC props (PC, Ancient PC,
        PC Monitor, PC Tower) file under Tech instead of Creature Parts.
      - SetDressing\Outpost\lootcontainers: the 14 Inorg* resource containers
        (Vintrium, Hypermatter, Water, Tibanna, Raw Spice...) file under
        Resources instead of Other.
    Misc / Other falls from 456 (17.7%) to 416 (16.1%).
  * No behaviour change to purchase, delivery, filters, pause or audio.

What changed in 0.8.348:
  * Opacity masks are accepted in BC7. Two texture packs ship the masks the base
    game stores as BC4 - the Erdebrau Pils can's label cutout (Star Wars -
    Environmental Immersion Project) and the Alien Genetic Material jar's lid
    (Starfield HD Overhaul) - as BC7_UNORM_SRGB, which the loader rejected, so
    the can printed its label atlas' black surround and the jar drew a black
    star. The measured coverage channel is red in both, the same as BC4, and a
    BC7 mask can now also be recognised as another part's borrowed cutout.

What changed in 0.8.347:
  * The vial liquid is green instead of neutral grey. ScienceGlassLiquid01/02
    stores no colour anywhere - no override, no layer kind, an empty set, and a
    texture set that points at the kitchen Scissors prop - so the opaque liquid
    fell back to the renderer's no-data grey and the bottle read as empty. It
    now takes the fill family's measured green (0, 0.6524, 0.5964) from
    ScienceGlass_GlassCleanTransparent01_Base_Green03, authored in FlatBaseColor
    the same way the coolant jar's tint is. Affects Bottle 00265AFD, Vial of
    Tranquilizer 0103D06E and Experimental Nutrifying Compound 01040786; plastic
    parts keep their own colours.

What changed in 0.8.346:
  * The ScienceGlass shelf's plastic and liquid parts draw as solids again. The
    sheer-glass scan read the whole material PATH, so the "Glass" of the
    ScienceGlass folder (and of the same word repeated in every file name) was
    read as a material word: ScienceGlassPlastic01/02, Terrormorph_cellsample and
    ScienceGlassLiquid01/02 became clear shells at alpha 0 - the vial caps, the
    DNA-sample tissue, the full-bottle liquid and the Vortex Ichor casing
    vanished. The scan now reads the material's own name (the stem, minus a
    leading repeat of a folder measured as a namespace, not a material word).
    Plastic02 keeps its authored blue override, Plastic01 its grey, and the
    glass parts are untouched - including Common\Glass's frosted panes, which
    repeat their folder name and ARE glass.
  * No behaviour change to purchase, delivery, filters, pause or audio.

What changed in 0.8.345:
  * Clear lab glass no longer previews as nothing. The clear-glass family the
    database ships with an empty material set (GlassCleanTransparent01 and the
    ScienceGlass _Base_Green fills) now draws with the coolant jar's measured
    5%-per-wall transmission instead of zero coverage, and a coloured fill keeps
    its own database colour instead of being bleached pale. Measured on the
    2026-10-24 contact sheet: the Beaker and Graduated Cylinder tiles were blank
    before and draw a vessel now.
  * No behaviour change to purchase, delivery, filters, pause or audio.

What changed in 0.8.344:
  * Misc has eight new sub-buckets, all decided by the item's model folder:
    Decor, Lab / Samples, Creature Parts, Tech / Parts, Data Slates,
    Food / Drink, Artifacts, Animal Parts. Misc / Other fell from 1,417 to 456.

What changed in 0.8.343:
  * Misc and Aid sub-filters read the item's model path as well as its name.
    Aid / Other fell from 58% to 4%, Misc / Cards went from empty to the full
    108-card Sabacc deck.

Notes:
  * Music/sound: Audio\ holds ui_click/hover/select/open/close/denied.wav and
    the tracks. Add a track by copying a .wav there (or into Audio\Music\), then
    open the Audio popup - the list is rescanned each time it opens.
  * src\ is the full C++ source for this build (xmake project, Visual Studio
    toolchain), included for reference and modding.
  * [shop] debugSpike stays 0 for normal play; set it to 1 only when chasing a
    specific purchase in the log.
  * Runtime-sensitive DX12/SFSE plugin. Keep a backup and never replace the DLL
    while Starfield is running.
