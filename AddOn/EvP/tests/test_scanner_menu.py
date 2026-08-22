"""What the scanner accepts, rejects and flattens for @tapioca.menu.

Kept OUTSIDE PyPackage so it never ships with the add-on. Run:
    python -m pytest AddOn/EvP/tests/test_scanner_menu.py

Three things are being pinned here:

  * the FLAT PARALLEL SHAPE. A command's right-click entries leave the scanner as
    `menu_items` + `menu_labels` + `menu_regions`, three arrays of the same length
    in declaration order — the C++ side reads string arrays out of ObjectState
    reliably and nested objects it does not, so a list of dicts would arrive as
    nothing at all and the menu would simply be empty.
  * the DIAGNOSTICS. A region that is not a region, or a "param:<name>" naming a
    parameter that does not exist, is an entry that can never appear. As a Rescan
    error it is a five-second fix; as silence it is an afternoon of right-clicking.
  * the SEPARATION from actions. @menu entries must NOT become action-bar buttons,
    and @action functions must not become menu entries. They share a runtime
    contract, not a place on the palette.
"""

import importlib.util
import os

import pytest

_SCANNER_PATH = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage", "evp", "_scanner.py")


def _load():
    spec = importlib.util.spec_from_file_location("_evp_scanner_menu_undertest", _SCANNER_PATH)
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


COMMAND = """
import tapioca

%s

@tapioca.command(title="T", category="C")
def run(offset: tapioca.Float(unit="m") = 0.1, label: tapioca.Text() = "x"):
    pass
"""


def test_entries_are_three_parallel_arrays_in_declaration_order(tmp_path):
    meta = scan(
        COMMAND
        % """
@tapioca.menu("Anywhere")
def anywhere(ctx, outputs):
    pass

@tapioca.menu("Any Control", region="params")
def any_control(ctx, outputs):
    pass

@tapioca.menu("Just Offset", region="param:offset")
def just_offset(ctx, outputs):
    pass
""",
        tmp_path,
    )

    assert meta["menu_items"] == ["anywhere", "any_control", "just_offset"]
    assert meta["menu_labels"] == ["Anywhere", "Any Control", "Just Offset"]
    assert meta["menu_regions"] == ["panel", "params", "param:offset"]


def test_region_defaults_to_panel(tmp_path):
    meta = scan(
        COMMAND
        % """
@tapioca.menu("Anywhere")
def anywhere(ctx, outputs):
    pass
""",
        tmp_path,
    )
    assert meta["menu_regions"] == ["panel"]


def test_name_keyword_overrides_the_function_name(tmp_path):
    meta = scan(
        COMMAND
        % """
@tapioca.menu("Anywhere", name="explicit")
def anywhere(ctx, outputs):
    pass
""",
        tmp_path,
    )
    assert meta["menu_items"] == ["explicit"]


def test_a_command_with_no_menu_still_emits_the_three_arrays(tmp_path):
    meta = scan(COMMAND % "", tmp_path)
    assert meta["menu_items"] == []
    assert meta["menu_labels"] == []
    assert meta["menu_regions"] == []


def test_every_region_name_is_accepted(tmp_path):
    meta = scan(
        COMMAND
        % """
@tapioca.menu("A", region="panel")
def a(ctx, outputs):
    pass

@tapioca.menu("B", region="params")
def b(ctx, outputs):
    pass

@tapioca.menu("C", region="commands")
def c(ctx, outputs):
    pass

@tapioca.menu("D", region="results")
def d(ctx, outputs):
    pass
""",
        tmp_path,
    )
    assert meta["menu_regions"] == ["panel", "params", "commands", "results"]


def test_unknown_region_is_a_scan_error_naming_the_alternatives(tmp_path):
    with pytest.raises(scanner.ScanError) as excinfo:
        scan(
            COMMAND
            % """
@tapioca.menu("Nope", region="sidebar")
def nope(ctx, outputs):
    pass
""",
            tmp_path,
        )
    message = str(excinfo.value)
    assert "sidebar" in message
    assert "panel" in message and "results" in message


def test_param_region_naming_an_undeclared_parameter_is_a_scan_error(tmp_path):
    with pytest.raises(scanner.ScanError) as excinfo:
        scan(
            COMMAND
            % """
@tapioca.menu("Nope", region="param:nosuch")
def nope(ctx, outputs):
    pass
""",
            tmp_path,
        )
    message = str(excinfo.value)
    assert "nosuch" in message
    # It says what IS declared, so the fix does not need a second look at the file.
    assert "offset" in message and "label" in message


def test_param_region_with_no_name_after_the_colon_is_a_scan_error(tmp_path):
    with pytest.raises(scanner.ScanError) as excinfo:
        scan(
            COMMAND
            % """
@tapioca.menu("Nope", region="param:")
def nope(ctx, outputs):
    pass
""",
            tmp_path,
        )
    assert "parameter name" in str(excinfo.value)


def test_a_non_literal_label_is_a_scan_error(tmp_path):
    with pytest.raises(scanner.ScanError):
        scan(
            COMMAND
            % """
TEXT = "computed"

@tapioca.menu(TEXT)
def nope(ctx, outputs):
    pass
""",
            tmp_path,
        )


def test_menu_entries_are_not_action_buttons_and_actions_are_not_menu_entries(tmp_path):
    meta = scan(
        COMMAND
        % """
@tapioca.menu("In The Menu")
def in_the_menu(ctx, outputs):
    pass

@tapioca.action("On The Bar")
def on_the_bar(ctx, outputs):
    pass
""",
        tmp_path,
    )

    assert meta["menu_items"] == ["in_the_menu"]
    assert meta["actions"] == ["on_the_bar"]
    assert meta["action_labels"] == ["On The Bar"]


def test_a_schema_command_skips_the_parameter_check(tmp_path):
    # `inputs=` ports live in a class body no AST pass can read, so there is nothing
    # to check "param:<name>" against — and refusing the entry would be refusing it
    # for a reason the scanner cannot actually establish.
    source = """
import tapioca

class Inputs:
    pass

@tapioca.menu("Pinned", region="param:whatever")
def pinned(ctx, outputs):
    pass

@tapioca.command(title="T", category="C", inputs=Inputs)
def run(ctx, inputs):
    pass
"""
    meta = scan(source, tmp_path)
    assert meta["menu_regions"] == ["param:whatever"]


def test_a_menu_entry_is_dispatched_as_an_action_at_runtime(tmp_path):
    """The claim the whole design rests on: @menu adds a PLACE, not a mechanism.

    `run_action` is what the palette calls for an action-bar button; a @menu
    function has to be reachable through exactly that, with no second path — and
    with `outputs` None, because a right-click can come before any run.
    """
    import sys
    import types

    package = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage")
    if package not in sys.path:
        sys.path.insert(0, package)
    from evp import _invoke

    module = types.ModuleType("_evp_menu_dispatch_undertest")
    sys.modules[module.__name__] = module
    try:
        exec(
            compile(
                """
import evp

@evp.menu("From The Menu", region="params")
def from_the_menu(ctx, outputs):
    return ("ran", outputs)

@evp.command(title="T", category="C")
def run(offset: evp.Float(unit="m") = 0.1):
    pass
""",
                module.__name__,
                "exec",
            ),
            module.__dict__,
        )

        assert sorted(_invoke._custom_actions(module.run)) == ["from_the_menu"]
        assert _invoke.run_action(module.run, "from_the_menu", folder=None) == ("ran", None)
    finally:
        del sys.modules[module.__name__]


def test_the_clicked_region_reaches_the_entry_as_ctx_region(tmp_path):
    """The palette resolves WHERE the click landed; the entry has to be able to
    read it, or an entry declared for a whole area cannot tell which control it
    was aimed at. `ctx.param` is the parameter name out of a "param:<name>"."""
    import sys
    import types

    package = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage")
    if package not in sys.path:
        sys.path.insert(0, package)
    from evp import _invoke

    module = types.ModuleType("_evp_menu_region_undertest")
    sys.modules[module.__name__] = module
    try:
        exec(
            compile(
                """
import evp

@evp.menu("Any Control", region="params")
def any_control(ctx, outputs):
    return (ctx.region, ctx.param, ctx.mode)

@evp.command(title="T", category="C")
def run(offset: evp.Float(unit="m") = 0.1):
    pass
""",
                module.__name__,
                "exec",
            ),
            module.__dict__,
        )

        # Aimed at one row: the entry can act on that parameter by name.
        assert _invoke.run_action(module.run, "any_control", folder=None, region="param:offset") == (
            "param:offset",
            "offset",
            "action",
        )
        # Aimed at the block: a region, but no single parameter.
        assert _invoke.run_action(module.run, "any_control", folder=None, region="params") == (
            "params",
            "",
            "action",
        )
        # Not from the menu at all — an action-bar button. `if ctx.region:` is
        # what tells the two apart, so it must be falsey and never None.
        assert _invoke.run_action(module.run, "any_control", folder=None) == ("", "", "action")
    finally:
        del sys.modules[module.__name__]
