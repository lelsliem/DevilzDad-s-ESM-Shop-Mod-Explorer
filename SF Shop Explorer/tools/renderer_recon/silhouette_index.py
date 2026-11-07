#!/usr/bin/env python3
"""Render the whole silhouette audit as one labelled, family-grouped 160px index.

    python tools/renderer_recon/silhouette_index.py
    python tools/renderer_recon/silhouette_index.py --out reports/silhouette-index-320.png --size 320

The index contains the 19 Bacta vials that the model-keyed close cleared and
the 28 records that remain flagged, grouped by the triage families in
`reports/silhouette-audit-2026-11-01.md`. Each tile carries its FormID, type,
closed-contour count and name, so a change to a pane-close rule can be reviewed
against the whole audit instead of one family at a time. The default run also
writes `reports/silhouette-index-160.html`: a self-contained copy with
combinable family, flagged/cleared and text filters, plus a workbench focus
link on every record. Search matches are highlighted; `/` focuses the search
box, Escape clears it, and Clear filters resets family, status and text
together. The active filters are kept in the URL as `family`, `status` and
`q`, so a filtered view can be reloaded or shared; Copy link copies that
canonical URL, each tile has its own copy link for its direct workbench record
URL, and Copy visible links copies the current filtered set one URL per line.

It exits non-zero if a listed record is missing or draws no geometry, or if a
Bacta vial is no longer one contour. The 28 flags are expected to keep two or
more contours; a flag that becomes one is shown green but does not fail the
index, because that would be a fix rather than a regression.
"""
from __future__ import annotations

import argparse
import base64
import io
import sys
from html import escape as html_escape
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402
import contact_sheet as sheet  # noqa: E402
from bacta_vial_sheet import (VIALS as VIAL_RECORDS, blank_tile, contours,
                              font)  # noqa: E402

# Triage groups from reports/silhouette-audit-2026-11-01.md. The first group is
# the cleared Bacta family; the rest are the 28 expected multi-pane props.
FAMILIES = (
    ("Bacta vials - cleared by the 1/13 close", True, VIAL_RECORDS),
    ("FloaterA Nautiloos eye lenses and skins", False, (
        ("Starfield.esm", "00205F01"),
        ("ShatteredSpace.esm", "010805BB"),
        ("ShatteredSpace.esm", "010805BC"),
        ("ShatteredSpace.esm", "010805BD"),
    )),
    ("Varuun Optic Lens 2 / (G)", False, (
        ("ClothingPack_Standalone_Z.esm", "FE045C84"),
        ("ClothingPack_Standalone_Z.esm", "FE045CE6"),
    )),
    ("SpartanArmorCollection split visors", False, (
        ("SpartanArmorCollection.esm", "FE047C8F"),
        ("SpartanArmorCollection.esm", "FE047C7B"),
        ("SpartanArmorCollection.esm", "FE047C90"),
        ("SpartanArmorCollection.esm", "FE047C84"),
        ("SpartanArmorCollection.esm", "FE047C8C"),
    )),
    ("SW weapon lens stacks", False, (
        ("Starfield.esm", "001F96EC"),
        ("Starfield.esm", "00215F42"),
        ("Starfield.esm", "00253A16"),
        ("Starfield.esm", "0026D96D"),
        ("SW Blasters.esm", "0F000987"),
        ("SW Blasters.esm", "0F00098D"),
        ("Endor.esm", "46000A5F"),
    )),
    ("Antique video tape windows", False, (
        ("Starfield.esm", "0005B4E4"),
        ("Starfield.esm", "00198EA4"),
    )),
    ("Bubble Level", False, (("Starfield.esm", "0020B982"),)),
    ("Separate glass inserts", False, (
        ("ClothingPack_Standalone_Z.esm", "FE045AEF"),
        ("Starfield.esm", "0012A061"),
        ("Starfield.esm", "0012C6CF"),
        ("Starfield.esm", "00190D42"),
        ("Starfield.esm", "0021952C"),
    )),
    ("Skin Cream Bottle", False, (("Starfield.esm", "0028B123"),)),
    ("Display Shelf", False, (("Starfield.esm", "00363CC9"),)),
)

EXPECTED = 47

CSS = """
:root { color-scheme: dark; }
body { margin: 0; padding: 18px; background: #101318; color: #d8dbe0;
       font: 12px/1.45 ui-sans-serif, system-ui, sans-serif; }
header { margin-bottom: 12px; }
h1 { margin: 0 0 3px; font-size: 20px; font-weight: 600; }
.head-meta { margin: 0; color: #8a8f98; }
.head-meta a { color: #ffcb8a; text-decoration: none; }
.controls { display: flex; flex-wrap: wrap; gap: 8px; align-items: center;
            margin: 14px 0 9px; }
#search { flex: 0 1 360px; min-width: 220px; padding: 7px 10px;
          background: #161a22; color: #d8dbe0; border: 1px solid #2a303c;
          border-radius: 5px; font: inherit; }
#search:focus { outline: none; border-color: #8a5a2c; }
.status { display: flex; border: 1px solid #2a303c; border-radius: 5px;
          overflow: hidden; }
.status button { background: #161a22; border: 0; border-right: 1px solid #2a303c;
                 color: #9fa6b2; padding: 7px 10px; cursor: pointer; font: inherit; }
.status button:last-child { border-right: 0; }
.status button.on { background: #241a12; color: #ffe0b2; }
.clear, .copy { padding: 7px 10px; border: 1px solid #2a303c; border-radius: 5px;
                background: #161a22; color: #c8ccd4; cursor: pointer; font: inherit; }
.copy { min-width: 88px; }
.clear:hover:not(:disabled), .copy:hover:not(:disabled) {
  border-color: #8a5a2c; color: #ffe0b2; }
.copy.copied, .copy.copied:hover { border-color: #4d7a5f; color: #a8e0bd; }
.copy.failed, .copy.failed:hover { border-color: #7a4d4d; color: #e0a8a8; }
.copy-visible { min-width: 126px; }
.clear:disabled, .copy:disabled { color: #5f6672; cursor: default; }
.sr-only { position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px;
           overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; border: 0; }
.shown { margin: 0 0 10px; color: #7f8590; font-size: 11px; }
.empty { margin: 24px 0; color: #9fa6b2; }
.filters { display: flex; flex-wrap: wrap; gap: 6px; margin: 0 0 18px; }
.filters button { background: #161a22; border: 1px solid #2a303c; color: #c8ccd4;
                  border-radius: 999px; padding: 5px 10px; cursor: pointer;
                  font: inherit; }
.filters button.on { background: #241a12; border-color: #ffcb8a; color: #ffe0b2; }
.filters .dot { display: inline-block; width: 7px; height: 7px; border-radius: 50%;
                margin-right: 5px; vertical-align: 1px; }
.dot.one { background: #82c8a0; }
.dot.split { background: #d2966e; }
.family { margin: 0 0 22px; padding-top: 10px; border-top: 1px solid #232936; }
.family h2 { margin: 0 0 9px; font-size: 15px; font-weight: 600; }
.family h2 em { margin-left: 7px; color: #7f8590; font-size: 10.5px;
                font-style: normal; font-weight: 400; }
.tag { display: inline-block; margin-right: 7px; padding: 1px 6px; border-radius: 3px;
       font-size: 9.5px; font-weight: 500; letter-spacing: .04em; text-transform: uppercase;
       vertical-align: 2px; }
.tag.cleared { background: #173326; color: #8fd8ad; }
.tag.flagged { background: #332215; color: #e0a273; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
        gap: 10px; }
figure { margin: 0; padding: 6px; background: #161a22; border: 1px solid #232936;
         border-radius: 6px; }
figure:hover { border-color: #3c4658; }
a.tile { display: block; }
img { display: block; border-radius: 4px; }
figcaption { margin-top: 5px; }
.tile-meta { color: #7f8590; font-size: 10px; letter-spacing: .02em; }
.tile-meta code { color: #9fa6b2; font: inherit; }
.count { margin-top: 2px; font-size: 10.5px; }
.count.one { color: #82c8a0; }
.count.split { color: #d2966e; }
.name { margin-top: 1px; color: #d8dbe0; overflow-wrap: anywhere; }
mark { background: #5a3b14; color: #ffe0b2; border-radius: 2px; }
.record-actions { display: flex; flex-wrap: wrap; align-items: baseline; gap: 8px;
                  margin-top: 4px; }
.record-actions .open { margin-top: 0; }
.open { display: inline-block; margin-top: 4px; color: #ffcb8a; font-size: 10px;
        text-decoration: none; border-bottom: 1px dotted #8a5a2c; }
.open:hover { color: #ffe0b2; }
.copy-record { padding: 0; background: none; border: 0; border-bottom: 1px dotted #4a5262;
               color: #9fa6b2; cursor: pointer; font: inherit; font-size: 10px; }
.copy-record:hover, .copy-record:focus-visible { color: #ffe0b2;
                                                border-color: #8a5a2c; }
.copy-record.copied, .copy-record.copied:hover { color: #a8e0bd;
                                                 border-color: #4d7a5f; }
.copy-record.failed, .copy-record.failed:hover { color: #e0a8a8;
                                                 border-color: #7a4d4d; }
"""

SCRIPT = """
const familyButtons = Array.from(document.querySelectorAll('.filters button'));
const statusButtons = Array.from(document.querySelectorAll('.status button'));
const sections = Array.from(document.querySelectorAll('section.family'));
const figures = Array.from(document.querySelectorAll('figure'));
const search = document.getElementById('search');
const clearButton = document.getElementById('clear');
const copyButton = document.getElementById('copy');
const copyVisibleButton = document.getElementById('copy-visible');
const copyStatus = document.getElementById('copy-status');
const shown = document.getElementById('shown');
const empty = document.getElementById('empty');
const initial = new URLSearchParams(location.search);
let family = familyButtons.some(button => button.dataset.filter === initial.get('family'))
  ? initial.get('family') : 'all';
let status = statusButtons.some(button => button.dataset.status === initial.get('status'))
  ? initial.get('status') : 'all';
search.value = initial.get('q') || '';
function clearHighlights() {
  for (const mark of Array.from(document.querySelectorAll('mark'))) {
    const parent = mark.parentNode;
    if (!parent) continue;
    parent.replaceChild(document.createTextNode(mark.textContent), mark);
    parent.normalize();
  }
}
function highlightText(root, query) {
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, {
    acceptNode(node) {
      return node.parentElement && node.parentElement.closest('a, mark, button')
        ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT;
    }
  });
  const nodes = [];
  while (walker.nextNode()) nodes.push(walker.currentNode);
  for (const node of nodes) {
    const text = node.nodeValue;
    const lower = text.toLowerCase();
    let from = 0;
    let at = lower.indexOf(query);
    if (at < 0) continue;
    const piece = document.createDocumentFragment();
    while (at >= 0) {
      piece.append(text.slice(from, at));
      const mark = document.createElement('mark');
      mark.textContent = text.slice(at, at + query.length);
      piece.append(mark);
      from = at + query.length;
      at = lower.indexOf(query, from);
    }
    piece.append(text.slice(from));
    node.parentNode.replaceChild(piece, node);
  }
}
function syncButtons() {
  for (const button of familyButtons)
    button.classList.toggle('on', button.dataset.filter === family);
  for (const button of statusButtons)
    button.classList.toggle('on', button.dataset.status === status);
}
function syncUrl() {
  const url = new URL(location.href);
  if (family === 'all') url.searchParams.delete('family');
  else url.searchParams.set('family', family);
  if (status === 'all') url.searchParams.delete('status');
  else url.searchParams.set('status', status);
  if (search.value === '') url.searchParams.delete('q');
  else url.searchParams.set('q', search.value);
  try {
    if (url.toString() !== location.href)
      history.replaceState(null, '', url);
  } catch (error) {
    // file:// previews can refuse replaceState; filtering still works.
  }
  return url.toString();
}
function legacyCopy(text) {
  const active = document.activeElement;
  const field = document.createElement('textarea');
  field.value = text;
  field.setAttribute('readonly', '');
  field.style.cssText = 'position:fixed;top:0;left:-9999px;opacity:0;';
  document.body.appendChild(field);
  field.focus();
  field.select();
  field.setSelectionRange(0, field.value.length);
  let copied = false;
  try {
    copied = document.execCommand('copy');
  } catch (error) {
    copied = false;
  }
  field.remove();
  if (active instanceof HTMLElement) active.focus();
  return copied;
}
async function copyText(text) {
  if (navigator.clipboard && navigator.clipboard.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (error) {
      // Fall through to the selection-based copy below.
    }
  }
  return legacyCopy(text);
}
function applyFilters() {
  const query = search.value.trim().toLowerCase();
  clearHighlights();
  let visible = 0;
  for (const figure of figures) {
    const okFamily = family === 'all' || figure.dataset.family === family;
    const okStatus = status === 'all' || figure.dataset.status === status;
    const okText = !query || (figure.dataset.search || '').includes(query);
    figure.hidden = !(okFamily && okStatus && okText);
    if (!figure.hidden) visible += 1;
  }
  for (const section of sections)
    section.hidden = !Array.from(section.querySelectorAll('figure')).some(f => !f.hidden);
  if (query) {
    for (const root of document.querySelectorAll('figcaption, section.family h2'))
      highlightText(root, query);
  }
  shown.textContent = 'Showing ' + visible + ' of ' + figures.length + ' records';
  empty.hidden = visible !== 0;
  clearButton.disabled = family === 'all' && status === 'all' && search.value === '';
  copyVisibleButton.disabled = visible === 0;
  syncUrl();
}
for (const button of familyButtons) {
  button.addEventListener('click', () => {
    family = button.dataset.filter;
    for (const other of familyButtons) other.classList.toggle('on', other === button);
    applyFilters();
  });
}
for (const button of statusButtons) {
  button.addEventListener('click', () => {
    status = button.dataset.status;
    for (const other of statusButtons) other.classList.toggle('on', other === button);
    applyFilters();
  });
}
clearButton.addEventListener('click', () => {
  family = 'all';
  status = 'all';
  search.value = '';
  syncButtons();
  applyFilters();
});
const copyTimers = new WeakMap();
let copyStatusToken = 0;
async function copyWithFeedback(button, text, announcement, resetLabel,
                                successLabel = 'Copied') {
  const copied = await copyText(text);
  button.textContent = copied ? successLabel : 'Copy failed';
  button.classList.toggle('copied', copied);
  button.classList.toggle('failed', !copied);
  const token = ++copyStatusToken;
  copyStatus.textContent = copied
    ? announcement + ' copied.'
    : 'Could not copy ' + announcement.charAt(0).toLowerCase() +
      announcement.slice(1) + '.';
  clearTimeout(copyTimers.get(button));
  copyTimers.set(button, setTimeout(() => {
    button.textContent = resetLabel;
    button.classList.remove('copied', 'failed');
    if (token === copyStatusToken) copyStatus.textContent = '';
  }, 1800));
}
copyButton.addEventListener('click', () => {
  copyWithFeedback(copyButton, syncUrl(), 'Filtered link', 'Copy link');
});
copyVisibleButton.addEventListener('click', () => {
  const urls = figures
    .filter(figure => !figure.hidden)
    .map(figure => figure.querySelector('.copy-record'))
    .filter(button => button)
    .map(button => button.dataset.url);
  if (!urls.length) return;
  const noun = urls.length === 1 ? 'link' : 'links';
  copyWithFeedback(copyVisibleButton, urls.join(String.fromCharCode(10)),
    urls.length + ' visible workbench ' + noun,
    'Copy visible links', 'Copied ' + urls.length);
});
for (const button of document.querySelectorAll('.copy-record')) {
  button.addEventListener('click', () => {
    copyWithFeedback(button, button.dataset.url,
      'Workbench link ' + button.dataset.form, 'copy link');
  });
}
document.addEventListener('keydown', (event) => {
  if (event.ctrlKey || event.metaKey || event.altKey) return;
  const target = event.target;
  const typing = target instanceof HTMLElement &&
    (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' ||
     target.isContentEditable);
  if (event.key === '/' && !typing) {
    event.preventDefault();
    search.focus();
    search.select();
  } else if (event.key === 'Escape' && search.value !== '') {
    event.preventDefault();
    search.value = '';
    applyFilters();
  }
});
search.addEventListener('input', applyFilters);
syncButtons();
applyFilters();
"""


def write_html(path: Path, tiles: list[dict], workbench_url: str,
               cleared_total: int, size: int) -> None:
    """A self-contained, filterable companion to the PNG sheet."""
    base_url = workbench_url.rstrip("/") + "/?form="
    buttons = [
        f'<button class="on" data-filter="all">All <b>{len(tiles)}</b></button>'
    ]
    for fi, (title, cleared, recs) in enumerate(FAMILIES):
        dot = "one" if cleared else "split"
        buttons.append(
            f'<button data-filter="{fi}"><span class="dot {dot}"></span>'
            f'{html_escape(title)} <b>{len(recs)}</b></button>')

    sections = []
    for fi, (title, cleared, recs) in enumerate(FAMILIES):
        figures = []
        for tile in (t for t in tiles if t["family"] == fi):
            form = tile["form"]
            rec = tile["rec"]
            raw_name = rec["name"] if rec else "record missing"
            raw_kind = rec["type"] if rec else "?"
            name = html_escape(raw_name)
            plugin = html_escape(tile["plugin"])
            kind = html_escape(raw_kind)
            count = tile["count"]
            if rec is None:
                count_text = "record missing"
                count_cls = "split"
            elif count == 0:
                count_text = tile["status"] or "no contour"
                count_cls = "split"
            else:
                count_text = f"{count} contour{'s' if count != 1 else ''}"
                count_cls = "one" if count == 1 else "split"
            buffer = io.BytesIO()
            tile["img"].save(buffer, format="PNG", optimize=True)
            data = base64.b64encode(buffer.getvalue()).decode("ascii")
            url = html_escape(base_url + form)
            tile_status = "cleared" if count == 1 else "flagged"
            search_text = " ".join((raw_name, form, raw_kind, tile["plugin"], title,
                                    count_text, tile_status)).lower()
            figures.append(
                f'<figure data-family="{fi}" data-status="{tile_status}" '
                f'data-search="{html_escape(search_text)}">'
                f'<a class="tile" href="{url}" target="_blank" rel="noopener" '
                f'title="Open {name} in the workbench">'
                f'<img src="data:image/png;base64,{data}" width="{size}" '
                f'height="{size}" alt="{name}"></a>'
                f'<figcaption><div class="tile-meta"><code>{form}</code> '
                f'{kind} &middot; {plugin}</div>'
                f'<div class="count {count_cls}">{html_escape(count_text)}</div>'
                f'<div class="name">{name}</div>'
                f'<div class="record-actions">'
                f'<a class="open" href="{url}" target="_blank" rel="noopener">'
                'open in workbench &nearr;</a>'
                f'<button class="copy-record" type="button" data-url="{url}" '
                f'data-form="{form}" aria-label="Copy workbench link for {name} ({form})" '
                f'title="Copy workbench link for {name} ({form})">copy link</button>'
                '</div></figcaption></figure>')
        tag = "cleared" if cleared else "flagged"
        sections.append(
            f'<section class="family" data-family="{fi}">'
            f'<h2><span class="tag {tag}">{tag}</span>{html_escape(title)}'
            f'<em>{len(recs)} record(s)</em></h2>'
            f'<div class="grid">{"".join(figures)}</div></section>')

    cleared_shown = sum(1 for tile in tiles if tile["count"] == 1)
    flagged_shown = len(tiles) - cleared_shown
    document = f"""<!doctype html>
<html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Silhouette audit - {size}px visual index</title>
<style>{CSS}</style>
<body>
<header>
<h1>Silhouette audit - {size}px visual index</h1>
<p class="head-meta">{len(tiles)} records &middot; {cleared_total} cleared &middot;
{len(tiles) - cleared_total} flagged &middot; workbench links assume
{html_escape(workbench_url)} &middot; <a href="silhouette-audit-2026-11-01.md">report</a></p>
</header>
<div class="controls">
<input id="search" type="search" autocomplete="off" spellcheck="false"
       placeholder="Filter by name, FormID, type, plugin or family"
       aria-label="Filter records by name, FormID, type, plugin or family"
       aria-keyshortcuts="/ Escape" title="Press / to focus, Escape to clear">
<div class="status" role="group" aria-label="Filter by contour state">
<button class="on" data-status="all">All <b>{len(tiles)}</b></button>
<button data-status="flagged">Flagged <b>{flagged_shown}</b></button>
<button data-status="cleared">Cleared <b>{cleared_shown}</b></button>
</div>
<button id="clear" class="clear" type="button" disabled
        title="Reset family, flagged/cleared and search filters">Clear filters</button>
<button id="copy" class="copy" type="button"
        title="Copy a shareable link to this filtered view">Copy link</button>
<button id="copy-visible" class="copy copy-visible" type="button" disabled
        title="Copy the workbench links for all currently visible records">Copy visible links</button>
<span id="copy-status" class="sr-only" role="status" aria-live="polite"></span>
</div>
<p class="shown" id="shown" aria-live="polite">Showing {len(tiles)} of {len(tiles)} records</p>
<div class="filters">{''.join(buttons)}</div>
{''.join(sections)}
<p class="empty" id="empty" hidden>No records match those filters.</p>
<script>{SCRIPT}</script>
</body></html>
"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(document, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="reports/silhouette-index-160.png")
    ap.add_argument("--html", default="reports/silhouette-index-160.html",
                    help="self-contained HTML index (empty to skip)")
    ap.add_argument("--workbench-url", default="http://127.0.0.1:8791/",
                    help="base URL for each record's focus link")
    ap.add_argument("--size", type=int, default=160)
    ap.add_argument("--audit", default="reports/silhouette-audit-2026-11-01.tsv",
                    help="audit TSV used to check that every remaining flag is indexed")
    ap.add_argument("--catalog", default=None)
    ap.add_argument("--cdb", default=str(audit.CDB))
    ap.add_argument("--data", default=str(audit.GAME_DATA))
    ap.add_argument("--mods-root", default=str(audit.MODS_ROOT))
    ap.add_argument("--swaps", default=str(audit.SWAPS))
    a = ap.parse_args()

    total = sum(len(records) for _title, _cleared, records in FAMILIES)
    if total != EXPECTED:
        print(f"family table has {total} records, expected {EXPECTED}", file=sys.stderr)
        return 2
    audit_path = Path(a.audit)
    if audit_path.is_file():
        flagged = set()
        for line in audit_path.read_text(encoding="utf-8").splitlines()[1:]:
            cols = line.split("\t")
            if len(cols) > 8 and cols[8] == "FLAG":
                flagged.add((cols[0], cols[1]))
        listed = {(plugin, form)
                  for _title, cleared, records in FAMILIES
                  if not cleared
                  for plugin, form in records}
        missing = flagged - listed
        extra = listed - flagged
        if missing or extra:
            print(f"index and {audit_path} disagree: missing={sorted(missing)} "
                  f"extra={sorted(extra)}", file=sys.stderr)
            return 2
    else:
        print(f"audit TSV not found at {audit_path}; skipping coverage check",
              file=sys.stderr)
    catalog_path = audit.find_catalog(a.catalog)
    if catalog_path is None:
        print("catalog dump not found", file=sys.stderr)
        return 2
    records = {(r["plugin"], f"{r['form']:08X}"): r
               for r in audit.parse_catalog(catalog_path)}
    store = audit.AssetStore([Path(a.data)],
                             Path(a.mods_root) if a.mods_root else None, False)
    plugins = audit.Plugins(store)
    swaps = audit.Swaps(Path(a.swaps))
    cdb = audit.Cdb(Path(a.cdb))
    textures = sheet.Textures(store)
    meshes: dict[str, dict] = {}

    size, pad, label_h, cols = a.size, 8, 44, 5
    cell_w, cell_h, header, section_h = size + pad * 2, size + label_h + pad, 50, 27
    heights = [section_h + ((len(recs) + cols - 1) // cols) * cell_h
               for _title, _cleared, recs in FAMILIES]
    out = Image.new("RGB", (cols * cell_w, header + sum(heights)), (10, 12, 16))
    draw = ImageDraw.Draw(out)
    title_font, small = font(14), font(11)
    draw.text((pad, 7), f"Silhouette audit - {size}px visual index",
              fill=(205, 210, 220), font=title_font)
    draw.text((pad, 25),
              "19 Bacta vials cleared by the 1/13 close; 28 expected multi-pane flags",
              fill=(140, 145, 155), font=small)
    legend = "green = 1 contour / amber = split"
    draw.text((cols * cell_w - pad - draw.textlength(legend, font=small), 7),
              legend, fill=(140, 145, 155), font=small)

    failures: list[str] = []
    tiles: list[dict] = []
    cleared_total = sum(len(recs) for _title, cleared, recs in FAMILIES if cleared)
    cleared_ok = 0
    y = header
    for fi, (title, cleared, recs) in enumerate(FAMILIES):
        accent = (130, 200, 160) if cleared else (210, 150, 110)
        draw.rectangle([0, y, cols * cell_w - 1, y + section_h - 1], fill=(16, 20, 27))
        draw.rectangle([0, y, 3, y + section_h - 1], fill=accent)
        draw.text((pad, y + 6), title, fill=(205, 210, 220), font=title_font)
        draw.text((cols * cell_w - 88, y + 7), f"{len(recs)} record(s)",
                  fill=(140, 145, 155), font=small)
        y += section_h
        for i, (plugin, form) in enumerate(recs):
            rec = records.get((plugin, form))
            x = (i % cols) * cell_w + pad
            yy = y + (i // cols) * cell_h
            if rec is None:
                missing_img = blank_tile(size, "record missing")
                out.paste(missing_img, (x, yy))
                draw.text((x, yy + size + 4), f"{form} {plugin}",
                          fill=(190, 120, 120), font=small)
                draw.text((x, yy + size + 19), "record missing",
                          fill=(190, 120, 120), font=small)
                tiles.append({"family": fi, "form": form, "plugin": plugin,
                              "rec": None, "count": 0, "img": missing_img,
                              "status": "record missing"})
                failures.append(f"{form} {plugin} missing")
                continue
            geoms, meta = sheet.prepare_item(
                rec, store, plugins, swaps, cdb, textures, meshes)
            count = contours(geoms, size) if geoms else 0
            if geoms:
                img, _tris = sheet.render_model(geoms, size)
            else:
                img = blank_tile(size, meta.get("status") or "no geometry")
            out.paste(img, (x, yy))
            tiles.append({"family": fi, "form": form, "plugin": plugin,
                          "rec": rec, "count": count, "img": img,
                          "status": meta.get("status") or ""})
            colour = (130, 200, 160) if count == 1 else (210, 150, 110)
            draw.text((x, yy + size + 4), f"{form}  {rec['type']}",
                      fill=(145, 150, 160), font=small)
            draw.text((x, yy + size + 19), f"{count} contour{'s' if count != 1 else ''}",
                      fill=colour, font=small)
            name = rec["name"] if len(rec["name"]) <= 28 else rec["name"][:27] + "..."
            draw.text((x, yy + size + 33), name, fill=(205, 210, 220), font=small)
            if cleared and count == 1:
                cleared_ok += 1
            elif cleared:
                failures.append(f"{form} {rec['name']} ({count} contours)")
            elif count == 0:
                failures.append(f"{form} {rec['name']} (no contour)")
        y += ((len(recs) + cols - 1) // cols) * cell_h

    out_path = Path(a.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out.save(out_path)
    print(f"wrote {out_path} ({out.size[0]}x{out.size[1]}), "
          f"{cleared_ok}/{cleared_total} vials one contour")
    if a.html:
        html_path = Path(a.html)
        write_html(html_path, tiles, a.workbench_url, cleared_total, size)
        print(f"wrote {html_path} ({len(tiles)} records)")
    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
