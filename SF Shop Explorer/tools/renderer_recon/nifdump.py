"""Parse the extracted rattler.nif: block tree, shader property, texture paths.

NIF 20.2.0.7 (Starfield / FO4-family) parser — just enough to answer:
  1. block list + type indices,
  2. node names + parent/child links,
  3. BSTriShape vertex/index layout flags,
  4. BSLightingShaderProperty -> material name (.mat path) and texture set paths.

Writes a human-readable dump to stdout and NIFDUMP.txt.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NIF = os.path.join(HERE, "extracted", "rattler.nif")


class R:
    def __init__(self, b):
        self.b = b
        self.o = 0

    def u8(self):
        v = self.b[self.o]
        self.o += 1
        return v

    def u16(self):
        v = struct.unpack_from("<H", self.b, self.o)[0]
        self.o += 2
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.o)[0]
        self.o += 4
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.o)[0]
        self.o += 4
        return v

    def f32(self):
        v = struct.unpack_from("<f", self.b, self.o)[0]
        self.o += 4
        return v

    def s(self, ln=None):
        if ln is None:
            ln = self.u16()
        v = self.b[self.o:self.o + ln].decode("latin-1")
        self.o += ln
        return v

    def skip(self, n):
        self.o += n


def parse(path):
    b = open(path, "rb").read()
    r = R(b)
    header = r.s(0x0D)  # "Gamebryo File " — length fixed? read to \n instead
    # read header line up to \n
    r.o = 0
    nl = b.index(b"\n")
    header_line = b[:nl].decode("latin-1")
    r.o = nl + 1
    print(header_line)
    ver = header_line.rsplit(" ", 1)[-1]  # "20.2.0.7"
    assert ver == "20.2.0.7", ver

    r.skip(1)  # unknown \x07? actually: after header line: 07 00 02 ...
    # Standard 20.2.0.7 layout after header line:
    #   byte 07 (unk), u16 endian=0x0002? then u8 userVer=20? Let's follow FO4 layout:
    # 0x07 0x00 0x02 0x14 0x01 0x0C 0x00 0x00 0x00 ...
    #   unk07=07, BSVersion u16? ... Use the known FO4 field map:
    r.skip(1)   # 0x07 unk
    r.skip(2)   # endian? 00 02
    r.skip(1)   # user version 0x14 = 20
    r.skip(1)   # 0x01 unk
    num_blocks = r.u32()
    r.skip(2)   # u16? 00 00
    r.skip(1)   # 0x01 author flag? actually...
    # The bytes: 07 00 02 14 01 | 0C 00 00 00 | 34 00 00 00 | AD 00 00 00 | 01 00 03 00 00 00 01 00
    # Known FO4 header after header line:
    #   u8  0x07 (unknown const)
    #   u16 endian = 0x0002? no: u8 0x00, u8 0x02 = endian 2? Actually u16 0x0200 LE = 512...
    # Simplify: use positions empirically: after \n:
    #   +0: 07 (unk)
    #   +1: 00 02 (unk2)
    #   +3: 14 (user ver 20)
    #   +4: 01 (BSVer? = 1 -> FO4+)
    #   +5: u32 num blocks = 0x0C = 12
    #   +9: u32 user ver 2 = 0x34 = 52
    #   +13: u32 export info len? = 0xAD = 173
    #   +17: 01 (export info present)
    #   +18: u16 03 00 (creator len 3?)
    r.skip(5)
    num_blocks = r.u32()
    user2 = r.u32()
    r.skip(2)   # 00 00 ?
    r.skip(1)
    r.skip(2)
    print(f"num_blocks={num_blocks} user2={user2}")
    export_len = r.u32()
    r.skip(export_len)
    print(f"export info skipped ({export_len})")

    # block type map
    num_types = r.u16()
    types = [r.s() for _ in range(num_types)]
    print(f"{num_types} block types:", types)

    type_idx = [r.u16() for _ in range(num_blocks)]
    block_sizes = [r.u32() for _ in range(num_blocks)]
    r.skip(num_blocks)  # meta flags
    r.skip(num_blocks)  # arg1 (u8 each? in FO4: 4 bytes per block? verify empirically)
    print("type idx:", type_idx)
    print("sizes:", block_sizes)
    return b, r, types, type_idx, block_sizes


if __name__ == "__main__":
    parse(NIF)
