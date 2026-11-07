#!/usr/bin/env python3
"""Read the material parent table that the game process has to supply.

`materialsbeta.cdb` cannot answer "who is this material's parent". It stores the
parent edges (451,529 rows of `{source id, target id, index, type=105}`) but no
`ObjectInfo`, so an edge endpoint never becomes a name - measured, see
`cdb_dbindex.py --report` and the 2026-10-30 entry in HANDOFF.md. The pairing
only exists inside the running engine, in `BSComponentDB2::DBFileIndex`.

This module is the offline half of that: it reads the sidecar such a dump would
write, and answers "the nearest ancestor that has art" for the renderer. When
the sidecar is absent - which is the state of the tree today - `load()` returns
None and the caller behaves exactly as before.

Sidecar format (`extracted/parent_index.tsv`, UTF-8, tab separated):

    # osf-parent-index 1
    # build: 1.16.244.0
    271702	AK_Kitchenware01_Base	31297
    67957	AK_Kitchenware01_Rubber01	271702

Three columns: object id, the object's `BSComponentDB2::CTName` name, and the
id of the object its type-105 edge points at (empty when it has none). `#` lines
and blank lines are ignored, so a dump can carry its provenance without a
separate manifest. Ids are decimal or `0x`-prefixed hex.

Anything that writes this file must satisfy one rule: **the name must be the
same string the CDB records**, because that is what `Cdb.own_set` is keyed by.
A dump that emits `AK_Kitchenware01_Base.mat` where the database says
`AK_Kitchenware01_Base` will find nothing; `find()` tolerates the `.mat`
spelling and the compiled `<path>.mat_<Object>` spelling, and nothing more.

    python parent_index.py --file extracted/parent_index.tsv --chain AK_Kitchenware01_Rubber01
"""
from __future__ import annotations

import argparse
import os
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(HERE, "extracted", "parent_index.tsv")

MAGIC = "osf-parent-index"


def fold(text: str) -> str:
    """The comparison form: forward slashes, lowercase, no `data/` prefix."""
    out = text.replace("\\\\", "\\").replace("\\", "/").casefold().strip()
    if out.startswith("data/"):
        out = out[5:]
    return out


def spellings(name: str) -> list[str]:
    """The forms `find()` will try, in order.

    A material reaches the renderer as a `.mat` path from a NIF, while the CDB
    keys its records by the material's own name, and a compiled `.mat` file's
    objects are spelled `<path>.mat_<Object>`. All three are the same material
    spelled by a different tool, so all three are tried before giving up.
    """
    base = fold(name)
    if not base:
        return []
    out = [base]
    if ".mat_" in base:
        path, _, obj = base.partition(".mat_")
        out += [obj, path + ".mat", path]
    if base.endswith(".mat"):
        out.append(base[:-4])
    stem = base.rsplit("/", 1)[-1]
    if stem and stem not in out:
        out.append(stem)
    if stem.endswith(".mat"):
        out.append(stem[:-4])
    seen: set[str] = set()
    return [x for x in out if x and not (x in seen or seen.add(x))]


@dataclass(frozen=True)
class ParentIndex:
    names: dict[int, str]
    parents: dict[int, int]
    origin: str = ""

    def __len__(self) -> int:
        return len(self.names)

    @property
    def available(self) -> bool:
        return bool(self.names)

    def find(self, name: str) -> int | None:
        """The id the dump gave this material, or None."""
        for candidate in spellings(name):
            for oid, known in self.names.items():
                if fold(known) == candidate:
                    return oid
        return None

    def parent_of(self, name: str) -> tuple[int, str] | None:
        """(id, name) of the immediate parent, when the dump has one."""
        oid = self.find(name)
        if oid is None:
            return None
        parent = self.parents.get(oid)
        if parent is None or parent not in self.names:
            return None
        return parent, self.names[parent]

    def chain(self, name: str, limit: int = 8) -> list[str]:
        """Ancestor names, nearest first, cycle-safe, self excluded."""
        oid = self.find(name)
        out: list[str] = []
        seen = {oid} if oid is not None else set()
        while oid is not None and len(out) < limit:
            parent = self.parents.get(oid)
            if parent is None or parent in seen:
                break
            seen.add(parent)
            anc = self.names.get(parent)
            if anc is None:
                break
            out.append(anc)
            oid = parent
        return out


def load(path: str = DEFAULT) -> ParentIndex | None:
    """Parse the sidecar, or None when it is not there."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return None
    names: dict[int, str] = {}
    parents: dict[int, int] = {}
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        try:
            oid = int(parts[0], 0)
        except ValueError:
            continue
        name = parts[1].strip()
        if not name:
            continue
        names[oid] = name
        if len(parts) > 2 and parts[2].strip():
            try:
                parents[oid] = int(parts[2], 0)
            except ValueError:
                pass
    if not names:
        return None
    return ParentIndex(names, parents, path)


_CACHE: dict[str, ParentIndex | None] = {}


def cached(path: str = DEFAULT) -> ParentIndex | None:
    """One parse per process; missing file is cached as None too."""
    if path not in _CACHE:
        _CACHE[path] = load(path)
    return _CACHE[path]


def reset(path: str = DEFAULT) -> None:
    _CACHE.pop(path, None)


def colour_via_parent(mat: str, cdb, index: ParentIndex | None) -> tuple[str, str] | None:
    """(colour map, ancestor name) for the nearest ancestor that has art.

    The walk only asks `Cdb.own_set`, so an ancestor is used exactly when the
    database would have answered for it directly - no new resolution rules are
    invented here, and an ancestor with an empty set lends nothing.
    """
    if index is None or not index.available:
        return None
    for ancestor in index.chain(mat):
        own, _how = cdb.own_set(ancestor)
        if own and own.get("color"):
            return own["color"], ancestor
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--file", default=DEFAULT)
    ap.add_argument("--chain", default="")
    ap.add_argument("--limit", type=int, default=8)
    a = ap.parse_args()
    index = load(a.file)
    if index is None:
        print("no parent index at %s" % a.file)
        return 2
    print("%d named objects, %d with a parent (%s)" % (len(index), len(index.parents), index.origin))
    if a.chain:
        print("%s -> %s" % (a.chain, " -> ".join(index.chain(a.chain, a.limit)) or "<no parent>"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
