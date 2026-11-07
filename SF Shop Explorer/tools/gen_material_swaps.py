"""Generate the layered-material-swap table the preview applies per item form.

Starfield item forms do not carry their colours on the NIF: the record references
one or more BGSLayeredMaterialSwap (LMSW) forms whose entries rename a material,
and the renderer then loads the override path. The shop preview reads the NIF's
own material names, so every recoloured garment previewed in its stock colours
("Resort Wear" as a grey tee, the Star Roamer suit as the blue SpaceTrucker one).

The same mechanism paints books, notes, posters and misc props. A poster NIF is
a blank sheet - `BlankPoster01_Glow.mat` on the front face - and the art lives
only in the BOOK/MISC/ACTI record's MOLM swap list, so without this table every
poster in the catalog previews as the same blank poster and every note previews
as its stock hardback. BOOK (114), MISC (136) and ACTI (48) base-game records
carry MOLM; ARMO carries none and is found by the looser scan below.

This tool reads the plugin files directly and writes

    <ARMO formID>\t<key material>\t<override material>

which the plugin loads from <cacheDir>/materialswaps.tsv (the DLL directory
unless [offlinepreview] dir says otherwise). Regenerate after a game update or
whenever the relevant plugins change, and restart the game.

    python gen_material_swaps.py --data "B:\\SteamLibrary\\steamapps\\common\\Starfield\\Data"
    python gen_material_swaps.py --plugin "SomeMod.esm" --index 3 --out mod_swaps.tsv

An ESM's own forms resolve to raw IDs (index 0); a plugin's forms are written
with the load-order index the game reports, so pass the index it has in the
user's load order.
"""
from __future__ import annotations

import argparse
import mmap
import struct
import sys
from pathlib import Path

RECORD_HEADER = 24
GRUP_HEADER = 24
NAME_FIELD = b"EDID"


def _u32(buf, pos: int) -> int:
    return struct.unpack_from("<I", buf, pos)[0]


def iter_records(buf, start: int, end: int):
    """Yield (tag, record_offset, data_offset, data_end) for every record.

    A field group's size counts its own header; a record's size does not. Both
    are bounds-checked against the enclosing group before descending, so a
    truncated or mis-sized group ends the walk instead of walking into a
    neighbouring group's bytes.
    """
    pos = start
    while pos + RECORD_HEADER <= end:
        tag = buf[pos:pos + 4]
        size = _u32(buf, pos + 4)
        if tag == b"GRUP":
            if size < GRUP_HEADER or pos + size > end:
                return
            yield from iter_records(buf, pos + GRUP_HEADER, pos + size)
            pos += size
        else:
            if pos + RECORD_HEADER + size > end:
                return
            yield tag, pos, pos + RECORD_HEADER, pos + RECORD_HEADER + size
            pos += RECORD_HEADER + size


def subrecords(buf, data_start: int, data_end: int):
    """Yield (tag, payload) from a record body.

    Every subrecord is a 4-character tag, a u16 length and the payload. A tag
    that is not printable ASCII means the walk drifted (compressed or versioned
    record), so the record is abandoned rather than misread.
    """
    pos = data_start
    while pos + 6 <= data_end:
        tag = buf[pos:pos + 4]
        if not all(32 < c < 127 or c == 32 for c in tag):
            return
        size = struct.unpack_from("<H", buf, pos + 4)[0]
        yield tag, buf[pos + 6:pos + 6 + size]
        pos += 6 + size


def material_pairs(blob: bytes) -> list[tuple[str, str]]:
    """Key/override pairs out of an LMSW component blob.

    The blob is a component database: a STRT name table, a class table, then a
    LIST of BGSLayeredMaterialSwap::Entry. Each entry names its key material and
    then one or more override paths, so the material strings appear in
    key, override, key, override order.
    """
    names: list[str] = []
    i = 0
    while i < len(blob):
        if 32 <= blob[i] < 127:
            j = blob.find(b"\0", i)
            if j < 0:
                break
            text = blob[i:j].decode("latin-1")
            if text.lower().endswith(".mat"):
                names.append(text)
            i = j + 1
        else:
            i += 1
    return [(names[k], names[k + 1]) for k in range(0, len(names) - 1, 2)]


def molm_refs(buf, start: int, end: int) -> list[int]:
    """LMSW form IDs a record names in its MOLM subrecord.

    MOLM is `<u16 count><u32 form IDs>`, the explicit model-material-swap
    list. It exists on BOOK (114 base-game records), MISC (136) and ACTI
    (48) - every poster, note and painted prop that renames a blank stock
    material - but not on ARMO, whose swaps are only reachable by the
    looser reference scan below. Reading the list instead of scanning the
    whole record body is what keeps coincidental 4-byte matches out: a
    BOOK's FULL text-index subrecord used to match a 0x2883 swap record.
    """
    for sub_tag, payload in subrecords(buf, start, end):
        if sub_tag != b"MOLM":
            continue
        if len(payload) < 2:
            return []
        count = struct.unpack_from("<H", payload, 0)[0]
        ids: list[int] = []
        for i in range(count):
            off = 2 + 4 * i
            if off + 4 > len(payload):
                break
            value = _u32(payload, off)
            if value and value not in ids:
                ids.append(value)
        return ids
    return []


def read_plugin(path: Path, index: int) -> tuple[dict[int, list[tuple[str, str]]], dict[int, list[int]]]:
    """Return (lmsw swaps by form ID, item refs by form ID) for one plugin."""
    with path.open("rb") as fh:
        with mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as buf:
            if buf[:4] != b"TES4":
                raise SystemExit(f"{path.name}: not a TES4-family plugin")
            header_end = RECORD_HEADER + _u32(buf, 4)
            # Two passes: every swap record must be known before an item
            # record's references can be recognised, and plugins do not order
            # the two groups predictably.
            # Two passes rather than a materialised record list: the file holds
            # millions of records and only these two kinds matter here.
            swaps: dict[int, list[tuple[str, str]]] = {}
            for tag, rec, start, end in iter_records(buf, header_end, len(buf)):
                if tag != b"LMSW":
                    continue
                form = _u32(buf, rec + 12)
                blob = b""
                for sub_tag, payload in subrecords(buf, start, end):
                    if sub_tag == b"REFL":
                        blob = payload
                        break
                pairs = material_pairs(blob)
                if pairs:
                    swaps[form] = pairs
            items: dict[int, list[int]] = {}
            for tag, rec, start, end in iter_records(buf, header_end, len(buf)):
                refs = [value for value in molm_refs(buf, start, end) if value in swaps]
                if tag == b"ARMO":
                    # No MOLM on ARMO: the swap records are only reachable by
                    # scanning the body for form IDs, then filtered by the
                    # modal material directory (see filter_unrelated).
                    for sub_tag, payload in subrecords(buf, start, end):
                        for k in range(0, len(payload) - 3):
                            value = _u32(payload, k)
                            if value in swaps and value not in refs:
                                refs.append(value)
                if refs:
                    items[_u32(buf, rec + 12)] = refs

    base = index << 24
    return ({base | k: v for k, v in swaps.items()},
            {base | k: v for k, v in items.items()})


def filter_unrelated(pairs: list[tuple[str, str]]) -> list[tuple[str, str]]:
    """Drop pairs whose material belongs to another asset family.

    The ARMO reference scan finds a swap record by form ID anywhere in the
    record body, so a coincidental 4-byte match can pull in an unrelated swap
    (one spacesuit carried an Akila brick recolour). Real swaps rename
    materials inside the garment's own tree, so the modal top-level directory
    of the item's keys is the trustworthy one: `Clothes\\...` for clothing.
    """
    if not pairs:
        return pairs
    counts: dict[str, int] = {}
    for key, _ in pairs:
        top = key.replace("\\", "/").split("/", 1)[0].lower()
        counts[top] = counts.get(top, 0) + 1
    top = max(counts, key=lambda k: counts[k])
    return [(key, over) for key, over in pairs
            if key.replace("\\", "/").split("/", 1)[0].lower() == top]


def build_table(paths: list[tuple[Path, int]]) -> list[tuple[int, str, str]]:
    rows: list[tuple[int, str, str]] = []
    for path, index in paths:
        swaps, items = read_plugin(path, index)
        print(f"  {path.name}: {len(items)} item form(s) with swaps, "
              f"{len(swaps)} swap record(s)", file=sys.stderr)
        for form, refs in sorted(items.items()):
            pairs = filter_unrelated([p for ref in refs for p in swaps[ref]])
            seen = set()
            for key, override in pairs:
                if (key, override) in seen or key == override:
                    continue
                seen.add((key, override))
                rows.append((form, key, override))
    rows.sort()
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", help="Starfield Data directory (Starfield.esm is read from it)")
    ap.add_argument("--plugin", action="append", default=[],
                    help="extra plugin file to read (repeatable)")
    ap.add_argument("--index", type=int, default=None,
                    help="load-order index for --plugin (default: 1, 2, ... in order)")
    ap.add_argument("--out", default="materialswaps.tsv")
    args = ap.parse_args()

    paths: list[tuple[Path, int]] = []
    if args.data:
        esm = Path(args.data) / "Starfield.esm"
        if not esm.exists():
            raise SystemExit(f"Starfield.esm not found in {args.data}")
        paths.append((esm, 0))
    for n, plugin in enumerate(args.plugin):
        index = args.index if args.index is not None else n + 1
        paths.append((Path(plugin), index))
    if not paths:
        ap.error("pass --data and/or --plugin")

    rows = build_table(paths)
    out = Path(args.out)
    out.write_text("".join(f"{form:08X}\t{k}\t{v}\n" for form, k, v in rows), encoding="utf-8")
    print(f"wrote {len(rows)} swap pair(s) for "
          f"{len({r[0] for r in rows})} form(s) to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
