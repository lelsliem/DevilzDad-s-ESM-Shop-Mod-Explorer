"""Scratch: index ARMA/ARMO record offsets in Starfield.esm by form ID."""
from __future__ import annotations

import pickle
import struct
import sys
from pathlib import Path

path = Path(r"B:/SteamLibrary/steamapps/common/Starfield/Data/Starfield.esm")
data = path.read_bytes()
want = {b"ARMA", b"ARMO", b"LMSW", b"TXST", b"MSWP", b"MATO"}
out: dict[int, tuple[bytes, int]] = {}


def walk(pos: int, end: int) -> None:
    while pos + 24 <= end:
        tag = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        if tag == b"GRUP":
            walk(pos + 24, pos + size)
            pos += size
            continue
        if tag in want:
            formid = struct.unpack_from("<I", data, pos + 12)[0]
            out[formid] = (tag, pos)
        pos += 24 + size


header = struct.unpack_from("<I", data, 4)[0]
walk(24 + header, len(data))
print("indexed", len(out))
with open("_esm_index.pkl", "wb") as fh:
    pickle.dump(out, fh)
for f in sys.argv[1:]:
    key = int(f, 0)
    print(f, out.get(key))
