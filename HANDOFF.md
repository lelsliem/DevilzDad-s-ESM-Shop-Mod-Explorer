# SF-Shop Live Render — HANDOFF

## Pre-publish audit, 2026-11-04 15:45

Checked before 1.0.0 goes official. Nothing shipped changed, so both archives and
the package folders are still the 15:18 cut.

- Package contents: 57 and 59 files, 12 WAVs each, standalone assets including
  `models\materialswaps.tsv`, OSF page 12 files, `node --check` clean on all
  four scripts, every `assets/...` reference resolves (the `assets/space_` hit is
  a `'assets/space_' + suffix` concatenation), packaged INIs and page files
  identical to their source trees, OSF page pins match the files on disk.
- Versions agree: xmake 1.0.0, `kVersion`, `kVersionOsf`, both `meta.ini` files,
  the INI header and the standalone README build line.
- Added the missing `NEXUS-RELEASE-1.0.0.bbcode.txt` and
  `NEXUS-RELEASE-1.0.0-OSF.bbcode.txt`; checklist 2 requires both and neither
  existed.
- `.gitignore` had a CR-terminated pattern (`OSF UI V1.5.0*/`) that could never
  match, and no rule for `build/`, `.xmake/`, `.tmp-aidshots/` or `dist/`, so
  428 MB PCH files and build output were staged for commit. Fixed both and
  removed those trees from the index; the worktree was not touched.
- Left alone on purpose: `SF Starfield MCM` is a separate mod sharing this
  workspace, so only its build output was untracked; `dist\` keeps its four
  archives on disk and is now ignored; the source has no TODO/FIXME and the
  request statics in `PushReply` are only a theoretical race while OSF UI
  dispatches requests serially, so no rebuild was triggered for it.

## 1.0.0 OSF audio-lock rebuild and recut, 2026-11-04 15:18

The single `sfx 'select' got no answer ... 2000ms` line from the 2026-11-04
pass was queueing, not audio: `AudioRequest` took the same `g_mutex` that
`PreviewRequest` holds across the whole synchronous GPU render, so a click made
during a preview waited the render out before its ack could be pushed. The sfx
branch now answers before the lock; it reads no shop state, and `PlaySfx` only
queues a slot for the audio thread.

    DevilzShopOSF.dll   1,809,408 B  2026-11-04 15:18
      DAA386604E77439F35CDCFD095A712546FFE2003AD41A654ABA8DB5782637175

`SFShopExplorer.dll` is unchanged (`5DBCB955…`) - the standalone target does not
compile `OsfFrontend.cpp`. Deployed to the OSF mod with a rollback copy in
`_backup-20261104-audiolock\`, packaged, and both archives recut (the standalone
archive changed only in its `src` tree):

    release\SFShopExplorer 18102 1.0.0-20261104.7z  32,247,343 B
      963A9B902E881FFF75E8D3C8B808871D0F435F70B49F2345C82CAD0111CCC3F8
    release\DevilzShopOSF 1.0.0-OSF-20261104.7z     17,261,806 B
      95BA955C4A151F48C3FFE17DC273668796779A87EDF18212E355102CFC621486

Both pass `7z t`; extraction matches the package folders with zero differences
and the README pins inside match the DLLs. Play-tested 2026-11-04 15:30-15:34:
audio reported working, 0 `[E]` and 0 `sfx ... got no answer` lines in either
log. Not published.

## 1.0.0 rebuilt, deployed and recut, 2026-11-03 17:45

Both targets rebuilt after two log-level changes (an absent derived
`_opacity.dds` logs at DEBUG; the mesh vert/index mismatch caps at three WARNs
per session instead of 1,292) and deployed to the MO2 folders. Rollback copies
of the previous DLLs and `src` trees are in `_backup-20261103-lognoise\`:

    SFShopExplorer.dll  2,363,904 B  2026-11-03 17:38
      5DBCB955CBCCECF43A6226DCA366AC4E0179169390A305E0B32D6D6FBB72122F
    DevilzShopOSF.dll   1,809,408 B  2026-11-03 17:38
      F72A0131186E91796EEA27031C3F922018FABF1DB3111D3FBFEDAF6D192BE54D

Archives were recut from the package folders (contents at root, `-t7z -mx=9`)
and the 2026-11-01 pair moved unchanged to `release\_archive\`:

    release\SFShopExplorer 18102 1.0.0-20261103.7z  32,247,228 B
      3BB0A2E8FCEC96F7E85512011582A931451E0AAC7894F597F204923C344B2053
    release\DevilzShopOSF 1.0.0-OSF-20261103.7z     17,261,287 B
      E7435AC5B5EFCEFFE52D8DDEA872B5E2CF5A50AF2907432A48EBA1C061121D98

Both pass `7z t`, extraction matches the package folders exactly (57 and 59
files), each holds one DLL, and the README pin inside each archive matches its
DLL. Smoke-tested 2026-11-04 14:51-14:57 on game 1.16.244.0: both DLLs load
v1.0.0.0, 0 `[E]` in either log, the mismatch cap holds at three WARNs per
session (was 1,292), and the one refused mask logged at INFO with its reason.
The AMMO crate fallback was not exercised (no crate preview). One
`sfx 'select' got no answer` timeout at 14:56:24 still wants a look. Not
published.

## Remaining older shop archives moved to _archive, 2026-11-01 15:45

The eight 0.8.x and 0.9.0 shop archives were moved unchanged from `release\`
to `release\_archive\`:

    SFShopExplorer 18102 0.8.300-20261018.7z  28,814,620 B
      FD43410D62E253E68160B9FE8623597BD51522B393215AE22452006F08E5C891
    SFShopExplorer 18102 0.8.301-20261018.7z  32,050,649 B
      8463147076A57F8B0DF7FEF93F3A8AE0B499A5EC28A855E9FF492A29D7AF4822
    SFShopExplorer 18102 0.8.350-20261024.7z  32,137,529 B
      C3CA2B50F8C25B2CD67449E6A07AACCAE1DAD1904C06E8E093F75C400D6A32F7
    SFShopExplorer 18102 0.9.0-20261025.7z    32,150,255 B
      1592CA804DF41B2A4A3364BDDE93D4E0A40E233773FBA730BF288E86FAF55D25
    SFShopExplorer 18102 0.9.0-20261026.7z    32,391,045 B
      04A036982992D826275997E2F959D91A215B86D004A98CF903A270D7887A0B13
    DevilzShopOSF 0.8.350-OSF-20261025.7z     15,842,126 B
      843DE91279B0BBF546ED8D24762A452E70E0A8A9A32EC8E4F691ABEC0AE38860
    DevilzShopOSF 0.9.0-OSF-20261025.7z       15,844,480 B
      2B9A0941B0ECB304A2A2D2EE4522B8663C5104C42BFF411CDAED928E0C425088
    DevilzShopOSF 0.9.0-OSF-20261026.7z       16,288,368 B
      666139D9404D5958A6E26A7A94DDE341700DAB1A8614023EF5781E32F56C1F98

Every size and SHA-256 is identical before and after the move. `release\` now
holds exactly two shop archives - the 1.0.0 pair - plus the separate
`ParcelFix 1.0.0-20261026.7z`. `release\_archive\` holds 12 shop archives and
the user-built Parcel Fix archive.

## 0.9.1 archives moved to _archive, 2026-11-01 15:44

Moved unchanged from `release\` to `release\_archive\`:

    SFShopExplorer 18102 0.9.1-20261101.7z   32,196,998 B
      8AEC91CC4A007E6958358C4B2602BB4E6482003FE785780942CE614CFA9FC42F
    DevilzShopOSF 0.9.1-OSF-20261101.7z      17,243,003 B
      F1F94C1A65D5A49BEBEF1A2A3C821E14553EECCD8F259588246760555D28A186

Hashes and sizes are identical before and after the move; both still pass
`7z t`. The 1.0.0 pair is now the current release pair in `release\`. The older
0.8.x and 0.9.0 archives were still in `release\` at that point; they
were moved to `release\_archive\` later the same day (see the entry at
the top).

## 1.0.0 archives cut, 2026-11-01 15:42

Both archives were cut from the verified package folders with contents at the
root, `-t7z -mx=9`, and the same 20261101 suffix:

    release\SFShopExplorer 18102 1.0.0-20261101.7z
      32,245,174 B  57 files / 6 folders  47,447,790 B unpacked
      sha256 E054F5E7A7AF9099C797606B7E37668B61860D8F49EE3826F99DCC886A349131
    release\DevilzShopOSF 1.0.0-OSF-20261101.7z
      17,259,440 B  59 files / 9 folders  31,196,172 B unpacked
      sha256 0EBD4E2F5379074D019F69E8CCDF4FCB25BCE97403E7014E6F336F60A3B3F098

`7z t` reports Everything is Ok for both. Each archive has `README.txt`,
`meta.ini`, `SFSE` and `src` at the root, with no wrapper folder. Extracting
each and comparing it with its package folder gives no differences (57/57 and
59/59 files). Each contains exactly one DLL, and the README pins inside the
archive match the DLL and page files inside it. The 0.9.1 archives were left
in place at that point; they were moved to `release\_archive` later the
same day (see the entry at the top).

## 1.0.0 package folders created, 2026-11-01 15:38

Both release folders were built fresh from the 1.0.0 outputs; no 0.9.1 folder
was edited.

    release\SFShopExplorer-1.0.0        57 files
      SFSE\Plugins\SFShopExplorer.dll  2,359,808 B
        4462805B97EB51F6A84A0815DAA819E95C84C3934A439F2D874210E87706A9B0
    release\DevilzShopOSF-1.0.0-OSF     59 files
      SFSE\Plugins\DevilzShopOSF.dll   1,806,336 B
        902F48EC348EED33FD59E7A1FF5DB118B9384ADF5D39E76F65CDD8D95A82ECE2

Standalone payload: the built DLL, the 1.0.0 source INI (theme=0, premium
counters 0), `audio\samples`'s 12 WAVs, the ten shipped `SFShopExplorerAssets`
images (byte-identical to the 0.9.1 package and the live install), and
`tools\materialswaps.tsv` at `SFShopExplorerAssets\models\`. No
`space_dragon.jpg`, cdbindex cache, PDB, build tree or archive is included.
OSF payload: the built DLL, source INI, the same 12 WAVs, the 13-file page
tree, and the source README/meta.

Both packages carry the current 30-file `src` tree. The OSF README is
byte-identical to the source; the standalone README is the 1.0.0 release
edition (1.0.0 header, build pin, new changelog) derived from the 0.9.1
user-facing document. Every DLL, page and README pin was re-hashed from inside
the package; every src, audio and asset payload was compared with its source.

## 1.0.0 builds and README pins, 2026-11-01 15:31

Both targets were built from the same source state after the 1.0.0 version
bump. Starfield was closed; `build.cmd` ran both gates and passed.

    SFShopExplorer.dll  2,359,808 B  2026-11-01 15:28
      4462805B97EB51F6A84A0815DAA819E95C84C3934A439F2D874210E87706A9B0
    DevilzShopOSF.dll   1,806,336 B  2026-11-01 15:29
      902F48EC348EED33FD59E7A1FF5DB118B9384ADF5D39E76F65CDD8D95A82ECE2

Both report FileVersion/ProductVersion 1.0.0.0. Outputs are
`build\windows\x64\releasedbg\`; neither build log contains `error C`,
`error LNK` or `fatal error`. The BA2 probe and the outfit audit passed (94
complete groups from 4,157 ARMO records).

README pins were filled from those DLLs and the three page files that ship
from `OSF Shop Explorer\...\shop`: `app.js` 472CC2F5... (73,239 B),
`index.html` 57460F96... (9,832 B), `shop.css` 081EE053... (21,286 B). The
standalone README gained its first release pin; the OSF README's placeholder
was replaced with the real plugin and page pins. Every pin was re-parsed and
re-hashed against the files.

No package folder, archive, or MO2 install was changed. The earlier full
preflight (788 cases across the three unittest trees plus the outfit audit)
still stands; no source file changed after it, only the READMEs.

## Live MO2 READMEs synced, 2026-11-01 15:00

Both MO2 installs now carry the refreshed README text; only `README.txt` was
replaced.

- `B:\MO2\Starfield\mods\OSF DevilzDad's ESM Shop\README.txt` is byte-identical
  to the 0.9.1-OSF package README (sha256
  `E79F66FA2BE67125CCBA67EA42BE293515175B4C6E437A09503B2986F92B3C1E`). Its DLL
  and all four page pins match the installed files: `46C66AD3...F207426E`,
  `472CC2F5...F09675`, `57460F96...A503E`, `081EE053...F8CA085`.
- `B:\MO2\Starfield\mods\DevilzDad's ESM Shop Mod Explorer\README.txt` is the
  standalone 0.9.1 README except for its three-line identity block, which now
  names the installed 2026-10-30 17:32 DLL: 2,358,272 bytes, sha256
  `DD3FDD0D2EBABB790566688294781F97683C04503EAB9EED3315A4968386D43A`. The
  archive README still pins the older 2,263,040-byte `25E4FCDA...44A45F` build;
  copying that pin would have mislabeled the live binary. README sha256
  `F0CDAC923091D2E667C3C621869A07A8EFBAF34A57614F9D36E5CEE6049BBD64`.

The INIs were not touched: `SFShopExplorer.ini` sha256
`97187E28...AC80BD` and `DevilzShopOSF.ini` sha256 `3FCC6CC7...5BA387`,
identical before and after. User music, `space_dragon.jpg`, backups and other
extra content remain. No archive, source or DLL changed in this sync.

## SFShopExplorer 0.9.1 archive re-cut, 2026-11-01 14:56

The standalone 0.9.1 folder was already current, but its 2026-10-29 archive
predated two documentation fixes: the README still called the OSF download
0.9.0-OSF, and `SFShopExplorer.ini` lacked the expanded music-format notes.
Re-cut from `release\SFShopExplorer-0.9.1` with 55 files and 6 folders at the
root (47,273,719 bytes unpacked):

    release\SFShopExplorer 18102 0.9.1-20261101.7z
      32,196,998 B
      sha256 8AEC91CC4A007E6958358C4B2602BB4E6482003FE785780942CE614CFA9FC42F

`7z t` reports Everything is Ok. Extracting it and comparing with the folder
gives no differences (55/55 files); the README's plugin pin
`25E4FCDA017E052E525C3AC38ABC21F4263FF74C319EC997EDD9B80B2C44A45F` /
2,263,040 bytes matches the DLL inside the archive. The old
`release\SFShopExplorer 18102 0.9.1-20261029.7z` was moved unchanged to
`release\_archive\` (32,196,927 bytes, sha256
`01B5F39CE727AE36641C62536952616F54EC533900FB510E6A2B025E063EFAF2`), so both
0.9.1 archives in the main release directory are now current.

## 0.9.1-OSF package and archive refreshed, 2026-11-01 14:52

The OSF view tree in `release\DevilzShopOSF-0.9.1-OSF` was refreshed from
`OSF Shop Explorer\SFSE\Plugins\OSFUI\views\devilzdad.osfshop`: `app.js`
73,239 bytes (sha256 `472CC2F5B0B1795B80B8C4836BA30E55C23699261FC7BD5D72AB858E55F09675`,
2026-10-31 14:06:30 UTC) and `index.html` 9,832 bytes (sha256
`57460F9671D805A08DF138591C7897173EDE00C586890B370E3E068F1B0A503E`, 2026-10-31
14:05:33 UTC). All 13 view files are now byte-identical; `shop.css` and the six
assets were already the same. The update carries the cache-busting
`?v=20261031b` query strings and the backdrop diagnostic logging into the
package.

README pins were updated in both `OSF Shop Explorer\README.txt` and the package
README, which remain identical. The DLL is unchanged at 1,804,288 bytes /
`46C66AD3C2D704C8BD1F66EC9ED5A22F530521E1DBAD19648378610BF207426E`;
`shop.css` remains 21,286 bytes /
`081EE0536B9FD7AED1A1136495A3A782DA189322E1514BCC773C9C8D7F8CA085`.

New archive, cut from the refreshed folder with contents at the root (57 files,
9 folders, 31,115,461 bytes unpacked):

    release\DevilzShopOSF 0.9.1-OSF-20261101.7z
      17,243,003 B
      sha256 F1F94C1A65D5A49BEBEF1A2A3C821E14553EECCD8F259588246760555D28A186

`7z t` reports Everything is Ok. Extracting it and comparing with
`release\DevilzShopOSF-0.9.1-OSF` gives no differences (57/57 files). The old
`release\DevilzShopOSF 0.9.1-OSF-20261029.7z` was moved unchanged to
`release\_archive\` (15,880,534 bytes, sha256
`2D8BF2E6C024ED5228C6E925A3ABA9253A41F9D046E47D6019D50AC75DD6A161`), leaving one
0.9.1-OSF archive in the main release directory. The standalone SFShopExplorer
archive was not touched.

## Outfit "render all" audit, 2026-11-01

The `LiveModel::BuildModels` / `AssembleSiblings` path was checked against
every installed ARMO record and every complete `upperbody` / `lowerbody` /
`sleeves` NIF set. There are 94 complete three-part groups on disk; 78 are
referenced by 280 record/group pairs (a record can name both gender groups).
195 pairs already declare all three parts in their group, 31 declare two, and
54 declare one, so 85 pairs have an in-group missing part. The fallback supplied
a part for 34 of them; in the other 51 the record already declares that type in
another group, so the guard skips the duplicate. **No record's assembled union
ended incomplete**, and all 234 unique NIFs in the assembled sets parsed to at
least one usable LOD0 mesh. Path health across
the 4,157 ARMO records is 4,066 with every declared NIF present, 50 with at
least one declared NIF absent while another is present, 2 with every NIF
missing, and 39 with no NIF paths (the sampled names are skin/strut tokens).
The two total misses are not outfits: ZNA4's skin
(`actors/zna4/zna4.nif`) and the Nite Owl helmet record, whose three declared
`clothes/mandaloriansuits/...` paths are absent although the install has
`helmet/niteowlhelmet.nif`. The 32 records with a single part-token path have
no sibling on disk, so they are standalone or single-part assets rather than
misses. The only complete-group NIFs with no usable mesh are unreferenced
`greedo/greedo_sleeves_f/m.nif`; no ARMO record uses that group.

The sibling lookup now takes the part token from the basename and replaces
that occurrence; `BuildModels` also passes the record's declared part types so
assembly only fills a missing type. This removes the SWCLMT folder-token
mismatch (three paths now resolve `lowerbody`) and stops Militia Armor's
`speclowerbodyaddon.nif` from pulling in `specupperbodyaddon.nif` beside its
custom upper body. The Akila `jeanslongsleeveshirt` names still trigger
harmless failed sibling lookups. `test_part_token_is_taken_from_the_filename`
pins both rules; `xmake build -y` and the 4-test inline/assembly suite pass.
The audit is now repeatable: `python tools/renderer_recon/outfit_assembly_audit.py`
rebuilds the report and exits 1 on a record missing a part type or an assembled
NIF without a usable LOD0 mesh, 2 if the catalog or complete groups are absent.
`build.cmd` now runs it as a pre-build gate when the Data directory and catalog
are present, writes its build-time report to `%TEMP%`, and aborts on drift;
`SFSE_SKIP_OUTFIT_AUDIT=1` opts out. `SFSE_CATALOG_OVERRIDE` and
`SFSE_MODS_ROOT_OVERRIDE` point the gate at another install.

`python -X utf8 tools/check.py` is the single local check: it discovers and
runs `tools/` (491 tests), `tools/ba2_audit/` (48), and
`tools/renderer_recon/` (248), then runs the outfit audit. It returns 1 if any
suite or the audit fails, skips the audit when Data/catalog are absent, and
accepts `--no-audit` / `--report`. `--junitxml <path>` writes one merged
`<testsuites>` document for CI, with a suite per unittest tree and the audit's
per-record suite. The audit names each failed record and its missing or
non-buildable parts; a clean audit supplies one passing case. A suite that
fails before writing XML becomes an error case instead of vanishing from the
report. `tools/test_junit_runner.py` pins the writer's outcome counts, XML
character filtering, and the merge edge cases. `.github/workflows/windows-checks.yml`
runs two jobs on `windows-latest` for `main` pushes and pull requests that touch
a non-Markdown file under `SF Shop Explorer`, the workflow, or
`.github/patches`; docs-only edits start neither job, and a newer run on the
same branch or pull request cancels the older one. The Python job installs
Python 3.12 with the pinned `tools/requirements-ci.txt` (NumPy/Pillow), caches
pip downloads keyed by that file, writes `reports/check-junit.xml`, and uploads
the `python-check-junit` artifact even when tests fail. The C++ job restores a
cached CommonLibSF checkout keyed by commit `9e8333a` and the patch hash,
validates the commit/header/submodule before reusing it, and applies
`.github/patches/commonlibsf-tesobjectweap.patch` idempotently. Its xmake setup
uses the action's package cache, keyed by xmake `3.0.9`, the project package
hash and the runner platform, caches the installed xmake binary in
`.xmake-cache`, and caches xmake's compiled-object store with the action's
`build-cache` input; `xmake.lua` opts into `build.ccache` only when
`XMAKE_ACTION_BUILD_CACHE` is set, because xmake otherwise disables it for
MSVC. It runs `build.cmd` with both game-data audit gates disabled, then
builds the non-default `DevilzShopOSF` target with xmake, checks the build log
for compiler diagnostics and both outputs, and uploads the
two `releasedbg` DLL/PDB pairs as separate `SFShopExplorer-releasedbg` and
`DevilzShopOSF-releasedbg` artifacts. The
Python audit skips on a runner without Data/catalog, and the three
weapon-texture tests now use `skipUnless` for their `B:\` archives instead of
asserting their presence. The full Python run is about seven minutes. Two stale
source-shape tests blocked the first run and were corrected:
the audio test now pins the current multi-format track allowlist, and the mesh
UV test uses `LoadConfig` as the end marker because `DiscoverMoMods` was removed
when asset discovery moved to the loaded Data view.
The post-fix run reports 34 fallback pairs, 51 pairs covered by another group,
0 missing records and 0 non-buildable records. `test_outfit_assembly_audit.py`
adds eleven tests for the command's pass/fail exits, the multi-group guard, the
`build.cmd` wiring, and the per-record JUnit cases. No Starfield process was
running, so this is an asset/source audit, not a live alignment or material
check. Full table and method:
[reports/outfit-render-all-audit-2026-11-01.md](SF%20Shop%20Explorer/reports/outfit-render-all-audit-2026-11-01.md).

## Transparent-prop silhouette audit, 2026-11-01

`tools/renderer_recon/silhouette_audit.py` scans every record with a resolved
NIF at 160 and counts closed rim loops per group with the workbench's own
`rim_groups`, `merge_panes` and `outline_mask`. That is the seven item forms
(ARMO, WEAP, MISC, AMMO, ALCH, KEYM, BOOK), containers (CONT), and any other
form that carries a model path. The 2026-11-01 run scanned 11,794 records: 989
had rimmed geometry in 1,043 groups, 39 items had more than one rim group, and
47 items flagged a group with more than one closed contour. Widening from the
seven item forms added 1,156 container/modeled records; 97 have rimmed glass and
the Display Shelf was the only new flag. The Bacta vial override then cleared
19, leaving 28 flags.

The 28 remaining flags are expected multi-pane props, not missed close merges:
separate windows, lenses, panes or bubbles (FloaterA eyes, VHS tape windows,
Bubble Level's four spirit vials, Varuun's visor plus sensor, five Spartan
split visors, seven SW weapon lens stacks, Aurora mask, submarine clock, bone
horn, swab tube, special-sauce cube, skin-cream bottle, and the Display Shelf's
three frosted panes). The 19 Bacta/Panacaea vials were one glass shell split
into two panes whose gap needs a close radius of about 11 at 160 (37 at 512).
`contact_sheet.PANE_MERGE_SCALE_BY_MODEL` now widens the close to 1/13 (12px at
160, 39px at 512) for the ten vial NIFs only. It is keyed by NIF basename, not
by `BactaGlass` shape or glass material, because injectors and sprays use the
same shape and are already one pane. Scale 13 merges at 160 through 1024; 14
merges at 160 but re-splits at 768, so it is not a safe margin. The report is
`SF Shop Explorer/reports/silhouette-audit-2026-11-01.md`, with the grid
`flagged-silhouettes-160.png`, the per-item data
`silhouette-audit-2026-11-01.tsv`, and the 19-vial before/after grid
`bacta-pane-merge-before-after-160.png` next to it.

The override was checked across the whole catalog: the after scan has exactly
19 fewer flags, no new flags, and no other record's group/contour counts
changed. Split visors, cassette windows, the display case and shelf, an
action-figure blister pack, and a non-vial Bacta injector render byte-identical
to their before tiles at 160.

`bacta_vial_sheet.py` is the focused regression view: 19 tiles at 160 with the
closed-contour count under each, written to `reports/bacta-vials-160.png`. It
exits non-zero if any vial is missing, blank or split, so run it after a change
to `merge_panes`, `pane_merge_scale` or `outline_mask`.

`silhouette_index.py` writes the whole audit as one family-grouped 160px index,
`reports/silhouette-index-160.png`: the 19 cleared vials first, then the 28
remaining flags grouped by the triage families. Each tile is labelled with its
FormID, type, closed-contour count and name. The companion
`reports/silhouette-index-160.html` is self-contained and combines family
buttons, a flagged/cleared toggle and a text search over name, FormID, type,
plugin and family; it highlights matches, binds `/` to focus the search box and
Escape to clear it, resets all three filters with a Clear filters button, shows
the live match count, hides empty sections, and links each tile to the workbench
at `http://127.0.0.1:8791/?form=<FormID>` (`--workbench-url` overrides the
base). It mirrors the active filters into the URL as `family` (section index),
`status` and `q` with `replaceState`, so reloading or sharing the address
restores the view; Clear filters removes those parameters and Copy link copies
the canonical filtered URL (Clipboard API, temporary-textarea fallback) with
Copied/Copy failed feedback; each tile has its own copy action for its direct
`?form=<FormID>` workbench link, and Copy visible links copies the filtered
set one URL per line (disabled when nothing matches). It fails on a missing/blank record or a
split Bacta vial, and it validates the 28 flag records against the audit TSV so
a new flag cannot be omitted; a flag turning green is a fix and is not a
failure.

The scan itself needed one supporting change: `AssetStore.index_loose()` walks
the 549 MO2 mod folders once instead of probing them on every read (about 15s),
which takes the scan from ~838ms/item to ~80ms/item. It is opt-in because the
workbench reads only a handful of files and does not want the one-time walk.
`prepare_item(material_files=False)` skips the .mat colour fallback for the
scan; a 30-item check against the normal path produced identical rim decisions
(only effect-only decal geometries disappear).

`test_silhouette_audit` pins the contour counting, the widened scan scope and
the loose index (6 tests); the contact-sheet suite is 38 (including the vial
family close at 160/512) and the workbench suite 19. No DLL or MO2 install
changes; the workbench hot-reloads contact_sheet.py.

## Nearby panes of one prop merge before the rim, 2026-11-01

Grouping rim parts by world bounds kept different props apart, but the panes
inside one group were still separate islands in the coverage mask: CrimX's two
visor panes drew two closed outlines with a frame gap between them. `merge_panes`
now closes each group's screen mask by `size / 20` (8px at 160, 26px at 512)
before `outline_mask`, so nearby panes of one prop become a single silhouette.
The radius is tied to the tile so the 160 contact sheet and the 512 focus agree;
two panes farther apart than two radii still keep two contours, and separate
rim groups are never closed together. The closing runs through a box-sum
dilate/erode: MaxFilter at r=17 cost 1.4s per 512 tile (21s at 1024), the box
version costs 2.5ms (10ms at 1024) for the same mask. The mask is padded by the
radius before closing because BoxBlur truncates at the canvas edge - without it
a pane near the edge survived erosion along the tile border and drew a rim down
the side of the image.

Verified at 160 and 512: CrimX now reads as one connected visor with a shallow
notch, while Eit Glasses, Mark VII, Mark VII JFO and Display Case keep their
single contour. `test_nearby_panes_of_one_prop_merge_before_the_rim`,
`test_panes_far_apart_stay_separate_after_merging` and
`test_merging_near_the_tile_edge_does_not_rim_the_border` pin the three cases.The contact sheet passes 38 tests and the workbench 19 (57 together). No DLL or
MO2 install changes; the workbench hot-reloads contact_sheet.py.

## Visor panes and display cases get the clear-shell contour, 2026-10-31

The clear-shell pass covered materials that name themselves (`glass`, `clear`,
`visor`, `window`, `plexiglass`), but an armor visor's material often carries no
name and no database alpha: the Spartan Mark VII visor's one BSGeometry is
`MarkVIIHelmet_MARKVII_visor` over `MARKVII_Helmet.mat` at authored alpha 1.0.
It drew as a dark solid and had no contour. `contact_sheet.clear_pane_shape`
now reads the BSGeometry name as a second, per-piece signal (`visor`, `window`,
`lens`/`lense`), with `frame`, `trim`, `housing`, `mount`, `bracket`, `handle`
and `cover` as blockers so a window frame or lens mount stays solid. A
shape-named pane whose material is opaque by omission draws at `0.30` (its
tint is still readable); empty glass keeps the `0.16` floor, and a material that
really authored 1.0 (an opaque glass body) still keeps that opacity.

The rim is now the outer silhouette, not a per-pane edge. `rim_groups` first
groups rim parts by their world-space bounds (same NIF, `1%` of the item
diagonal as margin): a visor and its light strip, or a lens inside its frame,
share one mask even when their screen gap is wide. `filled_mask` then floods
the padded coverage from the outside, turns the enclosed zeros solid, and the
edge is taken. Separate world boxes (two vials in a rack) and different NIFs
keep their own rims. `test_world_bounds_merge_panes_but_not_separate_props`,
`test_an_enclosed_hole_is_not_an_extra_rim` and
`test_two_separate_panes_keep_two_outer_rims` pin the split.

That rule needed a mesh to act on. An ARMO's `MODL` subrecord is a form link to
its ARMA addon, so `Plugins._scan` cannot hand the workbench a NIF and every
visor said **no MODL**. `parse_catalog` now keeps the dump's `modelPath` and
`nifPaths`, and `prepare_item` falls back to the first non-proxy path for ARMO.
`is_proxy_model` mirrors `NativeModel.cpp`'s `IsProxyModelPath`, so the
`SetDressing\...Container...` dressing box is never drawn over the garment.

Verified on the live workbench: a selection of Mark VII, Mark V[B] JFO, CrimX
Visor 1, Eit Glasses Visor (Red) and Display Case drew all 5 with geometry. The
focused Mark VII record resolves 314 triangles from its catalog NIF; the
display case still draws its six panes. `test_contact_sheet` passes 33 tests
(52 across the contact sheet and workbench), including the shape-vs-frame
split, the world-bounds grouping, the outer-silhouette hole fill, the catalog
path parse, and the shipped Mark VII NIF. No DLL or MO2 install changes.

## Offline weapon previews mount their OMOD parts, 2026-10-31

The offline workbench drew a dummy-base weapon as a list of OMOD NIFs, each at
its own origin, so A-300/Razorback was an exploded pile. `contact_sheet` now
resolves the NIFs' BSConnectPoint graph before drawing. `connect_points.py`
ports `NativeModel::ResolveAssemblyPlaces` and `LiveModel::ParseNifConnects`:
the `C-Receiver` holder is the identity root, a part lands at
`parentPlacement * hostNodeWorld * entryTransform`, and chains resolve in path
order. `prepare_item` applies one placement per part and fixes the vertex
convention to `p @ M.T` (NIF matrices are column-vector; the old row-wise use
silently dropped node translations and transposed rotations).

`omod_assembly.nifs_for` also adds the receiver OMOD when the `_Standard` tier
omits it: Microgun marks only its magazine, grip and barrel standard, so the
selected set had no `C-Receiver` root and every part stayed at the origin. With
the receiver, Z6 Rotary draws 4 parts / 53 shapes instead of 3 overlapping ones.
A part with no matching host still draws at identity - some OMOD meshes are
authored in place, and the C++ resolver logs the same fallback - and the focus
panel now lists it under **unplaced**, so a wrong assembly pick is visible.

Verified: `test_connect_points` (8: chain, mask, matrix convention),
`test_omod_assembly` (3), `test_contact_sheet` (25) and
`test_render_workbench` (19) pass. Live A-300 mounts 3/3 parts, Z6 Rotary 4/4.
Across the catalog's 126 multi-part weapons, 28 still have at least one part
with no host; those are assembly-pick gaps rather than placement math, and they
stay at the identity fallback with the **unplaced** label instead of being
silently dropped. The painter-sorted renderer still shows
an assembled gun's interior shells as an x-ray; placement and silhouette are the
real ones.

## Workbench references and clear-prop visibility, 2026-10-31

The workbench focus panel now links every record to a Fandom search
(`/wiki/Special:Search?query=`) and a StarfieldDB search (`/?s=`), so a tile can
be checked against the reference sites without retyping its name.

The Vial shelf showed why a whole class of Misc items looked empty: an empty
vial drew only its cap. `GlassCleanTransparent01.mat` is authored at alpha 0.05,
and this renderer has no specular or refraction, so the body vanished against
the dark backdrop. Clear shells below 0.16 are now drawn at 0.16 in the tile
only; the material report keeps the authored alpha and its reason. A clear shell
is identified by its material name (`glass`, `clear`, `transparent`, `visor`,
`window`, `plexiglass`, `acrylic`) or by the resolver's `clear shell` /
`fresnel` reason, so the industrial display case's `PlasticCleanClear01`
(alpha 0.00, a 16-triangle pane) now draws its panels. Re-running the 12-record
Vial batch changed 9 tiles and the empty bodies are visible. A silhouette rim
from the clear-shell coverage mask adds 1px at 160 and 2px at 512; the 160px
Vial grid now shows each empty body's contour rather than a faint rectangle. An
opaque shell body tints the rim toward itself and lowers its alpha to 0.24, so
the coloured vials do not get a pale outline while empty shells keep the
neutral 0.45 contour. `test_clear_glass_gets_a_visible_preview_alpha` pins the
floor and leaves non-clear alpha untouched;
`test_a_glass_rim_follows_the_silhouette_not_the_triangles` pins the edge pass;
`test_the_rim_is_for_translucent_glass_only` and
`test_a_coloured_glass_body_tints_and_softens_the_rim` pin the split;
`test_clear_plastic_and_visors_are_clear_shells` pins the broadened
classification. The white wireframe crates in the same batch are the
painter-sorted interior-shell limitation, not this alpha issue.

## GitHub repository organised into ImGui and OSF install trees, 2026-10-31

`lelsliem/DevilzDad-s-ESM-Shop-Mod-Explorer` now ships both front ends as separate,
install-shaped trees instead of a flat source dump:

- `imgui/` — `SFSE\Plugins\SFShopExplorer.ini`, `SFShopExplorerAssets\` (themed
  chest/backdrop art, logo, credit icon, `models\materialswaps.tsv`) and `Audio\`
  (12 WAVs: five theme/bed tracks and seven `ui_*.wav` effects).
- `osf/` — `SFSE\Plugins\DevilzShopOSF.ini`, the same audio set, and the whole
  `OSFUI\views\devilzdad.osfshop\shop` page (HTML/CSS/JS, manifest, `assets\` art).
- Shared `src/`, `xmake.lua`, root `README.md`, plus a per-tree `README.md` with
  build, install, controls and settings notes.
- No DLLs, PDBs, `Depends/` or build output are committed; `.gitignore` covers
  them and `.gitattributes` keeps INI/TSV bytes and marks art/audio binary.

Both targets were rebuilt from this layout with `Depends/commonlibsf` and
`Depends/imgui` present: `SFShopExplorer.dll` 2,358,272 bytes (sha256
`2449258E...B6B5621`), `DevilzShopOSF.dll` 1,804,288 bytes (sha256
`46C66AD3...F207426E`), both `releasedbg` / MSVC 2022, no `error C`, `error LNK`
or `fatal error` in the logs. All four OSF page scripts pass `node --check`; every
`assets/` reference resolves. The built DLLs were test-copied into both install
trees to confirm the layout, then removed before commit.

Commit `be1498be3a9b276aa0a353cdc38c83a5529e75f3` (“Organise ImGui and OSF
install trees; add INI, art and audio”) pushed to `main`; the remote tree is 102
blobs, including all 30 source files and both asset trees.

## OSF backdrop is one layer again, 2026-10-31

Updated: 2026-10-31 — the OSF backdrop is one layer again, and that is what
"background images are still weird" was. `#bgimg` was a `contain` sharp copy with
a `cover` blurred copy of the same file (`#bgblur`) behind it, drifting; wherever
a window's aspect differs from the art's the two copies put the same pixels at
different scales and offsets, so the sides read as a ghost of the picture that
the drift kept sliding and the seam between them kept moving. `#bgblur`, its
`drift` keyframes and the reduced-motion override are gone; the art is drawn
once, whole and uncropped, and the strip left over in an off-aspect window is a
plain matte (at the OSF view's 16:9 `contain` and `cover` are the same picture,
which is why this never showed in game). `#bgfx` loses the hard-coded Devilz Red
vignette for a `--accent` glow over a bottom-darker gradient, so the other eight
themes stop reading muddy. `app.js`, the `--bg-art` contract and the DLLs are
untouched; `index.html` drops the `#bgblur` div. Checked with headless Edge at
2560x1440 (fills exactly, no strip) and 1600x855 / 1920x1030 (matte, no ghost).
The game does not read this tree. It loads the copy under `B:\MO2\Starfield\mods\OSF DevilzDad's ESM Shop\SFSE\Plugins\OSFUI\views\devilzdad.osfshop\shop`, which was still the 2026-10-30 two-layer version - so the first run after the fix looked unchanged, and that is the whole reason it still looked broken. Both files were copied there (the old ones kept as `*.bak-osf-blur`) and now diff clean against source. OSF snapshots a view at startup, so the game needs a restart before the new backdrop is what you see.

Repo follow-up, same day: the first `osf/` tree was assembled from
`release\DevilzShopOSF-0.9.1-OSF`, and that package predates the fix - its
`shop.css` still had `#bgimg { background-size: 135% 135%; animation: drift }`,
which is exactly the zoom in the report. The current `app.js`, `audio.js`,
`index.html`, `shop.css` and the four 2560x1440 `space_*.jpg` files are now
synced from `OSF Shop Explorer` into the repo's `osf/` tree and back into the
0.9.1-OSF release folder; the served CSS resolves to one `contain` layer with no
animation. The release folder also carries the matching 10-31 DLL (1,804,288
bytes, sha256 `46C66AD3...F207426E`) and its README hash is updated, so that
package is coherent again rather than a new page over the old binary. Commit
`e659716` on `main`. The OSF view cache at
`%LOCALAPPDATA%\OSFUI\legacy-views-cache\mods\devilzdad.osfshop` already
matches, so only a game/OSF restart is needed to pick up the page.

Updated: 2026-10-30 (evening) — three render-tool findings, all deployed:
(a) the OSF backdrop was a 1600x900 JPEG drawn across 2560 device pixels, so it
read as blown up; the page is rasterised at the game's *output* resolution
(OSF UI's ViewSizeForOutput), the view's own art is now 2560x1440 and
`kBackdropEdge` is 2560 so a player's dropped image keeps its detail. `cover`
was still the wrong fit for a player's own picture: it crops a 16:9 image inside
a wider window and the crop reads as a zoom. The page now has two layers off one
`--bg-art` property - `#bgimg` is `contain` (the whole picture, its own aspect,
centred) and `#bgblur` is a blurred, darkened `cover` copy filling the rest, so
nothing is cropped or scaled unevenly at any window shape. The drift animation
moved to the blur layer, where it still has overflow to travel.
(b) the Misc audit (400 items through the workbench) found 45 blank tiles: mod
NIFs that carry their LOD0 geometry *inside* the file, which walk_geometries read
as a one-byte factory path. Now mirrors ParseNifGeos' printable-path test with a
second chance for the two-byte headers that read as text (KX Droid Head). Misc:
45 blank -> 0. (c) dummy-base weapons draw at last: `omod_assembly.py` binds OMODs
by the weapon's KWDA (MNAM = ma_<Weapon>), takes the standard tier, and
`prepare_item` renders the list the preview would show - Razorback is now 3 parts
/ 7 shapes / 4,763 tris instead of "no renderable geometry".
`tools/test_omod_assembly.py` pins the pick; 35 tests pass in
tools/test_render_workbench.py + renderer_recon/test_contact_sheet.py +
test_omod_assembly.py. Both DLLs rebuilt and deployed (DevilzShopOSF.dll sha256
14fa71ee..., SFShopExplorer.dll sha256 dd3fdd0d...).

(d) the 95 drawn-but-flat Misc tiles had two causes, both fixed in the offline
renderer. A material the route table and the CDB both miss still names its own
texture set in its .mat ("FileName" : "Data\\Textures\\..._Color.dds"), and the
colour maps it names are **BC7** (DXGI 99) - the numpy decoders cover BC1/3/4/5
only. `_color_from_material` reads the .mat and `decode_pillow` decodes BC7 by
rebuilding a DX10 container around the store's payload (Pillow 12 handles the
block format). Misc: 95 flat -> 52, and 48 tiles changed on the re-run. The 52
left are materials this tool cannot see at all: the base game keeps materials
inside `materials/materialsbeta.cdb` (the only file in Starfield -
Materials.ba2) and the audit's CDB index does not expose those records, plus six
mod NIFs whose material is an absolute `file:///E:/...` path that does not
exist. The game renders those items fine; it is the audit that is blind, and
fixing it means widening the CDB index, not the renderer.

Previous: 2026-10-30 — records the data defines twice now collapse to one row
with a count instead of listing the same item twice (§"Duplicate records",
the section below). Before that: the OSF page's backdrop art is the install's
own list now. Previous: 2026-10-25 — 0.9.0 and 0.9.0-OSF shipped. The item list sorts from one
key per row (0.5-1.1 ms on the numeric columns, 9,161 rows), the basket shows
weight per line and in the total, the sort column survives a restart, and
weapons that copy a vanilla gun without their own mod parts preview the vanilla
stock set instead of nothing. §0g. §0l followed: the NAME key is cached per
list instead of being folded again on every header click, the search keys no
longer rebuild on a sort, and the rebuild logs its gather and filter halves
separately. Both builds are deployed to the Test profile and packaged in
`dist\`.

Previous: 2026-10-24 — 0.8.350 gave audio its own controls: an Audio popup in
the toolbar with a volume slider and a mute checkbox each for music and sound,
plus a track picker that is independent of the visual theme. One flat `Audio\`
folder remains the whole contract - `ui_*.wav` is the six-effect sound bank and
is never a music candidate, so a dropped-in `.wav` appears in the list with no
second folder and no manifest. §3b. 0.8.349 deployed to the Test profile: the
last three Misc shelves that were still in the wrong bucket now read the NIF
sub-folder. The 22
SFBGS003 mission board terminals file under Décor while the four ammo primer
boxes beside them (Charged Diodes included) file under Tech; the seven Old Earth
PC props in `tankgirlck\xenologymesh` file under Tech instead of Creature Parts;
and the 14 `lootcontainers\Inorg*` resource containers file under Resources.
Misc / Other: 456 -> 416 (17.7% -> 16.1%). 0.8.348 fixed the BC7 opacity masks
(the Erdebrau Pils can's label cutout and the Alien Genetic Material jar's black
star), 0.8.347 gave the vial liquid the fill family's measured green instead of
the no-data grey. 0.8.346 made
the sheer-glass scan read the material's own name, so the ScienceGlass plastic
and liquid parts (vial caps, full-bottle liquid, the Vortex Ichor casing) draw
as solids instead of clear shells at alpha 0. 0.8.345 gave the artless
clear-glass family (lab beakers, flasks, cylinders, the `_Base_Green` fills)
its transmission, and
0.8.344's eight Misc shelf buckets (Décor, Lab, Creature, Tech, Slates, Food,
Artifacts, Animal) cut Misc / Other from 1,417 to 456. 0.8.343 (model-path
Misc/Aid filters, housekeeping, save/load delivery) is recorded below.

The OSF package was stale against the overlay. Its DLL (17:16) predated the
18:40 overlay build and did not contain the second-pass retire - `leftover
recheck` was in one binary and not the other - so the two 0.9.0 downloads were
not the same code. Rebuilt from the current sources at 19:47 (`DevilzShopOSF.dll`,
1,758,208 bytes, sha256 59C7F0B6...), deployed to `OSF DevilzDad's ESM Shop` and
refreshed in `release\DevilzShopOSF-0.9.0-OSF`; the OSFUI view files were
verified byte-identical against the deployed copy, so only the DLL changed. The
labels stay 0.9.0 / 0.9.0-OSF.

It happened twice: the sorter round left `OSF DevilzDad's ESM Shop` with the
20:32 DLL - the one without the Windows-1252 UTF-8 fix, so NAME and TYPE
answered `legacy handler returned invalid JSON` - and with the pre-fix `app.js`
(no remembered sort, selection rebuilding the row markup so double-click never
fired). Rebuilt at 21:12 (`DevilzShopOSF.dll`, 1,768,448 bytes, sha256
A87C7806...), deployed, and `app.js` copied; the package in
`release\DevilzShopOSF-0.9.0-OSF` carries the same two files and
`ShopExplorer.cpp` / `OsfFrontend.cpp`. Sort persistence is written twice on a
header click - by `ShopBrowser::OsfPage` when it rebuilds the order and by the
bridge's `SaveSort` - same values, so the INI converges either way.

Check a deploy by hash before asking for an in-game test: `sha256sum` on the
mod folder's DLL against `build\windows\x64\releasedbg\`, and `diff` the
`app.js` under `OSFUI\views\devilzdad.osfshop\shop\`.

### Player-supplied backdrop art on the OSF page, 2026-10-30 12:57

The custom theme's Art picker on the page listed `starfield`, `hell` and
`heaven` because those three strings were in `index.html`. The overlay's picker
had already become the install's list (0.9.1: the `space_<name>.<ext>` files
decoded beside the plugin); the page could not follow it, because OSF UI serves
a view from the snapshot it takes at startup - `PreviewHost` documents the same
limit - so a file dropped next to the page is never served, and the plugin's
assets folder is outside the view entirely.

- New `devilzdad.osfshop.backdrop`: `op:"list"` answers the names, `op:"art"`
  with a name answers the image. Art travels as a JPEG data URL in the reply,
  the same route the item previews already use.
- New `src/BackdropArt.cpp`: the `space_<name>.<png|jpg|jpeg|bmp>` scan over
  the build's own assets folder, then WIC decode - cubic scale to a 1920 long
  edge, convert to 24bpp BGR, JPEG encode. The cache is keyed by the file's
  write time, so switching themes (or dragging a colour picker, which
  re-applies the theme on every move) does not re-encode, and a replaced image
  is picked up without a restart. JPEG rather than PNG because the preview PNG of a
  photographic 1920x1080 frame is 4+ MB of base64 against the bridge's 8 MiB
  `kMaxMessageBytes`; measured 463,982 bytes / 618,667 as base64 for the
  player's own `space_dragon.jpg` and 599,351 / 799,159 for the shipped
  `space_heaven.png`.
- `app.js` resolves a backdrop in one place, in order: the view's own copy of
  the shipped art (already beside the page, no bytes over the bridge), then the
  plugin, then the shared art - so a backdrop whose file is gone degrades to
  the old look instead of to an empty background. The Art list is filled from
  the plugin when CUSTOMISE opens, and a saved name that no longer exists is
  kept as a `(missing)` option rather than silently reset on the next Save.
- The resolver's five cases were run against the real `app.js` text under
  stubs (suffix empty / shipped / dropped / missing / cached): shared art,
  the view's own file, the plugin's data URL, shared art again, and no second
  bridge call for a cached name. That harness caught a wrong function name
  (`fromPlugin` for `pluginBackdrop`) that `node --check` cannot see; it was
  deleted after the run, and `app.js` sha256 is
  `1F094327D0E840661C8A4189D1401F65FB07985562E27CCEBD6768B4AD9AA112`
  in the tree and in the deployed view.
- Verified with `tools/backdrop_art_probe` (new, WIC only, no game): 1920x1080
  JPEG and PNG sources encode, the cubic scale path is exercised at a_maxEdge
  800 (800x450 out), and every result carries a JPEG SOI and is far under the
  message limit.
- `DevilzShopOSF.dll` 13:12, 1,796,608 bytes, sha256
  `9C80DD424A06EAA2FBF0C9826C50C78D00709C2E65303654BC26F396C2F314A5`,
  built through the project's releasedbg config and deployed to
  `OSF DevilzDad's ESM Shop` with `app.js` and `index.html` copied and compared
  byte for byte. The drop folder
  `SFSE\Plugins\DevilzShopOSFAssets\` was created in that mod and the player's
  existing `space_dragon.jpg` copied in, so the first launch of this build has
  a custom backdrop to select.
- Follow-up the same day, from a launch screenshot: every OSF backdrop looked
  zoomed next to the overlay. `#bgimg` in `shop.css` carried
  `background-size: 135% 135%` - room for the `drift` pan animation - so the art
  was cropped by a third on every theme, the player's own images included. It
  is `cover` (100%) now; the drift still runs but only travels when the image's
  aspect differs from the window's, which is the only case with overflow. The
  overlay draws the same files edge to edge, so the two front ends now show the
  same picture. `shop.css` deployed and compared; no DLL change, and OSF UI
  serves the view from a snapshot, so this one needs a game restart to show.
- Not changed: the overlay build (`src/ImguiOverlay.cpp` untouched this round,
  its DLL is the 0.9.1 one), and the two builds keep separate asset folders by
  design - the page reads only `DevilzShopOSFAssets`.

### Duplicate records: the same item under two FormIDs, 2026-10-30 16:05

The user reported "duplicate items, some real some not", naming Razorback and
11mm ammo. Both rows are real: the catalog never repeated a record (0 repeated
FormIDs in `SFDevilzDump-all-forms.csv`'s 11,824 named catalog rows), so the
list was faithfully showing records the data defines twice.

- Razorback: `Starfield.esm` `00000FD6` "Razorback" (5,300 cr, 2.30) and
  `tailoredspaces.esm` `07000B0D` "Razorback" (EDID
  `SPP_TS_RazorbackTrophy`, same value and weight) - a mod's display clone of
  the master's gun.
- 11mm Caseless: `Starfield.esm` `00230679` (weight 0.000) and `002B5595`
  (weight 0.004) - the pre-ammo-weight record Bethesda left in the data beside
  the one that replaced it. The same pair pattern runs through every ballistic
  and energy round (7.77mm, .45 ACP, 1.5kv LZR, 15x25 CLL, ...).

What the catalog does now (`BuildFormList`, both front ends - the OSF page
reads the same build):

- Groups records by what a player can see: record type, folded display name,
  value, and weight. **AMMO ignores weight** in that key, because the legacy
  record and its replacement are the same box of rounds; every other type keeps
  weight, so a genuinely heavier variant stays its own row. Unnamed rows never
  group - they all share one placeholder label.
- **Only priced records group.** The first cut grouped on name and stats alone;
  `tools/audit_records.py` (below) then showed what that collapsed: 62 "Crew
  Member's Slate"s, 13 "Dying Words", 12 "Slate From Home", seven "Lab Key"s -
  identically-labelled plot objects that are different things, where hiding 61 of
  them is a worse bug than the duplicate row the feature removes. A record with
  value 0 is plot content, not merchandise, so it stays listed however it is
  named. 669 groups / 1,092 rows became 543 / 728, and the largest group went
  from 62 slates to 25 coffee mugs.
- Each group's row carries `copies`, so the overlay appends `(N records)` to the
  row and the OSF page shows a `xN` chip; `OsfDetailOf` / the overlay's item
  pane list the sibling FormIDs and their plugins, and on the page each sibling
  is a button that selects that exact record.
- `hideDuplicateNames` (Hide popup: "Hide duplicate names" / "Collapse duplicate
  names", default on) collapses each group to one row. The survivor is the
  **earliest plugin's newest record** - the master's own current definition, so
  Razorback keeps `00000FD6` (not the trophy clone) and 11mm keeps `002B5595`
  (not the weightless legacy one). The plugin rank is the top byte plus the
  middle 12 bits only for the indexed forms (0xFE light, 0xFD medium); in a
  full plugin those bits are the record id and mixing them in would have
  ordered records instead of plugins.
- The favourites view is deliberately not grouped: a star is a request, so a
  starred record cannot disappear under a base-game row of the same name.

Measured on the dump with the shipped rules: 543 groups, 728 rows collapsed
(7,661 named rows -> 6,933), 88 of the groups span plugins. The 126 groups the
price rule leaves alone are printed as a separate line by the audit tool.

- `SFShopExplorer.dll` 17:02, 2,358,272 bytes, sha256
  `7623486BB6E883ECC643ECDA21D5F0956BC64934B49D978EDA72DDE5E6F8D9A2`, built
  through `build.cmd` (BA2 audit passed) and deployed to `DevilzDad's ESM Shop
  Mod Explorer`.
- `DevilzShopOSF.dll` 17:03, 1,804,288 bytes, sha256
  `0194EA3F8E36BFDA287A9EF8D31E5D20C6137AB1CB327B67FDB1C0D300F886DA`. The OSF
  target is `set_default(false)`, so `build.cmd` does **not** build it - it
  needs `xmake build -y DevilzShopOSF`. Deployed to `OSF DevilzDad's ESM Shop`
  with `app.js` (`7CEDA943...`), `index.html` (`0725664E...`) and `shop.css`
  (`8F762576...`) copied and compared byte for byte. OSF UI serves the view
  from a startup snapshot, so the page changes need a game restart.
- `tools/test_osf_favourites.py` pinned the old `OsfSetFilter` signature; it now
  pins the new one (the parameter was appended after `favouritesOnly`) and the
  `hideDuplicates` round trip. 49 tests pass across the OSF filter, payload,
  JSON and catalog suites. `node --check app.js` clean.
- `tools/audit_duplicates.py` still reports its repeated-literal findings (33
  across 30 files, unchanged in kind); no duplicated function definitions.

#### Two tools, so the next such question is cheap

Both exist because the duplicate rule and the renderer can now be checked
without a launch. They are documented in `SF Shop Explorer/README.md`
("Offline tooling") and `tools/renderer_recon/README.md`.

- **`tools/audit_records.py`** re-derives the grouping rule from a form dump and
  prints the groups, the survivor per group, and the rows the shop will collapse
  - either kind of group labelled: `clone` (a later plugin copied an earlier
  one's item) or `same-file` (one file defines the name twice). `--type`,
  `--name`, `--plugin`, `--show`, `--out TSV`, `--json`, `--fail-over N` (a gate),
  `--all-types`, `--include-free`. This is what caught the 62-slate collapse
  before a player did. `tools/test_audit_records.py`, 9 tests, one per clause of
  the rule.
- **`tools/render_workbench.py`** keeps `contact_sheet.py` running: the archive
  index is paid once, items then redraw in ~0.37s each, and editing
  `contact_sheet.py` / `nif_uv.py` / `aid_colour_audit.py` reloads the module,
  redraws the current selection and refreshes `http://127.0.0.1:8791/`. Each
  pass keeps the previous tile per item and reports the mean pixel difference,
  so a rule change shows up as per-tile deltas with the before/after pair side
  by side. `--once` is the regression shape: `reports/workbench/{workbench.html,
  workbench.tsv, tiles/}`. Measured on this machine: 30 items (Razorback +
  Coffee Mug substrings) in 36.5s cold including the index, 5 items redrawn in
  2.3s per rule edit, and a brightness change produced deltas of 4.4-10.3 that
  went back to identical when the change was reverted.
  `tools/test_render_workbench.py`, 13 tests: selection (name/type/form/limit,
  duplicates dropped, missing form still draws a placeholder), the difference
  metric (None for a new tile, 0.0 for an identical redraw, a resized tile is not
  compared) and both page modes.
- Fixed while verifying the loop: the worker thread waited 0.5s for a redraw
  request and exited when none came, so the first pass was the only one. It now
  waits on `stop` and the request separately.
- Known gap the workbench makes obvious: the Razorback records draw nothing
  ("no renderable geometry") because `contact_sheet` draws the record's own MODL
  and a dummy-base weapon has no geometry of its own - the real gun is
  LiveModel's OMOD assembly. Wiring that assembly into the workbench is the next
  step if weapon work needs the loop.

### 0.9.0 release build, 2026-10-26 21:29

Both targets built from one tree, both deployed, both packaged:

- `SFShopExplorer.dll` 21:29, 2,291,200 bytes, sha256 `4C360299...` - overlay,
  built through `build.cmd`, whose BA2 audit ran first and passed (1 GNRL file,
  3 DX10 textures, 0 failures). Deployed to `DevilzDad's ESM Shop Mod Explorer`
  (previous copy kept as `SFShopExplorer.dll.bak-0.9.0-1840`).
- `DevilzShopOSF.dll` 21:12, 1,768,448 bytes, sha256 `A87C7806...` - OSF page;
  already in `OSF DevilzDad's ESM Shop`, unchanged by this build.
- Packages refreshed with both DLLs, all 28 `src` files and (for the OSF
  package) the view tree: `release\SFShopExplorer-0.9.0` and
  `release\DevilzShopOSF-0.9.0-OSF`. Archives written next to them:
  `SFShopExplorer 18102 0.9.0-20261026.7z` (32,391,045 bytes, sha256
  `04A03698...`) and `DevilzShopOSF 0.9.0-OSF-20261026.7z` (16,288,368 bytes,
  sha256 `666139D9...`), both listing the two DLLs at the expected paths.
- Parcel Fix is untouched by this build and already current in both places:
  `ParcelFix.dll` sha256 `20132BDD...` in `release\ParcelFix-1.0.7` and in
  `Parcel Fix` (1.0.7, the build from 20:43 that ran at 21:23).

What the new binaries add over the 18:40 overlay pair: the OSF page's column
sorter and its stored order (both DLLs contain the `leftover recheck` and
`SpikeDeliverWithMods` strings, so the purchase-time drop retire is in each).
The overlay's own behaviour is unchanged.

Nexus changelogs rewritten on request to cover only these two versions
(`NEXUS-RELEASE-0.9.0.bbcode.txt`, `NEXUS-RELEASE-0.9.0-OSF.bbcode.txt`): the
carried-over "Earlier versions" list and the "From 0.8.350-OSF" line are gone,
and the items that shipped during the day but never reached the text are in -
favourites, the Hide popup, the NAME-sort row fix on the overlay side; the six
heading sorter with its stored order, double-click adds and the Windows-1252
reply fix on the OSF side; `No more parcels` on both. BBCode tags verified
balanced ([b]/[/b] 12/12 and 14/14, one url pair each).

## STILL TO DO (user list, 2026-10-25)

Favourites (§0h) is confirmed in game - ticked off by the user on 2026-10-25 after
the star column and the Hide popup move.

Two tools had quietly drifted and are fixed: `tools/preview_transform_coverage.py`
read a literal `profiles\Test profile\loadorder.txt` that no longer exists (the
profile was renamed to `Star Wars Genesis Compiler`), so it either failed or -
on a machine where the old name still existed - measured a load order nobody
plays; it now resolves MO2's `selected_profile` from `ModOrganizer.ini`, takes
`--load-order`/`$SFSE_MO2_LOADORDER`, and prints the owner columns its own test
needed (an override moves a record out of `starfield.esm`, which is what made
`test_preview_transform_coverage.py` report four forms as missing when they were
present). `tools/test_ammo_nif_audit.py` no longer fails when the rewritten log
has no ammo preview in it; that is a skip like a missing log, not a binding
failure. Both suites pass here.

Open (2026-10-26): §0o's PgUp guard, draw watchdog and toggle edge are built for
both targets and pinned by `tools/test_open_guard.py`, and were pressed in game
on 2026-10-29 with no issue (§0u).
The Address Library report needs `sfse.txt` and the exe build from the two
reporters before anything else can be said about it. §0p's save/load duplicate
is answered as far as it can be without moving the item's delivery off a world
ref: persistence and delete do not keep such a ref out of a save, so the ref is
now *disabled* instead - not drawn, no collision, not activatable - which is
what makes a leftover invisible. Confirmed in game at 18:05 (no parcels on a
save/load), so new purchases are closed. Saves written *before* that are not
handled: §0q's load-time sweep was built, deployed twice and then removed at the
user's request before release, so this build leaves the parcels an old save
already contains exactly where they are.

Old saves now have their own answer: `Parcel Fix`, a separate plugin in
`Parcel Fix\`, now at **1.0.2** (`release\ParcelFix-1.0.2`, sha256 664E9B79...,
README beside it). It scans the player's cell once per cell per session, logs
every candidate (id, base, name, distance, flags) and hides the ones that are
runtime-created, persistent/deleted/disabled, of a type the shop sells, and
within `iRadius` (1500 units). Hidden = non-persistent + disabled, never
deleted, and reversible from the log id.

It took three builds to survive a load. 1.0.0 CTD'd with no sweep line at all;
1.0.1 added SEH guards and a line before the walk, and its log bracketed the
crash exactly: `scan start cell=000125AC radius=1500` at 19:31:00.562, no
`walk done`. That timestamp is 1.5 s after the load began - the loading screen
was still up, and `ForEachReferenceInRange` was walking a cell the game was
still streaming (it also takes the cell's read-write lock at cell+0x120). 1.0.2
gates the scan on the loading menu being closed, restarts its settle timer from
that moment, and reads the reference array directly (count at cell+0x80, array
at cell+0x88 - the offsets `SpikeWalkCell` has used in game for months) instead
of through the helper.

1.0.2 ran clean in game (19:40): `walk done - 946 slot(s), 0 out of range, 0
unreadable, 1 candidate(s)` and hid `FF02F203 base 00216F95 type=40 'Artifact
ETA'`, which was already `deleted=1`. No crash. Two problems remain, both
measured by 1.0.3: the walk took ~6 s end to end (the 19:40:00.018 -> 19:40:06.151
gap; 1.0.3 logs its own `walk done in N ms` to confirm whether that is the walk
or the logger), and the user's old-save parcels were **not** among the
candidates - one object passed the filters and it was an artifact, not a parcel.
1.0.3 therefore adds a rejection tally and `bDiagnose=1`.

The 1.0.3 diagnostic run (19:48) answered the first half: `walk done in 3845 ms -
946 slot(s), 946 in range, 0 unreadable, 1 candidate(s)` and `rejected - not
runtime-created 934, no persistent/deleted/disabled flag 7, base type not sold
3`. So only 12 refs in the cell are runtime-created at all, and of those 11 were
rejected by the flag/type rules - the parcels, if they are in this cell, are
among those 11, not among the 934. The 600-line listing could not show them: it
went in array order and filled up on planted scenery first (every one of the 600
lines read `created=0`). The walk is also genuinely slow, not a logger artifact
(1.0.3 measures it in-process): 3.8 s for 946 slots, which is the hitch right
after a load.

1.0.4 changes only the diagnostics: it lists the refs worth looking at (created,
carrying any save flag, or within 400 units of the player) instead of the first
600 in array order, counts slot faults separately, and keeps the in-process
duration. Next run should carry the parcels' ids, base forms, flags and
distances; nothing gets hidden differently until that is read.

**Answered in game, 1.0.7, 21:23 (2026-10-26).** The 1.0.4+ rules worked and the
parcels are the ones it hid: `cell 000125AC walked 950 slot(s) in 6674 ms - 15
runtime-created, 0 unreadable, 0 slot fault(s), 8 candidate(s)` and `rejected -
out of range 4, not runtime-created 935, without a save flag 0, base type not
sold 3`. All eight candidates sat 5-7 units from the player - the parcel ring
from a bought armour set, not scenery: `FF02FBF3..FF02FBF9`, bases
`FE06CA10/CA20/CA30/CA40/CA50/CA60/CA70`, type 34 (ARMO), 'Amara Boots' through
'Amara Vest', every one `persistent=0 deleted=0 disabled=0` until the sweep moved
it to z -20161 and set deleted + disabled; the eighth was the already-deleted
'Artifact ETA' activator. `8 reference(s) hidden. Save now, then the plugin can
be removed.`

That is the whole of the old-save question as far as this cell goes: the parcels
do come back as runtime-created refs (`created=1`), so the 1.0.5 relaxation of the
flag test was the right call - the old rule would have rejected all seven for
having no save flag. Nothing left to hide here, and the 3 `base type not sold`
and 4 `out of range` rejections are scenery.

**But they did not leave the screen (21:36 screenshot), and 1.0.8 answers why.**
The seven were drawn exactly where they had been, and could not be clicked - so
`disabled` and `deleted` had landed on the record and nothing had touched the
thing being rendered. A reference's 3D node is built once from the record and
keeps its own world transform; `data.location` is what the node was built *from*,
not a handle to it. That is the thread running through 1.0.5-1.0.7: 1.0.5 flagged
the record, 1.0.6 moved the record, 1.0.7 deleted the record, and the mesh never
heard about any of it. 1.0.8 adds one engine call after the same four changes -
`ref->SetScale(0.001f)` - which walks the attached node, so the box shrinks to a
fraction of a millimetre on the spot, with no cell rebuild and nothing freed. The
read-back line now ends `scale=0.001`, so the next run can tell a scale that did
not land from a save that re-created the object visible.

Built and deployed as `ParcelFix.dll` 21:35, 546,304 bytes, sha256 `4C1696EA...`
(previous kept as `ParcelFix.dll.bak-1.0.7`), packaged in
`release\ParcelFix-1.0.8` with its README, and the Nexus text updated in
`NEXUS-SUPPORT-NOTE-0.9.0-leftovers.bbcode.txt` (Parcel Fix changelog block) and
`NEXUS-RELEASE-0.9.0.bbcode.txt` (`No more parcels`). The last unproven step is
unchanged: that a save written after a sweep drops them for good.

One stale number found in the 21:46 test session, after the deploy: the log's
first line read `ParcelFix v1.0.7.0` while the second said v1.0.8 - the
commonlib plugin version comes from `xmake.lua` (`set_version` and
`COMMONLIB_PLUGIN_VERSION_*`), which was still 1.0.7/1.0.4. Both are 1.0.8 now,
rebuilt at 21:48 (sha256 `DC99BDF1...`, FileVersion 1.0.8.0) and copied into
`release\ParcelFix-1.0.8`; the mod folder still holds the 21:35 build because a
loaded DLL cannot be overwritten (`Device or resource busy`). Behaviour is
identical - only the banner string differs - so the running test is valid, and
the copy has to be redone after the game exits. Bump these two places in the
same edit as `kVersion` next time.

The clean version - an item that reaches the inventory without a world ref at
all - is investigated in `reports/inventory-attach-investigation-2026-10-26.md`:
the mod list is addressed by the extra-data list and the instance data, both of
which the inventory entry already has, so the route is plausible. The second
pass in that note adds the three facts the first one missed - `AttachMod` itself
is ref-bound (five ref-taking calls, not four), CommonLibSF has no class for the
0x33 extra (only its RTTI/vtable ids), and a record has to be created before
`store` can write to it - plus two holes in the first test plan (newest-stack
lookup, instance data never supplied by `AddObjectToContainer`).

Documentation pass (2026-10-26, pre-release): the shipped `SFShopExplorer.ini` now
explains every key it contains (26 keys, values unchanged, verified key-by-key
against the staged copy), including the ranges and enumerated values the code
clamps to. Every hand-written file under `src/` opens with a header naming what it
owns and the order of its sections, and the five largest translation units carry
section banners so the layout in the header matches the file. The two OMOD tables
are marked generated with their tool. No behaviour changed: both targets rebuilt
(same code, new debug info) and all 33 data-independent harnesses pass; the two
that fail read a game log and an empty MO2 loadorder, as before.

§0p (2026-10-26, pre-release) is the second 0.9.0 test session's three items:
render cost, purchase duplicates, and log size.

**The leftovers on the floor.** Reported as "i purchased and saved, upon loading
save previous purchases duplicates were dropped", and earlier as "a previous
launch drop common duplicates of legendary items purchased". The 2026-10-26
screenshot settled what they are: a ring of identical parcels lying around the
player after a load. They are the DROP REFERENCES from `SpikeDeliverWithMods`
(§5b): a delivery that leaves one behind leaves a second copy of the item in the
world, and because it is at the player's feet it is written into the save as one.
Three ways one was left behind, all now closed:

1. **Only one branch retired it.** The ref was disposed of inside the
   "instance add confirmed" branch alone, so a purchase confirmed by the take
   request or by activation - the fallbacks immediately below - reached the end
   of the function with its ref still standing. Every path that can confirm a
   delivery now goes through one helper, `SpikeDisposeDeliveryRef`, called from
   the instance-add branch and again after the fallbacks (`delivered` /
   `delivered-late`). `refDisposed` keeps it to one disposal per purchase, and
   the late call is skipped for a CLEAN activation, where the engine's own take
   path may already have freed the ref - the same reason the legacy move only
   runs when the activation faulted. Disposing there would be a use-after-free.
2. **`SetDelete` does not keep it out of the save.** This took two passes and the
   second test is what proved it: after the disposal fix above, a purchase left
   nothing on the floor (verified in game - one item bought and stood still for a
   minute, no parcel), and the whole ring came back on the next load. So the ref
   is created, hidden and marked correctly, but it is still written into the save
   and rebuilt on load. The reason is the persistent flag: the engine makes a ref
   persistent when the player drops it, and a persistent ref is serialised.
   `SpikeRetireRef` now clears persistence as well as flagging delete, which makes
   the ref temporary - owned by the cell, not carried by the save, discarded with
   the cell. It also parks the ref 20000 units down as a second line of defence.
   The retire line logs `persistent=0` read back from the ref, so the next log
   proves the flag actually changed rather than just the call being made.
3. **The shared-instance ref was kept in plain view.** When the ref's instance
   data is the same object the inventory entry now holds, deleting it would free
   that object, so it was deliberately kept - visible, at the player's feet. It
   is still kept (that reason has not changed) but parked the same way, with a
   WARN. The object is shared with the inventory entry, so moving the reference
   loses nothing.

Also closed: step 1 puts a PLAIN stack in the inventory so the drop has something
to move, and the drop is what removes it again. If that removal did not happen the
plain stack was still there when step 4 added the modified instance, so the player
held both a Common and the bought copy - and the old `after >= before + count`
test cannot see it, because "at least what I expect" is already true once the
plain copy is present. The count is now read once after the drop and any surplus
above `inventoryBefore` is removed before the modified instance is added.

`tools/test_rarity_delivery.py` moved its shared-instance and retire assertions to
the new helper and gained one: the delivery body must dispose through it on both
confirmation paths and must never call `SpikeRetireRef` directly.

Existing parcels in a save are not removed by any of this - they are already in
the save file. They can be picked up (each is a real copy of an item that was
bought) or removed with `markfordelete` on each in the console.

**Render cost.** `ShopExplorer::DrawImGui` called `RequestPreviewDirect` on every
frame the viewport was drawn, copying the item's whole vertex set into `direct`,
running `MaterialSwapFor` per mesh, and then content-hashing every byte of it to
build the request key - only to reach an identical key and discard the work. The
cost scales with the item's part and vertex counts, which is why a weapon built
from many OMOD assembly NIFs was the heaviest thing to select or scroll past, as
reported. The publish is now gated on a request signature (form, choice version,
skip decision, legacy preview, authored orientation, weapon, barter-menu
presence, label), so it runs when the request changes and not 60+ times a second.
`g_choiceVersion` is bumped by `Open()`, so reopening still republishes, and the
barter-menu flag is in the signature because that path re-reads the geometry from
the engine's own loaded model with form and choice unchanged. Orbit and zoom are
unaffected - they live in the renderer and are applied per frame. Frame-skipped
counts are logged at DEBUG.

**Log size.** A normal session's log was ~930 lines, of which ~916 were the
per-archive list (`914 BA2 archives indexed` is the line that matters and it is
still INFO). Moved to DEBUG: the per-archive open, the audio streaming heartbeat
(every ~600 buffers), the per-type form-array offsets, the texture read self-test,
and the OSF per-request preview framing line. The 0.9.0 default log is now a few
dozen lines; `[logging] verbose=1` restores all of it, and the shipped INI says so
in those words.

Both targets rebuilt clean (SFShopExplorer `34f85450…`, DevilzShopOSF
`149f94ac…`), 33 of 35 harnesses pass with the same two environmental failures as
before, and both DLLs are deployed to the Test profile and refreshed in the
release packages (build hash = live hash = package hash for both). Still the
user's to confirm in game: that a purchase leaves no second copy after a save and
load, and that selecting weapons is quicker.

§0m (2026-10-26) closed the two reports from the first 0.9.0 test session: the
Backspace that kept deleting until the shop was reopened, and a plugin pane that
showed 0 records for an ESL-flagged plugin (light/medium owner tables were
crossed). Both are fixed, tested, deployed and confirmed in game by the user; the
log backs the second one up with `list 7 row(s)` for AmaraArmor.esm, which is
exactly the 7 ARMO rows the catalog dump has for it.

§0i is the polish pass that followed: the item search and the visible-row list are
now derived once per change instead of every frame, and a list rebuild reads each
record's name and keywords once. Measured in game on 2026-10-26: a keystroke in the
search box costs 0.50-0.76 ms over 9,161 rows (`'al'` 2172 rows 0.67 ms, `'ale'`
132 rows 0.64 ms, `'alei'` 0 rows 0.68 ms), against 0.5-1.1 ms before for the same
box on a much smaller list.

§0l (2026-10-25) is the follow-up to the sort timings above: the NAME key is
now cached per list because it was being rebuilt for all 9,161 rows on every
header click, and the search-key cache, which had the same defect, no longer
rebuilds on a sort. The rebuild itself now logs `list phases: gather X ms,
filter Y ms` instead of one number that hid the gather half. Confirmed in game
with all five columns clicked ascending and descending (§0l): NAME 1.78/2.07 ms
(was 4.65), TYPE 0.89/0.75, VALUE 0.83/0.87, WT 0.73/0.91, FORMID 0.50/0.45, and
one 2.48 ms name-key build per list instead of one per click.

The 450 ms first keystroke that run exposed is fixed in §0n: the search key no
longer calls `FormModelPath`, so the 202 dummy-base weapons stop running their
OMOD assembly scan inside a keystroke. Read in game on 2026-10-26 on the 9,161-row
All Types list: `9161 search key(s) built in 9.12 ms`, 450.32 ms before, 49x. One
frame, once per list rebuild; the next keystroke was 0.48 ms.

§0o (2026-10-26) answers the PgUp report from the Nexus thread - "music starts,
the screen freezes, nothing shows" - and its twin "PgUp sometimes takes two
presses" (dustyroyalty, LogicSequence, kg4nbx, Tdehner all describe the first).
The cause is structural: `Open()` froze the world and started the score
unconditionally, while drawing is conditional in two other places - `SetInputEnabled`
needs `[overlay] render` 4, and `RenderOnPresent` needs the swap chain the DXGI
hook recorded. Either one failing left the shop open, unpaused, audible and
invisible, with the toggle as the only way out; the second press being reported as
"needed" is the invisible session being closed again. The toggle itself was
sampled once per tick with `GetAsyncKeyState`, so a press that began and ended
inside one long frame was never seen. Now: (a) `Open()` refuses before it touches
the pause counter when `Overlay::CanDraw()` is false, and logs renderMode and
presentHook once; (b) `FrameUpdate` closes a session that has not drawn a frame
within 3 s (`Overlay::FramesDrawn`, incremented only after `RenderDrawData`), so
the world can no longer stay frozen; (c) the window procedure records the toggle
key-down even while the shop is closed, drops auto-repeats, and the task consumes
that edge with the old poll kept as fallback. `Open()` is not the OSF entry point,
so that build is unchanged. `tools/test_open_guard.py` pins all three, 15 tests.
Owed: one in-game press on both builds, and from anyone still hitting it a log
with `presentHook=false` or `renderMode<4` in the refusal line.

§0p (2026-10-26) is the save/load duplicate after the leftover fix, and it is
honest about how far it got. The last test bought two items at 17:21 and both
logged the *same* retirement: `world ref retired code=0x0 persistent=0 parked
z=-20161`, with `delivery confirmed=1` for each. So the fix does what it claims
at the moment it runs, and the survivor is not one of the two shapes already
handled (a ring of untouched refs, or a shared instance the code refuses to
delete - that one would have logged the `shares the inventory instance` warning
and neither did). What is left is the window between the retire and the save:
the engine's own end-of-frame cell update runs after the delivery block, so it
can put a ref it still holds in the cell's persistent list back into the file
after the flags were cleared. A ref removed from the door but still listed in
the room, in other words.

So `SpikeReRetireLate` applies the two flags a second time from the delivery
verification task - a task boundary, i.e. after that cell update - and reaches
the ref through the drop's handle rather than the pointer captured at delivery,
because the first pass deleted it and the pointer may be stale. The candidate
is only touched when it still names the same base form, and the whole read is
SEH-guarded. Its line is the diagnostic:

- `persistent=0->0 deleted=1->1 disabled=1->1` - the flags held on their own and
  the second pass was a no-op.
- `persistent=1->0` - the engine re-persisted it after we cleared it, which is
  the state the pass exists for.
- no answer from the handle - says nothing either way, because the first pass
  deleted the ref. The 17:46 run disproved the reading that this meant the cell
  had discarded it; see the update below.

Built and deployed to the Test profile (`DevilzDad's ESM Shop Mod Explorer`,
DLL 17:38), `tools/test_rarity_delivery.py` 11/11. Owed: one buy-save-load in a
spot with nothing on the floor, so the count after the load starts from zero.

**Update, 17:51 - the 17:38 build ran and answered it.** That session (profile
`Star Wars Genesis Compiler`, standalone ImGui front end) bought four items at
17:46:58; every one logged `delivery confirmed=1` and then `leftover recheck
handle=... gone; nothing of this drop is left in the cell`, and the same four
parcels were on the landing pad after the load. Four purchases, four parcels:
that is the mapping, not a ring of stale refs.

The lesson is about the instrument, not the flags. `gone` was a **false
negative**: the first pass had already deleted the ref, and a deleted ref stops
answering its drop handle, so the second pass could not tell \"the cell dropped
it\" from \"the lookup went blind\". The recheck line proves nothing about
disposal and should never be read as if it does.

So `SetPersistent(false)` + `SetDelete(true)` do not keep a ref the engine has
already promoted out of the save, and no combination of those two was ever
going to. What the player actually needs is not removal but *invisibility*, and
the form flags already carry it: `kDisabled` (bit 11). `SpikeRetireRef` now sets
it alongside the other two, and the shared-instance path - which parks and
keeps its ref on purpose - sets it too. A disabled ref is not drawn, has no
collision and cannot be activated, and the flag is part of the ref record, so it
survives a save even when the record does. Worst case the cell carries an
invisible, unusable record that it eventually discards; nothing the player can
see or pick up.

Rebuilt and deployed 17:51 (same path), `tools/test_rarity_delivery.py` 11/11,
full harness sweep 33 pass / 2 fail - the same two data-dependent ones as
before (`test_ammo_nif_audit` wants a game log with an ammo preview in it,
`test_preview_transform_coverage` wants a non-empty `Test profile`
loadorder, and that profile is no longer the one in use).

Still open at the time: **saves written before 17:51 keep their parcels.** Those
refs are already in the file as enabled refs, and nothing that runs at purchase
time can reach them.

**Confirmed in game, 18:05:** a buy-free save/load produced no parcels. The
disabled flag is the fix for new drops.

§0q was the companion to §0p: a load-time sweep that walked the cell the player
stood in and disabled references that earlier builds left on the floor (§0p
explains why those refs survive a save whatever flags are cleared first). It was
built, harnessed and deployed twice, and **removed again on 2026-10-26 at the
user's request** - the release carries the purchase-time fix only.

Worth knowing if it ever comes back, because both attempts were wrong in ways
that only showed in game:

- v1 required a runtime form ID (`0xFF` high byte) as well as the deleted flag,
  and produced no line at all in a cell holding four parcels;
- v2 dropped that gate and ran, but remembered the cell on the first frame its
  pointer was non-null - before the save's references had streamed in - so it
  swept an empty list and never retried;
- with the retry fixed it did run (`refs=935 deleted=74 hidden=74`), hid 74 refs,
  and the parcels were still on screen: the disabled flag alone does not remove
  a 3D that is already placed, so a hide has to move the ref as well.

So the remaining behaviour is: **new purchases leave nothing behind, saves
written before 0.9.0 keep their parcels on the floor** and this build does not
touch them. The investigation into removing the world ref entirely (the route
that would have made the sweep unnecessary) is in
`reports/inventory-attach-investigation-2026-10-26.md`.

Support answer for the Address Library thread (AvatarZarlorne 13-17 Oct, NOMKAZ 17
Oct): the message is SFSE's address-library gate on a plugin that declares the
dependency, not our file access. This plugin sets `address_library = true` in
xmake.lua, so an ADL copy without a versionlib matching the running exe makes SFSE
refuse it - which is why 0.5.0-era installs looked fine. NOMKAZ's "every single
address library mod" is the tell that the ADL install itself is stale. Ask for
`sfse.txt` (its ADL lines predate ours) and confirm the exe build, which our own
log echoes on line 2 (`game runtime 1.16.244.0`). Remedy is a current Address
Library matching the game build, not a change here.

§0g's basket weight is confirmed in game (2026-10-26): the basket showed
`HKBlaster x1 - 4850 cr - 5.00 wt` and `BubBowLegendary x1 - 18000 cr - 3.10 wt`
with `Total: 22850 credits - 8.10 wt`, which is the two lines summed.

§0k is the NAME-sort row fix (2026-10-25), reported as "list messes up when I
click name": a star alone on a blank line with its label one row below, and that
row twice as tall as every other. The star and the label were two table cells and
a label with a control byte draws blank while measuring two lines; both are now
impossible (§0k). The same section records the sort key that makes a padded name
sort with its letters instead of its invisible prefix. The 2026-10-26 sorter run
clicked NAME ascending and descending on the 9,161-row list and nothing was
reported wrong with the rows, so the sorted list itself is confirmed; a specific
re-check of the star/label geometry was not asked for.

0.9.0 (§0g) closed the previous list: the list sort, the basket's missing
weight and the blank dummy-base weapons. The basket weight is confirmed in game
(2026-10-26): `5.00 wt` + `3.10 wt` against `Total: 22850 credits - 8.10 wt`.
Still owed: the OSF page's own weight total (only the overlay was looked at) and
the assembly rescue on the five named weapons.

Closed on 2026-10-25, §0g:

- Sort keys computed once per row (9,154 rows in 0.87 ms).
- Basket weight per line and in the total, both front ends.
- `[shop] sortColumn` / `sortAscending` persisted.
- Cross-owner MNAM rescue for dummy-base weapons with no parts of their own.

Earlier: the audio arrangement was the last 0.8.350 item and is a shipped
control set (§3b) - the beds can still be retuned by ear, but the player owns
the level, the mute and the track.

Closed on 2026-10-24:

- **Audio controls in 0.8.350** — per-channel level and mute, independent track
  picker, `ui_*` excluded from music. §3b.
- **Captain Galaxy's Note 02 cover — crossed off by the user** ("you already said
  you can't do it, it's close enough ... it looks like a similar book"). No
  further work; §1 keeps the measurements in case the cover art turns up.
- Misc shelf stragglers in 0.8.349: SFBGS003 boards vs ammo primers,
  xenologymesh PCs, lootcontainers -> Resources. Other 456 -> 416. §0f.

- Purchased modded items (weapons, armor rarity) **survive save/load** — user
  confirmed in game. The delivery route is finished; the fallback calls stay.
- Misc and Aid sub-filters: measured census, model-path rule, fixture + test.
  Aid / Other 58% -> 4%, Misc / Other 61% -> 55%, Cards 0 -> 108. §4.
- Housekeeping: live README refreshed with the deployed hash, materialswaps.tsv
  regenerated (byte-identical), the stale globe fixture fixed, version bumped to
  0.8.343. §6.
- Misc shelf buckets in 0.8.344: eight model-folder groups measured from the
  1,417 remaining Other records; Other 1,417 -> 456 without touching a name
  rule. §4a.

### 0h. Favourites in the overlay (unreleased, 2026-10-25)

Three UI changes, all in the IMGUI path.

**The five Hide filters moved into the catalog toolbar.** They lived in the
PLUGINS pane, which is where a *plugin* selection belongs; they decide what the
catalog contains, so they now sit behind a `Hide` button in the CATALOG row
(the same popup pattern as `Audio`), because five inline checkboxes wrap that
row a second time on a narrow window. Same five toggles, same rebuild-and-reset
behaviour on change.

**The star column.** `reference_form_table` gained a leading fixed 30 px `STAR`
column. *(It did not survive the 2026-10-25 NAME-sort report: the star now shares
the NAME cell with its label, so the two cannot be laid out on different rows -
see §0k.)* The row star is a `Selectable` submitted *before* the
`SpanAllColumns` row `Selectable`, which is what keeps a star click from also
selecting the row, adding to the basket or feeding the double-click add: the
first item to claim hover owns it, so the later spanning selectable never sees
the pointer. `sortColumn` keeps its 0-4 meaning - the header loop maps visual
column `c + 1` back to sort column `c` - so the `[shop] sortColumn` values
written by 0.9.0 still name the same column.

**The Favourites filter where the Hide toggles used to be.** A want-list is not
a shelf, so the filter is not a sub-filter: with it on, `BuildFormList` takes a
branch *before* the selected-plugin guard and lists every starred record across
the whole load order, ignoring the plugin list and the category combos. Ids are
re-resolved through `TESForm::LookupByID` and re-checked against
`IsCatalogRecordType`, and ids that no longer resolve are counted and logged
rather than drawn blank - a dropped plugin shows up in the log, not as an empty
row. Unstarring inside that view defers its rebuild until after `EndTable`, so
the clipper is never walking a list that changed under it.

Storage is `SFShopExplorerFavourites.txt` beside the INI, one `{:08X}` FormID
per line with a comment header: catalog content rather than a setting, and an
ini value would hit the profile-string length limit on a long list. Written on
every star.

**The star is drawn, not typed (and that was a bug report).** The first build
rendered U+2605 as the font's notdef diamond on every row, and beside
`All Plugins` and the filter label too. Measured with `GetGlyphIndicesW`
(GGI_MARK_NONEXISTING_GLYPHS) across the faces this overlay loads - segoeui,
seguisb, segoeuib, arial, seguisym - **every one reports U+2605 missing**, so no
font merge could fix it. `DrawStarGlyph` builds ten vertices at alternating
radius: filled stars go through `AddConvexPolyFilled`, hollow ones are ten
`AddLine` segments because a five-point star is concave. The row cell is an
`InvisibleButton` the width of the column (no label to mis-measure, no frame
padding to raise the row), the column header, the filter label and the item
pane title draw the same helper, and the `★` literals are gone from the source.

**The OSF page has the same stars.** New endpoint `devilzdad.osfshop.favourite`
(`{formId}` -> `{ok, favourite, ...state}`), `favourite` on every row,
`favourites` in every state answer. The page's toggle is not a client-side
filter: `OsfSetFilter` gained `favouritesOnly` and sets the engine flag, so the
plugin builds the starred list exactly as the overlay does. `OsfRefresh` clears
it with the rest of the filter, or the page would be shown favourites while its
filter state said otherwise. The page's stars are inline SVG for the same
notdef reason as above, and the star column, its click guard (`stopPropagation`,
plus a `dblclick` guard so two quick stars are not "select and basket") and the
five Hide filters behind a `Hide` popup all mirror the overlay.

Verified in the browser against the mock: 2 stars across Weapons and Armor, the
favourites view listing both with a pager of `1-2 of 2` while the category rail
sat on Weapons, unstarring inside the view leaving one row, the untick restoring
the 3-row Weapons list, a star click leaving the row unselected and the basket
empty, and a double-click on a star adding nothing.

Not done, on purpose: no version bump. `kVersion` is part of the persisted CDB
index stamp, so bumping it would force both builds to rebuild their item index
for a UI-only change; the packaged 0.9.0 archives are therefore older than the
deployed DLL. `favouritesOnly` is session state and resets to off on every open
in both front ends.

### 0i. List and search hot path (2026-10-25)

Three changes in the ImGui overlay's catalog, none of which alters a filter result.

**The item search was evaluated per frame.** `DrawImGui` rebuilt the visible-row list on
every frame, and with a term in the search box that meant, for each of 9,154 rows, a
`FormDisplayName`, a `FormModelPath`, a `std::format` for `FormIdentitySearchText`, another
`std::format` for the FormID fallback, and two `CaseFold` copies inside every
`FilterMatches` call (it folded both the row text and the needle, per call). At 60 fps that
is hundreds of thousands of allocations a second for an answer that only changes when the
text does. Now:

- `SearchKeyFor(form)` builds one folded key per row - display name, editor ID, model path,
  model file stem and the FormID in hex - and `EnsureSearchKeys()` caches the vector against
  `listStamp`.
- The visible set is cached in `visRows` (a member now, not a frame local) and rebuilt only
  when `listStamp` or the search text changes. `EnsureSearchKeys()` runs inside that gate, so
  an idle frame with a search term costs one string compare.
- `FormIdentitySearchText` and its two `std::format` calls are gone; the key covers the same
  fields, so a row that used to be found still is.

**A list rebuild did the same work several times per record.** `BuildFormList` called
`FormDisplayName` up to four times per record, rebuilt `CaseFold(ed + " " + name + " " +
FormKeywords(form, 512))` separately inside each of three Hide toggles, and re-derived
`CategorySearchText`/`CategoryModelText` for every sub-filter (All Types checks seven). One
name, one lazily-built identity fold and one lazily-built classifier text per record now
feed all of them.

**Owner lookups were per record.** `RuntimeOwnerOfFormID` returns a `std::string` and walks
`compiledFileCollection.files`; a plugin-scoped view called it for all 9,000 rows to answer
a question with at most 75 distinct answers. The loop now builds one table (compile index for
ESM/ESP, slot for `0xFD`/`0xFE` ESLs - the same mapping the function uses) and indexes it.

**Instrumentation.** Two new log lines, in the style of the existing sort timing:
`SFShopExplorer: list N row(s) from M candidate(s) in X.XX ms` after each rebuild, and
`SFShopExplorer: search '...' matched N of M row(s) in X.XX ms` when the visible set is
rederived, plus `N search key(s) built` on the one rebuild that pays for the keys.

**Tests.** `test_catalog_filters` gained
`test_item_search_and_visible_rows_are_cached_not_rebuilt_per_frame` and
`test_list_rebuild_looks_up_each_records_name_and_keywords_once`, which pin the cache gate
(and the absence of `std::format` on that path) and the once-per-record counts. The nine
source assertions that pinned the old literals now assert the hoisted call sites, with the
shared reads asserted separately - the behaviour they guard is unchanged.

**Binaries and packaging.** `SFShopExplorer.dll` 0.9.0.0, 2,279,936 bytes, SHA-256
`6EB994909F73FC80AEA2922B9D7E83DBE33B40EBBA5F025D080DA71D666873D4`; `DevilzShopOSF.dll`
0.9.0-OSF, 1,751,040 bytes, SHA-256
`3E4BDBA503C7FE12E1BDBD7DAF0959E2390947CD512D632943F746BEDFEE15AC`. Both deployed to the
Test profile (previous builds kept as `.bak-0.9.0-1841`) and both `dist` archives re-cut
from their own contents with the new DLL and refreshed `src`. Version stays 0.9.0 on
purpose: `kVersion` is part of the persisted CDB index stamp, so a bump would force every
player to rebuild the item index for a change that does not touch it.

Not measured in game yet - `xmake -r` both targets and 29 + 26 filter tests pass, but the
only honest claim so far is that the wasted work is gone, not a frame-time number.

### 0k. A star and its name are one cell (2026-10-25)

Reported as "list messes up when I click name". In the capture the top row of a
NAME sort showed a star alone with no label, "Golden Glow" on the line below,
and that row twice the height of every other row. Measured on the image (2512 px
wide): the first row band ran 273..313 while every following band was 22 px
apart, the star sat at 280..289 and the label at 298..307. The log puts the
header click at 20:09:41 (`view order 9161 row(s) by column 0 asc in 3.82 ms`)
and the capture 33 s later, so the geometry persisted - it was not a frame
caught mid-update.

**Why.** Two separate things can produce that picture, and a screenshot cannot
say which one did, so both were removed:

- The star owned a `STAR` table column and the label was the next cell, drawn
  as a `SpanAllColumns` Selectable. A cell starts at `RowPosY1 + CellPadding`,
  so the two share a line only as long as the table's row bookkeeping holds for
  both; a label 18 px below its row is what a label submitted after the star's
  17 px button looks like.
- A label carrying a control byte (a newline) measures every line but draws
  only the first: `CalcTextSize` returns two lines, `RenderText` stops at `\n`.
  That is a blank cell in a row twice as tall, and a raw-byte sort puts the
  record at the top of NAME (asc) - the same picture.

**The change.** `reference_form_table` is five columns: NAME, TYPE, VALUE, WT,
FORMID. The star is an `InvisibleButton` of a fixed 22 px slot (`kStarSlot`) at
the start of the NAME cell, `SameLine(0, kStarGap)` puts the label beside it, and
the label keeps `SpanAllColumns`. The header draws that same slot as a `Dummy`
with the glyph, `SameLine`, then the `NAME` sort button, so header and rows share
one geometry, the hit area no longer depends on `GetContentRegionAvail()`, and
the star is still submitted before the spanning selectable - which is what keeps
a star click from selecting or basketing the row. `sortColumn` keeps its 0-4
meaning, so the `[shop] sortColumn` values already written still name the same
column. The old `STAR` column is gone; a comment in §0h says so.

Names are folded once, in `FormDisplayName`: `SanitizeLabel` maps control bytes
(< 0x20 and 0x7F) to spaces for the full name and for the editor ID. One string
still feeds the label, the search keys, the sort key and the preview caption, so
a name cannot be blank or multi-line in one of them and fine in another.

**Evidence.** `tools/imgui_clip_probe/star_probe.cpp` lays the same table out
headlessly (its own xmake target) and prints, per row, the star item's Y and the
label cell's Y: equal for every row, and a row whose label holds a `\n` starts
30 px after the previous one instead of 17 - the reported shape, reproduced.
`test_catalog_filters` now pins the five-column table, the fixed slot and the
star-before-spanning-selectable order; `test_name_sort_key` gained
`test_display_name_folds_control_bytes_before_measuring`. Full suite: 412 tests,
OK.

**Binaries and packaging.** `SFShopExplorer.dll` 0.9.0.0, 2,281,984 bytes,
SHA-256 `682F0CE5DD2BDBA8F32FAA4053BE04CD973FA03A4526E9078DE82A910178017D`;
`DevilzShopOSF.dll` 0.9.0-OSF, 1,752,576 bytes, SHA-256
`C8B06C86FE804D6F7CA5F51D66B3F21E63EB2D2C3321532D9823724911429B7B`. Both are
deployed to their Test-profile mod folders (the previous pair kept as
`.bak-0.9.0-2042`), the standalone README's hash line and changelog are updated,
and both `dist` archives were re-cut from the staging trees - the DLL extracted
from each archive hashes to the value above. Version stays 0.9.0 on purpose:
`kVersion` is part of the persisted CDB index stamp.

Not seen in game yet: the honest claim is that the star and its label now come
from one cell, and that a control byte cannot blank a row.

### 0l. The sort key a click no longer rebuilds (2026-10-25)

The 21:41 session in `SFShopExplorer.log` is the measurement this started from,
all on the 9,161-row All Types list:

    view order 9161 row(s) by column 0 asc in 4.65 ms   (NAME)
    view order 9161 row(s) by column 1 asc in 1.08 ms   (TYPE)
    view order 9161 row(s) by column 2 asc in 0.95 ms   (VALUE)
    view order 9161 row(s) by column 3 asc in 0.81 ms   (WT)
    view order 9161 row(s) by column 4 asc in 0.49 ms   (FORMID)
    list 9161 row(s) from 10992 candidate(s) in 33.04 ms

NAME cost five times the numeric columns because `ApplyViewOrder` built its
text-key vector - `FormDisplayName` plus `NameSortKey` for every row - on every
call, and every header click calls it. §0g's 0.87 ms was measured before §0k
put the sanitize/fold inside `FormDisplayName`.

**The change.** The NAME keys live in `nameSortKeys`, cached against a new
`formsStamp` that only `BuildFormList` bumps. `ApplyViewOrder` bumps `listStamp`
on every click, which is why a cache keyed on that could never hit. TYPE keys
stay per sort: `RowTypeName` is a switch and the column already sorts in ~1 ms.
The sort clock now starts after the keys exist, so the existing line times the
sort and a key build logs `N name sort key(s) built in X.XX ms` on its own.

**The same defect in the search cache.** `EnsureSearchKeys` was gated on
`listStamp` too, so a header click while a term sat in the search box rebuilt
all 9,161 `SearchKeyFor` keys (display name, model path and two folds each) to
answer a question a sort does not change. Both caches now use `formsStamp`.

**The rebuild's two halves.** The runtime-array walk (`BuildFormListForType`
over seven types) ran before every timer, so "list 9,161 rows in 33.04 ms"
silently excluded it, and the total also contained the sort. The rebuild now
logs `list phases: gather X.XX ms, filter X.XX ms` before `ApplyViewOrder`. On
the 21:41 numbers the filter half is 33.04 - 5.03 = ~28 ms; gather is
unmeasured until the next session, which is the number this line exists for.

**Tests.** 413 tests, OK (412 before). `test_name_sort_key` gained
`test_name_keys_are_built_once_per_list_not_once_per_sort`, and
`test_name_column_sorts_on_the_readable_text` now pins the cached build rather
than the per-click fold; `test_catalog_filters` pins the `formsStamp` gate on
both caches, `++formsStamp` in `BuildFormList`, and the phase log line.

**Binaries and packaging.** `SFShopExplorer.dll` 0.9.0.0, 2,284,032 bytes,
SHA-256 `B0EB17C2F457512181F956E27D2751C3CB8722EC6CF5EF2B6A313E6466689EA3`;
`DevilzShopOSF.dll` 0.9.0-OSF, 1,754,624 bytes, SHA-256
`B0B4DBF29956DD990C450D42987B8B202BC51E6F99275AAC4051248231EE59AE`. Both are
deployed to their Test-profile mod folders (the previous pair kept as
`.bak-0.9.0-2126` and `.bak-0.9.0-OSF-2126`), both staging trees carry the new
DLL and `src`, both READMEs are re-hashed with the changelog bullet, and both
`dist` archives were re-cut - the DLL extracted from each archive hashes to the
value above. Version stays 0.9.0 on purpose: `kVersion` is part of the persisted
CDB index stamp.

**In-game read (2026-10-26, 14:54-14:55).** All Types, 9,161 rows, every column
clicked ascending then descending. Same session, same list:

    9161 name sort key(s) built in 2.48 ms        (and 2.38 ms after the next rebuild)
    view order 9161 row(s) by column 0 asc  in 1.78 ms   (NAME)
    view order 9161 row(s) by column 0 desc in 2.07 ms
    view order 9161 row(s) by column 1 asc  in 0.89 ms   (TYPE)
    view order 9161 row(s) by column 1 desc in 0.75 ms
    view order 9161 row(s) by column 2 asc  in 0.83 ms   (VALUE)
    view order 9161 row(s) by column 2 desc in 0.87 ms
    view order 9161 row(s) by column 3 asc  in 0.73 ms   (WT)
    view order 9161 row(s) by column 3 desc in 0.91 ms
    view order 9161 row(s) by column 4 asc  in 0.50 ms   (FORMID)
    view order 9161 row(s) by column 4 desc in 0.45 ms
    list phases: gather 1.06 ms, filter 24.21 ms
    list 9161 row(s) from 10992 candidate(s) in 28.50 ms

NAME was 4.65 ms with a key build inside the sort and 4.4-5.0 ms across the
21:41 log; it is now 1.78-2.30 ms, and the 2.48 ms key build appears once per
list rebuild instead of on every click (the 14:55 pair of NAME clicks has no
build line between them). The remaining NAME/TYPE gap is the compare itself:
NAME keys are 9,161 distinct strings, TYPE keys come from a switch with few
distinct values, so NAME resolves fewer comparisons per byte. The rebuild total
fell 33.04 -> 28.50 ms with the gather half now measured (1.06 ms).

The same run found one hitch this pass did not fix: the first search keystroke
after a list rebuild costs 450 ms. `9161 search key(s) built` then
`search 'a' matched 8922 of 9161 row(s) in 450.32 ms`; every keystroke after it
is 0.50-0.76 ms (`'al'` 2172 rows 0.67 ms, `'ale'` 132 rows 0.64 ms, `'alei'`
0 rows 0.68 ms, and the deletes back up the same way). The search keys are built
lazily on the first keystroke and are far more expensive per key than the name
keys (2.48 ms for 9,161 names vs ~450 ms for the same count of search keys,
which each resolve a model path). Open item; see STILL TO DO.

### 0m. A stuck Backspace, and a plugin that listed nothing (2026-10-26)

Two independent reports from the same session.

**Backspace stayed pressed.** Typing in the search box or the quantity field,
one Backspace deleted the text and kept deleting until the shop was reopened.
The log records the presses as single, non-repeat key-downs (`overlay key down
vk=8` at 14:24:45, :48, :48, :50, :52, :55 - none flagged `repeat`), so the
repetition was not the OS. It was ImGui's own held-key repeat: `WindowProc`
swallowed the key-up of every key whose down it had swallowed (that is what
stops the game seeing an orphan ESC up edge and opening PauseMenu), but it
swallowed that up *before* queueing it, so ImGui never cleared the key. A key
ImGui still believes is held auto-repeats, and reopening calls `ClearInputKeys`
- which is exactly why reopening fixed it.

Measured rather than argued, with `tools/renderer_recon/backspace_latch_harness.cpp`
(`build_backspace_latch_harness.bat`) against the vendored ImGui 1.92.6: a search
box with `alienzabrakhorn`, focus by a simulated click so nothing is selected, one
Backspace press, 90 frames at 1/60 s. Without the key-up all 15 characters go (the
first on the press frame, the rest from ~0.35 s onwards); with the key-up exactly
one goes. Both runs are printed by the harness.

**The fix.** The up is queued for the overlay and then swallowed for the game:
`ConsumeSwallowedKeyUp` decides, then `QueueInputMessage(WM_KEYUP, ...)` runs while
drawing and capturing, then `return 0`. The OSF page is untouched - its typing is
WebView's own.

**A plugin listed 0 records.** `0 Armor forms for AmaraArmor.esm` against
`list 0 row(s) from 4511 candidate(s)` in the same second, while All Plugins listed
the Amara set. `SFDevilzDump-catalog.csv` has the answer:
`"AmaraArmor.esm",FE06EA10,ARMO,"Amara Boots"` - an ESL-flagged `.esm`, so its forms
are `0xFE` with slot `0x06E`. `0xFE` indexes `smallFiles` and `0xFD` indexes
`mediumFiles` (SFSE `GetModByFormId` semantics; `SF Starfield MCM/src/Dump.cpp`
resolves owners for all 79k catalog rows that way), and `ownerOfFormID` had the two
tables crossed. Every light plugin was looked up in the 13-entry medium table -
out of range, no owner, 0 records - and every medium plugin showed whichever light
file happened to sit at its index, which is the "items displaying incorrectly" half
of the report. The two lookups are swapped back; the index-preserving
`push_back(... : nullptr)` in the table build is what keeps slot order intact and is
now pinned too.

**Tests.** 415 tests, OK (413 before). `test_catalog_filters` gained
`test_light_and_medium_file_owners_are_not_crossed` (which asserts `0xFE` reads
`ownerSmall`, `0xFD` reads `ownerMedium`, and not the other way round);
`test_overlay_input_recovery` gained
`test_swallowed_key_up_is_still_released_to_imgui` (the queue call sits before the
swallow).

**Binaries and packaging.** `SFShopExplorer.dll` 0.9.0.0, 2,284,032 bytes,
SHA-256 `794C331483CA4AF2FB9614278C700EE226EDA2DFAF697CEB144175123CAC5DE3`;
`DevilzShopOSF.dll` 0.9.0-OSF, 1,754,624 bytes, SHA-256
`8F75879E27D82B2C319F557A8480915619AE79716D74B1D466F2B03732FEE702`. Both built
with `xmake` (the build log was checked for compiler diagnostics, not just the exit
code), deployed to their Test-profile mod folders with the previous pair kept as
`.bak-0.9.0-1445` / `.bak-0.9.0-OSF-1445`, and the hashes were read back off the
deployed files. Both staging trees carry the new DLLs and `src`, both READMEs got
their changelog bullet (the standalone one also re-hashed), and both `dist`
archives were re-cut as `*-20261026.7z` - the DLL extracted from each archive
hashes to the value above.

**Confirmed in game by the user, 2026-10-26** - both reports closed: one Backspace
press deletes one character, and the plugin pane lists the selected plugin's
records. The log agrees on the second half: `list 7 row(s) from 10992 candidate(s)`
with `7 name sort key(s) built` at 14:53:57, against the 7 ARMO rows the catalog
dump carries for AmaraArmor.esm (`grep -c AmaraArmor.esm` -> 7, all ARMO). The
harness still only measures ImGui; the Backspace confirmation in game is the
user's, not mine.

### 0n. The 450 ms first keystroke (2026-10-26, 15:07)

§0l left one hitch: the first search keystroke after a list rebuild cost 450 ms.
The log says where it went. Immediately before `9161 search key(s) built` are the
weapon-assembly lines - `weapon quality options 42000860: ...`, `dummy weapon
'HKBlaster' OMODs: 1 owner, 241 cast, ...`, `dummy weapon 'Bowcaster_Unique'
assembly: 1 [receiver] ...`. Those are `CollectWeaponAssemblyNifs`, and
`SearchKeyFor` reached them because it asked `FormModelPath` for a model path, and
`FormModelPaths` runs the assembly scan for every dummy-base weapon (202 WEAP rows
in the catalog dump carry a dummy model). 202 cold scans inside one keystroke is
the half-second; once each is cached in `g_assemblyCache` the same loop is cheap,
which is why only one 450 ms event appears in the session.

**The change.** New `RawModelPath(form)` - the model string the record declares,
per type, with no assembly scan and no INI skip filter - and `SearchKeyFor` uses
that instead of `FormModelPath`. Everything else about the key is unchanged:
display name, editor ID, model path, model stem, FormID, all folded.

One deliberate behaviour note: a dummy-base weapon is now searchable by the NIF it
declares (the dummy model, which is the word someone troubleshooting types) rather
than by the OMOD part NIFs the preview assembles, which appear in no row. For
every other record the raw path and `FormModelPaths().front()` are the same string,
so nothing else moves.

**Instrumentation.** `N search key(s) built` became `N search key(s) built in X.XX
ms`, so the next session can read the number that used to be invisible.

**Tests.** `test_catalog_filters` gained
`test_search_keys_do_not_run_the_weapon_assembly_scan`, which pins `RawModelPath`
in the key, the absence of `FormModelPath`/`FormModelPaths` there, the absence of
`CollectWeaponAssemblyNifs`/`SkipModelRequested`/`EnsureSkipModels`/`FormModelPaths`
from `RawModelPath`, and the timed build line.

**Binaries and packaging.** `SFShopExplorer.dll` 0.9.0.0, 2,284,544 bytes, SHA-256
`5362116710F2C6B598E038A09194F2CD775027D4FFAE71995189A564D4C153D3`;
`DevilzShopOSF.dll` 0.9.0-OSF, 1,754,624 bytes, SHA-256
`E339AE929FF77B4C772A82297E6CA277B4DD0A68C9179F218DD19652ED3F042B`. Both deployed to
their Test-profile mod folders (previous pair kept as `.bak-0.9.0-1507` /
`.bak-0.9.0-OSF-1507`), hashes read back off the deployed files; both staging trees
carry the new DLLs and `src`, both READMEs got the changelog bullet (the standalone
one also re-hashed), and both `dist` archives were re-cut with the DLL hashes
verified from inside each archive.

**In-game read (2026-10-26).** Two sessions. The second one typed on the 9,161-row
All Types list, so the case that used to stall is measured directly:

    9161 search key(s) built in 9.12 ms   -> 0.996 us/key   (search 'd' 7247 rows 9.30 ms)
    273 search key(s) built in 0.32 ms    -> 1.17 us/key   (search 'd' 232 rows 0.33 ms)
     58 search key(s) built in 0.07 ms    -> 1.21 us/key   (search 'dl' 4 rows 0.07 ms)

450.32 ms -> 9.12 ms for the same 9,161 keys, 49 us/key -> 1.00 us/key, i.e. 49x.
The earlier extrapolation from the small lists said ~10.7 ms, which the direct
measurement confirms within 15%. 9.12 ms is one frame at 60 fps and it happens once
per list rebuild, not per keystroke: `'dl'` after it cost 0.48 ms. The name-key
build over the same rows is 4.00/2.63 ms (0.29-0.44 us/key), so a search key costs
~3x a name key - the three extra folds and the FormID format, as expected.

Same session's rebuild and sort numbers, for the record: `list 9161 row(s) in 43.70
ms` on the first open (filter 37.06 cold) and 27.76 ms after (`filter 23.28`),
3,399 rows 10.75 ms, 273 rows 2.12 ms, 58 rows 2.91 ms; NAME desc 2.57 ms then 1.83
ms. Only the preview's usual NIF warnings in the log, nothing on the sort, search or
visible-row path.

### 0g. 0.9.0 / 0.9.0-OSF (2026-10-25)

Four changes, one engine, two front ends.

**Sort keys.** `ApplyViewOrder` built `FormDisplayName` and case-folded it inside
the comparator: on the 9,155-row All Types list that is ~120k comparisons and
two heap strings each, per header click and per rebuild. Keys are now one per
row (`textKey` name/type; `numKey` value/weight/FormID as `double`, so a 32-bit
FormID stays exact) and the comparator only reads them. Measured in the 16:33
session on the deployed build: 9,154 rows in 0.87 ms and 0.90 ms, 3,277 in
0.29 ms, logged as `view order N row(s) by column C asc|desc in X.XX ms`.

**Basket weight.** Each line reads `xN - N cr - W.WW wt` and the total
`Total: N credits  -  W.WW wt` (weight only in free mode). The OSF page gets the
same numbers from a new `weight` field on the basket rows (`OsfBasketRow::weight`
-> `BasketJson` -> `app.js` `renderWallet`/`renderBasket`, mirrored in
`OSF Shop Explorer/tests/mock_server.py`). Verified in the mock page, not only
compiled: lines `Coachman · 1099c · 5.10 wt` and
`Deep Mining Spacesuit · 3200c · 12.40 wt`, footer `4299 credits · 17.50 wt`,
and the same page in free mode reads `Free · 17.50 wt`.

**Sort persistence.** `[shop] sortColumn` / `sortAscending`, 1-based in the file
(1 NAME .. 5 FORMID), written on every header click and restored once per
session; commented defaults document it in the shipped ini. The OSF page lists
in catalog order and has no clickable headers, so it has nothing to persist.

**Blank dummy-base weapons.** 202 records in this load order ship a placeholder
model instead of art - `BullpupDummyReceiver` (199) and `MeleeDummyHousing` (3).
Base-game ones assemble from their own OMODs, but a plugin that copies one
without model parts of its own resolved `0 family`, `0 assembly` and drew an
empty viewport. The live log names five: MagPulse (`SFFL_SE_EC03_MagPulse_Fanisha`),
Razorback (`SPP_TS_RazorbackTrophy`), Avarice, Custom Old Earth Revolver and
Arboron Novastrike Sniper. Example line:
`dummy weapon 'SPP_TS_RazorbackTrophy' OMODs: 0 owner, 240 cast, 0 slotted, 0
family (0 kw, ap 0 par 0 mnam 0 wk 17), anchor ''`.

The owner gate in `CollectWeaponAssemblyNifs` now admits a cross-owner OMOD when
its MNAM keyword - the `ma_<Weapon>` family keyword from `OmodMnamTable` - is on
the weapon (`onWeaponKeyword`, hoisted out of the OMOD loop so the gate and the
family test share one implementation). Those parts are held back by the family
gate as a last resort: own-owner parts still win, the `<150` all-parts fallback
stays own-owner only, and only a weapon that would otherwise draw nothing takes
the vanilla stock set, logging `dummy weapon '...' owns no model OMOD; assembled
from the N vanilla stock part(s) its MNAM keyword binds it to`. The keyword is
family-specific - the base-game MNAM families came out at 4-8 parts, not
everything - which is what keeps a plugin with its own art off this path.

Deployed to the Test profile and packaged:
`SFShopExplorer.dll` 0.9.0.0, 2,267,136 bytes, SHA-256
`EDF69338CFDAC64FFB6B97971D5FAFA98D11F1381BE13069D920AAB5D28AC997`
(`dist\DevilzDad's ESM Shop Mod Explorer-18102-0.9.0-20261025.7z`), and
`DevilzShopOSF.dll` 0.9.0-OSF, 1,739,776 bytes, SHA-256
`89B5349B4FA806812FD3F58F3C1BA83BF7E396DC7D25B89560865EBBE272912B`
(`dist\DevilzDad's ESM Shop OSF-0.9.0-OSF-20261025.7z`). Version comes from
four places in `xmake.lua` plus `SFE::kVersion`; both were rebuilt with
`xmake -r <target>` so the generated `commonlibsf-plugin.cpp` reads `{ 0, 9, 0 }`.
Release notes: `NEXUS-RELEASE-0.9.0.bbcode.txt` and
`NEXUS-RELEASE-0.9.0-OSF.bbcode.txt`.

The 0.8.350 binary is kept beside the DLL as `SFShopExplorer.dll.bak-0.8.350`
and the 16:33 build as `.bak-0.8.350-1633`.

### 0. Misc shelf buckets (0.8.344, 2026-10-24)

The first model pass still left 1,417 Misc records in Other because whole NIF
shelves carry no category word. Each new bucket is one measured family, and
none of them reads a name:

| Bucket | Records | Model token(s) |
| --- | --- | --- |
| Décor | 321 | `SetDressing\Office`, `\Kitchen`, `Privatelivingquarters`, `Posters`, `InteractiveHavok`, `Desktop_Items`, `Bar_Glassware`, `BarItems`, `\Neon`, `\Decoration`, `Dining_Set`, `\Lodge`, `Pegasus_HomeGarden`, `TrackerAllianceHQ`, `Mission_Board_Terminals` |
| Lab / Samples | 223 | `ScienceGlass` |
| Creature Parts | 125 | `xenologymesh` |
| Tech / Parts | 137 | `manufactured_goods`, `tech_clutter`, `tools_kit`, `prototype_tech`, `powercell`, `xtech_misc`, `AmmoPrimerBox`, `AAA_Base_Pc` (read before the xenology folder rule) |
| Data Slates | 48 | `Items\DataSlate` |
| Food / Drink | 57 | `fooddrink_`, `coffeetogo` |
| Artifacts | 48 | `SetDressing\Artifacts`, `SetDressing\Earth`, `ArmisticeDataObject` |
| Animal Parts | 28 | `AnimalGenericIngredients` |

Misc / Other: **1,417 -> 456** after 0.8.344 (55% -> 17.7%), then **416
(16.1%)** after the 0.8.349 stragglers below. The classifier stays
model-only for these eight (`test_misc_aid_filters` pins that, plus every
count), and `tools/catalog_bucket_census.py` mirrors the table so the numbers
can be re-measured after any load-order change.

Deployed: `SFShopExplorer.dll` 0.8.349.0, SHA-256
`80CF4CE27712B6F7197C0AE7964B2789E70B4A0CDFB4EF7D20B3CD28548FFAC3` (backups of
0.8.343 to 0.8.348 kept; the mod's `src\` snapshot is refreshed and the README
carries the new version, hash and change list). Note for the next
bump: `xmake.lua` declares the version in FOUR places - `set_version` plus the
three `COMMONLIB_PLUGIN_VERSION_*` configvars - and `xmake build` alone does not
recompile `build\.gens\...\commonlibsf-plugin.cpp` after a configvar change. A
plain edit there ships a DLL whose SFSE-reported version is the old one. On this
xmake (v3.0.9) the force flag is global: `xmake -r SFShopExplorer` (`xmake build
-r ...` is rejected as an invalid argument), then check
`build\.gens\SFShopExplorer\windows\x64\releasedbg\commonlibsf-plugin.cpp`
says the new version and that the hash moves. The audit-catalog dump DLL in `B:\MO2\Starfield\mods\Devild Dump`
was verified to be the current build (SHA-256 `b4f8bedb…`, identical to
`SF-Shop\SF Starfield MCM\build`), so the 17:56 catalog is from that binary and
no rebuild was needed; the mod folder now carries a README listing its outputs
and the `modelPath` column the bucket tables read.

### 0f. The last three Misc stragglers (0.8.349, 2026-10-24)

Everything below is the same shape of miss: one folder holds two things, and the
folder-level token filed both together. Each token is now the sub-folder (or the
file family), so the neighbours stay where they belong.

| Fix | Records | Before -> after |
| --- | --- | --- |
| `SetDressing\SFBGS003\Mission_Board_Terminals` (Bounty: ..., Guild Network) | 22 | Other -> Décor |
| `SetDressing\SFBGS003\AmmoPrimerBox` (both primers, Charged / Magnetic Diodes) | 4 | Other -> Tech |
| `tankgirlck\xenologymesh\AAA_Base_Pc*` (PC, Ancient PC, Monitor, Tower) | 7 | Creature -> Tech |
| `SetDressing\Outpost\lootcontainers\` (Inorg* gases, liquids, water) | 14 | Other -> Resources |

The board token is `mission_board_terminals`, not `sfbgs003`: a bare folder token
is exactly what put "Bounty: The Vulture" on the same row as "Charged Diodes"
in the first place. The PC token is read immediately before the `xenologymesh`
folder rule, in the *same* chain position as the C++ - the Python mirror carries
a `MISC_MODEL_GUARDS` entry for it so `tools/catalog_bucket_census.py` and the
fixture test agree.

Measured (2,579 shop-visible Misc records, `tools/catalog_bucket_census.py`):
Resources 186 -> 200, Décor 299 -> 321, Creature 132 -> 125, Tech 126 -> 137,
Other 456 -> 416. Nothing else moved.

The live 0.8.349 log reads the shelves a few records away from that census
(Other 423, Toys 106, Containers 101, Slates 45, Artifacts 46, total 2,577):
MISC/ALCH `keywords` are empty in the CSV dump but present on the record at
runtime, so the live classifier matches extra text the census cannot see. The
totals agree to two unnamed forms, which is the expected gap, and the four fix
groups behind 0.8.349 classify as intended in both (boards -> Décor, primers ->
Tech, PCs -> Tech bar the three Seed Vault names, Inorg containers ->
Resources).

Build 0.8.349 also carries the `SFE::kVersion` bump (it had been left at
0.8.347 through the 0.8.348 mask build, which `test_build_gate` caught):
kVersion is part of the persisted CDB index stamp, so the first run after this
build rebuilds the `.sfmodel` cache.

Known leftover, not in this change: three Seed Vault PCs (`AAA_Base_Pc`,
`AAA_Base_PcON`) still file as Flora because the name term `seed` is decided
before any model token - "Seed Vault Work PC", "Seed Vault Genetic Master
Archive", "Seed Vault Genetic Archive". Moving them is a two-line reorder if the
user wants it; they are not in the xenology folder, which is why this pass left
them.

### 0b. Clear glass with no art (0.8.345)

`Materials\Common\Glass\GlassCleanTransparent01.mat` (and its `_double`
sibling, and the ScienceGlass `..._Base_Green01/02/03` fills) are authored
**empty** sets in `materialsbeta.cdb`: no colour, no opacity, no map. The sheer
name scan then declared them clear shells at alpha 0, which is correct for a
printed can's sleeve and wrong for a beaker - a beaker IS the glass - so the
preview drew nothing. Measured on the 2026-10-24 contact sheet: tiles
`00265ADA` (Beaker) and `00265ADE` (Graduated Cylinder) were blank before the
change and both draw a vessel after it.

The rule is `IsFlatClearGlassMat` in `OfflineModel.cpp`: folder
(`/common/glass/` or `/setdressing/scienceglass/`) **and** the
`glasscleantransparent` stem, because "Glass" alone also appears inside
`ScienceGlassPlastic02`. Matched parts take the coolant jar's measured per-wall
transmission (`kAuthoredSheerAlpha` = 0.05, ~10/255 over two walls) instead of
alpha 0, and a `_Base_Green` fill keeps its database tint (0, 0.652, 0.596)
rather than being bleached to the pale glass tint. The generic token path still
returns 0.00 for every other glass-named material.

### 0c. The shelf name is not a material word (0.8.346)

`ScienceGlassPlastic01`, `ScienceGlassPlastic01_Base`, `ScienceGlassPlastic02`
and `ScienceGlassLiquid01/02` also hit the sheer token scan above - but through
the "Glass" of the **folder** (`SetDressing\ScienceGlass`), which every file on
the shelf repeats into its own name. The sheer scan read the whole path, so the
plastic caps and the vial liquid were clear shells at alpha 0: the caps
disappeared, the full bottles drew as a film over the backdrop, and the Vortex
Ichor's casing vanished entirely (01103FE2 was a black tile before the change).

The scan now reads `MaterialOwnName` in `OfflineModel.cpp` - the stem, minus a
leading repeat of its containing folder **when that folder is on
`kNamespaceFolders`** (`scienceglass`), and only when something follows the
repeat. On that shelf the prefix is a namespace (`ScienceGlassPlastic02` is the
material `Plastic02`); everywhere else a folder repeat is part of the name.
That gate is not caution for its own sake: a blanket strip was measured first
and broke `Materials\Common\Glass\GlassDirtyHeavyFrost01`, which repeats its
folder and **is** glass - the Vortex Ichor wears it and its frosted pane went
opaque. The other side of the same rule: `Exotic_Coffe_Glass\ExoticCoffe_Plastic`
was reading as glass through its folder word and is plastic.

Blast radius, measured by replaying every Misc material row of the 2026-10-24
audit (`/tmp/misc_audit_now.tsv`, 7,877 rows, 2,587 items) through both rules:
**383 items over 7 materials** - `ScienceGlassPlastic02` (219 items),
`Terrormorph_cellsample` (200), `ScienceGlassPlastic01` (10),
`ScienceGlassLiquid02` (3), `ScienceGlassPlastic01_Base` (1),
`ExoticCoffe_Plastic` (1), `Resource_Liquid_Sedative01` (1); no other material
moved. The DNA sample vials now show their tissue at the bottom
(`Terrormorph_cellsample` was a clear shell, 200 items), the caps draw in the
authored blue of `ScienceGlassPlastic02`'s CDB override (0.30, 0.58, 0.82),
`ScienceGlassPlastic01` in its neutral grey (0.70, 0.70, 0.72 - no record) and
the liquids in the same grey (empty set). Glass is untouched: the `_Base_Green`
fill stays opaque for its own measured reason (override alpha 0.196 is a
Multiply layer weight, so the layer-kind rule returns 1.0 before any token
scan). Tile check at the same framing: the Beaker (00265ADA) is pixel-identical,
the Graduated Cylinder (00265ADE) differs on 324 of 65,536 pixels - its plastic
base - and the vial / bottle / Vortex Ichor tiles differ only where the plastic
and liquid now draw.

Shelf-scale check: all **227 Lab records** re-rendered at 256 px and diffed with
`tools/renderer_recon/lab_shelf_diff.py` against the pre-0.8.345 Misc render -
**224 of 227 tiles changed, 3 identical** (two mod bottles that resolve to no
renderable mesh, one tissue sample). Against the same shelf rendered under 0.8.345 rules, **217
changed / 10 identical**, so the movers are this change and not the earlier one.
Numbers, movers and the tile sheets are in
`reports/glass-own-name-2026-10-24/` (`README.md`, both `.tsv` diffs, both
sheets, plus `lab-before-after.png` and `dna-before-after.png`).

The mirror is `own_name` in `tools/renderer_recon/aid_colour_audit.py` with
`NAMESPACE_FOLDERS`, exercised by `MaterialOwnNameTests` in
`tools/test_glass_and_wrap_blend.py` (8 checks: the five plastic/liquid
materials solid, the fills still through, `Common\Glass\GlassDirtyHeavyFrost01`
en force still clear, a non-namespace folder repeat left whole, a stem equal to
its folder kept whole, case preserved through the strip, bare stem unchanged,
and the C++ mirror's shape).

Known edges, measured and deliberately left alone: `SetDressing\SFBGS003`
(bounty boards, 22) is a display terminal shelf with `Charged Diodes` mixed in,
and `tankgirlck\xenologymesh` carries a handful of Old Earth PC props beside
the creature parts. `SetDressing\Outpost\lootcontainers\Inorg*` (14) still sits
in Other and belongs in Resources when someone wants it.

### 0d. Where a vial liquid's colour is not, and what it draws instead (0.8.347)

The liquid is `ScienceGlassLiquid01/02`, used by one mesh
(`ScienceGlass_Bottle01Full01.nif`, node `:9`) and three items (Bottle
00265AFD, Vial of Tranquilizer 0103D06E, Experimental Nutrifying Compound
01040786). Everything that could store a colour was read:

- **Material record:** `ScienceGlassLiquid01_Material1` and `..02_Material1`
  carry `0x020f` (a component link) and `0x01f4` (the name) and nothing else -
  no `0x025a` override colour/alpha, no `0x022b` layer kind. That is the field
  pair whose presence is what gives `ScienceGlassPlastic02` its blue.
- **Layer:** `ScienceGlassLiquid02_Layer1` has `0x0348` + `0x035f` and its name.
- **Texture set:** `ScienceGlassLiquid02_TextureSet1` names the kitchen
  **Scissors** prop's four maps (`Scissors_color/normal/metal`,
  `metalbarealuminumscuffed02_rough`) - the only set of 12,371 in the CDB that
  names a Scissors map, i.e. leftover authoring. Binding it would paint the
  bottle steel-grey-brown.
- **Record swap:** none of the three forms has one; `materialswaps.tsv` has no
  row for them (the lab rows it holds swap the green fill to `_Base_Green04`).

The colour that does exist nearby is the fill family
`ScienceGlass_GlassCleanTransparent01_Base_Green03` at (0, 0.6524, 0.5964) -
the green a filled vial shows, already drawn. 0.8.347 gives the liquid exactly
that colour: a `kAuthoredFlat` table at the top of `FlatBaseColor` in
`OfflineModel.cpp` (the shared build-time colour step, so the GPU build and the
CPU rasteriser both take it, and the upload's white-reset still applies if a
range ever gets an albedo). Scoped to the two liquid material names, so
`ScienceGlassPlastic01` keeps its neutral grey and `ScienceGlassPlastic02` its
authored blue. The mirror is `AUTHORED_FLAT_TINTS` / `authored_flat_tint` in
`aid_colour_audit.py`, pinned by `AuthoredLiquidTintTests` (the liquid's green,
that it equals the fill's stored tint, the two plastics unchanged, the pale
glass family unchanged, and the C++ table's keys and values). Measured on the
tiles: Bottle 00265AFD, Vial of Tranquilizer 0103D06E and Experimental
Nutrifying Compound 01040786 change; the vial control 00265B01 is pixel-exact.
Full evidence in `reports/glass-own-name-2026-10-24/README.md`.

### 0e. The masks the texture packs repack (0.8.348)

Two items came back wrong on the 2026-10-24 20:4x capture and both are the same
miss: **an opacity mask stored as BC7**.

- **Alien Genetic Material 000C1F57** drew the lid's cutout as a black star
  inside the jar - the exact defect 0.8.315 fixed for a single-channel mask.
- **Erdebrau Pils Can 001FF69A** printed its label atlas' black surround over the
  whole body: the atlas is a "Deathstick Premium Pilsener" sheet (the Star Wars
  prop replacement), its cutout is a BC7 mask, and with the mask rejected the
  decal drew with no clip at all.

Neither is a material or colour question. The log named it outright:
`opacity map rejected …erdebrau pilsner_decal_opacity.dds (fmt=98 1024x1024 vs
1024x1024)` and `range 3 … Isotopic_Coolant_Decals.mat <- slot 4 fmt=98 …
mask=yes`. Both files are BC7_UNORM_SRGB in the load order (Erdebrau from *Star
Wars - Environmental Immersion Project*, the jar lid from *Starfield HD
Overhaul*), while the base game ships them as BC4 (dxgi 80).

Measured before changing anything, with texconv on the extracted payloads: both
BC7 masks are **greyscale ink in RGB** (R=G=B, 178/183 distinct values) with
alpha at 255 - so the coverage channel is red, exactly like BC4, not BC3's
alpha. The can's mask is 88% white (a wrap), the jar's is a cutout.

Three rules changed in `OfflineModel.cpp`, and nothing else:

1. `maskFmtOk` accepts 98 (BC7) beside 80 (BC4) and 77 (BC3).
2. `range.opacityRedChannel` is true for 80 **or 98** - the measured channel.
3. The "mask-only decal" skip takes its borrowed-mask arm regardless of the
   decal's own channel count: `borrowedMask || (IsSingleChannelFormat &&
   !promotedMask)`. Before, a BC7 mask could never be recognised as another
   part's cutout because BC7 is multi-channel, which is how 0.8.315's fix came
   undone. The wine's shared-sheet case is untouched: `borrowedMask` still
   requires the *lender's* albedo to carry colour of its own.

Python needs no mirror change - the audit and contact sheet already sample the
mask whatever its format - so the offline tile for the can (`001FF69A`) remains
the reference: it prints the label with its cutout, which is now what the GPU
path does. Tests: `test_decal_mask_channels` gained
`test_repacked_bc7_mask_keeps_its_cutout`; the three assertions that pinned the
old mask-format strings were rewritten with their intent kept. Suites: tools
343 OK. `renderer_recon` 135 with **one pre-existing failure unrelated to this
change** - `test_contact_sheet.test_to_go_cup_renders_nothing_translucent`
returns "no renderable geometry" for the cup; its NIF resolves (200,611 B, no
mod overrides it) and `parse_shapes` returns 0 shapes, so that needs its own
look. The clear-glass-family test that 0.8.345 left stale was corrected while
here (flat clear glass is now 0.05 by design; frosted glass is the token
fallback control).

Build 0.8.348, `releasedbg`, SHA-256 `23FC7FA3A6B6C2F596B642E4B70DA60F1335BCF9AAC7424248757B8935A11A6B`,
deployed to `DevilzDad's ESM Shop Mod Explorer/SFSE/Plugins/`; 0.8.347 kept at
`Backups/SFShopExplorer.dll.0.8.347-<ts>`.

**User test, 2026-10-24 21:5x — signed off.** The jar's black star is gone.
The can now prints the label its texture mod supplies (the Star Wars "Deathstick
Premium Pilsener" atlas) and reads correctly; the difference from the base game
is the mod's art, not the renderer, so that item is closed as mod-related. The
Tranquilitea cup's contact-sheet failure is the same situation from the other
side (the profile's cup is a mod asset, so `parse_shapes` sees no BSGeometry) -
left alone rather than papered over.

**Decision kept from the test session:** `[offlinepreview] usecache=0` stays in
the shipped INI. Live model data reads closer to the game than the disk
`.sfmodel` cache; the cost is preview latency on first selection, which the user
accepts. Release hygiene done the same pass: the two stray backups that sat
beside the DLL and the INI (`SFShopExplorer.dll.backup-*`,
`SFShopExplorer.ini.pre-*`) moved into `SFSE\Plugins\Backups\`, so the plugin
folder now ships just the DLL, INI, assets, audio and `previewdebug.ini`.

### 1. Captain Galaxy's Note 02 — parked (measurements kept)

`Captain Galaxy's Note` (000A9010) previews as 5 meshes / 612 tris from
`SetDressing\Books\BookLarge02.nif`, and the detail panel says so. That is the
record's own model: the engine puts the same book in the world and in the
inventory, so the preview is faithful. `NativeModel.cpp` additionally applies
the record's LMSW cover swaps (`ApplyCaptainNoteMaterialSwaps`, 000A9010 /
000A9012) so the cover is the authored one, not the stock `BookA01`.

Measured 2026-10-18 after the user reported it still wrong (user chose "keep the
book, chase the cover art"):

- MODL is `SetDressing\Books\BookLarge02.nif`; no other sub-record names a NIF.
- The CDB's book materials carry no albedo at all: `GenBookB01_BookB01`
  resolves to a flat set and `GenBookB01_BookB04` has no set. The only printed
  book covers in the database belong to dedicated props
  (`booktheheavens.nif` -> `book_theheavens_color.dds`).
- The record's `MOLM` is `[0033D9F0, 0033D9F5]`. 0033D9F0 pairs
  `GenBookA01_BookA04 -> GenBookB01_BookB04` (the key names a sibling model, so
  it can never match this NIF). 0033D9F5 contains only
  `DecalLetterLineOverlay01_PatternF.mat` with **no key material at all** - an
  override-only entry - so `material_pairs()` returns nothing for it and the
  generator drops it. That is why the hardcoded
  `LinesStripes -> PatternF` swap in `ApplyCaptainNoteMaterialSwaps` still
  carries the note's pattern, and why the table alone cannot reproduce it.

Next step, now instrumented: every book/note preview logs its model and each
mesh's NIF material plus the material actually used (`SFShopExplorer book:` and
`book mesh:` lines). Compare that against the game's own inventory view for the
same item before changing anything; if a swap lands on the wrong mesh, the mesh
name in that log is the evidence.

Open design question for the cover (unchanged): the record's own art is a flat
book with a pattern decal. If that is not what the game shows, the difference is
in the pattern overlay or the layered material, not in a missing albedo.

No further action until someone has a concrete fix to try: the record ships a
flat book and a pattern decal, the CDB has no cover albedo for that material
family, and the only override in its MOLM has no key. The user's 2026-10-24
verdict was to stop unless a known fix exists.

The model-substitution route was offered and declined ("keep the book, chase
the cover art"). If it is ever revisited, it must be a named per-category table
with one log line per form (`preview model substituted: <source> -> <target>`),
never a fuzzy "closest NIF" match, and it must be possible to switch off.
`SHOP-CATALOG-REFERENCE.md` §6 has the rule this project has followed so far:
key acceptance never substitutes a similarly named NIF for a different form.

### 2. "Posters" were dataslates — bucket removed, book split is model-driven

User verdict on the 0.8.301 test: there is no poster category, and the records
that looked like posters are **dataslates** (`Paradiso Resort Advertisement` is
`Items\DataSlate\DataSlate01.nif`). The `Posters` book bucket was removed.

The real defect was the book split itself. Every printed book in the base game
carries `ObjectTypeDataSlate` in its keyword list (`Race to the Heavens`
00009A37 and `The Ancient Civilization of Egypt` 00009A3D are both
`SetDressing\Books\BookLarge01.nif` with that keyword), so a keyword rule can
never separate a slate from a book cover. `ClassifyBookSub` now takes the form's
NIF as well:

1. model contains `dataslate` -> Slates
2. model contains `notepad` / `paper` -> Notes
3. name says note/report/interview -> Notes; log/journal/communique -> Logs
4. name says slate -> Slates
5. model contains `book` -> Guides (this is what moved `Race to the Heavens`,
   `Hope Family Tree` and the SSNN interviews out of Misc / Other)
6. otherwise Misc / Other

No posters bucket, and no name-only guessing about what a record looks like.

What the swap-table work from 0.8.301 still does: `tools/gen_material_swaps.py`
reads the `MOLM` sub-record (`<u16 count><u32 LMSW ids>`) for BOOK (114
base-game records), MISC (136) and ACTI (48), so painted props get their own
art instead of the blank stock material. Table: 6521 pairs / 1834 forms.

### 3. Audio — softened/quieter/shipped, tuning still subjective

Landed: every UI one-shot regenerated with a fade-in, a 0.45-0.62 peak and a
low-pass (click centroid 8256 -> 3463 Hz, open 7057 -> 3749 Hz, denied
5930 -> 867 Hz); runtime defaults lowered to music 0.45 / SFX 0.55
(`[audio] musicVolume` / `sfxVolume` still override). Both shop beds rewritten:
`shop_music_elevator` is a 76 bpm lounge bed with no kick (25.3 s loop) and
`shop_music_dark` a 54 bpm ambient pad with no percussion (35.6 s loop), both
reverbed at -5 dB peak. The live ini now has `[shop] music=1` so the beds can
be heard; the arrangement itself is a taste call, not a measurement.

### 3b. Audio controls (0.8.350, 2026-10-24)

The 0.8.349 test log answered the first audio question before any code changed:
every 600th buffer logged `musicFade=0.00, musicPeak=0.0000` for the whole
session, including after `music track -> 'shop_music_dark.wav'` at shop open.
That was not a playback fault - the live ini had `[shop] music=0`, so `Open()`
called `StopMusic()` and the fade never left zero. The track was selected and
silent, which is exactly the failure a volume slider hides.

Confirmed in game by the user, 2026-10-24 ("works perfect").

Shipped in 0.8.350:

- Toolbar **Audio** popup: Music slider + Mute, Sound slider + Mute, Track
  combo. `ApplyAudioSettings()` runs on every drag; the INI write waits for
  `IsItemDeactivatedAfterEdit()`, so dragging is not two hundred profile
  writes.
- `[audio] musicVolume` (45), `sfxVolume` (55), `musicMuted`, `sfxMuted`,
  `musicTrack`. When `musicMuted` is absent, `[shop] music` is read and inverted
  - an update must not start playing over a profile that had it off, and this
  test profile had exactly `music=0`.
- Track choice is independent of the theme: `musicTrack` empty = "(theme
  track)" and follows `ActiveTheme(theme).music`; any other value overrides
  every theme and survives a theme switch. `Open()` sets the file, then the
  level, then starts the fade.
- `ShopAudio::ListMusicTracks()` scans `Audio\` and `Audio\Music\` for `.wav`,
  sorts case-insensitively, and returns names relative to Audio (nested tracks
  keep their `Music\` prefix). `IsMusicTrackName` is the only rule: extension
  `.wav`, and the lower-cased stem must not start with `ui_`. That is why one
  flat folder holds both banks - a hand-renamed `UI_CLICK.WAV` is still a
  sting. The list is rescanned when the popup opens, so a file copied in
  mid-session appears on the next open without a restart.
- Both beds already shipped (`audio/samples/shop_music_dark.wav`,
  `theme_starfield.wav`, generated 2026-10-18 by `audio/make_samples.py`), so no
  new audio was synthesised; the defect was the setting, not the files.
- Tests: `tools/test_audio_controls.py` (10 checks spanning the `ui_`
  exclusion, both scan folders, sorting, key persistence, migration, mute
  multiplication, open order, theme independence and popup contents).
  `test_catalog_filters` now pins the Audio popup instead of the removed Music
  checkbox.

### 4. DONE 2026-10-24 — Misc and Aid filters, measured then fixed (0.8.343)

The old classifiers were name-only. The census (`tools/catalog_bucket_census.py`,
2,579 Misc + 1,189 Aid rows with a name and a model from the 2026-10-24 catalog)
said why that failed: 61% of Misc and 58% of Aid sat in Other. MISC and ALCH
carry no keywords in the dump, so the name was the only signal - and whole
product lines do not name their category: the Sabacc deck is "Sabacc Card",
the Star Wars ores are "Lead"/"Neutronium"/"Coaxium" on an ore-chunk NIF, the
collectible globes are named Holocron, and the food families glue the word
(QuickSnack, Synthameat) or drop it (Ripe Jellyfruit, Koza).

Fix: `ClassifyMiscSub` / `ClassifyAidSub` take a second argument, the folded
`FormModelPath` (`CategoryModelText`, computed only for MISC/ALCH), and each
bucket checks measured model tokens after its name terms - the same "the model
decides" rule the Book split already uses. Model tokens: Toys
(`collectables`/`snowglobe`/`figurine`/`\toys\`/`desktoy`/`cutetoy`), Resources
(`landscape\resources`), Meds (`affliction_cures`/`bacta`/`trauma_pack`/
`panacaea`/`medstim`), Food (`fooddrink`/`xenoplantpod`/`\xenologymesh\food\`/
`coffeetogo`/`tranquilitea`/`foodanddrink`/`moonshine`/`cocktailstation`/
`foodpouch`/`foodpackets`/`itm_milk`), Chems (`\chem_`/`inhailer`/
`itm_potion`). Name terms added only where measured: `sabacc` (Cards was empty,
now 108), plural cures (`bandages`, `antibiotics`, `immobilizers`), drink and
food words (`beverage`, `ale`, `brew`, `whiskey`, `cocktail`, `heart`, `pie`,
`toast`, `baguette`, `cereal`, `seaweed`, `ichor`, `vinegar`, `alcohol`), and
`potion(s)` for chems.

Two ordering rules are load-bearing and pinned by the test: Meds before Chems
(bacta uses the chem-inhaler NIF) and the Food name term `heart` before the
Chems model token (Giant Heart is a cooking ingredient on
`crafted_inhailer_amp_01.nif`).

Measured result, same catalog: Misc = Flora 654 / Toys 120 / Cards 108 /
Containers 94 / Resources 186 / Other 1,417 (55%); Aid = Food 1,039 / Meds 51 /
Chems 53 / Other 46 (4%). Kept in Other on purpose: lab glassware, loose
clutter, display screens, power cells, and creature-part ingredients named by
body part.

Artifacts: `tools/catalog_bucket_census.py` (turns the live dump into bucket
counts and the top of each Other pile), `tools/gen_misc_aid_fixture.py` +
`tools/fixtures/misc_aid_catalog.csv` (3,768-row capture),
`tools/test_misc_aid_filters.py` (14 checks: bucket counts, the mis-filed rows,
and the C++ call sites carrying the model argument).

Triage of what is left: `reports/misc-other-triage-2026-10-24.md` ranks the
1,417 Misc / Other items by model folder. Biggest coherent pile = **Lab glass &
samples, 223 items** in one folder (`SetDressing\ScienceGlass`); a full audit of
it (`reports/misc-lab-shelf-audit-2026-10-24.tsv`) flags 226/226 records with a
flat range because `GlassCleanTransparent01.mat`, `MetalSteel02.mat` and
`ScienceGlassPlastic02.mat` have empty material sets in the CDB (neutral grey).
That pile is a materials-fallback target first, a filter second. Aid / Other 46
is 31 tankgirl creature parts, 5 ordinary food/drink misses (Koza, Mal'siir,
Spice, Hyperdrive Spirit, High Octane Fuel) and 4 mod odds and ends.

Render pass (sampled, `shelf_audit.py`, 150 items per shelf). No new defect:
Aid 52 clean / 56 flat-range / 15 untextured / 10 no-model / 8 shared-map /
2 decal-body / 1 ghost; Misc 61 clean / 48 flat-range / 18 decal-body /
16 untextured / 5 no-model / 2 shared-map. That matches the shape of the full
2026-10-23 Misc audit (786/1,482 flat-range, 248 decal-body, 193 untextured),
so the filter change did not move rendering. Data:
`reports/aid-shelf-audit-2026-10-24.tsv`, `reports/misc-shelf-audit-2026-10-24.tsv`.

### 5. DONE — rarity items reach inventory, not the floor (0.8.342 verified in save/load)

History: 0.8.338 resolved the wrong ref, 0.8.339 fixed resolution (attach and
rarity now land), 0.8.340 fixes the transfer so the item is not left on the
floor, 0.8.342 delivers the ref's own extra data list. **The user confirmed on
2026-10-24 that the purchased modded items survive save and load**, which was
the last unproven step. The engine take entries stay in place as fallbacks.

**0.8.338 failed in game, and the reason matters.** The delivery step was right
(activation), but the reference it ran against was garbage: the 15:28 log shows
the handle candidate accepted on a position-float match (`sig=0x2`) whose memory
was a string table (`53454D5C41544144` = `DATA\MES`). Every purchase then hit
`attach ... av=0xC0000005`, `mods pre ... seh=0xC0000005`, and an activation that
returned 0 and moved nothing — the real item stayed on the floor as a plain
Common. The lesson: `Misc::LookupReferenceByHandle` (ID 72986) has an unverified
signature, so its return is a guess, and a loose position match cannot validate
a reference.

0.8.339 changes the resolution, not the delivery:

1. the **cell walk runs first** (structural: `cell+0x80` count, `cell+0x88`
   array). It takes entries appended after the pre-drop snapshot, so it names
   the ref the drop just created;
2. every accepted entry must carry the dropped form as its base —
   `OBJ_REFR.objectReference` is at **ref+0x98** (`TESObjectREFR::data` at 0x80
   plus `OBJ_REFR::objectReference` at 0x18). The first version of this check
   read +0x18, which is a TESForm field and rejects every real ref;
3. the handle route remains a fallback and must pass the same test;
4. the per-pick record dumps moved behind `[shop] debugSpike=0` (they wrote
   eleven lines each), so a purchase log is readable again.

**0.8.339 result, 15:59 session.** Resolution is solved: three purchases logged
`cellwalk ... tail idx=... inst=0x...`, every pick logged `attach ... called=1
av=0x0`, and the repair ran clean. The attach and the rarity now survive. What
did not work is the delivery: `activate pickup code=0x0 playerCount=1->1
expected=2 confirmed=0` on all three, and the fallback request was the old
object+count one. The items were on the floor, rarity intact, waiting for a
manual pickup.

**0.8.340 tried a complete take request** (`SpikeTakeToInventory`): base object,
the ref's instance data (ref+0x98), its extra data list (ref+0xC8),
`inventoryIndex = -1`, count, `kTakeFromActivator`, `moveTo` = player. The 16:18
log shows it reached the right ref - `inst=1 extra=1`, `code=0x0` - and returned
`out=00000000`: `RemoveItem` found no list entry, because a loose ref is not a
container holding its own item. That closes the move-request family for good.

**0.8.341 ruled out the actor virtual.** `Actor::PickUpObject` faulted
`0xC0000005` on every purchase (`pickup stage code=0xC0000005`), so the vtable
slot commonlib models at 0x15B does not hold that function on 1.16.244. With
0.8.340's `out=00000000` and the earlier `Activate` no-op, all three engine take
entries are dead.

**0.8.342 adds the instance instead.** The dropped stack's `ExtraDataList` at
ref+0xC8 is the list the attach wrote extra 0x33 into, and
`AddObjectToContainer` accepts one: the item is added with a **refcounted copy**
of that list (`BSTSmartPointer<ExtraDataList>`), which is exactly what build 88
got wrong when it shared a raw pointer and freed the ref (the hang). The world
ref is retired with `SetDelete(true)` only when its instance-data pointer
differs from the inventory item's; a shared `TBO_InstanceData` keeps the ref
alive instead. Order is now instance add -> take request -> activate -> legacy
move, each later stage gated on the count and on `SpikeRefIsBound`.

---

### 5b. The delivery design, as built in 0.8.342

Everything that carries selected OMODs (weapon parts, quality tiers, armor
legendary effects) goes through `SpikeDeliverWithMods` in `ShopExplorer.cpp`:

1. add a clean stack to the player (`AddObjectToContainer`),
2. drop that stack at the player's feet (`RemoveItemRequest::kDropping`) to get
   a world ref — OMODs only attach to a reference,
3. attach each pick through the AttachMod impl (Address Library 40714),
4. **add the item with the ref's own extra data list** - a refcounted
   `BSTSmartPointer<ExtraDataList>` of ref+0xC8, which carries extra 0x33
   (`SpikeAddWithExtra`, 0.8.342) - then retire the ref when safe,
5. if the count says it did not land, send the complete `RemoveItemRequest`
   (0.8.340), then the ref's activate handler (0.8.338), then the object+count
   request only if activation faulted.

Why step 4: every engine take entry is a dead end here. `Activate` returns
clean and moves nothing; `RemoveItem` returns handle 0 even with instance data
and extra list supplied, because a loose ref has no inventory list; and
`PickUpObject` faults on this build. The add call is proven, and the list it
accepts is where the mods live.

Safety: the list is refcounted, so the ref may be deleted without dangling the
inventory entry (build 88's failure). The ref is only retired when the two
instance-data pointers differ. The fallback request is assembled in a flat 0x40
buffer; the queued verification reads the player, not the world, and the count -
not a void call's return - decides the outcome. `[shop] attachMods` defaults to
1; 0 still means "plain inventory add, OMODs not applied".

**Verified in game, 16:43 session (0.8.342).** Both purchases logged
`instance add ... confirmed=1`, `invDl` equal to the ref's own list,
`inventory mods=14` / `=2` reading back from the inventory entry,
`world ref retired code=0x0`, and `destination=inventory` with `+mods` in the
notice. No fallback stage ran (`take=0 activate=0 move=0`), nothing stayed on
the floor, and the game did not crash. The item-delivery work is closed.

Dead ends, all already paid for: `Actor::PickUpObject` (build 99 faulted),
aliasing a raw `ExtraDataList` into `AddObjectToContainer` (build 88
double-owned it and hung), and `RemoveItem` with `kNone` (build 100) or with
only object+count under `kTakeFromActivator` (build 101+): the request has to
carry the ref's instance data and extra list to name the stack, which is what
0.8.340 does.
Keep the drop route behind `BuildGate`, and never add a raw-address call that
was not read from the running build.

### 6. Housekeeping — DONE 2026-10-24

- Live mod folder: `README.txt` rewritten for **0.8.343** with the deployed
  SHA-256 (`52bac4b6...`) and the previous DLL kept in `SFSE\Plugins\Backups\`;
  `src\` refreshed from the repo. Retired probe files (`InventoryProbe.h`,
  `ShopMenu.*`) are left in place and called out in the README.
- `materialswaps.tsv` regenerated from the current plugins: 6,521 pairs across
  1,834 forms, **byte-identical** to the shipped table, so nothing needed
  deploying. Regenerate after any game patch or plugin change:
  `python tools/gen_material_swaps.py --data "<Starfield>\Data"` (writes
  `--out`, default `materialswaps.tsv` in the current directory; the shipped
  copy lives at `SFSE\Plugins\SFShopExplorerAssets\models\materialswaps.tsv`).
- `tools/test_globe_backdrop_shell.py` was failing because it looked up "London
  Snow Globe" by name and the Star Wars conversion deleted that record. It now
  resolves the shipped landmark globe by NIF (preferring London when present)
  and its backdrop match covers both `Snowglobe_Landmark_Backgrounds01` and
  `02`; the rule is measured on `Snowglobe_Landmark_St.nif` in this profile and
  all seven checks pass.
- Version bumped to 0.8.343 (`xmake.lua`, `ShopExplorer.h`) and deployed with
  Starfield closed.

---

Updated: 2026-10-10 — live Razorback render-path and archive-mask probe

Current status: the live probe in the v0.8.123 build read the Tombstone BC4 positive control successfully from `Starfield - Textures11.ba2` (fmt 80, 1024x1024, 524,288 bytes); both a deliberately absent key and the guessed Razorback receiver opacity sibling returned `fmt=0 0x0`. This confirms the plugin can read a packed BC4 mask and that the candidate is absent through its full configured search. In the live run, Razorback again extracted 52 meshes / 39,849 tris, prefetched 52 textures, submitted 41 ranges, and bound preview SRV slot 20; several guessed opacity siblings missed while known BC4 masks paired successfully. The user has now supplied a live screenshot showing the Razorback inside the popped-out Preview panel with `direct 3D | 52 meshes | 39849 tris | 41/41 textured`, confirming the SRV is visibly presented. Material fidelity and inventory-view parity remain unchecked: the image still looks somewhat flat, and no inventory reference is shown. Next: compare Razorback and a simple misc item with their inventory views, especially framing and cutouts. Details: [SHOP-RENDERING-NOTES.md](SF%20Shop%20Explorer/SHOP-RENDERING-NOTES.md), §6r-12 follow-up.

# What this is
`SFShopExplorer.dll` — native ImGui/DX12 shop (no OSF/WebView) in `B:\\MO2\\Starfield\\mods\\Shop Test Render\\SFSE\\Plugins\\`. Two threads: task (`ServicePending` resolve + `PrefetchTextures`) and Present (`UploadGPU → RenderGpuModel → UploadModelTextures` exact `matPath+nodeName`).


- **Offline `.sfmodel` cache** `B:\MO2\...\SFShopExplorerAssets\models\` (732 files) built by `tools/renderer_recon/build_model_cache.py:133` via `MeshConverter.dll`. Fixed: per-`(pos,uv)` expand + `V=1-t` flip (`OfflineModel.cpp:167-240`), else sampler hit wrong texels (brown slides). All 617 weapons batch-flipped and redeployed. Tag now `offline 3D — <key> — N meshes, T tris (P / Q textured)`.

- **Live build** `LiveModel.cpp:211` owns NIF 20.2.0.7 + `.mesh` LOD0 + BA2 (`BTDX` v2/v3 `LiveModel.cpp:237` `packed/size` tail-shift). v2 bulk `base=ntOff-totalBulk` verified on `SFBGS003/004/ShatteredSpace` with real zlib decodes. `usecache=0` in `[offlinepreview]` skips disk reads except just-written reload and forces NIF discovery (`LiveModel.cpp:2531` EDID-token scan, `asm_<edid>` namespaced so curated caches are never clobbered). Default `1` (curated).

- **Textures:** mod `.mat` JSON exact `Albedo.File` via `FindMatAlbedo:1932` first (`score albedo`), else fuzzy `FindDiffuse:2064` over `textures/**/*_color.dds` indexed from BA2s (now 38042, should be ~45K when MO2 discovery is fixed — see below). Family singleton for `FragMine` → `default_color`.

## NativeModel — game-native 3D preview (active R&D)

### Goal
`formID → internal model loader → NiAVObject → offscreen RTV → PREVIEW`

Use the engine's own model loading + rendering pipeline. No SFM1 cache, no `asm_*` assembly, no `FindDiffuse` fuzzy texture matching, no V-flip UV guesses. **OfflineModel is rejected by user** — too many mods, conflicts inevitable.

### Dead ends (do not retry)

**Clone3D on static TESBoundObject:**
- `TESBoundObject::Clone3D` vtable slot 0x78 = thunk `48 8b 01 45 33 c9 48 ff a0 30 03 00 00` → slot 0x66 with r9=0
- Returns non-null NiAVObject but always the **same static placeholder** — zeroed transforms, zeroed bounds, 1 node, 1 geo, 0 triangles
- Root cause: `form+0xE0` (unk0E0 / loadedData) is **always 0** for every weapon — never populated
- **InitItemImpl (vtable 0x1F) does NOT populate unk0E0** — tested, stays 0
- **Calling raw vtable slot 0x66 with guessed signature CTD'd** (`pgup ctd`) — never do guessed raw vtable calls
- **Form+0x100 model BSFixedString is always the dummy** (`Weapons\Bullpup\BullpupDummyReceiver.nif`, same pool entry `0x21cfff550e0` for ALL weapons)
- **Re-verified 2026-10-17** against `TESBoundObject` primary vtable `0x4b83448`
  (130 slots; a second 2-slot vtable `0x4b833d0` exists via another COL, so slot
  numbers must be taken against the richest one): slot `0x78` = `0xa0abc0` with
  bytes `48 8b 01 45 33 c9 48 ff a0 30 03 00 00`, thunk target `[rax+0x330]` ->
  slot `0x66` = `0xa70c90` (saves eight registers, reads `r9b`). The dead end
  stands; harness and full verdicts in
  reports/engine-claims-reverification-2026-10-17.md.

**BarterMenu_LoadModel event dispatch alone does not fix Clone3D:**
- `TryNativeVendorPreview` (ShopExplorer.cpp:443) dispatches successfully for every hovered form (source is non-null, no crash)
- But dispatch only notifies sinks — does **not** populate form's unk0E0
- Event only triggers the barter menu's own viewport loading path

**CommonLibSF TESObjectWEAP offsets are off by 8:**
- TESFullName at form+0xE8 (decl 0xE0)
- BGSModelMaterialSwap at form+0xF8 (decl 0xF0)
- `model` at form+0x100 (dummy path always)

### Native read-back — implemented 2026-10-17, awaiting a live run

With `[preview] legacyRenderer=0` the PREVIEW viewport no longer blanks itself. It
now reads the model the engine loaded for the barter preview and renders that
geometry through the existing DX12 rasterizer (`RequestPreviewDirect`); the
metadata strip labels it `engine model (barter)` so it cannot be confused with
the `direct 3D` Clone3D/NIF path. If the read-back finds nothing, the viewport
stays empty and the notice says why — it never falls back to a plugin-assembled
model.

`SFE::NativeModel::LoadFromBarterMenu` (src/NativeModel.cpp) does the work, and
it is **read-only** — no engine code is called, so there is no guessed-signature
CTD risk:

1. `REL::ID(91377)` → `0x15da450`, the `BarterMenu_LoadModel` source getter.
2. Walk that source's sink array. Layout is the SDK's own (`RE/B/BSTEvent.h`, `RE/B/BSTArray.h`): sinks at `+0x08`, and `BSTArray` is size@+0, capacity@+4, data@+8.
3. A sink whose RTTI demangles to `BarterMenu` **is** the menu's sink base, so `menu = sink - 0x150`.
4. Scan the menu's first `0x4000` bytes for 8-byte-aligned pointers whose RTTI is a `NiNode`/`BSFadeNode`/`BSModelNode` root, walk each with the existing `WalkTree`, and keep the root with the most triangles.
5. Bounds sanity (`SaneBounds`) then hands the meshes to the GPU path.

Why `0x150` is a fact and not a guess — all from the image, all re-checked by
`tools/renderer_recon/verify_barter_sink.py` (exit 0, five claims PASS):

- `BarterMenu` has 13 RTTI vtables, one per event-sink base. The sink base's vtable `0x4c808e8` (COL `0x51226e0`, TD `0x5b26208`) starts with the adjustor thunk `sub rcx, 0x150; jmp 0x15d0ea0` — that is the base-class offset, not a data member.
- The teardown at `0x15d0c71` calls the source getter `0x15da450` and unregisters `rdx = rsi + 0x150` from it, which is the LoadModel sink registration.
- Its ProcessEvent is `0x15d24f0` → deferred body `0x15d25e0` / `0x15db520`.

The open question is deliberately narrow: whether the menu holds the
**item** model as a direct `NiAVObject*` member inside the scanned window, or
only the scene root that then references it. The log names every sink, every
candidate and every tree, so one hovered item answers it.

**First live run (session 17644): the dispatch has never reached a menu.**
All 12 read-backs logged `barter source 0x… sink count=0`, and the source
pointer itself was valid. `BarterMenu` registers its LoadModel sink only while
the menu exists, and the shop opens from the global form catalog, so a
hotkey-opened shop has no barter menu and nothing can load a preview. So
`native dispatch: ok` only ever meant "Notify returned without faulting on an
empty sink list" — it was never evidence that a menu consumed the event. The
read-back now reports the sink count and `UI::IsMenuOpen("BarterMenu")`
together, so the next run distinguishes "menu closed" from "sink not found".
The native route therefore requires an open barter menu; opening the shop from
a vendor is the test, and instantiating the menu ourselves is not something
this project has done. Both caches now include `BarterMenuAvailable()`, so the
order "open shop, then open a vendor" re-dispatches the same form and re-reads
it instead of keeping the first, empty answer. The failure path prints a UI
census (`registered=… instance=… open=…`), which separates `not created` from
`created but closed` — the two have different fixes.

### Current approach: reverse the barter menu's model-load path (superseded by the read-back above)

The barter menu loads real 3D previews when you hover items — that's the proven live path. Plan:

1. **Walk `BarterMenu_LoadModel` event source sinks** (REL::ID **91377**)
   - Source layout: vtable@0, `BSTArray` sinks@+0x08 (size@+8, cap@+C, data@+10), sizeof 0x28
   - Each sink: RTTI via vtable[-1] → COL+0x0C RVA → TypeDescriptor+0x10 name
   - `ProcessEvent` at vtable slot 1 (slot 0 = dtor)
   - Follow E9/FF25 thunks, hex-dump 96 bytes, scan first 256 for LEA rip-relative strings
   - **Current build does this** via `NativeModel::DumpEventSinks()` — called after each vendor dispatch

2. **Extend PE LEA xref scanner** with targets: `.nif`, `BSModelDB`, `TESModelDB`, `NiStream`, `LoadFromNIF`, `GetModelPath`, etc.
   - Already in `ScanForStrings` targets (this build)

3. **Identify candidate functions** from sink ProcessEvent disassembly + string xrefs
4. **Call the internal loader** with an OMOD-assembled NIF path (`FormModelPaths()` / `CollectWeaponAssemblyNifs()` already produce real paths)
5. Wire resulting NiAVObject into OfflineModel GPU pipeline

### Key RTTI/VTABLE IDs (CommonLibSF)
- `BarterMenu_LoadModel::GetEventSource` REL::ID **91377**
- `BarterMenu_SetMouseOverModel` 91371, `BarterMenu_HideModel` 91376, `BarterMenu_ViewedItem` 91374
- `BSModelDB::BSModelProcessor` 399805, `TESModelDB::TESQueuedHandles` 399801, `TESModelDB::_TESProcessor` 399803
- `NiStream` vtable 498061 / RTTI 850154
- `BSResource2::DBStreamOps<BSModelDB::DBTraits>::CreateWithArgs` 400436/400434

### Confirmed form layout (TESObjectWEAP, Starfield 1.16.244)
| Offset | Field | Notes |
|--------|-------|-------|
| +0xE0 | TESFullName (name BSFixedString) | CommonLib says 0xE0 — actual name entry at +0xF0 |
| +0xE8 | (name BSFixedString) | off-by-8 |
| +0xF0 | name BSFixedString | "Razorback" etc. |
| +0xF8 | BGSModelMaterialSwap | CommonLib says 0xF0 — actual +0xF8 |
| +0x100 | model BSFixedString | ALWAYS dummy path |
| +0xE0 value | unk0E0 / loadedData | **always 0** — Clone3D root cause |
| +0xF8 value | constant code ptr `0x7FF7882387C0` | not a real component |

### Key files
- `src/NativeModel.h` + `src/NativeModel.cpp` — game-native model loader (PE scanner + sink walker + Clone3D)
- `src/ShopExplorer.cpp:443` — `TryNativeVendorPreview` dispatches LoadModel events + dumps sinks
- `src/ShopExplorer.cpp:457` — `FormModelPaths()` / `CollectWeaponAssemblyNifs()` — real OMOD NIF paths
- `src/OfflineModel.cpp` / `.h` — current GPU render pipeline (SFM1 cache, works)
- `src/ShopExplorer.cpp:1886` — calls `NativeModel::LoadFromForm(fid)` when hovering items

## How to test NativeModel
0. **Native read-back (current):** set `[preview] legacyRenderer=0` in `SFShopExplorer.ini`, hover an item, then check the log for `barter sink[0] … rtti='…BarterMenu…'`, `BarterMenu at 0x…`, `model candidate +0x… -> 0x… rtti='NiNode'`, and either `barter read-back ok: N meshes T tris root 0x…` (the viewport now shows it, labelled `engine model (barter)`) or a `barter read-back …` line naming why it found nothing.
1. Launch game via MO2 with Shop Test Render profile
2. Open any vendor (or use console to open BarterMenu)
3. Open Shop Explorer UI window (should auto-appear with vendor)
4. Hover over items in the Shop Explorer list
5. Check `C:\Users\mrlma\Documents\My Games\Starfield\SFSE\Logs\SFShopExplorer.log` for:
   - `BarterMenu_LoadModel source=` / `sinks count=` / `sink[N]: RTTI=` / `ProcessEvent =`
   - `xref:` lines for `.nif` / `BSModelDB` / `NiStream` targets

## Deployed builds
- 2026-10-09 — UI pass: theme legibility, banner layout, text input, preview backdrop. `03A26CD0CF3DCE13AF173E0E7BC7C7578EE079DB86C2F9BDE17A0A889DBCD07B` (1,878 KB). Details in `SF Shop Explorer/SHOP-RENDERING-NOTES.md` §6r-11. **Not yet visually verified.**
- 2026-09-22 — barter event sink walker + RTTI resolver + ProcessEvent hex dump + .nif LEA xrefs (current)
- 2026-09-21 — NativeModel: raw memory dump at form+0xE0 to find actual model path offset
- 2026-09-21 — NativeModel: vtable dump + RTTI scan (read-only, CTD'd when calling unknown slots)
- 2026-09-16 15:04 sweep — MO2 discovery + truncation + mag/assembly mutexes
- 2026-09-15 22:09 family-gated assembly — anchors to receiver dir, honest empty on miss
- 2026-09-15 19:22 usecache + NIF discovery

## CommonLibSF header fixes (Depends/commonlibsf-main)
- `TESObjectWEAP.h` inheritance comments corrected: TESFullName 0E0→0E8, BGSModelMaterialSwap 0F0→0F8 (based on sizeof(TESBoundObject)==0xE8). **static_assert member offsets still use original 0x238/0x248 etc — compiler layout unchanged; comments are documentation only.** Runtime dumps (form+0xF0 name, form+0x100 model) are authoritative for game 1.16.244.

## Layout gotchas
- MO2 mount: `mods\Shop Test Render\SFSE\Plugins\SFShopExplorer.dll` only copy
- **Deploy from `build\windows\x64\releasedbg\`, never `release\`.** `build.cmd` runs the `releasedbg` config, so that is the only directory xmake writes. A stale `release\` directory can survive from an older configuration and serves a frozen binary that behaves in game exactly like a no-op change — this cost a full session on 2026-10-09. `build.cmd` now prints the built DLL's SHA-256 and fails if the `releasedbg` output is missing; compare it against the deployed copy before testing.
- **`xmake` orphans `cl/xmake` after kills** — sweep before rebuild or `touch src\ShopExplorer.cpp` to force detection. Stale output directories are the same failure mode: a clean build writes somewhere other than where you are looking.
- User is on **Test profile** in MO2 (isolated, crash-allowed)
- **CommonLibSF is incomplete — dissecting exe and esm is the only way**
- **NiStream.h does NOT exist** in CommonLibSF headers — must reverse from exe
- `TESObjectWEAP.h` include causes CTD at splash — can't use typed accessor from NativeModel.cpp
- `player->inventoryList` is wrapped in `BSGuarded<...>` (needs `.get()` or `LockRead()`)
- Inventory holds `TESBoundObject*`, NOT `TESObjectREFR*` — no Clone3D on refs
- **Barter sink offset**: `menu = sink - 0x150` for `BarterMenu_LoadModel`; the sink is a base subobject of `BarterMenu` (13 sink bases at +0x10…+0x188). Re-check with `tools/renderer_recon/verify_barter_sink.py --exe <Starfield.exe>` after any game patch.
- **The game does not call `TranslateMessage`** on the path the shop is open on, so `WM_CHAR` never arrives and text input needs synthesising from `WM_KEYDOWN`. It *does* translate on other paths — hence the `g_hostTranslatesChars` latch and per-batch de-duplication in `ImguiOverlay.cpp`; without both, every keystroke doubles.

## 0n. OSF page sorter and Parcel Fix 1.0.5 (2026-10-26 20:10)

The overlay had column headers that sorted; the OSF page did not, so the two
front ends disagreed about the same catalog. The page's six headings (Name,
Shelf, Type, Value, Weight, FormID) are now the sorter, with the same meaning
the overlay gives its own: the first click on a column sorts ascending, a click
on the active column flips the direction, and the label carries the arrow.

The order cannot be built in the page: it asks for one window of rows at a
time, and sorting a hundred rows of the catalog would leave page 2 unsorted.
So `OsfPage` grew two parameters - column and direction - and keeps the sorted
index vector itself (`osfOrder`, keyed on the column, the direction and
`formsStamp`, so a page turn reuses it and only a list change or a header click
rebuilds it). Column numbers are the protocol between the page and the plugin:
0 name, 1 shelf, 2 type, 3 value, 4 weight, 5 FormID, -1 catalogue order. Name
uses `NameSortKey` for the same reason the overlay does (padded names and
punctuation prefixes otherwise sort to the top of the list); the tie-break is
FormID, so the order is total and stable.

Verified against `tests/mock_server.py`, which gained the same sort semantics so
the harness cannot disagree with the plugin: clicking Weight gives
`0,0,0.1,0.3,0.4,0.5,1.9,2,3.2,5.1,7.3,12.4` then the reverse, clicking Name
gives an alphabetical list, only one heading is gold at a time, and the page
returns to page 0 on every sort. `node --check app.js` clean; the two 404s in the
harness console are the theme backdrop probes that have no art, as before.
`tools/test_osf_sorter.py` (8 tests) pins the protocol: the six headings and
their numbers, the payload keys, the click rule, the arrow glyphs, the engine's
clamp and cache, and that the harness sorts the same way. Both xmake targets
(`SFShopExplorer` and `DevilzShopOSF`) compile against the new signature.

Parcel Fix 1.0.5 is the answer to "the parcels are still there". Two things
changed. The flag test (persistent/deleted/disabled) is gone: the 1.0.4 run in
cell `000125AC` saw the whole cell (946 slots), found eleven runtime-created
references and exactly one with a save flag - the deleted `Artifact ETA`
activator - so the test was rejecting the parcels it existed to find. The rule
is now created + base type the shop sells + within `iRadius`; the flags are
still printed per candidate, and `bFlagsOnly=1` restores the old test. The sweep
also walks the worldspace's persistent cells (`TESWorldSpace::unk0D0` /
`unk1D0`, both marked "persistent cell?" in commonlib) as well as the player's
own cell, deduplicating by reference id and naming the cell on every line -
1.0.4 only ever saw the local cell, and a reference promoted out of it would
never appear there. A worldspace cell over 60,000 slots is reported and skipped
rather than walked.

The diagnostic listing was reordered for the third time in as many builds: the
created references are now collected during the walk and written first (up to
200), then up to 200 references nearest the player. 1.0.3 and 1.0.4 filled their
line budget with planted scenery in array order, which is why four runs never
showed what the parcels actually are.

Deployed: `ParcelFix.dll` sha256 `547792B451B3657BFC60378CCBA7FA34BA5196BCBC8365B0A0850A36719A299E`
(1.0.5, `bDiagnose=1`, `bFlagsOnly=0`, `iRadius=1500`), `DevilzShopOSF.dll` sha256
`6A0034F6276F32307EA891521EFDBAD59F84C490F0498CBE9474E4DBEF19798A`, with the
three view files byte-identical to the deployed copy and to
`release\DevilzShopOSF-0.9.0-OSF`. Parcel Fix is packaged in
`release\ParcelFix-1.0.5`.

## 0o. Sorter regression, double click, Parcel Fix 1.0.6 (2026-10-26 20:45)

Three reports came out of one test session, and the log answered two of them.

**NAME and TYPE sorting failed** with `Catalog query failed: legacy handler
returned invalid JSON` in `DevilzShopOSF.log`, while VALUE, WEIGHT, SHELF and
FORMID worked. The bridge logs nothing about *what* was invalid, but only those
two columns put a Windows-1252 name on page 1: NAME starts at "Action Figure:
Fleet Admiral Fran\u00e7ois ..." and TYPE starts at the Aid shelf, whose
"Caf\u00e9 et th\u00e9 Prime" carries the same encoding. `SFDevilzDump-catalog.csv`
proves it - it is valid UTF-8 only because `Dump.cpp` converts cp1252 on the way
out (`ToUtf8`), and those six names decode to replacement characters in the raw
dump source. `Esc` in `OsfFrontend.cpp` copied every byte >= 0x20 into the JSON
string, so a cp1252 accent made the whole reply invalid UTF-8. `Esc` now
converts first (valid UTF-8 passthrough, cp1252 table for 0x80-0x9F, `push` for
0xA0-0xFF, `?` for undefined bytes) and escapes second. This was latent in every
reply - item detail, basket, plugin list - and only showed up where a cp1252
name reached a page.

**Double click did not add to the basket.** `selectItem` called `renderRows()`,
which replaces the markup of every row, so the second click of a double click
landed on a fresh node and the browser never fired `dblclick`. Selection now
repaints the highlight in place (`paintSelection()`), the same way the star
buttons already did; it also keeps the list's scroll position, which a rebuild
was destroying. Verified in the harness: a real double click on "Novalight
Pistol" now logs "Added to the basket." and the basket reads 1.

**The parcels were found and not moved.** 1.0.5's log is the first one that
names them:

    cell 000125AC 'NewAtlantisSpaceport03' walked 946 slot(s) in 2055 ms
        - 11 runtime-created, 0 unreadable, 0 slot fault(s), 8 candidate(s)
    candidate cell 000125AC ref FF02FB6B base FE06CA10 type=34 'Amara Boots'
        distance 1 persistent=0 deleted=0 disabled=0
    ... Amara Gloves, Jeans, Necklace, Sash, Shirt, Vest, and the deleted
        'Artifact ETA' activator ...
    sweep done ... 8 candidate(s), 8 hidden

Seven Amara pieces from an old purchase, all within one unit of the player, all
without a persistence flag - which is why 1.0.2-1.0.4 found nothing in this
cell - and all eight flagged by 1.0.5. They stayed on the pad because a
disabled reference stops being drawn when the cell next builds its scene, so a
flag-only run reads as a no-op. 1.0.6 parks each hidden reference 20,000 units
below the cell as well (the shop's own retire does the same), which removes it
from view immediately and keeps it away whether or not the flag is saved. Still
no `SetDelete`: a parcel whose instance the inventory holds would take that
instance with it.

Deployed: `ParcelFix.dll` 1.0.6 sha256
`0530E5F94D291CD21C6067474BC30A92EFF3776595742281ED302094A58913C2` (ini
unchanged from 1.0.5: `bDiagnose=1`, `bFlagsOnly=0`, `iRadius=1500`),
`DevilzShopOSF.dll` sha256
`0FCA526C308318DDA7FBD968511CBFF2A6FD911DA696926E2C7A4A376772229E`, `app.js`
byte-identical to the deployed copy and to `release\DevilzShopOSF-0.9.0-OSF`.
New guards: `tools/test_osf_sorter.py` (8) and `tools/test_osf_json_encoding.py`
(5), both green; `node --check app.js` clean.

### 0o addendum: Parcel Fix 1.0.7 (2026-10-26 21:10)

The next run answered the persistence question by itself. 1.0.5 hid eight refs
at 20:19; the 1.0.6 run at 20:37 found **the same seven Amara pieces with new
runtime ids** (`FF02FBD0-D6` against `FF02FB6B-71`) and `disabled=0`. So the
disabled flag 1.0.5 set was never written back, and (from the 1.0.6 run) neither
was the park. The save's own record of those references wins over anything
cleared while the cell is live.

1.0.7 therefore sets the delete bit as well: park 20,000 units down, persistent
clear, `SetDelete(true)`, disabled - the set `markfordelete` leaves, which is
the manual fix the support note already recommends. Each hidden reference now
logs twice, the second line a read-back of the live object:

    candidate cell ... ref FF02FBD0 ... at 12,-480,96 persistent=0 deleted=0 disabled=0
    hidden ref FF02FBD0 now at 12,-480,-19904 persistent=0 deleted=1 disabled=1

so the next run distinguishes "the write never happened" from "the save did not
keep it" by comparing positions. Deployment: `ParcelFix.dll` 1.0.7 sha256
`20132BDD8A0C38F73EAC07CD3F564B179BDD013E811CAF3D4C5AF3788919144D`, ini
unchanged, packaged in `release\ParcelFix-1.0.7`.

### 0p addendum: Parcel Fix 1.0.9 (2026-10-26 21:58)

1.0.8 shipped and closed the game on its first candidate. The log named the
mechanism exactly:

    [21:49:41.609] [I] ParcelFix: candidate ... ref FF02F2DB ... 'Artifact ETA'
    [21:49:41.609] [C] ParcelFix.dll
    Failed to find offset for Address Library ID!
    Invalid ID: 0
    Game Version: 1.16.244.0

The `hidden ref ...` line never appeared, so `HideRef` had returned: the park,
`SetScale`, the persistent clear, the delete bit and the disabled flag all
completed. The process died on the read-back added beside them in 1.0.8:

    const auto scale = candidate.ref->GetScale();

`GetScale()` is not a field read. `src/RE/T/TESObjectREFR.cpp` defines it as
`REL::Relocation<func_t> func{ ID::TESObjectREFR::GetScale }`, and this Address
Library has no entry for that id, so `IDDB::IDToOffset` raises `REX::FATAL_ERROR`.
A fatal error is not a structured exception, so the `__try` around the hide never
saw it - which is why the log has no "could not be hidden (fault ...)" line.

**Rule for Parcel Fix: field writes and virtual calls only.** Any RE method that
is a `REL::Relocation` stub turns a cosmetic fix into a hard dependency on the
player's exact Address Library. 1.0.9 drops `GetScale()` - the scale is not read
back any more, `setscale` on the id in the log still restores it - and reports
`shrunk=1/0` from its own state instead. The node guard on `SetScale` is
`HasAttached3D` (`loadedData.LockRead()` -> `data3D`) only: refs a previous run
already flagged deleted are exactly the ones still drawn in an existing save, so
gating on `IsDeleted()` would skip the boxes the user is reporting.

Deployed: `ParcelFix.dll` 1.0.9 sha256
`6A4CD168800E2AB26623EC4C7545FCA4DBF8024B648C4028109AD6C91DDC0117`, ini
unchanged (`bDiagnose=1`, `bFlagsOnly=0`, `iRadius=1500`), packaged in
`release\ParcelFix-1.0.9`, built with `xmake build ParcelFix` (releasedbg,
2.3 s). Verified in the deployed copy: contains `shrunk=`, no `scale={:.3f}`,
version string `1.0.9`.

### 0q confirmed: Parcel Fix 1.0.9 clears them (2026-10-26 22:05)

User test: **"works there gone"**. The leftovers that 1.0.5-1.0.8 left drawn on
the pad are off the screen after one run of 1.0.9, including `FF02F2DB 'Artifact
ETA'` - the already-deleted reference whose read-back had crashed 1.0.8 on the
same save.

So the two things this tool needed are now both proven in game, not inferred:

1. The record writes (park, persistent clear, delete bit, disabled) are what stop
   the next load re-creating the parcel. Unchanged since 1.0.7.
2. `SetScale(0.001f)` on the reference is what takes the existing mesh off the
   screen, and it works on a ref that is already flagged deleted as long as it
   still has a 3D node - which is why the guard is `HasAttached3D` and not
   `IsDeleted`.

1.0.8 remains in `release\` and must stay a dead version: it closed the game.
Documents corrected to match: `NEXUS-SUPPORT-NOTE-0.9.0-leftovers.bbcode.txt`
now says **use 1.0.9 or later, not 1.0.8**, and describes the boxes shrinking as
it runs rather than "hides the leftovers".

### 0r public release: Parcel Fix 1.0.0 (2026-10-26 22:15)

The download is being published as **1.0.0** - its first public version - so the
test builds that carried 1.0.x numbers are no longer in the release folder:

    release\ParcelFix-1.0.0                       <- the public package
    release\_archive\ParcelFix-dev-1.0.0-flagsonly
    release\_archive\ParcelFix-dev-1.0.1 ... -1.0.9

`kVersion`, `set_version` and `COMMONLIB_PLUGIN_VERSION_PATCH` are 1.0.0; the
source header now says the 1.0.0-1.0.9 in its history are test builds and that
public 1.0.0 is behaviourally test build 1.0.9 (the one confirmed in game).

Deployed and packaged: `ParcelFix.dll` sha256
`3E25880EE1C5E933C4BE7199C25838D9521FA01304DF6F8CEBDDEDB3A8606172`, same binary
in `B:\MO2\Starfield\mods\Parcel Fix\SFSE\Plugins\` and in the release folder
(546,304 bytes). Binary checks: contains `shrunk=`, no `scale={:.3f}`, no `1.0.9`
string, one `1.0.0`.

`README.txt` rewritten to the four things asked for and nothing else: what it
does, how to use it, the save warning (back up first, do not overwrite your only
good save, restore and report with the log if a save will not load, parcels are
hidden not deleted so they can be put back), and disable (`bEnabled=0`) or remove
afterwards - with the note to take it off before a new playthrough.

### 0s upload artifact: Parcel Fix 1.0.0 (2026-10-26 22:17)

    release\ParcelFix 1.0.0-20261026.7z      167,697 bytes
    sha256 D796AFB2537B46B4908B4BF2FA3920F3F5DCAC230E01C49C9AC23520D1AADFB6

7-Zip 24.x (`C:\Program Files\7-Zip\7z.exe`), `-t7z -mx=9 -m0=lzma2 -ms=on`,
3 files / 2 folders, files at the archive root with no wrapper folder - the same
layout as `DevilzShopOSF 0.9.0-OSF-20261026.7z` and the SFShopExplorer archives,
so MO2/Vortex drop straight into the mods folder.

Contents (sha256 of each, verified by extracting and hashing against the package):

    README.txt                  B901664AAF59E3FB662126801ACE7BA6557A88FE3DA579CB13F516E0A24F680E
    SFSE\Plugins\ParcelFix.ini  E17D101D0FC42EFDFFB37AF0B330DB2D189E4D3369C6579040096210796FF5A7
    SFSE\Plugins\ParcelFix.dll  3E25880EE1C5E933C4BE7199C25838D9521FA01304DF6F8CEBDDEDB3A8606172

The dll is the build confirmed in game and the one deployed to
`B:\MO2\Starfield\mods\Parcel Fix`. A 7z created in the package folder earlier
(`ParcelFix-1.0.0\ParcelFix-1.0.0.7z`) held the identical three files; it is moved
to `release\_archive\ParcelFix-1.0.0-userbuilt.7z` so the package folder no longer
contains an archive of itself. `7z t` on the upload archive: "Everything is Ok".

### 0t published state (2026-10-26 22:2x) - repo matches Nexus

Three files live on the page, all built from this workspace. Nexus reports sizes
in MiB, and each matches its local archive byte for byte, so what is downloadable
is what is in `release\`.

    Nexus section   file                        version  Nexus size  local bytes
    Main files      SFShopExplorer 0.9.0       0.9.0    30.9 MB     32,391,045
    Main files      DevilzShopOSF 0.9.0 OSF    0.9.0    15.5 MB     16,288,368
    Optional files  ParcelFix 1.0.0            1.0.0    164 KB         167,697

    sha256
    SFShopExplorer 18102 0.9.0-20261026.7z
      04A036982992D826275997E2F959D91A215B86D004A98CF903A270D7887A0B13
    DevilzShopOSF 0.9.0-OSF-20261026.7z
      666139D9404D5958A6E26A7A94DDE341700DAB1A8614023EF5781E32F56C1F98
    ParcelFix 1.0.0-20261026.7z
      D796AFB2537B46B4908B4BF2FA3920F3F5DCAC230E01C49C9AC23520D1AADFB6

Upload times on Nexus: the two 0.9.0 files 26 Oct 2026 21:36, ParcelFix 22:21.

File-level descriptions as published:

* SFShopExplorer 0.9.0 - "IMGUI Version pgup to launch. requirements Address
  Library and SFSE only use this or OSF Version not both"
* DevilzShopOSF 0.9.0 OSF - same shape, with the OSF UI link (mod 17711), OSF
  Settings and WebView2 covered by the page's Requirements section.
* ParcelFix 1.0.0 - short blurb only (the field truncates at 255 characters):
  "Only for saves from builds before 0.9.0 that still have parcels on the ground.
  Load the save and stand near them: Parcel Fix hides each leftover as it runs.
  Nothing leaves your inventory. Then save and disable the fix." It says *hides*,
  not deletes - the plugin flags and shrinks the reference, it does not purge the
  record, and every id stays recoverable from ParcelFix.log.

Description page: both the 0.9.0 and 0.9.0-OSF changelog sections now carry the
same parcels line pointing at "[b]Parcel Fix[/b], the separate download in
Optional files" - the earlier "(is in optional)" / "( Will is in optional)"
fragments are gone.

`release\` still carries the earlier shop builds and their unpacked folders
(0.8.300, 0.8.301, 0.8.350, 0.9.0-20261025) - published history, left alone.
Parcel Fix is the only entry that was renumbered: `ParcelFix-1.0.0\` is the
package, and the dev builds it supersedes are in
`_archive\ParcelFix-dev-1.0.0 ... -1.0.9\`.


### 0u. Test pass 2026-10-29: sound, ImGui guard, Parcel Fix, Absolute Control

All four open threads tested in game; every one passed.

1. **UI sounds** - click, hover, select and denied are all audible, alongside the
   plugin's own open/close stings. This closes the transport bug from the
   previous pass: `devilzdad.osfshop.audio` is a `RegisterRequest`, so the page
   must use `call()` and not `send()` (which routes to `RegisterCommand`). The
   one-shot `sfx '<name>' got no answer` line stayed silent, which is the other
   half of the proof - the request reached the handler, so playback was never
   the problem.
2. **ImGui PgUp guard / draw watchdog / toggle edge** - pressed in game between
   two OSF launches, no issue. §0o is now proven on both targets instead of only
   pinned by `tools/test_open_guard.py`.
3. **Parcel Fix** - works as intended on the old save; §0q's answer holds.
4. **Absolute Control** - 0.3.0.0 loads clean, every test session issue-free.

Absolute Control's install was audited while confirming, because "no issues" is
only worth something if the module is actually running and matched to the build:

    install shape
      mods\Absolute Control (all in one flight control and menu suite)
        Interface\AbsoluteControlPanelMenu.swf
        SFSE\Plugins\AbsoluteControlPanel.dll
        SFSE\Plugins\AbsoluteControlPanel.ini
        meta.ini
      one mod folder only, no second copy, nothing loose in Data\Interface or
      Data\SFSE\Plugins - so nothing shadows or duplicates it
      both enabled in profile "Star Wars Genesis Compiler": Absolute Control at
      modlist line 25, Address Library at 537, of 427 enabled mods

    requirements (its own meta.ini)
      game 1.16.244.0        matched - plugin logs runtime=1.16.244.0
      SFSE 0.2.20 or later   matched - sfse.txt reports runtime 0.2.21
      Address Library        matched - v22.0.0.0, versionlib-1-16-244-0.bin
                             present for exactly this build
      DLL 0.3.0.0            matches meta.ini newestVersion

The "is it loaded, and in the right order" question is answered from its own
evidence log rather than from filenames: `AbsoluteControlPanel.evidence.jsonl`
holds 9,682 events across 9 runs, every one `level=info` - no warn, no error, no
unparsed line. Each run completes the same five-step boot (`config_loaded`,
`relocation_validation_succeeded`, `registration_succeeded`,
`pointer_input_registered`, `lifecycle_trace_installed`), and the relocation line
carries `runtime=1.16.244.0 mapping_count=20`, which is the proof Address Library
is being read for the exe that is actually running. Pause-menu insertion is
balanced (2,172 `active_menu_insert_entered` against 2,172
`active_menu_insert_completed` - no half-applied inserts), and it injects at
vanilla+1 (`vanilla_count=8..11` -> `requested_count=9..12`), so other mods'
entries, this shop's included, keep their slots.

Three notes, none of them faults:

* `OpenHotkey=0x00` in its ini, so the hotkey path logs `open_hotkey_disabled`
  by design; it opens from Pause Menu -> MOD OPTIONS. Set the ini if a key is
  wanted.
* Audio leases: 9 acquired, 8 released - one panel was open when a session
  ended. Leases are per process so nothing carries over, but a stuck duck after
  closing mid-session would point here.
* Only the core module is installed, no HOTAS / Power / HeadTracking /
  AbsoluteZero siblings, so the archive's multi-module conflict warnings do not
  apply to this setup.

What is still open is unchanged and is not verification debt: the first search
keystroke costs ~450 ms while it builds search keys, and rendering is parked
last - NativeModel's barter read-back and the steppy previewer.


### 0v. 0.9.1 and 0.9.1-OSF: re-release after 0.9.0 was withdrawn (2026-10-29)

0.9.0 was pulled from Nexus after three reports: a CTD on load with both
downloads, and MO2's plugins.txt / loadorder.txt / modlist.txt coming back
reordered after the crash. Nothing in this build was verified against those
reports - they need the reporters' sfse.txt - but the audit for them found a
packaging defect big enough to explain "the mod breaks my setup" on its own:

**Both 0.9.0 archives shipped both DLLs.** `SFShopExplorer 18102 0.9.0` and
`DevilzShopOSF 0.9.0-OSF` each contained `SFShopExplorer.dll` AND
`DevilzShopOSF.dll`, so installing either download made SFSE load two copies of
the same engine: two catalogs, two audio sessions both ducking the game, two
bridges, and the overlay's Present hook installed even for OSF-only users. The
README's own "use this or OSF, not both" was impossible to follow from the
packages. Each 0.9.1 archive now carries exactly one DLL.

The other three fixes in 0.9.1:

1. Asset discovery reads only the loaded Data view. `DiscoverMoMods` and its
   ModOrganizer.ini / selected_profile / modlist.txt / B:\MO2 fallback are gone
   from the shipped source; nothing in the manager's tree is opened. This is
   also the whole of the plugin's filesystem reads at load - it never wrote to
   plugins.txt, loadorder.txt or modlist.txt in any version, and still does not.
2. Sounds. `devilzdad.osfshop.audio` is a `RegisterRequest`; the page posted on
   the command transport, so click/hover/select/denied were dropped at the
   bridge silently. Confirmed working in game 2026-10-29 before this build.
3. Closing stops the music; mute covers effects as well as music.

**Log budget.** The per-item preview lines are DEBUG now (prefetch, texturing,
per-range verdicts, model build, material lookups, OSF preview per selection) -
13 sites in OfflineModel.cpp, 5 in LiveModel.cpp, 1 in PreviewHost.cpp. The
DRED breadcrumb block at plugin load and the `%TEMP%\SFShopExplorer-prefetch.txt`
trace are gated on `SFE::VerboseLogging()` (declared in ShopExplorer.h, defined
in main.cpp against `[logging] verbose`), so `verbose=0` - the shipped default
in both INIs - writes neither. A browsing session that used to write ~10 lines
per item writes a few dozen lines in total.

**Version bump, and its cost.** 0.9.1 / 0.9.1-OSF in `xmake.lua` (set_version
plus both targets' COMMONLIB_PLUGIN_VERSION_PATCH), `SFE::kVersion`,
`SFE::kVersionOsf`, both meta.ini files and the overlay INI header. Earlier
builds stayed at 0.9.0 on purpose because `kVersion` stamps the persisted CDB
index. That cost is accepted here: a re-release after a withdrawal has to be
distinguishable from the withdrawn one in a log, and the first preview after
the update rebuilds the index once (`usecache=0` is the shipped default anyway).

**Builds.** 2026-10-29 17:09 (re-cut after the visit-count fix, see below),
x64 releasedbg, 0 errors, 0 warnings.

    SFShopExplorer.dll  2,262,528 bytes
      2E8C5D6B8427BCBAEDA171B9761CED79E2669FB88F666805E8FF443CBB369E12
    DevilzShopOSF.dll   1,739,776 bytes
      F2FD7765D4B5094323D71FD98CC69E6C978EA092FA3D1FB7459C9D3F86331024

Both DLLs report FileVersion 0.9.1.0.

**Archives** (`release\`, cut from the folders beside them, contents at root):

    SFShopExplorer 18102 0.9.1-20261029.7z   32,202,345 B   61 entries
    DevilzShopOSF 0.9.1-OSF-20261029.7z      15,881,782 B   66 entries

Each carries one DLL, `README.txt` with the 0.9.1 changelog and its own hash,
`meta.ini`, the INI with `verbose=0`, and the 29-file `src` tree. Parcel Fix was
not re-cut: 1.0.0 already does what it is for.

**Deployed to both MO2 mods** so the live install is what a user installs:

    mods\DevilzDad's ESM Shop Mod Explorer   SFShopExplorer.dll + ini + assets
    mods\OSF DevilzDad's ESM Shop            DevilzShopOSF.dll + ini + page

Each folder now holds exactly one DLL, and the page, INIs and `src` are
byte-identical to the archives (`diff -rq` clean, `cmp` clean). Profile state
left as it was: OSF enabled at modlist line 21, the overlay disabled at line 26
- one at a time, as the README says.

**Leftovers parked outside MO2's scan**, at
`B:\MO2\Starfield\_shop-leftovers-20261029` (113 files, 225 MB), so the test
starts clean and nothing is lost:

* 28 `SFShopExplorer.dll.bak-*` and 16 `DevilzShopOSF.dll.bak-*` /
  `.backup-*` from earlier sessions
* the overlay's `SFSE\Plugins\Backups\` folder (79 files) and its
  `previewdebug.ini` probe config (all values were 1, i.e. defaults)
* a stale `app.js.backup-before-pageup-close-20261027-1826` in the OSF page
* the `overwrite\SFSE\Plugins\*\Assets\models\cdbindex.bin` caches, which were
  stamped with the old kVersion. Both had the same file name, so one overwrote
  the other on the way out - either is regenerated on the next preview, and the
  new kVersion stamps a fresh one regardless

The two previous INIs are kept beside them as `*.ini.before-0.9.1`: the test
runs on the shipped defaults, which is what a new user gets, but their theme,
music and unlock progress can be put back by copying the file over.

**Suites: 37 of 38 pass.** The failure is `test_ammo_nif_audit.py`, and it is
not this build: it parses the 16:11 log written by the *previous* DLL, where
`002B558E '3.5kV Light Cartridge'` and `002B559C '1.0kV Cartridge'` log
`=== searching for model path ===` and then never a `MODEL PATH = '...'` line,
so the audit reports `nif-unreadable` for both. NativeModel's scan genuinely
found nothing for those two; the previews still drew (1 mesh, 352 tris) because
the offline path resolves the NIF itself. Left failing rather than loosened -
it is a real NativeModel gap on two ammo forms, and it belongs with the
rendering work.

### 0w. Visit counting: one open is one open (2026-10-29 17:09)

Reported: "opened 30+ times, the Starfield theme still says 1 / 8". Two separate
faults, one of them mine.

**The throttle was real.** `OsfBeginSession` counted a visit at most once per
300 s, on the belief that "the page re-requests the catalog on every retry and
refocus, so a request is not a visit". That premise died when the shop started
riding OSF's `osfui/views` state: `OsfBeginSession` is now reachable only from
`OsfViewShown()`, which is already guarded by `open.exchange(true)` so a
replayed state cannot restart the music or count twice. One open therefore
reached the counter exactly once and the throttle could only undercount -
six opens in five minutes counted 1. Removed; `AddProgress(Unlock::kOpens, 1)`
runs on every open in both front ends. The overlay's `Open()` always counted
per open and is unchanged.

**The reset was mine.** Deploying the release payload replaced both live INIs,
and the shipped overlay INI carries an empty `[premium]` section while the OSF
one carried none - so the swap zeroed the player's earned progress (28 opens /
1,059 credits / 6 free on the OSF side, 491 / 1,000,669 / 145 on the overlay).
Both INIs were restored by adding what the build wrote during the test session
onto the backed-up totals, since `AddProgress` saves absolute values:

    DevilzShopOSF.ini    opens 28 + 1  = 29     spent 1059 + 35 = 1094
                         free  6 + 25 = 31
    SFShopExplorer.ini   opens 491 + 1 = 492    spent unchanged 1,000,669
                         free unchanged 145

A release INI should keep shipping an empty `[premium]`, because that is what a
new player gets - the mistake was swapping it into a live install without
carrying the earned values across, not the INI's contents.

The Starfield theme's goal is 8 opens (`kPremium` in ShopExplorer.cpp); with
29 on record it is earned.

**Deploy state:** both archives re-cut with the fix and verified (one DLL each,
DLL hash inside the archive matches the build). The OSF DLL and both READMEs are
deployed; `SFShopExplorer.dll` could not be copied because the game was running
and holding it open. Copy
`release\SFShopExplorer-0.9.1\SFSE\Plugins\SFShopExplorer.dll`
(`2E8C5D6B...`) into the overlay mod once Starfield is closed - the build loaded
for the running test is the 16:40 one, which still has the throttle.

**Not verified:** a launch of 0.9.1 in game; and the withdrawal reports
themselves. The build removes the manager-tree read and stops shipping the
second DLL, and neither of those is a diagnosis of a stranger's CTD. What is
needed from a reporter is `sfse.txt` (game build, plugin load list) plus the
shop's own log from the crashing launch.


### 0x. Basket sounds, and a seventh effect (2026-10-29 17:20)

Reported: "remove basket sound missing". The overlay's whole basket panel was
silent - add, the +/- steppers, remove, accept and cancel played nothing, so the
catalog row and the star were the only places in the shop that made a sound.

**A new effect, in the house voice.** `ui_remove.wav` (0.17 s, 44.1 kHz stereo,
30,032 B) is written by `audio/make_samples.py: make_remove()` - the same
`click()` attack as ui_click at the same level, with the tone falling a fifth
(620 -> 415 Hz) instead of holding, so it reads as subtraction beside the rising
`ui_select`. `Slot::kRemove` / `Sfx::kRemove` are appended to both enums and
`kSfxNames`, keeping the two arrays in the same order.

**Old banks keep working.** `ui_remove.wav` is newer than the other six, so an
`Audio\` folder carried over from an earlier build has no such file. The loader
now copies the click clip into the remove slot when the decode fails, and logs
`no ui_remove.wav; removals use the click` - a missing optional file is not a
broken bank, so it no longer raises `SetError` either.

**Where it fires.** Overlay: ADD TO BASKET -> kSelect, `-` and `+` -> kClick,
`x` -> kRemove, ACCEPT -> kSelect, CANCEL -> kRemove. OSF page: the capture-phase
click delegate owns all button sounds, so removal is chosen there
(`data-act="rm"` -> `sfx('remove')`, everything else `sfx('click')`) rather than
in the action handler - an extra call from the handler double-fired. Clear stays
an ordinary click for the same reason. `OsfFrontend`'s audio endpoint maps the
name `remove` to `Sfx::kRemove`; an unknown name is still silence.

**Builds and packages.** 2026-10-29 17:18 (overlay) / 17:18-17:20 (OSF repacked
after the clear-button fix), 0 errors, 0 warnings.

    SFShopExplorer.dll  2,262,528 B
      D8B3C49E13ABB9A11DAF32D615EA60EBD83DD3C5CA997DAC2E1E0FEA93558BB2
    DevilzShopOSF.dll   1,739,776 B
      3191702AB90777AE3E0A3664A47DA96054D57DA061B6AFEC7B88FE9424B91BD8

Both archives re-cut with `ui_remove.wav` in their Audio folder (12 files) and
the 17:18 sources; the overlay README's "six-effect bank" line is now seven.
`SFShopExplorer.dll` (D8B3C49E) is deployed to the overlay mod. `DevilzShopOSF.dll`
could not be copied because Starfield was running with the **OSF** mod enabled
(modlist line 21 `+`, overlay line 26 `-`) - so the OSF side is still running
F2FD7765, the build without the `remove` name. The page is deployed and already
asks for `remove`, which the old DLL ignores silently, so nothing breaks; the
DLL lands as soon as the game is closed.

Worth remembering for the next test: the modlist state is what decides which
build loads, and swapping it is a MO2 job, not something a deploy does.


### 0y. 0.9.1 packages frozen (2026-10-29 17:2x)

Both archives are final and verified against their own READMEs: the DLL
extracted from each archive hashes to the value the README states.

    release\SFShopExplorer 18102 0.9.1-20261029.7z
      32,211,241 B
      7ccca347e4ef6130eb89a5027f8f8c01d9c915b509a3086b7990cf75b9ac70dd
      SFShopExplorer.dll D8B3C49E13ABB9A11DAF32D615EA60EBD83DD3C5CA997DAC2E1E0FEA93558BB2

    release\DevilzShopOSF 0.9.1-OSF-20261029.7z
      15,890,816 B
      af330d734443782927b5809a8f5ca96e26d542d96e9315140b8f693947266348
      DevilzShopOSF.dll 3191702AB90777AE3E0A3664A47DA96054D57DA061B6AFEC7B88FE9424B91BD8

The OSF package carries 58 files: one DLL, 12 wav, 13 page files, 29 sources,
the INI (`verbose=0`), `meta.ini` and the 391-line README at version 0.9.1-OSF.

**Deployed, finally, to both mods.** The OSF DLL was the one copy still behind:
it had been blocked while the game ran with the OSF mod enabled, so the session
that was reported as "OSF works well now" ran F2FD7765 (17:09) rather than the
shipped binary. It is now 3191702A, matching the archive byte for byte. The
difference between those two is exactly one thing - the `remove` sound name in
the audio endpoint - so the behaviour that was tested carries over unchanged.

With that, both MO2 mods hold their packaged DLL, page, INI and sources, and
neither contains the other's plugin.


### 0z. 0.9.1 re-cut: basket sound parity (2026-10-29 17:38)

Supersedes the archive hashes in 0y; the version stays 0.9.1 because the
ImGui basket report arrived 20 minutes after that pair was cut.

What changed:

* Enter was the one add path with no sting. `ConfirmAdd()` runs from the
  ADD TO BASKET button (`kSelect`), the row double-click (`kClick`) and the
  keyboard Enter route, and only Enter was silent while still notifying
  "added X to basket". It now plays `kSelect` like the button.

* `PlaySfx` writes one DEBUG line naming the slot
  ("audio: sfx ui_remove.wav"). `[logging] verbose=0` filters it, so a normal
  session's log is unchanged; with verbose=1 a "clicked it, heard nothing"
  report becomes decidable in one run - either the slot reached the queue or
  the click never got that far.

Verified before cutting (the part that matters, because the report said the
overlay's basket was silent):

* Installed overlay DLL for the 17:21-17:26 session was D8B3C49E (17:19 deploy,
  before the game started), and that session's log says "7 clips" - i.e. the
  bank already carried ui_remove.wav, and ShopExplorer.cpp.obj (17:18:20) was
  compiled twelve seconds after the 17:16:06 source edit that added the calls.
  So the build that was tested did contain them; the sounds are in the archive
  that was already on disk as well.

    release\SFShopExplorer 18102 0.9.1-20261029.7z
      32,196,927 B
      01b5f39ce727ae36641c62536952616f54ec533900fb510e6a2b025e063efaf2
      SFShopExplorer.dll 25E4FCDA017E052E525C3AC38ABC21F4263FF74C319EC997EDD9B80B2C44A45F (2,263,040 B)

    release\DevilzShopOSF 0.9.1-OSF-20261029.7z
      15,880,534 B
      2d8bf2e6c024ed5228c6e925a3aba9253a41f9d046e47d6019d50ac75dd6a161
      DevilzShopOSF.dll BFE4A4A4B159FC912AF64A63104A59FDC295DB7650530EC79BDB7F6B11192761 (1,740,288 B)

Both archives extract to trees byte-identical to their release folders (55 and
57 files), one DLL each, and both DLLs are the ones now sitting in the two MO2
mods. The mod INI's premium counters read opens=11, spent=25035, free=12 after
the 0.9.1 payload replaced the file that had been restored to 492 opens - the
shipped INI is empty by design, so every payload deploy resets them.

Leftover swept at cut time: `src/LiveModel.cpp.before-mo2-strip` (251,716 B,
a stale copy of LiveModel.cpp from the MO2-strip refactor) was in the working
src\ and therefore in both packages and both mods. It is out of all four now
and parked in `_archive\LiveModel.cpp.before-mo2-strip`, so the source tree
still has it but a `src\*` copy cannot ship it again. Both `src\` folders
otherwise match the real source tree file for file, byte for byte, and the
OSF page in the package is identical to the deployed one.


### 1a. 1.0.0 work started: the player's own music (2026-10-29 18:2x)

First 1.0.0 feature: any track the player owns, not only the shipped RIFF.

* `IsMusicTrackName` accepts wav, mp3, flac, ogg, m4a, aac and wma (still
  excluding `ui_*`, which are effects). The overlay's Audio popup, the OSF
  page's Track select and the `musicTrack=` INI key all read that one list, so
  nothing else needed touching to make a dropped file appear.
* `DecodeAudioTrack` keeps the hand-written RIFF reader for `.wav` and sends
  everything else to Media Foundation - the decoder Windows already ships, so
  no codec is bundled. The source reader is asked for float stereo at 44.1 kHz
  (what Pcm holds), which makes it insert its own resampler for 48 kHz or mono
  files; decoded samples are clamped because a lossy codec overshoots 0 dBFS
  (AAC measured 1.105). Frames are capped at six minutes: float stereo costs
  350 KB a second and the loop is forever.
* Both targets link mfplat, mfreadwrite, mfuuid and shlwapi.

Measured with a scratch harness (compiled against the same MF calls, then
deleted) on ffmpeg encodes of theme_starfield.wav, 1,008,000 source frames:

    wav    1,008,000 frames   peak 1.000
    mp3    1,009,776 frames   peak 0.970   (encoder padding)
    flac   1,008,000 frames   peak 1.000
    m4a    1,009,664 frames   peak 1.105   (AAC overshoot; clamped)
    ogg    fails 0xC00D36C4   - no Vorbis decoder in Windows

Ogg stays listed but fails soft: the switch logs the format error and the
current track keeps playing rather than the shop going silent.

Deployed for testing: overlay A38768406CEE98A314339634FDC6581AC7C3D452A3EB6081D9E6C793A0FEB436,
OSF DA80CABA7558D76B6E1E2AE9CF23D2F085480EF38792CC66E9A61CA998FC9297. The
0.9.1 archives in `release\` are one build behind this and must be re-cut at
1.0.0 - do not upload them as they stand.


### 1b. Soundtrack folders (2026-10-29 18:5x)

A folder under `Audio\Music\` is now a soundtrack. `ListMusicTracks` appends it
as `Music\<folder>` (no extension), so it appears in the overlay's Audio popup
and the OSF page's Track select exactly like a file. `ResolveClip` resolves the
name to a directory, and that is how the mixer knows to walk it.

`PlaylistFiles` lists the folder's decodable tracks in case-insensitive name
order, so "01 - ...", "02 - ..." plays as intended. The mixer advances at the
seam: the next file decodes inline (one short buffer gap, hidden by a fade from
zero), and the last track wraps to the first. A file that will not decode is
skipped - one pass over the folder per seam, then the current track stays.

Built: overlay 01F49F9437AC6151D9392DDC4E48DC50331B48AF17E14BAD6FB932F63EDAF99B,
OSF DBC57A358199F3506D15BB4F4D9EA1B4E2EA2C207B426213EC6105CDA3E62D2F.
NOT deployed: Starfield was running, so both mods still hold the previous pair
(BD4DAD16 / C5D9A91A era). Deploy when the game is closed.

### Layout presets in the custom theme editor (2026-10-29)

The CUSTOMISE window now has a LAYOUT block under Music: an **Arrangement**
combo and two width sliders. Presets are `Classic - basket right` (the old
3-column workspace), `Basket centre` (plugins | basket | items), `Basket left`
(basket | plugins | items) and `Catalog top, basket bottom` (plugins + items on
the top 62%, basket full width underneath). The shop behind the modal
re-arranges as the combo changes, because the workspace reads `Custom().layout`
every frame.

Implementation: the workspace is still one table and the three panes still draw
in source order - only the column index each pane draws into changes
(`TableSetColumnIndex`), so a preset cannot desync from the pane code. The
stacked preset is the exception: the outer table gets one column and two rows,
the top row is a nested two-column table for plugins + items, and the basket
takes row two. Column mapping for stacked must keep `detailsColumn = 0`; a
one-column table asserts on index 2 (caught before shipping).

Persistence is `[custom] layout=`, `panewidth=`, `basketwidth=` in the build's
own INI, written by SAVE. Layout loads unconditionally via `LoadCustomTheme()`
in the workspace, so choosing another theme after saving keeps the arrangement -
it is a shop preference that happens to be edited in the theme window. Dragging
the pane divider still works in-session; the sliders are what survives a
restart. Ranges: plugins 180-520 px, basket 240-620 px.

Deployed 2026-10-29: overlay `bf9606d3…c3537`, OSF `56008a22…c38228`. The OSF
page is unchanged: it has its own HTML layout, and `custom` is listed as a
theme there without a matching CSS palette yet.

### Album skip buttons (2026-10-29)

With a soundtrack folder selected, the Audio popup shows `<` and `>` beside the
track name (tooltips "Previous / Next track in the soundtrack"). They are hidden
for single-file tracks on purpose: a lone file loops, so there is nothing to step
to and a button that appeared to do nothing would read as broken.

`ShopAudio::SkipMusicTrack(+-1)` posts one direction to the audio thread
(`g_musicSkip`), which steps the queue index with wraparound, decodes the next
decodable file inline (same one-buffer seam cost as the automatic advance) and
lets `loadMusic` restart the fade from zero. `MusicIsPlaylist()` is an atomic
flag set when the queue is filled and cleared for a single file; it is one frame
behind a fresh selection because the audio thread owns the swap.

Deliberately not a transport bar: the mixer auto-advances, so these are "not
this one", and the wildcard screenshot's big back/next arrows would have been
more chrome than the popup carries.

Deployed 2026-10-29: overlay `fa543b2f…8912a`, OSF `5ce3a1e6…a584f`. The OSF
page's Track select has no skip buttons yet - it would need a new bridge op.

### CTD on shop open: the workspace table's missing row (2026-10-29)

`fa543b2f` crashed Starfield the instant the shop was opened - not at launch and
not the custom theme (the live INI was on `theme=7`, and no `[custom]` section
had ever been written). The log stops right after `game-audio duck`, i.e. in the
first frame of DrawImGui.

Cause: the layout presets replaced `TableNextColumn()` with
`TableSetColumnIndex(column)`, and those are not the same call. TableNextColumn
starts row 0 when no row exists; TableSetColumnIndex() calls TableBeginCell()
directly, and TableNextRow() is also what runs TableUpdateLayout(). So the first
cell was drawn with `RowPosY1` never set and every column width still 0 - the
three panes got degenerate child rects and the renderer faulted.

Fix: an explicit `ImGui::TableNextRow()` before the first column is selected, in
both the flat and the nested (stacked) table. TableNextRow is the call that locks
the layout, so it has to happen first.

Two more bugs the same probe found:
* Shared table ID. All four arrangements used `reference_workspace`, so ImGui
  reused the first arrangement's column widths: picking "Basket centre" kept the
  classic 280/626/340 columns and put the panes in the wrong cells. Each
  arrangement now has its own table state (`PushID(layoutMode)` around the
  workspace block; PopID sits after the block because the pop-out preview lives
  inside it and captures `drawViewport` from that scope).
* The stacked preset's basket row was 29 px tall. A table's outer_size.y only
  bounds a table that scrolls, so the nested catalog table ignored its 0.62*
  height, filled the pane and left the basket the remainder. The top row is now
  capped by a `BeginChild` of 60% (clamped to leave 180 px for the basket).

Verification without the game: `tools/workspace_probe` lays out all four
arrangements against the vendored ImGui with no renderer and asserts the real
pane rects. `xmake -P . && ./build/msys/x64/release/workspace_probe.exe` ->
PASS, panes 280/626/340 side by side and 399 above 293 stacked. The same tool
run with `--broken` (no row call) segfaults, which is the shipped CTD reproduced
in three seconds - the check that was missing when fa543b2f went out.

Deployed 2026-10-29: overlay `98c5476e…4320a`, OSF `6783ee9b…43f1`.

### OSF page: custom theme editor + soundtrack skip (2026-10-29)

The page could select Custom but not change it, and the overlay's skip buttons
had no OSF counterpart. Both are now in the page.

Customise: with Custom in use, the Theme popup shows a CUSTOMISE button under
the grid. It opens an editor with the name, the backdrop (shared / starfield /
hell / heaven) and the nine colours this page's CSS actually renders. Picking a
colour re-themes the shop behind the dialog; Save writes `[custom]` and
`[customcolours]` and re-applies the theme; closing without Save reloads the
saved palette, so an unsaved colour cannot look saved. The page and the overlay
edit the same in-memory `Custom()` state in the same process, so a page save is
visible in the overlay without a restart. Layout presets stay overlay-only -
the page's arrangement is CSS.

Bridge: new endpoint `devilzdad.osfshop.customtheme` (op "" reads, op "set"
writes flat colour keys - the bridge's JSON reader is a key lookup, so no nested
object). `ShopBrowser::OsfCustomTheme()` / `OsfSetCustomTheme()` wrap
`Custom()` + `SaveCustomTheme()`, and each theme row now carries `"custom":true`
so the page identifies the editable card without depending on its index, which
is an enum detail, or its name, which the player types.

Skip: `op:"skip"` with `delta` on the audio endpoint calls
`ShopAudio::SkipMusicTrack`, and the audio reply carries `"playlist"`.
`audio.js` enables the two buttons only for a folder, with a tooltip explaining
why they are dead on a single file.

Checks: `node --check` on all four page scripts, and every element id the page
greps for exists in index.html (one pre-existing `#qty` miss, guarded, left
alone). Both DLLs build. The page was not driven in a browser: it needs the OSF
bridge, so the editor and buttons are verified by construction, not by click.

Deployed 2026-10-29: overlay `e613438f…`, OSF DLL `f04e557b…`, plus index.html,
app.js, audio.js and shop.css into the OSF mod's view folder.

### Player backdrops: the assets folder is the list (2026-10-29)

The custom theme's Background picker was a fixed array of four strings
("", starfield, hell, heaven), so an image in the folder could never appear in it.
Now the list is what actually loaded:

* `CreateNativeTextureResources()` scans `<assets folder>\` for
  `space_*.<png|jpg|jpeg|bmp>` (WIC decodes all four), skips names already in the
  fixed set, and uploads the rest. Scanned files are capped at 2560 px on the
  long edge - a phone photo is otherwise decoded into a full RGBA texture at 8K.
  Names are owned by a `std::deque<std::wstring>` during the upload: the
  directory iterator's path does not outlive the loop.
* After the uploads, the suffixes of the files that decoded go into
  `g_backdropSuffixes`, sorted and deduped, read by `Overlay::BackdropSuffixes()`
  and logged as "N backdrop image(s) available: ...". A file that failed to
  decode is not offered, because choosing it would silently show the shared art.
* `ThemeSpaceTexture()` now resolves through `BackdropTexture(suffix)`, which
  matches a loaded file by its `space_<suffix>` stem instead of rebuilding
  `space_<suffix>.png`. That is what makes `.jpg` work at all, and it also means
  a theme's own art is just another entry in the same list.
* The editor's combo is "(shared)" + that list, and names a saved backdrop whose
  file has since disappeared instead of showing the shared art with no reason.

Naming is lowercase `space_<name>`: the suffix is the file name minus prefix and
extension, and it is stored as the theme's `backdrop=` value. The OSF page still
probes its own `assets/space_<suffix>.png` for the four names it knows - it does
not read this folder.

Deployed 2026-10-29: overlay `71ae7d25…`, OSF `a8fb1b13…`.

### Render workbench: the materials it could not see (2026-10-30)

The offline renderer (`tools/renderer_recon/contact_sheet.py`, driven by
`tools/render_workbench.py`) drew 95 of the first 400 Misc items as a flat
colour. 43 of those were BC7 colour maps (fixed earlier); the rest were
resolution gaps, and two of them were real bugs in the audit's mirror of
`FindCdbMaterialLocked`:

* **The stem lookup kept `.mat`.** `CdbPathParts` in `src/LiveModel.cpp` erases
  the extension before the stem lookup; `aid_colour_audit.Cdb.own_set` did not,
  so `Materials\Common\Metal\MetalBronze02.mat` matched the database's separate
  `metalbronze02.mat_TextureSet1` (an empty record) and stopped there, never
  reaching `metalbronze02_TextureSet1` (MetalDullWorn01_color.dds). Every
  material whose name is also a set stem with a `.mat` variant was flat.
* **The colour-family fallback only ran when there was no record at all**, and
  only for a colour word at the very end of the stem. A numbered variant
  (`PlasticWornScratched02_Grey02`) records its own *empty* set and keeps the
  art on `plasticwornscratched02`; the number is now stripped before the colour
  word is read, and an empty set gets the same family attempt as a missing one.
  A family that is itself flat is not lent - that would claim art the database
  does not have.
* **The `.mat` last resort demanded a `_color` substring.** Mod sets name their
  maps their own way: the Cantina mod ships `Cantina_SmallGenerator_C.dds`, the
  Star Wars prop pack ships `Textures\SWProps\Datapad\color.dds`. The colour
  slot is now read as a *role* from the last name token, with the non-albedo
  roles (normal/rough/metal/mask/opacity/ao/emissive/...) excluded and the
  first remaining map used when no name carries an albedo role at all.

Measured on the same selection (`--type MISC --limit 400 --once`, before/after
tile folders in `reports/workbench/misc-audit2` and `misc-audit3`):
`lab_shelf_diff.py` reports 35 tiles changed, 0 lost a texture, 365 identical -
flat items 52 -> 44, partly textured 0 -> 7, and 35 rows gained at least one
real map (snowglobes +1 to +2, the Plushie Parsecpooch 1 -> 5, the Datapad
items finally draw `swprops/datapad/color.dds`).

What is left is not a parser gap, and each case was checked against the bytes of
`materials/materialsbeta.cdb` (the extract is byte-identical to the archive's
copy):

* `MetalGold01_TextureSet1`, `MetalBronze03_TextureSet1`,
  `Bar_Shaker_MetalAluminum02_TextureSet1` and friends exist and carry only a
  roughness map (or nothing). They are layered materials: albedo comes from the
  layer's constant colour, so the flat tint the sheet draws is the honest
  render. Same for the glass/glow families.
* `AK_Kitchenware01_Base`, `OxygenRescueMask01_Metal/Paint/Plastic`,
  `Bar_Jigger_MetaAluminiumBrushed01`, `Hourglass_Sand`, `DesktopOrigami01_...`,
  `Contemporary_Cigarettes` appear in the database as *objects with no
  `_TextureSet` child* - they are layer instances over a parent material, and no
  pass over names can find an albedo that is not keyed anywhere.
* Five mod NIFs carry absolute `file:///E:/Program Files (x86)/Steam/...`
  material paths from the author's install (Mass Effect props). Nothing on this
  host can read those, and the mod's own BA2 was not found under
  `materials/ME/`.

Tests: `tools/renderer_recon/test_aid_colour_audit.py` pins the stem/extension
rule, the full-path-wins rule, the numbered family borrow and the flat-family
refusal; `tools/renderer_recon/test_contact_sheet.py` pins the `.mat` colour
role (`_color`, `_C`, bare `color.dds`, normal+rough is not a colour, a map the
host lacks is refused). 95 tests pass.

### Layered materials: the parent link is not in the record stream (2026-10-30)

Follow-up to the material-resolution fix above. Goal was to resolve a layer
instance's albedo through its `BSMaterial::MaterialID` parent, so records like
`AK_Kitchenware01_Base` stop drawing as a flat tint.

What the file does give:

* the DIFF record stream carries every object's **name** (`0x01F4`) and, next to
  it, a 4-byte **DBID** (`0x020F`, `0x04F8`, ...). Pairing those gives 243,303
  named objects - the id space `0x0000BE79`, `0x1CD7D`, `0xF8E2`, ...
* the 16k-34.4M region holds fixed-size component tables, and a 12-byte record
  shape `u16 0 | u16 type | u32 a | u32 b` occurs 160,056 times where both
  numbers fall in the DBID range. Type `0x69` is the overwhelmingly common one
  (130,417), and `MetalGold01_Material1 -> MetalGold01_Layer1 -> MetalGold01`
  does appear among them.

Why it was **not** wired in: the join does not survive checking.

* With ids paired correctly, only **3,727** of those 130,417 type-0x69 records
  have both endpoints in the DBID map (the earlier 130,417 figure came from a
  drifted pairing that invented ~12k extra ids). The DBID range holds only
  ~400k values, so "both endpoints known" is ~37% likely *by chance* - most of
  the matches are coincidence, not edges.
* The ground-truth relation `GrassSalt_Material1 -> GrassSalt` is **absent** from
  the validated set under every pairing rule tried, while the MetalGold01 chain
  is present under two of the three - i.e. the hits do not discriminate.
* The component tables' ids are therefore a different space from the DIFF DBIDs
  (that is what `BSComponentDB2::DBFileIndex`'s `ObjectInfo{DBID, Parent,
  ParentPersistentID, HasData}` / `ComponentInfo{ObjectID, Index, Type}` /
  `EdgeInfo{SourceID, TargetID}` structure in the CDB's own type table
  describes). Joining them needs those tables decoded, not guessed.

Measured cost if it had worked: a three-hop walk over the *incorrect* graph
resolved 0 of the 62 materials that are still flat on the Misc 400 selection, so
the feature would have changed no tile there either way. The tool therefore
stays as shipped in the entry above; the next step for anyone picking this up is
`cdb_schema.py --class <n>` on the DBFileIndex classes to get the offset/size of
`ObjectInfo`/`ComponentInfo`/`EdgeInfo`, then walk those arrays directly.

### The DBFileIndex arrays decoded - and why the walk still cannot be named (2026-10-30, later)

Took the "next step" from the entry above: decode the class layouts instead of
guessing strides. It worked. `tools/renderer_recon/cdb_dbindex.py` does it, and
the answer to the layered-material question is now a measurement rather than an
open question.

The two facts that turn the blobs into layouts:

* A CLAS blob's first record is a **class header** (type 0, flags 0, offset 0,
  size = fieldCount) whose leading u32 is the class id. Every later record is a
  field, and a field's `type` is either a scalar code (`0xff10` bool, `0xff0b`
  u16, `0xff11` u32) or **another class id** - that is how a nested struct is
  spelled, and `max(offset+size)` is the struct's size.
* An array block is length-prefixed with the class it holds:
  `LIST | u32 total | u32 classId | u32 count | count * layout.size`, and
  `total == 8 + count * stride` **exactly**. A "LIST" inside a string or a
  record payload never passes that equality, which is why 1,227 blocks validate
  out of 8,241 apparent hits.

What is actually in `materialsbeta.cdb`:

| class | layout | rows |
|---|---|---|
| `0x013d` | `BSComponentDB2::ID { Value: u32 }` | - |
| `0x017d` | `ComponentInfo { ObjectID: ID, Index: u16, Type: u16 }` | 1,438,778 |
| `0x01bc` | `{ SourceID: ID, TargetID: ID, Index: u16, Type: u16 }` | 451,529 |

Every row of the 12-byte array carries **Type 105 and Index 0**, so it is one
single relation - the parent edge - and 451,529 of them is the right order of
magnitude for 410,721 objects. The ids are one space: `ComponentInfo` covers
8..613,651 and the edges 8..613,653.

The wall, and it is in the file rather than in the reader:

* **No `ObjectInfo` is serialised.** That layout is
  `{PersistentID, ID, DBID, Parent, ParentPersistentID, HasData}`, so three of
  its fields would be `0x013d`-typed; the widest class in the table has two
  (`0x01bc`, `0x0108`). The name pool at the head of STRT does name
  `BSComponentDB2::DBFileIndex::ObjectInfo` - it is the JSON schema, which can
  express types this compiled database never instantiates.
* **Names carry no id.** The stream names 367,906 objects and, for most of them
  (all the `*_Layer1` family, for instance), the group holds the name and
  nothing else. Only ~96k objects store an id at all (`0x04f8`, `0x020f`,
  `0x035f`, `0x0348`, `0x052d`, `0x1301`). Their ids *are* monotonic in stream
  order (96.7% increasing), but they start at 48,758, and every edge endpoint is
  8..613,653, so position cannot supply ids for the objects that lack them.
* Failing that, nothing joins an edge endpoint to a name, so a layered-material
  walk still has no parent to follow. The 100%-overlap test is useless as a
  discriminator here: 500,300 of ~613k ids are edge endpoints, so every id
  field's values overlap the edge set by construction.

Cost of the negative result, kept so nobody re-runs it: three alternative
alignments of names to ids (stream order, sorted ids, all six id fields) were
scored by whether an edge's parent name shares the child's stem - 0.0%, 2.1%
and "unresolved" respectively, i.e. chance. The earlier three-hop walk over the
mis-decoded graph resolved 0 of the 62 flat materials on the Misc selection, and
this decode changes that not at all.

To unblock layered materials, the parent has to come from somewhere that pairs
an id with a name: the runtime `BSComponentDB2::DBFileIndex` via SFSE (the
`RTTI_BSComponentDB2__DBFileIndex__IDFixupVisitor` vtable in `GameRTTI.inl`
already names the class), or a CDB written in a mode that serialises `Objects`.

Tests: `tools/renderer_recon/test_cdb_dbindex.py` pins the two layouts, the
block-size equality, the single edge type, and the absence (it fails if a future
CDB does serialise `ObjectInfo`, which is the signal to build the walk). 9 pass
there; the 68 in `test_aid_colour_audit.py` / `test_contact_sheet.py` still pass.

### The id-to-name sidecar: half built, and what blocks the dumper (2026-10-30)

The layered-material fix needs one thing the CDB cannot give: an object id paired
with a material name. The offline half is now in place; the in-game half is
blocked on information this tree does not have.

**Shipped (offline, tested):** `tools/renderer_recon/parent_index.py` reads
`tools/renderer_recon/extracted/parent_index.tsv`

    # osf-parent-index 1
    # build: 1.16.244.0
    271702	AK_Kitchenware01_Base	31297
    67957	AK_Kitchenware01_Rubber01	271702

three tab-separated columns - object id, the object's `CTName`, the id its
type-105 edge points at (empty when none). `colour_via_parent()` walks the chain
and asks `Cdb.own_set` for each ancestor, so an ancestor is used exactly when the
database would have answered for it directly and an empty set lends nothing.
`contact_sheet.resolve_material` calls it after the `.mat` fallback; with no
sidecar it is one dict lookup that returns None, so the renderer is unchanged
today (verified: the 68 renderer tests still pass).

**Blocked: nothing in the process can write that file yet.** Not for lack of
trying - the specific missing pieces:

* `GameRTTI.inl` gives only *type descriptor* addresses
  (`RTTI_BSComponentDB2__DB = 0x05CA7518`), which is enough to recognise a
  `BSComponentDB2::DB` object in memory but not to read it. CommonLibSF here
  declares no `BSComponentDB2` class at all, so there are no field offsets for
  `DBFileIndex`, `ObjectInfo` or the component storage.
* There is no Address Library / versionlib and no id-to-name database in the
  tree (`find -iname '*versionlib*'` finds only Fallout archives), so the
  engine's own lookup functions cannot be called by id either.
* The id-only half *is* recognisable without offsets - a contiguous block of
  451,529 12-byte records whose last two fields are `index=0, type=105` is hard
  to mistake - but that yields ids with no names, which is the exact gap.

BuildGate::Verified() (1.16.244) is where such a dumper would have to sit, and
the minimum that would unblock it is one of: a versionlib id-to-name database
for 1.16.244, headers for `BSComponentDB2::DBFileIndex` (offsets of
`Objects`/`Components` and the `CTName` payload), or a game-side export that
already writes names next to ids.

Tests: `tools/renderer_recon/test_parent_index.py` (14) pins the format, the
three spellings, the cycle guard and the empty-set rule; with
`test_cdb_dbindex.py` (9) that is 23, all green.

### DBFileIndex field offsets, read out of Starfield.exe (2026-10-30, follow-up)

The entry above stopped at "there are no field offsets for `DBFileIndex`". There
were: the engine states them itself. `tools/renderer_recon/db2_layout.py` reads
them from `Starfield.exe` and refuses to emit anything that the binary does not
corroborate.

How the layout is stated. Each reflected class has one registration function that
fills a class schema with a triple per member - name string, type-info, offset -
and then calls the schema installer with `lea edx,[reg+0x38]` (descriptor size)
and `lea r8d,[reg+N]` (**its own field count**). Five of those functions exist for
this family and they sit together:

| class | size | registration |
| --- | --- | --- |
| `BSComponentDB2::DBFileIndex` | `0x88` | `0x244d450` |
| `…::DBFileIndex::ObjectInfo` | `0x28` | `0x244de10` |
| `…::DBFileIndex::ComponentTypeInfo` | `0x10` | `0x244e7c0` |
| `…::DBFileIndex::ComponentInfo` | `0x08` | `0x244f050` |
| `…::DBFileIndex::EdgeInfo` | `0x0c` | `0x244f8e0` |

    BSComponentDB2::DBFileIndex           sizeof 0x88
      +0x00  ComponentTypes   std::vector<ComponentTypeInfo>
      +0x38  Objects          std::vector<ObjectInfo>
      +0x50  Components       std::vector<ComponentInfo>
      +0x68  Edges            std::vector<EdgeInfo>
      +0x80  Optimized        bool

    ObjectInfo                            sizeof 0x28
      +0x00  PersistentID        12 bytes
      +0x0c  DBID                uint32
      +0x10  Parent              uint32   (the DBID type, not the PersistentID one)
      +0x14  ParentPersistentID  12 bytes
      +0x25  HasData             bool

    ComponentInfo                         sizeof 0x08   ObjectID@0x00, Index@0x04, Type@0x06
    ComponentTypeInfo                     sizeof 0x10   Class@0x00, Version@0x08, IsEmpty@0x0c
    EdgeInfo                              sizeof 0x0c   SourceID@0x00, TargetID@0x04, Index@0x08, Type@0x0a

The three index arrays are `std::vector`, not a Bethesda array: the field's
type-info reports `std::vector` through its vtable **and** a method of that
type-info zeroes exactly `[obj]`, `[obj+8]`, `[obj+0x10]`. So the header is
MSVC's `{first, last, capacity}` and the count is `(last - first) / stride`, with
strides `0x28` / `0x08` / `0x0c`. This also supersedes the earlier reading of the
file's 12-byte tables: `EdgeInfo` is `{SourceID, TargetID, Index, Type}` and the
"`index=0, type=105`" tail is its last two members, not a separate shape.

Two things this settles about the sidecar:

* **`Parent` is a DBID, `ParentPersistentID` is a PersistentID** - the same
  type-info as `DBID` and `PersistentID` respectively. Two id spaces live in one
  record, and that is why the earlier join against the DIFF stream's 4-byte
  `0x020F` DBIDs "worked by chance": it was matching the right field to the right
  space only sometimes. The hierarchy link for `parent_index.tsv` is `Parent`
  (0x10), in DBID space, so the names (which the DIFF stream already pairs with
  DBIDs) are the only piece still outside the process.
* The type-105 edge table is a second view of the same relation (`Type` is the
  edge kind), not the only source of it. A dump can be written from `Objects`
  alone: `DBID`, `Parent`, `HasData`.

What is still missing for a dumper, stated so nobody re-derives it:

* the **`DBFileIndex` instance**. `BSComponentDB2::DB`'s vtable was checked for a
  trivial accessor and does not have one; its second method walks
  `DB+0x1a0`, an array of nine polymorphic service pointers, which is not the
  index. Locating the singleton (or the `DB*` global) is the remaining step, and
  it is what `BuildGate` would have to guard alongside these offsets.
* the `CTName` **component payload** - where a component's data lives. `Type`
  indexes `ComponentTypes`, whose `Class` member points at the component class's
  schema, so identifying `CTName` is reachable; reading its string is not, and
  the DIFF-stream join avoids needing it at all.

Reproduce (artifact, constants for the dumper, and the guard):

    cd "SF-Shop/SF Shop Explorer/tools/renderer_recon"
    python db2_layout.py --json extracted/db2_layout.json
    python db2_layout.py --cpp
    python -m unittest test_db2_layout

`extracted/db2_layout.json` records `1.16.244.0` and the exe's sha256
(`c92d468c…6e187`), and `--cpp` emits the same numbers as `SFE::Db2Layout`
constants for whoever writes the dump. `test_db2_layout.py` (19) pins every
offset above, the declared-vs-recovered field count for all five classes, the
vector evidence, and the build string - so a game update fails the test instead of
silently feeding new offsets into the game.

Two traps the tool had to be fixed for, both of which produced confidently wrong
numbers first: an offset is not always an immediate (`ComponentTypes`,
`PersistentID`, `ObjectID` and `SourceID` sit at 0 and are stored from a
register, and `r14d`/`r14` are the same register), and members are written
name-first in `DBFileIndex` but type-first in `ObjectInfo`, so the triple is
paired by stack-slot arithmetic rather than instruction order. Order-based
pairing reported `Parent@0x14` and `EdgeInfo.TargetID@0x08`.

### Locating the DBFileIndex instance: there is no static one (2026-10-30, follow-up)

The layout entry above ended on "the `DBFileIndex` instance" being the remaining
step, with the hope of a singleton or a `DB*` global. That hope does not hold, and
`tools/renderer_recon/db2_instance.py` now measures why instead of leaving it open.

**No static index exists to dereference.**

* `BSComponentDB2::DBFileIndex` is **not polymorphic**. RTTI in `Starfield.exe`
  names exactly one class in the family - `IDFixupVisitor@DBFileIndex@BSComponentDB2`
  - so there is no TypeDescriptor, no COL and no vtable for the index itself. No
  memory scan can recognise an instance by type.
* The family is never constructed from a literal vtable either: a whole-`.text`
  scan finds **0** `lea reg,[rip+vtable]` sites for `DB@BSComponentDB2@0x4d6a6b8`,
  and **0** image sections hold that pointer as data. Objects of this family only
  ever arrive through pointers that come out of the loader.
* A pattern search for the index's own four vector headers (`+0x00/+0x38/
  +0x50/+0x68`) is not discriminating on this engine: `0x38`, `0x50` and `0x68`
  are among its most common struct offsets, so every variant tried (store-shape,
  stride leas, rip-relative clusters, `sizeof` immediates, `Optimized` at `+0x80`)
  returned hundreds of candidates and no way to rank them.

**And there is more than one instance anyway.** The game merges
`materials/materialsbeta.cdb` with every `materials/creations/<name>/materialsbeta.cdb`
(this tree's `LiveModel.cpp` depends on exactly that), and each loaded file owns
its own index. A single hard-coded RVA would be the wrong shape of answer even if
one existed - the dumper has to enumerate.

**So the dumper enumerates.** `db2_instance.py --attach Starfield.exe --dump`
walks committed, private, writable memory and accepts an address when all of it
holds up:

    +0x38 Objects     std::vector, stride 0x28, (last-first) % 0x28 == 0
    +0x50 Components  std::vector, stride 0x08
    +0x68 Edges       std::vector, stride 0x0c
    +0x00 ComponentTypes std::vector, stride 0x10, 1..0x4000 rows
    sample of 48 edge rows, spread: every Index == 0 and one shared Type

That last clause is what makes a false positive implausible - it is the structure
`cdb_dbindex.py` measured in the shipped database (451,529 rows, all `Index 0`,
all `Type 105`), not a shape any other engine object has. `python db2_instance.py
--attach Starfield.exe` prints, per index, the address and the three counts, which
is exactly the header the sidecar wants.

    python db2_instance.py --exe <Starfield.exe>          # the evidence above
    python db2_instance.py --json extracted/db2_instance.json
    python db2_instance.py --cpp extracted/db2_dumper     # Db2Index.h + Db2Index.cpp
    python db2_instance.py --attach Starfield.exe --dump  # locate them, now

`--cpp` writes `SFE::Db2Index::FindAll()` for the plugin: the same predicate in
C++, gated on `BuildGate::Verified()` and on build `1.16.244.0` (sha256
`c92d468c…6e187`) because the offsets are the ones `db2_layout.py` recovered.
It reads 256 KB chunks and pre-filters in the copied block, so a scan is a
`memcpy` pass plus arithmetic rather than four `VirtualQuery` calls per qword;
`Readable()` still guards every pointer it dereferences.

**Verification, and what is not verified.** `test_db2_instance.py` (14) pins the
predicate: a well-formed index is accepted; a capacity that does not divide by the
stride, capacity below last, an `Index != 0` row, two edge types in one array, an
empty vector, a `first` pointer outside readable memory and 8 KB of ordinary bytes
are all refused; scanning finds an index that does not start at the region base,
finds one per region, and honours `min_edges`. The shipped-build tests re-derive
the "no static instance" evidence and the emitted constants. 47 pass with
`test_db2_layout` (19) and `test_parent_index` (14).

Not verified: `--attach` (the game was not running on this host) and the emitted
C++ was not compiled - this shell has no MSVC, no clang and no Windows SDK, so it
is syntax-reviewed and constant-checked by the test, not built. Scan throughput
measured on real data: 36.8 MB of the exe's `.rdata` + `.data` in 0.86 s
(43 MB/s, 0 false positives), which puts a 2 GB heap at roughly 45 s.

The remaining half of the dump is unchanged: `Objects` supplies `DBID` and
`Parent` per record, and the DIFF stream's `0x020F`/`0x04F8` pairing supplies the
names, so a runtime dump of `DBID \t Parent` joins to the names offline and
`parent_index.tsv` can finally be written.

---

## 2026-10-31 (later) - publishing to GitHub: not done, and why

**Status: blocked by tool loss, and the tree briefly had a half-built repo.**

No git repository existed under `D:\Projects\Address Library` before today.
During this session a paste of a Bash heredoc block into cmd.exe ran the two
lines that were valid cmd: `git init -b main` and `git add -A`. The heredoc
itself failed, so neither `.gitignore` nor this section was written, and the
whole tree (including `Parcel Fix/build/.deps`, `Parcel Fix/.xmake` and the
vendored `OSF UI V1.5.0 ...` folder) went into the index. No commit, remote or
push was confirmed. If `.git` is still present, rebuild from scratch:

    rmdir /s /q .git
    powershell -NoProfile -Command "Set-Content -Path .gitignore -Value @('__pycache__/','*.pyc','tests/_shots/','*.bak*','bin/','*.dll','*.lib','*.pdb','*.obj','*.log','.vs/','Parcel Fix/build/','Parcel Fix/.xmake/','Depends/commonlibsf-main/','OSF UI V1.5.0*/')"
    git init -b main
    git add -A
    git status --short

**Decision: publish `SF-Shop` as its own repository, private first.** Not the
`Address Library` root: that is a working area of game database dumps, ENB and
patcher binaries and extracted archives. A push can be multiple GB and git
history is permanent, so deleting files later does not shrink it. It also mixes
third-party mod assets and audio that are not ours to license publicly.

Exclusions worth a second look before the first commit:

- `Depends/commonlibsf-main` is itself a git repo. Excluding it keeps the outer
  repo clean, but a clone cannot build until it is added as a submodule.
- `OSF UI V1.5.0 ...` is OSF's own packaged release, not this project's source.

Gates before any push, do not skip:

1. Read the full `git status --short` list. Cheapest guard against a stray dump
   or a config carrying a key.
2. Total the tracked files. Hundreds of MB means something large slipped in:
   exclude it or use Git LFS, never commit-then-delete.
3. `gh auth status` before `gh repo create`, and confirm `git remote -v` after.

Commands:

    git commit -m "SF-Shop: OSF view source, render tool, tests, handoff notes"
    gh auth status
    gh repo create sf-shop --private --source=. --remote=origin --push

A README was deliberately not written: it would have been guessed at from a tree
that could not be listed. Write it from a session that can read the directory.
`OSF Shop Explorer/README.txt` remains the shipping and install doc.

**Why this was parked:** the session that did the backdrop deploy lost its local
shell mid-conversation. `run_terminal_command` returned "not currently
available" from the tool harness on every attempt, after earlier calls in that
same conversation had succeeded on these files. The Local/Cloud selector applies
to new conversations and does not re-attach tools to a running one. Resume in a
fresh Local session.

**Unrelated but pending:** the backdrop fix is complete and deployed (see the
entry above). It has not been seen in game. OSF snapshots a view at startup, so
the game needs a restart before the new backdrop is displayed.
