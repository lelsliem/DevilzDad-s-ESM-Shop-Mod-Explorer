"""Measure how the ammo-crate paint colour composites, instead of guessing.

The `.27 Caliber` preview draws a white lid and a yellow band; the crate in the
game draws a yellow lid and a blue-grey band. Every ammo model already logs
`2 range(s) have a CDB override tint candidate`, so the database colour is
found and then dropped by `UploadModelTextures` (it resets a textured range to
white unless the range is in the `cdbOverrideTint` allow-list).

Dropping it is clearly wrong for these crates, but *how* the colour goes on is
not obvious from the files, and the two candidates are very different:

    multiply   albedo * colour            (baked paint)
    replace    colour                     (Lerp layer at weight 1)

This probe answers it with numbers. It parses the crate's own NIF, resolves each
shape's `.mesh`, samples that shape's colour atlas under its real UV footprint
(up-facing faces are the lid, which is what the screenshots compare), and prints
the albedo, the multiplied result, and the colour pre-converted from linear to
sRGB before the multiply.

The database colours are LINEAR (they are authored for the engine's linear
shader), while this preview's pipe multiplies sRGB-encoded bytes. That is why
the linear->sRGB candidate is listed:

    ammoboxlarge_darkgreen01   (0.750, 0.609, 0.188) -> sRGB (224, 206, 120)
    ammoboxprimer_lid          (0.584, 0.800, 0.320) -> sRGB (201, 231, 152)

Reference (in-game crate, sampled from the 2026-10-18 screenshot): the lid of
the .27 CAL crate measures about (201, 186, 118) lit and (189, 173, 102) on the
front face.

    python ammo_paint_probe.py --data "B:\\...\\Starfield\\Data" \
        --nif meshes/setdressing/ammo_crates/ammo27caliber.nif
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ba2lib import Ba2Archive  # noqa: E402
from dump_tex import bc1_block, bc4_block  # noqa: E402
from nif_uv import half_to_float  # noqa: E402

# Albedo each NIF material resolves to through the plugin's four-step lookup
# (the same pairs the 15:38:15 session log prints for this crate).
ALBEDO = {
    "materials/setdressing/ammobox01/ammoboxlarge_darkgreen01.mat":
        "textures/setdressing/ammobox01/ammobox01_color.dds",
    "materials/setdressing/ammobox01/ammobox01_revised.mat":
        "textures/setdressing/ammobox01/ammoboxrevised_color.dds",
    "materials/setdressing/ammobox01/paintedmetalshine01_orange.mat":
        "textures/common/paint/paint01_color.dds",
    # The 1.5kV LZR crate's paint slot. Its CDB texture set is EMPTY, so the
    # preview falls back to the containing folder and gets the crate atlas
    # (measured in the 17:20:47 session log), while its sibling Orange above
    # resolves to the neutral paint through its own set. Both are measured
    # here so the two crates' lower bodies can be compared part by part.
    "materials/setdressing/ammobox01/paintedmetalshine01_green.mat":
        "textures/setdressing/ammobox01/ammobox01_color.dds",
}

# CDB MaterialOverrideColor, linear RGBA (cdb_alpha_replica.scan()).
TINT = {
    "materials/setdressing/ammobox01/ammoboxlarge_darkgreen01.mat": (0.750, 0.609, 0.188),
    "materials/setdressing/ammobox01/ammobox01_revised.mat": None,
    "materials/setdressing/ammobox01/paintedmetalshine01_orange.mat": (0.846, 0.676, 0.395),
    "materials/setdressing/ammobox01/paintedmetalshine01_green.mat": (0.623, 0.663, 0.670),
}


def linear_to_srgb(x: float) -> float:
    return 12.92 * x if x <= 0.0031308 else 1.055 * (x ** (1.0 / 2.4)) - 0.055


# ── .mesh LOD0 (mirror of ParseMeshLod0 in src/LiveModel.cpp) ────────────────
def parse_mesh(blob: bytes):
    off = 0
    off += 4                      # unknown header
    nidx = struct.unpack_from("<I", blob, off)[0]
    off += 4
    idx = list(struct.unpack_from("<%dH" % nidx, blob, off))
    off += nidx * 2
    border = struct.unpack_from("<f", blob, off)[0]
    off += 4
    off += 4                      # weights per vertex
    nv = struct.unpack_from("<I", blob, off)[0]
    off += 4
    pos = []
    for _ in range(nv):
        x, y, z = struct.unpack_from("<3h", blob, off)
        off += 6
        pos.append((x / 32767.0 * border, y / 32767.0 * border, z / 32767.0 * border))
    uv = []
    uvs = []
    while off + 4 <= len(blob):
        cnt = struct.unpack_from("<I", blob, off)[0]
        off += 4
        if cnt != nv or off + nv * 4 > len(blob):
            break
        cand = []
        for _ in range(nv):
            u = half_to_float(struct.unpack_from("<H", blob, off)[0])
            v = half_to_float(struct.unpack_from("<H", blob, off)[0])
            off += 4
            cand.append((u, v))
        if not uv and any(abs(u) > 1e-6 or abs(v) > 1e-6 for u, v in cand):
            uv = cand
        uvs.append(cand)
    return pos, idx, (uv or (uvs[0] if uvs else [])), uvs


def sample(rgb: bytes, w: int, h: int, u: float, v: float):
    x = int((u % 1.0) * w) % w
    y = int((v % 1.0) * h) % h
    i = (y * w + x) * 3
    return rgb[i], rgb[i + 1], rgb[i + 2]


def decode_tex(data: Path, name: str):
    for archive in sorted(data.glob("*.ba2")):
        ar = Ba2Archive(archive)
        if not ar.valid or name not in ar.tex_files:
            continue
        payload, err = ar.read_texture(name)
        if payload is None:
            raise SystemExit("read_texture %s: %s" % (name, err))
        tf = ar.tex_files[name]
        w, h = tf.width, tf.height
        bw, bh = (w + 3) // 4, (h + 3) // 4
        rgb = bytearray(w * h * 3)
        single = tf.dxgi in (80, 81)
        for by in range(bh):
            for bx in range(bw):
                blk = payload[(by * bw + bx) * 8:(by * bw + bx) * 8 + 8]
                (bc4_block if single else bc1_block)(blk, rgb, bx, by, w, h)
        return bytes(rgb), w, h, archive.name, tf.dxgi
    raise SystemExit("texture not found in any archive: %s" % name)


# ── NIF geometry walk (shape -> material -> .mesh) ──────────────────────────
def nif_shapes(blob: bytes):
    """(node name, material path, lod0 factory) for each BSGeometry."""
    sys.path.insert(0, str(HERE))
    from nif_uv import Nif, R

    nif = Nif(blob)
    node_t, geo_t = nif.idx_of("NiNode"), nif.idx_of("BSGeometry")

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

    def block(i):
        return blob[nif.offsets[i]:nif.offsets[i] + nif.sizes[i]]

    referenced = set()
    for i in range(nif.nb):
        if nif.type_idx[i] != node_t:
            continue
        raw = block(i)
        _, end = node_prefix(R(raw))
        if end is None:
            continue
        cr = R(raw, end)
        for _ in range(min(cr.u32(), 4096)):
            c = cr.i32()
            if 0 <= c < nif.nb:
                referenced.add(c)
    root = next((i for i in range(nif.nb) if nif.type_idx[i] == node_t and i not in referenced), 0)

    stack, out, guard = [root], [], 0
    while stack and guard < 100000:
        guard += 1
        i = stack.pop()
        raw = block(i)
        if nif.type_idx[i] == node_t:
            _, end = node_prefix(R(raw))
            if end is None:
                continue
            cr = R(raw, end)
            for _ in range(min(cr.u32(), 4096)):
                c = cr.i32()
                if 0 <= c < nif.nb:
                    stack.append(c)
            continue
        if nif.type_idx[i] != geo_t:
            continue
        nm, end = node_prefix(R(raw))
        if end is None:
            continue
        cr = R(raw, end)
        for _ in range(10):
            cr.f32()
        cr.i32()
        shader = cr.i32()
        cr.i32()
        lod = None
        while cr.o + 18 <= len(raw):
            cr.u8()
            ni, nv, _unk, ln = cr.u32(), cr.u32(), cr.u32(), cr.u32()
            if ln == 0 or ln > 512 or ni == 0 or nv == 0 or not cr.need(ln):
                break
            path = raw[cr.o:cr.o + ln].decode("latin-1")
            cr.o += ln
            if lod is None:
                lod = (ni, nv, path)
        mat = ""
        if 0 <= shader < nif.nb:
            sr = R(block(shader))
            mat = nif.hstr_at(sr, sr.u32())
        if lod:
            out.append((nif.hstr_at(R(raw), nm or 0), mat, lod[2]))
    return out


def mesh_key(factory: str) -> str:
    key = factory.lower().replace("\\", "/")
    if key.endswith(".mesh"):
        key = key[:-5]
    if key.startswith("geometries/"):
        key = key[11:]
    return "geometries/%s.mesh" % key


def load_mesh(data: Path, key: str):
    for archive in sorted(data.glob("*.ba2")):
        ar = Ba2Archive(archive)
        if not ar.valid or key not in ar.entries:
            continue
        blob, err = ar.read_entry(key)
        if blob:
            return blob
        raise SystemExit("read_entry %s: %s" % (key, err))
    raise SystemExit("mesh not found: %s" % key)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    ap.add_argument("--nif", required=True, help="archive-relative .nif entry name")
    args = ap.parse_args()
    data = Path(args.data)

    nif_blob = load_mesh(data, args.nif.lower())
    shapes = nif_shapes(nif_blob)
    print("NIF %s: %d shape(s)" % (args.nif, len(shapes)))

    for name, mat, factory in shapes:
        key = mesh_key(factory)
        try:
            blob = load_mesh(data, key)
        except SystemExit as exc:
            print("  %-24s %-52s %s" % (name, mat, exc))
            continue
        pos, idx, uv, uvs = parse_mesh(blob)
        tint = TINT.get(mat.lower().replace("\\", "/"))
        albedo = ALBEDO.get(mat.lower().replace("\\", "/"))
        print("\n  shape %-22s %s" % (name, Path(mat).name))
        print("    mesh   %s  nv=%d tris=%d uv_sets=%d" % (key, len(pos), len(idx) // 3, len(uvs)))
        if not albedo:
            print("    no albedo mapping recorded for this material")
            continue
        rgb, w, h, archive, dxgi = decode_tex(data, albedo)
        print("    albedo %s (%dx%d dxgi=%d in %s)" % (albedo, w, h, dxgi, archive))

        up, side = [], []
        for t in range(0, len(idx) - 2, 3):
            a, b, c = idx[t], idx[t + 1], idx[t + 2]
            if max(a, b, c) >= len(pos):
                continue
            ax, ay, az = pos[a]
            bx, by, bz = pos[b]
            cx, cy, cz = pos[c]
            nx = (by - ay) * (cz - az) - (bz - az) * (cy - ay)
            ny = (bz - az) * (cx - ax) - (bx - ax) * (cz - az)
            nz = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
            length = (nx * nx + ny * ny + nz * nz) ** 0.5
            if length <= 0:
                continue
            if nz / length > 0.6:
                up.append(t)
            elif nz / length < 0.2:
                side.append(t)

        def mean_color(faces):
            if not faces:
                return None
            acc = [0.0, 0.0, 0.0]
            for t in faces:
                a, b, c = idx[t], idx[t + 1], idx[t + 2]
                # Centroid UV is enough for a paint flat: the atlas under a
                # face is one colour plus grunge, so area weighting per
                # triangle is what decides between multiply and replace.
                u = (uv[a][0] + uv[b][0] + uv[c][0]) / 3.0
                v = (uv[a][1] + uv[b][1] + uv[c][1]) / 3.0
                r, g, bl = sample(rgb, w, h, u, v)
                acc[0] += r
                acc[1] += g
                acc[2] += bl
            n = len(faces)
            return tuple(round(x / n) for x in acc)

        for label, faces in (("up-facing", up), ("side", side)):
            mean = mean_color(faces)
            if mean is None:
                print("    %-9s (no faces)" % label)
                continue
            line = "    %-9s n=%-4d albedo %s" % (label, len(faces), tuple(mean))
            if tint:
                raw = tuple(round(mean[i] * tint[i]) for i in range(3))
                srgb_tint = tuple(linear_to_srgb(c) for c in tint)
                baked = tuple(round(mean[i] * srgb_tint[i]) for i in range(3))
                line += "  x tint %s  x tint(sRGB) %s" % (raw, baked)
            print(line)
    print("\nreference: game .27 CAL lid (201,186,118) lit / (189,173,102) front")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
