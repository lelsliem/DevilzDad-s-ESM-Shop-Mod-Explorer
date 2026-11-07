# BA2 / material audit tools

Offline regression tests for the archive and material assumptions in
`src/LiveModel.cpp`. They exist because the two worst bugs in this plugin
(the `materialsbeta.cdb` 0-byte read in 6r-5j, and the stem-only material
lookup in 6r-5k) were both invisible until someone launched the game and
looked at a picture.

Nothing here ships in the DLL. `ba2lib.py` is a deliberate line-for-line
replica of the C++ reader, so a format change must be made in both places.

## Files

| file | what it does |
|---|---|
| `ba2lib.py` | `Ba2Archive` - replica of `OpenBa2` / `Ba2Read` / the DX10 chunk walk, plus `MapTexFormat`, `TexRowPitch` and a pure-Python LZ4 block decoder |
| `ba2_audit.py` | parses archives, probes random entries and textures, exits non-zero on any failure |
| `material_sweep.py` | walks every material in `materialsbeta.cdb` and reports what the preview will actually do with it |
| `probe_material.py` | runs one NIF material path through the plugin's exact four-step CDB lookup and reports the resulting map, its archive, dimensions and DXGI format |
| `find_nif.py` | substring search over archive **name tables only** (no payload reads) - answers "which NIF is this item" without parsing 4 GB |
| `nif_materials.py` | extracts one `.nif` from an archive and lists the material strings it references |
| `dump_tex.py` | decodes a BC1/BC4 texture from an archive to a PNG, so an atlas can be looked at rather than guessed at |
| `test_dump_tex_decoder.py` | pins both decoders to recorded bytes: a sentinel detects pixels a block never wrote, and the four shipped 4x4 placeholder bytes must decode to one flat colour |
| `family_gate_diff.py` | extracts the CDB weapon-family gate out of `LiveModel.cpp`, compiles it, and checks the Python replica in `test_cdb_material_sets.py` against the real thing |
| `ammo_nif_audit.py` | audits every ammo item's shop ranges and UV footprints against the NIF its **own** `MODEL PATH` names; fails with `wrong-nif` if any other file is parsed |

## ba2_audit.py

```bash
# single archive
python ba2_audit.py "C:\...\Data\Starfield - Materials.ba2" --sample 4

# every archive in the data dir, probing a few files each
python ba2_audit.py --dir "C:\...\Data" --glob "*Textures.ba2" --sample 10
```

`--kind gnrl|tex` narrows it, `--quiet` drops the header banner,
`--sample 0` parses the entry tables without reading any payloads (fast,
useful on the 4 GB base-game archives).

It catches the class of bug where a stored size field disagrees with the
bytes actually on disk - the 6r-5j failure printed as
`materials/materialsbeta.cdb: zlib error: incorrect header check`.

## material_sweep.py

```bash
# the whole database
python material_sweep.py --data "C:\...\Data" --report sweep.tsv

# one family
python material_sweep.py --data "C:\...\Data" --filter tomato --verbose

# a random sample, failing the build if any colour map is unresolvable
python material_sweep.py --data "C:\...\Data" --sample 400 --strict
```

Reports per material: whether the database names a colour map at all,
whether that map resolves to a real file, whether there is a companion
opacity map, whether the companion resolves, and whether its dimensions
agree. `--strict` returns exit 1 when any colour map is unresolved, so it
can gate a build.

Use this before adding a hardcoded override: if the database already
answers for a family, an override is the wrong fix.

**A texture set with no colour map is an answer, not a miss.** The sweep
keeps those sets in the index (as `EnsureCdbIndex` does since 6r-5n) and
reports them as `flat/no albedo by design`; `--drop-empty` restores the
pre-6r-5n behaviour for A/B. Before that change the plugin erased them and
the fuzzy scorer ran on those materials, which is what turned the crate's
metal panels into black-and-red noise.

## probe_material.py

The one to reach for when a user reports a specific item:

```bash
python probe_material.py --data "C:\...\Data" \\
  --mat "Materials\SetDressing\SupplyContainers\Supplycontainer_Labels_Green.mat" \\
  --mat "Materials\SetDressing\Manufactured_Goods\Isotopic_Coolant\Isotopic_Coolant_Lid.mat"
```

It reports which of the four lookup steps hit, and whether the resulting
map is single-channel (`!! SINGLE-CHANNEL dxgi=80 used as albedo` - the
signature of the fuzzy scorer handing back a mask). Exit 1 on a miss or a
single-channel pick.

**Case matters in the glob.** An earlier version used `*[Tt]extures.ba2`;
Git Bash matches case-sensitively, so it silently indexed 10 archives
instead of 25 and reported real textures as `missing`. It now filters
`*.ba2` on `"textures" in name.lower()`.

## dump_tex.py

```bash
python dump_tex.py --ba2 "SFBGS00D - Textures.ba2" \\
  --tex "textures/.../actionfigurepackagingart_color.dds" --out atlas.png
```

BC1 and BC4 only - the formats the preview samples. It is what proved the
action figure packaging atlas is mostly padding and that the London snow
globe's skyline plate is sampled unflipped in V. Overlay the colour and
its `_opacity` to see which sub-rectangle is actually live.

**The decoder was wrong until 2026-10-17, three times over.**

- `bc1_block` and `bc4_block` iterated the four selector bytes as if each held
  one pixel, so **12 of every 16 pixels in a block were never written** and stayed
  black. A quarter-filled atlas reads as padding, which is what the two
  conclusions above concluded from.
- `bc1_block` unpacked the block endpoints as RGB555. BC1 endpoints are RGB565,
  so every decoded colour had green and blue shifted.
- Both decoders treated the `bx`/`by` arguments as *pixel* origins while every
  caller passed *block* indices, so each block decoded starting a fraction of a
  row off and real textures came out as overlapping stripes. They now take block
  coordinates and derive the pixel origin internally.

All three are fixed and pinned by `test_dump_tex_decoder.py`, which detects an
unwritten pixel with a sentinel value the decoder cannot produce. The 4x4
`textures/weapons/coachmanshotgun/decals_color.dds` block `f6c5f6c500000000`
decodes as 100% uniform `(197,190,180)`, and the same texture's neighbours now
read as ordinary albedos (`coachman_barrelstandart_color` mean (152,140,132),
`coachman_stdstock_color` (86,59,51)) instead of the near-black they appeared to
be. **Any earlier conclusion drawn from these PNGs about how much of an atlas is
live - including the two above - should be re-derived before it is relied on.**

## family_gate_diff.py

`test_cdb_material_sets.py` keeps a Python replica of
`CdbWeaponTextureMatchesFamily`, because the real function lives in an anonymous
namespace inside a plugin translation unit and cannot be linked from a test.
Source-string assertions do not prove that replica answers correctly on inputs
nobody wrote down, so this tool compiles the shipped function itself:

```bash
python tools/ba2_audit/family_gate_diff.py          # shipped rows + traps + 4000 fuzz pairs
python tools/ba2_audit/family_gate_diff.py --fuzz 40000
```

It pulls `LowerAscii`, `WeaponDirNamesFamily` and
`CdbWeaponTextureMatchesFamily` verbatim out of `src/LiveModel.cpp`, builds a
throwaway console program in a temp directory, and feeds both implementations
the same pairs: every shipped Coachman CDB row, every CDB weapon texture
directory against its own name and every prefix of it, the cross-family traps,
and deterministic token-soup fuzz.

Exit 0 means the two agree, 1 prints the disagreeing pairs, and **2 is a skip**
(no MSVC `vcvars64.bat`, or `extracted/materialsbeta.cdb` missing) - not a pass.
It found a real replica bug on its first run: `materials/weapons/<stem>.mat` has
no directory under `materials/weapons/`, and the replica treated the prefix
remainder as a family instead of returning early the way the C++ does.

## Adding a check

Two rules, both learned the hard way:

1. **Prove a field offset against the real bytes before shipping it.** The
   GNRL entry size field is the *decompressed* length (entry+20 is the
   on-disk one); the v2 DX10 header stride is 24, not 20. Both were wrong
   in a first draft of this harness and the harness caught them by
   failing against archives the plugin reads correctly.
2. **Keep the replica honest.** If `ba2lib.py` and `LiveModel.cpp` drift,
   the harness stops being evidence. Change both in the same commit.

3. **Look at the pixels before theorising.** Three wrong diagnoses in one
   session (the tomato, the empty drink pack, the action figure front
   cover) were all resolved in seconds by decoding the texture or reading
   the CDB record around it, after minutes of reading code. `dump_tex.py`
   and `renderer_recon/cdb_strings.py` exist for that.

## UV stream tools (6r-6c)

`uv_stream_check.py` and `uv_sweep.py` print the UV stream table for a NIF's
`BSGeometry` shapes and say which stream the plugin's new rule would take.

    python uv_sweep.py --data "C:\...\Data" \
        --nif meshes/setdressing/sfbgs00d/actionfigures_packaging/actionfigurepackaging_uc_armysoldier.nif

Both cache a name -> archive index at `extracted/ba2nameindex.tsv` (1,196,451
entries, ~6 s to build). Without it every `.mesh` lookup opens all 110
archives, which is why `nif_uv.py --first-uv` appeared to hang.

`parse_mesh_streams` in `nif_uv.py` is shared, so there is one implementation
of the `.mesh` stream walk. `read_nif` / `resolve_mesh_uv` are shared the same
way: the LOD-to-`.mesh` key rule exists once, and the CLI plus the ammo audit
both call it.

## ammo_nif_audit.py

Audits the shop's bound materials and UV footprints for **every** ammo item
against the file that item names, so an audit cannot silently compare a
sibling's NIF (which is exactly what went wrong on 2026-10-18):

    python ammo_nif_audit.py --data "C:\...\Data" \
        --log "%USERPROFILE%\Documents\My Games\Starfield\SFSE\Logs\SFShopExplorer.log" \
        --write ..\..\..\..\reports

Exit status is 1 on any failure, 0 otherwise. The item's own `MODEL PATH` is
the only file it will audit; there is no override flag on purpose.
`tools/test_ammo_nif_audit.py` pins the parser, the guard and each check.

## Mod and flat-material diagnostics (6r-8)

`mod_material_trace.py` traces one Creation Kit mod item end to end: the
NIF's material string, what the mod's own `materials/creations/<name>/
materialsbeta.cdb` says about it, and whether the texture it names exists in
any archive.

    python mod_material_trace.py \
        --ba2 "B:\MO2\Starfield\mods\Official CK\kgcdoom - main.ba2" \
        --nif meshes/kgcdoom/clutter/plush/doom_mancubus_plush_01.nif

This is the tool that established the general finding: **23 mod archives ship
their own `materialsbeta.cdb`** and the plugin reads only the base game's, so
every CK mod material is invisible to it. See 6r-8a - fixing that is separate
work and would not have fixed the Mancubus, whose texture is not installed.

`flat_explain.py` cross-checks the sweep's work queue against the CDB
override census, to separate "flat because the tint will cover it" from "flat
because the texture does not exist".

    python flat_explain.py --verdicts "D:\Projects\Address Library\reports\material-verdicts.tsv"

**The verdict TSV has no header row** - the first line is already data. A
parser that assumes a header reports zero rows and looks like it worked. The
columns are positional: material, verdict, colour key, opacity key.

Note this tool derives its CDB lookup keys from the verdict table's stem
only, so it discards path information. Its `tint-explained` counts are
indicative; for an exact answer on a specific material, query the CDB for
that material by name as 6r-8b does.
