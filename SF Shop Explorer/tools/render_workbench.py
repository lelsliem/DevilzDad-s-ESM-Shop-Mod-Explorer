#!/usr/bin/env python3
"""Live render workbench: draw catalog items offline, watch the rules, redraw.

`contact_sheet.py` answers "what does a shelf look like" as a batch: it renders,
writes an index and exits. Iterating on a renderer rule with it means running the
whole selection again and refreshing the page by hand, which is the same
edit-build-look loop as a game launch, only smaller.

This tool keeps the expensive state alive and shows the result in a browser:

  * the archive index (512 BA2s, ~1.4M named entries) is built once instead of
    per run - it is the first asset read, not the startup, that pays it;
  * after that an item costs a few hundred milliseconds, so a selection of
    twenty redraws while you watch;
  * editing `contact_sheet.py`, `nif_uv.py` or `aid_colour_audit.py` reloads it,
    redraws the same selection and pushes the new tiles to the page: no restart,
    no game launch;
  * every pass keeps the previous image of each item and reports the pixel
    difference, so "did my change do what I meant" is one number per tile and the
    before/after pair sits side by side.

The page is at http://127.0.0.1:<port>/ and polls the server; tiles carry the
pass version, so only the ones that changed are re-fetched.

    python tools/render_workbench.py --item Razorback --item "Coffee Mug"
    python tools/render_workbench.py --type AMMO --limit 40
    python tools/render_workbench.py --form 00000FD6 --size 320
    python tools/render_workbench.py --type MISC --limit 300 --once   # batch

Searching is the preview loop: open the page and type a name, FormID or plugin
into the find box. Clicking a result focuses that one record - it renders at
`--detail-size` (512 by default) with its model, geometry counts and a
per-material report - and `?form=00000FD6` on the URL opens a record directly.
`/` focuses the search box. Nothing is rebuilt to do this: the search runs over
the catalog already in memory and the render uses the same hot-reloading rules.

`--once` renders, writes `reports/workbench/workbench.html` (a self-contained
snapshot: the state is embedded and the tile paths are relative), `workbench.tsv`
and exits - the regression shape: render, edit a rule, render again, diff the
TSVs.

Honest limits, inherited from contact_sheet: flat-shaded triangles, painter
sorted (no z-buffer), BC1/BC3/BC4/BC5 colour maps only, and the record's own
MODL rather than LiveModel's sibling/OMOD assembly. It is a fast look at
material, tint, silhouette and which path an item resolves to - not a
replacement for the in-game preview when judging a hero shot.
"""
from __future__ import annotations

import argparse
import importlib
import io
import json
import sys
import threading
import time
import traceback
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
RECON = HERE / "renderer_recon"
sys.path[:0] = [str(RECON), str(HERE), str(HERE / "ba2_audit")]

import aid_colour_audit as audit  # noqa: E402
import contact_sheet as sheet  # noqa: E402

# The drawing rules. A change to any of these is what the watcher reacts to;
# they are reloaded in dependency order (nif_uv is imported by contact_sheet).
WATCHED = ("nif_uv.py", "contact_sheet.py", "aid_colour_audit.py")
MODULES = {"nif_uv.py": "nif_uv", "contact_sheet.py": "contact_sheet",
           "aid_colour_audit.py": "aid_colour_audit"}
# aid_colour_audit holds the classes the long-lived runtime is made of, so a
# change to it cannot be picked up by a reload alone: the store, plugin index and
# material database are rebuilt, and the archive index is paid again.
RUNTIME_OWNER = "aid_colour_audit.py"
DEFAULT_OUT = HERE.parents[2] / "reports" / "workbench"


# ── runtime ──────────────────────────────────────────────────────────────────

class Runtime:
    """Everything expensive that survives a redraw."""

    def __init__(self, args) -> None:
        self.args = args
        self.catalog = audit.find_catalog(args.catalog)
        self.items: list[dict] = []
        self.store = None
        self.plugins = None
        self.swaps = None
        self.cdb = None
        self.textures = None
        self.meshes: dict = {}
        self.log: list[str] = []

    def note(self, text: str) -> None:
        self.log.append(f"{time.strftime('%H:%M:%S')} {text}")
        del self.log[:-40]
        print(text, flush=True)

    def build(self) -> None:
        """Load the catalog and the files it points at. The index stays lazy."""
        if not self.catalog:
            raise SystemExit("no catalog CSV found; pass --catalog")
        started = time.perf_counter()
        self.items = audit.parse_catalog(self.catalog)
        self.store = audit.AssetStore([Path(self.args.data)], Path(self.args.mods_root), False)
        self.plugins = audit.Plugins(self.store)
        self.swaps = audit.Swaps(Path(self.args.swaps))
        self.cdb = audit.Cdb(Path(self.args.cdb))
        self.textures = sheet.Textures(self.store)
        self.meshes = {}
        self.note(f"catalog: {len(self.items):,} records in {time.perf_counter() - started:.2f}s")
        self.note("the first redraw also indexes 512 archives (~25s); later ones do not")

    def reload_rules(self, changed: str) -> None:
        """Re-import the renderer sources, rebuilding the runtime if it owns them."""
        for name in WATCHED:
            module = MODULES[name]
            if module in sys.modules:
                importlib.reload(sys.modules[module])
        self.meshes = {}
        if changed == RUNTIME_OWNER:
            self.note(f"{changed} changed: rebuilding the runtime (archive index again)")
            self.build()
        else:
            # Decoded textures came out of the old decoders.
            self.textures = sheet.Textures(self.store)
            self.note(f"{changed} changed: rules reloaded, texture cache cleared")


# ── selection ────────────────────────────────────────────────────────────────

def pick_from_args(args) -> dict:
    return {"item": list(args.item), "form": list(args.form),
            "type": list(args.type), "search": args.search, "limit": args.limit,
            "size": args.size, "focus": bool(args.form) and not args.item and not args.type}


def search_catalog(items: list[dict], needle: str, limit: int = 24) -> list[dict]:
    """Rank catalog rows for the page's find box.

    A FormID query is a suffix match so a load-order prefix (FE0xxxxx) still
    works; a text query prefers a name prefix, then any name match, then the
    plugin and type. Ranking is a single stable pass, so two records that match
    equally keep the order the catalog dumped them in.
    """
    query = (needle or "").strip().casefold()
    if not query:
        return []
    digits = query[2:] if query.startswith("0x") else query
    form_query = len(digits) >= 3 and all(c in "0123456789abcdef" for c in digits)
    ranked: list[tuple[int, int, dict]] = []
    for index, row in enumerate(items):
        name = row["name"].casefold()
        fid = f"{row['form']:08X}".casefold()
        plugin = row["plugin"].casefold()
        kind = row["type"].casefold()
        if form_query and fid.endswith(digits):
            rank = 0
        elif name.startswith(query):
            rank = 1
        else:
            # A word start ("mug" in "Coffee Mug") is a better answer than
            # the same letters inside a word ("mug" in "Smuggler").
            at = name.find(query)
            if at > 0 and not name[at - 1].isalnum():
                rank = 2
            elif at > 0:
                rank = 3
            elif query in plugin or query == kind:
                rank = 4
            else:
                continue
        ranked.append((rank, index, row))
    ranked.sort(key=lambda hit: (hit[0], hit[1]))
    return [{"form": f"{row['form']:08X}", "name": row["name"],
             "type": row["type"], "plugin": row["plugin"]}
            for _, _, row in ranked[:max(0, limit)]]


def select(runtime: Runtime, pick: dict) -> list[dict]:
    """Pick the records to draw. Order is the operator's, duplicates removed."""
    wanted: list[dict] = []
    if pick["form"]:
        by_form = {f"{r['form']:08X}": r for r in runtime.items}
        for raw in pick["form"]:
            key = raw.upper().removeprefix("0X").rjust(8, "0")
            wanted.append(by_form.get(key, {"name": f"(form {key})", "type": "?",
                                            "form": int(key, 16), "plugin": "Starfield.esm",
                                            "keywords": ""}))
    types = {t.upper() for t in pick["type"] if t}
    needle = (pick["search"] or "").casefold()

    def matches(row: dict) -> bool:
        return (not needle) or needle in row["name"].casefold() or \
            needle in f"{row['form']:08X}".casefold()

    if types:
        wanted += [r for r in runtime.items if r["type"] in types and matches(r)]
    for name in pick["item"]:
        folded = name.casefold()
        wanted += [r for r in runtime.items
                   if folded in r["name"].casefold() and matches(r)]
    if needle and not types and not pick["item"]:
        wanted += [r for r in runtime.items if matches(r)]
    seen: set[int] = set()
    unique: list[dict] = []
    for row in wanted:
        if row["form"] in seen:
            continue
        seen.add(row["form"])
        unique.append(row)
    return unique[: pick["limit"]] if pick["limit"] else unique


def selection_label(pick: dict) -> str:
    parts = []
    if pick["item"]:
        parts.append(", ".join(pick["item"]))
    if pick["form"]:
        parts.append(" ".join(pick["form"]))
    if pick["type"]:
        parts.append("/".join(pick["type"]))
    if pick["search"]:
        parts.append(f'"{pick["search"]}"')
    return " + ".join(parts) or "selection"


# ── workbench ────────────────────────────────────────────────────────────────

class Workbench:
    def __init__(self, args) -> None:
        self.args = args
        self.runtime = Runtime(args)
        self.lock = threading.Lock()
        self.pick = pick_from_args(args)
        self.version = 0
        self.phase = "starting"
        self.items: list[dict] = []
        self.done = 0
        self.total = 0
        self.drawn = 0
        self.started = 0.0
        self.finished = 0.0
        self.error = ""
        self.pending = threading.Event()
        self.stop = threading.Event()
        self.out = Path(args.out)
        self.tiles = self.out / "tiles"
        self.previous_dir = self.tiles / "prev"
        self.focus = bool(self.pick.get("focus"))
        self.pass_size = args.size
        # --no-tiles is for a full audit: 10,638 PNGs is 88 MB and the TSV is
        # what a sweep is compared on.
        self.write_tiles = not getattr(args, "no_tiles", False)

    def state(self, with_items: bool = True) -> dict:
        with self.lock:
            return {
                "version": self.version,
                "phase": self.phase,
                "done": self.done,
                "total": self.total,
                "drawn": self.drawn,
                "elapsed": round((self.finished or time.time()) - self.started, 2) if self.started else 0,
                "label": selection_label(self.pick),
                "error": self.error,
                "log": list(self.runtime.log[-6:]),
                "items": list(self.items) if with_items else [],
                "size": self.pass_size,
                "focus": self.focus,
                "detailSize": self.args.detail_size,
            }

    def apply_selection(self, body: dict) -> None:
        """The page's form (or a find-box click): replace the selection and redraw."""
        item = str(body.get("item") or "")
        form = str(body.get("form") or "")
        kind = str(body.get("type") or "")
        limit = str(body.get("limit") or "")
        raw_size = str(body.get("size") or "").strip()

        def split(text: str) -> list[str]:
            return [x.strip() for x in text.replace(";", ",").split(",") if x.strip()]

        try:
            size = max(96, min(1024, int(raw_size))) if raw_size else self.args.size
        except ValueError:
            size = self.args.size
        forms = form.split()
        with self.lock:
            self.pick = {
                "item": split(item),
                "form": forms,
                "type": [kind] if kind else [],
                "search": str(body.get("search") or ""),
                "limit": 0,
                "size": size,
            }
            try:
                self.pick["limit"] = max(0, int(limit)) if limit.strip() else 0
            except ValueError:
                self.pick["limit"] = 0
            # A find-box click is one form and nothing else: draw it large.
            self.focus = bool(body.get("focus")) or (
                len(forms) == 1 and not self.pick["item"] and not self.pick["type"])
        self.pending.set()

    # -- one pass -------------------------------------------------------------
    def render_pass(self, selection: list[dict], size: int | None = None) -> None:
        size = max(96, min(1024, size or self.args.size))
        # The before/after pass needs the previous image on disk, so it is
        # skipped with the tiles rather than comparing against nothing.
        if self.write_tiles:
            self.tiles.mkdir(parents=True, exist_ok=True)
            self.previous_dir.mkdir(parents=True, exist_ok=True)
        with self.lock:
            self.pass_size = size
            self.phase = "rendering"
            self.done = 0
            self.drawn = 0
            self.error = ""
            self.started = time.time()
            self.finished = 0.0
            self.items = []
            self.total = len(selection)
            self.version += 1
        rt = self.runtime
        for index, item in enumerate(selection, 1):
            name = f"{item['form']:08X}"
            path = self.tiles / f"{name}.png"
            keep_prev = self.previous_dir / f"{name}.png"
            before = None
            if self.write_tiles:
                try:
                    before = path.read_bytes()
                except OSError:
                    pass
            geoms: list[dict] = []
            try:
                geoms, meta = sheet.prepare_item(item, rt.store, rt.plugins, rt.swaps, rt.cdb,
                                                 rt.textures, rt.meshes)
                if geoms:
                    image, tris = sheet.render_model(geoms, size)
                    meta["tris"] = tris
                else:
                    image = sheet.placeholder(size, meta["status"] or "no geometry")
            except Exception as exc:               # a crash is a finding, not a stop
                traceback.print_exc()
                meta = {"model": "", "geoms": 0, "tris": 0, "textured": 0,
                        "status": f"render failed: {exc}", "materials": []}
                image = sheet.placeholder(size, "render failed")
            if self.write_tiles:
                if before is not None:
                    keep_prev.write_bytes(before)
                elif keep_prev.exists():
                    keep_prev.unlink()             # a stale "before" would lie
                image.save(path)
            delta = difference(before, image) if self.write_tiles else None
            with self.lock:
                if geoms:
                    self.drawn += 1
                self.items.append({
                    "form": name,
                    "name": item["name"],
                    "type": item["type"],
                    "plugin": item["plugin"],
                    "status": meta["status"],
                    "model": meta["model"],
                    "geoms": meta["geoms"],
                    "tris": meta["tris"],
                    "textured": meta["textured"],
                    "delta": None if delta is None else round(delta, 3),
                    "changed": bool(delta is not None and delta > 0.05),
                    "png": f"{name}.png" if self.write_tiles else "",
                    "size": size,
                    "parts": list(meta.get("parts") or []),
                    "unplaced": list(meta.get("unplaced") or []),
                    # The per-material report is what a focused preview is for;
                    # a shelf of forty tiles does not need forty copies of it.
                    "materials": (list(meta.get("materials") or [])[:40]
                                  if len(selection) == 1 else []),
                })
                self.done = index
                self.version += 1
        with self.lock:
            self.phase = "idle"
            self.finished = time.time()
            changed = sum(1 for i in self.items if i["changed"])
            self.version += 1
        rt.note(f"drew {self.done} item(s): {self.drawn} with geometry, {changed} changed "
                f"in {self.finished - self.started:.1f}s")

    # -- threads --------------------------------------------------------------
    def worker(self) -> None:
        try:
            self.runtime.build()
            while not self.stop.is_set():
                with self.lock:
                    pick = dict(self.pick)
                selection = select(self.runtime, pick)
                self.runtime.note(f"selection: {len(selection)} record(s) - {selection_label(pick)}")
                if not selection:
                    with self.lock:
                        self.phase = "idle"
                        self.error = "nothing selected: pass --item, --form, --type or --search"
                else:
                    self.render_pass(selection, pick.get("size"))
                # Stay alive until the page or the watcher asks for another
                # pass. Waiting with a timeout only to watch `stop` keeps a
                # shutdown prompt; breaking out of the loop here is the bug
                # that made the first pass the only one.
                while not self.stop.is_set() and not self.pending.wait(0.25):
                    pass
                self.pending.clear()
        except SystemExit as exc:
            with self.lock:
                self.phase = "idle"
                self.error = str(exc)
        except Exception as exc:
            traceback.print_exc()
            with self.lock:
                self.phase = "idle"
                self.error = f"{type(exc).__name__}: {exc}"

    def watcher(self) -> None:
        """Poll the renderer sources; a settled mtime change triggers a redraw."""
        stamps = {name: mtime(RECON / name) for name in WATCHED}
        while not self.stop.is_set():
            time.sleep(0.6)
            changed = None
            for name in WATCHED:
                stamp = mtime(RECON / name)
                if stamp != stamps[name]:
                    changed = name
            if not changed:
                continue
            time.sleep(0.3)          # let the editor finish writing
            for name in WATCHED:
                stamps[name] = mtime(RECON / name)
            rt = self.runtime
            try:
                rt.reload_rules(changed)
            except Exception as exc:
                traceback.print_exc()
                with self.lock:
                    self.error = f"reload of {changed} failed: {exc}"
                continue
            with self.lock:
                self.phase = "reloading"
                self.version += 1
            self.pending.set()


def mtime(path: Path) -> float:
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def difference(before: bytes | None, after: Image.Image) -> float | None:
    """Mean absolute pixel difference against the previous pass, 0-255."""
    if not before:
        return None
    try:
        old = np.asarray(Image.open(io.BytesIO(before)).convert("RGB"), dtype=np.float32)
    except Exception:
        return None
    new = np.asarray(after.convert("RGB"), dtype=np.float32)
    if old.shape != new.shape:
        return None
    return float(np.abs(old - new).mean())


# ── page ─────────────────────────────────────────────────────────────────────

PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>SF-Shop render workbench</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; font: 12px/1.45 "Segoe UI", system-ui, sans-serif;
         background: #0b0d12; color: #e7e3e8; }
  header { position: sticky; top: 0; z-index: 2; display: flex; gap: 14px;
           align-items: baseline; flex-wrap: wrap; padding: 9px 14px;
           background: #14161deb; border-bottom: 1px solid #2a2130; }
  header b { color: #ffb3bf; letter-spacing: .04em; }
  .pill { padding: 1px 8px; border-radius: 9px; border: 1px solid #443041;
          color: #c9c3cc; font-size: 11px; }
  .pill.on { border-color: #b06b2c; color: #ffcb8a; }
  .bar { flex: 1 1 220px; height: 5px; background: #241a26; border-radius: 3px; overflow: hidden; }
  .bar i { display: block; height: 100%; width: 0; background: linear-gradient(90deg,#8a2d3e,#e0a24a); }
  form { display: flex; gap: 6px; align-items: center; padding: 8px 14px;
         border-bottom: 1px solid #2a2130; background: #101218; flex-wrap: wrap; }
  input, select, button { background: #1b1d26; color: #e7e3e8; border: 1px solid #39293d;
                          border-radius: 5px; padding: 4px 7px; font: inherit; }
  button { cursor: pointer; } button:hover { border-color: #8a5a2c; color: #ffcb8a; }
  main { padding: 12px 14px 40px; display: grid; gap: 10px;
         grid-template-columns: repeat(auto-fill, minmax(var(--tile), 1fr)); }
  .card { background: #12141b; border: 1px solid #241d2b; border-radius: 7px; overflow: hidden; }
  .card.changed { border-color: #e0a24a99; }
  .card .imgs { display: flex; background: #141820; }
  .card img { width: 100%; display: block; }
  .card .imgs.two img { width: 50%; }
  .card .imgs.two img + img { border-left: 1px dashed #3a2b38; }
  .card .meta { padding: 5px 7px; display: flex; flex-direction: column; gap: 1px; }
  .card .nm { color: #f3eef4; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .card .sub { color: #8d8794; font-size: 10.5px; overflow: hidden;
               text-overflow: ellipsis; white-space: nowrap; }
  .d { font-family: Consolas, monospace; }
  .d.hit { color: #ffcb8a; } .d.same { color: #6f8a6f; } .d.new { color: #7c8ba8; }
  .warn { color: #ff9aa8; }
  .note { padding: 6px 14px; color: #8d8794; }
  .findwrap { position: relative; flex: 0 1 300px; }
  .findwrap input { width: 100%; box-sizing: border-box; }
  .hits { position: absolute; top: calc(100% + 4px); left: 0; right: 0; z-index: 9;
          background: #151821; border: 1px solid #443041; border-radius: 6px;
          max-height: 340px; overflow: auto; box-shadow: 0 12px 30px #0009; }
  .hit { display: flex; gap: 8px; align-items: baseline; padding: 5px 8px;
         cursor: pointer; border-bottom: 1px solid #221c28; }
  .hit:last-child { border-bottom: 0; }
  .hit:hover, .hit.on { background: #241a26; }
  .hit b { color: #f3eef4; font-weight: 600; }
  .hit span { color: #8d8794; font-size: 10.5px; }
  .hit .fid { margin-left: auto; color: #ffcb8a; font-family: Consolas, monospace; }
  .focus { display: grid; grid-template-columns: minmax(240px, 512px) 1fr; gap: 16px;
           padding: 0 14px 24px; align-items: start; }
  .focus .shot { background: #12141b; border: 1px solid #241d2b; border-radius: 8px; padding: 8px; }
  .focus .shot img { width: 100%; display: block; }
  .focus h2 { margin: 2px 0 4px; font-size: 16px; color: #f3eef4; }
  .focus .sub { color: #8d8794; }
  .focus dl { display: grid; grid-template-columns: max-content 1fr; gap: 2px 12px; margin: 10px 0; }
  .focus dt { color: #8d8794; }
  .focus dd { margin: 0; overflow-wrap: anywhere; }
  .focus a { color: #ffcb8a; text-decoration: none; border-bottom: 1px dotted #8a5a2c; }
  .focus a:hover { color: #ffe0b2; }
  .focus table { border-collapse: collapse; width: 100%; font-size: 11px; margin-top: 8px; }
  .focus th, .focus td { text-align: left; padding: 3px 6px; border-bottom: 1px solid #221c28;
                         vertical-align: top; }
  .focus th { color: #8d8794; font-weight: 600; }
  @media (max-width: 900px) { .focus { grid-template-columns: 1fr; } }
</style></head>
<body>
<header>
  <b>RENDER WORKBENCH</b>
  <div class="findwrap">
    <input id="find" placeholder="Find an item" autocomplete="off" spellcheck="false"
           title="Name, FormID or plugin (press /)">
    <div id="hits" class="hits" hidden></div>
  </div>
  <span id="label" class="pill"></span>
  <span id="phase" class="pill"></span>
  <span id="count" class="pill"></span>
  <span id="elapsed" class="pill"></span>
  <div class="bar"><i id="prog"></i></div>
  <span id="ver" class="pill"></span>
</header>
<form id="pick"%FORMHIDDEN%>
  <input id="fItem" placeholder="name contains" size="16">
  <input id="fForm" placeholder="formID(s)" size="11">
  <select id="fType">
    <option value="">any type</option>
    <option>WEAP</option><option>ARMO</option><option>AMMO</option><option>MISC</option>
    <option>ALCH</option><option>BOOK</option><option>KEYM</option>
  </select>
  <input id="fLimit" placeholder="limit" size="5">
  <button type="submit">Redraw selection</button>
  <label class="pill"><input type="checkbox" id="onlyChanged"> changed only</label>
  <input id="filter" placeholder="filter tiles" size="13">
  <span id="hint" class="pill"></span>
</form>
<div class="note" id="note"></div>
<section id="focus" class="focus" hidden></section>
<main id="grid"></main>
<script>
const STATIC_SNAPSHOT = %STATIC%;
const MAX_TILES = %MAXTILES%;
const DEFAULTS = %DEFAULTS%;
let state = STATIC_SNAPSHOT;

function tileUrl(image, version) {
  return STATIC_SNAPSHOT ? 'tiles/' + image : 't/' + image + '?v=' + version;
}
function beforeUrl(image, version) {
  return STATIC_SNAPSHOT ? 'tiles/prev/' + image : 'prev/' + image + '?v=' + version;
}
function esc(s) {
  return String(s === undefined || s === null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}
function badge(it) {
  if (it.delta === null || it.delta === undefined) return '<span class="d new">&mdash; new</span>';
  if (it.changed) return '<span class="d hit">&Delta; ' + it.delta.toFixed(1) + '</span>';
  return '<span class="d same">&Delta; ' + it.delta.toFixed(2) + '</span>';
}
function card(it) {
  const two = it.changed && it.delta !== null && it.delta !== undefined;
  const v = state.version;
  const bits = [it.type, it.plugin, it.form].filter(Boolean).join(' &middot; ');
  const detail = it.geoms + ' geom &middot; ' + it.tris + ' tris &middot; ' + it.textured + ' tex';
  const status = it.status ? '<span class="sub warn">' + esc(it.status) + '</span>' : '';
  return '<div class="card' + (it.changed ? ' changed' : '') + '">' +
    '<div class="imgs' + (two ? ' two' : '') + '">' +
      '<img loading="lazy" src="' + tileUrl(it.png, v) + '" alt="' + esc(it.name) +
        '" title="' + esc(it.model || it.name) + '">' +
      (two ? '<img loading="lazy" src="' + beforeUrl(it.png, v) + '" alt="previous ' +
        esc(it.name) + '" title="previous pass">' : '') +
    '</div><div class="meta">' +
      '<span class="nm" title="' + esc(it.name) + '">' + esc(it.name) + '</span>' +
      '<span class="sub">' + bits + '</span>' +
      '<span class="sub">' + detail + ' ' + badge(it) + '</span>' + status +
    '</div></div>';
}
function baseName(path) {
  return String(path || '').split(/[\\/]/).pop();
}
function focusPanel(it) {
  const mats = (it.materials || []).map((m) =>
    '<tr><td class="d">' + esc(baseName(m.mat)) + '</td>' +
    '<td>' + esc(m.texture || 'none') + '</td>' +
    '<td>' + (typeof m.alpha === 'number' ? m.alpha.toFixed(2) : '') + '</td>' +
    '<td>' + esc([m.swap ? 'swap ' + m.swap : '', m.color_why || '', m.alpha_why || '',
                  m.effect ? 'effect only' : ''].filter(Boolean).join(' / ')) + '</td></tr>').join('');
  const parts = (it.parts || []).map((p) => esc(p)).join('<br>') || 'none';
  const unplaced = (it.unplaced || []).map((p) => esc(p)).join('<br>');
  const query = encodeURIComponent(it.name || '');
  const refs = query
    ? '<a href="https://starfield.fandom.com/wiki/Special:Search?query=' + query +
      '" target="_blank" rel="noopener">Fandom</a> · ' +
      '<a href="https://www.starfielddb.com/?s=' + query +
      '" target="_blank" rel="noopener">StarfieldDB</a>'
    : 'none';
  return '<div class="shot"><img src="' + tileUrl(it.png, state.version) + '" alt="' +
      esc(it.name) + '"></div>' +
    '<div><h2>' + esc(it.name) + '</h2>' +
    '<div class="sub">' + esc([it.type, it.plugin, it.form].filter(Boolean).join(' · ')) + '</div>' +
    '<dl>' +
      '<dt>status</dt><dd class="' + (it.status ? 'warn' : '') + '">' + esc(it.status || 'ok') + '</dd>' +
      '<dt>model</dt><dd class="d">' + esc(it.model || '') + '</dd>' +
      '<dt>reference</dt><dd>' + refs + '</dd>' +
      '<dt>parts</dt><dd class="d">' + parts + '</dd>' +
      (unplaced ? '<dt>unplaced</dt><dd class="warn d">' + unplaced + '</dd>' : '') +
      '<dt>geometry</dt><dd>' + it.geoms + ' geom · ' + it.tris + ' tris · ' +
        it.textured + ' textured</dd>' +
    '</dl>' +
    '<table><thead><tr><th>material</th><th>texture</th><th>alpha</th><th>decision</th></tr></thead>' +
    '<tbody>' + (mats || '<tr><td colspan="4">no material report</td></tr>') + '</tbody></table></div>';
}
function draw() {
  if (!state) return;
  document.documentElement.style.setProperty('--tile', (state.size || 160) + 'px');
  document.getElementById('label').textContent = state.label;
  const phase = document.getElementById('phase');
  phase.textContent = state.phase;
  phase.className = 'pill' + (state.phase === 'rendering' ? ' on' : '');
  document.getElementById('count').textContent =
    state.done + ' / ' + state.total + ' (' + state.drawn + ' drawn)';
  document.getElementById('elapsed').textContent = (state.elapsed || 0).toFixed(1) + 's';
  document.getElementById('ver').textContent = 'pass ' + state.version;
  document.getElementById('prog').style.width =
    state.total ? (100 * state.done / state.total) + '%' : '0';
  document.getElementById('note').innerHTML = state.error
    ? '<span class="warn">' + esc(state.error) + '</span>'
    : (state.log || []).map(esc).join('<br>');
  const all = state.items || [];
  const grid = document.getElementById('grid');
  const focus = document.getElementById('focus');
  if (state.focus) {
    grid.hidden = true;
    focus.hidden = false;
    focus.innerHTML = all.length ? focusPanel(all[0]) :
      '<p class="note">' + (state.phase === 'rendering' ? 'rendering the focused record…' :
        'nothing to show yet') + '</p>';
    document.getElementById('hint').textContent = 'focused record';
    return;
  }
  focus.hidden = true;
  grid.hidden = false;
  const onlyChanged = document.getElementById('onlyChanged').checked;
  const needle = document.getElementById('filter').value.trim().toLowerCase();
  let items = all;
  if (onlyChanged) items = items.filter((i) => i.changed);
  if (needle) items = items.filter((i) =>
    (i.name + ' ' + i.form + ' ' + (i.status || '')).toLowerCase().includes(needle));
  grid.innerHTML = items.slice(0, MAX_TILES).map(card).join('') ||
    '<p class="note">nothing to show yet</p>';
  document.getElementById('hint').textContent =
    items.length + ' tile(s)' + (items.length > MAX_TILES ? ', showing ' + MAX_TILES : '');
}
async function poll() {
  try {
    const reply = await fetch('api/state', { cache: 'no-store' });
    const next = await reply.json();
    const moved = !state || next.version !== state.version;
    state = next;
    if (moved) draw();
  } catch (err) {
    document.getElementById('phase').textContent = 'server gone';
  }
  setTimeout(poll, 900);
}
document.getElementById('pick').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (STATIC_SNAPSHOT) return;
  await fetch('api/select', { method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      item: document.getElementById('fItem').value,
      form: document.getElementById('fForm').value,
      type: document.getElementById('fType').value,
      limit: document.getElementById('fLimit').value,
    }) });
});

const findBox = document.getElementById('find');
const hitsBox = document.getElementById('hits');
let hits = [];
let hitIndex = -1;
let searchTimer = 0;
function closeHits() {
  hits = [];
  hitIndex = -1;
  hitsBox.hidden = true;
  hitsBox.innerHTML = '';
}
function paintHits() {
  hitsBox.innerHTML = hits.map((h, i) =>
    '<div class="hit' + (i === hitIndex ? ' on' : '') + '" data-i="' + i + '">' +
      '<b>' + esc(h.name) + '</b><span>' + esc(h.type) + ' · ' + esc(h.plugin) + '</span>' +
      '<span class="fid">' + esc(h.form) + '</span></div>').join('');
  hitsBox.hidden = hits.length === 0;
}
async function searchCatalog() {
  const q = findBox.value.trim();
  if (q.length < 2) { closeHits(); return; }
  try {
    const reply = await fetch('api/search?q=' + encodeURIComponent(q) + '&limit=24',
                              { cache: 'no-store' });
    hits = await reply.json();
    hitIndex = hits.length ? 0 : -1;
    paintHits();
  } catch (err) {
    closeHits();
  }
}
async function focusForm(form) {
  if (STATIC_SNAPSHOT) return;
  closeHits();
  findBox.blur();
  await fetch('api/select', { method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ item: '', form: form, type: '', limit: '1',
      size: (DEFAULTS && DEFAULTS.detailSize) || 512, focus: true }) });
  try {
    const url = new URL(location.href);
    url.searchParams.set('form', form);
    history.replaceState(null, '', url);
  } catch (err) { /* file: or an old browser: the focus still happened */ }
}
findBox.addEventListener('input', () => {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(searchCatalog, 140);
});
findBox.addEventListener('keydown', (event) => {
  if (event.key === 'ArrowDown' && hits.length) {
    hitIndex = (hitIndex + 1) % hits.length; paintHits(); event.preventDefault();
  } else if (event.key === 'ArrowUp' && hits.length) {
    hitIndex = (hitIndex - 1 + hits.length) % hits.length; paintHits(); event.preventDefault();
  } else if (event.key === 'Enter' && hitIndex >= 0) {
    focusForm(hits[hitIndex].form); event.preventDefault();
  } else if (event.key === 'Escape') {
    closeHits(); findBox.blur();
  }
});
hitsBox.addEventListener('mousedown', (event) => {
  const hit = event.target.closest('.hit');
  if (!hit) return;
  event.preventDefault();
  const picked = hits[Number(hit.dataset.i)];
  if (picked) focusForm(picked.form);
});
document.addEventListener('click', (event) => {
  if (!event.target.closest('.findwrap')) closeHits();
});
document.addEventListener('keydown', (event) => {
  if (event.key === '/' && document.activeElement !== findBox) {
    event.preventDefault(); findBox.focus();
  }
});
document.getElementById('filter').addEventListener('input', draw);
document.getElementById('onlyChanged').addEventListener('change', draw);
if (DEFAULTS) {
  document.getElementById('fItem').value = DEFAULTS.item || '';
  document.getElementById('fForm').value = DEFAULTS.form || '';
  document.getElementById('fType').value = DEFAULTS.type || '';
  document.getElementById('fLimit').value = DEFAULTS.limit || '';
}
if (STATIC_SNAPSHOT) {
  findBox.disabled = true;
  draw();
} else {
  const wanted = new URL(location.href).searchParams.get('form');
  if (wanted) focusForm(wanted);
  poll();
}
</script>
</body></html>
"""


def page_html(snapshot: dict | None, defaults: dict | None) -> str:
    """The page. A snapshot makes it a self-contained file; without one it polls."""
    return (PAGE
            .replace("%STATIC%", "null" if snapshot is None else json.dumps(snapshot))
            .replace("%MAXTILES%", json.dumps(600 if snapshot is None else 4000))
            .replace("%DEFAULTS%", json.dumps(defaults or {}))
            .replace("%FORMHIDDEN%", ' hidden' if snapshot is not None else ""))


# ── server ───────────────────────────────────────────────────────────────────

def make_handler(bench: Workbench):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):        # a poll every 0.9s is not log-worthy
            pass

        def _send(self, code: int, body: bytes, ctype: str) -> None:
            self.send_response(code)
            self.send_header("content-type", ctype)
            self.send_header("content-length", str(len(body)))
            self.send_header("cache-control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _png(self, path: Path) -> None:
            try:
                self._send(200, path.read_bytes(), "image/png")
            except OSError:
                self._send(404, b"no tile", "text/plain")

        def do_GET(self) -> None:            # noqa: N802 (stdlib naming)
            route = self.path.split("?")[0]
            if route in ("/", "/index.html"):
                defaults = {"item": ", ".join(bench.pick["item"]),
                            "form": " ".join(bench.pick["form"]),
                            "type": (bench.pick["type"] or [""])[0],
                            "limit": str(bench.pick["limit"] or ""),
                            "detailSize": bench.args.detail_size}
                return self._send(200, page_html(None, defaults).encode(),
                                  "text/html; charset=utf-8")
            if route == "/api/state":
                return self._send(200, json.dumps(bench.state()).encode(), "application/json")
            if route == "/api/search":
                query = parse_qs(urlparse(self.path).query)
                needle = (query.get("q") or [""])[0]
                try:
                    limit = max(1, min(50, int((query.get("limit") or ["24"])[0])))
                except ValueError:
                    limit = 24
                rows = search_catalog(bench.runtime.items, needle, limit)
                return self._send(200, json.dumps(rows).encode(), "application/json")
            if route.startswith("/t/"):
                return self._png(bench.tiles / route[3:])
            if route.startswith("/prev/"):
                return self._png(bench.previous_dir / route[6:])
            return self._send(404, b"not found", "text/plain")

        def do_POST(self) -> None:           # noqa: N802
            if self.path.split("?")[0] != "/api/select":
                return self._send(404, b"not found", "text/plain")
            length = int(self.headers.get("content-length") or 0)
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except ValueError:
                return self._send(400, b"bad json", "text/plain")
            bench.apply_selection(body)
            return self._send(200, b'{"ok":true}', "application/json")

    return Handler


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--item", action="append", default=[], help="name substring, repeatable")
    ap.add_argument("--form", action="append", default=[], help="formID, repeatable")
    ap.add_argument("--type", action="append", default=[], help="record type, repeatable")
    ap.add_argument("--search", default="", help="name/FormID substring")
    ap.add_argument("--limit", type=int, default=40, help="cap the selection (0 = no cap)")
    ap.add_argument("--shard", default="",
                    help="batch only: N/M renders every Mth selected record from N")
    ap.add_argument("--size", type=int, default=160, help="tile size in pixels")
    ap.add_argument("--detail-size", type=int, default=512,
                    help="focused-item size in pixels (the find box uses this)")
    ap.add_argument("--port", type=int, default=8791)
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="tile directory and page output")
    ap.add_argument("--once", action="store_true", help="render the selection and exit")
    ap.add_argument("--no-tiles", action="store_true",
                    help="batch only: keep the TSV, skip the PNG tile tree")
    ap.add_argument("--open", action="store_true", help="open the page in the browser")
    ap.add_argument("--catalog", default=None)
    ap.add_argument("--cdb", default=str(audit.CDB))
    ap.add_argument("--data", default=str(audit.GAME_DATA))
    ap.add_argument("--mods-root", default=str(audit.MODS_ROOT))
    ap.add_argument("--swaps", default=str(audit.SWAPS))
    args = ap.parse_args()

    bench = Workbench(args)
    if args.once:
        return run_once(bench)

    threading.Thread(target=bench.worker, daemon=True).start()
    threading.Thread(target=bench.watcher, daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(bench))
    url = f"http://127.0.0.1:{args.port}/"
    print(f"workbench: {url}  (tiles in {bench.tiles})", flush=True)
    if args.open:
        threading.Timer(1.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("stopping", flush=True)
    finally:
        bench.stop.set()
        bench.pending.set()
        server.server_close()
    return 0


def shard_selection(selection: list[dict], shard: str) -> list[dict]:
    """Every Mth row of one shard, so a long audit can run in parallel."""
    if not shard:
        return selection
    try:
        at, of = (int(part) for part in shard.split("/", 1))
    except ValueError:
        raise SystemExit(f"--shard wants N/M, got {shard!r}")
    if of < 1 or at < 0 or at >= of:
        raise SystemExit(f"--shard wants 0 <= N < M, got {shard!r}")
    return selection[at::of]


def run_once(bench: Workbench) -> int:
    """Batch mode: render, write a self-contained page and a TSV, exit."""
    bench.runtime.build()
    selection = shard_selection(select(bench.runtime, bench.pick), bench.args.shard)
    if not selection:
        print("nothing selected: pass --item, --form, --type or --search", file=sys.stderr)
        return 2
    bench.render_pass(selection, bench.pick.get("size") or bench.args.size)
    out = Path(bench.args.out)
    out.mkdir(parents=True, exist_ok=True)
    state = bench.state()
    html = out / "workbench.html"
    if bench.write_tiles:
        html.write_text(page_html(state, None), encoding="utf-8")
    tsv = out / "workbench.tsv"
    with tsv.open("w", encoding="utf-8", newline="") as fh:
        fh.write("form\tname\ttype\tplugin\tstatus\tmodel\tgeoms\ttris\ttextured\tdelta\tchanged\n")
        for it in state["items"]:
            fh.write("\t".join(str(it[k]) for k in
                               ("form", "name", "type", "plugin", "status", "model",
                                "geoms", "tris", "textured", "delta", "changed")) + "\n")
    where = f"tiles in {bench.tiles}" if bench.write_tiles else "no tiles (--no-tiles)"
    print(f"rendered {len(state['items'])} record(s); {where}; {tsv}"
          + (f"; {html}" if bench.write_tiles else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
