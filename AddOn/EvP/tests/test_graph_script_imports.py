"""The folder model's import rules, offline.

⚠️ THIS IS THE SUITE THAT DECIDES WHETHER "SAVE THE HELPER, SEE THE CHANGE"
WORKS. A script node is a folder now, so `main.py` importing `calculations` is
the ordinary case rather than an advanced one - and Python caches an imported
module forever. The tests below are mostly about what must NOT survive a run: a
stale helper, a path entry, another node's module.

Runs the real `evp._graphscript.run` against temporary folders. No Archicad, no
CPython embedding, no add-on.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources" / "PyPackage"))

from evp import _graphscript  # noqa: E402


def write(folder: Path, name: str, text: str) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    (folder / name).write_text(text, encoding="utf-8")


def run(folder: Path, source: str, outputs: list[str], roots: list[str] | None = None):
    return _graphscript.run(
        source,
        str(folder / "main.py"),
        {},
        [{"portId": name} for name in outputs],
        roots if roots is not None else [str(folder)],
        2000.0,
    )


def test_a_helper_beside_main_is_importable(tmp_path: Path) -> None:
    write(tmp_path, "calculations.py", "def double(value):\n    return value * 2\n")
    result = run(tmp_path, "from calculations import double\nvalue = double(21)\n", ["value"])
    assert result["ok"], result["error"]
    assert result["outputs"]["value"] == 42


def test_an_edited_helper_is_picked_up_on_the_next_run(tmp_path: Path) -> None:
    # The whole reason _invalidate exists. Without it the second run returns the
    # FIRST answer for the rest of the session, while the editor shows the new
    # source - the most confusing failure this feature could have.
    #
    # The two versions here are deliberately the SAME LENGTH and written within
    # the same second, which is the case that also defeats Python's bytecode
    # cache: a .pyc records its source's mtime truncated to whole seconds plus
    # its size, so both still match and the stale bytecode is served. Dropping
    # the module is not enough on its own - see _discard_bytecode.
    write(tmp_path, "calculations.py", "def answer():\n    return 1\n")
    source = "from calculations import answer\nvalue = answer()\n"
    assert run(tmp_path, source, ["value"])["outputs"]["value"] == 1

    write(tmp_path, "calculations.py", "def answer():\n    return 2\n")
    assert run(tmp_path, source, ["value"])["outputs"]["value"] == 2


def test_two_nodes_may_each_have_a_helper_of_the_same_name(tmp_path: Path) -> None:
    # Node folders are independent namespaces. If the cache were keyed by module
    # name alone, the second node would silently run the first node's helper -
    # and `helpers.py` is exactly the name two people pick independently.
    first, second = tmp_path / "first", tmp_path / "second"
    write(first, "helpers.py", "NAME = 'first'\n")
    write(second, "helpers.py", "NAME = 'second'\n")
    source = "import helpers\nvalue = helpers.NAME\n"
    assert run(first, source, ["value"])["outputs"]["value"] == "first"
    assert run(second, source, ["value"])["outputs"]["value"] == "second"


def test_a_shared_library_root_resolves_as_a_package_and_directly(tmp_path: Path) -> None:
    # Both spellings must work: the plan prefers `from libs.geometry import ...`,
    # and a script that reads correctly must not fail at run time because only
    # the other form was wired up.
    library = tmp_path / "libs"
    write(library, "__init__.py", "")
    write(library, "geometry.py", "def area():\n    return 7\n")
    node = tmp_path / "node"
    node.mkdir()
    roots = [str(node), str(library), str(tmp_path)]

    packaged = run(node, "from libs.geometry import area\nvalue = area()\n", ["value"], roots)
    assert packaged["ok"], packaged["error"]
    assert packaged["outputs"]["value"] == 7

    direct = run(node, "from geometry import area\nvalue = area()\n", ["value"], roots)
    assert direct["ok"], direct["error"]
    assert direct["outputs"]["value"] == 7


def test_the_node_folder_wins_over_a_shared_helper_of_the_same_name(tmp_path: Path) -> None:
    # Deliberate shadowing: a node that needs its own version of a shared helper
    # puts one beside main.py. The order of the roots is what guarantees this,
    # and it is the reason the node's folder is first.
    library = tmp_path / "libs"
    node = tmp_path / "node"
    write(library, "geometry.py", "NAME = 'shared'\n")
    write(node, "geometry.py", "NAME = 'mine'\n")
    result = run(node, "import geometry\nvalue = geometry.NAME\n", ["value"], [str(node), str(library)])
    assert result["outputs"]["value"] == "mine"


def test_sys_path_is_left_exactly_as_it_was_found(tmp_path: Path) -> None:
    # A root left behind is a module the NEXT node can import by accident, and
    # that failure shows up as a graph that works until it is opened in a
    # different order.
    write(tmp_path, "calculations.py", "value = 1\n")
    before = list(sys.path)
    run(tmp_path, "import calculations\nvalue = 1\n", ["value"])
    assert sys.path == before


def test_a_script_that_edits_sys_path_itself_does_not_leak_it(tmp_path: Path) -> None:
    before = list(sys.path)
    run(tmp_path, "import sys\nsys.path.append('C:\\\\nowhere')\nvalue = 1\n", ["value"])
    assert sys.path == before


def test_installed_dependencies_are_importable(tmp_path: Path) -> None:
    # A script node runs inside Tapioca's own interpreter, so whatever is
    # installed in its site-packages is available with no per-node environment.
    # json stands in for that here: the assertion is about the mechanism, and the
    # offline suite must not require numpy to be installed to make the point.
    result = run(tmp_path, "import json\nvalue = json.dumps({'a': 1})\n", ["value"])
    assert result["ok"], result["error"]
    assert result["outputs"]["value"] == '{"a": 1}'


def test_a_third_party_module_is_not_invalidated_between_runs(tmp_path: Path) -> None:
    # _invalidate must drop the node's OWN modules and nothing else. Reloading
    # site-packages per node evaluation would be slow, and for anything holding
    # state it would be wrong.
    import json as first

    run(tmp_path, "value = 1\n", ["value"])
    import json as second

    assert first is second


def test_a_missing_helper_fails_the_node_rather_than_the_process(tmp_path: Path) -> None:
    result = run(tmp_path, "import nowhere_at_all\nvalue = 1\n", ["value"])
    assert not result["ok"]
    assert "nowhere_at_all" in result["error"]


def test_no_import_roots_still_runs(tmp_path: Path) -> None:
    # A node whose workspace would not resolve still evaluates; it simply cannot
    # import its neighbours. Refusing to run would turn a path problem into a
    # dead node.
    result = _graphscript.run("value = 3\n", "main.py", {}, [{"portId": "value"}], None, 2000.0)
    assert result["ok"], result["error"]
    assert result["outputs"]["value"] == 3


def test_the_time_budget_still_stops_a_runaway_helper(tmp_path: Path) -> None:
    # The containment property, restated for the folder model: the budget is
    # enforced by a trace function inside the interpreter, so it applies to code
    # in an imported helper exactly as it does to main.py.
    write(tmp_path, "slow.py", "def spin():\n    while True:\n        pass\n")
    result = _graphscript.run(
        "from slow import spin\nspin()\nvalue = 1\n",
        str(tmp_path / "main.py"),
        {},
        [{"portId": "value"}],
        [str(tmp_path)],
        200.0,
    )
    assert not result["ok"]
    assert "budget" in result["error"]


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__]))
