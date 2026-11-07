import struct
import sys
import zlib
from pathlib import Path

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

esm_path = Path(r"B:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm")
out_path = Path(__file__).resolve().parents[1] / "src" / "OmodMnamTable.h"


def subrecords(body):
    pos = 0
    while pos + 6 <= len(body):
        sig = body[pos:pos + 4]
        if not all(0x20 <= c < 0x7F for c in sig):
            break
        size = struct.unpack_from("<H", body, pos + 4)[0]
        if pos + 6 + size > len(body):
            break
        yield sig, body[pos + 6:pos + 6 + size]
        pos += 6 + size


data = esm_path.read_bytes()
pairs = {}
standard_model_omods = set()


def handle_record(pos, size, flags, form_id):
    body = data[pos:pos + size]
    if flags & 0x00040000 and len(body) >= 4:
        try:
            body = zlib.decompress(body[4:])
        except zlib.error:
            pass

    edid = ""
    model = ""
    mnam = 0
    for sig, subdata in subrecords(body):
        if sig == b"EDID":
            edid = subdata.split(b"\0", 1)[0].decode("ascii", "replace")
        elif sig == b"MODL":
            model = subdata.split(b"\0", 1)[0].decode("ascii", "replace")
        elif sig == b"MNAM" and len(subdata) >= 4:
            mnam = struct.unpack_from("<I", subdata)[0]

    if mnam:
        pairs.setdefault(form_id, mnam)
    if edid.lower().startswith("mod_") and "standard" in edid.lower() and model:
        standard_model_omods.add(form_id)


def walk(pos, end):
    while pos + 24 <= end:
        sig = data[pos:pos + 4]
        if sig == b"GRUP":
            group_size = struct.unpack_from("<I", data, pos + 4)[0]
            if group_size < 24 or pos + group_size > end:
                break
            walk(pos + 24, pos + group_size)
            pos += group_size
        elif sig == b"TES4" or all(chr(c).isalnum() for c in sig):
            size, flags, form_id = struct.unpack_from("<III", data, pos + 4)
            if size > 64 * 1024 * 1024 or pos + 24 + size > end:
                pos += 1
                continue
            if sig == b"OMOD":
                handle_record(pos + 24, size, flags, form_id)
            pos += 24 + size
        else:
            pos += 1


walk(0, len(data))
rows = sorted(pairs.items())
standard_rows = sorted(standard_model_omods)
print(f"entries: {len(rows)}; model-bearing standard OMODs: {len(standard_rows)}")

lines = [
    "#pragma once",
    "",
    "#include <cstdint>",
    "",
    "namespace SFE",
    "{",
    "\t// OMOD formID -> MNAM keyword formID (ma_<Weapon>), generated from",
    "\t// Starfield.esm by tools/gen_omod_mnam_table.py. Sorted for binary search.",
    "\tstruct OmodMnamEntry",
    "\t{",
    "\t\tstd::uint32_t omod;",
    "\t\tstd::uint32_t mnam;",
    "\t};",
    "",
    "\tinline constexpr OmodMnamEntry kOmodMnamTable[] = {",
]
lines.extend(f"\t\t{{ 0x{omod:08X}u, 0x{mnam:08X}u }}," for omod, mnam in rows)
lines.extend([
    "\t};",
    "",
    "\t// Model-bearing standard OMODs, used to resolve shared-model defaults",
    "\t// independently of the runtime form-array iteration order.",
    "\tinline constexpr std::uint32_t kStandardModelOmods[] = {",
])
lines.extend(f"\t\t0x{form_id:08X}u," for form_id in standard_rows)
lines.extend([
    "\t};",
    "",
    "\tinline bool IsStandardModelOmod(std::uint32_t a_omod)",
    "\t{",
    "\t\tstd::uint32_t lo = 0;",
    "\t\tstd::uint32_t hi = static_cast<std::uint32_t>(sizeof(kStandardModelOmods) / sizeof(kStandardModelOmods[0]));",
    "\t\twhile (lo < hi) {",
    "\t\t\tconst auto mid = lo + (hi - lo) / 2;",
    "\t\t\tif (kStandardModelOmods[mid] < a_omod) {",
    "\t\t\t\tlo = mid + 1;",
    "\t\t\t} else {",
    "\t\t\t\thi = mid;",
    "\t\t\t}",
    "\t\t}",
    "\t\treturn lo < sizeof(kStandardModelOmods) / sizeof(kStandardModelOmods[0]) && kStandardModelOmods[lo] == a_omod;",
    "\t}",
    "",
    "\tinline std::uint32_t LookupOmodMnam(std::uint32_t a_omod)",
    "\t{",
    "\t\tstd::uint32_t lo = 0;",
    "\t\tstd::uint32_t hi = static_cast<std::uint32_t>(sizeof(kOmodMnamTable) / sizeof(kOmodMnamTable[0]));",
    "\t\twhile (lo < hi) {",
    "\t\t\tconst auto mid = lo + (hi - lo) / 2;",
    "\t\t\tif (kOmodMnamTable[mid].omod < a_omod) {",
    "\t\t\t\tlo = mid + 1;",
    "\t\t\t} else {",
    "\t\t\t\thi = mid;",
    "\t\t\t}",
    "\t\t}",
    "\t\tif (lo < sizeof(kOmodMnamTable) / sizeof(kOmodMnamTable[0]) && kOmodMnamTable[lo].omod == a_omod) {",
    "\t\t\treturn kOmodMnamTable[lo].mnam;",
    "\t\t}",
    "\t\treturn 0;",
    "\t}",
    "}",
])
out_path.write_text("\n".join(lines) + "\n", encoding="ascii")
print("wrote", out_path)
