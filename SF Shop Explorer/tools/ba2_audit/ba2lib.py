"""Offline replica of the plugin's BA2 reader (src/LiveModel.cpp).

Purpose: reproduce OpenBa2 / Ba2Read / the DX10 chunk walk on the command
line so archive-format assumptions can be regression-tested without
launching Starfield. Every constant and field offset here mirrors the C++
one-for-one; if you change one, change the other and re-run ba2_audit.py.

Format facts proven against the shipped archives (see
SHOP-RENDERING-NOTES.md 6r-5j):

  * GNRL entry size (record +24) is the DECOMPRESSED length. Starfield -
    Materials.ba2 stores one entry at offset 68 with size 105,037,616
    while the stream is 17,576,587 bytes and 68 + 17,576,587 lands
    exactly on the name table. Reading `size` bytes short-reads.
  * The name-table offset bounds the bulk region, so the true on-disk
    length of any entry is min(size, data_end - offset).
  * DX10 (texture) archives come in two generations. v3 chunk records
    carry an absolute u64 file offset; v2 carries none and the bulk base
    is recovered as name_table_offset - sum(packed or size).
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

GNRL_ENTRY_STRIDE = 36
GNRL_OFF_AT = 12
GNRL_ON_DISK_SIZE_AT = 20  # discovered: the real stream length
GNRL_SIZE_AT = 24  # discovered: the DECOMPRESSED length
GNRL_SIZE_MASK = 0x3FFFFFFF

TEX_HDR_STRIDE = 24  # both generations: the C++ reads a fixed 24-byte header
TEX_CHUNK_STRIDE = 24
MAX_CHUNKS = 64
MAX_DIM = 16384

# Guards copied from the C++ so the harness fails the same way it does live.
MAX_ENTRY = 1 << 31
MAX_INFLATE = 1 << 29


@dataclass
class Entry:
    offset: int
    on_disk: int
    size: int
    ext: bytes = b""


@dataclass
class Chunk:
    off: int
    packed: int
    size: int
    start_mip: int
    end_mip: int


@dataclass
class TexFile:
    width: int = 0
    height: int = 0
    mips: int = 0
    dxgi: int = 0
    chunks: list = field(default_factory=list)


@dataclass
class Reject:
    index: int
    name: str
    reason: str


def map_tex_format(dxgi: int) -> int:
    """Replica of SFE::LiveModel's MapTexFormat."""
    return {
        71: 71, 72: 71,   # BC1
        74: 74, 75: 74,   # BC2
        77: 77, 78: 77,   # BC3
        80: 80,           # BC4
        83: 83,           # BC5
        # BC5_UNORM_SRGB. The archives tag normal maps sRGB although both
        # channels are vector components; the C++ side samples them linear and
        # this replica has to agree, or the archive gate skips exactly the maps
        # that carry relief (the Erdebrau can's pull ring is one).
        84: 83,           # BC5, read as linear vectors
        # BC7. Repacked mod texture archives tag almost every map BC7_UNORM_SRGB
        # (10,230 entries in the 2026-10-23 profile), and treating it as
        # unsupported dropped the entire archive. Same linear policy as 72, 78
        # and 84. The result must be 98 (BC7_UNORM), not 97: 97 is
        # BC7_TYPELESS, and a typeless SRV format is invalid in the preview's
        # UploadTextureImage - that was the A-300 device removal.
        97: 98, 98: 98, 99: 98,   # BC7_UNORM
    }.get(dxgi, 0)


def tex_row_pitch(fmt: int, width: int) -> int:
    """Replica of SFE::LiveModel's TexRowPitch."""
    if fmt == 28:
        return width * 4
    bb = 8 if fmt in (71, 72, 80) else 16
    return ((width + 3) // 4) * bb


def surface_bytes(tf: "TexFile") -> int:
    """Bytes the reader actually needs: pitch * rows (BC formats)."""
    fmt = map_tex_format(tf.dxgi)
    if fmt == 0:
        return 0
    if fmt == 28:
        return tex_row_pitch(fmt, tf.width) * tf.height
    return tex_row_pitch(fmt, tf.width) * ((tf.height + 3) // 4)


def lz4_block_decompress(src: bytes, out_size: int) -> bytes:
    """Minimal LZ4 block decoder (matches LZ4_decompress_safe).

    v3 archives declare method=3 and the plugin calls
    LZ4_decompress_safe on the chunk; this lets the harness exercise that
    same path without a native dependency.
    """
    dst = bytearray()
    i = 0
    n = len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        dst += src[i : i + lit]
        i += lit
        if i >= n:
            break
        offset = src[i] | (src[i + 1] << 8)
        i += 2
        if offset == 0:
            raise ValueError("lz4: zero match offset")
        match = token & 0x0F
        if match == 15:
            while True:
                b = src[i]
                i += 1
                match += b
                if b != 255:
                    break
        match += 4  # minmatch
        start = len(dst) - offset
        if start < 0:
            raise ValueError("lz4: match before start of output")
        for k in range(match):
            dst.append(dst[start + k])
    if out_size and len(dst) != out_size:
        raise ValueError(f"lz4: produced {len(dst)} != expected {out_size}")
    return bytes(dst)


class Ba2Error(Exception):
    pass


def _u16(b: bytes, at: int) -> int:
    return struct.unpack_from("<H", b, at)[0]


def _u32(b: bytes, at: int) -> int:
    return struct.unpack_from("<I", b, at)[0]


def _u64(b: bytes, at: int) -> int:
    return struct.unpack_from("<Q", b, at)[0]


class Ba2Archive:
    """Mirrors SFE::LiveModel's Ba2Archive / OpenBa2 / Ba2Read / ReadTexture."""

    def __init__(self, path: str | Path, verbose: bool = False):
        self.path = Path(path)
        self.verbose = verbose
        self.valid = False
        self.is_tex = False
        self.method = 0
        self.version = 0
        self.data_end = 0
        self.header_bytes = 36
        self.num = 0
        self.entries: dict[str, Entry] = {}
        self.tex_files: dict[str, TexFile] = {}
        self.rejects: list[Reject] = []
        self._open()

    # -- header ---------------------------------------------------------
    def _open(self) -> None:
        with self.path.open("rb") as f:
            # The original BTDX header is 24 bytes; v2 grew it to 36 and v3 put
            # the compression method at +32. Reading 36 unconditionally left
            # v1 TOCs shifted by 12 bytes, so every entry parsed as garbage
            # (the Star Wars Genesis compendium ships its whole armour texture
            # set as v1).
            hdr = f.read(24)
            if len(hdr) < 24 or hdr[:4] != b"BTDX":
                return
            self.version = _u32(hdr, 4)
            self.is_tex = hdr[8:12] == b"DX10"
            self.num = _u32(hdr, 12)
            self.data_end = _u64(hdr, 16)
            self.header_bytes = 24 if self.version == 1 else 36
            if self.header_bytes == 36:
                tail = f.read(12)
                if len(tail) < 12:
                    return
                if self.version >= 3:
                    self.method = _u32(tail, 8)
            if self.num == 0 or self.num > 4_000_000:
                return

            raw_entries = self._walk_entries(f)
            names = self._walk_names(f)
            if raw_entries is None or names is None:
                return

            if self.is_tex and self.version == 2:
                if not self._resolve_v2_bulk(raw_entries):
                    return

            for name, ent in zip(names, raw_entries):
                if ent is None:
                    continue
                if self.is_tex:
                    self.tex_files[name.lower()] = ent
                else:
                    self.entries[name.lower()] = ent
            self.valid = True

    def _walk_names(self, f) -> list[str] | None:
        f.seek(self.data_end)
        names: list[str] = []
        for _ in range(self.num):
            raw = f.read(2)
            if len(raw) < 2:
                return None
            ln = _u16(raw, 0)
            if ln == 0 or ln > 512:
                return None
            nm = f.read(ln)
            if len(nm) < ln:
                return None
            names.append(nm.decode("latin-1"))
        return names

    # -- GNRL -----------------------------------------------------------
    def _walk_entries(self, f) -> list:
        out: list = [None] * self.num
        if not self.is_tex:
            f.seek(self.header_bytes)
            # v1 general records lead with a name hash, shifting offset and
            # lengths by four bytes (one such archive ships 118 textures in
            # the Genesis compendium).
            legacy = self.version == 1
            off_at = 16 if legacy else GNRL_OFF_AT
            size_at = 28 if legacy else GNRL_SIZE_AT
            on_disk_at = 24 if legacy else GNRL_ON_DISK_SIZE_AT
            for i in range(self.num):
                rec = f.read(GNRL_ENTRY_STRIDE)
                if len(rec) < GNRL_ENTRY_STRIDE:
                    return None
                off = _u64(rec, off_at)
                size = _u32(rec, size_at) & GNRL_SIZE_MASK
                on_disk = _u32(rec, on_disk_at)
                if size == 0 or size > MAX_ENTRY:
                    continue
                out[i] = Entry(off, on_disk, size, rec[:4])
            return out
        return self._walk_tex_entries(f)

    # -- DX10 -----------------------------------------------------------
    def _walk_tex_entries(self, f) -> list | None:
        # v1 uses the same body/chunk layout as v3 (4-byte prefix, absolute
        # chunk offsets); only the header size differs.
        is_v3 = self.version >= 3 or self.version == 1
        stride = TEX_HDR_STRIDE
        out: list = [None] * self.num
        f.seek(self.header_bytes)
        for i in range(self.num):
            h = f.read(stride)
            if len(h) < stride:
                return None
            nchunks = h[13] if is_v3 else h[9]
            if nchunks > MAX_CHUNKS:
                return None
            width = _u16(h, 16 if is_v3 else 12)
            height = _u16(h, 18 if is_v3 else 14)
            tf = TexFile(
                width=width,
                height=height,
                mips=h[20] if is_v3 else h[16],
                dxgi=h[21] if is_v3 else h[17],
            )
            chunks: list[Chunk] = []
            for _ in range(nchunks):
                rec = f.read(TEX_CHUNK_STRIDE)
                if len(rec) < TEX_CHUNK_STRIDE:
                    return None
                if is_v3:
                    chunks.append(
                        Chunk(
                            off=_u64(rec, 0),
                            packed=_u32(rec, 8),
                            size=_u32(rec, 12),
                            start_mip=_u16(rec, 16),
                            end_mip=_u16(rec, 18),
                        )
                    )
                else:
                    packed = _u32(rec, 4)
                    size = _u32(rec, 8)
                    mips = _u32(rec, 12)
                    chunks.append(
                        Chunk(
                            off=0,
                            packed=packed,
                            size=size,
                            start_mip=mips & 0xFFFF,
                            end_mip=(mips >> 16) & 0xFFFF,
                        )
                    )
            ok = nchunks != 0 and 0 < width <= MAX_DIM and 0 < height <= MAX_DIM
            if ok:
                for c in chunks:
                    if c.size == 0 or c.size > (1 << 28) or c.packed > (1 << 28):
                        ok = False
                        break
                    tf.chunks.append(c)
            if not ok or not tf.chunks:
                self.rejects.append(Reject(i, "", "tex entry failed header/chunk validation"))
                continue
            out[i] = tf
        return out

    def _resolve_v2_bulk(self, raw_entries: list) -> bool:
        total = 0
        for ent in raw_entries:
            if ent is None:
                continue
            for c in ent.chunks:
                total += c.packed if c.packed != 0 else c.size
        if total == 0 or total > self.data_end:
            return False
        base = self.data_end - total
        cum = 0
        for ent in raw_entries:
            if ent is None:
                continue
            for c in ent.chunks:
                c.off = base + cum
                cum += c.packed if c.packed != 0 else c.size
        return True

    # -- reads ----------------------------------------------------------
    def read_entry(self, name: str) -> tuple[bytes | None, str]:
        """Replica of Ba2Read. Returns (payload, reason-if-failed)."""
        ent = self.entries.get(name.lower())
        if ent is None:
            return None, "not found"
        if ent.size == 0 or ent.size > MAX_ENTRY:
            return None, "size out of range"
        avail = self.data_end - ent.offset if self.data_end > ent.offset else ent.size
        stream_len = min(ent.size, avail)
        if stream_len == 0:
            return None, "empty stream span"
        with self.path.open("rb") as f:
            f.seek(ent.offset)
            blob = f.read(stream_len)
        if len(blob) != stream_len:
            return None, f"short read {len(blob)}/{stream_len}"
        if blob[0] != 0x78:
            return blob, ""
        if self.method == 3:
            try:
                return lz4_block_decompress(blob, ent.size if ent.size > stream_len else 0), ""
            except (ValueError, IndexError) as exc:
                return None, f"lz4 error: {exc}"
        exact = ent.size > stream_len
        cap = ent.size if exact else ent.size * 4 + 1024
        for _ in range(8):
            if cap > MAX_INFLATE:
                return None, "inflate buffer cap exceeded"
            # decompressobj, not zlib.decompress: the second positional
            # argument of zlib.decompress is wbits, not a buffer cap.
            try:
                raw = zlib.decompressobj().decompress(blob, cap)
            except zlib.error as exc:
                if exact:
                    return None, f"zlib error: {exc}"
                cap *= 4
                continue
            return raw, ""
        return None, "inflate exhausted"

    def best_chunk(self, tf: TexFile) -> Chunk | None:
        best: Chunk | None = None
        for c in tf.chunks:
            if c.start_mip == 0:
                return c
            if best is None or c.size > best.size:
                best = c
        return best

    def read_texture(self, name: str) -> tuple[bytes | None, str]:
        """Replica of the DX10 branch of ReadTexture (best chunk only)."""
        tf = self.tex_files.get(name.lower())
        if tf is None:
            return None, "not found"
        if tf.width == 0 or tf.height == 0 or not tf.chunks:
            return None, "empty tex file"
        best = self.best_chunk(tf)
        if best is None or best.size == 0:
            return None, "no usable chunk"
        want = best.packed if best.packed else best.size
        if best.off + want > self.data_end:
            return None, f"chunk overruns bulk end (off {best.off} + {want} > {self.data_end})"
        with self.path.open("rb") as f:
            f.seek(best.off)
            packed = f.read(want)
        if len(packed) != want:
            return None, f"short read {len(packed)}/{want}"
        if best.packed != 0 and best.packed != best.size:
            try:
                if self.method == 3:
                    raw = lz4_block_decompress(packed, best.size)
                else:
                    raw = zlib.decompress(packed)
                    if len(raw) != best.size:
                        raise ValueError(f"inflated {len(raw)} != size {best.size}")
            except (zlib.error, ValueError, IndexError) as exc:
                kind = "lz4" if self.method == 3 else "zlib"
                return None, f"chunk {kind} error: {exc}"
            return raw, ""
        return packed, ""

    # -- reporting ------------------------------------------------------
    def describe(self) -> str:
        kind = "DX10" if self.is_tex else "GNRL"
        return (
            f"{self.path.name}\n"
            f"  header    v{self.version} {kind} num={self.num} "
            f"data_end={self.data_end} filesize={self.path.stat().st_size}\n"
            f"  parsed    {len(self.entries)} gnrl / {len(self.tex_files)} tex"
            + (f"   method={self.method} (3=LZ4)" if self.method == 3 else "")
            + "\n"
        )
