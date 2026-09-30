"""Colour annotation and generated-control contracts."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
import evp
from evp import _scanner


def test_color_annotation_is_exported_and_scannable(tmp_path):
    assert repr(evp.Color) == "evp.Color"
    annotation = evp.Color(show_when={"action": "Attributes"})
    assert annotation.kind == "Color" and annotation.show_when == {"action": "Attributes"}
    source = '''import evp
@evp.command(title="Colour")
def run(tint: evp.Color = "#5A7F62"):
    pass
'''
    path = tmp_path / "command.py"
    path.write_text(source, encoding="utf-8")
    result = _scanner.scan_file(str(path), "Colour")
    assert result["params"][0]["type"] == "Color"
    assert result["params"][0]["default"] == "#5A7F62"


def test_color_bad_default_is_a_scan_error(tmp_path):
    path = tmp_path / "command.py"
    path.write_text('import evp\n@evp.command(title="Colour")\n'
                    'def run(tint: evp.Color = "#12345678"):\n    pass\n', encoding="utf-8")
    try:
        _scanner.scan_file(str(path), "Colour")
    except _scanner.ScanError as error:
        assert "opaque #RRGGBB" in str(error)
    else:
        raise AssertionError("the scanner accepted an alpha default")


def test_color_helper_and_probe_are_separate():
    root = os.path.join(os.path.dirname(__file__), "..", "Sources", "AddOn", "Palette")
    with open(os.path.join(root, "ParamPanel.cpp"), encoding="utf-8") as source:
        panel = source.read()
    assert 'pc.type == "Color"' in panel
    assert "BuildColorControl" in panel
    with open(os.path.join(root, "ParamColorControls.cpp"), encoding="utf-8") as source:
        colors = source.read()
    assert 'DG::GetColor ("Choose colour", &color)' in colors
    assert "hex = ColorToHex (color)" in colors
    assert "context.FillRect" in colors
    assert "swatch->Redraw" in colors
    assert 'pc.type == "DateProbe"' in panel
    with open(os.path.join(root, "ParamValues.cpp"), encoding="utf-8") as source:
        values = source.read()
    assert "hex.GetLength () != 7" in values
    assert '"#%02X%02X%02X"' in values
