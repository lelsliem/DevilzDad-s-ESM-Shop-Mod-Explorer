"""Audit what the preview ACTUALLY bound, from one SFShopExplorer log.

`shelf_audit.py` answers what the files say a shelf should resolve to;
`contact_sheet.py` draws the offline approximation. Neither can see the fuzzy
texture scorer, so neither can say what the running preview chose for a
material whose database record is empty or missing. The session log can: the
direct renderer logs every range it bound, with the texture, the tint and the
"UNTEXTURED (no prefetch)" verdict, plus the score of every fuzzy pick.

This tool joins those per-range decisions back to the database and sorts each
one into a verdict:

  authored    the bound map is the material's own CDB colour map
  substituted a map was bound but the CDB names a different one (fuzzy pick)
  flat        CDB has no colour map; the range is painted with its colour
  missing     CDB names a colour map but the range drew untextured
  glass       untextured because the material names itself see-through
  skipped     an effect/decal shell the renderer deliberately drops

    python tools/renderer_recon/session_preview_audit.py
    python tools/renderer_recon/session_preview_audit.py --item "Snow Globe"
    python tools/renderer_recon/session_preview_audit.py --out reports/session-preview.tsv

Read-only: the log, the extracted materialsbeta.cdb and the current rules in
aid_colour_audit.py (imported, never copied).
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402

DEFAULT_LOG = Path(r"C:\Users\mrlma\Documents\My Games\Starfield\SFSE\Logs\SFShopExplorer.log")

REQUEST = re.compile(r"direct: request '(.+?) \[FormID ([0-9A-Fa-f]{8})\]' \((\d+) mesh")
BOUND = re.compile(r"direct GPU: range \d+ '([^']+)' <- slot \d+ fmt=\d+ (\d+)x(\d+)"
                   r".*?tint=\(([^)]+)\)")
UNTEXTURED = re.compile(r"(?:direct|offline) GPU: range \d+ '([^']+)' <- UNTEXTURED")
PREFETCH = re.compile(r"prefetched '([^']+)' <- (\S+) \(score (\d+)\)")
FLAT_CDB = re.compile(r"cdb flat material '([^']+)'")
MAP_CDB = re.compile(r"cdb material map '([^']+)' -> (\S+)")
FAMILY = re.compile(r"cdb family set '([^']+)' supplies '(\S+)'")
TINT = re.compile(r"CDB override tint candidate")


def parse(log: Path) -> list[dict]:
    """Items in request order, each with the ranges the GPU pass bound."""
    items: list[dict] = []
    cur: dict | None = None
    for line in log.read_text(errors="replace").splitlines():
        m = REQUEST.search(line)
        if m:
            cur = {"name": m.group(1), "form": m.group(2), "meshes": int(m.group(3)),
                   "ranges": [], "fuzzy": {}, "flat": set(), "maps": {}, "family": {}}
            items.append(cur)
            continue
        if cur is None:
            continue
        m = BOUND.search(line)
        if m:
            cur["ranges"].append({"mat": m.group(1), "texture": "",
                                  "size": f"{m.group(2)}x{m.group(3)}",
                                  "tint": m.group(4)})
            continue
        m = UNTEXTURED.search(line)
        if m:
            cur["ranges"].append({"mat": m.group(1), "texture": "UNTEXTURED",
                                  "size": "", "tint": ""})
            continue
        m = PREFETCH.search(line)
        if m:
            cur["fuzzy"][audit.norm(m.group(1))] = (m.group(2), int(m.group(3)))
            continue
        m = FLAT_CDB.search(line)
        if m:
            cur["flat"].add(audit.norm(m.group(1)))
            continue
        m = MAP_CDB.search(line)
        if m:
            cur["maps"][audit.norm(m.group(1))] = m.group(2)
            continue
        m = FAMILY.search(line)
        if m:
            cur["family"][m.group(1)] = m.group(2)
    # The GPU pass logs the bound key on the slot line only via the prefetch
    # line; attach it by material there.
    for it in items:
        for r in it["ranges"]:
            hit = it["fuzzy"].get(audit.norm(r["mat"]))
            r["bound"] = hit[0] if hit else ""
            r["score"] = hit[1] if hit else 0
            r["cdb_map"] = it["maps"].get(audit.norm(r["mat"]), "")
    return items


def verdict(mat: str, r: dict, cdb: audit.Cdb) -> tuple[str, str]:
    """One range's verdict against the database."""
    own, how = cdb.own_set(mat)
    own_color = (own or {}).get("color", "")
    tex = r["texture"]
    key = audit.norm(mat)
    if tex == "UNTEXTURED":
        if own_color:
            return "missing", f"database names {own_color} but the range drew flat"
        if any(audit.has_name_word(mat, t) for t in audit.SHEER_TOKENS):
            return "glass", "clear shell (Fresnel only)"
        if key in r.get("flat", ()) or how in ("empty set", "no record"):
            return "flat", f"textureless set ({how})"
        return "flat", f"textureless set ({how})"
    if own_color and r["bound"] and audit.norm(r["bound"]) == audit.norm(own_color):
        return "authored", own_color
    if own_color and not r["bound"]:
        return "authored", own_color
    if own_color and r["bound"] and audit.norm(r["bound"]) != audit.norm(own_color):
        return "substituted", f"CDB {own_color} replaced by {r['bound']} (score {r['score']})"
    if r["bound"]:
        return "substituted", f"{r['bound']} (score {r['score']}; CDB has no map)"
    return "authored", "bound without a logged pick"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", default=str(DEFAULT_LOG))
    ap.add_argument("--item", action="append", default=[],
                    help="substring of the item name (repeatable)")
    ap.add_argument("--out", default=None, help="TSV path")
    ap.add_argument("--cdb", default=str(audit.CDB))
    a = ap.parse_args()

    log = Path(a.log)
    if not log.is_file():
        print(f"no log at {log}", file=sys.stderr)
        return 2
    items = parse(log)
    if a.item:
        items = [it for it in items
                 if any(s.lower() in it["name"].lower() for s in a.item)]
    cdb = audit.Cdb(Path(a.cdb))

    rows = []
    counts: collections.Counter = collections.Counter()
    for it in items:
        per_item: collections.Counter = collections.Counter()
        for r in it["ranges"]:
            v, why = verdict(r["mat"], r, cdb)
            per_item[v] += 1
            counts[v] += 1
            rows.append({"item": it["name"], "form": it["form"], "mat": r["mat"],
                         "verdict": v, "bound": r["bound"], "texture": r["texture"],
                         "score": r["score"], "tint": r["tint"], "why": why})
        it["verdicts"] = per_item

    print(f"[session] {len(items)} item(s), {len(rows)} range(s)", file=sys.stderr)
    for v, n in counts.most_common():
        print(f"  {v:<12} {n:5d}", file=sys.stderr)
    interesting = [it for it in items
                   if it["verdicts"].get("substituted") or it["verdicts"].get("missing")]
    print(f"[session] {len(interesting)} item(s) with a substituted or missing range",
          file=sys.stderr)
    for it in interesting:
        v = it["verdicts"]
        print(f"  {it['form']} {it['name'][:44]:44s} "
              f"authored={v.get('authored', 0)} flat={v.get('flat', 0)} "
              f"sub={v.get('substituted', 0)} missing={v.get('missing', 0)}")

    if a.out:
        out = Path(a.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        cols = ("item", "form", "mat", "verdict", "bound", "texture", "score", "tint", "why")
        with open(out, "w", encoding="utf-8", newline="") as fh:
            fh.write("\t".join(cols) + "\n")
            for r in rows:
                fh.write("\t".join(str(r[c]) for c in cols) + "\n")
        print(f"[session] -> {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
