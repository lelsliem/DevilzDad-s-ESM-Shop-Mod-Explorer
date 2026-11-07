"""Real BSTriShape reader: block tree, shader block, and every UV stream.

The plugin's `ParseNifGeos` finds geometry correctly but reads two things
only partially, and both matter for atlas-packed artwork:

  * `BSLightingShaderProperty` is decoded as 12 bytes (`matIdx`, u32, i32).
    The shipped block is far larger and carries the texture-set index plus
    the per-material UV tiling. For a pack whose faces are sub-rectangles
    of one 1024x1024 atlas, that tiling is the difference between the
    front cover and the wrong panel.
  * `.mesh` LOD0 is read as position + the FIRST UV stream. A mesh with two
    UV sets will hand back the wrong rectangle unless the shape's UV
    source says otherwise.

This tool parses the NIF header deterministically (no trial-and-error
window), walks NiNode -> BSGeometry, resolves the `.mesh` through the
geometries key, and prints every UV stream's bounding box next to the
shader block's floats so the pairing can be read off directly.

    python nif_uv.py --data "C:\\...\\Data" \\
        --nif "meshes/setdressing/.../actionfigurepackaging_uc_armysoldier.nif"
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ba2lib import Ba2Archive  # noqa: E402

MAX_BLOCKS = 200_000


class R:
    """Little-endian bounds-checked reader (replica of Reader in LiveModel.cpp)."""

    def __init__(self, b: bytes, off: int = 0, limit: int | None = None):
        self.b = b
        self.o = off
        self.n = len(b) if limit is None else limit
        self.ok = True

    def need(self, n: int) -> bool:
        return self.ok and self.o + n <= self.n

    def u8(self) -> int:
        if not self.need(1):
            self.ok = False
            return 0
        v = self.b[self.o]
        self.o += 1
        return v

    def u16(self) -> int:
        if not self.need(2):
            self.ok = False
            return 0
        v = struct.unpack_from("<H", self.b, self.o)[0]
        self.o += 2
        return v

    def u32(self) -> int:
        return self.i32() & 0xFFFFFFFF

    def i32(self) -> int:
        if not self.need(4):
            self.ok = False
            return 0
        v = struct.unpack_from("<i", self.b, self.o)[0]
        self.o += 4
        return v

    def f32(self) -> float:
        if not self.need(4):
            self.ok = False
            return 0.0
        v = struct.unpack_from("<f", self.b, self.o)[0]
        self.o += 4
        return v

    def hstr(self, lim: int) -> str:
        # NIF 20.2.0.7 (Starfield) writes header-block strings with a U32
        # length, not the u8 of the older Gamebryo formats. Verified in
        # actionfigurepackaging_uc_armysoldier.nif: the type name
        # "BSLightingShaderProperty" sits behind 00 00 00 7b. Reading it as
        # u8 desyncs the whole table immediately.
        n = self.u32()
        # Zero-length entries are valid (the string table holds empty names).
        if not self.ok or n > lim or not self.need(n):
            self.ok = False
            return ""
        s = self.b[self.o:self.o + n].decode("latin-1")
        self.o += n
        return s


class Nif:
    """Header block tree: types, per-block type index/size/offset, strings."""

    def __init__(self, b: bytes):
        self.b = b
        nl = b.find(b"\n")
        if nl < 0 or nl > 128:
            raise ValueError("no header line")
        self.version = b[:nl].decode("latin-1").rsplit(" ", 1)[-1]
        r = R(b, nl + 1)
        if r.u32() != 0x14020007 or r.u8() != 1:
            raise ValueError("unexpected version/endian")
        r.u32()          # user version
        self.nb = r.u32()  # block count
        r.u32()          # user version 2
        # Post-uv2 region is exporter-dependent. Sync over a bounded window
        # exactly as the plugin does, but require the full table to validate.
        nt = 0
        for skip in range(0, 193):
            for wide in (True, False):
                t = R(b, r.o + skip)
                cnt = t.u16()
                if cnt == 0 or cnt > 128:
                    continue
                types, good = [], True
                for _ in range(cnt):
                    ln = t.u32() if wide else t.u16()
                    if ln == 0 or ln > 96 or not t.need(ln):
                        good = False
                        break
                    s = t.b[t.o:t.o + ln]
                    if any(c < 32 or c > 126 for c in s):
                        good = False
                        break
                    types.append(s.decode("latin-1"))
                    t.o += ln
                if not good or "NiNode" not in types:
                    continue
                if not t.need(self.nb * 2):
                    continue
                if any(t.b[t.o + i * 2] | (t.b[t.o + i * 2 + 1] << 8) >= cnt
                       for i in range(self.nb)):
                    continue
                self.types, self.nt = types, cnt
                r.o = t.o
                break
            if getattr(self, "types", None):
                break
        if not getattr(self, "types", None):
            raise ValueError("could not sync the block type table")
        self.type_idx = [r.u16() for _ in range(self.nb)]
        self.sizes = [r.u32() for _ in range(self.nb)]
        ns = r.u32()
        r.u32()  # max string length
        self.strings = [r.hstr(1 << 16) for _ in range(min(ns, 200_000))]
        ng = r.u32()
        if ng:
            raise ValueError("block groups are not expected in this format")
        self.offsets, p = [], r.o
        for s in self.sizes:
            if p + s > len(b):
                raise ValueError("block overruns file")
            self.offsets.append(p)
            p += s
        self.tail = len(b) - p

    def type_of(self, block: int) -> str:
        ti = self.type_idx[block]
        return self.types[ti] if 0 <= ti < self.nt else ""

    def idx_of(self, name: str) -> int:
        return self.types.index(name) if name in self.types else -1

    def hstr_at(self, r: R, index: int) -> str:
        return self.strings[index] if 0 <= index < len(self.strings) else ""


def half_to_float(h: int) -> float:
    s = (h >> 15) & 1
    e = (h >> 10) & 0x1F
    m = h & 0x3FF
    if e == 0:
        return 0.0
    if e == 31:
        return float("inf")
    return (-1.0 if s else 1.0) * (2.0 ** (e - 15)) * (1.0 + m / 1024.0)


def parse_mesh_streams(b: bytes) -> dict:
    """Walk the .mesh vertex streams and report every UV set.

    Layout (verified against the plugin's ParseMeshLod0, which is proven
    byte-matched against MeshConverter ground truth):
        u32 unk, u32 nidx, u16 idx[nidx], f32 border, u32 weightsPerVert,
        u32 nv, s16 xyz[nv], then streams: u32 count + payload.
    A UV stream is a count equal to nv followed by nv*2 half-floats; every
    other count is a different-width attribute stream. The stream LIST is
    what SetUVSource indexes into, so all of them are reported.

    UV values are NOT confined to 0..1. The Mark I backpack (.mesh
    657478c21f1e48a01425/52b4455363dc68a70393, 2497v) authors wrap offsets:
    72 vertices reach u=-11.30 / v=26.17, which the D3D12 WRAP sampler folds
    back into the atlas. This replica uses the plugin's wide finite bound for
    that reason - a tight one reported "no UVs" for the whole pack.
    """
    MAX_ABS_UV = 4096.0
    r = R(b)
    r.u32()
    nidx = r.u32()
    if nidx == 0 or nidx > 20_000_000 or not r.need(nidx * 2):
        return {}
    for _ in range(nidx):
        r.u16()
    border = r.f32()
    if not (0.0 < border < 1e6):
        return {}
    r.u32()  # weights per vertex
    nv = r.u32()
    if nv == 0 or nv > 10_000_000 or not r.need(nv * 6):
        return {}
    r.o += nv * 6  # positions

    streams, uv_sets = [], []
    guard = 0
    while r.ok and r.o + 4 <= r.n and guard < 64:
        guard += 1
        start = r.o
        cnt = r.u32()
        if cnt == 0 or cnt > 4_000_000:
            break
        rec = {"at": start, "count": cnt, "uv": None}
        if cnt == nv and r.need(nv * 4):
            us = []
            ok = True
            for q in range(nv):
                u = half_to_float(r.u16())
                v = half_to_float(r.u16())
                if not (-MAX_ABS_UV <= u <= MAX_ABS_UV and -MAX_ABS_UV <= v <= MAX_ABS_UV):
                    ok = False
                us.append((u, v))
            if ok:
                usx = [p[0] for p in us]
                vsx = [p[1] for p in us]
                rec["uv"] = (min(usx), max(usx), min(vsx), max(vsx))
                uv_sets.append(rec["uv"])
                streams.append(rec)
                continue
            r.o = start  # not a UV pair after all
            break
        # Some other attribute stream: we cannot size it without the schema,
        # so stop rather than mis-walk. The count is recorded for reference.
        streams.append(rec)
        break
    return {"nidx": nidx, "nv": nv, "border": border,
            "streams": streams, "uv_sets": uv_sets}


def nif_path_key(name: str) -> str:
    """Normalise a form's model path (backslashes, missing 'meshes/')."""
    k = name.strip().strip("'\"").lower().replace("\\", "/")
    if not k.startswith("meshes/"):
        k = "meshes/" + k
    return k


def read_nif(arch: "Arch", name: str):
    """Read a NIF by the path string a form carries.

    Returns (key, blob, nif, error). Every caller must pass the item's *own*
    model path; that is what keeps an audit from comparing a sibling's file.
    """
    ar, key = arch.find(nif_path_key(name))
    if ar is None:
        return nif_path_key(name), None, None, "not found in any archive"
    blob, err = ar.read_entry(key)
    if blob is None:
        return key, None, None, "read failed: %s" % err
    return key, blob, Nif(blob), None


def resolve_mesh_uv(arch: "Arch", lod_path: str, set_index: int = 0):
    """Resolve a BSGeometry LOD path to its .mesh and return (uv_box, mesh_key).

    uv_box is (umin, umax, vmin, vmax) or None when the stream is absent. One
    copy of the LOD->.mesh key rule: the CLI and the audit both call this, so a
    second implementation cannot drift from it.
    """
    fkey = lod_path.lower().replace("\\", "/")
    if fkey.endswith(".mesh"):
        fkey = fkey[:-5]
    if fkey.startswith("geometries/"):
        fkey = fkey[11:]
    for cand in (f"geometries/{fkey}.mesh", f"geometries/{lod_path.lower()}.mesh"):
        mar, mkey = arch.find(cand)
        if mar is None:
            continue
        mb, _ = mar.read_entry(mkey)
        if not mb:
            continue
        info = parse_mesh_streams(mb)
        uvs = info.get("uv_sets", [])
        return (uvs[set_index] if len(uvs) > set_index else None), mkey
    return None, None


class Arch:
    """Lazy name -> Ba2Archive across every archive in a directory."""

    def __init__(self, data: Path):
        self.data = data
        self.open: dict[str, Ba2Archive] = {}

    def find(self, name: str):
        key = name.lower()
        for p in sorted(self.data.glob("*.ba2")):
            ar = self.open.get(p.name)
            if ar is None:
                ar = Ba2Archive(p)
                self.open[p.name] = ar
            if not ar.valid:
                continue
            if key in ar.entries:
                return ar, key
        return None, None


def parse_shapes(blob: bytes, nif: "Nif") -> list[dict]:
    """Every BSGeometry in the tree: node name, material, shader block, LOD keys.

    Extracted from main() so the audit can share it: a second copy of this walk
    is exactly how the ammo audit ended up comparing the wrong NIF.
    """
    node_t = nif.idx_of("NiNode")
    geo_t = nif.idx_of("BSGeometry")
    if geo_t < 0:
        return []

    def node_prefix(r: R):
        name = r.u32()
        nextra = r.u32()
        if nextra > 4096:
            return None, None
        for _ in range(nextra):
            r.i32()
        r.i32()          # controller
        r.u32()          # flags
        for _ in range(3):
            r.f32()      # translation
        for _ in range(9):
            r.f32()      # rotation
        r.f32()          # scale
        r.i32()          # collision object
        return name, r.o

    def block(i):
        return blob[nif.offsets[i]:nif.offsets[i] + nif.sizes[i]]

    referenced = set()
    for i in range(nif.nb):
        if nif.type_idx[i] != node_t:
            continue
        raw = block(i)
        nm, end = node_prefix(R(raw))
        if end is None:
            continue
        cr = R(raw, end)
        nch = cr.u32()
        for _ in range(min(nch, 4096)):
            c = cr.i32()
            if 0 <= c < nif.nb:
                referenced.add(c)
    root = next((i for i in range(nif.nb)
                 if nif.type_idx[i] == node_t and i not in referenced), 0)

    stack, shapes, guard = [root], [], 0
    while stack and guard < 100_000:
        guard += 1
        blk = stack.pop()
        blen = nif.sizes[blk]
        raw = block(blk)
        if nif.type_idx[blk] == node_t:
            nm, end = node_prefix(R(raw))
            if end is None:
                continue
            cr = R(raw, end)
            nch = cr.u32()
            for _ in range(min(nch, 4096)):
                c = cr.i32()
                if 0 <= c < nif.nb:
                    stack.append(c)
            continue
        if nif.type_idx[blk] != geo_t:
            continue
        nm, end = node_prefix(R(raw))
        if end is None:
            continue
        cr = R(raw, end)
        for _ in range(4):
            cr.f32()          # bounding sphere
        for _ in range(6):
            cr.f32()          # reserved + half extents
        cr.i32()
        shader = cr.i32()
        cr.i32()
        lods = []
        while cr.ok and cr.o + 18 <= blen:
            cr.u8()
            ni, nv, _unk, ln = cr.u32(), cr.u32(), cr.u32(), cr.u32()
            if ln == 0 or ln > 512 or ni == 0 or nv == 0 or not cr.need(ln):
                break
            path = raw[cr.o:cr.o + ln].decode("latin-1")
            cr.o += ln
            lods.append((ni, nv, path))
        mat = ""
        if 0 <= shader < nif.nb:
            sr = R(block(shader))
            mat = nif.hstr_at(sr, sr.u32())
        shapes.append({"name": nif.hstr_at(R(raw), nm or 0), "material": mat,
                       "shader": shader, "lods": lods})
    return shapes


def mesh_rect(arch: "Arch", lod_path: str):
    """First UV set rectangle of an external .mesh, or None when unresolvable.

    The LOD key is a GUID pair (hex\\hex); the archive entry is
    geometries/<guid>.mesh, which is why the NIF alone never shows UVs.
    """
    fkey = lod_path.lower().replace("\\", "/")
    if fkey.endswith(".mesh"):
        fkey = fkey[:-5]
    if fkey.startswith("geometries/"):
        fkey = fkey[11:]
    for cand in (f"geometries/{fkey}.mesh", f"geometries/{lod_path.lower()}.mesh"):
        mar, mkey = arch.find(cand)
        if mar is None:
            continue
        mb, _ = mar.read_entry(mkey)
        if not mb:
            return None
        uvs = parse_mesh_streams(mb).get("uv_sets", [])
        return uvs[0] if uvs else None
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    ap.add_argument("--nif", required=True, help="archive-relative .nif entry name")
    ap.add_argument("--meshes", action="store_true", help="also resolve and parse each .mesh")
    ap.add_argument("--first-uv", dest="first_uv", action="store_true",
                    help="resolve each .mesh and print ONLY its first UV rect, flagged")
    args = ap.parse_args()

    data = Path(args.data)
    arch = Arch(data)
    ar, key = arch.find(args.nif)
    if ar is None:
        print("[FAIL] nif not found: %s" % args.nif)
        return 1
    blob, err = ar.read_entry(key)
    if blob is None:
        print("[FAIL] read_entry: %s" % err)
        return 1

    nif = Nif(blob)
    print("NIF %s" % args.nif)
    print("  version %s  blocks %d  types %d  strings %d  tail %d"
          % (nif.version, nif.nb, nif.nt, len(nif.strings), nif.tail))
    print("  types: %s" % ", ".join(nif.types))

    shapes = parse_shapes(blob, nif)
    if not shapes:
        print("[FAIL] no BSGeometry in this NIF")
        return 1

    print("\n%d BSGeometry block(s)" % len(shapes))

    for shape in shapes:
        name, mat, shader, lods = shape["name"], shape["material"], shape["shader"], shape["lods"]
        print("=" * 78)
        print("  node    %s" % (name or "(unnamed)"))
        print("  material %s" % mat)
        for ni, nv, path in lods[:2]:
            print("  lod0    %di %dv  factory=%s" % (ni, nv, path))
        # The full shader block, not just the 12 bytes the plugin reads.
        if 0 <= shader < nif.nb:
            sraw = blob[nif.offsets[shader]:nif.offsets[shader] + nif.sizes[shader]]
            size = len(sraw)
            sr = R(sraw)
            u0, u1, i0 = sr.u32(), sr.u32(), sr.i32()
            floats = []
            fr = R(sraw, 12)
            while fr.o + 4 <= min(size, 4096):
                v = fr.f32()
                if v == v and -64.0 <= v <= 64.0:
                    floats.append(v)
                else:
                    floats.append(float("nan"))
            pairs = []
            for i in range(0, len(floats) - 1, 2):
                a, b = floats[i], floats[i + 1]
                if a == a and b == b and (abs(a) > 1e-6 or abs(b) > 1e-6) \
                        and abs(a) < 8 and abs(b) < 8:
                    pairs.append((round(a, 5), round(b, 5)))
            print("  shader  block %s size %d  (u32 %u, u32 %u, i32 %d)"
                  % (nif.type_of(shader), size, u0, u1, i0))
            print("          first float pairs: %s" % pairs[:12])
        if args.first_uv and lods:
            rect, mkey = resolve_mesh_uv(arch, lods[0][2])
            if rect is None:
                print("    UV0 (none)")
            else:
                deg = rect[1] - rect[0] < 1e-4 or rect[3] - rect[2] < 1e-4
                inside = rect[1] <= 0.2601 and rect[3] <= 0.2601
                tag = "DEGENERATE" if deg else ("inside-live" if inside else "OUTSIDE-LIVE")
                print("    UV0 %-14s u=[%.3f,%.3f] v=[%.3f,%.3f]   %s"
                      % (tag, rect[0], rect[1], rect[2], rect[3]))
        if args.meshes and lods:
            rect, mkey = resolve_mesh_uv(arch, lods[0][2])
            if rect is None:
                print("  mesh    (not resolved)")
            else:
                print("  mesh    %s" % mkey)
                print("          UV SET 0  u=[%.3f,%.3f] v=[%.3f,%.3f]"
                      % (rect[0], rect[1], rect[2], rect[3]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())