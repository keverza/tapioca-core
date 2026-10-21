"""Generate TapiocaUI's compiled-in glyph tables from the curated Iconoir SVGs.

    python AddOn/EvP/tools/generate_iconoir_icons.py           write Iconoir.gen.cpp
    python AddOn/EvP/tools/generate_iconoir_icons.py --check   fail if it is out of date

Input: the append-only id registry ``TapiocaUI/Foundation/IconId.hpp`` (each entry names its
upstream file) and the glyphs copied verbatim into ``AddOn/EvP/Icons/`` from the pinned
Iconoir v7.12.1 drop. Output: ``TapiocaUI/Foundation/Iconoir.gen.cpp``, the only icon data the
binary carries -- nothing reads an SVG at run time.

Every curve is flattened here, once, into a polyline in units of the 24 x 24 grid divided by 24,
so the HUD draws a glyph by transforming points and never tessellates per frame. Only what
Iconoir's regular set uses is accepted -- <path> elements with M/L/H/V/C/S/Q/T/Z commands, a 1.5
stroke, round caps and joins, no fill, no transform. Anything else FAILS the generation instead
of being dropped, because a silently missing subpath is a wrong toolbar glyph nobody notices.

Iconoir (https://iconoir.com) - MIT, Copyright (c) 2021 Luca Burgio.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass
from pathlib import Path

EVP_ROOT = Path(__file__).resolve().parents[1]
ICONS_DIR = EVP_ROOT / "Icons"
FOUNDATION = EVP_ROOT / "Sources" / "AddOn" / "ArchViz" / "TapiocaUI" / "Foundation"
REGISTRY = FOUNDATION / "IconId.hpp"
OUTPUT = FOUNDATION / "Iconoir.gen.cpp"

GRID = 24.0
STROKE_WIDTH = "1.5"
# How far a flattened curve may stray from the true one, in grid units: 0.01 of 24 is under a
# hundredth of a pixel at 48 px, far below what the stroke's antialiasing can show.
TOLERANCE = 0.01
SVG_NS = "{http://www.w3.org/2000/svg}"

_ENTRY = re.compile(r"^\s*(\w+)\s*=\s*(\d+)\s*,\s*//\s*([\w.-]+\.svg)\b", re.M)
_NONE = re.compile(r"^\s*None\s*=\s*0\s*,", re.M)
_TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtZzAa]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


class GlyphError(ValueError):
    """A glyph or registry entry the generator cannot represent exactly."""


@dataclass
class Entry:
    name: str
    value: int
    file: str


def read_registry(text: str) -> list[Entry]:
    if not _NONE.search(text):
        raise GlyphError("IconId.hpp has no `None = 0` placeholder entry")
    entries = [Entry(m.group(1), int(m.group(2)), m.group(3)) for m in _ENTRY.finditer(text)]
    seen: dict[int, str] = {}
    for entry in entries:
        if entry.value == 0:
            raise GlyphError(f"{entry.name} takes 0, which is None's")
        if entry.value in seen:
            raise GlyphError(f"{entry.name} and {seen[entry.value]} share the value {entry.value}")
        seen[entry.value] = entry.name
    return sorted(entries, key=lambda e: e.value)


def _cubic(points: list[tuple[float, float]], p0, p1, p2, p3, depth: int = 0) -> None:
    """Append the cubic's flattened points after p0, p3 last."""
    ux, uy = p3[0] - p0[0], p3[1] - p0[1]
    length = math.hypot(ux, uy)

    def distance(p) -> float:
        if length < 1e-12:
            return math.hypot(p[0] - p0[0], p[1] - p0[1])
        return abs((p[0] - p0[0]) * uy - (p[1] - p0[1]) * ux) / length

    if depth >= 16 or max(distance(p1), distance(p2)) <= TOLERANCE:
        points.append(p3)
        return

    def mid(a, b):
        return ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)

    p01, p12, p23 = mid(p0, p1), mid(p1, p2), mid(p2, p3)
    p012, p123 = mid(p01, p12), mid(p12, p23)
    centre = mid(p012, p123)
    _cubic(points, p0, p01, p012, centre, depth + 1)
    _cubic(points, centre, p123, p23, p3, depth + 1)


def flatten_path(d: str) -> list[tuple[list[tuple[float, float]], bool]]:
    """The subpaths of one `d` attribute as (points, closed), in grid units."""
    tokens = _TOKEN.findall(d)
    residue = _TOKEN.sub("", d).strip(" ,\t\r\n")
    if residue:
        raise GlyphError(f"path data has characters that are neither commands nor numbers: {residue[:20]!r}")
    subpaths: list[tuple[list[tuple[float, float]], bool]] = []
    current: list[tuple[float, float]] = []
    x = y = 0.0
    start = (0.0, 0.0)
    last_control: tuple[float, float] | None = None
    last_quad: tuple[float, float] | None = None
    command = ""
    i = 0

    def number() -> float:
        nonlocal i
        if i >= len(tokens) or tokens[i].isalpha():
            raise GlyphError(f"command {command} is missing a number in {d[:40]!r}")
        value = float(tokens[i])
        i += 1
        return value

    def finish(closed: bool) -> None:
        nonlocal current
        if len(current) >= 1:
            subpaths.append((current, closed))
        current = []

    while i < len(tokens):
        if tokens[i].isalpha():
            command = tokens[i]
            i += 1
            if command in "Aa":
                raise GlyphError("arc commands are not supported; Iconoir's regular set has none")
            if command in "Zz":
                if current:
                    finish(True)
                x, y = start
                last_control = last_quad = None
                continue
        elif not command:
            raise GlyphError(f"path data starts with a number: {d[:40]!r}")
        relative = command.islower()
        op = command.upper()
        ox, oy = (x, y) if relative else (0.0, 0.0)
        if op == "M":
            if current:
                finish(False)
            x, y = ox + number(), oy + number()
            start = (x, y)
            current = [(x, y)]
            command = "l" if relative else "L"  # further pairs are line-tos
            last_control = last_quad = None
        elif op in "LHV":
            if op == "L":
                x, y = ox + number(), oy + number()
            elif op == "H":
                x = ox + number()
            else:
                y = oy + number()
            current.append((x, y))
            last_control = last_quad = None
        elif op in "CS":
            p0 = (x, y)
            if op == "C":
                p1 = (ox + number(), oy + number())
            else:
                p1 = (2 * x - last_control[0], 2 * y - last_control[1]) if last_control else p0
            p2 = (ox + number(), oy + number())
            p3 = (ox + number(), oy + number())
            _cubic(current, p0, p1, p2, p3)
            last_control, last_quad = p2, None
            x, y = p3
        elif op in "QT":
            p0 = (x, y)
            if op == "Q":
                q = (ox + number(), oy + number())
            else:
                q = (2 * x - last_quad[0], 2 * y - last_quad[1]) if last_quad else p0
            p3 = (ox + number(), oy + number())
            p1 = (p0[0] + 2 / 3 * (q[0] - p0[0]), p0[1] + 2 / 3 * (q[1] - p0[1]))
            p2 = (p3[0] + 2 / 3 * (q[0] - p3[0]), p3[1] + 2 / 3 * (q[1] - p3[1]))
            _cubic(current, p0, p1, p2, p3)
            last_quad, last_control = q, None
            x, y = p3
        else:
            raise GlyphError(f"unsupported path command {command!r}")
    if current:
        finish(False)
    return subpaths


def read_glyph(path: Path) -> list[tuple[list[tuple[float, float]], bool]]:
    """Every subpath of one curated SVG, validated against what the generator can represent."""
    root = ElementTree.parse(path).getroot()
    if root.tag != SVG_NS + "svg":
        raise GlyphError(f"{path.name}: the root is not <svg>")
    if (root.get("viewBox") or "").split() != ["0", "0", "24", "24"]:
        raise GlyphError(f"{path.name}: viewBox is not 0 0 24 24")
    subpaths: list[tuple[list[tuple[float, float]], bool]] = []
    for element in root.iter():
        if element is root:
            continue
        if element.tag != SVG_NS + "path":
            raise GlyphError(f"{path.name}: <{element.tag.replace(SVG_NS, '')}> is not supported, only <path>")
        if element.get("transform"):
            raise GlyphError(f"{path.name}: a transform is not supported")
        if element.get("fill", root.get("fill", "none")) != "none":
            raise GlyphError(f"{path.name}: a filled path needs a mesh or the atlas backend, not a stroke")
        width = element.get("stroke-width", root.get("stroke-width"))
        if width != STROKE_WIDTH:
            raise GlyphError(f"{path.name}: stroke-width {width!r}, every glyph is drawn at {STROKE_WIDTH}")
        flattened = flatten_path(element.get("d", ""))
        # A cap shows only at an open end: upstream leaves it off its closed outlines
        # (cursor-pointer), and the HUD draws every open end round, so only an open subpath
        # declared with another cap would be drawn wrong.
        cap = element.get("stroke-linecap", root.get("stroke-linecap"))
        if cap not in (None, "round") or (cap is None and any(not closed for _, closed in flattened)):
            raise GlyphError(f"{path.name}: an open stroke without round caps")
        if element.get("stroke-linejoin", root.get("stroke-linejoin")) not in (None, "round", "miter"):
            raise GlyphError(f"{path.name}: stroke-linejoin is neither round nor miter")
        subpaths.extend(flattened)
    if not subpaths:
        raise GlyphError(f"{path.name}: no strokes")
    return subpaths


def _clean(points: list[tuple[float, float]], closed: bool) -> list[tuple[float, float]]:
    out: list[tuple[float, float]] = []
    for p in points:
        q = (round(p[0] / GRID, 4), round(p[1] / GRID, 4))
        if not out or out[-1] != q:
            out.append(q)
    if closed and len(out) > 1 and out[-1] == out[0]:
        out.pop()
    return out


def _number(value: float) -> str:
    text = f"{value:.4f}".rstrip("0").rstrip(".")
    return (text if text not in ("-0", "") else "0") + "f"


PLACEHOLDER = [([(4.0, 4.0), (20.0, 4.0), (20.0, 20.0), (4.0, 20.0)], True)]


def generate(registry_text: str, icons_dir: Path) -> str:
    entries = read_registry(registry_text)
    by_value = {entry.value: entry for entry in entries}
    count = (max(by_value) if by_value else 0) + 1
    points: list[tuple[float, float]] = []
    strokes: list[tuple[int, int, bool]] = []
    glyphs: list[tuple[str, int, int, str]] = []
    for value in range(count):
        entry = by_value.get(value)
        if entry is None:
            subpaths, file, name = PLACEHOLDER, "", "None" if value == 0 else f"(unused {value})"
        else:
            source = icons_dir / entry.file
            if not source.is_file():
                raise GlyphError(f"{entry.name}: {entry.file} is not in {icons_dir}")
            subpaths, file, name = read_glyph(source), entry.file, entry.name
        first_stroke = len(strokes)
        for raw, closed in subpaths:
            cleaned = _clean(raw, closed)
            strokes.append((len(points), len(cleaned), closed))
            points.extend(cleaned)
        glyphs.append((file, first_stroke, len(strokes) - first_stroke, name))

    lines = [
        "// GENERATED by AddOn/EvP/tools/generate_iconoir_icons.py from TapiocaUI/Foundation/IconId.hpp",
        "// and AddOn/EvP/Icons/*.svg. Do not edit: change the registry or the SVG and regenerate.",
        "//",
        "// Iconoir v7.12.1 (https://iconoir.com) - MIT, Copyright (c) 2021 Luca Burgio.",
        "",
        '#include "ArchViz/TapiocaUI/Foundation/IconData.hpp"',
        "",
        "namespace geomsrv::archviz::ui::icondata {",
        "",
        "namespace {",
        "",
        "// clang-format off",
        f"constexpr float kPoints[] = {{ // {len(points)} points",
    ]
    for start in range(0, len(points), 6):
        chunk = points[start : start + 6]
        lines.append("    " + " ".join(f"{_number(px)}, {_number(py)}," for px, py in chunk))
    lines += ["};", "", f"constexpr Stroke kStrokes[] = {{ // {len(strokes)} strokes"]
    for start in range(0, len(strokes), 6):
        chunk = strokes[start : start + 6]
        lines.append("    " + " ".join(f"{{ {f}, {n}, {'true' if c else 'false'} }}," for f, n, c in chunk))
    lines += ["};", "", "constexpr Glyph kGlyphs[] = {"]
    for value, (file, first, n, name) in enumerate(glyphs):
        lines.append(f'    {{ "{file}", "{name}", {first}, {n} }}, // {value}')
    lines += [
        "};",
        "// clang-format on",
        "",
        "} // namespace",
        "",
        "uint32_t GlyphCount ()",
        "{",
        "    return uint32_t (sizeof (kGlyphs) / sizeof (kGlyphs[0]));",
        "}",
        "",
        "const Glyph& GlyphAt (uint32_t index)",
        "{",
        "    return kGlyphs[index < GlyphCount () ? index : 0];",
        "}",
        "",
        "const Stroke* Strokes ()",
        "{",
        "    return kStrokes;",
        "}",
        "",
        "const float* Points ()",
        "{",
        "    return kPoints;",
        "}",
        "",
        "} // namespace geomsrv::archviz::ui::icondata",
        "",
    ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if Iconoir.gen.cpp is out of date")
    args = parser.parse_args(argv)
    try:
        text = generate(REGISTRY.read_text(encoding="utf-8"), ICONS_DIR)
    except GlyphError as error:
        print(f"generate_iconoir_icons: {error}", file=sys.stderr)
        return 1
    current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
    if args.check:
        if current.replace("\r\n", "\n") != text:
            print(f"{OUTPUT.name} is out of date: run {Path(__file__).name}", file=sys.stderr)
            return 1
        print(f"{OUTPUT.name} is up to date.")
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"Wrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
