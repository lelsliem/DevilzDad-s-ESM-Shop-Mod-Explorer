# Lab shelf: pre-0.8.345 vs 0.8.346, and where a vial liquid's colour is not

2026-10-24. Two questions: did scoping the sheer-glass rule to the material's own
name change the right tiles, and can the vial liquid's colour be recovered from
the item record or its material swaps?

## 1. The Lab shelf, re-rendered and diffed

Shelf = the 227 catalog records whose MODL sits under `SetDressing\ScienceGlass`
(the same 227 the Lab / Samples bucket matches).

| render | rules | tiles |
| --- | --- | --- |
| A | pre-0.8.345 (folder words counted, no clear-glass family) | 227 |
| B | 0.8.346 (own name + clear-glass family) | 227 |
| C | 0.8.345 (`own_name` patched back to identity) | 227 |

`tools/renderer_recon/lab_shelf_diff.py` compares pixel for pixel at 256 px.

| comparison | changed | identical | reading |
| --- | --- | --- | --- |
| A -> B (the ask) | **224** | 3 | the shelf was almost entirely blank glass before 0.8.345 |
| C -> B (this change alone) | **217** | 10 | plastic caps, cell-sample tissue and the bottle liquid on nearly every lab item |

Biggest movers, A -> B:

| px changed | form | item | what moved |
| --- | --- | --- | --- |
| 35,619 | 00265ADA | Beaker | 0.8.345: blank tile -> vessel |
| 19,392 | 00265ADC | Erlenmeyer Flask | 0.8.345 |
| 18,880 | 00265AD6 | Vial | 0.8.345 |
| 17,761 | 00265AFC / 00265AD7 / 001E79B0 | Vial / Cell Sample | 0.8.345 glass + 0.8.346 plastic cap |
| 12,658 | 00265AFD / 0103D06E / 01040786 | Bottle / Vial of Tranquilizer / Nutrifying Compound | 0.8.346: the `ScienceGlassLiquid02` body |

Identical against A: `FD0B0D22`, `FD0B0D23` (Bottle), `FE030AEC` (Tissue Sample) -
mod records whose model does not resolve to a renderable mesh.

Identical against C (no plastic or liquid material): `00265ADA` Beaker,
`00265ADC` Flask, `00265ADD` Funnel, vials `00265AD6` / `00265AD8` / `00265AE0` /
`00265B01`, and the three mod records above.

Files: `lab-shelf-diff.tsv` + `lab-shelf-diff.png` (A -> B),
`lab-diff-08345-vs-08346.tsv` + `.png` (C -> B).

### 1b. 0.8.347 — the liquid tint, measured on the same shelf

0.8.347 gives `ScienceGlassLiquid01/02` the fill family's green (see section 3).
Same 227 tiles, 0.8.346 -> 0.8.347:

| changed | identical | what moved |
| --- | --- | --- |
| **3** | **224** | Bottle 00265AFD, Vial of Tranquilizer 0103D06E, Experimental Nutrifying Compound 01040786 - 9,100 px each |

Everything else on the shelf, including the vial control 00265B01, is
pixel-identical: the tint table only matches those two material names.
Files: `lab-diff-08346-vs-08347.tsv` + `.png`.

## 2. The vial liquid's colour: not in the record, not in the swaps

`ScienceGlassLiquid01/02` is used by exactly one mesh,
`SetDressing\ScienceGlass\ScienceGlass_Bottle01Full01.nif`, on node `:9`, by
three items: Bottle (00265AFD), Vial of Tranquilizer (0103D06E), Experimental
Nutrifying Compound (01040786). Everything that could carry a colour was read:

- **Material record.** `ScienceGlassLiquid01_Material1` and
  `ScienceGlassLiquid02_Material1` carry two fields each: `0x020f` (a component
  link) and `0x01f4` (the name). There is no `0x025a` override colour/alpha and
  no `0x022b` layer kind - the fields whose presence gives the plastic its blue.
- **Layer.** `ScienceGlassLiquid02_Layer1` carries `0x0348` + `0x035f` (two
  links) and its name, nothing else.
- **Texture set.** `ScienceGlassLiquid02_TextureSet1` names four maps and all
  four belong to the kitchen Scissors prop: `Scissors_color.DDS`,
  `Scissors_normal.DDS`, `Scissors_metal.DDS`,
  `metalbarealuminumscuffed02_rough.DDS`. Of 12,371 texture sets in the CDB
  this is the **only** one naming a Scissors map, i.e. leftover authoring, not
  a liquid colour. Using it would paint the bottle steel-grey-brown.
- **Record swap.** None of the three forms has a material swap for the liquid;
  `materialswaps.tsv` has no row for any of them (the lab rows it does carry
  swap the green fill to `_Base_Green04`).
- **Colour that does exist nearby:** the fill family
  `ScienceGlass_GlassCleanTransparent01_Base_Green*` stores (0, 0.652, 0.596).
  That is the green a filled vial shows, and it is already drawn.

So the liquid has no authored colour anywhere. The neutral grey it drew was this
renderer's "no data" state, not the game's look. Three honest options were
weighed:

1. leave it grey (the body is at least visible);
2. draw it as the clear-glass family at 0.05 - the bottle reads nearly empty
   again, which is what the pre-0.8.346 tile did;
3. give `scienceglassliquid01/02` an authored tint equal to the fill family's
   measured green, in the same spirit as `kAuthoredGlassTint` for the coolant
   jar. This is invention, but it is the smallest invention consistent with the
   shelf's own palette.

## 3. What shipped (0.8.347): option 3

`kAuthoredFlat` at the top of `FlatBaseColor` in `OfflineModel.cpp` gives both
liquid materials the fill family's stored colour, (0, 0.6524, 0.5964), scoped to
the two names so the plastics keep theirs (`Plastic01` neutral grey, `Plastic02`
the authored blue). It lives in the shared build-time colour step, so the GPU
build and the CPU rasteriser agree, and `UploadModelTextures` still resets the
colour to white for any range that does get an albedo.

The mirror is `AUTHORED_FLAT_TINTS` / `authored_flat_tint` in
`aid_colour_audit.py`; `AuthoredLiquidTintTests` pins the green, that it equals
the fill's stored tint, the two plastics, the untouched pale glass family, and
the C++ table's keys and values. On the shelf diff it moves exactly the three
bottle records and nothing else.
