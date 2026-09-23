"""Mutually exclusive selection sets are declared statically, like the sets."""

import importlib.util
from pathlib import Path

import pytest

_SCANNER = Path(__file__).parents[1] / "Sources" / "PyPackage" / "evp" / "_scanner.py"
_SPEC = importlib.util.spec_from_file_location("_scanner_exclusive_sets", _SCANNER)
scanner = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(scanner)


def scan(tmp_path, arguments):
    entry = tmp_path / "command.py"
    entry.write_text(
        "import evp\n@evp.command(%s)\ndef run():\n    pass\n" % arguments,
        encoding="utf-8",
    )
    return scanner.scan_file(str(entry), "Exclusive")


def test_the_flag_survives_the_scan(tmp_path):
    meta = scan(tmp_path, "selection_sets=('Analysis', 'Context', 'Ignored'), "
                          "exclusive_selection_sets=True")
    assert meta["exclusive_selection_sets"] is True


def test_it_defaults_to_off(tmp_path):
    meta = scan(tmp_path, "selection_sets=('Analysis', 'Context')")
    assert meta["exclusive_selection_sets"] is False


@pytest.mark.parametrize(
    "arguments",
    [
        # Exclusive among fewer than two sets means nothing -- refused, not ignored.
        "selection_sets=('Objects',), exclusive_selection_sets=True",
        "exclusive_selection_sets=True",
        "selection_sets=('A', 'B'), exclusive_selection_sets=1",
    ],
)
def test_malformed_declarations_are_refused(tmp_path, arguments):
    with pytest.raises(scanner.ScanError):
        scan(tmp_path, arguments)
