"""Audit every ammo item's preview against the NIF its *own* model path names.

The mistake this exists to prevent: the 2026-10-18 crate audit compared every
ammo item against `ammo_crates/ammobox_01.nif` instead of the file each item
carries at form+0x100, and concluded the material mapping was permuted. It was
not - the comparison used the wrong file.

So the rule here is structural, not a convention: an item is audited against
the path printed in its own `MODEL PATH = '...'` line, and `audit_item` fails
loudly if the file actually parsed is not that path. There is no flag to point
the audit at a hand-picked NIF.

What it checks, per item, offline:

  * the NIF parsed is the item's own model path (`wrong-nif` otherwise);
  * every material the shop bound appears in that NIF's BSGeometry bindings,
    and every binding is used (`range-not-in-nif`, `nif-shape-unused`);
  * every bound shape has a UV stream in its `.mesh` (`no-uv-stream`);
  * the shop's UV footprint lies inside the NIF shape's UV box
    (`uv-outside`), with a warning when it covers much less (`uv-narrow`);
  * the material order matches the NIF's traversal order (`order`).

The shop side is read from a real `SFShopExplorer.log`; the file side from the
Data archives via `nif_uv.parse_shapes` / `resolve_mesh_uv`, so both sides walk
the same code the CLI does.

    python tools/ba2_audit/ammo_nif_audit.py \
        --data "B:\\...\\Starfield\\Data" \
        --log "C:\\Users\\...\\My Games\\Starfield\\SFSE\\Logs\\SFShopExplorer.log" \
        --write ../../../reports

Exit status is 1 when any check fails, so a stale report cannot go unnoticed.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from nif_uv import (  # noqa: E402
    Arch, nif_path_key, parse_shapes, read_nif, resolve_mesh_uv,
)

FORM_RE = re.compile(r"NativeModel: form ([0-9A-Fa-f]{8}) type=(\d+)")
MODEL_RE = re.compile(r"MODEL PATH = '([^']+)'")
REQUEST_RE = re.compile(
    r"direct: request '(.+?) \[FormID ([0-9A-Fa-f]{8})\]' \((\d+) mesh\(es\), (\d+) tris\)")
TEXTURE_RE = re.compile(
    r"texturing 'direct:(.+?) \[FormID ([0-9A-Fa-f]{8})\]#[0-9a-fA-F]+'"
    r" \((\d+) ranges(?:, (\d+) prefetched)?\)")
RANGE_RE = re.compile(
    r"direct GPU: range (\d+) '([^']+)' <- slot (\d+).*?tint=\(([^)]*)\)"
    r" mask=(\w+) uv=\[(-?[\d.]+),(-?[\d.]+)\]x\[(-?[\d.]+),(-?[\d.]+)\]")

# Containment tolerance for the shop's UV footprint against the .mesh box. The
# plugin rounds half-float boxes when it logs them, so a few thousandths leak.
UV_TOL = 0.01


@dataclass
class Range:
    index: int
    material: str
    tint: str
    mask: str
    uv: tuple[float, float, float, float] | None


@dataclass
class Render:
    form: str
    label: str
    model_path: str = ""
    form_type: int = 0        # 49 = AMMO in this dump; 48 = the player actor
    meshes: int = 0
    tris: int = 0
    ranges: list[Range] = field(default_factory=list)


@dataclass
class ShapeBinding:
    """One BSGeometry: the node name, the material it binds, its UV0 box."""
    node: str
    material: str
    uv: tuple[float, float, float, float] | None
    mesh_key: str | None = None


def parse_log(text: str) -> dict[str, Render]:
    """Latest preview per FormID: own model path, mesh count, shop ranges.

    MODEL PATH is logged during the NativeModel scan, before the request is
    queued, so it is collected per form and attached when the render appears.
    """
    paths: dict[str, str] = {}
    types: dict[str, int] = {}
    renders: dict[str, Render] = {}
    current: Render | None = None
    cur_form = ""
    for line in text.splitlines():
        form = FORM_RE.search(line)
        if form:
            cur_form = form.group(1).upper()
            types[cur_form] = int(form.group(2))
            current = None
        path = MODEL_RE.search(line)
        if path and cur_form:
            # The MODEL PATH line carries no FormID of its own; it belongs to
            # the most recent `NativeModel: form X` line in the same request.
            paths[cur_form] = path.group(1)
        req = REQUEST_RE.search(line)
        if req:
            key = req.group(2).upper()
            current = Render(key, req.group(1), meshes=int(req.group(3)),
                             tris=int(req.group(4)))
            current.model_path = paths.get(key, "")
            current.form_type = types.get(key, 0)
            renders[key] = current
            continue
        tex = TEXTURE_RE.search(line)
        if tex:
            key = tex.group(2).upper()
            current = renders.get(key)
            if current is None:
                current = Render(key, tex.group(1), meshes=int(tex.group(3)))
                current.model_path = paths.get(key, "")
                current.form_type = types.get(key, 0)
                renders[key] = current
            continue
        rng = RANGE_RE.search(line)
        if rng and current is not None:
            current.ranges.append(Range(
                index=int(rng.group(1)),
                material=rng.group(2),
                tint=rng.group(3),
                mask=rng.group(5),
                uv=(float(rng.group(6)), float(rng.group(7)),
                    float(rng.group(8)), float(rng.group(9))),
            ))
    return renders


def select_items(renders: dict[str, Render], forms: set[str] | None = None,
                 any_type: bool = False) -> list[Render]:
    """Ammo previews that carry both a model path and shop ranges.

    Type 49 is AMMO in this dump and 48 is the player actor; an unknown type is
    kept rather than dropped, so a renumbered build cannot empty the audit.
    """
    out = []
    for form, render in sorted(renders.items()):
        if forms is not None and form not in forms:
            continue
        if not render.ranges or not render.model_path:
            continue
        if not any_type and render.form_type not in (0, 49):
            continue
        out.append(render)
    return out


def load_shapes(arch: Arch, model_path: str):
    """Parse the NIF named by `model_path` and resolve each shape's UV0 box."""
    key, blob, nif, err = read_nif(arch, model_path)
    if nif is None:
        return key, None, err
    shapes = []
    for shape in parse_shapes(blob, nif):
        uv, mesh_key = None, None
        if shape["lods"]:
            uv, mesh_key = resolve_mesh_uv(arch, shape["lods"][0][2])
        shapes.append(ShapeBinding(shape["name"], shape["material"], uv, mesh_key))
    return key, shapes, None


def _norm_material(path: str) -> str:
    return path.strip().strip("'\"").lower().replace("/", "\\")


def _covers(inner, outer) -> bool:
    """Is `inner` inside `outer` within UV_TOL?"""
    return (inner[0] >= outer[0] - UV_TOL and inner[1] <= outer[1] + UV_TOL
            and inner[2] >= outer[2] - UV_TOL and inner[3] <= outer[3] + UV_TOL)


def _span(rect) -> float:
    return max(rect[1] - rect[0], 1e-6) * max(rect[3] - rect[2], 1e-6)


def audit_item(render: Render, audited_path: str, shapes: list[ShapeBinding] | None,
               error: str | None = None) -> list[tuple[str, str, str]]:
    """Checks for one item. Returns (level, code, detail); level is fail/warn."""
    out: list[tuple[str, str, str]] = []

    # Guard first: the file audited must be the item's own model path.
    if not render.model_path:
        out.append(("fail", "no-model-path",
                    "%s has no MODEL PATH line; nothing to audit" % render.form))
        return out
    if nif_path_key(audited_path) != nif_path_key(render.model_path):
        out.append(("fail", "wrong-nif",
                    "audited %s but the item names %s" % (audited_path, render.model_path)))
        return out

    if shapes is None:
        out.append(("fail", "nif-unreadable", error or "no geometry parsed"))
        return out
    if not shapes:
        out.append(("fail", "no-geometry", "NIF has no BSGeometry blocks"))
        return out

    by_material: dict[str, ShapeBinding] = {}
    for shape in shapes:
        by_material.setdefault(_norm_material(shape.material), shape)
    used: set[str] = set()
    matched_order: list[str] = []

    for rng in render.ranges:
        key = _norm_material(rng.material)
        shape = by_material.get(key)
        if shape is None:
            out.append(("fail", "range-not-in-nif",
                        "range %d '%s' is not bound by %s"
                        % (rng.index, rng.material, render.model_path)))
            continue
        used.add(key)
        matched_order.append(key)
        node = shape.node or "(unnamed)"
        if shape.uv is None:
            out.append(("fail", "no-uv-stream",
                        "range %d %s: .mesh '%s' has no UV stream"
                        % (rng.index, node, shape.mesh_key or "?")))
            continue
        if rng.uv is not None:
            if not _covers(rng.uv, shape.uv):
                out.append(("fail", "uv-outside",
                            "range %d %s: shop uv=[%.3f,%.3f]x[%.3f,%.3f] "
                            "exceeds NIF box u=[%.3f,%.3f] v=[%.3f,%.3f]"
                            % (rng.index, node, rng.uv[0], rng.uv[1], rng.uv[2], rng.uv[3],
                               shape.uv[0], shape.uv[1], shape.uv[2], shape.uv[3])))
            elif _span(rng.uv) < 0.5 * _span(shape.uv):
                out.append(("warn", "uv-narrow",
                            "range %d %s covers %.0f%% of the NIF UV box"
                            % (rng.index, node, 100.0 * _span(rng.uv) / _span(shape.uv))))

    for key, shape in by_material.items():
        if key not in used:
            out.append(("fail", "nif-shape-unused",
                        "%s binds '%s', which no shop range uses"
                        % (shape.node or "(unnamed)", shape.material)))

    if render.ranges and matched_order:
        nif_order = [_norm_material(s.material) for s in shapes
                     if _norm_material(s.material) in by_material]
        shop_order = matched_order
        seen, dedup = set(), []
        for key in nif_order:
            if key not in seen:
                seen.add(key)
                dedup.append(key)
        if dedup != shop_order:
            out.append(("warn", "order",
                        "shop ranges follow %s, the NIF traverses %s"
                        % (_short(shop_order), _short(dedup))))

    if render.meshes and len(render.ranges) != render.meshes:
        out.append(("warn", "range-count",
                    "%d shop range(s) for %d mesh(es)" % (len(render.ranges), render.meshes)))
    return out


def _short(materials: list[str]) -> str:
    return "[" + ", ".join(m.rsplit("\\", 1)[-1].replace(".mat", "") for m in materials) + "]"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--data", required=True, help="Starfield Data directory (BA2s)")
    ap.add_argument("--log", required=True, help="SFShopExplorer.log with previews")
    ap.add_argument("--form", action="append", default=None,
                    help="limit to these FormIDs (repeatable); default: every ammo item")
    ap.add_argument("--any-type", action="store_true",
                    help="audit non-ammo form types too (default: type 49 only)")
    ap.add_argument("--write", default=None,
                    help="directory to write ammo-nif-binding-audit.{md,tsv}")
    args = ap.parse_args(argv)

    log = Path(args.log)
    if not log.is_file():
        print("[FAIL] log not found: %s" % log)
        return 2
    renders = parse_log(log.read_text(encoding="utf-8", errors="replace"))
    wanted = {f.upper() for f in args.form} if args.form else None
    items = select_items(renders, wanted, args.any_type)
    if not items:
        print("[FAIL] no ammo previews with a model path and ranges in %s" % log)
        return 2

    arch = Arch(Path(args.data))
    results = []
    for render in items:
        key, shapes, err = load_shapes(arch, render.model_path)
        checks = audit_item(render, render.model_path, shapes, err)
        results.append((render, key, shapes or [], checks))

    fails = sum(1 for _, _, _, c in results for level, _, _ in c if level == "fail")
    warns = sum(1 for _, _, _, c in results for level, _, _ in c if level == "warn")

    for render, key, shapes, checks in results:
        status = "FAIL" if any(l == "fail" for l, _, _ in checks) else "ok"
        print("\n%s  %s  %s  (%s)" % (status, render.form, render.label, render.model_path))
        print("  nif %s  %d shape(s)  %d range(s)"
              % (key, len(shapes), len(render.ranges)))
        for level, code, detail in checks:
            print("  %-4s %-16s %s" % (level.upper(), code, detail))

    print("\n%d item(s): %d failure(s), %d warning(s)" % (len(results), fails, warns))

    if args.write:
        out = Path(args.write)
        out.mkdir(parents=True, exist_ok=True)
        _write_markdown(out / "ammo-nif-binding-audit.md", results, fails, warns)
        _write_tsv(out / "ammo-nif-binding-audit.tsv", results)
        print("wrote %s and ammo-nif-binding-audit.tsv" % (out / "ammo-nif-binding-audit.md"))
    return 1 if fails else 0


def _write_tsv(path: Path, results) -> None:
    lines = ["formID\tlabel\tmodelPath\tshape\tmaterial\tnifUV\tshopUV\tverdict\tdetail"]
    for render, key, shapes, checks in results:
        shop = {_norm_material(r.material): r for r in render.ranges}
        by_material = {_norm_material(s.material): s for s in shapes}
        if not shapes:
            lines.append("\t".join([render.form, render.label, render.model_path,
                                    "", "", "", "", "fail", "nif unreadable"]))
        for shape in shapes:
            key_m = _norm_material(shape.material)
            rng = shop.get(key_m)
            nif_uv = "" if shape.uv is None else "%.3f,%.3f,%.3f,%.3f" % shape.uv
            shop_uv = "" if rng is None or rng.uv is None else \
                "%.3f,%.3f,%.3f,%.3f" % rng.uv
            own = [c for c in checks if shape.node and shape.node in c[2]]
            verdict = "ok"
            if any(c[0] == "fail" for c in own):
                verdict = "fail"
            elif any(c[0] == "warn" for c in own):
                verdict = "warn"
            elif rng is None:
                verdict = "fail"
            detail = " | ".join(c[1] + ": " + c[2] for c in own)
            lines.append("\t".join([render.form, render.label, render.model_path,
                                    shape.node, shape.material, nif_uv, shop_uv,
                                    verdict, detail]))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _write_markdown(path: Path, results, fails: int, warns: int) -> None:
    out = ["# Ammo NIF binding audit (automated)", "",
           "Generated by `tools/ba2_audit/ammo_nif_audit.py` from a real",
           "`SFShopExplorer.log` plus the Data archives. Each item is audited",
           "against the NIF its own `MODEL PATH` names; `audit_item` fails with",
           "`wrong-nif` if any other file is parsed, so the wrong-NIF mistake",
           "cannot recur silently.", "",
           "**%d item(s): %d failure(s), %d warning(s)**" % (len(results), fails, warns), ""]
    for render, key, shapes, checks in results:
        out.append("## `%s` - FormID %s" % (render.label, render.form))
        out.append("")
        out.append("`%s` - %d shape(s) in the file, %d shop range(s)."
                   % (render.model_path, len(shapes), len(render.ranges)))
        out.append("")
        if shapes:
            by_material = {_norm_material(s.material): s for s in shapes}
            out.append("| range | material | NIF UV set 0 | shop UV | verdict |")
            out.append("|---|---|---|---|---|")
            for rng in render.ranges:
                shape = by_material.get(_norm_material(rng.material))
                nif_uv = "(none)" if shape is None or shape.uv is None else \
                    "u=[%.3f,%.3f] v=[%.3f,%.3f]" % shape.uv
                shop_uv = "(none)" if rng.uv is None else \
                    "[%.3f,%.3f]x[%.3f,%.3f]" % rng.uv
                own = [c for c in checks if shape and shape.node and shape.node in c[2]]
                if any(c[0] == "fail" for c in own):
                    verdict = "FAIL " + own[0][1]
                elif any(c[0] == "warn" for c in own):
                    verdict = "warn " + own[0][1]
                elif shape is None:
                    verdict = "FAIL range-not-in-nif"
                else:
                    verdict = "match"
                out.append("| %d | %s | %s | %s | %s |"
                           % (rng.index, rng.material.rsplit("\\", 1)[-1].replace(".mat", ""),
                              nif_uv, shop_uv, verdict))
            out.append("")
        extra = [c for c in checks if c[1] not in
                 ("range-not-in-nif", "no-uv-stream", "uv-outside", "uv-narrow")]
        for level, code, detail in extra:
            out.append("* **%s** `%s` %s" % (level.upper(), code, detail))
        out.append("")
    path.write_text("\n".join(out), encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
