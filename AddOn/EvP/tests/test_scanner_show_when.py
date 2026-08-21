"""F3 — what the scanner accepts, rejects and flattens for evp.Action / show_when.

Kept OUTSIDE PyPackage so it never ships with the add-on. Run:
    python -m pytest AddOn/EvP/tests/test_scanner_show_when.py

Two things are being pinned here, and both fail SILENTLY otherwise:

  * the FLAT shape. show_when leaves the scanner as `show_when_param` plus
    `show_when_values`, never as a nested dict — the C++ side reads a param's
    metadata as flat scalars and string arrays, and a nested dict arrives as
    nothing at all, which reads as a control that is simply always visible.
  * the DIAGNOSTICS. A show_when naming a parameter that does not exist, or a value
    its controller can never hold, produces a control the user can never see. As an
    error it is a five-second fix; as silence it is an afternoon.
"""
import importlib.util
import os

import pytest

_SCANNER_PATH = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage", "evp",
                             "_scanner.py")


def _load():
    spec = importlib.util.spec_from_file_location("_evp_scanner_undertest", _SCANNER_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


scanner = _load()


def scan(source, tmp_path):
    folder = tmp_path / "Probe"
    folder.mkdir()
    entry = folder / "command.py"
    entry.write_text(source, encoding="utf-8")
    return scanner.scan_file(str(entry), "Probe")


def params_by_name(meta):
    return {p["name"]: p for p in meta["params"]}


def test_rejects_hardcoded_dimensional_label_unit(tmp_path):
    source = '''
import evp

@evp.command(labels={"offset": "Offset (mm)"})
def run(offset: evp.Float(unit="m") = 0.1):
    pass
'''
    with pytest.raises(scanner.ScanError, match="open project's Working Unit"):
        scan(source, tmp_path)


@pytest.mark.parametrize(("old_unit", "api_unit"), [("mm", "m"), ("deg", "rad")])
def test_rejects_display_units_in_dimensional_annotations(tmp_path, old_unit, api_unit):
    source = '''
import evp

@evp.command()
def run(value: evp.Float(unit=%r) = 1.0):
    pass
''' % old_unit
    with pytest.raises(scanner.ScanError, match="unit=%r" % api_unit):
        scan(source, tmp_path)


ACTION_COMMAND = '''
import evp


@evp.command(title="Slope Symbols", category="Annotation")
def run(
    action: evp.Action("Place", "Update", "Remove") = "Place",
    layer: evp.Layer(show_when={"action": ["Place", "Update"]}) = "Annotation",
    radius: evp.Float(unit="m", show_when={"action": "Place"}) = 1.0,
    dry_run: bool = True,
    report: evp.Bool(show_when={"dry_run": False}) = False,
):
    pass
'''


def test_action_and_show_when_flatten(tmp_path):
    meta = scan(ACTION_COMMAND, tmp_path)
    params = params_by_name(meta)

    assert params["action"]["type"] == "Action"
    assert params["action"]["args"] == ["Place", "Update", "Remove"]

    # A picker CALLED with kwargs still scans as its own kind — evp.Layer(...) is
    # the parameterised form of the evp.Layer instance, not a different type.
    assert params["layer"]["type"] == "Layer"
    assert params["layer"]["show_when_param"] == "action"
    assert params["layer"]["show_when_values"] == ["Place", "Update"]

    # A single value is accepted and normalised to a list, so the C++ side reads
    # one shape (a string array) rather than two.
    assert params["radius"]["show_when_values"] == ["Place"]

    # A bool controller compares as "true"/"false" — the spelling the palette reads
    # a checkbox back as. `False` would never match anything.
    assert params["report"]["show_when_values"] == ["false"]

    # The nested form is GONE from every param: it must not reach the flat JSON.
    assert all("show_when" not in p for p in meta["params"])


def test_show_when_survives_json_as_flat_keys(tmp_path):
    """paramsJson is what the C++ side actually parses; assert the flat keys are in it."""
    import json
    meta = scan(ACTION_COMMAND, tmp_path)
    layer = next(json.loads(p) for p in [json.dumps(x) for x in meta["params"]]
                 if json.loads(p)["name"] == "layer")
    assert layer["show_when_param"] == "action"
    assert layer["show_when_values"] == ["Place", "Update"]


def test_file_path_extensions_survive_scan(tmp_path):
    source = '''
import evp


@evp.command()
def run(
    executable: evp.FilePath(extensions=("exe",)) = "CloudCompare.exe",
    tile: evp.FilePath(extensions=("e57", "las", "laz", "ply")) = "tile.e57",
):
    pass
'''
    params = params_by_name(scan(source, tmp_path))

    assert params["executable"]["type"] == "FilePath"
    assert params["executable"]["extensions"] == ("exe",)
    assert params["tile"]["extensions"] == ("e57", "las", "laz", "ply")


def test_selection_sets_are_preserved_in_declaration_order(tmp_path):
    source = '''
import evp


@evp.command(selection_sets=("Targets", "Operators", "Context"))
def run():
    pass
'''
    meta = scan(source, tmp_path)
    assert meta["selection_sets"] == ["Targets", "Operators", "Context"]


@pytest.mark.parametrize("value", ["Targets", [], ["Targets", "targets"], [""], [42]])
def test_invalid_selection_sets_are_diagnostic(tmp_path, value):
    source = '''
import evp


@evp.command(selection_sets=%r)
def run():
    pass
''' % (value,)
    with pytest.raises(scanner.ScanError):
        scan(source, tmp_path)


def test_two_actions_are_a_diagnostic(tmp_path):
    source = '''
import evp


@evp.command()
def run(a: evp.Action("X", "Y") = "X", b: evp.Action("P", "Q") = "P"):
    pass
'''
    with pytest.raises(scanner.ScanError) as excinfo:
        scan(source, tmp_path)
    assert "evp.Action" in excinfo.value.message


def test_action_without_choices_is_a_diagnostic(tmp_path):
    source = '''
import evp


@evp.command()
def run(a: evp.Action() = "X"):
    pass
'''
    with pytest.raises(scanner.ScanError):
        scan(source, tmp_path)


def test_show_when_naming_an_unknown_parameter_is_a_diagnostic(tmp_path):
    source = '''
import evp


@evp.command()
def run(action: evp.Action("Place") = "Place",
        radius: evp.Float(show_when={"mode": "Place"}) = 1.0):
    pass
'''
    with pytest.raises(scanner.ScanError) as excinfo:
        scan(source, tmp_path)
    assert "mode" in excinfo.value.message


def test_show_when_on_an_impossible_value_is_a_diagnostic(tmp_path):
    source = '''
import evp


@evp.command()
def run(action: evp.Action("Place", "Remove") = "Place",
        radius: evp.Float(show_when={"action": "Modify"}) = 1.0):
    pass
'''
    with pytest.raises(scanner.ScanError) as excinfo:
        scan(source, tmp_path)
    assert "Modify" in excinfo.value.message


def test_show_when_on_two_controllers_is_a_diagnostic(tmp_path):
    source = '''
import evp


@evp.command()
def run(action: evp.Action("Place", "Remove") = "Place",
        dry_run: bool = True,
        radius: evp.Float(show_when={"action": "Place", "dry_run": True}) = 1.0):
    pass
'''
    with pytest.raises(scanner.ScanError):
        scan(source, tmp_path)


def test_show_when_on_itself_is_a_diagnostic(tmp_path):
    source = '''
import evp


@evp.command()
def run(radius: evp.Float(show_when={"radius": 1.0}) = 1.0):
    pass
'''
    with pytest.raises(scanner.ScanError):
        scan(source, tmp_path)


def test_show_when_against_an_open_type_is_allowed(tmp_path):
    """A layer or a text field can hold anything, so its values cannot be checked
    ahead of time — and refusing them would be refusing a legal command."""
    source = '''
import evp


@evp.command()
def run(layer: evp.Layer = "Annotation",
        radius: evp.Float(show_when={"layer": "Annotation"}) = 1.0):
    pass
'''
    meta = scan(source, tmp_path)
    assert params_by_name(meta)["radius"]["show_when_param"] == "layer"


def test_a_command_without_show_when_is_untouched(tmp_path):
    source = '''
import evp


@evp.command(title="Plain")
def run(count: evp.Int(minimum=1) = 3, name: str = "x"):
    pass
'''
    meta = scan(source, tmp_path)
    for param in meta["params"]:
        assert "show_when_param" not in param
        assert "show_when_values" not in param
