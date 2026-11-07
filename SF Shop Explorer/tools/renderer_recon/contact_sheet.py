"""Every shelf's items rendered offline, as one contact sheet per shelf.

The colour audit answers "why is this material this colour" from the files. It
cannot answer "does the item look right", because nothing was drawn. This tool
draws it: for each item it loads the record's MODL, walks the NIF to its
BSGeometry blocks, resolves every shape's material through the same rules the
plugin ships (the audit's swap/route/CDB/alpha decisions, imported rather than
copied), pulls the LOD0 vertex data out of the referenced .mesh and rasterises
a 3/4 view into a PNG tile.

    python contact_sheet.py --shelf aid                  # 365 tiles + index
    python contact_sheet.py --item "TranquiliTea Classic" --size 256
    python contact_sheet.py --shelf books --out reports/contact-books.html

What a tile is, exactly - the honest limits:

  * flat shading, one colour per triangle, sampled from the material's colour
    map at the triangle's UV centroid. Detail inside a triangle is lost, but
    material assignment, tint and silhouette are not.
  * painter-sorted by mean depth, not a z-buffer: interpenetrating parts can
    draw in the wrong order. Fine for a contact sheet, wrong for a hero shot.
    An assembled gun has overlapping shells, so interior faces show as an
    x-ray; the silhouette and part alignment are still the real ones.
  * translucency is composited against the sheet background rather than the
    geometry behind it - a glass jar reads as glass, but what shows through it
    is the backdrop, not the specimen inside. Clear shells - glass, clear
    plastic, visor panes, display-case plexiglass - are drawn at a preview alpha
    floor (0.16): the authored 0.05 transmission (and no specular or refraction
    here) made an empty vial invisible except for its cap. A silhouette rim from
    the clear-shell coverage mask (1px at 160, 2px at 512) gives the contour an
    edge without drawing the triangle wireframe. Rim parts are grouped by their
    world-space bounds first (same NIF, a small margin), nearby panes in a group
    are closed into one island, then enclosed holes are filled before the edge is
    taken, so a visor with a light strip or a lens inside a frame gets one outer
    contour rather than a rim around every seam; separate props keep separate
    contours. When an opaque shell body
    exists, the rim is tinted toward it and softened, so a coloured vial is not
    outlined as if it were empty. The material report keeps the authored alpha.
  * colour maps decode for BC1/BC3/BC4/BC5 in numpy; BC7 (DXGI 98/99, what
    Starfield tags almost every colour map with) goes through Pillow after a
    DX10 container is rebuilt around the store's raw payload. Anything else
    falls back to the material's flat colour and the row says "flat" so a tile
    never pretends to be textured when it is not.
  * a material the route table and the CDB both miss is read from its own file:
    the .mat names its texture set inline ("FileName" : "Data\\Textures\\...").
    That is the last resort, and it is what the game does.
  * the record's own MODL, except for a dummy-base weapon: Razorback's NIF is
    the empty BullpupDummyReceiver and the gun is its OMOD parts, so those are
    assembled by omod_assembly (the standard tier of each slot, the same family
    binding the plugin uses) and placed through their BSConnectPoint graph by
    connect_points. A part with no matching host stays at the identity
    root, the same fallback the C++ resolver logs, and the workbench lists it
    as unplaced so a wrong assembly pick is visible rather than silent. No user
    choices, rarity or quality tier - that is runtime state.
  * an ARMO's world model is often the dressing-box proxy; the visible garment
    is the linked ARMA addon set, which is drawn in its bind pose. The female
    model of a `_M`/`_F` pair is the one the engine picks.
  * geometry a mod NIF carries inside itself (a Blender export has no
    geometries/<guid>.mesh entry) is drawn from those bytes; see
    walk_geometries.

The point is regression: render a shelf before and after a rule change and the
tiles that moved are the items the change touched, without a game launch.
"""
from __future__ import annotations

import argparse
import math
import os
import re
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageOps

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402
import connect_points  # noqa: E402
import nif_uv  # noqa: E402
import omod_assembly  # noqa: E402
import parent_index  # noqa: E402
from nif_uv import Nif  # noqa: E402

BACKGROUND = (20, 24, 32)          # the preview panel's own backdrop
# The game sells clear glass with specular highlights and refraction; this
# renderer has neither, so the authored 0.05 transmission is a ghost against
# the dark backdrop (an empty vial showed only its cap). Draw glass at a
# visible preview floor while keeping the authored alpha in the report.
CLEAR_PREVIEW_ALPHA = 0.16
# A pane whose material carries no database alpha at all (a visor lens is
# opaque by omission, not by intent) keeps a little of its tint so the contour
# is not the only thing on the tile. This is deliberately above the empty-vial
# floor - a visor is a surface, not a closed shell around a specimen.
CLEAR_PANE_ALPHA = 0.30
CLEAR_RIM = (196, 212, 218)       # pale cyan, not a white outline
CLEAR_RIM_ALPHA = 0.45           # empty clear shell: the contour is the whole read
CLEAR_RIM_ON_BODY_ALPHA = 0.24   # coloured shell: tint it, do not outline it
# A clear prop is not always named glass: the industrial display case uses
# PlasticCleanClear01, and a visor may only say Visor. The resolver's alpha
# reason is the other half of the evidence ("clear shell (fresnel only)").
CLEAR_NAME_TOKENS = ("glass", "clear", "transparent", "visor", "window",
                     "plexiglass", "acrylic", "lens", "lense")
# A pane is often named only by its shape: the Spartan visor NIF's material is
# `MARKVII_Helmet.mat` (opaque, no database alpha), while its one BSGeometry is
# `MarkVIIHelmet_MARKVII_visor`. The shape is per-piece evidence from the NIF
# itself, unlike the item name, and it is what lets a visor or lens take the
# clear-shell treatment without turning an opaque glass body transparent.
CLEAR_SHAPE_TOKENS = ("visor", "window", "lens", "lense")
CLEAR_SHAPE_BLOCKERS = ("frame", "trim", "housing", "mount", "bracket",
                        "handle", "cover")
CLEAR_WHY_TOKENS = ("clear shell", "clear glass", "fresnel")
LIGHT = np.array([0.35, -0.75, 0.55], dtype=np.float32)
LIGHT /= np.linalg.norm(LIGHT)
AMBIENT = 0.42
MAX_TRIS = 1500                    # per item; the rest are strided away
MESH_CACHE_LIMIT = 1024            # parsed LOD0 arrays, bounded for long sweeps
YAW_DEG, PITCH_DEG = -32.0, 14.0
# Additive/emissive overlays. Drawn opaque they erase everything behind them:
# the Isotopic Coolant jar's water-caustic gobo is a full jar-shaped shell that
# blacked out the jar and its specimen. The engine adds them to the frame, so a
# faint tint is the honest stand-in for a contact sheet.
EFFECT_TOKENS = ("caustic", "gobo", "glow", "materials/effects/")


# ── DDS ───────────────────────────────────────────────────────────────────────

def _rgb565(c: np.ndarray) -> np.ndarray:
    r = ((c >> 11) & 0x1F).astype(np.float32)
    g = ((c >> 5) & 0x3F).astype(np.float32)
    b = (c & 0x1F).astype(np.float32)
    return np.stack([(r * 8 + r / 4) / 255.0,
                     (g * 4 + g / 16) / 255.0,
                     (b * 8 + b / 4) / 255.0], axis=-1)


def _blocks_to_image(colours: np.ndarray, bx: int, by: int,
                     width: int, height: int) -> np.ndarray | None:
    """(n,16,3) texels -> (height, width, 3), cropping the block padding."""
    if colours.size == 0:
        return None
    texels = colours.reshape(by, bx, 4, 4, 3).transpose(0, 2, 1, 3, 4)
    img = texels.reshape(by * 4, bx * 4, 3)
    return img[:height, :width]


def decode_bc1(payload: bytes, width: int, height: int) -> np.ndarray | None:
    """BC1 -> (h, w, 3) float. Punch-through blocks keep their colour."""
    bx, by = (width + 3) // 4, (height + 3) // 4
    need = bx * by * 8
    if len(payload) < need:
        return None
    raw = np.frombuffer(payload, dtype=np.uint8, count=need).reshape(-1, 8)
    c0 = raw[:, 0].astype(np.uint16) | (raw[:, 1].astype(np.uint16) << 8)
    c1 = raw[:, 2].astype(np.uint16) | (raw[:, 3].astype(np.uint16) << 8)
    a, b = _rgb565(c0), _rgb565(c1)
    four = c0 > c1
    third = np.where(four[:, None], (2 * a + b) / 3.0, (a + b) / 2.0)
    fourth = np.where(four[:, None], (a + 2 * b) / 3.0, 0.0)
    table = np.stack([a, b, third, fourth], axis=1)          # (n, 4, 3)
    word = np.ascontiguousarray(raw[:, 4:8]).view(np.uint32).reshape(-1)
    idx = np.zeros((raw.shape[0], 16), dtype=np.uint8)
    for k in range(16):
        idx[:, k] = (word >> (2 * k)) & 3
    colours = table[np.arange(table.shape[0])[:, None], idx]  # (n, 16, 3)
    return _blocks_to_image(colours, bx, by, width, height)


def decode_bc3(payload: bytes, width: int, height: int) -> np.ndarray | None:
    bx, by = (width + 3) // 4, (height + 3) // 4
    need = bx * by * 16
    if len(payload) < need:
        return None
    raw = np.frombuffer(payload, dtype=np.uint8, count=need).reshape(-1, 16)
    return decode_bc1(np.ascontiguousarray(raw[:, 8:16]).tobytes(), width, height)


def decode_bc4(payload: bytes, width: int, height: int) -> np.ndarray | None:
    """BC4/BC5 -> grey; the red channel is what the sheet wants."""
    bx, by = (width + 3) // 4, (height + 3) // 4
    need = bx * by * 8
    if len(payload) < need:
        return None
    raw = np.frombuffer(payload, dtype=np.uint8, count=need).reshape(-1, 8)
    r0 = raw[:, 0].astype(np.float32) / 255.0
    r1 = raw[:, 1].astype(np.float32) / 255.0
    bits = np.zeros(raw.shape[0], dtype=np.uint64)
    for k in range(6):
        bits |= raw[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
    eight = r0 > r1
    palette = np.zeros((raw.shape[0], 8), dtype=np.float32)
    palette[:, 0] = r0
    palette[:, 1] = r1
    for k in range(1, 7):
        palette[:, k + 1] = r0 + (r1 - r0) * (k / 7.0)
    palette[~eight, 6] = (r0[~eight] + r1[~eight]) / 2.0
    palette[~eight, 7] = 0.0
    idx = np.zeros((raw.shape[0], 16), dtype=np.uint8)
    for k in range(16):
        idx[:, k] = ((bits >> np.uint64(3 * k)) & np.uint64(7)).astype(np.uint8)
    vals = palette[np.arange(raw.shape[0])[:, None], idx]
    return _blocks_to_image(np.repeat(vals[..., None], 3, axis=2), bx, by, width, height)


def decode_pillow(payload: bytes, dxgi: int, width: int, height: int) -> np.ndarray | None:
    """Anything Pillow decodes and the numpy decoders do not: BC7 and friends.

    Starfield tags almost every colour map BC7_UNORM_SRGB (DXGI 99), and the
    block decoders above cover only BC1/3/4/5 - so 95 drawn Misc items came out
    flat grey with a colour map sitting in the archive. Pillow's DDS plugin
    decodes BC1-BC7; it wants a container, and the store hands back the raw
    DX10 payload, so the header is rebuilt here (one face, one mip).
    """
    import io

    from PIL import Image

    header = bytearray()
    header += b"DDS "
    header += struct.pack("<I", 124)
    header += struct.pack("<I", 0x1007 | 0x80000)     # caps, w, h, pf, linearsize
    header += struct.pack("<II", height, width)
    header += struct.pack("<I", len(payload))
    header += struct.pack("<I", 1)                   # depth
    header += struct.pack("<I", 1)                   # mip count
    header += bytes(44)
    header += struct.pack("<I", 32) + struct.pack("<I", 0x4) + b"DX10" + bytes(20)
    header += struct.pack("<I", 0x1000) + bytes(16)  # DDSCAPS_TEXTURE
    header += struct.pack("<IIIII", dxgi, 3, 0, 1, 0)  # dxgi, TEXTURE2D, misc, array, misc2
    try:
        img = Image.open(io.BytesIO(bytes(header) + payload))
        img.load()
    except Exception:
        return None
    return np.asarray(img.convert("RGB"), dtype=np.float32) / 255.0


def decode_dds(payload: bytes, dxgi: int, width: int, height: int) -> np.ndarray | None:
    if not payload or width <= 0 or height <= 0:
        return None
    if dxgi in (71, 72):
        return decode_bc1(payload, width, height)
    if dxgi in (77, 78):
        return decode_bc3(payload, width, height)
    if dxgi in (80, 81, 83, 84):
        return decode_bc4(payload, width, height)
    return decode_pillow(payload, dxgi, width, height)


# ── .mesh ─────────────────────────────────────────────────────────────────────

def _half_array(raw: np.ndarray) -> np.ndarray:
    """Vectorised half -> float, matching nif_uv.half_to_float."""
    h = raw.astype(np.uint16)
    sign = np.where((h >> 15) & 1, -1.0, 1.0).astype(np.float32)
    exp = ((h >> 10) & 0x1F).astype(np.int32)
    man = (h & 0x3FF).astype(np.float32)
    out = np.zeros(h.shape, dtype=np.float32)
    normal = (exp > 0) & (exp < 31)
    out[normal] = sign[normal] * np.exp2(exp[normal] - 15) * (1.0 + man[normal] / 1024.0)
    return out


def parse_mesh(blob: bytes) -> dict | None:
    """Replica of ParseMeshLod0 (src/LiveModel.cpp): pos, uv, idx."""
    if not blob or len(blob) < 16:
        return None
    r = nif_uv.R(blob)
    r.u32()
    nidx = r.u32()
    if nidx == 0 or nidx > 20_000_000 or not r.need(nidx * 2):
        return None
    idx = np.frombuffer(blob, dtype="<u2", count=nidx, offset=r.o).astype(np.int64)
    r.o += nidx * 2
    border = r.f32()
    if not (0.0 < border < 1e6):
        return None
    r.u32()
    nv = r.u32()
    if nv == 0 or nv > 10_000_000 or not r.need(nv * 6):
        return None
    pos = (np.frombuffer(blob, dtype="<i2", count=nv * 3, offset=r.o)
           .astype(np.float32) / 32767.0 * border).reshape(nv, 3)
    r.o += nv * 6
    if idx.size and int(idx.max()) >= nv:
        return None
    uv = None
    for _ in range(16):
        if r.o + 4 > len(blob):
            break
        cnt = struct.unpack_from("<I", blob, r.o)[0]
        if cnt == 0 or cnt > 4_000_000 or cnt != nv or r.o + 4 + nv * 4 > len(blob):
            break
        pairs = np.frombuffer(blob, dtype="<u2", count=nv * 2, offset=r.o + 4)
        u = _half_array(pairs[0::2])
        v = _half_array(pairs[1::2])
        if not (np.isfinite(u).all() and np.isfinite(v).all()):
            break
        if np.abs(u).max(initial=0.0) > 4096.0 or np.abs(v).max(initial=0.0) > 4096.0:
            break
        if u.max(initial=0.0) - u.min(initial=0.0) > 1e-6 or \
                v.max(initial=0.0) - v.min(initial=0.0) > 1e-6:
            uv = np.stack([u, v], axis=1)
            break
        r.o += 4 + nv * 4          # degenerate set: keep looking, like the C++
    return {"pos": pos, "uv": uv, "idx": idx, "nv": nv, "nidx": nidx}


# ── NIF ───────────────────────────────────────────────────────────────────────

def _prefix(raw: bytes):
    """NodePart: name, extras, controller, flags, t[3], rot[9], scale, collision."""
    r = nif_uv.R(raw)
    name = r.u32()
    ne = r.u32()
    if ne > 4096:
        return None, None
    for _ in range(ne):
        r.i32()
    r.i32()
    r.u32()
    t = [r.f32() for _ in range(3)]
    rot = [r.f32() for _ in range(9)]
    scale = r.f32()
    r.i32()
    if not r.ok or not (0.0 < scale < 1e6):
        return None, None
    return {"name": name, "t": t, "rot": rot, "scale": scale}, r.o


def _local_matrix(part: dict) -> np.ndarray:
    """LocalMat, row-major with the row-vector convention (v' = v * M)."""
    t, rot, s = part["t"], part["rot"], part["scale"]
    return np.array([
        [rot[0] * s, rot[1] * s, rot[2] * s, t[0]],
        [rot[3] * s, rot[4] * s, rot[5] * s, t[1]],
        [rot[6] * s, rot[7] * s, rot[8] * s, t[2]],
        [0.0, 0.0, 0.0, 1.0],
    ], dtype=np.float64)


def walk_geometries(blob: bytes, nif: Nif) -> list[dict]:
    """Every BSGeometry with its accumulated world matrix and LOD list."""
    node_t = nif.idx_of("NiNode")
    geo_t = nif.idx_of("BSGeometry")
    if geo_t < 0:
        return []

    def block(i: int) -> bytes:
        return blob[nif.offsets[i]:nif.offsets[i] + nif.sizes[i]]

    referenced: set[int] = set()
    children: dict[int, list[int]] = {}
    for i in range(nif.nb):
        if nif.type_idx[i] != node_t:
            continue
        part, end = _prefix(block(i))
        if part is None:
            continue
        r = nif_uv.R(block(i), end)
        kids = []
        for _ in range(min(r.u32(), 4096)):
            c = r.i32()
            if 0 <= c < nif.nb:
                kids.append(c)
                referenced.add(c)
        children[i] = kids
    roots = [i for i in range(nif.nb)
             if nif.type_idx[i] == node_t and i not in referenced] or [0]

    out: list[dict] = []
    stack = [(root, np.eye(4)) for root in roots]
    guard = 0
    while stack and guard < 200_000:
        guard += 1
        blk, parent = stack.pop()
        raw = block(blk)
        part, end = _prefix(raw)
        if part is None:
            continue
        world = parent @ _local_matrix(part)
        if nif.type_idx[blk] == node_t:
            for kid in reversed(children.get(blk, [])):
                stack.append((kid, world))
            continue
        if nif.type_idx[blk] != geo_t:
            continue
        r = nif_uv.R(raw, end)
        for _ in range(4 + 6):
            r.f32()
        r.i32()
        shader = r.i32()
        r.i32()
        lods = []
        inline = b""
        inline_alt = b""
        while r.ok and r.o + 18 <= len(raw):
            r.u8()
            ni, nv, _unk = r.u32(), r.u32(), r.u32()
            path_at = r.o
            ln = r.u32()
            if not r.ok or ni == 0 or ni > 20_000_000 or nv == 0 or nv > 10_000_000:
                break
            # A Bethesda LOD entry continues with u32 pathLen + a printable
            # "guid\\guid" factory path. A Blender export writes no path at all
            # and follows the 13-byte entry with the raw .mesh LOD0 blob, so the
            # field read here is the blob's own leading u32 and the "length" is
            # meaningless. Same test as ParseNifGeos (src/LiveModel.cpp): the
            # bytes that would be the path must all be printable. A file that
            # fails it carries its geometry inline - the tissue boxes, desk
            # fans, notebooks and crimp tools all rendered blank without this.
            chunk = raw[r.o:r.o + ln] if ln and ln <= 512 and r.need(ln) else b""
            if chunk and all(32 <= c <= 126 for c in chunk):
                lods.append((ni, nv, chunk.decode("latin-1")))
                # Kept as a second chance: a two-byte inline header can read as
                # printable text (the KX droid head's is "[D"), so the path is
                # believed first and the same bytes are retried if the archive
                # has no such entry.
                inline_alt = raw[path_at:]
                r.o += ln
                continue
            lods.append((ni, nv, ""))
            inline = raw[path_at:]
            break
        mat = ""
        if 0 <= shader < nif.nb:
            sr = nif_uv.R(block(shader))
            mat = nif.hstr_at(sr, sr.u32())
        out.append({"name": nif.hstr_at(nif_uv.R(raw), part["name"] or 0),
                    "material": mat, "world": world, "lods": lods,
                    "inline": inline, "inline_alt": inline_alt})
    return out


def mesh_key(lod_path: str) -> str:
    """LOD guid pair -> geometries/<guid>.mesh, as resolve_mesh_uv derives it."""
    key = lod_path.lower().replace("\\", "/")
    if key.endswith(".mesh"):
        key = key[:-5]
    if key.startswith("geometries/"):
        key = key[11:]
    return f"geometries/{key}.mesh"


# ── rendering ─────────────────────────────────────────────────────────────────

def camera(points: np.ndarray, yaw_deg: float = YAW_DEG,
           pitch_deg: float = PITCH_DEG) -> tuple[np.ndarray, np.ndarray]:
    """World points -> (screen xy, depth). Orthographic, Z-up."""
    yaw = math.radians(yaw_deg)
    pitch = math.radians(pitch_deg)
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    rz = np.array([[cy, -sy, 0.0], [sy, cy, 0.0], [0.0, 0.0, 1.0]])
    rx = np.array([[1.0, 0.0, 0.0], [0.0, cp, -sp], [0.0, sp, cp]])
    v = points @ rz @ rx
    return np.stack([v[:, 0], -v[:, 2]], axis=1), v[:, 1]


def fit(screen: np.ndarray, size: int, margin: float = 0.08) -> np.ndarray:
    """Projected xy -> pixel xy, scaled to fill `size` with a margin."""
    lo = screen.min(axis=0)
    hi = screen.max(axis=0)
    span = np.maximum(hi - lo, 1e-6)
    scale = size * (1.0 - 2.0 * margin) / float(max(span[0], span[1]))
    px = (screen - (lo + hi) / 2.0) * scale
    px[:, 0] += size / 2.0
    px[:, 1] += size / 2.0
    return px


def shade(colours: np.ndarray, normals: np.ndarray) -> np.ndarray:
    lam = np.clip(normals @ LIGHT, 0.0, 1.0)
    return colours * (AMBIENT + (1.0 - AMBIENT) * lam)[:, None]


def tri_colours(mesh: dict, idx: np.ndarray, textures: dict, material: dict) -> np.ndarray:
    """One colour per triangle: the map at its UV centroid, or the flat colour."""
    base = np.array(material["color"], dtype=np.float32)
    tex = textures.get(material.get("texture") or "")
    n = idx.shape[0]
    uv = mesh.get("uv")
    if tex is None or uv is None:
        return np.tile(base, (n, 1))
    h, w = tex.shape[:2]
    tris = uv[idx]
    cu, cv = tris[:, :, 0].mean(axis=1), tris[:, :, 1].mean(axis=1)
    xi = np.mod((cu * w).astype(np.int64), w)
    yi = np.mod((cv * h).astype(np.int64), h)
    return tex[yi, xi] * base


def _hex(colour, alpha: float | None = None) -> tuple:
    rgb = tuple(int(min(255, max(0, round(float(c) * 255)))) for c in colour[:3])
    return rgb if alpha is None else rgb + (int(round(alpha * 255)),)


def world_vertices(mesh: dict, world: np.ndarray) -> np.ndarray:
    """Object-space vertices through a NIF world matrix.

    The matrices are column-vector matrices (translation in the last column),
    the same convention LocalMat/QuatEntry use in src/LiveModel.cpp, so the
    row-vector form is ``p @ M.T``. Doing ``p @ M`` here silently drops every
    node translation and transposes the rotation.
    """
    pos = np.concatenate([mesh["pos"], np.ones((mesh["nv"], 1), np.float32)], axis=1)
    return (pos @ world.T)[:, :3]


def clear_pane_shape(mat: dict) -> bool:
    """A geometry whose own name says it is a pane, not its frame.

    `WindowFrame`, `VisorHousing` and `LensMount` stay solid; `Visor`,
    `MarkVIIHelmet_MARKVII_visor` and `Lense` are the see-through piece.
    """
    shape = (mat.get("node") or "").lower()
    if not shape or any(token in shape for token in CLEAR_SHAPE_BLOCKERS):
        return False
    return any(token in shape for token in CLEAR_SHAPE_TOKENS)


def is_clear_shell(mat: dict) -> bool:
    """A material that carries a see-through pane, whatever it is made of."""
    name = (mat.get("mat") or "").lower()
    why = (mat.get("alpha_why") or "").lower()
    return (any(token in name for token in CLEAR_NAME_TOKENS)
            or clear_pane_shape(mat)
            or any(token in why for token in CLEAR_WHY_TOKENS))


def preview_alpha(mat: dict) -> float:
    """The alpha a tile draws, with the clear-shell visibility floor.

    A material with no database alpha defaults to 1.0, so a visor whose
    material is opaque by omission would never reach the floor. When the
    geometry itself names the pane, the preview treats it as the clear shell
    the game draws; a material that merely says `glass` but has an authored
    1.0 (an opaque body) keeps that opacity.
    """
    alpha = float(mat["alpha"])
    if clear_pane_shape(mat) and alpha >= 0.995:
        return CLEAR_PANE_ALPHA
    if alpha < CLEAR_PREVIEW_ALPHA and is_clear_shell(mat):
        return CLEAR_PREVIEW_ALPHA
    return alpha


def needs_rim(mat: dict) -> bool:
    """Only a clear shell that needs help being seen; an opaque body does not.

    The coloured vials carry an opaque `Base_Green` pane next to the clear
    shell; rimming both drew a pale line around a body that was already solid.
    """
    return is_clear_shell(mat) and preview_alpha(mat) < 0.999


def rim_style(body: np.ndarray | None) -> tuple[tuple[int, int, int], float]:
    """Rim colour and alpha, tinted by an opaque shell body when one exists.

    A clear shell around green glass got a white-looking edge; blending the
    rim toward the body colour keeps it an edge without making the vial look
    outlined. Empty glass has no body, so it keeps the stronger neutral rim.
    """
    if body is None:
        return CLEAR_RIM, CLEAR_RIM_ALPHA
    body = np.clip(np.asarray(body, dtype=np.float64), 0.0, 1.0)
    colour = tuple(int(round(255 * (0.65 * body[i] + 0.35 * (CLEAR_RIM[i] / 255.0))))
                   for i in range(3))
    return colour, CLEAR_RIM_ON_BODY_ALPHA


def filled_mask(mask: Image.Image) -> Image.Image:
    """Fill enclosed holes, so a rim follows the prop and not its panes.

    A visor or a pair of glasses is several rimmed pieces: a lens, a light
    strip, a frame. Outlining their coverage directly traces every seam and
    every hole those pieces enclose. Flooding the outside to a mid value
    leaves enclosed 0s; turning those solid keeps the actual outer contour
    while separate props (two bottles, two lenses) stay separate.
    """
    width, height = mask.size
    padded = ImageOps.expand(mask, border=1, fill=0)
    ImageDraw.floodfill(padded, (0, 0), 128)
    # Background 128 -> 0; original coverage 255 and enclosed holes 0 -> 255.
    filled = padded.point(lambda value: 0 if value == 128 else 255)
    return filled.crop((1, 1, width + 1, height + 1))


def outline_mask(mask: Image.Image, width: int = 1) -> Image.Image:
    """The outer silhouette of a binary mask, `width` pixels thick.

    This is a silhouette rim, not the triangle outlines: filling enclosed
    holes first, then eroding and subtracting, leaves the contour a whole
    prop actually has. Interior seams (a frame around a lens, a light strip
    on a visor) do not become extra outlines.
    """
    width = max(1, int(width))
    filled = filled_mask(mask)
    return ImageChops.subtract(filled, filled.filter(ImageFilter.MinFilter(2 * width + 1)))


# A pane's screen gap is not a reliable "same prop" test: two vials in a rack
# are far apart in the world and near on screen at some angles. Group the rim
# parts by their world-space bounds instead - panes that touch or overlap in 3D
# are one prop, so a visor and its light strip or a lens inside its frame get
# one silhouette. Different NIFs never merge (an assembled weapon's scope glass
# is not the magazine's), and separate vials keep their own boxes.
RIM_GROUP_MARGIN = 0.01            # of the item's world-space diagonal


def _bounds(tris: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """World bbox of a triangle array, whether it is (n,3,3) or (v,3)."""
    flat = np.asarray(tris, dtype=np.float64).reshape(-1, 3)
    return flat.min(axis=0), flat.max(axis=0)


def rim_groups(parts: list[dict]) -> list[list[int]]:
    """Indices of rim parts that belong to one world-space prop."""
    rim = [i for i, part in enumerate(parts)
           if part.get("rim") and part.get("tris") is not None]
    if not rim:
        return []
    boxes = {i: _bounds(parts[i]["tris"]) for i in rim}
    lo = np.min([_bounds(part["tris"])[0] for part in parts], axis=0)
    hi = np.max([_bounds(part["tris"])[1] for part in parts], axis=0)
    margin = float(np.linalg.norm(hi - lo)) * RIM_GROUP_MARGIN

    parent = {i: i for i in rim}

    def find(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def union(i: int, j: int) -> None:
        a, b = find(i), find(j)
        if a != b:
            parent[b] = a

    for a in range(len(rim)):
        for b in range(a + 1, len(rim)):
            i, j = rim[a], rim[b]
            if parts[i].get("nif") != parts[j].get("nif"):
                continue
            (ilo, ihi), (jlo, jhi) = boxes[i], boxes[j]
            if all(ilo[d] <= jhi[d] + margin and jlo[d] <= ihi[d] + margin
                   for d in range(3)):
                union(i, j)

    groups: dict[int, list[int]] = {}
    for i in rim:
        groups.setdefault(find(i), []).append(i)
    return list(groups.values())


# A prop's panes are separate islands in the coverage mask even after the parts
# are grouped: a visor's two eye panes, a display case's plates. Their gap is the
# prop's frame, which the tile does not draw, so outlining each island rims the
# frame gap instead of the prop. Close the gap first. The radius is a fraction
# of the tile, so the 160 and 512 previews agree.
PANE_MERGE_SCALE = 20              # one closing-radius pixel per 20 tile pixels
# The Bacta-family vial is one glass bottle split into two panes, and its frame
# gap is wider than the default close: ~11 px at 160 and ~37 px at 512. Raising
# the global radius would also fuse the eye lenses, cassette windows and spirit
# bubbles, so widen only the models that need it. Scale 13 (12 px at 160, 39 px
# at 512) merges every size from 160 to 1024; scale 14 merges at 160 but
# re-splits at 768, so a larger radius is not a safe margin here.
PANE_MERGE_SCALE_BY_MODEL = {
    "afflictioncure_antibiotics_green_01.nif": 13,
    "panacaea_01.nif": 13,
    "crafted_inhailer_penicilinx_01.nif": 13,
    "crafted_inhailer_snake_oil_01.nif": 13,
    "bacta.nif": 13,
    "bacta_pink.nif": 13,
    "bacta_orange.nif": 13,
    "bacta_green.nif": 13,
    "bacta_yellow.nif": 13,
    "bacta_red.nif": 13,
}


def _nif_name(path: str) -> str:
    """Basename of a model path, independent of the host separator."""
    return str(path).replace("/", "\\").rsplit("\\", 1)[-1].lower()


def pane_merge_scale(nifs) -> int:
    """The close scale for a group of parts, widened for known vial models.

    Keyed by NIF basename rather than by `BactaGlass` shape or glass material:
    the same shape is used by injectors and sprays that are already one pane,
    and widening those would fuse their cap or nozzle glass. A group is one
    NIF in practice; a mixed group takes the widest radius (smallest scale) so
    its parts are treated consistently.
    """
    scales = [PANE_MERGE_SCALE_BY_MODEL.get(_nif_name(nif), PANE_MERGE_SCALE)
              for nif in nifs if nif]
    return min(scales) if scales else PANE_MERGE_SCALE


def _dilate(mask: Image.Image, radius: int) -> Image.Image:
    """Dilate by a square of `radius` through a box sum, not a rank filter.

    ImageFilter.MaxFilter(2r+1) is O(r^2) per pixel: ~1.4s at 512 for r=17 and
    ~21s at 1024 for r=34. A box blur answers the same "any coverage in the
    window" in O(1) per pixel (2.5ms and 10ms).
    """
    return mask.filter(ImageFilter.BoxBlur(radius)).point(lambda value: 255 if value else 0)


def _erode(mask: Image.Image, radius: int) -> Image.Image:
    """The inverse of `_dilate`: keep a pixel only if its whole window is set."""
    return mask.filter(ImageFilter.BoxBlur(radius)).point(
        lambda value: 255 if value >= 255 else 0)


def merge_panes(mask: Image.Image, size: int, scale: float | None = None) -> Image.Image:
    """Close the screen gap between panes of one prop.

    Called per rim group, after `rim_groups` has already kept different props
    apart in world space, so a rack of vials or a scope on another NIF cannot
    be bridged here. Closing only fills gaps narrower than two radii; two panes
    far apart in the same group stay two islands and keep two contours.

    `scale` is tile pixels per closing-radius pixel; it is widened for the
    Bacta-family models by `pane_merge_scale`. A larger radius is not
    monotonic in this box close, so callers must pass the family value rather
    than round it up.

    The mask is padded by the radius first. BoxBlur truncates its window at the
    canvas edge, so a pane near the edge would otherwise survive erosion along
    the border and draw a rim along the tile edge.
    """
    radius = max(1, int(round(size / (scale or PANE_MERGE_SCALE))))
    width, height = mask.size
    padded = ImageOps.expand(mask, border=radius, fill=0)
    closed = _erode(_dilate(padded, radius), radius)
    return closed.crop((radius, radius, radius + width, radius + height))


def render_model(geoms: list[dict], size: int) -> tuple[Image.Image, int]:
    """Rasterise every geometry that has a mesh and a resolved material.

    Opaque triangles go straight onto the canvas far-to-near. A translucent
    geometry is drawn on its own layer and composited, so 30%-opaque glass over
    the specimen reads as glass over the specimen rather than as glass over the
    backdrop.
    """
    # Every geometry is framed by ONE fit over the whole item: framing each
    # part on its own drew the jar's lid scaled up to the full tile and put it
    # across the middle of the glass.
    parts: list[dict] = []
    has_rim = False
    body_acc = np.zeros(3, dtype=np.float64)
    body_n = 0
    for geom in geoms:
        mesh = geom.get("mesh")
        mat = geom.get("mat")
        if mesh is None or mat is None:
            continue
        raw = mesh["idx"]
        if raw.size < 3:
            continue
        idx = raw[:(raw.size // 3) * 3].reshape(-1, 3)
        if idx.shape[0] > MAX_TRIS:
            idx = idx[np.linspace(0, idx.shape[0] - 1, MAX_TRIS).astype(np.int64)]
        world = world_vertices(mesh, geom["world"])
        e1 = world[idx[:, 1]] - world[idx[:, 0]]
        e2 = world[idx[:, 2]] - world[idx[:, 0]]
        normals = np.cross(e1, e2)
        length = np.linalg.norm(normals, axis=1)
        keep = length > 1e-9
        if not keep.any():
            continue
        idx, normals, length = idx[keep], normals[keep], length[keep]
        colours = tri_colours(mesh, idx, geom.get("textures") or {}, mat)
        colours = np.clip(shade(np.clip(colours, 0.0, 1.0), normals / length[:, None]), 0.0, 1.0)
        alpha = preview_alpha(mat)
        opaque_shell = is_clear_shell(mat) and alpha >= 0.999
        count = colours.shape[0]
        if opaque_shell:
            body_acc += colours.mean(axis=0) * count
            body_n += count
        parts.append({"tris": world[idx], "colours": colours,
                      "alpha": alpha, "rim": needs_rim(mat),
                      "nif": geom.get("nif", "")})
    if not parts:
        return Image.new("RGB", (size, size), BACKGROUND), 0

    all_world = np.concatenate([p["tris"] for p in parts], axis=0)
    screen, depth = camera(all_world.reshape(-1, 3))
    px = fit(screen, size).reshape(-1, 3, 2)
    depth = depth.reshape(-1, 3).mean(axis=1)
    groups = rim_groups(parts)
    group_of = {index: group for group, members in enumerate(groups)
                for index in members}
    group_masks = [Image.new("L", (size, size), 0) for _ in groups]
    group_draws = [ImageDraw.Draw(mask) for mask in group_masks]

    opaque: list[tuple[float, np.ndarray, tuple]] = []
    layers: list[tuple[float, list[tuple[np.ndarray, tuple, float]]]] = []
    at = 0
    total = 0
    for index, part in enumerate(parts):
        n = part["tris"].shape[0]
        total += n
        tri_px = px[at:at + n]
        tri_depth = depth[at:at + n]
        at += n
        if part["rim"] and index in group_of:
            draw = group_draws[group_of[index]]
            for k in range(n):
                tri = tri_px[k]
                draw.polygon([(tri[0, 0], tri[0, 1]), (tri[1, 0], tri[1, 1]),
                              (tri[2, 0], tri[2, 1])], fill=255)
            has_rim = True
        alpha = part["alpha"]
        if alpha >= 0.999:
            for k in range(n):
                opaque.append((float(tri_depth[k]), tri_px[k], _hex(part["colours"][k])))
        else:
            polys = [(tri_px[k], _hex(part["colours"][k]), alpha) for k in range(n)]
            layers.append((float(tri_depth.mean()) if n else 0.0, polys))

    img = Image.new("RGB", (size, size), BACKGROUND)
    draw = ImageDraw.Draw(img)
    for _depth, tri, rgb in sorted(opaque, key=lambda t: -t[0]):
        draw.polygon([(tri[0, 0], tri[0, 1]), (tri[1, 0], tri[1, 1]),
                      (tri[2, 0], tri[2, 1])], fill=rgb)
    for _depth, polys in sorted(layers, key=lambda t: -t[0]):
        layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        ldraw = ImageDraw.Draw(layer)
        for tri, rgb, alpha in polys:
            ldraw.polygon([(tri[0, 0], tri[0, 1]), (tri[1, 0], tri[1, 1]),
                           (tri[2, 0], tri[2, 1])], fill=rgb + (int(round(alpha * 255)),))
        img = Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")
    if has_rim:
        # The 0.16 floor gives the body a fill; this gives the contour an edge.
        # A 1px rim at 160 is the difference between a faint rectangle and a
        # readable vial; at 512 it scales to 2px so the proportion is the same.
        width = max(1, int(round(size / 320)))
        rim_colour, rim_alpha = rim_style(body_acc / body_n if body_n else None)
        edge = Image.new("L", (size, size), 0)
        for members, mask in zip(groups, group_masks):
            scale = pane_merge_scale([parts[i].get("nif", "") for i in members])
            edge = ImageChops.lighter(edge, outline_mask(merge_panes(mask, size, scale), width))
        rim = Image.new("RGBA", (size, size), rim_colour + (0,))
        rim.putalpha(edge.point(lambda value: int(round(value * rim_alpha))))
        img = Image.alpha_composite(img.convert("RGBA"), rim).convert("RGB")
    return img, total


# ── item pipeline ─────────────────────────────────────────────────────────────

class Textures:
    """Decoded colour maps, bounded so a shelf run cannot grow forever.

    A decoded 2048x2048 colour map is 48 MB as float32 and a 4096x4096 one is
    192 MB, so both an entry count and the decoded bytes have to be bounded: a
    full seven-form sweep died with MemoryError after a few hundred items.
    Anything above 4096x4096 is not drawn - the sweep hit 16384x16384 records
    whose float decode alone would be 3 GB - and the kept set stays near 512 MB.
    """

    LIMIT = 64
    MAX_PIXELS = 4096 * 4096
    MAX_BYTES = 512 * 1024 * 1024

    def __init__(self, store: audit.AssetStore) -> None:
        self.store = store
        self.cache: dict[str, np.ndarray | None] = {}
        self.formats: dict[str, int] = {}
        self.bytes = 0

    def get(self, key: str) -> np.ndarray | None:
        if not key:
            return None
        if key in self.cache:
            return self.cache[key]
        info = self.store.texture(key)
        img = None
        if info is not None:
            self.formats[key] = info.dxgi
            if 0 < info.width * info.height <= self.MAX_PIXELS:
                img = decode_dds(info.payload, info.dxgi, info.width, info.height)
        cost = img.nbytes if img is not None else 0
        if len(self.cache) >= self.LIMIT or self.bytes + cost > self.MAX_BYTES:
            self.cache.clear()
            self.formats.clear()
            self.bytes = 0
        self.cache[key] = img
        self.bytes += cost
        return img


# Colour-map slots a Starfield .mat names itself. The CDB holds the shared
# component sets, so a material that is not in it (or is layered, like
# swdecoration\droidhead.mat) still declares its own set inline:
# "FileName" : "Data\\Textures\\..._Color.dds".
#
# The role is read from the LAST underscore token of the file name, because the
# spelling is the author's: the base game's own sets use `_color.dds`, but a
# mod set is just as likely to name `Cantina_SmallGenerator_C.dds` (the
# Cantina mod's toaster) or `Textures\SWProps\Datapad\color.dds` (the Star Wars
# prop pack, whose file is named after the slot, not the material). Requiring a
# `_color` substring skipped both and drew them flat.
ALBEDO_ROLES = frozenset(("color", "colour", "albedo", "diffuse", "basecolor",
                          "clr", "c", "d"))
NON_ALBEDO_ROLES = frozenset(("nrm", "normal", "norm", "normalmap", "rough",
                              "roughness", "roughmask", "metal", "metalness",
                              "metallic", "mask", "opacity", "alpha", "ao",
                              "ambientocclusion", "glow", "emissive", "emissivemask",
                              "height", "disp", "displacement", "specular", "spec",
                              "gloss", "glossiness", "transmissive", "transmittance",
                              "sss", "rma", "orm", "pbr", "curvature", "detailmask",
                              "blendmask", "flow", "cube", "lut"))
_MAT_COLOR_CACHE: dict[str, str] = {}


def _map_role(path: str) -> str:
    """The role token of a texture path: `.../toaster_roughness.dds` -> `roughness`."""
    return path[:-4].rsplit("/", 1)[-1].rsplit("_", 1)[-1].casefold()


def _color_from_material(mat_path: str, store) -> str:
    """The colour map named inside the material file, or "".

    Last resort behind the route table and the CDB: 95 of 400 Misc items came
    back flat grey because their material is neither (droidhead.mat, the Akila
    kitchenware set). Reading the map the .mat itself points at is what the
    game does; the key is normalised to the lowercase `textures/...` form the
    store indexes.

    A texture set lists its maps in slot order - colour, normal, roughness,
    metal, ... - so when no file name carries an albedo role at all, the first
    map that is not one of the other slots is the colour (that is how the
    Cantina mod's `_C` pair and the prop pack's `color.dds` resolve).
    """
    key = mat_path.casefold()
    if key in _MAT_COLOR_CACHE:
        return _MAT_COLOR_CACHE[key]
    found = ""
    fallback = ""
    blob = store.read(mat_path)
    if blob:
        text = blob.decode("latin-1", "replace")
        for match in re.finditer(r'"FileName"\s*:\s*"([^"]+)"', text):
            path = match.group(1).replace("\\\\", "\\").replace("\\", "/").casefold()
            if path.startswith("data/"):
                path = path[5:]
            if not path.startswith("textures/") or not path.endswith(".dds"):
                continue
            role = _map_role(path)
            if role in NON_ALBEDO_ROLES:
                continue
            if store.texture(path) is None:
                continue
            if role in ALBEDO_ROLES:
                found = path
                break
            if not fallback:
                fallback = path
    found = found or fallback
    _MAT_COLOR_CACHE[key] = found
    return found


def resolve_material(mat_path: str, node: str, form: int, cdb: audit.Cdb,
                     swaps: audit.Swaps, mean_of, present_of, store=None) -> dict:
    """The plugin's per-material decision: final path, rgb, alpha, map."""
    authored = audit.AUTHORED_SWAPS.get(form) or {}
    swapped = authored.get(audit.book_key(mat_path)) if authored else None
    how = "authored" if swapped else ""
    if not swapped:
        swapped = swaps.apply(form, mat_path)
        how = "table" if swapped else ""
    mat = swapped or mat_path
    route = audit.exact_route(mat, node)
    own, own_how = cdb.own_set(mat)
    own_color = (own or {}).get("color", "")
    installed = bool(own_color) and present_of(own_color)
    color, why = audit.color_decision(mat, cdb, mean_of, present_of)
    alpha, translucent, alpha_why = audit.blend_alpha(mat, *cdb.alpha(mat))
    texture = None
    if route:
        texture = route
    elif installed:
        texture = own_color
    elif store is not None:
        # Neither the route table nor the CDB knows this material; its own file
        # still names the colour map. Without it the tile is flat grey.
        texture = _color_from_material(mat, store) or None
    if texture is None:
        # A layered material keeps its art on an ancestor (`AK_Kitchenware01_
        # Rubber01` -> `AK_Kitchenware01_Base`). The parent edges are in the CDB
        # but its endpoints have no names there, so the table comes from the
        # game process; without that sidecar this costs one dict lookup and
        # changes nothing (see parent_index.py).
        via_parent = parent_index.colour_via_parent(mat, cdb, parent_index.cached())
        if via_parent:
            texture = via_parent[0]
    if texture is None and audit.is_crafted_drink_bag(mat) and "pouch" in audit.norm(mat):
        alpha, translucent = 0.55, True
    normalized = audit.norm(mat)
    effect = any(t in normalized for t in EFFECT_TOKENS)
    if not effect and texture is None and audit.is_decal_sheet(mat):
        # A decal with an opacity mask and no colour map is a mask, not a
        # surface: drawn flat grey it is the black net on Sparkling Wine and
        # the grey panels wrapping the Isotopic Coolant jar.
        effect = True
    if effect:
        alpha, translucent = 0.0, True
    return {"mat": mat, "swap": how, "texture": texture, "node": node,
            "color": tuple(float(c) for c in color), "alpha": float(alpha),
            "translucent": translucent, "color_why": why,
            "alpha_why": alpha_why, "set": own_how, "effect": effect}


# One plugin record walk per store: the OMOD family scan is several seconds on
# a 1.4 GB master, and every weapon of a run asks the same question.
_ASSEMBLY_CACHE: dict[int, "omod_assembly.Assemblies"] = {}


def _assemblies(store: audit.AssetStore, existing=None):
    if existing is not None:
        return existing
    cached = _ASSEMBLY_CACHE.get(id(store))
    if cached is None:
        cached = _ASSEMBLY_CACHE[id(store)] = omod_assembly.Assemblies(store)
    return cached


def is_proxy_model(path: str) -> bool:
    """The dressing-box world model on a spacesuit/clothes ARMO.

    Mirrors `IsProxyModelPath` in `NativeModel.cpp`: a SetDressing container
    NIF is an inventory prop, not the garment, and must not be drawn when the
    record points at the real armor addon.
    """
    low = path.lower().replace("/", "\\")
    return "setdressing\\" in low and "container" in low


def catalog_models(item: dict) -> list[str]:
    """Every resolved NIF the catalog carries, duplicate paths removed.

    The ARMO's `MODL` subrecord is a form link to its ARMA addon, not a string,
    so the plugin walk cannot hand the workbench a path. The dump already
    resolved the world model and every addon into `nifPaths` (the same column
    `mo2_audit.row_nifs` reads). This is the raw list: an ARMO's dressing-box
    proxy is filtered by `catalog_addons`, not here, because a MISC container
    legitimately lives under `SetDressing\\Container\\`.
    """
    seen: set[str] = set()
    out: list[str] = []
    for raw in item.get("nif_paths") or ():
        path = str(raw).strip()
        if not path:
            continue
        key = path.lower().replace("/", "\\")
        if key in seen:
            continue
        seen.add(key)
        out.append(path)
    return out


def _looks_like_nif(path: str) -> bool:
    return path.lower().replace("/", "\\").endswith(".nif")


def _path_key(path: str) -> str:
    return path.replace("/", "\\").lower()


def _female_preferred(paths: list[str]) -> list[str]:
    """Drop a `_M` addon when the same stem has a `_F` path.

    `FormModelPaths` walks the ARMO's addon array and takes the female model
    when the ARMA has one, else the male. The dump flattens both genders into
    `nifPaths` (male first), so a tile drawn from every entry would overlap a
    male and female copy of the same garment. Pairing the basenames recovers
    the engine's one-model-per-addon choice without the link table.
    """
    keys = {_path_key(p) for p in paths}
    out = []
    for index, path in enumerate(paths):
        key = _path_key(path)
        if key.endswith("_m.nif"):
            # Mods often name the female model with no suffix at all (swvt's
            # `UpperBody.nif` beside `UpperBody_M.nif`), so the `_F` check
            # alone leaves both copies in the tile. The dump emits each
            # ARMA's male model first and its female model second.
            nxt = _path_key(paths[index + 1]) if index + 1 < len(paths) else ""
            unfixed_female = nxt == key[:-6] + ".nif"
            if key[:-6] + "_f.nif" in keys or unfixed_female:
                continue
        out.append(path)
    return out


def is_world_model(path: str) -> bool:
    """A dropped-armour ground model, which `FormModelPaths` never draws.

    The dump lists an ARMO's `worldModels` before its ARMA addons, but only a
    `SetDressing\\...Container...` path is filtered by `is_proxy_model`. A
    ground model can also sit beside the addon under its own folder; its
    basename ends `_go.nif` (helmet_go, backpack_go, SSMiner_Backpack_GO) or
    contains `groundcontainer`. Drawing one puts the folded prop on top of the
    worn armour, so it is dropped when an addon remains.
    """
    base = path.replace("/", "\\").rsplit("\\", 1)[-1].lower()
    return base.endswith("_go.nif") or "groundcontainer" in base


def catalog_addons(item: dict) -> list[str]:
    """The ARMO addon NIFs in the order `FormModelPaths` asks for them."""
    paths = [p for p in _female_preferred(catalog_models(item))
             if not is_proxy_model(p)]
    addons = [p for p in paths if not is_world_model(p)]
    # A record with no addon path at all still has its authored world model;
    # drawing that beats an empty tile, and it is the only geometry on disk.
    return addons or paths


def _unique_paths(paths: list[str]) -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    for path in paths:
        if not path:
            continue
        key = _path_key(path)
        if key in seen:
            continue
        seen.add(key)
        out.append(path)
    return out


def _load_candidate(models: list[str], store: audit.AssetStore):
    """Read one candidate NIF set; return geoms, missing count, unplaced, error."""
    blobs = [store.read(path) for path in models]
    missing = sum(1 for blob in blobs if not blob)
    # A dummy-base gun is several NIFs. Each part is authored at its own
    # origin and the NIFs carry the C-/P- connect graph the engine mounts them
    # with, so a list of paths is not yet an assembly: resolve one placement
    # per part first. Single-NIF items resolve to identity and are untouched.
    places, mounted = connect_points.resolve_placements(
        [connect_points.parse_connect(blob) for blob in blobs])
    # The C++ resolver draws an unhosted part at identity (some parts are
    # authored in place, e.g. a blade on its handle), so do the same and only
    # record the fallbacks. They are the parts whose assembly pick may be
    # wrong, and the page shows them next to the tile.
    unplaced = [Path(models[i]).name for i, ok in enumerate(mounted) if not ok]
    geoms: list[dict] = []
    errors: list[str] = []
    for path, blob, place in zip(models, blobs, places):
        if not blob:
            continue
        try:
            nif = Nif(blob)
            for geom in walk_geometries(blob, nif):
                geom = dict(geom)
                geom["world"] = place @ geom["world"]
                geom["nif"] = path
                geoms.append(geom)
        except Exception as exc:
            # A geometry-less placeholder addon (Pyke_Details2, the Assault
            # decal sheet) has a valid NIF header but no block table to walk.
            # One such part must not blank the nine sound parts beside it; it
            # is only a finding when no part of the set could be read.
            errors.append(f"{Path(path).name}: {exc}")
    if not geoms and errors:
        return [], missing, unplaced, f"NIF parse failed: {errors[0]}"
    return geoms, missing, unplaced, ""


# C++ FormModelPaths gives a key with no model string the generic keycard NIF
# (NativeModel then renders that stand-in). Mirror it here, or the offline
# audit calls 46 perfectly previewable keys "no MODL".
KEYCARD_FALLBACK = r"SetDressing\Keycards\KeycardGen01.nif"

# `FormModelPaths` gives ammo its crate MODL and, when present, its shell
# casing. Several records name a `type{a..f}_{medium,large,xlarge}.nif` crate
# that ships in no archive or mod; the game has no geometry to draw either.
# The generic `ammobox_01.nif` is a shipped crate and is the honest preview
# stand-in for those absent placeholders. A placeholder that does exist, and
# every real casing path, is left untouched.
AMMO_CRATE_FALLBACK = r"SetDressing\ammo_crates\ammobox_01.nif"
_AMMO_CRATE_PLACEHOLDER = re.compile(r"^type[a-f]_(?:medium|large|xlarge)\.nif$", re.I)


def is_ammo_crate_placeholder(path: str) -> bool:
    base = path.replace("/", "\\").rsplit("\\", 1)[-1]
    return bool(_AMMO_CRATE_PLACEHOLDER.match(base))


def ammo_models(paths: list[str], store: audit.AssetStore) -> list[str]:
    """Repoint an absent type* ammo-crate placeholder at the shipped crate."""
    out: list[str] = []
    for path in paths:
        if is_ammo_crate_placeholder(path) and not store.read(path):
            path = AMMO_CRATE_FALLBACK
        out.append(path)
    return _unique_paths(out)


def prepare_item(item: dict, store: audit.AssetStore, plugins: audit.Plugins,
                 swaps: audit.Swaps, cdb: audit.Cdb, textures: Textures,
                 meshes: dict, assemblies=None,
                 material_files: bool = True) -> tuple[list[dict], dict]:
    """Resolve one item's geometries + materials. Returns (geoms, metadata).

    The candidate order mirrors `FormModelPaths` in `src/ShopExplorer.cpp`:
    an AMMO draws its crate and shell casing together; a KEYM with no model
    gets the generic keycard; an ARMO draws every linked addon when its world
    model is the dressing-box proxy, and falls back to that set when a real
    world model cannot be parsed. That keeps an offline blank tile honest - it
    is a path the game also cannot draw, not a gap in this tool.

    `material_files=False` is for bulk scans: it skips opening a .mat just to
    pick a colour, which is the expensive per-item step when textures are
    already skipped. It does not change geometry, alpha or `needs_rim`.
    """
    form = item["form"]
    kind = item.get("type")
    record = plugins.record(item["plugin"], form) or {}
    model = record.get("model", "")
    if model and not _looks_like_nif(model):
        model = ""                       # an ARMO MODL is an ARMA form link
    addons = catalog_addons(item) if kind == "ARMO" else []
    fallbacks: list[list[str]] = []
    primary: list[str] = []
    if model and not (kind == "ARMO" and is_proxy_model(model)):
        primary = [model]
    meta = {"model": model, "geoms": 0, "tris": 0, "textured": 0,
            "status": "", "materials": [], "parts": [], "unplaced": []}
    if kind == "ARMO":
        # A proxy world model is the dressing box, not the garment: the game
        # draws every linked addon in its bind pose. A real world model keeps
        # its single path first and only falls back to those addons when it
        # cannot be parsed, which is NativeModel's candidate policy.
        if not primary:
            primary = addons
        else:
            fallbacks.append([p for p in addons if _path_key(p) != _path_key(model)])
    elif kind == "AMMO":
        # `FormModelPaths` pushes the crate model and the shell casing together;
        # NativeModel adopts both. The dump's `nifPaths` is that pair when the
        # round has a casing, and only the crate when it does not. A crate
        # placeholder that is absent from every archive is replaced by the
        # shipped generic crate; the casing is never replaced.
        primary = ammo_models([model] + [p for p in catalog_models(item)
                                         if _looks_like_nif(p)], store)
    elif kind == "KEYM":
        # A key has no model string of its own; the plugin substitutes the
        # generic keycard. Do not add it when a real model path exists, or a
        # missing key asset would be masked by a path the game never falls to.
        if not primary:
            fallbacks.append([KEYCARD_FALLBACK])
    elif kind == "WEAP":
        # A dummy-base weapon has no geometry of its own: Razorback's NIF is
        # the empty BullpupDummyReceiver and the gun is receiver + grip +
        # barrel OMODs. A melee dummy housing is different: it carries no
        # geometry either, but its P-Blade/P-Handle connect points place the
        # blade and handle, so the housing must stay as the placement root.
        if assemblies is None:
            assemblies = _assemblies(store, assemblies)
        parts = assemblies.nifs_for(item["plugin"], form, model)
        if parts:
            # The melee housing is the connect-point root; Breech Block's own
            # NIF is a complete receiver and FormModelPaths puts it first too.
            if model and ("meleedummyhousing" in _path_key(model)
                          or "breechblock" in _path_key(model)):
                parts = [model] + parts
            primary = _unique_paths(parts)
    if not primary and kind in ("ALCH", "BOOK", "MISC", "WEAP"):
        # The plugin walk and the catalog dump should agree for these forms,
        # but a record the walk cannot type still has a resolved path here.
        catalog_nifs = [p for p in catalog_models(item) if _looks_like_nif(p)]
        if catalog_nifs:
            primary = catalog_nifs[:1]
    fallbacks = [c for c in (_unique_paths(c) for c in fallbacks) if c]
    candidates = [primary] if primary else []
    seen = {_path_key(p) for p in primary}
    for candidate in fallbacks:
        candidate = [p for p in candidate if _path_key(p) not in seen]
        if not candidate:
            continue
        seen.update(_path_key(p) for p in candidate)
        candidates.append(candidate)
    if not candidates:
        meta["status"] = "no MODL"
        return [], meta

    geoms: list[dict] = []
    unplaced: list[str] = []
    models: list[str] = []
    last_status = "no MODL"
    for candidate in candidates:
        cand_geoms, missing, cand_unplaced, error = _load_candidate(candidate, store)
        if error:
            last_status = error
            continue
        if cand_geoms:
            geoms, unplaced, models = cand_geoms, cand_unplaced, candidate
            break
        last_status = "NIF missing" if missing else "no renderable geometry"
    if not geoms:
        meta["status"] = last_status
        return [], meta

    model = models[0]
    meta["model"] = model
    meta["parts"] = list(models)
    if len(models) > 1:
        meta["model"] = " + ".join(Path(p).name for p in models)
    if unplaced:
        meta["unplaced"] = unplaced

    def mean_of(key: str):
        img = textures.get(key)
        if img is None:
            return None
        return tuple(float(v) for v in img.reshape(-1, 3).mean(axis=0))

    def present_of(key: str) -> bool:
        return textures.get(key) is not None

    out = []
    for geom in geoms:
        if not geom["material"] or not geom["lods"]:
            continue
        # Inline geometry (a Blender export) has no archive entry at all: the
        # LOD0 blob sits in the NIF itself, see walk_geometries. Keyed by the
        # NIF plus the shape name so one file's shapes do not share a slot.
        if geom.get("inline"):
            key = f"inline:{geom.get('nif', model)}#{geom['name']}"
        else:
            key = mesh_key(geom["lods"][0][2])
        if key not in meshes:
            mesh = parse_mesh(geom.get("inline") or store.read(key) or b"")
            if mesh is None and geom.get("inline_alt"):
                mesh = parse_mesh(geom["inline_alt"])
            if len(meshes) >= MESH_CACHE_LIMIT:
                meshes.clear()
            meshes[key] = mesh
        mesh = meshes[key]
        mat = resolve_material(geom["material"], geom["name"], form, cdb, swaps,
                               mean_of, present_of,
                               store if material_files else None)
        if mat["effect"]:
            # Additive overlays carry no surface colour; drawing them (even
            # faintly) hides the parts they are meant to light.
            meta["materials"].append(mat)
            continue
        tex = textures.get(mat["texture"] or "")
        geom = dict(geom)
        geom["mesh"] = mesh
        geom["mat"] = mat
        geom["textures"] = {mat["texture"]: tex} if mat["texture"] and tex is not None else {}
        if tex is not None:
            meta["textured"] += 1
        meta["geoms"] += 1
        meta["materials"].append(mat)
        out.append(geom)
    if not out:
        meta["status"] = "no renderable geometry"
    return out, meta


# ── output ────────────────────────────────────────────────────────────────────

def placeholder(size: int, label: str) -> Image.Image:
    """A tile that says why nothing was drawn, instead of a fake render."""
    img = Image.new("RGB", (size, size), BACKGROUND)
    draw = ImageDraw.Draw(img)
    draw.text((6, size // 2 - 6), label[:24], fill=(120, 124, 132))
    return img


def write_index(rows: list[dict], path: Path, tiles: Path, shelf: str,
                source: str, size: int) -> None:
    """One page per shelf: a labelled grid of tiles with a filter box."""
    base = Path(path).parent
    cards = []
    for r in rows:
        rel = Path(os.path.relpath(tiles / r["png"], base))
        mat_title = " | ".join(f"{m['mat'].rsplit(chr(92), 1)[-1]} a={m['alpha']:.2f}"
                               for m in r["materials"][:6])
        search = f"{r['item']} {r['form']} {r['model']} {mat_title}".lower()
        cards.append(
            f'<figure data-s="{search}" title="{mat_title}">'
            f'<img src="{rel.as_posix()}" width="{size}" height="{size}" alt="">'
            f'<figcaption><b>{r["item"]}</b><span>{r["form"]}</span>'
            f'<em>{r["geoms"]} geom / {r["tris"]} tri / {r["textured"]} tex'
            f'{" - " + r["status"] if r["status"] else ""}</em></figcaption></figure>')
    html = f"""<!doctype html>
<html lang="en"><meta charset="utf-8">
<title>{shelf} contact sheet</title>
<style>
:root {{ color-scheme: dark; }}
body {{ margin: 0; padding: 18px; background: #101318; color: #d8dbe0;
        font: 12px/1.4 ui-sans-serif, system-ui, sans-serif; }}
h1 {{ font-size: 17px; margin: 0 0 2px; }}
.meta {{ color: #8a8f98; margin: 0 0 12px; }}
input {{ width: 320px; max-width: 100%; padding: 6px 8px; margin-bottom: 14px;
         background: #1a1e26; color: #d8dbe0; border: 1px solid #333a46; border-radius: 4px; }}
.grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax({size + 18}px, 1fr));
         gap: 10px; }}
figure {{ margin: 0; background: #161a22; border: 1px solid #232936; border-radius: 6px;
          padding: 6px; }}
img {{ display: block; border-radius: 4px; image-rendering: auto; }}
figcaption {{ margin-top: 5px; }}
figcaption b {{ display: block; font-weight: 600; overflow-wrap: anywhere; }}
figcaption span {{ display: block; color: #7f8590; font-size: 10px; letter-spacing: .04em; }}
figcaption em {{ display: block; color: #7f8590; font-size: 10px; font-style: normal; }}
</style>
<h1>{shelf} contact sheet</h1>
<p class="meta">{len(rows)} item(s) &middot; {size}px tiles &middot; generated offline from
{source}; flat-shaded, painter-sorted, translucency composited on the backdrop</p>
<input id="f" placeholder="Filter by item, form, model or material">
<div class="grid">
{chr(10).join(cards)}
</div>
<script>
f.addEventListener('input', () => {{
  const q = f.value.toLowerCase();
  for (const el of document.querySelectorAll('figure'))
    el.hidden = q !== '' && !el.dataset.s.includes(q);
}});
</script>
</html>
"""
    path.write_text(html, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--shelf", action="append", default=[], choices=sorted(audit.SHELVES))
    ap.add_argument("--aid", action="store_true", help="the Aid shelf (--shelf aid)")
    ap.add_argument("--item", action="append", default=[], help="substring of the item name")
    ap.add_argument("--form", action="append", default=[], help="form id, hex")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--size", type=int, default=128)
    ap.add_argument("--out", default=None, help="index HTML path")
    ap.add_argument("--tiles", default=None, help="tile directory (PNG per item)")
    ap.add_argument("--catalog", default=None)
    ap.add_argument("--cdb", default=str(audit.CDB))
    ap.add_argument("--data", default=str(audit.GAME_DATA))
    ap.add_argument("--mods-root", default=str(audit.MODS_ROOT))
    ap.add_argument("--swaps", default=str(audit.SWAPS))
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    catalog = audit.find_catalog(a.catalog)
    if not catalog:
        print("no catalog CSV found; pass --catalog", file=sys.stderr)
        return 2
    items = audit.parse_catalog(catalog)
    unique = audit.select_items(items, a.form, a.item, a.aid, a.limit, a.shelf)
    if not unique:
        print("nothing selected: pass --aid, --shelf, --item or --form", file=sys.stderr)
        return 2
    picked = list(dict.fromkeys((["aid"] if a.aid else []) + list(a.shelf)))
    label = " + ".join({"books": "Books"}.get(s, s.capitalize()) for s in picked) or "Selection"

    out = Path(a.out) if a.out else HERE.parents[3] / "reports" / f"contact-{picked[0] if picked else 'selection'}.html"
    tiles = Path(a.tiles) if a.tiles else out.parent / "contact" / (picked[0] if picked else "selection")
    tiles.mkdir(parents=True, exist_ok=True)

    store = audit.AssetStore([Path(a.data)], Path(a.mods_root) if a.mods_root else None, not a.quiet)
    plugins = audit.Plugins(store)
    swaps = audit.Swaps(Path(a.swaps) if a.swaps else None)
    cdb = audit.Cdb(Path(a.cdb))
    textures = Textures(store)
    meshes: dict[str, dict | None] = {}

    rows = []
    for n, item in enumerate(unique, 1):
        geoms, meta = prepare_item(item, store, plugins, swaps, cdb, textures, meshes)
        png = f"{item['form']:08X}.png"
        if geoms:
            img, tris = render_model(geoms, a.size)
            meta["tris"] = tris
        else:
            img = placeholder(a.size, meta["status"] or "no geometry")
        img.save(tiles / png)
        rows.append({"item": item["name"], "form": f"{item['form']:08X}",
                     "model": meta["model"], "geoms": meta["geoms"],
                     "tris": meta["tris"], "textured": meta["textured"],
                     "status": meta["status"], "png": png,
                     "materials": meta["materials"]})
        if not a.quiet and (n % 25 == 0 or n == len(unique)):
            print(f"[contact] {n}/{len(unique)}", file=sys.stderr)

    write_index(rows, out, tiles, label, catalog.name, a.size)
    tsv = out.with_suffix(".tsv")
    with open(tsv, "w", encoding="utf-8", newline="") as fh:
        fh.write("item\tform\tmodel\tgeoms\ttris\ttextured\tstatus\tpng\n")
        for r in rows:
            fh.write("\t".join(str(r[k]) for k in
                               ("item", "form", "model", "geoms", "tris", "textured",
                                "status", "png")) + "\n")
    print(f"[contact] wrote {len(rows)} tile(s) to {tiles} and {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
