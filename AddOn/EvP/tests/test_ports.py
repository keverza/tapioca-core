"""evp._ports — a Pydantic Inputs model must produce the SAME parameter
metadata the AST scanner produces for the equivalent run() signature.

Kept OUTSIDE PyPackage so it never ships with the add-on. Run:
    python -m pytest AddOn/EvP/tests/test_ports.py

This is the safety net for the whole schema migration. `Palette/ParamPanel.cpp`
has one decode path and branches on a `type` string; both authoring styles feed
it. If the two paths ever disagree, a command written the new way renders a
different control from the same declaration — and the failure mode is silent,
because ParamPanel skips a param it cannot decode rather than reporting it.

So the assertions here are equality against the scanner's real output, not
against a hand-written expectation. When the scanner changes shape, this test
fails and both paths get updated together.
"""

import importlib.util
import os
import textwrap

import pytest

pytest.importorskip("pydantic", reason="pydantic ships in the Tapioca runtime baseline")

_PACKAGE = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage")


def _load(module_name):
    """Load one evp module by path.

    The tests import the modules directly rather than the `evp` package,
    because importing the package pulls in the whole Layer 2 surface and, in a
    bare checkout, a transport that does not exist.
    """
    path = os.path.join(_PACKAGE, "evp", "%s.py" % module_name)
    spec = importlib.util.spec_from_file_location("evp.%s" % module_name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def ports():
    import sys

    # _ports does `from .schema import PORT_CONTROLS`, so the package has to be
    # importable for real. Front of sys.path, the way the add-on stages it.
    if _PACKAGE not in sys.path:
        sys.path.insert(0, _PACKAGE)
    from evp import _ports

    return _ports


def _scanned(source, tmp_path):
    """Run the real AST scanner over a command.py written from `source`."""
    path = tmp_path / "command.py"
    path.write_text(textwrap.dedent(source), encoding="utf-8")
    scanner_module = _load("_scanner")
    return scanner_module.scan_file(str(path), "Probe")["params"]


def _ported(model, ports_module, labels=None):
    return ports_module.ports_from_schema(model.model_json_schema(), labels=labels)


# --------------------------------------------------------------------------
# Equivalence — the same declaration, both ways, must land identically.
# --------------------------------------------------------------------------

def test_numeric_bounds_and_unit_match_the_scanner(tmp_path, ports):
    from evp.schema import Field, NodeModel, port

    class Inputs(NodeModel):
        plot_area: float = Field(default=0.0, ge=0.0, title="Sklypo plotas",
                                 json_schema_extra=port(unit="m2"))
        count: int = Field(default=4, ge=1, le=255)

    scanned = _scanned(
        """
        import evp

        @evp.command(title="Probe", labels={"plot_area": "Sklypo plotas"})
        def run(
            plot_area: evp.Float(unit="m2", minimum=0.0) = 0.0,
            count: evp.Int(minimum=1, maximum=255) = 4,
        ):
            pass
        """,
        tmp_path,
    )
    assert _ported(Inputs, ports) == scanned


def test_enum_choices_reach_the_palette_as_a_flat_array(tmp_path, ports):
    from typing import Literal

    from evp.schema import NodeModel

    class Inputs(NodeModel):
        scope: Literal["selection", "all"] = "selection"

    scanned = _scanned(
        """
        import evp

        @evp.command(title="Probe")
        def run(scope: evp.Enum("selection", "all") = "selection"):
            pass
        """,
        tmp_path,
    )
    ported = _ported(Inputs, ports)
    assert ported == scanned
    # The failure this pins: a nested array arrives at DG as nothing, and the
    # popup appears with NO OPTIONS IN IT.
    assert ported[0]["args"] == ["selection", "all"]
    assert all(isinstance(choice, str) for choice in ported[0]["args"])


def test_pickers_match_the_scanner(tmp_path, ports):
    from evp.schema import Field, NodeModel, port

    class Inputs(NodeModel):
        layer: str = Field(default="Annotation",
                           json_schema_extra=port(control="layer"))
        surface: str = Field(default="",
                             json_schema_extra=port(control="surface"))

    scanned = _scanned(
        """
        import evp

        @evp.command(title="Probe")
        def run(layer: evp.Layer = "Annotation", surface: evp.Surface = ""):
            pass
        """,
        tmp_path,
    )
    assert _ported(Inputs, ports) == scanned


def test_show_when_is_flattened_to_two_keys(tmp_path, ports):
    from typing import Literal

    from evp.schema import Field, NodeModel, port

    class Inputs(NodeModel):
        use_project_stories: bool = True
        floor_height: float = Field(
            default=3.1, ge=0.1, title="Floor height",
            json_schema_extra=port(unit="m",
                                   show_when={"use_project_stories": False}))
        mode: Literal["Place", "Remove"] = "Place"
        radius: float = Field(default=1.0, title="Radius",
                              json_schema_extra=port(unit="m",
                                                     show_when={"mode": ["Place"]}))

    ported = _ported(Inputs, ports)
    by_name = {p["name"]: p for p in ported}

    # A checkbox reads back as "true"/"false" on the C++ side; a Python False
    # that stayed False would never match and the row would never appear.
    assert by_name["floor_height"]["show_when_param"] == "use_project_stories"
    assert by_name["floor_height"]["show_when_values"] == ["false"]
    assert by_name["radius"]["show_when_values"] == ["Place"]
    # Flat, never nested.
    assert "show_when" not in by_name["floor_height"]


def test_required_field_has_no_default(ports):
    from evp.schema import Field, NodeModel

    class Inputs(NodeModel):
        name: str = Field(title="Name")

    ported = _ported(Inputs, ports)
    assert ported[0]["required"] is True
    assert "default" not in ported[0]


def test_decorator_label_beats_the_field_title(ports):
    from evp.schema import Field, NodeModel

    class Inputs(NodeModel):
        plot_area: float = Field(default=0.0, title="Plot area")

    ported = _ported(Inputs, ports, labels={"plot_area": "Sklypo plotas"})
    assert ported[0]["label"] == "Sklypo plotas"


def test_field_description_becomes_per_parameter_help(ports):
    from evp.schema import Field, NodeModel

    class Inputs(NodeModel):
        height: float = Field(default=2.8, title="Height",
                              description="Wall height, top of slab to top of slab.")

    ported = _ported(Inputs, ports)
    assert ported[0]["help"] == "Wall height, top of slab to top of slab."


def test_exclusive_bound_becomes_an_inclusive_control_minimum(ports):
    from evp.schema import Field, NodeModel

    class Inputs(NodeModel):
        height: float = Field(default=2.8, gt=0)

    # DG has no exclusive minimum. The control clamps at 0 and pydantic rejects
    # 0 at the boundary — clamping is a convenience, validation is the contract.
    assert _ported(Inputs, ports)[0]["minimum"] == 0


# --------------------------------------------------------------------------
# Diagnostics — each of these is a control the user would never see.
# --------------------------------------------------------------------------

def test_show_when_naming_an_unknown_field_is_refused(ports):
    from evp.schema import Field, NodeModel, port

    class Inputs(NodeModel):
        radius: float = Field(default=1.0,
                              json_schema_extra=port(show_when={"nope": "Place"}))

    with pytest.raises(ports.PortError, match="not a field"):
        _ported(Inputs, ports)


def test_show_when_on_itself_is_refused(ports):
    from evp.schema import Field, NodeModel, port

    class Inputs(NodeModel):
        radius: float = Field(default=1.0,
                              json_schema_extra=port(show_when={"radius": 1.0}))

    with pytest.raises(ports.PortError, match="show_when on itself"):
        _ported(Inputs, ports)


def test_impossible_show_when_value_is_refused(ports):
    from typing import Literal

    from evp.schema import Field, NodeModel, port

    class Inputs(NodeModel):
        mode: Literal["Place", "Remove"] = "Place"
        radius: float = Field(default=1.0,
                              json_schema_extra=port(show_when={"mode": "Rotate"}))

    with pytest.raises(ports.PortError, match="can only be"):
        _ported(Inputs, ports)


def test_two_actions_are_refused(ports):
    from typing import Literal

    from evp.schema import Field, NodeModel, port

    class Inputs(NodeModel):
        mode: Literal["Place", "Remove"] = Field(
            default="Place", json_schema_extra=port(control="action"))
        other: Literal["A", "B"] = Field(
            default="A", json_schema_extra=port(control="action"))

    with pytest.raises(ports.PortError, match="ONE mode"):
        _ported(Inputs, ports)


def test_a_field_with_no_renderable_type_is_refused(ports):
    from evp.schema import NodeModel

    class Inputs(NodeModel):
        points: list = []

    with pytest.raises(ports.PortError, match="no control"):
        _ported(Inputs, ports)


def test_an_empty_inputs_model_is_refused(ports):
    from evp.schema import NodeModel

    class Inputs(NodeModel):
        pass

    with pytest.raises(ports.PortError, match="no controls"):
        _ported(Inputs, ports)


def test_a_display_unit_is_refused_at_declaration(ports):
    from evp.schema import port

    # The palette converts API base units to the project's Working Units
    # itself, so "mm" here would be applied twice.
    with pytest.raises(ValueError, match="API base unit"):
        port(unit="mm")


def test_an_unknown_control_is_refused_at_declaration(ports):
    from evp.schema import port

    with pytest.raises(ValueError, match="unknown port control"):
        port(control="slider")
