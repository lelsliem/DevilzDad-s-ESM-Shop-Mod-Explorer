"""Tests for tools/ba2_audit/ammo_nif_audit.py.

The regression this protects: the 2026-10-18 audit compared every ammo item
against one crate NIF instead of each item's own model path and reported a
"permutation" that did not exist. The parser and the comparator are pinned here
on synthetic data, and the last test re-runs the audit over the real log, where
a wrong-file comparison shows up immediately.

    python tools/test_ammo_nif_audit.py
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "ba2_audit"))

import dataclasses  # noqa: E402

from ammo_nif_audit import (  # noqa: E402
    Render, Range, ShapeBinding, audit_item, parse_log, select_items,
)

LOG = Path(r"C:/Users/mrlma/Documents/My Games/Starfield/SFSE/Logs/SFShopExplorer.log")
DATA = Path(r"B:/SteamLibrary/steamapps/common/Starfield/Data")

LOG_SAMPLE = """
[19:39:44.712] [23368] [I] SFShopExplorer NativeModel: form 0000E8EC type=49 vtable=0x7ff6
[19:39:44.712] [23368] [I]   FOUND MODEL PATH at form+0x100: 'SetDressing\\ammo_crates\\AmmoLaser.nif'
[19:39:44.712] [23368] [I] SFShopExplorer NativeModel: MODEL PATH = 'SetDressing\\ammo_crates\\AmmoLaser.nif'
[19:39:44.861] [23368] [I] SFShopExplorer direct: request '3kV LZR Cartridge [FormID 0000E8EC]' (3 mesh(es), 4568 tris)
[19:39:44.962] [23368] [I] SFShopExplorer offline GPU: texturing 'direct:3kV LZR Cartridge [FormID 0000E8EC]#3cf9ec5b' (3 ranges, 3 prefetched)
[19:39:44.964] [23368] [I] SFShopExplorer direct GPU: range 0 'Materials\\SetDressing\\AmmoBox01\\PaintedMetalShine01_Green.mat' <- slot 1 fmt=71 1024x1024 pitch=2048 tint=(1.00,1.00,1.00) mask=none uv=[-0.126,1.083]x[-0.103,0.965]
[19:39:44.965] [23368] [I] SFShopExplorer direct GPU: range 1 'Materials\\SetDressing\\AmmoBox01\\AmmoBoxLarge_DarkGreen01.mat' <- slot 2 fmt=71 1024x1024 pitch=2048 tint=(1.00,1.00,1.00) mask=none uv=[-0.068,1.122]x[-0.093,1.101]
[19:39:44.966] [23368] [I] SFShopExplorer direct GPU: range 2 'Materials\\SetDressing\\AmmoBox01\\AmmoBox01_Revised.mat' <- slot 3 fmt=71 1024x1024 pitch=2048 tint=(1.00,1.00,1.00) mask=yes uv=[-0.054,0.933]x[0.022,0.907]
[19:39:47.100] [23368] [I] SFShopExplorer NativeModel: form 0023155F type=49 vtable=0x7ff6
[19:39:47.100] [23368] [I] SFShopExplorer NativeModel: MODEL PATH = 'SetDressing\\ammo_crates\\Ammo27Caliber.nif'
[19:39:47.200] [23368] [I] SFShopExplorer direct: request '.27 Caliber [FormID 0023155F]' (3 mesh(es), 7216 tris)
[19:39:47.300] [23368] [I] SFShopExplorer offline GPU: texturing 'direct:.27 Caliber [FormID 0023155F]#aabb' (3 ranges, 3 prefetched)
[19:39:47.301] [23368] [I] SFShopExplorer direct GPU: range 0 'Materials\\SetDressing\\AmmoBox01\\AmmoBoxLarge_DarkGreen01.mat' <- slot 1 fmt=71 1024x1024 pitch=2048 tint=(1.00,1.00,1.00) mask=none uv=[-0.232,1.222]x[-0.193,1.198]
[19:39:47.302] [23368] [I] SFShopExplorer direct GPU: range 1 'Materials\\SetDressing\\AmmoBox01\\AmmoBox01_Revised.mat' <- slot 2 fmt=71 1024x1024 pitch=2048 tint=(1.00,1.00,1.00) mask=none uv=[0.066,0.933]x[0.032,0.676]
[19:39:47.303] [23368] [I] SFShopExplorer direct GPU: range 2 'Materials\\SetDressing\\AmmoBox01\\PaintedMetalshine01_Orange.mat' <- slot 3 fmt=71 1024x1024 pitch=2048 tint=(0.93,0.84,0.66) mask=none uv=[-0.241,1.221]x[-0.227,1.023]
"""


def _shapes_for_laser() -> list[ShapeBinding]:
    return [
        ShapeBinding("AmmoLaser:3", "Materials\\SetDressing\\AmmoBox01\\PaintedMetalShine01_Green.mat",
                     (-0.129, 1.118, -0.129, 0.975), "geometries/a.mesh"),
        ShapeBinding("AmmoLaser:2", "Materials\\SetDressing\\AmmoBox01\\AmmoBoxLarge_DarkGreen01.mat",
                     (-0.122, 1.123, -0.102, 1.129), "geometries/b.mesh"),
        ShapeBinding("AmmoLaser:1", "Materials\\SetDressing\\AmmoBox01\\AmmoBox01_Revised.mat",
                     (-0.057, 1.000, -0.000, 0.999), "geometries/c.mesh"),
    ]


def test_parse_log_keeps_each_items_own_path():
    renders = parse_log(LOG_SAMPLE)
    assert renders["0000E8EC"].model_path.endswith("AmmoLaser.nif"), renders["0000E8EC"]
    assert renders["0023155F"].model_path.endswith("Ammo27Caliber.nif"), renders["0023155F"]
    # Two items, two different files, even though they share most materials.
    assert renders["0000E8EC"].model_path != renders["0023155F"].model_path
    assert [r.material.rsplit("\\", 1)[-1] for r in renders["0023155F"].ranges] == [
        "AmmoBoxLarge_DarkGreen01.mat", "AmmoBox01_Revised.mat",
        "PaintedMetalshine01_Orange.mat"], renders["0023155F"].ranges
    print("ok  parse_log keeps each item's own model path")


def test_only_ammo_forms_are_audited():
    """The player actor (type 48) reaches the same renderer; it is not ammo."""
    renders = parse_log(LOG_SAMPLE)
    ammo = renders["0000E8EC"]
    actor = dataclasses.replace(ammo, form="000001F4", label="UnarmedHuman", form_type=48)
    picked = [r.form for r in select_items({**renders, actor.form: actor})]
    assert "0000E8EC" in picked and actor.form not in picked, picked
    every = [r.form for r in select_items({**renders, actor.form: actor}, any_type=True)]
    assert actor.form in every, every
    print("ok  only ammo form types are selected by default")


def test_wrong_nif_is_a_failure_not_a_footnote():
    """The exact mistake: audit a sibling's NIF and still call it a result."""
    render = parse_log(LOG_SAMPLE)["0000E8EC"]
    checks = audit_item(render, "meshes/setdressing/ammo_crates/ammobox_01.nif", _shapes_for_laser())
    assert any(level == "fail" and code == "wrong-nif" for level, code, _ in checks), checks
    assert not any(code == "nif-shape-unused" for _, code, _ in checks), "must stop at the guard"
    print("ok  wrong-nif fails before any binding is compared")


def test_matching_bindings_pass():
    render = parse_log(LOG_SAMPLE)["0000E8EC"]
    checks = audit_item(render, render.model_path, _shapes_for_laser())
    fails = [c for c in checks if c[0] == "fail"]
    assert not fails, fails
    print("ok  own-path bindings pass")


def test_material_missing_from_nif():
    render = parse_log(LOG_SAMPLE)["0000E8EC"]
    shapes = _shapes_for_laser()[:2]
    checks = audit_item(render, render.model_path, shapes)
    assert any(c[1] == "range-not-in-nif" for c in checks), checks
    print("ok  a shop range the NIF does not bind fails")


def test_unused_nif_shape():
    render = parse_log(LOG_SAMPLE)["0000E8EC"]
    shapes = _shapes_for_laser() + [
        ShapeBinding("AmmoLaser:0", "Materials\\SetDressing\\AmmoBox01\\Unused.mat",
                     (-0.1, 1.0, -0.1, 1.0), "geometries/d.mesh")]
    checks = audit_item(render, render.model_path, shapes)
    assert any(c[1] == "nif-shape-unused" and "AmmoLaser:0" in c[2] for c in checks), checks
    print("ok  a NIF binding no range uses fails")


def test_missing_uv_stream():
    render = parse_log(LOG_SAMPLE)["0000E8EC"]
    shapes = _shapes_for_laser()
    shapes[1] = ShapeBinding(shapes[1].node, shapes[1].material, None, "geometries/b.mesh")
    checks = audit_item(render, render.model_path, shapes)
    assert any(c[1] == "no-uv-stream" and "AmmoLaser:2" in c[2] for c in checks), checks
    print("ok  a UV-less shape (the 3kV clone bug) fails")


def test_uv_outside_the_nif_box():
    render = parse_log(LOG_SAMPLE)["0000E8EC"]
    shapes = _shapes_for_laser()
    # Shop sampled a wider region than the file allows: wrong atlas placement.
    shapes[0] = ShapeBinding(shapes[0].node, shapes[0].material,
                             (-0.05, 0.60, -0.05, 0.60), "geometries/a.mesh")
    checks = audit_item(render, render.model_path, shapes)
    assert any(c[1] == "uv-outside" and "AmmoLaser:3" in c[2] for c in checks), checks
    print("ok  shop UV outside the NIF box fails")


def test_order_mismatch_warns():
    render = parse_log(LOG_SAMPLE)["0000E8EC"]
    shapes = [_shapes_for_laser()[1], _shapes_for_laser()[0], _shapes_for_laser()[2]]
    checks = audit_item(render, render.model_path, shapes)
    assert any(c[0] == "warn" and c[1] == "order" for c in checks), checks
    print("ok  a reordered NIF traversal warns")


def test_real_log_has_no_binding_failures():
    if not (LOG.is_file() and DATA.is_dir()):
        print("skip real-log audit (log or Data not present)")
        return
    from ammo_nif_audit import load_shapes, select_items
    from nif_uv import Arch
    # Ammo only, through the same selector the tool uses. Taking every render
    # with a model path and ranges pulled in the session's weapons (type 48:
    # Razorback, Cutter, Rev8 Laser) and failed them for reasons that say
    # nothing about crates - a weapon's NIF has no BSGeometry through this
    # path, and a turret's shared decal materials are bound by no shop range.
    # Those shells are audited by their own suites, not this one.
    renders = select_items(parse_log(LOG.read_text(encoding="utf-8", errors="replace")))
    # The game rewrites this log every launch, so the count is whatever the last
    # session previewed. The report run over a full catalogue lives in
    # reports/ammo-nif-binding-audit.md; this only needs a real sample.
    # A log from a session that never opened an ammo item (or died before any
    # preview) has nothing to audit. That is the environment, not the code, so
    # it skips like a missing log rather than failing the suite.
    if not renders:
        print("skip real-log audit (%s has no ammo preview with a model path and "
              "ranges)" % LOG.name)
        return
    arch = Arch(DATA)
    failures = []
    for render in renders:
        key, shapes, err = load_shapes(arch, render.model_path)
        checks = audit_item(render, render.model_path, shapes, err)
        failures += [(render.form, render.label, c) for c in checks if c[0] == "fail"]
    assert not failures, failures[:5]
    print("ok  %d real ammo items, 0 binding failures" % len(renders))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fast":
        test_parse_log_keeps_each_items_own_path()
        test_only_ammo_forms_are_audited()
        test_wrong_nif_is_a_failure_not_a_footnote()
        test_matching_bindings_pass()
        test_material_missing_from_nif()
        test_unused_nif_shape()
        test_missing_uv_stream()
        test_uv_outside_the_nif_box()
        test_order_mismatch_warns()
    else:
        test_parse_log_keeps_each_items_own_path()
        test_only_ammo_forms_are_audited()
        test_wrong_nif_is_a_failure_not_a_footnote()
        test_matching_bindings_pass()
        test_material_missing_from_nif()
        test_unused_nif_shape()
        test_missing_uv_stream()
        test_uv_outside_the_nif_box()
        test_order_mismatch_warns()
        test_real_log_has_no_binding_failures()
    print("all ammo NIF audit tests passed")
