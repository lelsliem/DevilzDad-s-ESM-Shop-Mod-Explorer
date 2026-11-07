"""Sweep every material in materialsbeta.cdb and report what the preview will do.

This is the automated form of the check that has been done by hand every
time a user reports "this item still looks wrong": for each material stem
the CDB names a texture set, work out

  * does the set have a colour map at all (else the preview stays neutral)?
  * does that colour map actually resolve to a file in the BA2 set?
  * is there a companion opacity map, and does it resolve and match the
    colour map's dimensions/format?
  * does an opacity map with the same stem exist (the same-stem fallback
    PrefetchTextures tries when the CDB has no companion)?

Findings are grouped so a family like "all produce" or "all aid items"
can be fixed once instead of item by item.

Usage
-----
  python material_sweep.py --data "C:\\...\\Data" --sample 400
  python material_sweep.py --data "C:\\...\\Data" --filter tomato --verbose
  python material_sweep.py --data "C:\\...\\Data" --report sweep.txt

Exit code is 0 when the sweep completes; --strict makes unresolved colour
maps a failure so it can gate a build.
"""

from __future__ import annotations

import argparse
import random
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ba2lib import Ba2Archive, map_tex_format, surface_bytes  # noqa: E402

TEX_SUFFIXES = ("_color.dds", "_opacity.dds", "_alpha.dds", "_normal.dds",
                "_rough.dds", "_metal.dds", "_mask.dds", "_decal_color.dds",
                "_decal_opacity.dds")


def cdb_texture_key(s: str) -> str:
    """Normalise a CDB texture path to the plugin's asset key form."""
    k = s.replace("\\", "/").lower()
    for prefix in ("data/", "data\\"):
        if k.startswith(prefix):
            k = k[len(prefix):]
            break
    return k


# The game does not reserve one albedo spelling. Measured in the base game's
# own database: 41,000+ sets use "_color.dds", every canned-beverage
# Menuitem_* set uses "_BaseColor.dds" (MenuItem_Blend_BaseColor.dds),
# flora bases use "...Purplecolor.dds", a few detail sets "_albedo.dds" and
# "_diffuse.dds". Matching only "_color.dds" dropped those sets from the
# index altogether - and an absent set is the game's own statement that the
# material is flat-shaded, so the plugin drew them as bare grey geometry
# instead of falling back. The Blend can (Aid) was one of them.
ALBEDO_TAILS = ("color", "colour", "albedo", "diffuse")
NON_ALBEDO_TAILS = ("opacity", "alpha", "normal", "rough", "roughness",
                    "metal", "metalness", "mask", "emissive", "glow",
                    "gloss", "specular", "height", "ao")


def albedo_role_token(key: str) -> str | None:
    """The role token that makes a texture path an albedo, or None.

    The role is read from the LAST underscore token of the FILE NAME
    ("..._BaseColor.dds" -> "basecolor", "..._Purplecolor.dds" ->
    "purplecolor"), never from the directory, so a folder that happens to
    contain "color" cannot qualify a map. Mirrors CdbKeyIsAlbedo in
    src/LiveModel.cpp.
    """
    low = key.lower()
    if not low.endswith(".dds"):
        return None
    filename = low[:-4].rsplit("/", 1)[-1]
    token = filename.rsplit("_", 1)[-1]
    if token.endswith(NON_ALBEDO_TAILS):
        return None
    return token if token.endswith(ALBEDO_TAILS) else None


def is_color_map(key: str) -> bool:
    """True when this CDB texture path is a material's albedo."""
    return albedo_role_token(key) is not None


def is_opacity_map(key: str) -> bool:
    low = key.lower()
    return low.endswith("_opacity.dds") or low.endswith("_alpha.dds")


def choose_variant(stem: str, variants: dict[int, dict[str, object]]) -> dict[str, str]:
    """Pick ONE numbered `<stem>_TextureSet<N>` as the stem's donor.

    Several texture sets collapse onto the same stem, and they are NOT one
    material's maps: they are the base set plus every per-object swap variant
    the database ships. Merging them field by field produced frankenstein
    sets - `plasticplainsmooth02` took its colour from TextureSet3
    (MetalCopperAged01_color.dds) and its mask from TextureSet2
    (UnitedColoniesLogo_01_opacity.dds), so the plastic plate on every food
    item drew as bronze metal punched through by a mostly-black logo mask and
    all but vanished.

    The numbered variants therefore stay separate and one is chosen whole -
    no field is ever borrowed from another number. Numbering is the database's
    own statement about which set is the material and which are swaps, so the
    preference order is:

      1. the set names the stem AND carries a colour map (the material's own
         complete art),
      2. the set names the stem but has no albedo (an authored flat surface),
      3. any set with maps at all - lowest number, i.e. TextureSet1,
      4. an empty set (authoritative flat shaded).

    Duplicate records of the SAME number are one set split across the file
    (or merged from two databases) and are unioned before ranking; that is why
    `decallabelsletters03` keeps its `_color.dds` while
    `plasticplainsmooth02` no longer borrows a metal one.
    """
    def rank(item: tuple[int, dict[str, object]]) -> tuple[int, int]:
        num, slot = item
        maps = [m for m in (slot.get("color", ""), slot.get("opacity", "")) if m]
        named = any(stem in str(m) for m in maps)
        if named and slot.get("color"):
            quality = 3
        elif named:
            quality = 2
        elif slot.get("maps"):
            # ANY map counts, not just the colour and the mask: the plate's own
            # TextureSet1 carries only a `_rough` companion, and counting just
            # the two slots we keep let TextureSet2 (an unrelated logo mask)
            # outrank it.
            quality = 1
        else:
            quality = 0
        return (quality, -num)

    # The `_maps` flag is bookkeeping for the ranking above; the returned slot
    # keeps the two-field shape every caller and test compares against.
    return {k: v for k, v in max(variants.items(), key=rank)[1].items() if k != "maps"}


def _family_tokens(text: str) -> set[str]:
    return {t for t in re.split(r"[^a-z0-9]+", text.lower()) if len(t) >= 4}


def opacity_shares_family(set_name: str, color: str | None, opacity: str | None) -> bool:
    """True when an opacity map plausibly belongs to the set that owns it.

    The DIFF walk attaches every texture path that follows a set name, and for
    a layered item the next object's paths can follow it. PackagedFood_Cheese
    owns `cheese_color.dds` and inherited `drinkpack_singlebarrelbourbon_
    opacity.dds`; drawing the cheese block through a bourbon wrapper's cutout
    is what the "Cheese still looks the same" report was. 377 of the
    database's 23,045 colour/opacity pairs fail this test and every one of
    them pairs two different families.

    Mirror of the check at the end of FindCdbMaterialLocked (LiveModel.cpp).
    """
    if not opacity or not color:
        return True
    op = _family_tokens(opacity.rsplit("/", 1)[-1])
    if _family_tokens(color.rsplit("/", 1)[-1]) & op:
        return True
    return bool(_family_tokens(set_name) & op)


def build_cdb_index(cdb: Path, keep_empty: bool = True) -> dict[str, dict[str, str]]:
    """texture-set object name -> {'color': key, 'opacity': key}.

    A texture-set name opens its record group; its texture paths follow it
    until the next object name. Paths seen before any set name are unowned and
    ignored rather than shifted onto the next weapon/material texture set.

    Numbered variants of one stem are collected separately and then reduced to
    a single donor by `choose_variant`.

    `keep_empty` mirrors EnsureCdbIndex: a set with no colour map is the
    game's statement that the material is flat-shaded, so it must survive
    into the index - dropping it (pre-6r-5n) made the plugin's fuzzy scorer
    run on those materials and hand back unrelated maps.
    """
    data = cdb.read_bytes()
    variants: dict[str, dict[int, dict[str, object]]] = {}
    pending: dict[str, str] = {}
    pending_maps = False
    active_set: str | None = None
    active_num = 0
    pos = 0
    set_re = re.compile(r"^(?P<stem>.+)_textureset(?P<n>\d+)$", re.I)
    # The texture-map field id differs per database (0x038A base game,
    # 0x0EE8 sfbgs050, 0x03A9 sfbgs007, 0x031F shatteredspace, 0x02E7
    # sfbgs00d, 0x0DB1 sfbgs003, 0x0DB5 sfbgs008, 0x09E3 sfbgs047), so a
    # record is a texture path when it HOLDS one. Mirrors LooksLikeTexturePath
    # in src/LiveModel.cpp.
    name_field = 0x01F4

    def is_texture_path(value: bytes, count: int) -> bool:
        s = value[:count].split(b"\0", 1)[0].decode("latin-1").replace("\\", "/").lstrip("/ ")
        low = s.lower()
        if low.startswith("data/"):
            low = low[5:]
        return low.startswith("textures/")

    def commit_pending() -> None:
        nonlocal pending_maps
        if active_set is not None:
            # Same stem AND same number = one set seen twice: union it.
            slot = variants[active_set].setdefault(active_num, {})
            for map_kind, texture_key in pending.items():
                slot.setdefault(map_kind, texture_key)
            if pending_maps:
                slot["maps"] = True
            # A mask from another family is not this set's mask. See
            # opacity_shares_family for the measured case.
            if not opacity_shares_family(active_set, slot.get("color"), slot.get("opacity")):
                slot.pop("opacity", None)
        pending.clear()
        pending_maps = False

    # A name starts an object, and its data records follow it. Flush paths
    # owned by the previous name before switching; non-texture object names
    # close the prior group without becoming keys. Some CDB records contain
    # paths before the name, but those belong to the prior object (or have no
    # texture-set owner) and must never be shifted onto a weapon set.
    while pos + 16 <= len(data):
        if data[pos : pos + 4] != b"DIFF":
            pos += 1
            continue
        length = int.from_bytes(data[pos + 4 : pos + 8], "little")
        if length < 12 or length > 65536 or pos + 8 + length > len(data):
            pos += 4
            continue
        field_id = int.from_bytes(data[pos + 8 : pos + 12], "little")
        kind = int.from_bytes(data[pos + 12 : pos + 14], "little")
        count = int.from_bytes(data[pos + 14 : pos + 16], "little")
        value = data[pos + 16 : pos + 8 + length]
        if kind == 0 and count > 0 and 8 + count <= length and is_texture_path(value, count):
            raw = value[:count].split(b"\0", 1)[0].decode("latin-1")
            key = cdb_texture_key(raw)
            if active_set is not None and key.startswith("textures/"):
                pending_maps = True
                if not pending.get("color") and is_color_map(key):
                    pending["color"] = key
                elif not pending.get("opacity") and is_opacity_map(key):
                    pending["opacity"] = key
        elif field_id == name_field and kind == 0 and count >= 3 and 8 + count <= length:
            commit_pending()
            raw = value[:count].split(b"\0", 1)[0].decode("latin-1")
            # A planet's terrain layer is that planet's colour, and the desktop
            # globes name the layer itself: `Desktop_PlanetGlobe_Planet_Alchiba
            # II_terrain` carries BiomeMasks/Alchiba II_color.dds. Strip the
            # layer word before matching so both it and
            # `..._textureset_1_terrain` key as the material they belong to.
            # Mirrors IndexCdbBlob in src/LiveModel.cpp.
            terrain = raw.lower().endswith("_terrain") and len(raw) > 8
            set_key = raw[:-len("_terrain")] if terrain else raw
            match = set_re.match(set_key)
            if match and len(set_key) <= 128:
                # Two thirds of the keys are full paths written with
                # backslashes; store them slash-normalised, exactly as
                # EnsureCdbIndex does in LiveModel.cpp.
                active_set = match.group("stem").lower().replace("\\", "/")
                active_num = int(match.group("n"))
                variants.setdefault(active_set, {})
            elif (terrain and "_material_" not in set_key.lower()
                  and "_uvstream_" not in set_key.lower()
                  and "_textureset" not in set_key.lower() and len(set_key) >= 3):
                active_set = set_key.lower().replace("\\", "/")
                active_num = 1
                variants.setdefault(active_set, {})
            else:
                active_set = None
        pos += 8 + length
    commit_pending()
    index = {stem: choose_variant(stem, vs) for stem, vs in variants.items()}
    if keep_empty:
        return index
    return {k: v for k, v in index.items() if v.get("color")}


def opacity_pair_issue(color, opacity) -> str | None:
    """Return why a shipped opacity map cannot pair with its color map.

    Texture sampling uses shared UV coordinates, so resolution may differ.
    The audited weapon placeholders are 4x4; require at least 8x8 to avoid
    treating a single BC4 block as authored coverage. Color may be None for an
    opacity-only cutout.
    """
    if isinstance(opacity, tuple):
        return None  # Non-texture entry; caller already reports its source.
    if opacity.dxgi not in (77, 80):
        return "unsupported format"
    if opacity.width < 8 or opacity.height < 8:
        return "degenerate <8x8"
    if color is not None and not isinstance(color, tuple):
        if color.height == 0 or opacity.height == 0 or color.width * opacity.height != opacity.width * color.height:
            return "aspect ratio differs"
    return None


class TextureIndex:
    """Name -> (archive, TexFile) across the supplied archives."""

    def __init__(self, archives: list[Path], verbose: bool = False):
        self.map: dict[str, tuple[Ba2Archive, object]] = {}
        self.archives: list[Ba2Archive] = []
        self.unparsed: list[str] = []
        for p in archives:
            ar = Ba2Archive(p)
            if not ar.valid:
                self.unparsed.append(p.name)
                continue
            self.archives.append(ar)
            if verbose:
                print(f"  indexed {p.name}: {len(ar.tex_files)} tex / {len(ar.entries)} gnrl")
            for name, tf in ar.tex_files.items():
                self.map.setdefault(name, (ar, tf))
            for name, ent in ar.entries.items():
                self.map.setdefault(name, (ar, ent))

    def has(self, key: str) -> bool:
        return key in self.map

    def describe(self, key: str) -> str:
        hit = self.map.get(key)
        if hit is None:
            return "missing"
        ar, obj = hit
        if isinstance(obj, tuple):  # GNRL entry
            return f"{ar.path.name} (gnrl, {obj.size} B)"
        fmt = map_tex_format(obj.dxgi)
        return f"{ar.path.name} ({obj.width}x{obj.height} dxgi={obj.dxgi} fmt={fmt})"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, help="Starfield Data directory")
    ap.add_argument("--cdb", help="materialsbeta.cdb path (default: read from Data)")
    ap.add_argument("--archives", nargs="*", help="explicit .ba2 list (default: all in --data)")
    ap.add_argument("--filter", help="only sweep stems containing this substring")
    ap.add_argument("--sample", type=int, default=0, help="randomly sample N materials (0 = all)")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--report", help="also write the full per-material table here")
    ap.add_argument("--strict", action="store_true", help="exit 1 if any colour map is unresolved")
    ap.add_argument("--drop-empty", action="store_true",
                    help="pre-6r-5n behaviour: drop textureless sets from the index (for A/B)")
    args = ap.parse_args()

    data = Path(args.data)
    cdb = Path(args.cdb) if args.cdb else None
    if cdb is None:
        # Prefer the real archive when available, else the extracted copy.
        cdb = Path(__file__).resolve().parent.parent / "renderer_recon" / "extracted" / "materialsbeta.cdb"
    if not cdb.exists():
        print(f"[FAIL] material database not found: {cdb}")
        return 1

    print(f"reading material database {cdb} ({cdb.stat().st_size:,} bytes)...")
    index = build_cdb_index(cdb, keep_empty=not args.drop_empty)
    print(f"  {len(index):,} material texture sets indexed "
          f"({sum(1 for v in index.values() if not v.get('color')):,} textureless)")

    archives = [Path(a) for a in args.archives] if args.archives else sorted(data.glob("*.ba2"))
    print(f"indexing {len(archives)} archive(s)...")
    tidx = TextureIndex(archives, args.verbose)
    if tidx.unparsed:
        print(f"  [WARN] {len(tidx.unparsed)} archive(s) did not parse: {', '.join(tidx.unparsed[:6])}")
    print(f"  {len(tidx.map):,} unique texture names")

    stems = sorted(index)
    if args.filter:
        stems = [s for s in stems if args.filter.lower() in s]
    if args.sample and args.sample < len(stems):
        random.seed(args.seed)
        stems = random.sample(stems, args.sample)

    stats = defaultdict(int)
    missing_color: list[tuple[str, str]] = []
    missing_opacity: list[tuple[str, str]] = []
    opacity_mismatch: list[tuple[str, ...]] = []
    rows: list[str] = []

    for stem in stems:
        slot = index[stem]
        color = slot.get("color", "")
        opacity = slot.get("opacity", "")
        stats["materials"] += 1
        if not color:
            # Flat-shaded material: no albedo exists, and none should be
            # guessed. Reported, not counted as a failure.
            stats["flat_no_albedo"] += 1
            if opacity:
                stats["flat_with_mask"] += 1
            rows.append(f"{stem}\tFLAT\t-\t{opacity or '-'}\t(no albedo map)\t{tidx.describe(opacity) if opacity else '-'}")
            continue

        color_ok = tidx.has(color)
        if color_ok:
            stats["color_ok"] += 1
        else:
            stats["color_missing"] += 1
            missing_color.append((stem, color))

        if opacity:
            if tidx.has(opacity):
                stats["opacity_ok"] += 1
                # Color/opacity dimensions need not match: the live preview
                # samples both with the same UV and permits masks down to one
                # real BC block. Only a sub-block dimension (<4 px) is a
                # degenerate placeholder. Format need not match either: BC1
                # color + BC4 or BC3 mask is the normal shipped arrangement.
                # Guard lookups on color_ok: doing the map access anyway can
                # crash on missing-albedo records.
                a, b = None, tidx.map[opacity][1]
                if color_ok:
                    a = tidx.map[color][1]
                issue = opacity_pair_issue(a, b)
                if issue:
                    stats["opacity_mismatch"] += 1
                    opacity_mismatch.append((stem, issue,
                        f"color {a.width}x{a.height}/dxgi{a.dxgi}" if a is not None and not isinstance(a, tuple) else "color unavailable",
                        f"opacity {b.width}x{b.height}/dxgi{b.dxgi}" if not isinstance(b, tuple) else "non-texture"))
                elif color_ok and not isinstance(a, tuple) and not isinstance(b, tuple) and (a.width, a.height) != (b.width, b.height):
                    stats["opacity_scaled"] += 1
            else:
                stats["opacity_missing"] += 1
                missing_opacity.append((stem, opacity))
        else:
            # Same-stem fallback the prefetcher would try.
            guess = color[:-8] + "_opacity.dds"
            if tidx.has(guess):
                stats["opacity_samestem"] += 1
            else:
                stats["opacity_absent"] += 1

        rows.append(
            f"{stem}\t{'OK' if color_ok else 'MISSING'}\t{color}\t"
            f"{opacity or '-'}\t{tidx.describe(color)}\t{tidx.describe(opacity) if opacity else '-'}"
        )

    total = stats["materials"] or 1
    flat = stats["flat_no_albedo"]
    print()
    print(f"swept {stats['materials']:,} materials")
    print(f"  flat/no albedo by design {flat:,}  ({flat * 100 // total}%), "
          f"of which {stats['flat_with_mask']:,} carry an opacity-only cutout")
    print(f"  colour map resolves      {stats['color_ok']:,}  ({stats['color_ok'] * 100 // total}%)")
    print(f"  colour map MISSING       {stats['color_missing']:,}")
    print(f"  companion opacity map    {stats['opacity_ok']:,} present, "
          f"{stats['opacity_scaled']:,} lower-resolution paired, "
          f"{stats['opacity_mismatch']:,} rejected format/shape, "
          f"{stats['opacity_missing']:,} not found")
    print(f"  no companion, same-stem  {stats['opacity_samestem']:,} found, "
          f"{stats['opacity_absent']:,} none (opaque preview)")

    if missing_color:
        print(f"\nfirst {min(12, len(missing_color))} unresolved colour maps:")
        for stem, key in missing_color[:12]:
            print(f"  {stem:52s} -> {key}")
    if opacity_mismatch:
        print(f"\nfirst {min(12, len(opacity_mismatch))} opacity mismatches:")
        for finding in opacity_mismatch[:12]:
            print(f"  {finding[0]:52s} {': '.join(finding[1:])}")

    if args.report:
        Path(args.report).write_text("\n".join(rows) + "\n", encoding="utf-8")
        print(f"\nfull table written to {args.report} ({len(rows):,} rows)")

    if args.strict and missing_color:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
