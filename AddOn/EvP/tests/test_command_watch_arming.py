import importlib.util
import io
import os
import sys

import pytest

_PACKAGE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
if _PACKAGE not in sys.path:
    sys.path.insert(0, _PACKAGE)

import evp  # noqa: E402
from evp import _invoke, _watchstore  # noqa: E402


def _external_main_module():
    path = os.path.join(_PACKAGE, "_evp_external_main.py")
    spec = importlib.util.spec_from_file_location("_evp_external_main_undertest", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize(("environment", "expected"), (("1", True), ("0", False)))
def test_external_runner_forwards_watch_arming(monkeypatch, tmp_path, environment, expected):
    folder = tmp_path / "Command"
    folder.mkdir()
    (folder / "command.py").write_text(
        "import evp\n@evp.command(title='Command')\ndef run():\n    return None\n",
        encoding="utf-8",
    )
    runner = _external_main_module()
    seen = []
    monkeypatch.setattr(runner, "_handshake", lambda: None)
    monkeypatch.setattr(sys, "stdin", io.StringIO("{}"))
    monkeypatch.setenv("EVP_COMMAND_DIR", str(folder))
    monkeypatch.setenv("EVP_WATCH", environment)
    monkeypatch.delenv("EVP_ACTION", raising=False)
    monkeypatch.setattr(_invoke, "invoke", lambda *args, **kwargs: seen.append(kwargs["watch_armed"]))

    assert runner.main() == 0
    assert seen == [expected]


def test_explicitly_unarmed_run_ignores_inherited_watch_environment(monkeypatch, tmp_path):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    monkeypatch.setenv("EVP_WATCH", "1")
    monkeypatch.setattr(evp.api, "call", lambda *args, **kwargs: pytest.fail("unarmed run published a trace"))

    @evp.command(title="Unarmed")
    def run():
        evp.watch.point(object())

    folder = str(tmp_path / "Unarmed")
    _invoke.invoke(run, {}, folder=folder, watch_armed=False)

    assert _watchstore.load(folder) is None
