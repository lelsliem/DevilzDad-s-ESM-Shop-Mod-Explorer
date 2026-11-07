#!/usr/bin/env python3
"""Authored preview-transform coverage across the shop's render candidates.

Reads plugin records (never the running game) and answers one question:
**which of the eight `BGSPreviewTransform::transforms[]` slots do the catalog's
forms actually author, and what do those transforms say?**

Why this exists: the live run of 2026-10-17 showed the panel was framing models
from geometry bounds while the engine frames them from per-context authored
`BGSTransform` (TRNS) records. Measuring one item at a time in-game needs a
launch per item. The same data is in the plugin files, so the whole catalog can
be tallied in one pass here -- and nothing about it goes into the shipped mod.

What the records look like (verified against the live log, see
SHOP-RENDERING-NOTES.md):

    TESBoundObject                          TRNS record
      PTT2  = 8 x uint32 formID    -->        EDID = Inv_Guns_Inventory_10
      slot 0..7 = contexts                    DATA = 9 floats
                                              pos(3) rot(3) scale minZoom maxZoom

`PTT2` is a fixed eight-entry array; slot index is exactly the context the
plugin reads at runtime. Reading a form's PTT2 here reproduces what the game
logged for it (checked for Alien Tea, Razorback, Digipick, 7.62x39mm).

Output: one TSV (form, type, name, slot, transform) plus a summary on stdout.
No per-item files, no cache in the mod directory.

Usage:
    python tools/preview_transform_coverage.py                 # MO2's active profile
    python tools/preview_transform_coverage.py --out x.tsv --quiet
    python tools/preview_transform_coverage.py --load-order <profile>/loadorder.txt
"""

import argparse
import os
import struct
import sys
import zlib
from collections import Counter, defaultdict

DATA_DIR = r"B:\SteamLibrary\steamapps\common\Starfield\Data"
MO2_INI = r"B:\MO2\ModOrganizer.ini"
MO2_MODS = r"B:\MO2\Starfield\mods"


def _mo2_ini_value(key):
    """One value out of ModOrganizer.ini, with Qt's @ByteArray() unwrapped.

    Qt writes byte arrays as `key=@ByteArray(B:\\\\SteamLibrary\\\\...)` -- the
    doubled backslashes are the escaping, so they collapse on the way out.
    """
    try:
        with open(MO2_INI, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                name, _, value = line.partition("=")
                if name.strip() != key:
                    continue
                value = value.strip()
                if value.startswith("@ByteArray(") and value.endswith(")"):
                    value = value[len("@ByteArray("):-1]
                return value.replace("\\\\", "\\") or None
    except OSError:
        return None
    return None


def resolve_load_order(explicit=None):
    """The loadorder.txt of the profile MO2 actually has selected.

    This used to be a literal path to "Test profile". That is a snapshot of one
    machine on one afternoon: rename or switch the profile and the tool keeps
    reading a file nobody plays, then prints a confident coverage table for a
    load order that is no longer installed. Preference order is the flag, then
    $SFSE_MO2_LOADORDER, then the profile named in ModOrganizer.ini, then the
    most recently written profile that has a non-empty load order.

    Returns (path, how) where path is None if no candidate exists.
    """
    if explicit:
        return explicit, "--load-order"
    env = os.environ.get("SFSE_MO2_LOADORDER")
    if env:
        return env, "SFSE_MO2_LOADORDER"

    profiles_dir = _mo2_ini_value("profiles_directory") or r"B:\MO2\Starfield\profiles"
    selected = _mo2_ini_value("selected_profile")
    if selected:
        path = os.path.join(profiles_dir, selected, "loadorder.txt")
        if os.path.isfile(path):
            return path, "MO2 selected_profile=%r" % selected

    try:
        candidates = [os.path.join(profiles_dir, name, "loadorder.txt")
                      for name in os.listdir(profiles_dir)]
    except OSError:
        candidates = []
    candidates = [p for p in candidates if os.path.isfile(p) and os.path.getsize(p)]
    if candidates:
        newest = max(candidates, key=os.path.getmtime)
        return newest, "newest profile under %s" % profiles_dir
    return None, ("no load order: no --load-order, no $SFSE_MO2_LOADORDER, "
                  "and no non-empty profile under %s" % profiles_dir)

# Form types the shop catalog can draw. Matches BoundObjectOf() in
# ShopExplorer.cpp: anything outside this set has no mapped bound-object class
# and therefore no preview placement to read.
CANDIDATE_SIGS = {
    b"WEAP": "WEAP",
    b"ARMO": "ARMO",
    b"MISC": "MISC",
    b"ALCH": "ALCH",
    b"AMMO": "AMMO",
    b"BOOK": "BOOK",
    b"KEYM": "KEYM",
}

SLOT_COUNT = 8


def subrecords(body):
    """Split a record body into (signature, payload) pairs.

    Stops at the first non-printable signature: compressed and XXXX-prefixed
    records end up with a short tail, and a partial read is better than a wrong
    one for this measurement.
    """
    out = []
    pos = 0
    while pos + 6 <= len(body):
        sig = body[pos:pos + 4]
        if not all(0x20 <= c < 0x7F for c in sig):
            break
        size = struct.unpack_from("<H", body, pos + 4)[0]
        if pos + 6 + size > len(body):
            out.append((sig, body[pos + 6:]))
            break
        out.append((sig, body[pos + 6:pos + 6 + size]))
        pos += 6 + size
    return out


def record_body(data, pos, size, flags):
    body = data[pos:pos + size]
    if flags & 0x00040000 and len(body) >= 4:
        # Compressed record: 4-byte uncompressed size, then zlib data.
        try:
            body = zlib.decompress(body[4:])
        except zlib.error:
            pass
    return body


def plugin_masters(data):
    """Master file names from the TES4 header, in formID-index order."""
    masters = []
    if data[:4] != b"TES4":
        return masters
    size = struct.unpack_from("<I", data, 4)[0]
    for sig, payload in subrecords(data[24:24 + size]):
        if sig == b"MAST":
            masters.append(payload.split(b"\x00")[0].decode("ascii", "replace").lower())
    return masters


def walk_records(data, handlers):
    """Depth-first record walk; hands (sig, formID, body) to handlers."""
    total = len(data)

    def walk(pos, end):
        while pos + 24 <= end:
            sig = data[pos:pos + 4]
            if sig == b"GRUP":
                gsize = struct.unpack_from("<I", data, pos + 4)[0]
                if gsize < 24 or pos + gsize > end:
                    return
                walk(pos + 24, pos + gsize)
                pos += gsize
            elif all((0x41 <= c <= 0x5A or 0x61 <= c <= 0x7A or 0x30 <= c <= 0x39) for c in sig):
                size, flags, fid = struct.unpack_from("<III", data, pos + 4)
                if size > 64 * 1024 * 1024 or pos + 24 + size > end:
                    pos += 1
                    continue
                handler = handlers.get(sig)
                if handler is not None:
                    handler(fid, record_body(data, pos + 24, size, flags))
                pos += 24 + size
            else:
                pos += 1

    walk(0, total)


def find_plugin(name, data_dir, mods_root):
    """Locate a plugin: Data/ first (loose or copy), then any MO2 mod dir."""
    direct = os.path.join(data_dir, name)
    if os.path.isfile(direct):
        return direct
    if mods_root and os.path.isdir(mods_root):
        for entry in os.scandir(mods_root):
            if not entry.is_dir():
                continue
            candidate = os.path.join(entry.path, name)
            if os.path.isfile(candidate):
                return candidate
            for sub in ("Data", ""):
                candidate = os.path.join(entry.path, sub, name)
                if os.path.isfile(candidate):
                    return candidate
    return None


def load_order(path):
    names = []
    if not path or not os.path.isfile(path):
        return names
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if line and not line.startswith("#"):
                names.append(line)
    return names


def text(value):
    return value.split(b"\x00")[0].decode("utf-8", "replace")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--data-dir", default=DATA_DIR, help="Starfield Data directory")
    parser.add_argument("--load-order", default=None,
                        help="loadorder.txt (default: MO2's selected profile)")
    parser.add_argument("--mods-root", default=MO2_MODS, help="MO2 mods root (for plugins not in Data)")
    parser.add_argument("--out", default=None, help="TSV path (default: reports/preview-transform-coverage.tsv)")
    parser.add_argument("--quiet", action="store_true", help="only print the summary")
    args = parser.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    repo_reports = os.path.normpath(os.path.join(here, "..", "..", "..", "reports"))
    out_path = args.out or os.path.join(repo_reports, "preview-transform-coverage.tsv")

    profile_path, how = resolve_load_order(args.load_order)
    if profile_path is None:
        print("SKIP: %s" % how, file=sys.stderr)
        return 2
    if not args.quiet:
        print("load order: %s (%s)" % (profile_path, how))

    plugins = load_order(profile_path)
    if not plugins:
        print("SKIP: no plugins in %s" % profile_path, file=sys.stderr)
        return 2

    # canonical key = (owning plugin name, local formID); masters resolve the
    # high byte, so a mod's reference to Starfield.esm lands on the same key as
    # the master's own record.
    trns = {}                      # (plugin, fid) -> dict
    forms = {}                     # (plugin, fid) -> dict (winning record only)
    plugin_maps = {}
    missing = []

    def owner_of(plugin, fid):
        """(owning plugin, local id) for a record or a reference.

        A plugin's own records sit at index `len(masters)`, immediately after
        its masters -- a mod with three masters writes its own forms as
        `03xxxxxx`. An index below that is an override of that master, so the
        record is stored under the master's key and the later plugin wins.
        """
        index = fid >> 24
        local = fid & 0x00FFFFFF
        masters = plugin_maps.get(plugin, [])
        if index < len(masters):
            return (masters[index], local)
        return (plugin, local)

    for name in plugins:
        path = find_plugin(name, args.data_dir, args.mods_root)
        if not path:
            missing.append(name)
            continue
        with open(path, "rb") as handle:
            data = handle.read()
        key = name.lower()
        masters = plugin_masters(data)
        plugin_maps[key] = masters

        def on_trns(fid, body, key=key):
            edid = ""
            values = None
            for sig, payload in subrecords(body):
                if sig == b"EDID":
                    edid = text(payload)
                elif sig == b"DATA" and len(payload) >= 36:
                    values = struct.unpack("<9f", payload[:36])
            # Keyed by local id: resolve() hands back (plugin, local), so a
            # record stored under its full id (a mod's own 01xxxxxx) would
            # never be found, and later plugins overwrite earlier ones.
            trns[owner_of(key, fid)] = {"edid": edid, "values": values}

        handlers = {b"TRNS": on_trns}

        def make_form_handler(signature):
            def handler(fid, body, key=key, signature=signature):
                name_text = ""
                slots = [0] * SLOT_COUNT
                saw_ptt2 = False
                for sig, payload in subrecords(body):
                    # FULL is a string-table id, not a usable name offline; EDID
                    # is the only field that identifies the form here.
                    if sig == b"EDID":
                        name_text = text(payload)
                    elif sig == b"PTT2" and len(payload) >= 4 * SLOT_COUNT:
                        saw_ptt2 = True
                        slots = list(struct.unpack_from("<8I", payload, 0))
                record = {
                    "sig": signature,
                    "edid": name_text,
                    "slots": slots,
                    "fid": fid,
                    "plugin": key,
                }
                owner = owner_of(key, fid)
                if saw_ptt2:
                    forms[owner] = record
                else:
                    # A candidate form with no PTT2 at all is the auto-fit case
                    # the engine falls back on (e.g. UnarmedHuman). It must be
                    # counted, or coverage reads as a misleading 100%. A later
                    # override without PTT2 must not erase an earlier one that
                    # has it, so this case only fills gaps.
                    forms.setdefault(owner, record)
            return handler

        for sig in CANDIDATE_SIGS:
            handlers[sig] = make_form_handler(CANDIDATE_SIGS[sig])
        walk_records(data, handlers)

        if not args.quiet:
            print("  %-40s TRNS so far %5d  catalog forms %6d" % (name, len(trns), len(forms)))

    def resolve(plugin, ref):
        return owner_of(plugin, ref)

    rows = []
    unresolved = 0
    per_slot = Counter()
    per_slot_edid = defaultdict(Counter)
    per_sig = Counter()
    per_sig_covered = Counter()
    none_covered = 0

    for owner, form in sorted(forms.items(), key=lambda item: -item[1]["slots"].count(0)):
        plugin = form["plugin"]
        fid = form["fid"]
        used = [(slot, ref) for slot, ref in enumerate(form["slots"]) if ref]
        per_sig[form["sig"]] += 1
        if used:
            per_sig_covered[form["sig"]] += 1
        else:
            none_covered += 1
        for slot, ref in used:
            per_slot[slot] += 1
            target = resolve(plugin, ref)
            entry = trns.get(target) if target else None
            if not entry or not entry.get("values"):
                unresolved += 1
                per_slot_edid[slot]["<unresolved %08X>" % ref] += 1
                continue
            per_slot_edid[slot][entry["edid"] or "<no EDID>"] += 1
            values = entry["values"]
            rows.append((plugin, fid, owner, form["sig"], form["edid"], slot, ref,
                         entry["edid"], values))

    rows.sort(key=lambda row: (row[2], row[1], row[5]))

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="") as handle:
        # localID is printed alongside the full id because two mods can each
        # define their own 01xxxxxx record: the full id alone is ambiguous
        # across plugins, the (plugin, localID) pair is not.
        #
        # `plugin` is the file the winning record lives in, so an override
        # moves a form from starfield.esm to whichever mod replaces it. The
        # owner columns are the stable identity: (owner, ownerID) names the
        # record's master and local id, which is the same whether or not a mod
        # overrides it. Pin to those when comparing against a known form.
        handle.write("plugin\tformID\tlocalID\towner\townerID\ttype\tname\tslot\t"
                     "trnsID\ttrnsEDID\t"
                     "posX\tposY\tposZ\trotX\trotY\trotZ\tscale\tminZoom\tmaxZoom\n")
        for plugin, fid, owner, sig, name_text, slot, ref, trns_edid, values in rows:
            handle.write("%s\t%08X\t%06X\t%s\t%06X\t%s\t%s\t%d\t%08X\t%s\t" % (
                plugin, fid, fid & 0x00FFFFFF, owner[0], owner[1], sig, name_text,
                slot, ref, trns_edid))
            handle.write("\t".join("%.4f" % v for v in values))
            handle.write("\n")

    total_forms = len(forms)
    with_slots = total_forms - none_covered
    print()
    print("plugins read      : %d of %d%s" % (
        len(plugins) - len(missing), len(plugins),
        "" if not missing else "  (missing: %s)" % ", ".join(missing[:5])))
    print("catalog forms     : %d" % total_forms)
    print("  with any slot   : %d (%.1f%%)" % (with_slots, 100.0 * with_slots / max(total_forms, 1)))
    print("  with no slot    : %d (%.1f%%)" % (none_covered, 100.0 * none_covered / max(total_forms, 1)))
    print("unresolved refs   : %d" % unresolved)
    print()
    print("slot  forms      top authored transforms")
    for slot in range(SLOT_COUNT):
        if not per_slot[slot]:
            print("  %d   %6d      (unused)" % (slot, 0))
            continue
        top = ", ".join("%s x%d" % (name, count)
                        for name, count in per_slot_edid[slot].most_common(3))
        print("  %d   %6d      %s" % (slot, per_slot[slot], top))
    print()
    print("per type          : " + "  ".join(
        "%s %d/%d" % (sig, per_sig_covered[sig], per_sig[sig]) for sig in sorted(per_sig)))
    print("tsv               : %s (%d rows)" % (out_path, len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
