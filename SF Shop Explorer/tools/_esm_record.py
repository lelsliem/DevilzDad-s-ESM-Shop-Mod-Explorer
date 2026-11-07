"""Scratch: walk one Starfield record and dump every subrecord payload in hex."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

path = Path(r"B:/SteamLibrary/steamapps/common/Starfield/Data/Starfield.esm")
data = path.read_bytes()
start = int(sys.argv[1], 0)
size = struct.unpack_from("<I", data, start + 4)[0]
formid = struct.unpack_from("<I", data, start + 12)[0]
print(f"record {data[start:start+4].decode(errors='replace')} size=0x{size:x} form={formid:08X} at 0x{start:x}")


def printable(b: bytes) -> str:
    return "".join(chr(c) if 32 <= c < 127 else "." for c in b)


pos = start + 24
end = start + 24 + size
while pos + 6 <= end:
    tag = data[pos:pos + 4]
    if not all(65 <= c <= 90 or 48 <= c <= 57 or c == 95 or c == 32 for c in tag):
        print(f"  ?? non-tag at +0x{pos-start:x}: {printable(data[pos:pos+16])}")
        break
    sub = struct.unpack_from("<H", data, pos + 4)[0]
    payload = data[pos + 6:pos + 6 + sub]
    extra = ""
    if sub == 4:
        extra = f" form={struct.unpack_from('<I', payload, 0)[0]:08X}"
    elif sub in (8, 12):
        extra = " hex=" + payload.hex()
    print(f"  {tag.decode(errors='replace')} +0x{pos-start:03x} size={sub:4d}{extra} {printable(payload[:72])}")
    pos += 6 + sub
print("ended at", hex(pos - start), "expected", hex(end - start))
