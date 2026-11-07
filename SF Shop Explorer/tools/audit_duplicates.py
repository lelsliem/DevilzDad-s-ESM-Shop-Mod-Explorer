"""Static audit: duplicate definitions, duplicated string literals, markers.

Not a compiler. It looks for what a compiler stays quiet about:
  - the same function defined twice with the same parameter list,
  - the same string literal repeated inside one file (the shape of a
    copy-paste that forgot to change the message),
  - the same `constexpr const char*` value declared twice,
  - leftover TODO/FIXME/HACK markers.
Overloads (same name, different parameters) and identical literals in
different files are deliberately not reported.
"""
from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src"
FILES = sorted(list(SRC.glob("*.cpp")) + list(SRC.glob("*.h")))

CONTROL = {"if", "for", "while", "switch", "catch", "return", "else", "do", "sizeof"}

# A definition: `ret name(params)` followed by a line that opens a brace.
SIG = re.compile(
    r"^[ \t]*(?:[A-Za-z_][\w:<>,\s\*&]*?)\b(\w+)\s*\(([^;{]*)\)\s*(?:const)?\s*(?:noexcept)?\s*$"
)

LITERAL = re.compile(r'"(?:[^"\\]|\\.){12,}"')  # only long strings: log lines, keys
CONST = re.compile(r'constexpr const char\* (\w+)\s*=\s*"([^"]+)"')


def lines_of(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def raw_string_mask(lines: list[str]) -> list[bool]:
    """True for every line that is inside a C++ raw-string literal.

    Shader source lives in R"( ... )" blocks, where `register(b0)` looks
    exactly like a function definition and `#include` is HLSL, not C++.
    """
    mask: list[bool] = []
    inside = False
    for line in lines:
        if inside:
            mask.append(True)
            if ')"' in line:
                inside = False
            continue
        if 'R"(' in line:
            mask.append(True)
            if line.count('R"(') <= line.count(')"') and ')"' in line.split('R"(', 1)[1]:
                inside = False
            else:
                inside = True
            continue
        mask.append(False)
    return mask


def norm_params(params: str) -> str:
    """Parameter list with names dropped, so a rename is not a new overload."""
    out = []
    for part in params.split(","):
        part = part.strip()
        if not part or part == "void":
            continue
        # Drop a trailing identifier, keeping the type.
        out.append(re.sub(r"\b\w+\s*$", "", part).strip() or part)
    return ",".join(out).replace(" ", "")


duplicate_defs: list[str] = []
duplicate_literals: list[str] = []
duplicate_consts: list[str] = []
markers: list[str] = []

for path in FILES:
    lines = lines_of(path)
    in_raw = raw_string_mask(lines)

    seen: dict[tuple[str, str], int] = {}
    for i, line in enumerate(lines):
        stripped = line.lstrip()
        if stripped.startswith("//") or stripped.startswith("*") or in_raw[i]:
            continue
        m = SIG.match(line)
        if not m or m.group(1) in CONTROL:
            continue
        body = None
        for nxt in lines[i + 1 : i + 4]:
            if not nxt.strip():
                continue
            body = nxt.strip()
            break
        if body is None or not body.startswith("{"):
            continue
        key = (m.group(1), norm_params(m.group(2)))
        if key in seen:
            duplicate_defs.append(
                f"{path.name}: {m.group(1)}() defined at line {seen[key]} and again at {i + 1}"
            )
        else:
            seen[key] = i + 1

    for n, line in enumerate(lines, 1):
        if re.search(r"\b(TODO|FIXME|HACK|XXX)\b", line):
            markers.append(f"{path.name}:{n}: {line.strip()[:90]}")

# Literals repeated within one file: a copy-paste that forgot its own message.
for path in FILES:
    counts: dict[str, list[int]] = defaultdict(list)
    file_lines = lines_of(path)
    in_raw = raw_string_mask(file_lines)
    for n, line in enumerate(file_lines):
        if line.lstrip().startswith("//") or in_raw[n]:
            continue
        for lit in LITERAL.findall(line):
            # Asset paths and file names are reused on purpose; only prose
            # (a message with a space in it) reads as a copy-paste tell.
            if " " not in lit:
                continue
            counts[lit].append(n)
    for lit, where in counts.items():
        if len(where) > 1:
            duplicate_literals.append(f"{path.name}:{where} shares {lit[:60]}")

# Constant declarations: same value under two names, or the same name twice.
by_value: dict[str, list[tuple[str, str]]] = defaultdict(list)
for path in FILES:
    for line in lines_of(path):
        m = CONST.search(line)
        if m:
            by_value[m.group(2)].append((path.name, m.group(1)))
for value, owners in by_value.items():
    names = {n for _, n in owners}
    if len(names) < len(owners) or len(owners) > 1:
        duplicate_consts.append(f'"{value}" -> {owners}')

sections = [
    ("duplicate definitions (same name and parameter types)", duplicate_defs),
    ("duplicated string literals inside one file", duplicate_literals),
    ("duplicate constant declarations", duplicate_consts),
    ("leftover markers", markers),
]
total = sum(len(items) for _, items in sections)
for title, items in sections:
    print(f"{title}: {len(items)}")
    for item in items:
        print("  " + item)
print(f"\n{total} finding(s) across {len(FILES)} files")
sys.exit(0)
