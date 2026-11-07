"""Pull `materials/creations/<name>/materialsbeta.cdb` out of the Data archives.

Every installed Creation ships its OWN material database, and that file is the
only statement it makes about its materials: the base-game database has no
record of them at all. The plugin reads them through `EnsureModCdbIndex`, which
is why they matter here - when the index misses one, the fuzzy scorer returns a
plausible sibling of the same weapon instead of the authored map and nothing
reports it (SFBGS050's BreechBlock receiver wore `breechblock_mods_color.dds`,
mean RGB 69/70/69, instead of `receiver_2_color.dds`, 115/99/95).

The extracted copies are fixtures for the data tests, the same way
`extracted/materialsbeta.cdb` is: the shipped databases cannot be regenerated,
so the assertions pin real bytes rather than a paraphrase of them.

    python extract_creation_cdb.py --list
    python extract_creation_cdb.py --extract
    python extract_creation_cdb.py --extract --force
"""
from __future__ import annotations

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "ba2_audit"))
from ba2lib import Ba2Archive  # noqa: E402

OUT_DIR = os.path.join(HERE, "extracted", "creations")
DEFAULT_DATA = r"B:\SteamLibrary\steamapps\common\Starfield\Data"
# SFBGS007's database is a complete copy of the base game's 105 MB file; the
# fixtures only need the small per-Creation ones.
DEFAULT_MAX_BYTES = 16 * 1024 * 1024


def data_dir() -> str:
    return os.environ.get("SFSE_STARFIELD_DATA", DEFAULT_DATA)


def candidates(root: str):
    """Yield (archive, entry, on_disk_bytes) for every Creation database."""
    if not os.path.isdir(root):
        return
    for name in sorted(os.listdir(root), key=str.lower):
        if not name.lower().endswith(".ba2"):
            continue
        try:
            ar = Ba2Archive(os.path.join(root, name))
        except Exception:
            continue
        if not ar.valid:
            continue
        for entry, rec in sorted(ar.entries.items()):
            if entry.endswith("materialsbeta.cdb") and "/creations/" in entry:
                yield os.path.join(root, name), entry, rec.size


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=data_dir())
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", action="store_true")
    ap.add_argument("--force", action="store_true", help="rewrite files that already exist")
    ap.add_argument("--max-mb", type=int, dest="max_mb", default=DEFAULT_MAX_BYTES // (1024 * 1024))
    a = ap.parse_args()

    max_bytes = a.max_mb * 1024 * 1024
    found = list(candidates(a.data))
    if not found:
        print("no Creation databases under %s" % a.data)
        return 1

    if a.list or not a.extract:
        for archive, entry, size in found:
            print("%10d  %-28s %s" % (size, os.path.basename(archive), entry))
        print("%d database(s)" % len(found))

    if not a.extract:
        return 0

    os.makedirs(OUT_DIR, exist_ok=True)
    written = skipped = 0
    for archive, entry, size in found:
        stem = entry.split("/creations/", 1)[1].split("/", 1)[0]
        dest = os.path.join(OUT_DIR, stem + ".cdb")
        if os.path.exists(dest) and not a.force:
            print("keep   %s (exists)" % os.path.basename(dest))
            continue
        if size > max_bytes:
            print("skip   %s (%d bytes > %d)" % (stem, size, max_bytes))
            skipped += 1
            continue
        ar = Ba2Archive(archive)
        blob, err = ar.read_entry(entry)
        if not blob:
            print("FAILED %s: %s" % (entry, err))
            skipped += 1
            continue
        with open(dest, "wb") as fh:
            fh.write(blob)
        print("write  %-28s %8d bytes" % (os.path.basename(dest), len(blob)))
        written += 1
    print("%d written, %d skipped" % (written, skipped))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
