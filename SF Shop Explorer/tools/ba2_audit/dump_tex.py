"""Decode a BC1/BC4 DDS out of a BA2 into a viewable PNG.

Only the formats the preview actually samples are supported (BC1 colour and
BC4 mask). The point is to LOOK at an atlas and its cutout mask - that is how
the action figure packaging front cover was diagnosed.

    python dump_tex.py --ba2 "SFBGS00D - Textures.ba2" \\
        --tex "textures/.../actionfigurepackagingart_color.dds" --out atlas.png
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ba2lib import Ba2Archive  # noqa: E402


def _block_pixels(b: bytes, payload: bytes) -> list[int]:
    """The 16 sub-pixel selectors of one BC block, in D3D raster order.

    Payload byte n holds the four selectors of row n (bits 0-1 leftmost pixel,
    bits 6-7 rightmost). Both decoders below walk all four bytes: an earlier
    version iterated the four bytes as if each held one pixel, which left 12 of
    every 16 pixels black and made a quarter-filled atlas look like padding.
    """
    selectors = []
    for row in range(4):
        byte = payload[row]
        for column in range(4):
            selectors.append((byte >> (2 * column)) & 3)
    return selectors


def bc1_block(b: bytes, out: bytearray, bx: int, by: int, w: int, h: int) -> None:
    """Decode one BC1 block whose *block* coordinates are `bx`, `by`.

    Both decoders take block coordinates and derive the pixel origin here. A
    caller that passed block indices into a pixel-origin argument decoded every
    block starting one pixel to the right of where it belongs, which smears the
    whole image into overlapping stripes.
    """
    x0, y0 = bx * 4, by * 4
    c0, c1 = struct.unpack_from("<HH", b, 0)
    r0, g0, b0, a0 = _rgb565(c0)
    r1, g1, b1, a1 = _rgb565(c1)
    if c0 > c1:
        r2, g2, b2 = (2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3
        r3, g3, b3 = (r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3
    else:
        r2, g2, b2 = (r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2
        r3, g3, b3 = 0, 0, 0  # BC1 3-colour mode: the fourth entry is black
    palette = ((r0, g0, b0), (r1, g1, b1), (r2, g2, b2), (r3, g3, b3))
    for i, sel in enumerate(_block_pixels(b, b[4:8])):
        px = (x0 + (i & 3), y0 + (i >> 2))
        if px[0] >= w or px[1] >= h:
            continue
        r, g, bl = palette[sel]
        o = (px[1] * w + px[0]) * 3
        out[o:o + 3] = bytes((r, g, bl))


def bc4_block(b: bytes, out: bytearray, bx: int, by: int, w: int, h: int) -> None:
    """Decode one BC4 block whose *block* coordinates are `bx`, `by`."""
    x0, y0 = bx * 4, by * 4
    # BC4_UNORM mode 0 palette, exactly as the D3D spec defines it.
    a0, a1 = b[0], b[1]
    if a0 > a1:
        pal = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
    else:
        pal = [a0, a1] + [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]
    # BC4 packs all sixteen selectors as one contiguous 48-bit little-endian
    # run in raster order - unlike BC1, whose selector bytes are per row. Reading
    # it as six bytes per row shifts every row after the first.
    bits = int.from_bytes(b[2:8], "little")
    for i in range(16):
        px = (x0 + (i & 3), y0 + (i >> 2))
        if px[0] >= w or px[1] >= h:
            continue
        v = pal[(bits >> (3 * i)) & 7]
        o = (px[1] * w + px[0]) * 3
        out[o:o + 3] = bytes((v, v, v))


def _rgb565(c: int):
    # BC1 endpoints are RGB565, not 555: green keeps six bits. Reading them as
    # 555 shifted every decoded colour's green and blue channels.
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31, 255)


def write_png(path: Path, rgb: bytes, w: int, h: int) -> None:
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ba2", required=True)
    ap.add_argument("--tex", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--data", default="B:/SteamLibrary/steamapps/common/Starfield/Data")
    args = ap.parse_args()

    ar = Ba2Archive(Path(args.ba2) if Path(args.ba2).exists() else Path(args.data) / args.ba2)
    if not ar.valid:
        print("[FAIL] archive did not parse")
        return 1
    name = args.tex.lower()
    if name not in ar.tex_files:
        print("[FAIL] not in archive: %s" % name)
        return 1
    tf = ar.tex_files[name]
    data, err = ar.read_texture(name)
    if data is None:
        print("[FAIL] read_texture: %s" % err)
        return 1
    mip = next((c for c in tf.chunks if c.start_mip == 0), None)
    w, h = tf.width, tf.height
    bw, bh = (w + 3) // 4, (h + 3) // 4
    need = bw * bh * 8
    if len(data) < need:
        print("[FAIL] short mip: %d < %d" % (len(data), need))
        return 1
    rgb = bytearray(w * h * 3)
    single = tf.dxgi in (80, 81)
    for by in range(bh):
        for bx in range(bw):
            blk = data[(by * bw + bx) * 8:(by * bw + bx) * 8 + 8]
            if single:
                bc4_block(blk, rgb, bx, by, w, h)
            else:
                bc1_block(blk, rgb, bx, by, w, h)
    write_png(Path(args.out), bytes(rgb), w, h)
    print("wrote %s (%dx%d dxgi=%s %s)" % (args.out, w, h, tf.dxgi, "BC4" if single else "BC1"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())