"""Why does this item look this colour? One table, no game launch.

Every colour question on this shelf so far has been answered by reading the
session log after a playtest, which costs a game launch per iteration and only
covers the items that were actually opened. This tool answers the same question
offline, for a whole list of items at once:

    python aid_colour_audit.py --aid                  # every Aid row in the dump
    python aid_colour_audit.py --shelf books          # the Books shelf (BOOK)
    python aid_colour_audit.py --item "Erdebrau Light Can" --explain
    python aid_colour_audit.py --form 001FC9C1 --tsv audit.tsv
    python aid_colour_audit.py --aid --html aid.html  # one page, swatches, filter box

The shelves are the shop's own tabs, not keywords: ShopExplorer maps
BrowseType::kAid to ALCH, kAmmo to AMMO, kBook to BOOK and kMisc to MISC, and
the catalog dump's `type` column carries exactly that form type.

It resolves, per ITEM: the record's MODL model, the NIF's own materials, the
item's layered material swap (materialswaps.tsv), and then per MATERIAL: the
database's override colour and layer kind, the material's own texture set (with
the folder fallback and the crate paint rule), the upload colour decision, and
the blend-alpha decision - the same rules the plugin ships, mirrored here.

The mirror is deliberate, not a reimplementation of the idea: the docstrings
name the C++ function each rule copies, and test_aid_colour_audit.py fails when
a constant in the source and the constant here drift apart. What this tool
cannot see is the fuzzy texture scorer (FindDiffuse), so a material with no
authored set is reported as "stand-in" rather than with the atlas it may land -
which is exactly the distinction the colour rules turn on.

Sources, all read-only:
  * the record walk of a plugin (MODL / MOLM) - shared with gen_material_swaps
  * the NIF geometry walk (node name + material per shape) - nif_uv.parse_shapes
  * materialsbeta.cdb through cdb_alpha_replica (isset components) and
    material_sweep.build_cdb_index (texture sets)
  * the archives on this machine, base game plus the MO2 mod folders
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "ba2_audit"))

import cdb_alpha_replica as replica  # noqa: E402
from material_sweep import build_cdb_index  # noqa: E402
from ba2lib import Ba2Archive  # noqa: E402
from nif_uv import Nif, parse_shapes  # noqa: E402
import gen_material_swaps as gms  # noqa: E402

CDB = HERE / "extracted" / "materialsbeta.cdb"
CREATIONS = HERE / "extracted" / "creations"
CACHE = HERE / "extracted" / "cdb_rules_cache.json"
GAME_DATA = Path(r"B:\SteamLibrary\steamapps\common\Starfield\Data")
MODS_ROOT = Path(r"B:\MO2\Starfield\mods")
SWAPS = Path(r"B:\MO2\Starfield\mods\DevilzDad's ESM Shop Mod Explorer\SFSE\Plugins"
             r"\SFShopExplorerAssets\models\materialswaps.tsv")
CATALOGS = [
    Path(r"C:\Users\mrlma\Documents\My Games\Starfield\SFSE\Logs\SFDevilzDump-catalog.csv"),
    TOOLS.parent.parent / "SFDevilzDump-all-forms.csv",
]

# Shelf name -> catalog form type, mirroring BrowseTypeForm (src/ShopExplorer.h).
# test_aid_colour_audit.py reads the C++ switch and fails when one drifts.
SHELVES = {
    "aid": ("ALCH",),
    "ammo": ("AMMO",),
    "books": ("BOOK",),
    "misc": ("MISC",),
}

# ── Rule constants, mirrored from src/OfflineModel.cpp and src/LiveModel.cpp ──
# test_aid_colour_audit.py asserts each of these against the C++ source.
DECAL_TOKENS = ("decal", "label", "letter", "symbol", "poster", "overlay", "blank")
SHEER_TOKENS = ("glass", "clear", "translucent", "transpar")

MIN_SURFACE_ALPHA = 0.05
# A name token says "this is glass", not how much light the body stops. The
# blend shader now derives coverage from Fresnel (4% head-on, 1.0 at the
# silhouette), so a stand-in that is clear is 0.0 - the 0.30 this replaced
# composited a closed shell to 51% of a pale film over the whole item (jar,
# wine bottle, Pils can; see kAuthoredSheerAlpha in OfflineModel.cpp).
TOKEN_FALLBACK_ALPHA = 0.00
NEAR_WHITE_MAX = 0.97
NEAR_WHITE_SPREAD = 0.20
GRUNGE_PREFIX = "textures/common/grunge/"
GRUNGE_SPREAD = 0.30

# Glass the database does not mark at all (see kAuthoredSheer in
# OfflineModel.cpp). Alien Genetic Material's jar body is the measured case: an
# empty database set, so it drew as a solid grey tub with the specimen hidden.
# A named container keeps a small presence (two walls of 0.05 over a 0.70 body
# is ~10% coverage - the game's own top-down render of this jar adds ~10/255
# over its backdrop), while the generic name-token glass is clear. See the
# kAuthoredSheerAlpha comment in OfflineModel.cpp.
AUTHORED_SHEER = ("isotopic_coolant_container",)
AUTHORED_SHEER_ALPHA = 0.05
# The tint that goes with it (see kAuthoredGlassTint in OfflineModel.cpp). The
# database stores no colour for this material, so the jar fell back to the
# generic pale tint and read as a blue-grey tub beside the game's own render,
# which is green. Measured from the 2026-10-22 12:04 inventory capture: the
# left glass wall averages (27,43,44) over an (8,23,31) backdrop - it adds
# (19,20,13)/255, green above red, blue lowest - while the pale tint added
# (35,36,37). Each channel is the pale tint scaled by that ratio. The jar's
# fluid shell is drawn additively in the same colour.
AUTHORED_GLASS_TINT = (0.34, 0.37, 0.25)

# The clear-glass family the database leaves completely empty (see
# IsFlatClearGlassMat in OfflineModel.cpp): `Common\Glass\GlassCleanTransparent01`
# and its `_double` sibling, plus the ScienceGlass `..._Base_Green` fills. The
# token scan calls a glass-named material a clear shell with alpha 0, which is
# right for a printed can's sleeve and wrong for a beaker - a beaker IS the
# glass, so its preview drew nothing at all (2026-10-24 contact sheet: tiles
# 00265ADA Beaker and 00265ADE Graduated Cylinder were blank). The family keeps
# the jar's measured per-wall transmission and, where the database stores no
# colour of its own, the engine's pale glass tint.
FLAT_CLEAR_GLASS_FOLDERS = ("/common/glass/", "/setdressing/scienceglass/")
FLAT_CLEAR_GLASS_STEM = "glasscleantransparent"
FLAT_GLASS_TINT = (0.62, 0.66, 0.72)

# Names the material itself as see-through. The override alpha is surface
# opacity only when one of these is in the name (the C++ gate); on the Aid and
# Ammo shelves that leaves exactly one material, GlassHelmets01 at 0.686.
SHEER_NAME_TOKENS = ("glass", "visor", "lens", "translu", "transpar", "clear")

# The consumable folders whose override alpha is a layer weight, mirroring the
# branch in MaterialBlendAlpha. The to-go cup's base and lid store 0.607843 and
# the engine draws them opaque, but they are under setdressing/tranquilitea -
# outside these marks - so the rule that keeps them opaque is the layer kind,
# not this list. Keep the list exactly as the C++ has it.
CONSUMABLE_MARKS = ("/fooddrink_singleitems/", "/drugs_or_medical/")

# Hand-authored texture routes in FindDiffuse, keyed by normalized material.
# A route beats the database, so the audit must show it or it would report a
# material as flat when the preview actually draws art on it.
EXACT_ROUTES = {
    "materials/setdressing/tranquilitea/tranquilitea_togocup/tranquilitea_togocup_sleeve01.mat":
        "textures/setdressing/tranquilitea/tranquilitea_logo01_color.dds",
    "materials/items/fooddrink_singleitems/erdebrau_beers/erdebrau_light_decal.mat":
        "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau light_decal_color.dds",
    "materials/items/fooddrink_singleitems/erdebrau_beers/erdebrau_pilsner_decal.mat":
        "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau pilsner_decal_color.dds",
    # The Lager can carries one wrap map and one aluminium map instead of a
    # label; neither Lager material has a CDB record at all.
    "materials/items/fooddrink_singleitems/erdebrau_beers/erdebrau_lager_decal.mat":
        "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau lager_color.dds",
    "materials/items/fooddrink_singleitems/erdebrau_beers/erdebrau_lager_metal.mat":
        "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau lager_aluminium_color.dds",
}

# ApplyCaptainNoteMaterialSwaps in NativeModel.cpp: the note records' LMSW keys
# name another model's material, so the plugin carries these by hand.
AUTHORED_SWAPS = {
    # Keys are `book_key` spellings (lowercase, backslashes, no materials\);
    # the first draft used forward slashes and silently never matched.
    0x000A9010: {
        "setdressing\\books\\genbooka01_booka01.mat": "Materials\\SetDressing\\Books\\GenBookA01_BookA04.mat",
        "setdressing\\books\\genbookb01_bookb01.mat": "Materials\\SetDressing\\Books\\GenBookB01_BookB04.mat",
        "setdressing\\books\\decalletterlineoverlay01_linesstripes.mat":
            "Materials\\SetDressing\\Books\\DecalLetterLineOverlay01_PatternF.mat",
    },
    0x000A9012: {
        "setdressing\\books\\genbookb01_bookb01.mat": "Materials\\SetDressing\\Books\\GenBookB01_BookB04.mat",
        "setdressing\\books\\decalletterlineoverlay01_linesstripes.mat":
            "Materials\\SetDressing\\Books\\DecalLetterLineOverlay01_PatternF.mat",
    },
}

# FindDiffuse's node-scoped routes: the material, the node substring that must
# match, and the atlas bound.
BOOK_ROUTES = (
    ("materials/setdressing/books/genbookb01_bookb04.mat", "book01",
     "textures/setdressing/books/genbooka01_color.dds"),
    ("materials/setdressing/books/genbookb01_pagesb01.mat", "book01",
     "textures/setdressing/books/genbooka01_color.dds"),
    ("materials/setdressing/books/decalletterlineoverlay01_patternf.mat", "book01",
     "textures/effects/gobos/lift02_color.dds"),
)


def book_key(mat: str) -> str:
    """Mirror of NormalizeBookMaterialPath: lowercase, backslashes, no root."""
    s = mat.lower().replace("/", "\\")
    return s[10:] if s.startswith("materials\\") else s


def exact_route(mat: str, node: str) -> str | None:
    """The hand-authored route FindDiffuse would take for this material."""
    key = norm(mat)
    if key in EXACT_ROUTES:
        return EXACT_ROUTES[key]
    n = node.lower()
    for route_mat, node_part, atlas in BOOK_ROUTES:
        if key == route_mat and node_part in n:
            return atlas
    return None
NAMED_COLORS = {
    "white": (0.86, 0.87, 0.84), "silver": (0.72, 0.73, 0.76),
    "grey": (0.55, 0.56, 0.57), "gray": (0.55, 0.56, 0.57),
    "green": (0.42, 0.55, 0.45), "red": (0.62, 0.22, 0.20),
    "bronze": (0.60, 0.44, 0.26), "gold": (0.78, 0.66, 0.32),
    "black": (0.17, 0.17, 0.18), "blue": (0.30, 0.38, 0.60),
    "yellow": (0.80, 0.72, 0.30), "orange": (0.78, 0.48, 0.20),
    "brown": (0.42, 0.30, 0.20), "tan": (0.76, 0.70, 0.58),
    "beige": (0.76, 0.70, 0.58), "soil": (0.34, 0.26, 0.18),
    "dirt": (0.34, 0.26, 0.18), "earth": (0.34, 0.26, 0.18),
    "purple": (0.42, 0.32, 0.55),
}


def norm(path: str) -> str:
    """Lowercase, slash-normalized, no leading slash, no geometry suffix."""
    s = path.lower().replace("\\", "/")
    while s.startswith("/"):
        s = s[1:]
    colon = s.rfind(":")
    if colon > 0 and s[colon + 1:].isdigit():
        s = s[:colon]
    return s


def stem_of(path: str) -> str:
    s = norm(path)
    return s.rsplit("/", 1)[-1]


def bare_of(stem: str) -> str:
    return stem[:-4] if stem.endswith(".mat") else stem


def base_of(stem: str) -> str:
    """`x_01` -> `x`, the numbered-family fallback (FindCdbMaterialLocked)."""
    base = bare_of(stem)
    while base and base[-1].isdigit():
        base = base[:-1]
    if base.endswith("_"):
        base = base[:-1]
    return base


# The colour word a material variant is named after, mirrored from
# ColourWordSplit in LiveModel.cpp (whose list in turn mirrors FlatBaseColor's
# table in OfflineModel.cpp).
COLOUR_WORDS = ("white", "silver", "grey", "gray", "green", "red", "bronze", "gold",
                "black", "blue", "yellow", "orange", "brown", "tan", "beige", "purple")


def colour_family_base(stem: str) -> str | None:
    """`crafted_inhaileryellow` -> `crafted_inhailer`; None when no suffix."""
    s = bare_of(stem)
    for word in COLOUR_WORDS:
        if len(s) > len(word) + 2 and s.endswith(word):
            base = s[: -len(word)].rstrip("_")
            if len(base) >= 3:
                return base
    return None


def mat_candidates(mat_path: str) -> list[str]:
    """The key spellings the plugin's lookups try, in its own order."""
    p = norm(mat_path)
    stem = stem_of(p)
    out = [p]
    if not p.startswith("data/"):
        out.append("data/" + p)
    for key in (stem, bare_of(stem), base_of(stem)):
        if key and key not in out:
            out.append(key)
    return out


def swap_key(path: str) -> str:
    """Mirror of SwapKey in LiveModel.cpp."""
    s = norm(path)
    if s.startswith("data/"):
        s = s[5:]
    if s.startswith("materials/"):
        s = s[10:]
    return bare_of(s)


# ── CDB access ────────────────────────────────────────────────────────────────

class Cdb:
    """Override rgba + layer kind + texture set, exactly as the plugin reads it.

    The plugin mirrors the game here: `EnsureCdbIndex` indexes the base
    database AND every `materials/creations/<name>/materialsbeta.cdb` it can
    find, first writer wins (`IndexCdbBlob` only ever fills a gap), so a
    Creation's own materials are answered by the Creation's own database.
    The audit used to read only the base blob, which is why every Creation row
    on the Misc shelf reported "no record" - action-figure packaging and
    kibble are indexed by SFBGS00D and SFBGS050 respectively, and both were
    being judged as if the game had never heard of them. The extracted
    fixtures in `extracted/creations/` are the shipped bytes
    (`extract_creation_cdb.py`); when they are absent the audit stays
    base-only rather than guessing.
    """

    CACHE_VERSION = 5  # 5: planet `_terrain` layers index as their material

    def __init__(self, path: Path = CDB, creations: Path | None = None) -> None:
        self.path = path
        self.creations = sorted((creations or CREATIONS).glob("*.cdb"))
        self.rgba: dict[str, tuple] = {}
        self.kinds: dict[str, int] = {}
        self.sets: dict[str, dict] = {}
        if not path.is_file():
            return
        if not self._load_cache():
            self.rgba = replica.scan(str(path))
            self.kinds = replica.scan_kinds(str(path))
            self.sets = build_cdb_index(path)
            for extra in self.creations:
                for key, value in build_cdb_index(extra).items():
                    self.sets.setdefault(key, value)
                for key, value in replica.scan(str(extra)).items():
                    self.rgba.setdefault(key, value)
                for key, value in replica.scan_kinds(str(extra)).items():
                    self.kinds.setdefault(key, value)
            self._save_cache()

    def _stamp(self) -> str:
        st = self.path.stat()
        extras = ";".join(f"{p.name}:{p.stat().st_size}" for p in self.creations)
        return f"{self.CACHE_VERSION}:{st.st_size}:{int(st.st_mtime)}:{extras}"

    def _load_cache(self) -> bool:
        try:
            blob = json.loads(CACHE.read_text(encoding="utf-8"))
        except Exception:
            return False
        if blob.get("stamp") != self._stamp():
            return False
        self.rgba = {k: tuple(v) for k, v in blob["rgba"].items()}
        self.kinds = {k: int(v) for k, v in blob["kinds"].items()}
        self.sets = blob["sets"]
        return True

    def _save_cache(self) -> None:
        try:
            CACHE.write_text(json.dumps({
                "stamp": self._stamp(),
                "rgba": {k: list(v) for k, v in self.rgba.items()},
                "kinds": self.kinds,
                "sets": self.sets,
            }), encoding="utf-8")
        except Exception:
            pass  # a cache is a convenience; never fail the audit for it

    # -- the plugin's own lookups ------------------------------------------
    def override(self, mat: str):
        for key in mat_candidates(mat):
            if key in self.rgba:
                return self.rgba[key], self.kinds.get(key)
        return None, None

    def layer_kind(self, mat: str):
        """The record's TextureSetKind, matched like override(). Missing is 0."""
        for key in mat_candidates(mat):
            if key in self.rgba:
                return self.kinds.get(key)
        return None

    def tint(self, mat: str):
        """Mirror of FindCdbTint: the rgb, or None when it says nothing."""
        ov, _ = self.override(mat)
        if not ov:
            return None
        mx, mn = max(ov[:3]), min(ov[:3])
        if (mx >= NEAR_WHITE_MAX and mx - mn < NEAR_WHITE_SPREAD) or mx <= 0.0:
            return None
        return ov[:3]

    def alpha(self, mat: str):
        ov, kind = self.override(mat)
        return (ov[3] if ov else None), kind

    def _family_with_colour(self, stem: str) -> str | None:
        """The sibling record that actually holds this family's colour map.

        A numbered colour variant is spelled `<base>_<colour><nn>`
        (`plasticwornscratched02_grey02`), so the trailing number has to go
        before the colour word is read; the old order never matched those and
        the variant drew flat. Only a family that names a colour counts - a
        family that is itself flat has nothing to lend, and claiming it would
        replace an honest "no record" with a misleading hit.
        """
        numbered = stem
        while numbered and numbered[-1].isdigit():
            numbered = numbered[:-1]
        for candidate in (colour_family_base(stem), colour_family_base(numbered)):
            if candidate and self.sets.get(candidate, {}).get("color"):
                return candidate
        return None

    def own_set(self, mat: str):
        """Mirror of FindCdbMaterialLocked, including its fallbacks.

        The texture-set lookup is path, then the stem WITHOUT its extension,
        then the numbered family base - exactly CdbPathParts in
        src/LiveModel.cpp, which erases `.mat` before the stem lookup.
        Keeping the extension here used to send `MetalBronze02.mat` to the
        database's `metalbronze02.mat_TextureSet1`, a different record whose
        set carries no maps, and the search stopped there: the material drew
        flat while the game and the plugin both resolve
        `metalbronze02_TextureSet1` (MetalDullWorn01_color.dds). Every material
        whose name is also a texture-set stem with a `.mat` variant - metals,
        plastics, the citicard parts - was one of those silent flats.
        """
        p = norm(mat)
        stem = bare_of(stem_of(p))
        keys = [p]
        if not p.startswith("data/"):
            keys.append("data/" + p)
        keys += [k for k in (stem, base_of(stem)) if k]
        found = None
        for key in keys:
            if key in self.sets:
                found = self.sets[key]
                break
        if found is None:
            # A colour-named variant borrows its family's art (the chem
            # inhaler's two case halves are `Crafted_InHailer` and
            # `Crafted_InHailerYellow`, and only the base has a record).
            family = self._family_with_colour(stem)
            if family:
                return self.sets[family], f"colour-family '{family}'"
            return None, "no record"
        if found.get("color") or found.get("opacity") or found.get("maps"):
            return found, "own set"
        # Empty set: the same colour-family rule as the no-record arm, because a
        # numbered colour variant records its own (empty) set while the art
        # stays with the base material the variant layers over.
        # `PlasticWornScratched02_Grey02` was drawing as a flat grey wash while
        # `plasticwornscratched02` holds paintmetalscratched01_color.dds.
        family = self._family_with_colour(stem)
        if family:
            return self.sets[family], f"colour-family '{family}'"
        # Empty set: the plugin falls back to the material's own folder, and to
        # the shelf's shared paint map for the ammo crates.
        if "setdressing/ammobox01/" in p:
            return {"color": "textures/common/paint/paint01_color.dds"}, "crate paint map"
        parts = p.split("/")
        if len(parts) >= 2:
            folder = parts[-2]
            for key in (folder, "data/" + folder):
                if key in self.sets and self.sets[key].get("color"):
                    return self.sets[key], f"folder '{folder}'"
        return found, "empty set"


# ── Archives and plugins ──────────────────────────────────────────────────────

class TexInfo:
    """A texture payload with the geometry the mean decoder needs."""

    __slots__ = ("payload", "width", "height", "dxgi")

    def __init__(self, payload: bytes, width: int, height: int, dxgi: int) -> None:
        self.payload = payload
        self.width = width
        self.height = height
        self.dxgi = dxgi


def dds_image(blob: bytes) -> tuple[bytes, int, int, int] | None:
    """Split a full DDS container into (payload, width, height, dxgi).

    A texture normally arrives from a texture BA2 with its dimensions already
    in the index. Mods also pack DDS files as general archive entries (and as
    loose files), where the dimensions and DXGI format live in the container
    header. Returning those bytes as `TexInfo(blob, 0, 0, 0)` made every such
    map look empty to the renderer, so the tile fell back to a flat colour.
    DX10 stores the format in its extension header; DXT1/DXT3/DXT5 map to the
    legacy BC1/BC3 values the decoder already understands.
    """
    if len(blob) < 128 or blob[:4] != b"DDS ":
        return None
    height, width = struct.unpack_from("<II", blob, 12)
    fourcc = blob[84:88]
    if fourcc == b"DX10":
        if len(blob) < 148:
            return None
        return blob[148:], width, height, struct.unpack_from("<I", blob, 128)[0]
    dxgi = {b"DXT1": 71, b"DXT3": 77, b"DXT5": 77, b"BC5U": 83,
            b"ATI2": 83}.get(fourcc)
    if dxgi is None:
        return None
    return blob[128:], width, height, dxgi


class AssetStore:
    """Loose files first (MO2 VFS order), then archive name tables."""

    def __init__(self, roots: list[Path], mods_root: Path | None, verbose: bool) -> None:
        self.roots = [r for r in roots if r.is_dir()]
        self.mod_dirs = []
        if mods_root and mods_root.is_dir():
            self.mod_dirs = [d for d in sorted(mods_root.iterdir()) if d.is_dir()]
        self.archives: dict[str, Ba2Archive] = {}
        self.name_owner: dict[str, str] = {}
        self.indexed = False
        self.verbose = verbose
        # A bulk scan reads thousands of NIFs; probing 549 mod folders per read
        # is what made it slow. index_loose() builds the same lookup once.
        self.loose_index: list[dict[str, Path]] | None = None

    @staticmethod
    def keys(rel: str) -> list[str]:
        """A MODL string is `Items\\...\\x.nif`; the archives key it under
        `meshes/<path>`. Try both, plus the Data-relative spelling."""
        p = rel.replace("\\", "/").lower().lstrip("/")
        out = [p]
        if not p.startswith("meshes/"):
            out.append("meshes/" + p)
        if p.startswith("data/"):
            out.append(p[5:])
        return out

    def index_loose(self) -> None:
        """Index every loose file once, preserving roots-then-mods precedence.

        The workbench reads a handful of files, but a shelf scan reads thousands
        and the 549 MO2 mod folders make a per-read probe the dominant cost.
        Indexed per root so a later mod still never wins over an earlier one.
        """
        if self.loose_index is not None:
            return
        self.loose_index = []
        for root in self.roots + self.mod_dirs:
            files: dict[str, Path] = {}
            for p in root.rglob("*"):
                if not p.is_file():
                    continue
                rel = p.relative_to(root).as_posix().lower()
                files.setdefault(rel, p)
            self.loose_index.append(files)

    def _loose(self, keys: list[str]) -> tuple[str, bytes] | None:
        if self.loose_index is not None:
            for files in self.loose_index:
                for sub in ("", "data/"):
                    for key in keys:
                        p = files.get(sub + key)
                        if p is not None:
                            return key, p.read_bytes()
            return None
        for root in self.roots + self.mod_dirs:
            for sub in ("", "Data"):
                for key in keys:
                    p = root / sub / key
                    if p.is_file():
                        return key, p.read_bytes()
        return None

    def _build_index(self) -> None:
        roots = self.roots + self.mod_dirs
        arcs: list[Path] = []
        for root in roots:
            arcs += sorted(root.glob("*.ba2"))
            if root in self.mod_dirs:
                arcs += sorted(root.glob("*.ba2"))
        for p in arcs:
            if p.name in self.archives:
                continue
            ar = Ba2Archive(p)
            if not ar.valid:
                continue
            self.archives[p.name] = ar
            for name in list(ar.entries) + list(ar.tex_files):
                self.name_owner.setdefault(name, p.name)
        self.indexed = True
        if self.verbose:
            print(f"[index] {len(self.archives)} archive(s), "
                  f"{len(self.name_owner)} named entries", file=sys.stderr)

    def read(self, rel: str) -> bytes | None:
        keys = self.keys(rel)
        loose = self._loose(keys)
        if loose:
            return loose[1]
        if not self.indexed:
            self._build_index()
        for key in keys:
            owner = self.name_owner.get(key)
            if owner:
                blob, _err = self.archives[owner].read_entry(key)
                if blob:
                    return blob
        for name, ar in self.archives.items():
            for key in keys:
                if key in ar.entries:
                    blob, _err = ar.read_entry(key)
                    if blob:
                        return blob
        return None

    def texture(self, key: str) -> TexInfo | None:
        """A texture as payload + size + DXGI format, from loose files or a
        BA2's texture index (whose entries live in their own descriptor list
        and need read_texture, not read_entry)."""
        keys = self.keys(key)
        loose = self._loose(keys)
        if loose:
            blob = loose[1]
            image = dds_image(blob)
            if image is not None:
                payload, width, height, dxgi = image
                return TexInfo(payload, width, height, dxgi)
            return TexInfo(blob, 0, 0, 0)
        if not self.indexed:
            self._build_index()
        for key2 in keys:
            owner = self.name_owner.get(key2)
            if not owner:
                continue
            ar = self.archives[owner]
            tf = ar.tex_files.get(key2)
            if tf is not None:
                payload, _err = ar.read_texture(key2)
                if payload:
                    return TexInfo(payload, tf.width, tf.height, tf.dxgi)
            blob, _err = ar.read_entry(key2)
            if blob:
                # A mod may pack a full DDS as a general entry. Read its
                # header instead of handing the renderer an untyped blob.
                image = dds_image(blob)
                if image is not None:
                    payload, width, height, dxgi = image
                    return TexInfo(payload, width, height, dxgi)
                return TexInfo(blob, 0, 0, 0)
        return None


class Plugins:
    """One record walk per plugin, indexed by file-local form id."""

    def __init__(self, store: AssetStore) -> None:
        self.store = store
        self.by_plugin: dict[str, dict[int, dict]] = {}

    def _plugin_bytes(self, plugin: str) -> bytes | None:
        for cand in (plugin, plugin.replace("/", "\\")):
            blob = self.store.read(cand)
            if blob:
                return blob
        for ext in (".esm", ".esp", ".esl"):
            blob = self.store.read(plugin + ext)
            if blob:
                return blob
        return None

    def record(self, plugin: str, form_id: int) -> dict | None:
        table = self.by_plugin.get(plugin)
        if table is None:
            table = self.by_plugin[plugin] = self._scan(plugin)
        return table.get(form_id & 0xFFFFFF)

    def _scan(self, plugin: str) -> dict[int, dict]:
        blob = self._plugin_bytes(plugin)
        if not blob:
            print(f"[warn] plugin not found: {plugin}", file=sys.stderr)
            return {}
        out: dict[int, dict] = {}
        end = gms.RECORD_HEADER + gms._u32(blob, 4)
        for _tag, rec, start, stop in gms.iter_records(blob, end, len(blob)):
            model = ""
            molm: list[int] = []
            for sub_tag, payload in gms.subrecords(blob, start, stop):
                if sub_tag == b"MODL":
                    model = payload.split(b"\0")[0].decode("latin-1")
                elif sub_tag == b"MOLM":
                    molm = gms.molm_refs(blob, start, stop)
                    break
            if model or molm:
                out[gms._u32(blob, rec + 12) & 0xFFFFFF] = {"model": model, "molm": molm}
        return out


class Swaps:
    """materialswaps.tsv, keyed the way MaterialSwapFor keys it."""

    def __init__(self, path: Path | None) -> None:
        self.by_form: dict[int, list[tuple[str, str]]] = {}
        if path and path.is_file():
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                if not line or line.startswith("#"):
                    continue
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 3:
                    continue
                try:
                    form = int(parts[0], 16)
                except ValueError:
                    continue
                key, over = swap_key(parts[1]), parts[2]
                if key and over and key != swap_key(over):
                    self.by_form.setdefault(form, []).append((key, over))

    def apply(self, form_id: int, mat: str) -> str | None:
        for key, over in self.by_form.get(form_id, ()):
            if key == swap_key(mat):
                return over if swap_key(over).startswith("materials/") else "Materials\\" + over
        return None


# ── Rules ─────────────────────────────────────────────────────────────────────

def is_decal_sheet(mat: str) -> bool:
    """Mirror of IsDecalSheet (OfflineModel.cpp)."""
    ml = mat.lower()
    return any(t in ml for t in DECAL_TOKENS)


def is_crafted_drink_bag(mat: str) -> bool:
    """Mirror of IsCraftedDrinkBagMaterial: the pouch folder."""
    return "crafteddrinkbags" in norm(mat) or "drinkbag" in norm(mat)


def is_flat_clear_glass(mat: str) -> bool:
    """Mirror of IsFlatClearGlassMat: the art-less clear-glass family.

    Both the folder and the stem are required: "Glass" alone also appears
    inside `ScienceGlassPlastic02`, which is opaque plastic.
    """
    ml = norm(mat)
    return (any(folder in ml for folder in FLAT_CLEAR_GLASS_FOLDERS)
            and FLAT_CLEAR_GLASS_STEM in ml)


# The asset-shelf folders whose name is a namespace, not a material word (see
# kNamespaceFolders in OfflineModel.cpp). `SetDressing\ScienceGlass` holds the
# plastic caps and the vial liquid beside its glass fills, and every file
# repeats the shelf name, so `ScienceGlassPlastic02` is the material
# `Plastic02`. The list stays short because the same structure proves the
# opposite elsewhere: `Common\Glass\GlassDirtyHeavyFrost01` repeats its folder
# AND IS glass, so stripping there would make a real frosted pane opaque.
NAMESPACE_FOLDERS = ("scienceglass",)


def own_name(mat: str) -> str:
    """Mirror of MaterialOwnName: the stem, minus a leading namespace repeat.

    The sheer scan read the whole path, so the folder counted as part of the
    name. On `ScienceGlass` the shelf name repeats into its files and the
    "Glass" of that repetition is a word start - which made
    `ScienceGlassPlastic02` (the plastic caps) and `ScienceGlassLiquid01` (the
    vial liquid) clear shells at alpha 0. Only a LEADING repeat is dropped, only
    for a folder on NAMESPACE_FOLDERS, and only when something follows it.
    """
    s = mat.replace("/", "\\")
    slash = s.rfind("\\")
    stem = s if slash < 0 else s[slash + 1:]
    if len(stem) > 4 and stem[-4:].lower() == ".mat":
        stem = stem[:-4]
    if slash <= 0:
        return stem
    prev = s.rfind("\\", 0, slash)
    folder = (s[:slash] if prev < 0 else s[prev + 1:slash]).lower()
    if (folder in NAMESPACE_FOLDERS and len(stem) > len(folder)
            and stem[:len(folder)].lower() == folder):
        at = len(folder)
        while at < len(stem) and stem[at] in "_-":
            at += 1
        if at < len(stem):
            stem = stem[at:]
    return stem


def has_name_word(mat: str, word: str) -> bool:
    """A material NAME word, not a substring of one (MaterialNameHasWord).

    The sheer scan used `word in path`, so `Hourglass_Case` and
    `Hourglass_Sand` were both read as glass through the "glass" inside
    "hourglass" - the Misc audit of 2026-10-23 found the hourglass preview
    empty because of it. A word starts at the path start, after any
    non-alphanumeric separator, or at a lower->upper camel hump; the case is
    read before lowercasing so `ScienceGlassLiquid` still matches while
    `PlasticFiberglass01` does not.
    """
    s = mat.replace("/", "\\")
    low = s.lower()
    needle = word.lower()
    i = low.find(needle)
    while i >= 0:
        if i == 0 or not s[i - 1].isalnum():
            return True
        if s[i].isupper() and (s[i - 1].islower() or s[i - 1].isdigit()):
            return True
        i = low.find(needle, i + 1)
    return False


def blend_alpha(mat: str, alpha: float | None, kind: int | None) -> tuple[float, bool, str]:
    """Mirror of MaterialBlendAlpha. Returns (alpha, translucent, reason)."""
    ml = norm(mat)
    if is_decal_sheet(mat):
        return 1.0, False, "decal sheet: coverage from its own maps"
    if alpha is not None and 0.0 < alpha < 0.995:
        if kind in (1, 2):
            return 1.0, False, f"opaque: {alpha:.3f} is a {'Multiply' if kind == 1 else 'Lerp'} layer weight"
        sheer = any(has_name_word(mat, t) for t in SHEER_TOKENS)
        garment = "clothes/" in ml
        consumable = any(d in ml for d in CONSUMABLE_MARKS)
        if (garment or consumable) and not sheer:
            what = "garment" if garment else "consumable"
            return 1.0, False, f"opaque: {alpha:.3f} is a {what} layer weight"
        if alpha >= MIN_SURFACE_ALPHA:
            # Only a material that names itself see-through keeps this value,
            # and on the shelves only GlassHelmets01 (0.686, the inhaler body)
            # does. Everything else measured here - the Tranquilitea cup's
            # 0.608, the Isotopic Coolant lid's 0.252 - is solid on screen.
            if not any(has_name_word(mat, t) for t in SHEER_NAME_TOKENS):
                return 1.0, False, (f"opaque: {alpha:.3f} override on a material that does not "
                                    "name itself see-through (layer weight)")
            return alpha, True, f"opacity {alpha:.3f} used as surface alpha"
        below = f"opacity {alpha:.3f} below the {MIN_SURFACE_ALPHA:.2f} floor -> token scan"
    elif alpha is not None and alpha >= 0.995:
        below = "alpha 1.0: opaque"
    else:
        below = "no database alpha: token scan"
    if is_crafted_drink_bag(mat) and "pouch" in ml:
        return 1.0, False, "drink-bag film (map-driven coverage)"
    for token in AUTHORED_SHEER:
        if token in ml:
            return AUTHORED_SHEER_ALPHA, True, \
                "authored glass: the database has no opacity for it"
    if is_flat_clear_glass(mat):
        return AUTHORED_SHEER_ALPHA, True, \
            "clear glass family: empty database set, art-less -> authored per-wall transmission"
    for token in SHEER_TOKENS:
        # The material's OWN name, not the path: see own_name. `ScienceGlass`
        # is the shelf, and `ScienceGlassPlastic02` is a plastic cap.
        if has_name_word(own_name(mat), token):
            return TOKEN_FALLBACK_ALPHA, True, f"'{token}' in the name -> clear shell (Fresnel only)"
    return 1.0, False, below if alpha is None else below


# The vial liquid (see kAuthoredFlat in OfflineModel.cpp).
# `ScienceGlassLiquid01/02` has no colour word in its name, no map and no colour
# in the database - no `0x025a` override, no `0x022b` layer kind, an empty set
# (HANDOFF 0d) - so it drew the neutral grey fallback and the bottle read as
# empty. The fill family behind it stores (0, 0.6524, 0.5964): the shelf's own
# measured liquid green. Authored, in the same spirit as kAuthoredGlassTint.
AUTHORED_FLAT_TINTS = {
    "scienceglassliquid01": (0.0, 0.6524, 0.5964),
    "scienceglassliquid02": (0.0, 0.6524, 0.5964),
}
AUTHORED_FLAT_WHY = ("authored liquid tint: the fill family's green "
                     "(ScienceGlass_GlassCleanTransparent01_Base_Green03)")


def authored_flat_tint(mat: str) -> tuple[float, float, float] | None:
    """Mirror of the kAuthoredFlat lookup inside FlatBaseColor."""
    ml = norm(mat)
    for key, rgb in AUTHORED_FLAT_TINTS.items():
        if key in ml:
            return rgb
    return None


def flat_base_color(mat: str) -> tuple[float, float, float] | None:
    """Mirror of FlatBaseColor: the colour a flat range is built with."""
    authored = authored_flat_tint(mat)
    if authored is not None:
        return authored
    # Keep the authored case through the camelCase split, exactly as the C++
    # now does; only the compared token is lowercased.
    stem = mat.replace("\\", "/").rsplit("/", 1)[-1]
    if "." in stem:
        stem = stem[:stem.rfind(".")]
    while stem and (stem[-1] in "_-" or stem[-1].isdigit()):
        stem = stem[:-1]
    if "_" not in stem and "wood" in stem.lower():
        return (0.42, 0.30, 0.20)
    # Only the last underscore segment can hold the colour word; camelCase
    # starts a new token inside it (InHailerYellow -> yellow).
    tail = stem.rsplit("_", 1)[-1]
    word = ""
    for i, ch in enumerate(tail):
        if i and ch.isupper() and (tail[i - 1].islower() or tail[i - 1].isdigit()):
            word = ""
        word += ch.lower()
    return NAMED_COLORS.get(word)


def color_decision(mat: str, cdb: Cdb, mean_reader=None,
                   map_present=None) -> tuple[tuple, str]:
    """Mirror of the upload colour decision. Returns (rgb, reason).

    The base colour is what the range is built with (FlatBaseColor, then
    FindCdbTint); the upload then either keeps it or resets it to white when an
    albedo lands. `map_present(key)` says whether the set's own map is really
    installed on this machine, because a set that names a file the host does
    not have leaves the range flat.
    """
    tint = cdb.tint(mat)
    own, how = cdb.own_set(mat)
    own_color = (own or {}).get("color", "")
    installed = bool(own_color) and (map_present(own_color) if map_present else True)

    if tint is None:
        if is_flat_clear_glass(mat):
            return FLAT_GLASS_TINT, \
                "flat: clear glass family has no art in the database -> pale glass tint"
        named = flat_base_color(mat)
        base = named if named else (0.70, 0.70, 0.72)
        what = (AUTHORED_FLAT_WHY if authored_flat_tint(mat) is not None else
                "name token colour" if named else "neutral grey (no colour word)")
        borrowed = how.startswith("colour-family")
        if installed and not borrowed:
            return (1.0, 1.0, 1.0), \
                f"white: own map carries the art ({own_color}); base was {what} ({how})"
        if installed and borrowed:
            # The mirrored upload arm: the sibling holds the art, the name
            # holds the colour, so the name's colour rides over the map.
            return base, (f"name colour over a borrowed colour-family map {own_color} "
                          f"({how})")
        if own_color:
            return base, f"flat: own map {own_color} is NOT installed on this host ({how})"
        opacity = (own or {}).get("opacity", "")
        if opacity:
            # Sparkling Wine's pair is exactly this: the CDB names a cutout
            # mask and no colour, so the range keeps its flat base and the
            # mask is what the preview composites.
            return base, f"flat: set has no colour map, only opacity mask {opacity} ({how})"
        return base, f"flat: no authored colour, no map -> {what} ({how})"

    if not own_color:
        return tint, ("authored override kept: no colour map in the material's own set "
                      f"({how}); any substitute map is ignored")
    if not installed:
        return tint, f"authored override kept: own map {own_color} is not installed ({how})"
    if norm(own_color).startswith(GRUNGE_PREFIX) and max(tint) - min(tint) >= GRUNGE_SPREAD:
        return tint, "authored override kept: grunge overlay left untextured"
    mean = mean_reader(own_color) if mean_reader else None
    if mean is None:
        return (1.0, 1.0, 1.0), (f"white: own map {own_color} carries the art "
                                 "(mean not measurable here)")
    mx = max(mean)
    kind = cdb.layer_kind(mat)
    if kind not in (0, None):
        # The upload's paint arm is gated on cdbPaintTint (kind 0). A layered
        # record's rgb is a layer value: `Crafted_InHailer` stores kind 1 and
        # (1.0,0.314,0.325), and the preview draws its own map at white - the
        # gold case's chrome collar - not a red one.
        return (1.0, 1.0, 1.0), (f"white: layer kind {kind} - the override is a layer value, "
                                 f"not a paint colour ({how})")
    if mx > 0.55 and (mx - min(mean)) / mx < 0.12:
        return tint, ("authored override kept and multiplied over a neutral own map "
                      f"{own_color} (mean {mean[0]:.2f},{mean[1]:.2f},{mean[2]:.2f})")
    return (1.0, 1.0, 1.0), f"white: own map carries the art ({own_color})"


def bc1_mean(info: "TexInfo") -> tuple[float, float, float] | None:
    """Mean rgb of a BC1/BC3 map, sampled like Bc1MeanRgb.

    Bc1MeanRgb accepts DXGI 71/72 (BC1) and 77/78 (BC3) and reads the first
    eight bytes of each block; anything else is "could not measure", never
    "neutral", which is mirrored here by returning None.
    """
    if info is None or not info.payload:
        return None
    if info.dxgi not in (71, 72, 77, 78) or info.width == 0 or info.height == 0:
        return None
    width, height = info.width, info.height
    blocks = info.payload
    stride = 8 if info.dxgi in (71, 72) else 16
    bx, by = (width + 3) // 4, (height + 3) // 4
    row_pitch = bx * stride
    step = max(1, int((bx * by / 4096.0) ** 0.5))
    total = [0.0, 0.0, 0.0]
    samples = 0
    for y in range(0, by, step):
        for x in range(0, bx, step):
            off = y * row_pitch + x * stride
            if off + 8 > len(blocks):
                continue
            c0, c1 = struct.unpack_from("<HH", blocks, off)
            def rgb565(c: int):
                r, g, b = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
                return (((r << 3) | (r >> 2)) / 255.0,
                        ((g << 2) | (g >> 4)) / 255.0,
                        ((b << 3) | (b >> 2)) / 255.0)
            a = rgb565(c0)
            b = rgb565(c1)
            for i in range(3):
                total[i] += 0.5 * (a[i] + b[i])
            samples += 1
    if samples == 0:
        return None
    return tuple(t / samples for t in total)


# ── The audit ─────────────────────────────────────────────────────────────────

def parse_catalog(path: Path) -> list[dict]:
    rows = []
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    lines = text.splitlines()
    head = [h.strip().strip('"') for h in lines[0].split(",")]
    idx = {name: i for i, name in enumerate(head)}
    for line in lines[1:]:
        parts = [p.strip().strip('"') for p in line.split(",")]
        if len(parts) < len(head):
            continue
        try:
            form = int(parts[idx["formID"]], 16)
        except ValueError:
            continue
        def field(name: str) -> str:
            at = idx.get(name)
            return parts[at] if at is not None and at < len(parts) else ""

        rows.append({
            "name": field("name"),
            "type": field("type"),
            "form": form,
            "plugin": field("plugin"),
            "keywords": field("keywords"),
            # The dump resolves the record's world model and every linked ARMA
            # addon. The plugin walk cannot: an ARMO's MODL is a form link, not
            # a NIF string, so the workbench uses these paths for armor.
            "model": field("modelPath"),
            "nif_paths": [p.strip() for p in field("nifPaths").split("|") if p.strip()],
        })
    return rows


def find_catalog(explicit: str | None) -> Path | None:
    if explicit:
        p = Path(explicit)
        return p if p.is_file() else None
    for p in CATALOGS:
        if p.is_file():
            return p
    return None


def select_items(items: list[dict], forms=(), needles=(), aid: bool = False,
                 limit: int = 0, shelves=()) -> list[dict]:
    """Resolve --form/--item/--aid/--shelf into unique rows, in input order."""
    wanted: list[dict] = []
    if forms:
        by_form = {f"{r['form']:08X}": r for r in items}
        for f in forms:
            key = f.upper().lstrip("0x").rjust(8, "0")
            if key in by_form:
                wanted.append(by_form[key])
            else:
                wanted.append({"name": f"(form {key})", "type": "?", "form": int(key, 16),
                               "plugin": "Starfield.esm", "keywords": ""})
    for needle in needles:
        wanted += [r for r in items if needle.lower() in r["name"].lower()]
    # A shelf is every form of its type: ShopExplorer maps BrowseType::kAid to
    # RE::FormType::kALCH and so on. Selecting on category keywords instead
    # finds nothing, because the catalog dump carries a keyword string for only
    # 1,530 of its 75,999 rows and none of those mention a shelf name.
    if aid:
        shelves = tuple(shelves) + ("aid",)
    types = {t for name in shelves for t in SHELVES[name]}
    if types:
        wanted += [r for r in items if r["type"] in types]
    seen = set()
    unique = []
    for r in wanted:
        if r["form"] not in seen:
            seen.add(r["form"])
            unique.append(r)
    return unique[:limit] if limit else unique


def audit_item(item: dict, plugins: Plugins, swaps: Swaps, store: AssetStore,
               cdb: Cdb, explain: bool) -> list[dict]:
    form = item["form"]
    record = plugins.record(item["plugin"], form)
    model = (record or {}).get("model", "")
    rows: list[dict] = []
    if not model:
        return [{"item": item["name"], "form": f"{form:08X}", "model": "",
                 "node": "", "nif_mat": "", "mat": "", "swap": "",
                 "set": "?", "cdb": "", "kind": "", "color": "",
                 "alpha": "", "color_why": "no MODL on the record",
                 "alpha_why": "", "note": ""}]
    blob = store.read(model)
    if not blob:
        return [{"item": item["name"], "form": f"{form:08X}", "model": model,
                 "node": "", "nif_mat": "", "mat": "", "swap": "", "set": "?",
                 "cdb": "", "kind": "", "color": "", "alpha": "",
                 "color_why": f"NIF not found on this host: {model}",
                 "alpha_why": "", "note": ""}]
    try:
        shapes = parse_shapes(blob, Nif(blob))
    except Exception as exc:  # a NIF this parser cannot read is a finding, not a crash
        shapes = []
        note = f"NIF parse failed: {exc}"
    else:
        note = ""
    if not shapes:
        return [{"item": item["name"], "form": f"{form:08X}", "model": model,
                 "node": "", "nif_mat": "", "mat": "", "swap": "", "set": "?",
                 "cdb": "", "kind": "", "color": "", "alpha": "", "color_why": note or
                 "no BSGeometry in the NIF", "alpha_why": "", "note": ""}]

    tex_cache: dict[str, TexInfo | None] = {}
    mean_cache: dict[str, tuple | None] = {}

    def info_of(key: str) -> TexInfo | None:
        if key not in tex_cache:
            tex_cache[key] = store.texture(key)
            mean_cache[key] = bc1_mean(tex_cache[key])
        return tex_cache[key]

    def mean_of(key: str):
        info_of(key)
        return mean_cache[key]

    def present_of(key: str) -> bool:
        return info_of(key) is not None

    authored = AUTHORED_SWAPS.get(form, {})
    for shape in shapes:
        nif_mat = shape.get("material") or ""
        if not nif_mat:
            continue
        node = shape.get("name", "")
        swapped = authored.get(book_key(nif_mat)) if authored else None
        how_swapped = "authored (NativeModel)" if swapped else ""
        if not swapped:
            swapped = swaps.apply(form, nif_mat)
            how_swapped = "table" if swapped else ""
        mat = swapped or nif_mat
        route = exact_route(mat, node)
        own, how = cdb.own_set(mat)
        rgba, kind = cdb.override(mat)
        if route:
            color, color_why = (1.0, 1.0, 1.0), \
                f"white: hand-authored route binds {route}"
        else:
            color, color_why = color_decision(mat, cdb, mean_of, present_of)
        alpha, _kind = cdb.alpha(mat)
        a_val, translucent, alpha_why = blend_alpha(mat, alpha, kind)
        set_col = (own or {}).get("color", "")
        rows.append({
            "item": item["name"],
            "form": f"{form:08X}",
            "model": model,
            "node": node,
            "nif_mat": nif_mat,
            "mat": mat,
            "swap": how_swapped,
            "set": how + (f" | {set_col}" if set_col else "") + (f" | route {route}" if route else ""),
            "cdb": ", ".join(f"{c:.3f}" for c in rgba[:3]) if rgba else "-",
            "kind": "" if kind is None else str(kind),
            "color": ", ".join(f"{c:.3f}" for c in color),
            "alpha": f"{a_val:.3f}" + ("  (translucent)" if translucent else ""),
            "color_why": color_why,
            "alpha_why": alpha_why,
            "note": note,
        })
    return rows


HEAD = ("item", "form", "node", "mat", "swap", "set", "cdb", "kind", "color",
        "alpha", "color_why", "alpha_why")


def print_rows(rows: list[dict], explain: bool) -> None:
    current = None
    for r in rows:
        if r["item"] != current:
            current = r["item"]
            print(f"\n== {r['item']} [{r['form']}]  {r['model']}")
        print(f"  {r['node'] or '-'}  {r['mat'] or r['nif_mat'] or '-'}"
              f"{'  (swap)' if r['swap'] else ''}")
        print(f"     set   : {r['set']}")
        print(f"     db    : rgba=({r['cdb']}) kind={r['kind'] or '-'}")
        print(f"     look  : rgb=({r['color']}) alpha={r['alpha']}")
        if explain:
            print(f"     colour: {r['color_why']}")
            print(f"     blend : {r['alpha_why']}")
        if r["note"]:
            print(f"     note  : {r['note']}")


def _rgb(text: str) -> str:
    """`1.000, 0.961, 0.798` (or `-`) to a #rrggbb swatch."""
    try:
        parts = [float(p) for p in text.split(", ")]
    except ValueError:
        return ""
    if len(parts) != 3:
        return ""
    return "#" + "".join(f"{max(0, min(255, round(c * 255))):02x}" for c in parts)


def _esc(text: str) -> str:
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def write_html(rows: list[dict], path: Path, source: str, shelf: str = "Aid") -> None:
    """One self-contained page: every material row with its swatch and why."""
    items = len({r["form"] for r in rows})
    body = []
    for r in rows:
        swatch = _rgb(r["color"])
        sw = f'<span class="sw" style="background:{swatch}"></span>' if swatch else ""
        translucent = " (translucent)" in r["alpha"]
        alpha = r["alpha"].replace("  (translucent)", "")
        alpha_html = _esc(alpha)
        if translucent:
            alpha_html += ' <span class="tag">blend</span>'
        search = f"{r['item']} {r['form']} {r['mat']} {r['node']} {r['color_why']}".lower()
        body.append((
            "<tr data-s=\"{s}\">"
            f"<td class=\"item\">{_esc(r['item'])}<div class=\"f\">{_esc(r['form'])}</div></td>"
            f"<td class=\"node\">{_esc(r['node']) or '&mdash;'}</td>"
            f"<td class=\"mat\">{_esc(r['mat'] or r.get('nif_mat', '')) or '&mdash;'}"
            f"{'<div class=\"swap\">swapped</div>' if r['swap'] else ''}"
            f"<div class=\"set\">{_esc(r['set'])}</div></td>"
            f"<td class=\"db\">{_esc(r['cdb']) or '&mdash;'}"
            f"{' / kind ' + _esc(r['kind']) if r['kind'] else ''}</td>"
            f"<td class=\"look\">{sw}<code>{_esc(r['color'])}</code></td>"
            f"<td class=\"alpha\">{alpha_html}</td>"
            f"<td class=\"why\">{_esc(r['color_why'])}</td>"
            f"<td class=\"why\">{_esc(r['alpha_why'])}</td>"
            "</tr>"
        ).replace("{s}", _esc(search)))
    html = f"""<!doctype html>
<html lang="en"><meta charset="utf-8">
<title>{shelf} colour audit</title>
<style>
:root {{ color-scheme: light; --line: #8884; }}
body {{ font: 13px/1.45 ui-sans-serif, system-ui, sans-serif; margin: 24px;
       background: #fff; color: #1a1a1a; }}
h1 {{ font-size: 18px; margin: 0 0 4px; }}
.meta {{ color: #888; margin: 0 0 14px; }}
input {{ width: 320px; max-width: 100%; padding: 6px 8px; margin-bottom: 12px; }}
table {{ border-collapse: collapse; width: 100%; }}
th, td {{ border-bottom: 1px solid var(--line); padding: 5px 7px;
         text-align: left; vertical-align: top; }}
th {{ position: sticky; top: 0; background: #fff; z-index: 2; }}
th:first-child, td.item {{ position: sticky; left: 0; background: #fff; }}
th:first-child {{ z-index: 3; }}
tr:hover td {{ background: #8881; }}
.item {{ white-space: nowrap; font-weight: 600; }}
.f, .set, .swap {{ font-weight: 400; font-size: 11px; color: #888; }}
.node, .mat {{ max-width: 360px; overflow-wrap: anywhere; }}
.sw {{ display: inline-block; width: 16px; height: 16px; border-radius: 3px;
       border: 1px solid #0006; vertical-align: -3px; margin-right: 6px; }}
.tag {{ font-size: 10px; border: 1px solid #8886; border-radius: 3px; padding: 0 3px; }}
.why {{ max-width: 420px; }}
code {{ font-size: 12px; }}
</style>
<h1>{shelf} colour audit</h1>
<p class="meta">{items} {shelf} item(s), {len(rows)} material row(s) &middot; generated from
{_esc(source)} without launching the game</p>
<input id="f" placeholder="Filter by item, material, node or rule">
<table>
<thead><tr><th>Item</th><th>Node</th><th>Material / texture set</th><th>DB override</th>
<th>Look</th><th>Alpha</th><th>Why this colour</th><th>Why this alpha</th></tr></thead>
<tbody>
{chr(10).join(body)}
</tbody></table>
<script>
f.addEventListener('input', () => {{
  const q = f.value.toLowerCase();
  for (const tr of document.querySelectorAll('tbody tr'))
    tr.hidden = q !== '' && !tr.dataset.s.includes(q);
}});
</script>
</html>
"""
    path.write_text(html, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--item", action="append", default=[], help="substring of the item name")
    ap.add_argument("--form", action="append", default=[], help="form id, hex")
    ap.add_argument("--aid", action="store_true", help="every Aid-shelf row in the catalog")
    ap.add_argument("--shelf", action="append", default=[], choices=sorted(SHELVES),
                    help="every row of one shop shelf (repeatable)")
    ap.add_argument("--catalog", default=None)
    ap.add_argument("--cdb", default=str(CDB))
    ap.add_argument("--data", default=str(GAME_DATA))
    ap.add_argument("--mods-root", default=str(MODS_ROOT))
    ap.add_argument("--swaps", default=str(SWAPS))
    ap.add_argument("--tsv", default=None)
    ap.add_argument("--html", default=None, help="write the whole table as one self-contained page")
    ap.add_argument("--explain", action="store_true", help="print the rule each column came from")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    catalog = find_catalog(a.catalog)
    if not catalog:
        print("no catalog CSV found; pass --catalog", file=sys.stderr)
        return 2
    items = parse_catalog(catalog)

    unique = select_items(items, a.form, a.item, a.aid, a.limit, a.shelf)
    if not unique:
        print("nothing selected: pass --aid, --shelf, --item or --form", file=sys.stderr)
        return 2
    picked_shelves = list(dict.fromkeys((["aid"] if a.aid else []) + list(a.shelf)))
    label = " + ".join({"books": "Books"}.get(s, s.capitalize()) for s in picked_shelves)

    if not a.quiet:
        print(f"[audit] {len(unique)} item(s); catalog {catalog.name}", file=sys.stderr)
    store = AssetStore([Path(a.data)], Path(a.mods_root) if a.mods_root else None, not a.quiet)
    plugins = Plugins(store)
    swaps = Swaps(Path(a.swaps) if a.swaps else None)
    cdb = Cdb(Path(a.cdb))

    all_rows: list[dict] = []
    for item in unique:
        all_rows += audit_item(item, plugins, swaps, store, cdb, a.explain)
    print_rows(all_rows, a.explain)

    if a.tsv:
        with open(a.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write("\t".join(HEAD + ("model",)) + "\n")
            for r in all_rows:
                fh.write("\t".join(str(r.get(k, "")) for k in HEAD + ("model",)) + "\n")
        print(f"\n[audit] wrote {len(all_rows)} row(s) to {a.tsv}", file=sys.stderr)
    if a.html:
        write_html(all_rows, Path(a.html), catalog.name, label or "Selection")
        print(f"[audit] wrote {a.html}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
