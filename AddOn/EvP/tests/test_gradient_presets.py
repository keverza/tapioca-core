"""The gradient preset list is one list (#102).

GradientPresets.json is the source; the C++ table, evp.gradients and every schema
`preset` enum are generated from it. These tests fail when a copy drifts and when
a preset's stops are malformed.
"""

from __future__ import annotations

import importlib.util
import json
import re
import sys
from pathlib import Path

EVP = Path(__file__).resolve().parents[1]
SOURCE = EVP / "Sources" / "AddOn" / "ArchViz" / "GradientPresets.json"
TABLE = EVP / "Sources" / "AddOn" / "ArchViz" / "GradientLibraryTable.inc"
GENERATOR = EVP / "tools" / "generate_gradient_presets.py"
LEGACY = ("viridis", "inferno", "magma", "plasma", "turbo", "coolwarm", "greys", "sunhours", "slope", "clearance")


def _generator():
    spec = importlib.util.spec_from_file_location("generate_gradient_presets", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _presets():
    return json.loads(SOURCE.read_text(encoding="utf-8"))["presets"]


def test_every_generated_copy_is_current():
    assert _generator().main(["--check"]) == 0, "run tools/generate_gradient_presets.py"


def test_the_list_keeps_the_original_ten_first_and_adds_seventy_four():
    ids = [preset["id"] for preset in _presets()]
    assert tuple(ids[:10]) == LEGACY
    assert len(ids) == 84 and len(set(ids)) == 84


def test_every_preset_is_a_valid_ramp():
    for preset in _presets():
        stops = preset["stops"]
        assert 2 <= len(stops) <= 16, preset["id"]
        positions = [at for at, _ in stops]
        assert positions == sorted(positions), preset["id"]
        assert positions[0] == 0.0 and positions[-1] == 1.0, preset["id"]
        assert all(re.fullmatch(r"#[0-9A-F]{6}([0-9A-F]{2})?", colour) for _, colour in stops), preset["id"]
        assert preset["min"] < preset["max"], preset["id"]


def test_the_table_carries_each_presets_first_last_and_count():
    rows = [line for line in TABLE.read_text(encoding="utf-8").splitlines() if line.startswith('{ "')]
    presets = _presets()
    assert len(rows) == len(presets)
    rgba = _generator().rgba
    for row, preset in zip(rows, presets):
        assert row.startswith(f'{{ "{preset["id"]}", '), preset["id"]
        assert f", {len(preset['stops'])}, {{ " in row, preset["id"]
        first, last = preset["stops"][0][1], preset["stops"][-1][1]
        assert f"0x{rgba(first):08X}u" in row.split("}, {")[0], preset["id"]
        assert row.rstrip().endswith(f"0x{rgba(last):08X}u }} }} }},"), preset["id"]


def test_the_python_list_matches():
    sys.path.insert(0, str(EVP / "Sources" / "PyPackage"))
    try:
        from evp import gradients
    finally:
        sys.path.pop(0)
    assert gradients.IDS == tuple(preset["id"] for preset in _presets())
    assert gradients.get("sunlight-warm").stepped is True
    assert gradients.get("cut-fill").min == -2.0


def test_the_wrapper_accepts_every_id_and_names_the_unknown():
    sys.path.insert(0, str(EVP / "Sources" / "PyPackage"))
    try:
        from evp import overlay
    finally:
        sys.path.pop(0)
    assert overlay._colormap("sunlight-warm", 0, 11) == {"preset": "sunlight-warm", "min": 0.0, "max": 11.0}
    try:
        overlay._colormap("no-such-ramp")
    except ValueError as error:
        assert "evp.gradients.IDS" in str(error)
    else:
        raise AssertionError("an unknown preset is refused before it is sent")
