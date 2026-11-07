"""Scratch: for each problem ARMO, find the LMSW forms it references and print
their material key/override pairs."""
from __future__ import annotations

import pickle
import re
import struct
from pathlib import Path

data = Path(r"B:/SteamLibrary/steamapps/common/Starfield/Data/Starfield.esm").read_bytes()
idx = pickle.load(open("_esm_index.pkl", "rb"))
lmsw = {f for f, (t, _) in idx.items() if t == b"LMSW"}


def record(form: int):
    _, start = idx[form]
    size = struct.unpack_from("<I", data, start + 4)[0]
    end = start + 24 + size
    out, pos = [], start + 24
    while pos + 6 <= end:
        tag = data[pos:pos + 4].decode(errors="replace")
        n = struct.unpack_from("<H", data, pos + 4)[0]
        out.append((tag, data[pos + 6:pos + 6 + n]))
        pos += 6 + n
    return out


def edid(recs) -> str:
    for tag, payload in recs:
        if tag == "EDID":
            return payload.rstrip(b"\0").decode(errors="replace")
    return "?"


def pairs(form: int) -> list[tuple[str, str]]:
    blob = b""
    for tag, payload in record(form):
        if tag == "REFL":
            blob = payload
    strings = []
    i = 0
    while i < len(blob):
        if 32 <= blob[i] < 127:
            j = blob.find(b"\0", i)
            s = blob[i:j].decode("latin-1")
            if s.lower().endswith(".mat"):
                strings.append(s)
            i = j + 1
        else:
            i += 1
    return [(strings[k], strings[k + 1]) for k in range(0, len(strings) - 1, 2)]


items = {
    0x9CC6: "Synth Leatherwear",
    0x3BF6: "Resort Wear 01",
    0x3C4D: "Resort Wear 02",
    0x3E8F: "Star Roamer Space Helmet",
    0x4E78: "Star Roamer Spacesuit",
}
for form, name in items.items():
    recs = record(form)
    refs = []
    for tag, payload in recs:
        for k in range(0, len(payload) - 3, 1):
            v = struct.unpack_from("<I", payload, k)[0]
            if v in lmsw and v != form:
                refs.append(v)
    refs = list(dict.fromkeys(refs))
    print(f"== {form:08X} {name} ({edid(recs)}) lmsw refs: {[f'{r:08X}' for r in refs]}")
    for r in refs:
        print(f"   {r:08X} {edid(record(r))}")
        for key, over in pairs(r):
            print(f"      {key} -> {over}")
