"""Offline model-cache builder for the Shop Explorer offline preview.

Pipeline (all OUTSIDE the game process):
  1. Scan the game's Meshes BA2 archives for *.nif under meshes/weapons/.
  2. For each candidate NIF (or a curated subset), run MeshConverter.dll's
     ImportNif to get the node tree + per-geometry mat_path + factory paths.
  3. Extract the LOD0 .mesh for each geometry, run ImportMesh, parse the
     obj_content (v/vt/f lines) into positions/uvs/indices.
  4. Write one compact binary .sfmodel per weapon:
        magic 'SFM1'
        u32 numMeshes
        per mesh: name[64], u32 matPathLen, matPath, u32 numVerts,
                  u32 numIndices, floats pos[3], uv[2], u32 indices
  5. Write an index JSON (weapon -> model file) for the plugin to consult.

The plugin never runs MeshConverter.dll, never touches BA2s, and never
executes foreign code inside Starfield — it only reads the cache files this
tool produced. That keeps the in-game process 100% under our control.
"""
import ctypes
import json
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUTDIR = os.path.join(HERE, "cache")
MESHDLL = r"C:\Users\mrlma\AppData\Roaming\Blender Foundation\Blender\3.6\scripts\addons\tool_export_mesh\MeshConverter.dll"
DATA = r"B:\SteamLibrary\steamapps\common\Starfield\Data"
MESH_ARCHIVES = ("Starfield - Meshes01.ba2", "Starfield - Meshes02.ba2", "Starfield - MeshesPatch.ba2",
                 "SFBGS003 - Main.ba2", "SFBGS050 - Main.ba2", "ShatteredSpace - Main01.ba2")
LIMIT_NIFS = int(os.environ.get("LIMIT_NIFS", "40"))
TARGET_SUBSTR = os.environ.get("TARGET", "").lower()


def open_ba2(path):
    f = open(path, "rb")
    hdr = f.read(36)
    magic, ver, typ, num = struct.unpack("<4sI4sI", hdr[:16])
    assert magic == b"BTDX"
    nt_off = struct.unpack("<Q", hdr[16:24])[0]
    ents = []
    for _ in range(num):
        e = f.read(36)
        ext = e[0:4].rstrip(b"\0").decode("latin-1")
        off = struct.unpack_from("<Q", e, 12)[0]
        sz = struct.unpack_from("<I", e, 24)[0]
        ents.append(["", ext, off, sz])
    f.seek(nt_off)
    for i in range(num):
        ln = struct.unpack("<H", f.read(2))[0]
        ents[i][0] = f.read(ln).decode("latin-1")
    return f, ents


def read_entry(f, ent):
    _, ext, off, sz = ent
    f.seek(off)
    raw = f.read(sz)
    if raw[:1] == b"\x78":
        try:
            raw = zlib.decompress(raw)
        except OSError:
            pass
    return raw


dll = ctypes.CDLL(MESHDLL)
dll.ImportNif.argtypes = [ctypes.c_char_p, ctypes.c_bool, ctypes.c_char_p]
dll.ImportNif.restype = ctypes.c_char_p
dll.ImportMesh.argtypes = [ctypes.c_char_p]
dll.ImportMesh.restype = ctypes.c_char_p


def nif_to_geo(dll, nif_path):
    s = dll.ImportNif(nif_path.encode(), False, b"").decode("utf-8", "replace")
    j = s[s.index("{"):]
    return json.loads(j)


def mesh_json(dll, mesh_path):
    s = dll.ImportMesh(mesh_path.encode()).decode("utf-8", "replace")
    j = s[s.index("{"):]
    return json.loads(j)


def parse_obj(obj_content):
    pos, uv, idx = [], [], []
    for line in obj_content.splitlines():
        if line.startswith("v "):
            _, x, y, z = line.split()[:4]
            pos.append((float(x), float(y), float(z)))
        elif line.startswith("vt "):
            _, u, v = line.split()[:3]
            uv.append((float(u), float(v)))
        elif line.startswith("f "):
            parts = line.split()[1:]
            tri = []
            for p in parts:
                a, t, _ = (p.split("/") + [""])[:3]
                vi = int(a) - 1
                ti = int(t) - 1 if t else -1
                tri.append((vi, ti))
            # fan-triangulate (obj faces from the dll are triangles already)
            for k in range(1, len(tri) - 1):
                idx.append((tri[0], tri[k], tri[k + 1]))
    # Expand to one vertex per unique (position, uv) pair. Hard edges and
    # UV splits duplicate positions (this mirrors the engine's own vertex
    # buffer); the old code zipped pos/uv positionally and indexed by
    # position alone, smearing texels across the model (brown slides on
    # grey guns). Missing/invalid refs fail closed per triangle.
    # V is flipped (1-t): MeshConverter exports OBJ-style bottom-origin
    # UVs, D3D samples top-origin (verified live: unflipped renders land
    # the slide on the map's brown weathering instead of grey metal).
    out_pos, out_uv, out_idx = [], [], []
    seen = {}
    for (a, b, c) in idx:
        tri = []
        ok = True
        for (vi, ti) in (a, b, c):
            if vi < 0 or vi >= len(pos):
                ok = False
                break
            key = (vi, ti)
            j = seen.get(key)
            if j is None:
                j = len(out_pos)
                seen[key] = j
                out_pos.append(pos[vi])
                if 0 <= ti < len(uv):
                    out_uv.append((uv[ti][0], 1.0 - uv[ti][1]))
                else:
                    out_uv.append((0.0, 0.0))
            tri.append(j)
        if ok:
            out_idx.append((tri[0], tri[1], tri[2]))
    return out_pos, out_uv, out_idx


def mat4_identity():
    return [[1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0]]


def mat4_mul(a, b):
    r = [[0.0] * 4 for _ in range(4)]
    for i in range(4):
        for j in range(4):
            r[i][j] = sum(a[i][k] * b[k][j] for k in range(4))
    return r


def mat4_scale(m, s):
    r = [row[:] for row in m]
    for i in range(3):
        r[i][0] *= s
        r[i][1] *= s
        r[i][2] *= s
    return r


def mat4_apply(m, p):
    x, y, z = p
    return (
        m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3],
        m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3],
        m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3],
    )


def collect_transforms(node, parent_mat, out):
    # MeshConverter bakes WORLD-space matrices onto every node (child matrix
    # already contains the parent offset - verified to 4 decimals on AR99
    # mag: raw+leaf == cached). Accumulating would double/triple offsets,
    # so each geometry keeps only its own node matrix + scale.
    gi = node.get("geometry_index", -1)
    if gi is not None and gi != 4294967295:
        mat = node.get("matrix")
        scale = node.get("scale", 1.0)
        if mat is not None:
            out[gi] = mat4_scale(mat, scale) if scale != 1.0 else mat
        else:
            out[gi] = mat4_identity()
    for ch in node.get("children", []):
        collect_transforms(ch, parent_mat, out)


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    archives = {}
    for name in MESH_ARCHIVES:
        f, ents = open_ba2(os.path.join(DATA, name))
        archives[name] = (f, {e[0].lower(): e for e in ents})
    mesh_lookup = {}
    for name, (f, ents) in archives.items():
        for key, e in ents.items():
            mesh_lookup.setdefault(key, (name, e))

    def extract(path_l, tmp):
        ent = mesh_lookup.get(path_l)
        if not ent:
            return None
        name, e = ent
        data = read_entry(archives[name][0], e)
        with open(tmp, "wb") as o:
            o.write(data)
        return tmp

    # 1) list weapon nifs
    nifs = []
    for name, (f, ents) in archives.items():
        for key, e in ents.items():
            if key.endswith(".nif") and key.startswith("meshes/weapons/") and "projectile" not in key:
                nifs.append((key, name, e))
    nifs.sort()
    if TARGET_SUBSTR:
        nifs = [n for n in nifs if TARGET_SUBSTR in n[0]]
    nifs = nifs[:LIMIT_NIFS]
    print(f"{len(nifs)} nif candidates")

    index = {}
    tmpnif = os.path.join(OUTDIR, "_tmp.nif")
    tmpmesh = os.path.join(OUTDIR, "_tmp.mesh")
    for key, arname, ent in nifs:
        weapon = os.path.basename(key)[:-4]
        try:
            data = read_entry(archives[arname][0], ent)
            with open(tmpnif, "wb") as o:
                o.write(data)
            tree = nif_to_geo(dll, tmpnif)
        except Exception as ex:
            print("skip", key, ex)
            continue
        geos = tree.get("geometries", [])
        if not geos:
            continue
        geo_xforms = {}
        collect_transforms(tree, mat4_identity(), geo_xforms)
        meshes = []
        for gi, g in enumerate(geos):
            lods = g.get("geo_mesh_lod") or []
            if not lods:
                continue
            fp = lods[0].get("factory_path", "").replace("\\", "/")
            if not extract(("geometries/" + fp + ".mesh").lower(), tmpmesh):
                continue
            try:
                mj = mesh_json(dll, tmpmesh)
            except Exception as ex:
                print("  mesh fail", fp, ex)
                continue
            pos, uv, idx = parse_obj(mj.get("obj_content", ""))
            if not pos or not idx:
                continue
            xform = geo_xforms.get(gi)
            if xform is not None:
                pos = [mat4_apply(xform, p) for p in pos]
            meshes.append({
                "name": g.get("mat_path", "mat"),
                "mat_path": g.get("mat_path", ""),
                "pos": pos,
                "uv": uv,
                "idx": idx,
            })
        if not meshes:
            continue
        # write binary model
        out = os.path.join(OUTDIR, weapon + ".sfmodel")
        with open(out, "wb") as o:
            o.write(b"SFM1")
            o.write(struct.pack("<I", len(meshes)))
            for m in meshes:
                nm = m["name"].encode("utf-8")[:63]
                nm = nm + b"\0" * (64 - len(nm))
                o.write(nm)
                mp = m["mat_path"].encode("utf-8")
                o.write(struct.pack("<I", len(mp)))
                o.write(mp)
                o.write(struct.pack("<I", len(m["pos"])))
                o.write(struct.pack("<I", len(m["idx"]) * 3))
                for p, t in zip(m["pos"], m["uv"]):
                    o.write(struct.pack("<5f", p[0], p[1], p[2], t[0], t[1]))
                for tri in m["idx"]:
                    o.write(struct.pack("<3I", tri[0], tri[1], tri[2]))
        index[weapon] = {"file": os.path.basename(out), "meshes": len(meshes)}
        print("wrote", weapon, len(meshes), "meshes")

    for name, (f, _) in archives.items():
        f.close()
    with open(os.path.join(OUTDIR, "index.json"), "w") as o:
        json.dump(index, o, indent=1)
    print("index:", len(index), "weapons")


if __name__ == "__main__":
    main()
