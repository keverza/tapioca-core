"""Task-specific selection roles use the same literal show_when rules as inputs."""

import importlib.util
import json
from pathlib import Path

import pytest

SCANNER = Path(__file__).parents[1] / "Sources/PyPackage/evp/_scanner.py"
SPEC = importlib.util.spec_from_file_location("_scanner_role_visibility", SCANNER)
scanner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scanner)


def scan(tmp_path, declaration, annotation='tapioca.Action("Roof", "Parking")'):
    entry = tmp_path / "command.py"
    entry.write_text(
        f'import tapioca\n@tapioca.command(selection_sets=("Roof slab", "Parking regions"), {declaration})\n'
        f'def run(task: {annotation} = "Roof"):\n    pass\n',
        encoding="utf-8",
    )
    return scanner.scan_file(str(entry), "Roles")


def test_rules_are_flat_json_strings_parallel_to_role_names(tmp_path):
    meta = scan(
        tmp_path, 'selection_sets_show_when={"Roof slab": {"task": "Roof"}, "Parking regions": {"task": "Parking"}}'
    )
    assert "selection_sets_show_when" not in meta
    rules = [json.loads(value) for value in meta["selection_set_rules_json"]]
    assert rules == [
        {"show_when_param": "task", "show_when_values": ["Roof"]},
        {"show_when_param": "task", "show_when_values": ["Parking"]},
    ]


def test_existing_commands_keep_unconditional_roles(tmp_path):
    meta = scan(tmp_path, "")
    assert [json.loads(value) for value in meta["selection_set_rules_json"]] == [
        {"show_when_param": "", "show_when_values": []},
        {"show_when_param": "", "show_when_values": []},
    ]


@pytest.mark.parametrize(
    "spec",
    [
        "[]",
        '{"Missing role": {"task": "Roof"}}',
        '{"Roof slab": {"missing": "Roof"}}',
        '{"Roof slab": {"task": "Typo"}}',
        '{"Roof slab": {"task": []}}',
        '{"Roof slab": "Roof"}',
    ],
)
def test_invalid_role_rules_fail_loudly(tmp_path, spec):
    with pytest.raises(scanner.ScanError):
        scan(tmp_path, "selection_sets_show_when=" + spec)


def test_open_text_controllers_are_not_supported(tmp_path):
    with pytest.raises(scanner.ScanError, match="Action, Enum or Bool"):
        scan(tmp_path, 'selection_sets_show_when={"Roof slab": {"task": "Roof"}}', "tapioca.String()")


def test_native_role_visibility_preserves_captures_and_reflows_on_task_change():
    root = Path(__file__).parents[1] / "Sources/AddOn/Palette"
    source = (root / "SelectionSetPanel.cpp").read_text(encoding="utf-8")
    visibility = source[
        source.index("bool SelectionSetPanel::RefreshVisibility") : source.index("short SelectionSetPanel::PlaceAt")
    ]
    assert "MatchesVisibility" in visibility
    assert "Clear" not in visibility and "Configure" not in visibility
    layout = source[source.index("short SelectionSetPanel::PlaceAt") : source.index("bool SelectionSetPanel::Apply")]
    assert "if (!row.visible)" in layout and "continue;" in layout
    events = (root / "ControlPaletteInputEvents.cpp").read_text(encoding="utf-8")
    assert "selectionSets.RefreshVisibility (params) || reflow" in events
