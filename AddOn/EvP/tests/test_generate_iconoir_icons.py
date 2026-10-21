"""The Iconoir glyph generator: up to date, exact, and loud about what it cannot represent.

HANDOFF-TapiocaUI-Library.md section 7.6: the generated table is reviewed like source, so
regeneration must be a no-op; and section 7.5: a glyph the generator cannot represent exactly
fails generation instead of silently losing a subpath.
"""

import importlib.util
import math
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
GENERATOR_PATH = REPO_ROOT / "AddOn" / "EvP" / "tools" / "generate_iconoir_icons.py"
SPEC = importlib.util.spec_from_file_location("generate_iconoir_icons", GENERATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
generator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generator
SPEC.loader.exec_module(generator)

REGISTRY = """
enum class IconId : uint16_t {
    None = 0, // placeholder
    Box = 1,  // box.svg
};
"""


def _svg(body: str, root: str = 'stroke-width="1.5"') -> str:
    return f'<svg width="24" height="24" {root} viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">{body}</svg>'


def _write(tmp_path: Path, body: str, root: str = 'stroke-width="1.5"') -> Path:
    (tmp_path / "box.svg").write_text(_svg(body, root), encoding="utf-8")
    return tmp_path


def test_the_checked_in_table_is_what_the_generator_writes():
    assert generator.main(["--check"]) == 0


def test_every_registry_entry_names_a_curated_file():
    entries = generator.read_registry(generator.REGISTRY.read_text(encoding="utf-8"))
    assert entries, "the registry is empty"
    for entry in entries:
        assert (generator.ICONS_DIR / entry.file).is_file(), entry.file
    names = {entry.name: entry.file for entry in entries}
    assert names["BooleanSubtract"] == "substract.svg"  # upstream spells it with an s


def test_generation_is_deterministic(tmp_path):
    directory = _write(
        tmp_path, '<path d="M4 4H20V20H4Z" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"/>'
    )
    assert generator.generate(REGISTRY, directory) == generator.generate(REGISTRY, directory)


def test_a_flattened_curve_stays_within_the_tolerance():
    # A quarter circle of radius 10 as Iconoir writes one: one cubic, the usual 0.5523 handles.
    k = 0.5523 * 10
    [(points, closed)] = generator.flatten_path(f"M22 12C22 {12 + k} {12 + k} 22 12 22")
    assert not closed
    assert points[0] == (22.0, 12.0) and points[-1] == (12.0, 22.0)
    assert len(points) > 4
    for x, y in points:
        assert abs(math.hypot(x - 12, y - 12) - 10) < 0.03  # the cubic's own error plus tolerance


def test_relative_and_shorthand_commands_are_resolved():
    [(points, closed)] = generator.flatten_path("m2 2 h4 v4 l-4 0 z")
    assert closed
    assert points == [(2.0, 2.0), (6.0, 2.0), (6.0, 6.0), (2.0, 6.0)]


@pytest.mark.parametrize(
    ("body", "root", "message"),
    [
        ('<path d="M2 2A4 4 0 0 1 10 10" stroke-linecap="round"/>', 'stroke-width="1.5"', "arc"),
        ('<path d="M2 2H10V10Z" fill="currentColor"/>', 'stroke-width="1.5"', "filled"),
        ('<path d="M2 2H10" transform="rotate(45)" stroke-linecap="round"/>', 'stroke-width="1.5"', "transform"),
        ('<circle cx="12" cy="12" r="4"/>', 'stroke-width="1.5"', "<circle>"),
        ('<path d="M2 2H10" stroke-linecap="round"/>', 'stroke-width="2"', "stroke-width"),
        ('<path d="M2 2H10"/>', 'stroke-width="1.5"', "round caps"),
        ('<path d="M2 2H10 Q" stroke-linecap="round"/>', 'stroke-width="1.5"', "missing a number"),
        ('<path d="M2 2H10 #" stroke-linecap="round"/>', 'stroke-width="1.5"', "neither commands nor numbers"),
    ],
)
def test_what_cannot_be_drawn_exactly_fails_generation(tmp_path, body, root, message):
    directory = _write(tmp_path, body, root)
    with pytest.raises(generator.GlyphError, match=message):
        generator.generate(REGISTRY, directory)


def test_a_closed_outline_may_leave_its_caps_unsaid(tmp_path):
    # Upstream's cursor-pointer: one closed outline, no stroke-linecap.
    directory = _write(tmp_path, '<path d="M2 2H10V10Z" stroke="currentColor" stroke-width="1.5"/>')
    assert '"box.svg", "Box"' in generator.generate(REGISTRY, directory)


def test_a_registry_value_is_given_once():
    with pytest.raises(generator.GlyphError, match="share the value"):
        generator.read_registry("None = 0,\nA = 1, // a.svg\nB = 1, // b.svg\n")
    with pytest.raises(generator.GlyphError, match="None"):
        generator.read_registry("A = 1, // a.svg\n")
