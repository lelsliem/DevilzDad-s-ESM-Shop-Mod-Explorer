"""Which NIFs a weapon is made of, read from the plugins instead of the game.

A dummy-base gun carries no geometry of its own: Razorback's WEAP record points
at `Weapons\\Bullpup\\BullpupDummyReceiver.nif`, a receiver placeholder with no
BSGeometry, and the engine assembles the real gun out of OMODs. The plugin's
live preview does the same through CollectWeaponAssemblyNifs
(src/ShopExplorer.cpp), so an offline tile of such a weapon drew nothing at all
unless this file replicated it.

What is replicated, and what is deliberately not:

  * the family is the weapon's own keyword list (KWDA): every OMOD whose MNAM
    keyword is on the weapon belongs to it. That is the same binding the
    vanilla data uses (`ma_Razorback` = 001F70BF on all 34 Razorback mods), and
    it needs no runtime object.
  * the pick is the standard tier - the OMODs the crafting menu starts from,
    whose editor ids carry `_Standard` (grip, receiver, barrel). Without a
    standard tier (a plugin weapon with no vanilla table entry) the first
    model-bearing OMOD per slot word is taken, so the tile still draws. A
    receiver is added when the standard set has none (Microgun marks only its
    accessories standard), because the receiver is the placement root.
  * no user choices, no rarity, no quality tier: those are runtime state the
    plugin keeps in game, and a contact sheet has no session to read.
  * placement is read from the NIFs, not guessed: each part declares the slots
    it fills (C-<Slot> children) and each host declares where a slot sits
    (P-<Slot> parents on a node), so connect_points.py composes
    parentPlacement * hostNodeWorld * entryTransform and the parts draw as one
    gun instead of a pile at the origin. This is the same chain as
    NativeModel::ResolveAssemblyPlaces; see connect_points.py.

    from omod_assembly import Assemblies
    Assemblies(store).nifs_for("Starfield.esm", 0x00000FD6, model)
"""
from __future__ import annotations

import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "ba2_audit"))

import gen_material_swaps as gms  # noqa: E402

# A model that is a placeholder, not geometry. "dummy" is the vanilla marker
# (BullpupDummyReceiver, MeleeDummyHousing); Breech Block is the one stock NIF
# that is a complete receiver but still wants its crafting parts appended.
DUMMY_MARKERS = ("dummy", "breechblock")
MAX_PARTS = 8


class Assemblies:
    """One record walk per plugin, indexed for family and keyword lookups."""

    def __init__(self, store) -> None:
        self.store = store
        self._plugins: dict[str, dict] = {}

    def _read(self, plugin: str) -> bytes | None:
        for cand in (plugin, plugin.replace("/", "\\")):
            blob = self.store.read(cand)
            if blob:
                return blob
        for ext in (".esm", ".esp", ".esl"):
            blob = self.store.read(plugin + ext)
            if blob:
                return blob
        return None

    def _scan(self, plugin: str) -> dict:
        """{omods: [...], kwda: {form: [keyword ids]}} for one plugin."""
        cached = self._plugins.get(plugin)
        if cached is not None:
            return cached
        out = {"omods": [], "kwda": {}}
        self._plugins[plugin] = out
        blob = self._read(plugin)
        if not blob:
            print(f"[warn] plugin not found: {plugin}", file=sys.stderr)
            return out
        end = gms.RECORD_HEADER + gms._u32(blob, 4)
        for tag, rec, start, stop in gms.iter_records(blob, end, len(blob)):
            if tag not in (b"OMOD", b"WEAP"):
                continue
            form = gms._u32(blob, rec + 12) & 0xFFFFFF
            edid = ""
            model = ""
            mnam = 0
            kwda: list[int] = []
            for sub, payload in gms.subrecords(blob, start, stop):
                if sub == b"EDID":
                    edid = payload.split(b"\0")[0].decode("latin-1")
                elif sub == b"MODL":
                    model = payload.split(b"\0")[0].decode("latin-1")
                elif sub == b"MNAM" and len(payload) >= 4:
                    mnam = gms._u32(payload, 0)
                elif sub == b"KWDA":
                    kwda = [gms._u32(payload, at) for at in range(0, len(payload) - 3, 4)]
            if tag == b"WEAP":
                out["kwda"][form] = kwda
            elif model:
                out["omods"].append({"form": form, "edid": edid,
                                     "model": model, "mnam": mnam})
        return out

    def keywords(self, plugin: str, form: int) -> list[int]:
        return self._scan(plugin)["kwda"].get(form & 0xFFFFFF, [])

    def nifs_for(self, plugin: str, form: int, model: str) -> list[str]:
        """The part NIFs of a dummy-base weapon, or [] when it has geometry."""
        folded = (model or "").lower().replace("\\", "/")
        if folded and not any(marker in folded for marker in DUMMY_MARKERS):
            return []
        keywords = set(self.keywords(plugin, form))
        if not keywords:
            return []
        # The weapon's owner first: a mod's copy of a vanilla gun binds its own
        # OMODs, and the vanilla table is the fallback the plugin also scans.
        plugins = [plugin] if plugin.lower().endswith("starfield.esm") \
            else [plugin, "Starfield.esm"]
        parts: list[dict] = []
        for name in plugins:
            for omod in self._scan(name)["omods"]:
                if omod["mnam"] and omod["mnam"] in keywords:
                    parts.append(omod)
            if parts:
                break
        if not parts:
            return []
        standard = [p for p in parts if "_standard" in p["edid"].lower()]
        picked = standard or _first_per_slot(parts)
        # A receiver is the placement root. Some families mark every accessory
        # standard but leave the receiver unmarked (Microgun: standard mag,
        # grip and barrel, plain Receiver_FullAuto), and picking only the
        # standard set then mounts nothing: every part has a slot but no host.
        # The receiver is not optional, so add it even when it is not standard.
        if not any(_slot_of(p["edid"]) == "receiver" for p in picked):
            receivers = sorted((p for p in parts if _slot_of(p["edid"]) == "receiver"),
                               key=lambda p: p["edid"].lower())
            if receivers:
                picked = [receivers[0]] + list(picked)
        out: list[str] = []
        for omod in picked:
            if omod["model"] not in out:
                out.append(omod["model"])
            if len(out) >= MAX_PARTS:
                break
        return out


def _slot_of(edid: str) -> str:
    """`mod_Razorback_Barrel_Standard` -> `barrel`: the third id word."""
    words = edid.split("_")
    return words[2].lower() if len(words) > 2 else edid.lower()


def _first_per_slot(parts: list[dict]) -> list[dict]:
    """One part per slot, in editor-id order, when nothing is marked standard."""
    seen: dict[str, dict] = {}
    for omod in sorted(parts, key=lambda p: p["edid"].lower()):
        seen.setdefault(_slot_of(omod["edid"]), omod)
    return list(seen.values())
