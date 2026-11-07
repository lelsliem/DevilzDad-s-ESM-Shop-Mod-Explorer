"""Probe real Starfield asset formats for the shop's own renderer.

Writes findings to tools/renderer_recon/RECON.md:
  1. Locate the game Data dir and list candidate BA2 archives.
  2. Extract a real weapon NIF (Rattler), its BGSM/BGTE material, and DDS textures.
  3. Dump the NIF header/block structure and the BGSM fields we need.
"""
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "extracted")


def find_data_dir():
    candidates = [
        r"B:\SteamLibrary\steamapps\common\Starfield\Data",
        r"C:\Program Files (x86)\Steam\steamapps\common\Starfield\Data",
    ]
    for c in candidates:
        if os.path.isdir(c):
            return c
    # fall back: search a few roots shallowly for Starfield.exe
    for root in ("B:\\", "C:\\", "D:\\"):
        for dirpath, dirnames, filenames in os.walk(root):
            if "Starfield.exe" in filenames and os.path.isdir(os.path.join(dirpath, "Data")):
                return os.path.join(dirpath, "Data")
            if dirpath.count(os.sep) - root.count(os.sep) >= 4:
                dirnames[:] = []
    return None


def open_ba2(path):
    # Proven BTDX v2 layout (matches tools/ba2_extract/ba2_extract_files.py):
    # 36-byte header, then `num` 36-byte entries, then the name table at nt_off.
    f = open(path, "rb")
    header = f.read(36)
    magic, ver, typ, num = struct.unpack("<4sI4sI", header[:16])
    assert magic == b"BTDX", f"{path}: bad magic {magic!r}"
    nt_off = struct.unpack("<Q", header[16:24])[0]
    entries = []
    for _ in range(num):
        e = f.read(36)
        ext = e[0:4].rstrip(b"\0").decode("latin-1")
        off = struct.unpack_from("<Q", e, 12)[0]
        size = struct.unpack_from("<I", e, 24)[0]
        entries.append(["", ext, off, size])
    f.seek(nt_off)
    for i in range(num):
        ln = struct.unpack("<H", f.read(2))[0]
        entries[i][0] = f.read(ln).decode("latin-1")
    return f, entries


def read_ba2_entry(f, ext, off, size):
    f.seek(off)
    raw = f.read(size)
    if raw[:1] == b"\x78":  # per-entry zlib (BA2 compression flag)
        try:
            return zlib.decompress(raw)
        except OSError:
            pass
    return raw


def main():
    data = find_data_dir()
    if not data:
        print("no Data dir found")
        return 1
    print(f"Data dir: {data}")
    ba2s = sorted(x for x in os.listdir(data) if x.lower().endswith(".ba2"))
    print(f"{len(ba2s)} archives")

    target_archives = [x for x in ba2s if x.startswith("Starfield - Meshes")]
    print("mesh archives:", target_archives)

    rattler = None
    for name in target_archives:
        path = os.path.join(data, name)
        f, entries = open_ba2(path)
        for (n, ext, off, size) in entries:
            if n.lower().endswith("rattler.nif") or (rattler is None and n.lower().endswith(".nif") and "rattler" in n.lower()):
                rattler = (path, n, ext, off, size)
                break
        f.close()
        if rattler:
            break
    if not rattler:
        print("no rattler nif found")
        return 1
    print("rattler:", rattler)

    f, entries = open_ba2(rattler[0])
    data_bytes = read_ba2_entry(f, rattler[2], rattler[3], rattler[4])
    os.makedirs(OUT, exist_ok=True)
    out_nif = os.path.join(OUT, "rattler.nif")
    with open(out_nif, "wb") as o:
        o.write(data_bytes)
    print(f"wrote {out_nif} ({len(data_bytes)} bytes)")

    # Dump NIF header structure
    b = data_bytes
    print("\n== NIF header ==")
    print("NL ver:", struct.unpack_from("<I", b, 0)[0])
    print("ver1/2/3:", struct.unpack_from("<BBBB", b, 4))
    print("ver4:", struct.unpack_from("<H", b, 8)[0])
    ver5, = struct.unpack_from("<H", b, 10)
    print("ver5:", ver5)
    off = 12
    if ver5 == 0x001F:
        # NIF v20+ / SF
        enc = b[off:off + 2]
        off += 2
        print("encoding:", enc)
        # read as NetImmerse/native format... Starfield NIF uses standard header
    # generic: dump first 512 bytes hex
    print(b[:256].hex(" "))
    f.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
