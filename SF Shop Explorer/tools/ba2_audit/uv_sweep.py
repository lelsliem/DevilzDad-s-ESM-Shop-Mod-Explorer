"""UV stream table for every BSTriShape in a NIF, with the plugin's verdict.

nif_uv.py prints one NIF; this prints the per-shape UV stream table for all
of them and says which stream the new ParseMeshLod0 rule would take, so the
atlas sub-rectangle story can be checked face by face instead of guessed at.

Uses the cached name index from uv_stream_check.py, because opening a BA2
just to resolve one .mesh is the slow part.

    python uv_sweep.py --data "C:\\...\\Data" \\
        --nif meshes/setdressing/sfbgs00d/actionfigures_packaging/actionfigurepackaging_uc_armysoldier.nif
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ba2lib import Ba2Archive  # noqa: E402
from nif_uv import Nif, parse_mesh_streams  # noqa: E402
from uv_stream_check import TARGETS as _UNUSED  # noqa: F401,E402

IDX = os.path.join(HERE, "extracted", "ba2nameindex.tsv")


def load_index() -> dict[str, str]:
    owner: dict[str, str] = {}
    with open(IDX, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            name, _, arc = line.rstrip("\n").rpartition("\t")
            if name and arc:
                owner[name] = arc
    return owner


def node_prefix(r):
    name = r.u32()
    nextra = r.u32()
    if nextra > 4096:
        return None, None
    for _ in range(nextra):
        r.i32()
    r.i32()
    r.u32()
    for _ in range(3):
        r.f32()
    for _ in range(9):
        r.f32()
    r.f32()
    r.i32()
    return name, r.o


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--nif", required=True)
    a = ap.parse_args()
    data = Path(a.data)
    owner = load_index()
    print("name index: %d entries" % len(owner))

    arcname = owner.get(a.nif.lower())
    if not arcname:
        print("[FAIL] nif not in index: %s" % a.nif)
        return 1
    ar = Ba2Archive(data / arcname)
    blob, err = ar.read_entry(a.nif.lower())
    print("nif %s (%s, %s)" % (a.nif, arcname, err or "ok"))
    if not blob:
        return 1
    nif = Nif(blob)
    node_t, geo_t = nif.idx_of("NiNode"), nif.idx_of("BSGeometry")
    print("blocks %d types %d; %d BSGeometry" % (nif.nb, nif.nt, sum(1 for i in range(nif.nb) if nif.type_idx[i] == geo_t)))

    def blk(i):
        return blob[nif.offsets[i]:nif.offsets[i] + nif.sizes[i]]

    referenced = set()
    for i in range(nif.nb):
        if nif.type_idx[i] != node_t:
            continue
        raw = blk(i)
        _, end = node_prefix(_R(raw))
        if end is None:
            continue
        cr = _R(raw, end)
        for _ in range(min(cr.u32(), 4096)):
            c = cr.i32()
            if 0 <= c < nif.nb:
                referenced.add(c)
    root = next((i for i in range(nif.nb) if nif.type_idx[i] == node_t and i not in referenced), 0)

    meshes: dict[str, Ba2Archive] = {}
    stats = {"degenerate": 0, "ok": 0, "missing": 0, "nou": 0}
    stack, rows, guard = [root], [], 0
    while stack and guard < 100000:
        guard += 1
        i = stack.pop()
        raw = blk(i)
        if nif.type_idx[i] == node_t:
            _, end = node_prefix(_R(raw))
            if end is None:
                continue
            cr = _R(raw, end)
            for _ in range(min(cr.u32(), 4096)):
                c = cr.i32()
                if 0 <= c < nif.nb:
                    stack.append(c)
            continue
        if nif.type_idx[i] != geo_t:
            continue
        nm, end = node_prefix(_R(raw))
        if end is None:
            continue
        cr = _R(raw, end)
        for _ in range(4):
            cr.f32()
        for _ in range(6):
            cr.f32()
        cr.i32()
        shader = cr.i32()
        cr.i32()
        lod = None
        while cr.ok and cr.o + 18 <= nif.sizes[i]:
            cr.u8()
            ni, nv, _u, ln = cr.u32(), cr.u32(), cr.u32(), cr.u32()
            if ln == 0 or ln > 512 or ni == 0 or nv == 0 or not cr.need(ln):
                break
            lod = (ni, nv, raw[cr.o:cr.o + ln].decode("latin-1"))
            cr.o += ln
            break
        mat = ""
        if 0 <= shader < nif.nb:
            sr = _R(blk(shader))
            mat = nif.hstr_at(sr, sr.u32())
        rows.append((nif.hstr_at(_R(raw), nm or 0), mat, lod))

    opened: dict[str, Ba2Archive] = {}
    print()
    print("%-26s %-30s %7s %6s  %s" % ("node", "material", "nidx/nv", "uvs", "verdict"))
    for name, mat, lod in rows:
        if not lod:
            print("%-26s %-30s %7s %6s  %s" % (name[:26], mat.split("\\")[-1][:30], "-", "-", "no LOD"))
            continue
        ni, nv, path = lod
        fkey = path.lower().replace("\\", "/")
        if fkey.endswith(".mesh"):
            fkey = fkey[:-5]
        if fkey.startswith("geometries/"):
            fkey = fkey[11:]
        cand = "geometries/%s.mesh" % fkey
        arc2 = owner.get(cand)
        if not arc2:
            print("%-26s %-30s %5d/%-4d %6s  %s" % (name[:26], mat.split("\\")[-1][:30], ni, nv, "-",
                                                     "MESH NOT IN INDEX (%s)" % cand))
            stats["missing"] += 1
            continue
        a2 = opened.get(arc2)
        if a2 is None:
            a2 = opened[arc2] = Ba2Archive(data / arc2)
        mb, _ = a2.read_entry(cand)
        info = parse_mesh_streams(mb) if mb else {}
        uvs = info.get("uv_sets", [])
        chosen = next((k for k, u in enumerate(uvs) if (u[1] - u[0]) > 1e-4 or (u[3] - u[2]) > 1e-4), None)
        if not uvs:
            verdict, stats_key = "no UV stream found", "nou"
        elif chosen is None:
            verdict = "DEGENERATE only -> now untextured (was: 1 texel)"
            stats_key = "degenerate"
        else:
            u = uvs[chosen]
            verdict = "stream %d u=[%.3f,%.3f] v=[%.3f,%.3f]%s" % (
                chosen, u[0], u[1], u[2], u[3], "" if chosen == 0 else "  (was stream 0!)")
            stats_key = "ok"
        stats[stats_key] += 1
        print("%-26s %-30s %5d/%-4d %6d  %s" % (name[:26], mat.split("\\")[-1][:30], ni, nv, len(uvs), verdict))
    print()
    print("summary:", stats)
    return 0


class _R:
    """Thin reader so nif_uv's parser stays the single source of truth."""

    def __init__(self, b, off=0):
        import struct
        self.b, self.o, self.ok = b, off, True

    def u8(self):
        if self.o + 1 > len(self.b):
            self.ok = False
            return 0
        v = self.b[self.o]
        self.o += 1
        return v

    def u32(self):
        import struct
        if self.o + 4 > len(self.b):
            self.ok = False
            return 0
        v = struct.unpack_from("<I", self.b, self.o)[0]
        self.o += 4
        return v

    def i32(self):
        import struct
        if self.o + 4 > len(self.b):
            self.ok = False
            return 0
        v = struct.unpack_from("<i", self.b, self.o)[0]
        self.o += 4
        return v

    def f32(self):
        import struct
        if self.o + 4 > len(self.b):
            self.ok = False
            return 0.0
        v = struct.unpack_from("<f", self.b, self.o)[0]
        self.o += 4
        return v

    def need(self, n):
        return self.ok and self.o + n <= len(self.b)


if __name__ == "__main__":
    raise SystemExit(main())