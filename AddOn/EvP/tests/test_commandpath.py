"""PLAT-3 — the three import sources, their precedence, and the eviction.

Kept OUTSIDE PyPackage so it never ships with the add-on. Run:
    python -m pytest AddOn/EvP/tests/test_commandpath.py

Every one of these fails SILENTLY in production if it regresses:

  * a `_lib/` module that does not resolve looks like a typo in the command;
  * a sibling folder importable WITHOUT its `_exports.py` marker turns every
    command folder into public API by accident, so a rename breaks a stranger;
  * a shadowed `evp` breaks the bus from a stray file in someone's folder;
  * and a module that is NOT evicted keeps serving its stale version for the rest
    of the Archicad session — an AttributeError on a function that plainly exists
    in the file, which reads like a typo and is not (the SunStudy bug).

The eviction test is the one worth the setup cost: it REWRITES a helper between
two runs, which is exactly what a person editing a command does.
"""
import importlib.util
import os
import sys

import pytest

_MODULE_PATH = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage",
                            "evp", "_commandpath.py")


def _load():
    spec = importlib.util.spec_from_file_location("_evp_commandpath_undertest",
                                                  _MODULE_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture
def cp():
    return _load()


@pytest.fixture
def root(tmp_path):
    """A scripts root shaped like the real one: two commands, one exporting, and
    a `_lib/`."""
    (tmp_path / "MyCommand").mkdir()
    (tmp_path / "MyCommand" / "command.py").write_text("def run(): pass\n")
    (tmp_path / "MyCommand" / "helper.py").write_text("WHO = 'own'\n")
    # Same module NAME in the command folder and in _lib -- the clash the
    # precedence rule exists for.
    (tmp_path / "_lib").mkdir()
    (tmp_path / "_lib" / "helper.py").write_text("WHO = 'lib'\n")
    (tmp_path / "_lib" / "shared.py").write_text("VALUE = 1\n")

    (tmp_path / "Exporter").mkdir()
    (tmp_path / "Exporter" / "command.py").write_text("def run(): pass\n")
    (tmp_path / "Exporter" / "_exports.py").write_text("MARKER = 'exported'\n")
    (tmp_path / "Exporter" / "tools.py").write_text("VALUE = 2\n")

    (tmp_path / "Private").mkdir()
    (tmp_path / "Private" / "command.py").write_text("def run(): pass\n")
    (tmp_path / "Private" / "secret.py").write_text("VALUE = 3\n")
    return tmp_path


@pytest.fixture
def activated(cp, root):
    """activate() for MyCommand, always deactivated again — a leaked sys.path
    entry or meta_path finder would poison every later test in the session."""
    token = cp.activate(str(root / "MyCommand"))
    try:
        yield token
    finally:
        cp.deactivate(token)


def test_own_folder_and_lib_are_importable(activated):
    import shared
    assert shared.VALUE == 1


def test_own_folder_wins_over_lib(activated):
    import helper
    assert helper.WHO == "own"


def test_neither_entry_is_prepended(cp, root, activated):
    # `evp` and the stdlib must stay reachable ahead of anything a command folder
    # contains: a stray json.py in someone's folder would otherwise break the bus.
    assert sys.path[0] != str(root / "MyCommand")
    assert sys.path.index(str(root / "MyCommand")) < sys.path.index(str(root / "_lib"))


def test_exporting_sibling_is_importable(activated):
    from Exporter import tools
    assert tools.VALUE == 2

    import Exporter
    assert Exporter.MARKER == "exported"          # _exports.py IS the package body


def test_sibling_without_the_marker_stays_private(activated):
    with pytest.raises(ImportError):
        __import__("Private")


def test_exporting_folders_lists_only_opted_in(cp, root):
    assert cp.exporting_folders(str(root)) == ["Exporter"]


def test_deactivate_removes_what_it_added(cp, root):
    before_path = list(sys.path)
    before_meta = list(sys.meta_path)
    token = cp.activate(str(root / "MyCommand"))
    import shared                                  # noqa: F401
    cp.deactivate(token)
    assert sys.path == before_path
    assert sys.meta_path == before_meta
    assert "shared" not in sys.modules


def test_deactivate_evicts_a_folder_the_command_added_itself(cp, root):
    """THE SUNSTUDY BUG. A command may put its own folder on sys.path (the
    documented `sys.path.insert(0, dirname(__file__))`), so `activate` adds
    nothing — and a cleanup that only undoes its OWN additions then evicts
    nothing at all, for the whole session."""
    own = str(root / "MyCommand")
    sys.path.insert(0, own)
    token = None
    try:
        token = cp.activate(own)
        assert own not in token["added"]          # nothing of ours to undo...
        import helper                             # noqa: F401
        cp.deactivate(token)
        token = None
        assert "helper" not in sys.modules        # ...but it is still evicted
    finally:
        cp.deactivate(token)
        sys.path.remove(own)


def test_an_edited_helper_takes_effect_on_the_next_run(cp, root):
    """Two runs with an edit between them — what a person actually does."""
    token = cp.activate(str(root / "MyCommand"))
    import shared
    assert shared.VALUE == 1
    cp.deactivate(token)

    (root / "_lib" / "shared.py").write_text("VALUE = 99\n")

    token = cp.activate(str(root / "MyCommand"))
    try:
        import shared as shared_again
        assert shared_again.VALUE == 99
    finally:
        cp.deactivate(token)


def test_an_exporting_siblings_module_is_evicted_too(cp, root):
    token = cp.activate(str(root / "MyCommand"))
    from Exporter import tools                    # noqa: F401
    cp.deactivate(token)
    assert "Exporter" not in sys.modules
    assert "Exporter.tools" not in sys.modules


def test_deactivate_survives_a_junk_token(cp):
    cp.deactivate(None)
    cp.deactivate({})
