"""Replica of the plugin's CDB DIFF walk, so its alpha can be checked here.

LiveModel.cpp's second pass over materialsbeta.cdb walks the "DIFF" record
framing and keeps the first 0x25a override colour after the name record that
opens the object. That code cannot be exercised without launching Starfield,
so this file re-implements it and cross-checks the result against
cdb_records.py, which parses the same file through a different code path.

The layout was probed rather than assumed, because both earlier readings were
wrong in opposite directions:

    name (0x1f4)  ->  object id (0x020f)  ->  0x22b kind / 0x25a colour

Across the whole base database 16,211 0x25a records follow a name+id pair and
ZERO appear before the first name or between a name and its id. Each Velocity
beverage therefore carries the colour its own name claims (Red 0.967/0/0,
Blue 0/0.908/0.967, Yellow 1.0/0.917/0, ...), which is the check that proves
the association: the old value-first order gave Yellow the violet variant's
record, and the name-first pass that ignored the following id would have done
the same thing one object later.

    python cdb_alpha_replica.py            # summary + spot checks
    python cdb_alpha_replica.py --sample 20
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cdb_records as R  # noqa: E402
import cdb_schema as S  # noqa: E402

CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")


def lower_ascii(s: str) -> str:
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)


def alpha_key(object_name: str) -> str:
    """Mirror of CdbAlphaKey in LiveModel.cpp."""
    key = lower_ascii(object_name)
    key = key.replace("\\", "/")
    while True:
        us = key.rfind("_")
        if us < 0:
            break
        tail = key[us + 1:]
        if len(tail) < 8 or tail[:8] != "material":
            break
        if not all("0" <= c <= "9" for c in tail[8:]):
            break
        key = key[:us]
    while key.startswith("/"):
        key = key[1:]
    return key


KINDS = {b"Multiply": 1, b"Lerp": 2}


def _add_kind(out: dict[str, int], name: str, code: int) -> None:
    """Same alias set as the override-colour pass (see StoreCdbKind)."""
    out.setdefault(name, code)
    if name.endswith(".mat"):
        return
    out.setdefault(name + ".mat", code)
    if "/" not in name:
        out.setdefault("materials/" + name + ".mat", code)
        out.setdefault("data/materials/" + name + ".mat", code)


def scan_kinds(path: str = CDB) -> dict[str, int]:
    """Mirror of the plugin's TextureSetKind map (field 0x22b).

    Kept apart from the override colours on purpose: the kind is authored in
    the base database while the colour can come from a Creation Kit database
    for the same object, so the two are joined by name at lookup time. A kind
    of 1 (Multiply) or 2 (Lerp) means the material's texture set composites
    over a layer beneath it and the override alpha is that layer's weight, not
    the surface's opacity.
    """
    with open(path, "rb") as fh:
        blob = fh.read()
    size = len(blob)
    out: dict[str, int] = {}
    pending = 0
    current = ""
    pos = 0
    while pos + 16 <= size:
        if blob[pos:pos + 4] != b"DIFF":
            pos += 1
            continue
        ln = struct.unpack_from("<I", blob, pos + 4)[0]
        if ln < 12 or ln > 65536 or pos + 8 + ln > size:
            pos += 4
            continue
        field_id, kind, count = struct.unpack_from("<IHH", blob, pos + 8)
        data = pos + 16
        if field_id == 0x022B and kind == 0 and 2 <= count <= 200 and 8 + count <= ln:
            raw = blob[data:data + count].split(b"\x00")[0]
            kind_code = KINDS.get(raw, 3 if raw else 0)
            # NAME-FIRST: the CTName record opens the object, its components
            # follow it. Values seen before a name are that object's fallback
            # for the minority records that serialize components first.
            if kind_code and current:
                _add_kind(out, current, kind_code)
                slash = current.rfind("/")
                if slash >= 0:
                    _add_kind(out, current[slash + 1:], kind_code)
                current_kind = True
            elif kind_code:
                pending = kind_code
        elif field_id == 0x01F4 and kind == 0 and 4 <= count <= 200 and 8 + count <= ln:
            name = blob[data:data + count].split(b"\x00")[0]
            if all(0x20 <= c <= 0x7E for c in name):
                key = alpha_key(name.decode("latin-1"))
                if key and pending:
                    _add_kind(out, key, pending)
                    slash = key.rfind("/")
                    if slash >= 0:
                        _add_kind(out, key[slash + 1:], pending)
                pending = 0
                current = key
                current_kind = False
        pos += 8 + ln
    return out


def _store(out: dict[str, tuple], name: str, rgba: tuple) -> None:
    """Mirror of StoreCdbAlpha: bare key, .mat spelling, materials aliases."""
    if not name:
        return
    out.setdefault(name, rgba)
    if name.endswith(".mat"):
        return
    out.setdefault(name + ".mat", rgba)
    if "/" not in name:
        out.setdefault("materials/" + name + ".mat", rgba)
        out.setdefault("data/materials/" + name + ".mat", rgba)


def scan(path: str = CDB) -> dict[str, tuple]:
    """Mirror of the EnsureCdbIndex DIFF pass (NAME-FIRST).

    The CTName record (0x1f4) opens an object, its id (0x20f) follows, then the
    components - 0x22b kind and 0x25a override colour. Verified by probing the
    record order rather than by guessing: no 0x25a in the file precedes a name
    or sits between a name and its id. The value therefore belongs to the name
    before it, and the first one wins (a layered material carries more than one
    colour component and the first is the base layer).

    Both earlier readings were wrong: attaching the value to the PRECEDING
    name moved every colour one object early (Velocity Yellow wore Violet's),
    and the first name-first attempt here compared against a walk that also
    accepted components-before-name groups, which are simply the previous
    object's trailing records.
    """
    with open(path, "rb") as fh:
        blob = fh.read()
    size = len(blob)
    out: dict[str, tuple] = {}
    pending: tuple | None = None
    current = ""
    stored = False
    pos = 0
    while pos + 16 <= size:
        if blob[pos:pos + 4] != b"DIFF":
            pos += 1
            continue
        ln = struct.unpack_from("<I", blob, pos + 4)[0]
        if ln < 12 or ln > 65536 or pos + 8 + ln > size:
            pos += 4
            continue
        field_id, kind, count = struct.unpack_from("<IHH", blob, pos + 8)
        data = pos + 16
        # `ln` counts from pos+8 (the field id), not from the tag, so every
        # in-record bound below is measured against pos+8.
        if field_id == 0x025A and kind == 0 and count == 0:
            f = []
            for q in range(4):
                if 8 + q * 6 + 6 > ln:
                    break
                f.append(struct.unpack_from("<f", blob, data + q * 6)[0])
                nxt = struct.unpack_from("<H", blob, data + q * 6 + 4)[0]
                if nxt == 0xFFFF:
                    break
            if len(f) >= 4:
                value = (f[0], f[1], f[2], f[3])
                if current:
                    # First colour of the object wins: the base layer of a
                    # layered material, and the base variant of a recoloured
                    # one. A later permutation layer must not overwrite it.
                    if not stored:
                        _store(out, current, value)
                        stored = True
                elif pending is None:
                    pending = value
        elif field_id == 0x01F4 and kind == 0 and 4 <= count <= 200 and 8 + count <= ln:
            raw = blob[data:data + count]
            name = raw.split(b"\x00")[0]
            printable = all(0x20 <= c <= 0x7E for c in name)
            if printable:
                key = alpha_key(name.decode("latin-1"))
                # A value seen before the first name belongs to that object
                # only in the rare components-first layout; keep it as a
                # fallback, then let the following records own the object.
                if key and pending is not None:
                    _store(out, key, pending)
                current = key
                stored = False
                pending = None
        pos += 8 + ln
    return out


def truth(path: str = CDB) -> dict[str, tuple]:
    """Independent parse: the first 0x25a after each name record.

    Deliberately a second traversal (a record list plus an index walk) rather
    than a copy of the streaming loop above, so a framing mistake in one shows
    up as a disagreement. It also checks the ORDER the layout claim rests on:
    every colour must come after the name and after that object's id.
    """
    with open(path, "rb") as fh:
        blob = fh.read()
    recs = list(R.iter_records(blob))
    names: list[tuple[int, str]] = []
    for i, (_off, fid, kind, cnt, pay) in enumerate(recs):
        if fid == 0x01F4 and kind == 0 and 4 <= cnt <= 200:
            name = pay[:cnt].split(b"\x00")[0]
            if all(0x20 <= c <= 0x7E for c in name):
                names.append((i, name.decode("latin-1")))
    out: dict[str, tuple] = {}
    for n, (start, name) in enumerate(names):
        end = names[n + 1][0] if n + 1 < len(names) else len(recs)
        key = alpha_key(name)
        if not key:
            continue
        for j in range(start + 1, end):
            _off, fid, kind, cnt, pay = recs[j]
            if fid != 0x025A:
                continue
            v = R.decode(0, 0, pay)
            if isinstance(v, list) and len(v) == 4:
                out.setdefault(key, (v[0], v[1], v[2], v[3]))
            break
    return out


def layout_audit(path: str = CDB) -> tuple[int, int, int]:
    """Where the 0x25a records sit relative to their object's name/id pair.

    Returns (after name+id, between name and id, before any name). The layout
    claim in this module's docstring is exactly `(n, 0, 0)`, and it is cheap to
    re-prove on any database: a single linear pass over the record framing.
    """
    with open(path, "rb") as fh:
        blob = fh.read()
    recs = list(R.iter_records(blob))
    after_name = between = before_name = 0
    seen_name = False
    seen_id = False
    for _off, fid, kind, _cnt, _pay in recs:
        if fid == 0x01F4 and kind == 0:
            seen_name, seen_id = True, False
        elif fid == 0x020F:
            seen_id = True
        elif fid == 0x025A:
            if not seen_name:
                before_name += 1
            elif seen_id:
                after_name += 1
            else:
                between += 1
    return after_name, between, before_name


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sample", type=int, default=8)
    a = ap.parse_args()
    rep = scan()
    tru = truth()
    after, between, before = layout_audit()
    print("0x25a layout: after name+id %d | between name and id %d | before any name %d"
          % (after, between, before))
    layout_ok = between == 0 and before == 0 and after > 15000
    print("comparing all four channels (rgb tint + alpha)")
    print("replica keys %d   independent keys %d" % (len(rep), len(tru)))
    common = set(rep) & set(tru)
    bad = [k for k in common
           if any(abs(a - b) > 1e-6 for a, b in zip(rep[k], tru[k]))]
    print("common %d   mismatched %d" % (len(common), len(bad)))
    for k in bad[:10]:
        print("   MISMATCH %-56s replica=%s truth=%s"
              % (k, tuple(round(x, 4) for x in rep[k]), tuple(round(x, 4) for x in tru[k])))
    missing = [k for k in tru if k not in rep][:6]
    print("in truth but not replica: %d (e.g. %s)" % (len([k for k in tru if k not in rep]), missing[:3]))
    # Mirror FindCdbTint's key candidates for a material the CDB stores under
    # its bare name. The full-path and stem-with-extension candidates miss, so
    # the .mat-stripped bare-stem fallback is what resolves it.
    material_path = "Materials\\\\SetDressing\\\\CommonClutter\\\\PlasticPlainSmooth03_NotebookBlue01.mat"
    normalized = material_path.lower().replace("\\\\", "/")
    stem = normalized.rsplit("/", 1)[-1]
    bare_stem = stem[:-4] if stem.endswith(".mat") else stem
    # FindCdbTint strips the directory from the .mat stem before asking for
    # the no-extension bare key; reproduce that exact sequence.
    lookup_candidates = (normalized, "data/" + normalized, stem, bare_stem)
    resolved_key = next((key for key in lookup_candidates if key in rep), None)
    tint_ok = resolved_key is not None and len(rep.get(resolved_key, ())) == 4
    print("bare-stem FindCdbTint fallback key: %s -> %s"
          % (resolved_key or "MISS", tuple(round(x, 3) for x in rep[resolved_key]) if resolved_key else "-"))
    # Keep the code replica tied to the production fallback branch.
    live_model = os.path.join(HERE, "..", "..", "src", "LiveModel.cpp")
    with open(live_model, "r", encoding="utf-8") as src:
        tint_ok = tint_ok and "ov = tryKey(bareStem);" in src.read()
    print("bare-stem material-path tint resolves: %s" % ("PASS" if tint_ok else "FAIL"))
    # Name-first guards: the six items reported as unchanged after 0.8.304,
    # each of which previously read the record of the object BEFORE it.
    checks = [
        # (object name, expected rgba)
        ("Velocity_Beverage_Yellow_Material1", (1.0, 0.916671, 0.0, 0.501961)),
        ("Tranquilitea_ToGoCup_Base01_Material1", (0.138796, 0.763806, 0.809522, 0.607843)),
        ("HeartPlus_Material1", (0.733349, 0.647913, 0.0, 0.742962)),
        ("ErdeBrau_Can_Material1", (1.0, 0.961075, 0.797738, 0.0)),
        # Note 02's cover keeps its full CDB path (Material1 of the .mat), so
        # its colour is stored under the data/materials spelling and found by
        # FindCdbTint's "data/" + path candidate.
        ("Data\\Materials\\SetDressing\\Books\\GenBookB01_BookB04.mat_Material1",
         (0.42282700538635254, 0.43858999013900757, 0.4135200083255768, 0.0)),
    ]
    spot_ok = True
    for name, want in checks:
        got = rep.get(alpha_key(name))
        ok = got is not None and all(abs(a - b) <= 5e-6 for a, b in zip(got, want))
        spot_ok = spot_ok and ok
        shown = tuple(round(x, 4) for x in got) if got else "MISS"
        print("name-first %-40s %s %s" % (name, "PASS" if ok else "FAIL", shown))
    for name in ("PackagedFood_Cheese_Material1", "Razorback_Receiver_Material1"):
        got = rep.get(alpha_key(name))
        ok = got is None
        spot_ok = spot_ok and ok
        print("name-first %-40s %s %s" % (name, "PASS" if ok else "FAIL", got))
    # The strongest check that the association is right: every Velocity
    # beverage reads the colour its own name claims (under the old value-first
    # order each one wore the NEXT variant - Yellow wore violet).
    hues = {
        "velocity_beverage_red": lambda r, g, b: r > 0.9 and g < 0.1 and b < 0.1,
        "velocity_beverage_blue": lambda r, g, b: b > 0.9 and r < 0.1,
        "velocity_beverage_orange": lambda r, g, b: r > 0.9 and 0.4 < g < 0.8 and b < 0.1,
        "velocity_beverage_green": lambda r, g, b: g > 0.85 and r < 0.2 and b < 0.3,
        "velocity_beverage_violet": lambda r, g, b: r > 0.9 and b > 0.9 and g < 0.1,
        "velocity_beverage_yellow": lambda r, g, b: r > 0.9 and g > 0.85 and b < 0.05,
    }
    for key, test in hues.items():
        got = rep.get(key)
        ok = got is not None and test(got[0], got[1], got[2])
        spot_ok = spot_ok and ok
        print("hue %-30s %s %s" % (key, "PASS" if ok else "FAIL",
                                   tuple(round(x, 3) for x in got[:3]) if got else "MISS"))
    print("0x25a layout: %s" % ("PASS" if layout_ok else "FAIL"))
    print()
    print("alpha value spread:")
    from collections import Counter
    c = Counter(round(v[3], 3) for v in rep.values())
    for v, n in c.most_common(12):
        print("   %.3f  %d" % (v, n))
    print()
    print("sample lookups by bare stem (what FindCdbAlpha tries):")
    shown = 0
    for k in sorted(rep):
        if "/" in k or not k.endswith(".mat"):
            continue
        print("   %-56s rgba=%s" % (k, tuple(round(x, 3) for x in rep[k])))
        shown += 1
        if shown >= a.sample:
            break
    return 0 if not bad and tint_ok and spot_ok and layout_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())