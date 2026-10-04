"""Hour annotation is a bounded integer with a native edit spin, not GSTime."""

import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
import evp
from evp import _ports, _scanner


def test_hour_annotation_and_schema_route(tmp_path):
    assert evp.Hour.kind == "Hour"
    assert evp.Hour(readonly=True).readonly is True
    path = tmp_path / "command.py"
    path.write_text(
        'import evp\n@evp.command(title="Hours")\ndef run(hour: evp.Hour = 9):\n    pass\n', encoding="utf-8"
    )
    result = _scanner.scan_file(str(path), "Hours")
    assert result["params"][0]["type"] == "Hour"
    assert result["params"][0]["default"] == 9
    schema = {"properties": {"hour": {"type": "integer", "default": 9, "x-port": {"control": "hour"}}}}
    ports = _ports.ports_from_schema(schema)
    assert ports[0]["type"] == "Hour"
    assert ports[0]["minimum"] == 0 and ports[0]["maximum"] == 23


@pytest.mark.parametrize("value", ["24", "-1", "9.5", "True", '"9"'])
def test_hour_invalid_default_is_rejected(tmp_path, value):
    path = tmp_path / "command.py"
    path.write_text(
        'import evp\n@evp.command(title="Hours")\ndef run(hour: evp.Hour = %s):\n    pass\n' % value, encoding="utf-8"
    )
    with pytest.raises(_scanner.ScanError, match="integer from 0 to 23"):
        _scanner.scan_file(str(path), "Hours")


def test_hour_spin_lifecycle_and_readback():
    root = os.path.join(os.path.dirname(__file__), "..", "Sources", "AddOn", "Palette")
    with open(os.path.join(root, "ParamNumericControls.cpp"), encoding="utf-8") as handle:
        numeric = handle.read()
    assert "std::make_unique<DG::EditSpin> (panel, seed, *edit)" in numeric
    assert "pc.hourSpin->SetMin (0)" in numeric
    assert "pc.hourSpin->SetMax (maximum)" in numeric
    assert "maximum = 23" in numeric
    assert "pc.kind = ParamControl::Kind::Int" in numeric
    with open(os.path.join(root, "ParamPanel.cpp"), encoding="utf-8") as handle:
        panel = handle.read()
    assert "pc.hourSpin->Disable" in panel
    assert "pc.hourSpin->Hide" in panel
    assert "clip.Place (pc.hourSpin.get ()" in panel
    assert "pc.hourSpin->Redraw ();" in panel
    assert "pc.hourSpin->Invalidate ();" in panel
    assert panel.index("pc.hourSpin->Show ();") < panel.index("pc.hourSpin->Redraw ();")
    assert "pc.Widget ()->Invalidate ();" in panel
    with open(os.path.join(root, "WorkflowPanel.cpp"), encoding="utf-8") as handle:
        workflow = handle.read()
    assert "control.hourSpin->Redraw ();" in workflow
    assert "control.hourSpin->Invalidate ();" in workflow
