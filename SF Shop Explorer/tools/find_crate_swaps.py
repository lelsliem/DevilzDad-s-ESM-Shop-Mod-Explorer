"""Which forms swap the ammo-crate lid material, and to what.

The crate NIFs name their lid material directly (`PaintedMetalShine01_Green`
on AmmoLaser.nif, `PaintedMetalshine01_Orange` on Ammo27Caliber.nif), yet the
game draws the 3kV LZR lid in a lime that no base-game texture carries. The
only base-game record that holds that lime is the CDB material
`AmmoBoxPrimer_Lid` (0.584, 0.800, 0.320), so something must be renaming the
NIF material. In Starfield that is a BGSLayeredMaterialSwap (LMSW) referenced
from the item form.

gen_material_swaps.py only walks ARMO, so ammo forms never made it into
materialswaps.tsv. This tool answers the narrower question: does any LMSW
rename the crate lid materials, and which record (AMMO/REFM/CONT/MISC) points
at it?

    python find_crate_swaps.py --data "B:\\SteamLibrary\\steamapps\\common\\Starfield\\Data"
"""
from __future__ import annotations

import argparse
import mmap
import struct
import zlib
from pathlib import Path

RECORD_HEADER = 24
GRUP_HEADER = 24
COMPRESSED = 0x00040000

WANT = ("paintedmetalshine01_green", "paintedmetalshine01_orange", "ammoboxprimer_lid")


def u32(buf, pos: int) -> int:
    return struct.unpack_from("<I", buf, pos)[0]


def records(buf, start: int, end: int):
    """Yield (tag, rec_offset, body_start, body_end) with compression removed.

    A compressed record's body begins with the decompressed size followed by a
    zlib stream; every caller here wants the decoded bytes, so the flag is
    handled once instead of at each call site.
    """
    pos = start
    while pos + RECORD_HEADER <= end:
        tag = bytes(buf[pos:pos + 4])
        size = u32(buf, pos + 4)
        flags = u32(buf, pos + 8)
        if tag == b"GRUP":
            if size < GRUP_HEADER or pos + size > end:
                return
            yield from records(buf, pos + GRUP_HEADER, pos + size)
            pos += size
            continue
        if pos + RECORD_HEADER + size > end:
            return
        body = bytes(buf[pos + RECORD_HEADER:pos + RECORD_HEADER + size])
        if flags & COMPRESSED and len(body) >= 4:
            try:
                body = zlib.decompress(body[4:])
            except zlib.error:
                body = b""
        yield tag, pos, body
        pos += RECORD_HEADER + size


def printable_materials(body: bytes) -> list[str]:
    out, i = [], 0
    while i < len(body):
        if 32 <= body[i] < 127:
            j = body.find(b"\0", i)
            if j < 0:
                break
            text = body[i:j].decode("latin-1")
            if text.lower().endswith(".mat"):
                out.append(text)
            i = j + 1
        else:
            i += 1
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, help="Starfield Data directory")
    a = ap.parse_args()

    esm = Path(a.data) / "Starfield.esm"
    swap_hits: dict[int, list[str]] = {}
    with esm.open("rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as buf:
        header_end = RECORD_HEADER + u32(buf, 4)
        for tag, rec, body in records(buf, header_end, len(buf)):
            if tag != b"LMSW":
                continue
            mats = printable_materials(body)
            if any(m.lower().find(w) >= 0 for m in mats for w in WANT):
                swap_hits[u32(buf, rec + 12)] = mats
        print(f"{len(swap_hits)} LMSW record(s) name a crate lid material")
        for form, mats in sorted(swap_hits.items()):
            print(f"  LMSW {form:08X}: {mats}")

        if not swap_hits:
            return 0

        print("\nforms referencing those swaps:")
        for tag, rec, body in records(buf, header_end, len(buf)):
            if tag == b"LMSW":
                continue
            refs = [f for f in swap_hits if struct.pack("<I", f) in body]
            if refs:
                edid = b""
                idx = body.find(b"EDID")
                if idx >= 0 and idx + 6 <= len(body):
                    ln = struct.unpack_from("<H", body, idx + 4)[0]
                    edid = body[idx + 6:idx + 6 + ln].rstrip(b"\0")
                print(f"  {tag.decode('latin-1')} {u32(buf, rec + 12):08X} {edid.decode('latin-1', 'replace')}"
                      f" -> {['%08X' % f for f in refs]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
