"""End to end: a schema-style command must reach the palette with real controls.

Kept OUTSIDE PyPackage so it never ships with the add-on. Run:
    python -m pytest AddOn/EvP/tests/test_schema_command_scan.py

This exercises the whole Phase-2 path — AST scan for identity, subprocess import
for ports, cache in between — against command folders written to a tmp_path. It
is the test that would catch the failure the scanner gate exists for: a command
that does not appear in the palette at all.
"""

import json
import os
import subprocess
import sys
import textwrap

import pytest

pytest.importorskip("pydantic", reason="pydantic ships in the Tapioca runtime baseline")

_PACKAGE = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
if _PACKAGE not in sys.path:
    sys.path.insert(0, _PACKAGE)


@pytest.fixture(autouse=True)
def isolated_cache(tmp_path, monkeypatch):
    """Point evp.paths at a throwaway root so the cache never touches the user's.

    EVP_HOME is the documented override and the dry-run harness's own seam; a
    test that wrote into %LOCALAPPDATA%\\Tapioca would poison a real Rescan.
    """
    monkeypatch.setenv("EVP_HOME", str(tmp_path / "home"))
    from evp import _schemacache

    _schemacache.clear()
    yield
    _schemacache.clear()


def _write(folder, source):
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "command.py"), "w", encoding="utf-8") as handle:
        handle.write(textwrap.dedent(source))
    return folder


_GOOD = '''
    """A schema-style probe."""
    import tapioca
    from tapioca.schema import Field, NodeModel, port


    class Inputs(NodeModel):
        plot_area: float = Field(default=0.0, ge=0.0, title="Sklypo plotas",
                                 description="Plot area from Project Info.",
                                 json_schema_extra=port(unit="m2"))
        layer: str = Field(default="Annotation",
                           json_schema_extra=port(control="layer"))


    class Outputs(NodeModel):
        count: int = Field(default=0, ge=0)


    @tapioca.command(title="Schema Probe", category="Diagnostics",
                     description="Ports come from the model.",
                     inputs=Inputs, outputs=Outputs, needs_preview=True)
    def run(ctx, inputs):
        return Outputs(count=1)
    '''


def test_a_schema_command_reaches_the_palette_with_its_ports(tmp_path):
    from evp import _scanner

    _write(str(tmp_path / "SchemaProbe"), _GOOD)
    result = _scanner.scan_root(str(tmp_path))

    assert result["diagnostics"] == []
    assert len(result["commands"]) == 1
    command = result["commands"][0]

    # Identity still comes from the AST, so it is available even when the
    # module cannot be imported (see the next test).
    assert command["title"] == "Schema Probe"
    assert command["inputs"] == "Inputs"
    assert command["needs_preview"] is True

    by_name = {p["name"]: p for p in command["params"]}
    assert by_name["plot_area"]["type"] == "Float"
    assert by_name["plot_area"]["unit"] == "m2"
    assert by_name["plot_area"]["minimum"] == 0.0
    assert by_name["plot_area"]["label"] == "Sklypo plotas"
    assert by_name["plot_area"]["help"] == "Plot area from Project Info."
    assert by_name["layer"]["type"] == "Layer"

    # paramsJson is what actually crosses to C++: a list of JSON STRINGS, one
    # flat object each. A nested array would arrive as nothing.
    assert len(command["paramsJson"]) == 2
    assert json.loads(command["paramsJson"][0])["name"] == "plot_area"


def test_a_command_that_dies_on_import_still_appears_carrying_its_reason(tmp_path):
    from evp import _scanner

    _write(str(tmp_path / "BrokenProbe"), '''
        import tapioca
        from tapioca.schema import Field, NodeModel

        raise RuntimeError("boom at import time")


        class Inputs(NodeModel):
            x: float = Field(default=1.0)


        @tapioca.command(title="Broken Probe", inputs=Inputs)
        def run(ctx, inputs):
            pass
        ''')

    result = _scanner.scan_root(str(tmp_path))
    # The whole point: it is LISTED, with its reason. Its identity came from the
    # AST and is intact; only the controls are missing. Sending it to
    # `diagnostics` instead removed it from the command list, and a command that
    # vanishes looks exactly like a failed build.
    assert len(result["commands"]) == 1
    command = result["commands"][0]
    assert command["title"] == "Broken Probe"
    assert command["params"] == []
    assert "boom at import time" in command["scan_error"]
    assert "boom at import time" in command["description"]
    # ASCII only: this text is scanner-generated and passes through a cp1252
    # console, where one decorative glyph would take the whole scan down.
    command["description"].encode("ascii")


def test_a_schema_command_must_define_run_ctx_inputs(tmp_path):
    from evp import _scanner

    _write(str(tmp_path / "BadSignature"), '''
        import tapioca
        from tapioca.schema import Field, NodeModel


        class Inputs(NodeModel):
            x: float = Field(default=1.0)


        @tapioca.command(title="Bad", inputs=Inputs)
        def run(x: float = 1.0):
            pass
        ''')

    result = _scanner.scan_root(str(tmp_path))
    assert result["commands"] == []
    assert "run(ctx, inputs)" in result["diagnostics"][0]["error"]


def test_outputs_or_plan_without_inputs_is_refused():
    import evp

    with pytest.raises(TypeError, match="need inputs="):
        evp.command(title="X", outputs=object)


def test_a_computed_inputs_argument_is_refused(tmp_path):
    from evp import _scanner

    _write(str(tmp_path / "ComputedInputs"), '''
        import tapioca

        MODELS = {}


        @tapioca.command(title="Computed", inputs=MODELS["a"])
        def run(ctx, inputs):
            pass
        ''')

    result = _scanner.scan_root(str(tmp_path))
    assert "must name a class or function" in result["diagnostics"][0]["error"]


def test_a_signature_style_command_still_scans_without_a_subprocess(tmp_path):
    from evp import _scanner

    _write(str(tmp_path / "LegacyProbe"), '''
        import evp

        @evp.command(title="Legacy Probe", category="Diagnostics")
        def run(count: evp.Int(minimum=1) = 4, dry_run: bool = True):
            pass
        ''')

    result = _scanner.scan_root(str(tmp_path))
    command = result["commands"][0]
    assert command["title"] == "Legacy Probe"
    assert [p["name"] for p in command["params"]] == ["count", "dry_run"]
    # No inputs= means no schema generation at all — the 60+ existing commands
    # must not pay a subprocess each on every Rescan.
    assert "inputs" not in command


# --------------------------------------------------------------------------
# The cache
# --------------------------------------------------------------------------

def test_the_cache_is_reused_until_the_source_changes(tmp_path, monkeypatch):
    from evp import _schemacache

    folder = _write(str(tmp_path / "SchemaProbe"), _GOOD)

    first = _schemacache.ports_for(folder)
    assert first["ok"]

    calls = []
    real_run = subprocess.run

    def counting_run(*args, **kwargs):
        calls.append(args)
        return real_run(*args, **kwargs)

    monkeypatch.setattr(subprocess, "run", counting_run)

    assert _schemacache.ports_for(folder) == first
    assert calls == [], "a cache hit must not start a subprocess"

    # Content change -> regenerate. The key is a content hash, not an mtime,
    # because Sync-Commands.ps1 rewrites mtimes without changing content.
    _write(folder, _GOOD.replace('title="Sklypo plotas"', 'title="Plot"'))
    regenerated = _schemacache.ports_for(folder)
    assert len(calls) == 1
    assert regenerated["params"][0]["label"] == "Plot"


def test_the_generator_reports_a_module_with_no_command(tmp_path):
    from evp import _schemagen

    folder = str(tmp_path / "NotACommand")
    os.makedirs(folder)
    with open(os.path.join(folder, "command.py"), "w", encoding="utf-8") as handle:
        handle.write("x = 1\n")

    result = _schemagen.generate(folder)
    assert result["ok"] is False
    assert "no run()" in result["error"]


def test_the_generator_never_raises(tmp_path):
    from evp import _schemagen

    # A folder with no command.py at all — the caller is a scanner whose job is
    # to report a broken command, not to fail with it.
    result = _schemagen.generate(str(tmp_path / "does-not-exist"))
    assert result["ok"] is False


# --------------------------------------------------------------------------
# Regressions from the first in-Archicad run (2026-08-19)
# --------------------------------------------------------------------------

def test_the_interpreter_is_never_sys_executable_when_a_runtime_python_exists(
        tmp_path, monkeypatch):
    """⚠️ Inside Archicad `sys.executable` is not a launchable Python.

    EvPPy.cpp sets program_name to the literal "EvP", so handing sys.executable
    to subprocess bought a hang and then a timeout. The runtime's own python.exe
    sits at sys.prefix, because EvPPy sets PyConfig.home to the runtime home.
    """
    from evp import _schemacache

    runtime = tmp_path / "runtime"
    runtime.mkdir()
    exe = runtime / ("python.exe" if os.name == "nt" else "python3")
    exe.write_text("", encoding="utf-8")

    monkeypatch.setattr(sys, "prefix", str(runtime))
    monkeypatch.setattr(sys, "executable", r"C:\Program Files\Archicad 29\Archicad.exe")

    assert _schemacache._interpreter() == str(exe)


def test_no_runtime_python_is_reported_with_the_path_it_looked_in(monkeypatch, tmp_path):
    from evp import _schemacache

    monkeypatch.setattr(sys, "prefix", str(tmp_path / "nowhere"))
    monkeypatch.setattr(sys, "base_prefix", str(tmp_path / "nowhere"))
    monkeypatch.setattr(sys, "executable", r"C:\Archicad.exe")

    assert _schemacache._interpreter() is None
    result = _schemacache._generate(str(tmp_path))
    assert result["ok"] is False
    # "not found" with no path is the information-free failure this repo bans.
    assert "nowhere" in result["error"]
    assert "Install-Runtime" in result["error"]


def test_a_command_whose_ports_fail_STILL_APPEARS_in_the_list(tmp_path, monkeypatch):
    """The cardinal rule: a command that vanishes looks like a failed build.

    Raising here sent the command to `diagnostics`, which the command LIST does
    not render — so one misresolved interpreter path made it disappear entirely.
    """
    from evp import _scanner

    _write(str(tmp_path / "SchemaProbe"), _GOOD)

    monkeypatch.setattr(
        "evp._schemacache.ports_for",
        lambda folder, package_root=None: {"ok": False, "error": "pydantic is missing"},
    )

    result = _scanner.scan_root(str(tmp_path))

    assert len(result["commands"]) == 1, "the command must still be listed"
    command = result["commands"][0]
    assert command["title"] == "Schema Probe"
    assert command["params"] == []
    assert command["scan_error"] == "pydantic is missing"
    # The reason rides in the description because that is a surface the palette
    # already renders; a new field would need a C++ change to be visible.
    assert "pydantic is missing" in command["description"]
    assert result["diagnostics"] == []


def test_the_cache_key_separates_two_folders_with_the_same_name(tmp_path):
    """The repo copy and the deployed copy share a basename.

    Keyed by name alone they fought over one file and each invalidated the
    other's entry on every run — a permanent miss that reads as a broken cache.
    """
    from evp import _schemacache

    repo = tmp_path / "repo" / "SchemaProbe"
    deployed = tmp_path / "deployed" / "SchemaProbe"
    _write(str(repo), _GOOD)
    _write(str(deployed), _GOOD)

    assert _schemacache._cache_path(str(repo)) != _schemacache._cache_path(str(deployed))


def test_an_authored_title_survives_even_when_it_matches_the_generated_one(tmp_path):
    """Seen live: `spacing`, `columns` and `layer` rendered as raw identifiers.

    Each had `title="Spacing"` / `"Columns"` / `"Layer"` written by hand, and
    each was discarded because it equalled the title pydantic would have
    generated. The schema alone cannot tell the two apart; model_fields can, and
    _schemagen reads it there.
    """
    from evp import _scanner

    _write(str(tmp_path / "TitleProbe"), '''
        import tapioca
        from tapioca.schema import Field, NodeModel, port


        class Inputs(NodeModel):
            spacing: float = Field(default=1.5, title="Spacing",
                                   json_schema_extra=port(unit="m"))
            columns: int = Field(default=3, title="Columns")
            count: int = Field(default=6, title="Columns to place")
            untitled: int = 1


        @tapioca.command(title="Title Probe", inputs=Inputs)
        def run(ctx, inputs):
            pass
        ''')

    command = _scanner.scan_root(str(tmp_path))["commands"][0]
    by_name = {p["name"]: p for p in command["params"]}

    assert by_name["spacing"]["label"] == "Spacing"
    assert by_name["columns"]["label"] == "Columns"
    assert by_name["count"]["label"] == "Columns to place"
    # A field nobody titled still gets no label, so the palette falls back to the
    # parameter name rather than inventing display text.
    assert "label" not in by_name["untitled"]
