"""Per-pixel orthographic render of the wine shell with the normalized UVs.

contact_sheet.render_model shades one mean colour per triangle, which cannot
show a texture's density. This probe rasterizes the shell's own triangles with
barycentric UV interpolation instead, so the web can be compared to the game
capture directly.
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))

import contact_sheet as sheet  # noqa: E402

DATA = Path(sheet.audit.GAME_DATA)
store = sheet.audit.AssetStore([DATA], Path(sheet.audit.MODS_ROOT), False)
nif_blob = store.read("meshes/setdressing/generic/genericspacewine/genericspacewine.nif")
geoms = sheet.walk_geometries(nif_blob, sheet.Nif(nif_blob))
shell = [g for g in geoms if "glass" in g["material"].lower()][0]
mesh = sheet.parse_mesh(store.read(sheet.mesh_key(shell["lods"][0][2])))
mask = sheet.Textures(store).get(
    "textures/common/detail/manmade/detailabstractpattern06_mask.dds")[..., 0]

pos = mesh["pos"].astype(np.float64)
idx = mesh["idx"].reshape(-1, 3)
uv = mesh["uv"].astype(np.float64)
span = uv.max(axis=0) - uv.min(axis=0)
uv = (uv - uv.min(axis=0)) / span

extent = pos.max(axis=0) - pos.min(axis=0)
depth_axis = int(np.argmin(extent))
up_axis = 1 if depth_axis != 1 else 2
side_axis = 3 - depth_axis - up_axis
print("extent", extent, "depth", depth_axis, "up", up_axis, "side", side_axis)

# camera basis
zdir = np.zeros(3)
zdir[depth_axis] = 1.0
xdir = np.zeros(3)
xdir[side_axis] = 1.0
ydir = np.zeros(3)
ydir[up_axis] = 1.0
cam = pos @ np.stack([xdir, ydir, zdir], axis=1)  # x, y up, depth
mx, my = cam[:, 0], cam[:, 1]
scale = 560.0 / max(mx.max() - mx.min(), my.max() - my.min())
W = int((mx.max() - mx.min()) * scale) + 40
H = int((my.max() - my.min()) * scale) + 40
sx = (mx - mx.min()) * scale + 20
sy = (my.max() - my) * scale + 20

faces = cam[idx]
light = np.array([0.35, 0.6, -0.72])
light /= np.linalg.norm(light)
normal = np.cross(faces[:, 1] - faces[:, 0], faces[:, 2] - faces[:, 0])
nlen = np.linalg.norm(normal, axis=1) + 1e-12
normal /= nlen[:, None]
shade = np.abs(normal @ -light)
shade = 0.35 + 0.65 * shade

img = np.zeros((H, W, 3), dtype=np.float64)
zbuf = np.full((H, W), -1e9)
for t in range(len(faces)):
    a, b, c = faces[t]
    ax, ay, bx, by, cx, cy = sx[idx[t, 0]], sy[idx[t, 0]], sx[idx[t, 1]], sy[idx[t, 1]], sx[idx[t, 2]], sy[idx[t, 2]]
    x0, x1 = int(max(0, min(ax, bx, cx))), int(min(W - 1, max(ax, bx, cx)))
    y0, y1 = int(max(0, min(ay, by, cy))), int(min(H - 1, max(ay, by, cy)))
    if x1 < x0 or y1 < y0:
        continue
    xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
    d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(d) < 1e-9:
        continue
    w0 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / d
    w1 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / d
    w2 = 1.0 - w0 - w1
    inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
    if not inside.any():
        continue
    depth = w0 * a[2] + w1 * b[2] + w2 * c[2]
    sub = zbuf[y0:y1 + 1, x0:x1 + 1]
    hit = inside & (depth > sub)
    if not hit.any():
        continue
    tu = w0 * uv[idx[t, 0], 0] + w1 * uv[idx[t, 1], 0] + w2 * uv[idx[t, 2], 0]
    tv = w0 * uv[idx[t, 0], 1] + w1 * uv[idx[t, 1], 1] + w2 * uv[idx[t, 2], 1]
    px = np.clip((tu * (mask.shape[1] - 1)).astype(np.int64), 0, mask.shape[1] - 1)
    py = np.clip((tv * (mask.shape[0] - 1)).astype(np.int64), 0, mask.shape[0] - 1)
    ink = mask[py, px]
    sub[hit] = depth[hit]
    img[y0:y1 + 1, x0:x1 + 1][hit] = (ink[hit] * shade[t])[:, None] * np.array([0.95, 0.96, 1.0])

out = ROOT / "tools" / "renderer_recon" / "extracted" / "wine_normalized_probe.png"
Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(out)
print("wrote", out, img.shape)
