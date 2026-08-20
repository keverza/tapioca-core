"""PLAT-4 scanner contract for command version declarations."""
import importlib.util
import os

import pytest

_SCANNER_PATH = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage", "evp",
                             "_scanner.py")


def _load():
    spec = importlib.util.spec_from_file_location("_evp_scanner_versions", _SCANNER_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


scanner = _load()


def scan(source, tmp_path):
    entry = tmp_path / "command.py"
    entry.write_text(source, encoding="utf-8")
    return scanner.scan_file(str(entry), "Probe")


def test_version_requirements_survive_scan(tmp_path):
    meta = scan('''
import evp

@evp.command(requires_api=">=1.0 <2.0", requires_tapir=">=1.5.2")
def run():
    pass
''', tmp_path)
    assert meta["requires_api"] == ">=1.0 <2.0"
    assert meta["requires_tapir"] == ">=1.5.2"


@pytest.mark.parametrize("source", [
    '''
import tapioca

@tapioca.command()
def run():
    pass
''',
    '''
from tapioca import command

@command()
def run():
    pass
''',
])
def test_tapioca_namespace_spellings_survive_scan(tmp_path, source):
    assert scan(source, tmp_path)["title"] == "Probe"


@pytest.mark.parametrize("argument", ["requires_api=\"1.0\"", "requires_tapir=\"=>1.5\"",
                                      "requires_tapir=[]"])
def test_invalid_version_requirement_is_a_scan_diagnostic(tmp_path, argument):
    source = "import evp\n@evp.command(%s)\ndef run():\n    pass\n" % argument
    with pytest.raises(scanner.ScanError):
        scan(source, tmp_path)
