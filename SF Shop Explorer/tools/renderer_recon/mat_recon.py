"""Locate the Rattler material inside materialsbeta.cdb and resolve its texture ids.

The CDB ('BETH' magic) is Bethesda's content database: string table + typed
records with 4-char type tags (here: 'DIFF'). Texture paths ('textures/...')
should also live in the string table; the goal is to connect a material
record's DIFF fields to texture path strings, or to find how texture names are
referenced (string-table offsets like the 0x2EC1C / 0x7B918 seen near names).
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")

raw = open(CDB, "rb").read()
print("cdb size:", len(raw))

# 1) Are texture paths stored as strings?
for probe in (b"textures/", b"textures\\", b".dds", b"Rattler_Slideandlower_Material1"):
    i = raw.find(probe)
    print(f"probe {probe!r}: first at {i}")
    if i >= 0:
        print("   ctx:", raw[max(0, i - 64):i + 96])

# 2) Dump the region around the rattler material name to see the record layout
j = raw.find(b"Rattler_Slideandlower_Material1")
if j >= 0:
    lo = max(0, j - 512)
    hi = min(len(raw), j + 512)
    seg = raw[lo:hi]
    print("\n== context window ==")
    # show printable runs to visualize the string table structure
    for m in re.finditer(rb"[ -~]{4,}", seg):
        print(f"  +{lo + m.start():08X}: {m.group().decode('latin-1')[:100]}")

# 3) The DIFF fields contained 0x02EC0F-style values — check if they index a
#    string table whose base we can find ('STRT' chunk was at file start).
str_base = raw.find(b"STRT")
print("\nSTRT chunk at:", str_base)
