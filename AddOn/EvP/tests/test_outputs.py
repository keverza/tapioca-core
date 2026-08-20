"""evp.outputs — the sink library every command names instead of re-implementing.

Kept OUTSIDE PyPackage so it never ships with the add-on. Run:
    python -m pytest AddOn/EvP/tests/test_outputs.py

The file sinks are exercised for real, against a temporary Tapioca root
(`evp.paths` reads LOCALAPPDATA, so pointing that at tmp_path is enough to keep
the suite out of the user's own output folder). The project sinks are not: they
end in a bus call, and what matters about them offline is that `run_action`
routes and refuses correctly, which is what is tested here.
"""

import os
import sys

import pytest

_PACKAGE = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage")
if _PACKAGE not in sys.path:
    sys.path.insert(0, _PACKAGE)

from evp import outputs  # noqa: E402
from evp.outputs import OutputError  # noqa: E402

pydantic = pytest.importorskip("pydantic")

from evp.schema import Field, NodeModel, output  # noqa: E402


@pytest.fixture(autouse=True)
def tapioca_root(tmp_path, monkeypatch):
    """Point evp.paths at a throwaway root, so a test run never writes into the
    user's own %LOCALAPPDATA%\\Tapioca\\output."""
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))
    return tmp_path


class Row(NodeModel):
    zone: str = ""
    area: float = 0.0


class Outputs(NodeModel):
    rows: list[Row] = Field(default=[], json_schema_extra=output(role="table"))
    total: float = Field(default=0.0, json_schema_extra=output(role="summary"))


class NoTable(NodeModel):
    count: int = 0


# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------

def test_csv_is_written_with_a_bom_so_excel_reads_it_as_unicode():
    artifact = outputs.csv_file("areas", ["Zone", "m2"], [["Butas Ą", 42.5]])

    assert artifact.kind == "csv"
    with open(artifact.path, "rb") as handle:
        raw = handle.read()
    assert raw.startswith(b"\xef\xbb\xbf"), "no BOM: Excel would mojibake the name"
    assert "Butas Ą" in raw.decode("utf-8-sig")


def test_an_extension_is_not_added_twice():
    artifact = outputs.csv_file("areas.csv", ["a"], [])
    assert os.path.basename(artifact.path) == "areas.csv"


def test_a_second_run_overwrites_rather_than_accumulating():
    first = outputs.csv_file("areas", ["a"], [[1], [2], [3]])
    second = outputs.csv_file("areas", ["a"], [[1]])

    assert first.path == second.path
    assert second.size < first.size


def test_text_accepts_a_list_of_lines_as_well_as_a_string():
    from_lines = outputs.text_file("report", ["one", "two"])
    with open(from_lines.path, encoding="utf-8") as handle:
        assert handle.read() == "one\ntwo"


def test_json_keeps_non_ascii_as_itself():
    artifact = outputs.json_file("data", {"name": "Ąžuolas"})
    with open(artifact.path, encoding="utf-8") as handle:
        assert "Ąžuolas" in handle.read()


# ---------------------------------------------------------------------------
# The table a standard action exports
# ---------------------------------------------------------------------------

def test_table_of_finds_the_field_marked_role_table():
    result = Outputs(rows=[Row(zone="A", area=1.5), Row(zone="B", area=2.0)], total=3.5)

    headers, rows = outputs.table_of(result)

    assert headers == ["zone", "area"]
    assert rows == [["A", 1.5], ["B", 2.0]]


def test_a_model_with_no_table_reports_none_rather_than_an_empty_table():
    # None and [] mean different things to the palette: None greys the button
    # out, [] offers an export that writes a header and nothing else.
    assert outputs.table_of(NoTable(count=3)) is None
    assert outputs.table_of(Outputs()) == ([], [])


def test_table_of_accepts_plain_dict_rows():
    class DictRows(NodeModel):
        rows: list[dict] = Field(default=[], json_schema_extra=output(role="table"))

    headers, rows = outputs.table_of(DictRows(rows=[{"a": 1, "b": 2}]))
    assert headers == ["a", "b"]
    assert rows == [[1, 2]]


# ---------------------------------------------------------------------------
# run_action — the palette's one entry point into the standard set
# ---------------------------------------------------------------------------

def test_run_action_csv_needs_no_command_code():
    result = Outputs(rows=[Row(zone="A", area=1.5)])

    artifact = outputs.run_action("csv", "myCommand", outputs_obj=result)

    assert artifact.kind == "csv"
    with open(artifact.path, encoding="utf-8-sig") as handle:
        assert handle.read().splitlines() == ["zone,area", "A,1.5"]


def test_run_action_json_writes_one_object_per_row():
    import json

    result = Outputs(rows=[Row(zone="A", area=1.5)])
    artifact = outputs.run_action("json", "myCommand", outputs_obj=result)

    with open(artifact.path, encoding="utf-8") as handle:
        assert json.load(handle) == [{"zone": "A", "area": 1.5}]


def test_an_unknown_action_names_the_ones_that_exist():
    with pytest.raises(OutputError) as caught:
        outputs.run_action("xlsx", "myCommand")

    assert "xlsx" in str(caught.value)
    assert "csv" in str(caught.value), "the refusal has to say what IS available"


def test_a_table_action_on_a_model_with_no_table_says_how_to_fix_it():
    with pytest.raises(OutputError) as caught:
        outputs.run_action("csv", "myCommand", outputs_obj=NoTable(count=1))

    message = str(caught.value)
    assert "NoTable" in message
    assert 'role="table"' in message, "the refusal must name the fix, not just the fault"


def test_bake_without_a_plan_says_the_command_must_declare_one():
    with pytest.raises(OutputError) as caught:
        outputs.run_action("bake", "myCommand", outputs_obj=Outputs())

    assert "plan=" in str(caught.value)


def test_layout_and_worksheet_refuse_the_generic_path_by_name():
    # Both need names only the command knows, so there is no framework version.
    # Refusing with the call to make is the difference between a dead button and
    # a signpost.
    for action, expected in (("layout", "to_layout"), ("worksheet", "to_worksheet")):
        with pytest.raises(OutputError) as caught:
            outputs.run_action(action, "myCommand")
        assert expected in str(caught.value)


# ---------------------------------------------------------------------------
# schema.output
# ---------------------------------------------------------------------------

def test_output_refuses_an_unknown_role():
    with pytest.raises(ValueError) as caught:
        output(role="chart")
    assert "chart" in str(caught.value)
    assert "table" in str(caught.value)


def test_output_and_port_are_separate_namespaces_in_the_schema():
    from evp.schema import port

    assert "x-output" in output(role="table")
    assert "x-port" in port(unit="m")


# ---------------------------------------------------------------------------
# The two halves of the standard set
# ---------------------------------------------------------------------------

def test_every_standard_action_has_a_button_label_and_vice_versa():
    # The button text lives in _scanner (which must read a command without
    # executing it, and so cannot import a sibling) and the routing lives here.
    # One source for each fact — and this is what stops them drifting.
    from evp import _scanner

    assert set(outputs.STANDARD_ACTIONS) == set(_scanner.STANDARD_ACTION_LABELS)


def test_every_standard_action_declares_what_it_needs():
    assert set(outputs.STANDARD_ACTIONS.values()) <= {"table", "plan", "view"}


# ---------------------------------------------------------------------------
# An action reads the STORED result — it never re-runs the command
# ---------------------------------------------------------------------------

def test_a_stored_dict_exports_exactly_as_the_live_model_does(tmp_path):
    import evp
    from evp import _invoke, _planstore

    live = Outputs(rows=[Row(zone="A", area=1.5)])
    folder = str(tmp_path / "MyCommand")
    os.makedirs(folder, exist_ok=True)
    _planstore.save_outputs(folder, live)

    @evp.command(title="Exporter", inputs=NoTable, outputs=Outputs,
                 actions=["csv"])
    def run(ctx, inputs):
        raise AssertionError("run() must not be called to serve an action button")

    artifact = _invoke.run_action(run, "csv", folder=folder)

    with open(artifact.path, encoding="utf-8-sig") as handle:
        assert handle.read().splitlines() == ["zone,area", "A,1.5"]


def test_an_action_with_nothing_stored_says_to_run_the_command_first(tmp_path):
    import evp
    from evp import _invoke

    @evp.command(title="Exporter", inputs=NoTable, outputs=Outputs, actions=["csv"])
    def run(ctx, inputs):
        return Outputs()

    with pytest.raises(_invoke.InvokeError) as caught:
        _invoke.run_action(run, "csv", folder=str(tmp_path / "Never Run"))

    assert "Run the command first" in str(caught.value)


def test_an_action_the_command_never_declared_is_refused_by_name(tmp_path):
    import evp
    from evp import _invoke

    @evp.command(title="Exporter", inputs=NoTable, outputs=Outputs, actions=["csv"])
    def run(ctx, inputs):
        return Outputs()

    with pytest.raises(_invoke.InvokeError) as caught:
        _invoke.run_action(run, "pdf", folder=str(tmp_path))

    assert "pdf" in str(caught.value)
    assert "csv" in str(caught.value)
