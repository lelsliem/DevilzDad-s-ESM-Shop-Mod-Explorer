"""BSConnectPoint assembly placement for the offline renderer.

A dummy-base weapon is a list of OMOD part NIFs, and each part is authored at
its own origin. The game does not draw them there: the NIFs declare a mounting
graph. A part says which slots it fills with ``C-<Slot>`` children; a host says
where a slot sits with ``P-<Slot>`` parents on a named node (or the file root),
each carrying an entry transform. A part's root matrix is

    parentPlacement * hostNodeWorld * entryTransform

The ``C-Receiver`` holder (or path 0 when nothing declares one) is the identity
root, and chains resolve in path order with hosts from earlier placements
visible in the same pass.

This is a direct port of ``NativeModel::ResolveAssemblyPlaces`` and
``LiveModel::ParseNifConnects`` (src/NativeModel.cpp, src/LiveModel.cpp), which
was validated offline over all 657 base-game weapon NIFs: every part of all 13
assemblies placed, attach points matched receiver geometry to <1 mm. The
matrices are column-vector matrices, exactly like the C++ code: translation in
m[3], m[7], m[11], and a point is transformed as ``p' = M * p``.

    from connect_points import resolve
    places = resolve([store.read(path) for path in part_paths])
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import nif_uv  # noqa: E402

IDENTITY = np.eye(4, dtype=np.float64)

# A malformed block is skipped, exactly like the C++ parser; a malformed
# Children run stops at its first bad entry. The limits are the C++ limits.
MAX_CONNECT_ENTRIES = 1024
MAX_CHILD_NAME = 512


def _node_prefix(raw: bytes) -> tuple[dict | None, int | None]:
    """Name index + local transform at the start of a NiNode block."""
    r = nif_uv.R(raw)
    name = r.u32()
    extras = r.u32()
    if extras > 4096:
        return None, None
    for _ in range(extras):
        r.i32()
    r.i32()          # controller
    r.u32()          # flags
    t = [r.f32() for _ in range(3)]
    rot = [r.f32() for _ in range(9)]
    scale = r.f32()
    r.i32()          # collision object
    if not r.ok or not (0.0 < scale < 1e6):
        return None, None
    return {"name": name, "t": t, "rot": rot, "scale": scale}, r.o


def local_matrix(part: dict) -> np.ndarray:
    """NiNode local transform, column-vector convention (same as LocalMat)."""
    t, rot, scale = part["t"], part["rot"], part["scale"]
    return np.array([
        [rot[0] * scale, rot[1] * scale, rot[2] * scale, t[0]],
        [rot[3] * scale, rot[4] * scale, rot[5] * scale, t[1]],
        [rot[6] * scale, rot[7] * scale, rot[8] * scale, t[2]],
        [0.0, 0.0, 0.0, 1.0],
    ], dtype=np.float64)


def quat_entry(r: nif_uv.R) -> np.ndarray | None:
    """quat(w,x,y,z) + translation + scale -> the connect entry matrix."""
    f = [r.f32() for _ in range(8)]
    if not r.ok or any(not math.isfinite(v) for v in f):
        return None
    w, x, y, z, tx, ty, tz, scale = f
    if not (0.0 < scale < 1e6):
        return None
    return np.array([
        [(1 - 2 * (y * y + z * z)) * scale, (2 * (x * y - z * w)) * scale,
         (2 * (x * z + y * w)) * scale, tx],
        [(2 * (x * y + z * w)) * scale, (1 - 2 * (x * x + z * z)) * scale,
         (2 * (y * z - x * w)) * scale, ty],
        [(2 * (x * z - y * w)) * scale, (2 * (y * z + x * w)) * scale,
         (1 - 2 * (x * x + y * y)) * scale, tz],
        [0.0, 0.0, 0.0, 1.0],
    ], dtype=np.float64)


def _connect_blocks(blob: bytes, nif: nif_uv.Nif) -> tuple[list[dict], list[str]]:
    """The Parents and Children blocks of one NIF."""
    parents: list[dict] = []
    childs: list[str] = []
    for i in range(nif.nb):
        kind = nif.type_of(i)
        if kind not in ("BSConnectPoint::Parents", "BSConnectPoint::Children"):
            continue
        raw = blob[nif.offsets[i]:nif.offsets[i] + nif.sizes[i]]
        r = nif_uv.R(raw)
        r.u32()      # block format version (parents=3, children=2)
        if kind.endswith("Parents"):
            count = r.u32()
            if not r.ok or count > MAX_CONNECT_ENTRIES:
                continue
            for _ in range(count):
                node = r.hstr(4096)
                name = r.hstr(4096)
                if not r.ok:
                    break
                matrix = quat_entry(r)
                if matrix is not None:
                    parents.append({"node": node, "name": name, "m": matrix})
        else:
            for _ in range(5):       # pad after the version
                r.u8()
            while r.ok and r.o + 4 <= len(raw):
                length = r.u32()
                if not r.ok or length == 0 or length > MAX_CHILD_NAME or r.o + length > len(raw):
                    break
                text = raw[r.o:r.o + length]
                if any(c < 32 or c > 126 for c in text):
                    break
                childs.append(text.decode("latin-1"))
                r.o += length
    return parents, childs


def parse_connect(blob: bytes | None) -> dict:
    """Connect data and named-node worlds for one NIF.

    Returns ``{"parents", "childs", "node_worlds", "root_world"}``. A missing or
    unparseable NIF is an empty answer, not an error: the caller falls back to
    identity placement, which is what the C++ resolver does for a part whose
    connects it could not read.
    """
    empty = {"parents": [], "childs": [], "node_worlds": {}, "root_world": IDENTITY.copy()}
    if not blob:
        return empty
    try:
        nif = nif_uv.Nif(blob)
    except Exception:
        return empty

    parents, childs = _connect_blocks(blob, nif)
    node_type = nif.idx_of("NiNode")
    if node_type < 0:
        return {"parents": parents, "childs": childs, "node_worlds": {},
                "root_world": IDENTITY.copy()}

    children: dict[int, list[int]] = {}
    referenced: set[int] = set()
    for i in range(nif.nb):
        if nif.type_idx[i] != node_type:
            continue
        raw = blob[nif.offsets[i]:nif.offsets[i] + nif.sizes[i]]
        part, end = _node_prefix(raw)
        if part is None:
            continue
        r = nif_uv.R(raw, end)
        kids = []
        for _ in range(min(r.u32(), 4096)):
            c = r.i32()
            if 0 <= c < nif.nb:
                kids.append(c)
                referenced.add(c)
        children[i] = kids

    root = next((i for i in range(nif.nb)
                 if nif.type_idx[i] == node_type and i not in referenced), 0)
    node_worlds: dict[str, np.ndarray] = {}
    root_world = IDENTITY.copy()
    stack = [(root, IDENTITY.copy())]
    guard = 0
    while stack and guard < 100_000:
        guard += 1
        block, parent_world = stack.pop()
        raw = blob[nif.offsets[block]:nif.offsets[block] + nif.sizes[block]]
        part, _ = _node_prefix(raw)
        if part is None:
            continue
        world = parent_world @ local_matrix(part)
        if nif.type_idx[block] == node_type:
            name = nif.strings[part["name"]] if part["name"] < len(nif.strings) else ""
            if name:
                node_worlds[name] = world       # later same-named node wins
            if block == root:
                root_world = world
            for child in reversed(children.get(block, [])):
                stack.append((child, world))
    return {"parents": parents, "childs": childs,
            "node_worlds": node_worlds, "root_world": root_world}


def resolve_placements(infos: list[dict]) -> tuple[list[np.ndarray], list[bool]]:
    """Placements plus the placed/fallback mask, from parsed connect data.

    The mask matters because a mounted part can legitimately land at the
    receiver's origin: an identity matrix alone does not mean "no host". A
    caller that drops unplaceable parts has to trust the resolved chain, not
    the matrix value.
    """
    places = [IDENTITY.copy() for _ in infos]
    if len(infos) < 2:
        return places, [True] * len(infos)

    root = 0
    for i, info in enumerate(infos):
        if any(c.lower() == "c-receiver" for c in info["childs"]):
            root = i
            break

    placed = [False] * len(infos)
    placed[root] = True
    hosts: dict[str, np.ndarray] = {}

    def add_hosts(index: int) -> None:
        info = infos[index]
        for entry in info["parents"]:
            if entry["name"] in hosts:
                continue
            node_world = info["node_worlds"].get(entry["node"], info["root_world"])
            hosts[entry["name"]] = places[index] @ node_world @ entry["m"]

    add_hosts(root)
    progress = True
    while progress:
        progress = False
        for i, info in enumerate(infos):
            if placed[i]:
                continue
            for child in info["childs"]:
                if not child:
                    continue
                host = hosts.get("P" + child[1:])
                if host is None:
                    continue
                places[i] = host
                placed[i] = True
                progress = True
                add_hosts(i)
                break
    return places, placed


def resolve_infos(infos: list[dict]) -> list[np.ndarray]:
    """One placement matrix per NIF, from already-parsed connect data."""
    return resolve_placements(infos)[0]


def resolve(blobs: list[bytes | None]) -> list[np.ndarray]:
    """Placement matrices for an assembly, in the order the NIFs were passed."""
    return resolve_infos([parse_connect(blob) for blob in blobs])
