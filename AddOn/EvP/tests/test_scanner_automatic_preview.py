"""Safe automatic selection-preview metadata is entirely static."""

import importlib.util
import json
from pathlib import Path

import pytest

_SCANNER = Path(__file__).parents[1] / "Sources" / "PyPackage" / "evp" / "_scanner.py"
_SPEC = importlib.util.spec_from_file_location("_scanner_automatic_preview", _SCANNER)
scanner = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(scanner)


def scan(tmp_path, arguments):
    entry = tmp_path / "command.py"
    entry.write_text(
        "import evp\n@evp.command(%s)\ndef run(apply_changes: bool = True):\n    pass\n" % arguments,
        encoding="utf-8",
    )
    return scanner.scan_file(str(entry), "Automatic")


def test_opt_in_and_canonical_forced_overrides_survive_scan(tmp_path):
    meta = scan(
        tmp_path,
        "selection_sets=('Reference', 'Objects'), preview_on_selection=True, "
        "preview_overrides={'apply_changes': False, 'options': [1, None]}",
    )

    assert meta["preview_on_selection"] is True
    assert json.loads(meta["preview_overrides_json"]) == {
        "apply_changes": False,
        "options": [1, None],
    }


@pytest.mark.parametrize(
    "arguments",
    [
        "selection_sets=('Objects',), preview_on_selection=True",
        "preview_on_selection=True, preview_overrides={'apply_changes': False}",
        "selection_sets=('Objects',), preview_on_selection=1, preview_overrides={'apply_changes': False}",
        "selection_sets=('Objects',), preview_on_selection=True, preview_overrides={1: False}",
        "selection_sets=('Objects',), preview_on_selection=True, preview_overrides={'x': (1, 2)}",
    ],
)
def test_unsafe_or_incomplete_opt_in_is_a_scan_error(tmp_path, arguments):
    with pytest.raises(scanner.ScanError):
        scan(tmp_path, arguments)


def test_default_never_opts_a_command_in(tmp_path):
    meta = scan(tmp_path, "selection_sets=('Objects',)")
    assert meta["preview_on_selection"] is False
    assert meta["preview_overrides_json"] == "{}"
