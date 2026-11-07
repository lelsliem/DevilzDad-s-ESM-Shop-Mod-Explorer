#!/usr/bin/env python3
"""Read a profile's effective asset overlay the way LiveModel.cpp does.

The previewer never asks the engine for a file: `ResolveAsset` walks loose mod
directories in MO2 priority order, then the game's Data, then mod BA2 archives
(mods first), then the base-game archives. This module mirrors that order so
the audit tools can answer "which file will the preview actually read?" for a
given path - and therefore "is this item wearing a mod asset?".

The resolution order matters twice over:

  * loose files beat archives, in both the engine and MO2;
  * among loose files the FIRST enabled mod in modlist.txt wins. modlist.txt
    stores priority reversed - the last line is priority 0 - so the first
    `+Name` line is the highest priority.

Archives are parsed lazily by name map (ba2lib), including the BTDX v1 and
GNRL-DDS variants mod packs ship.
"""
from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "ba2_audit"))
from ba2lib import Ba2Archive  # noqa: E402
DEFAULT_BASE_INDEX = HERE / "ba2_audit" / "extracted" / "ba2nameindex.tsv"
DEFAULT_MO2 = Path(r"B:\MO2")
DEFAULT_MODS_ROOT = Path(r"B:\MO2\Starfield\mods")
DEFAULT_DATA_DIR = Path(r"B:\SteamLibrary\steamapps\common\Starfield\Data")

ASSET_EXTS = {".nif", ".mat", ".dds", ".mesh", ".cdb"}


def norm_key(key: str) -> str:
    """The key shape LiveModel's archive name tables and ResolveAsset use."""
    k = key.replace("\\", "/").lower()
    parts = [p for p in k.split("/") if p not in ("", ".")]
    if parts and parts[0] == "data":
        parts = parts[1:]
    return "/".join(parts)


def nif_key(path: str) -> str:
    """Canonical archive key for a model path (LiveModel::NormalizeNifKey).

    Form model fields are usually `Weapons\\Razorback\\Razorback.nif`, while
    the archive stores `meshes/weapons/razorback/razorback.nif`. A bare
    filename has no root to recover and is dropped rather than guessed.
    """
    k = norm_key(path)
    if not k.endswith(".nif") or k.startswith("geometries/"):
        return ""
    if not k.startswith("meshes/"):
        if "/" not in k:
            return ""
        k = "meshes/" + k
    while k.startswith("meshes/meshes/"):
        k = "meshes/" + k[len("meshes/meshes/"):]
    return k


def selected_profile(mo2_root: Path = DEFAULT_MO2) -> str:
    """The profile MO2 last launched, from ModOrganizer.ini."""
    ini = Path(mo2_root) / "ModOrganizer.ini"
    try:
        text = ini.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    m = re.search(r"^selected_profile=@ByteArray\((.*)\)\s*$", text, re.MULTILINE)
    return m.group(1).strip() if m else ""


def profile_order(profile: str, profiles_root: Path) -> tuple[list[str], list[str]]:
    """Enabled mod names in priority order, then the disabled ones.

    MO2 stores priority implicitly in the line order, reversed: the LAST line
    is priority 0 and the FIRST line has the highest priority. Both lists keep
    the file's order.
    """
    path = Path(profiles_root) / profile / "modlist.txt"
    enabled: list[str] = []
    disabled: list[str] = []
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if len(line) < 2:
                continue
            name = line[1:].strip()
            if name.endswith("_separator"):
                continue
            if line[0] == "+":
                enabled.append(name)
            elif line[0] in "-*":
                disabled.append(name)
    return enabled, disabled


@dataclass(frozen=True)
class Source:
    """Where the preview would read a path from."""

    kind: str  # "mod-loose", "data-loose", "mod-ba2", "base-ba2", "missing"
    mod: str = ""
    archive: str = ""

    @property
    def is_mod(self) -> bool:
        return self.kind in ("mod-loose", "mod-ba2")

    def __str__(self) -> str:
        if self.kind == "mod-loose":
            return "loose:" + self.mod
        if self.kind == "mod-ba2":
            return "ba2:%s/%s" % (self.mod, self.archive)
        if self.kind == "data-loose":
            return "Data (loose)"
        if self.kind == "base-ba2":
            return "vanilla:" + self.archive
        return "missing"


class Overlay:
    """Effective file overlay of one MO2 profile."""

    def __init__(
        self,
        profile: str = "",
        mods_root: Path | str = DEFAULT_MODS_ROOT,
        data_dir: Path | str = DEFAULT_DATA_DIR,
        profiles_root: Path | str | None = None,
        base_index: Path | str = DEFAULT_BASE_INDEX,
        mo2_root: Path | str = DEFAULT_MO2,
        parse_archives: bool = True,
    ):
        self.mods_root = Path(mods_root)
        self.data_dir = Path(data_dir)
        self.profile = profile or selected_profile(Path(mo2_root))
        if profiles_root is None:
            # profiles live next to the game dir, not next to the mods root,
            # for a portable MO2 instance (B:\MO2\Starfield\profiles).
            candidates = [
                self.mods_root.parent / "profiles",
                Path(mo2_root) / "profiles",
            ]
            profiles_root = next((p for p in candidates if p.is_dir()), candidates[0])
        self.enabled, self.disabled = (
            profile_order(self.profile, Path(profiles_root)) if self.profile else ([], [])
        )
        self.folder_by_lc = {
            p.name.lower(): p for p in self.mods_root.iterdir() if p.is_dir()
        } if self.mods_root.is_dir() else {}
        self.rank = {name.lower(): i for i, name in enumerate(self.enabled)}

        self.loose: dict[str, str] = {}       # path -> winning mod
        self.loose_owners: dict[str, set[str]] = {}
        self._scan_loose()

        self.archives: list[tuple[str, Path]] = []  # (mod, archive path) in lookup order
        self.archive_owner: dict[str, tuple[str, str]] = {}  # entry -> (mod, file)
        self._scan_archives(parse_archives)

        self.base_index: dict[str, str] = {}
        self.base_parsed: dict[str, Ba2Archive] = {}
        self._read_base_index(Path(base_index))

    # ── construction ──────────────────────────────────────────────────
    def _scan_loose(self) -> None:
        for name in self.enabled:
            mod_dir = self.folder_by_lc.get(name.lower())
            if mod_dir is None:
                continue
            root = str(mod_dir)
            for dirpath, dirnames, filenames in os.walk(mod_dir):
                dirnames[:] = [d for d in dirnames if d.lower() not in ("fomod", "docs", ".git")]
                rel_dir = os.path.relpath(dirpath, root)
                for fname in filenames:
                    if os.path.splitext(fname)[1].lower() not in ASSET_EXTS:
                        continue
                    rel = norm_key(fname if rel_dir == "." else rel_dir + "/" + fname)
                    # First enabled mod in modlist order wins. Later mods are
                    # still recorded for conflict tables.
                    self.loose.setdefault(rel, name)
                    self.loose_owners.setdefault(rel, set()).add(name)

    def _scan_archives(self, parse: bool) -> None:
        self.parsed: dict[Path, Ba2Archive] = {}
        for name in self.enabled:
            mod_dir = self.folder_by_lc.get(name.lower())
            if mod_dir is None:
                continue
            try:
                entries = sorted(mod_dir.glob("*.ba2"), key=lambda p: p.name.lower())
            except OSError:
                continue
            for entry in entries:
                self.archives.append((name, entry))
                if not parse:
                    continue
                ar = self._open(entry)
                if not ar or not ar.valid:
                    continue
                for key in ar.entries:
                    self.archive_owner.setdefault(norm_key(key), (name, entry.name))
                for key in ar.tex_files:
                    self.archive_owner.setdefault(norm_key(key), (name, entry.name))

    def _open(self, path: Path) -> Ba2Archive | None:
        ar = self.parsed.get(path)
        if ar is None:
            try:
                ar = Ba2Archive(path)
            except OSError:
                return None
            self.parsed[path] = ar
        return ar

    def _read_base_index(self, path: Path) -> None:
        if not path.exists():
            return
        with path.open("r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                name, _, arc = line.rstrip("\n").rpartition("\t")
                if name and arc:
                    self.base_index[norm_key(name)] = arc

    # ── lookups ───────────────────────────────────────────────────────
    def resolve(self, key: str) -> Source:
        """Mirror LiveModel::ResolveAsset: loose mods, Data, mod BA2s, base."""
        k = norm_key(key)
        mod = self.loose.get(k)
        if mod:
            return Source("mod-loose", mod=mod)
        if (self.data_dir / k).is_file():
            return Source("data-loose")
        owner = self.archive_owner.get(k)
        if owner:
            return Source("mod-ba2", mod=owner[0], archive=owner[1])
        base = self.base_index.get(k)
        if base:
            return Source("base-ba2", archive=base)
        return Source("missing")

    def read(self, key: str) -> bytes | None:
        """The winning payload, including base-game archives.

        Base archives are opened lazily and cached: reading a base NIF's
        material strings is what lets the catalog catch a mod that replaces
        only the .mat or texture of a vanilla model.
        """
        k = norm_key(key)
        mod = self.loose.get(k)
        if mod:
            return _read_file(self.folder_by_lc[mod.lower()] / k)
        data_file = self.data_dir / k
        if data_file.is_file():
            return _read_file(data_file)
        owner = self.archive_owner.get(k)
        if owner:
            mod_dir = self.folder_by_lc[owner[0].lower()]
            ar = self._open(mod_dir / owner[1])
            if ar and ar.valid:
                blob, _err = ar.read_entry(k)
                return blob
        base = self.base_index.get(k)
        if base:
            ar = self.base_parsed.get(base)
            if ar is None:
                archive_path = self.data_dir / base
                if not archive_path.is_file():
                    return None
                ar = Ba2Archive(archive_path)
                self.base_parsed[base] = ar
            if ar.valid:
                blob, _err = ar.read_entry(k)
                return blob
        return None

    def mods_shipping(self, key: str) -> set[str]:
        """Every enabled mod that ships this path, loose or archived."""
        owners = set(self.loose_owners.get(norm_key(key), ()))
        ao = self.archive_owner.get(norm_key(key))
        if ao:
            owners.add(ao[0])
        return owners


def _read_file(path: Path) -> bytes | None:
    try:
        return path.read_bytes()
    except OSError:
        return None


# ── NIF / .mat helpers (shared by the audit and catalog modes) ────────
_PRINTABLE = re.compile(rb"[ -~]{4,}")


def material_paths(nif: bytes) -> list[str]:
    """Every .mat path a NIF names, from its header-string runs."""
    seen: set[str] = set()
    out: list[str] = []
    for m in _PRINTABLE.finditer(nif):
        s = m.group().decode("latin-1")
        if not s.lower().endswith(".mat"):
            continue
        if s not in seen:
            seen.add(s)
            out.append(s)
    return out


def _quoted_after(low: str, key_end: int) -> str:
    """The JSON string value that follows a key, or ""."""
    p = key_end
    while p < len(low) and low[p] in " \t\r\n":
        p += 1
    if p >= len(low) or low[p] != ":":
        return ""
    p += 1
    while p < len(low) and low[p] in " \t\r\n":
        p += 1
    if p >= len(low) or low[p] != '"':
        return ""
    p += 1
    raw: list[str] = []
    while p < len(low) and low[p] != '"':
        if low[p] == "\\" and p + 1 < len(low):
            raw.append(low[p + 1])
            p += 2
        else:
            raw.append(low[p])
            p += 1
    return "".join(raw)


def mat_albedo(blob: bytes) -> str:
    """The albedo path of a JSON .mat, normalized; "" if none.

    Mirrors LiveModel::FindMatAlbedo: prefer the Summary/Albedo/File block;
    Blender exports that omit Summary still carry `"FileName": "...color.DDS"`
    under BSMaterial::MRTextureFile, so fall back to the first colour-ish
    texture filename.
    """
    try:
        text = blob.decode("utf-8")
    except UnicodeDecodeError:
        return ""
    low = text.lower()
    alb = low.find('"albedo"')
    if alb >= 0:
        brace = low.find("{", alb)
        if brace >= 0 and brace - alb <= 64:
            depth = 0
            end = -1
            for i in range(brace, min(len(low), brace + 8192)):
                if low[i] == "{":
                    depth += 1
                elif low[i] == "}":
                    depth -= 1
                    if depth == 0:
                        end = i
                        break
            if end > 0:
                f = low.find('"file"', brace)
                while 0 <= f < end:
                    path = norm_key(_quoted_after(low, f + 6))
                    if path.endswith(".dds"):
                        return path
                    f = low.find('"file"', f + 6)
    # No Summary: MRTextureFile component list. Index 0 is the albedo, but the
    # export order is not guaranteed, so pick by filename token.
    suffix = ""
    for match in re.finditer(r'"filename"', low):
        path = norm_key(_quoted_after(low, match.end()))
        if not path.endswith(".dds"):
            continue
        base = path.rsplit("/", 1)[-1]
        if any(tok in base for tok in ("color", "colour", "albedo", "diffuse")):
            return path
        # SGTexSet sheets call the albedo `_d.DDS` (`_n`, `_r`, `_l` siblings).
        # Second choice only, so a colour-word map always wins.
        if not suffix and base.endswith("_d.dds"):
            suffix = path
    return suffix


def log_material_map(path: Path | str) -> dict[str, set[str]]:
    """formID -> material paths, from the offline GPU texturing lines.

    `texturing 'direct:<name> [FormID XXXXXXXX]#hash'` opens a per-item
    range block; each following `range N '<mat>'` names a material the
    preview actually drew. Unlike the NativeModel tree this carries the
    form ID, so the association is exact.
    """
    out: dict[str, set[str]] = {}
    form = ""
    tex = re.compile(r"texturing 'direct:.*\[FormID ([0-9A-Fa-f]{8})\]")
    rng = re.compile(r"range \d+ '([^']+\.mat)'")
    try:
        lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return out
    for line in lines:
        m = tex.search(line)
        if m:
            form = m.group(1).upper()
            out.setdefault(form, set())
            continue
        if not form:
            continue
        m = rng.search(line)
        if m:
            out[form].add(norm_key(m.group(1)))
    return out


def log_nif_map(path: Path | str) -> dict[str, set[str]]:
    """formID -> NIF paths, from SFShopExplorer.log preview requests.

    Each `direct: request '<name> [FormID XXXXXXXX]'` opens a segment; the
    NativeModel assembly lines inside it name the meshes that were loaded.
    """
    out: dict[str, set[str]] = {}
    form = ""
    req = re.compile(r"direct: request '.*\[FormID ([0-9A-Fa-f]{8})\]'")
    nif = re.compile(r"'((?:[A-Za-z0-9_.\- ]+\\)*[A-Za-z0-9_.\- ]+\.nif)'", re.IGNORECASE)
    try:
        lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return out
    for line in lines:
        m = req.search(line)
        if m:
            form = m.group(1).upper()
            out.setdefault(form, set())
            continue
        if not form:
            continue
        if "MODEL PATH =" in line or "connect root" in line or "assembly geometry path=" in line:
            for n in nif.findall(line):
                key = nif_key(n)
                if key:
                    out[form].add(key)
    return out
