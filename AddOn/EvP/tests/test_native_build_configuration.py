"""Keep C and C++ dependencies on the host's dynamic CRT (#143)."""

import re
from pathlib import Path


def test_addon_enables_both_languages_with_the_host_runtime_before_dependencies():
    source = (Path(__file__).parents[1] / "CMakeLists.txt").read_text(encoding="utf-8")
    # C enabled only by a vendored project left parent C targets without the
    # runtime selection flags: QuickJS then defaulted to /MT and caused LNK4098.
    project = re.search(r"^project\s*\(\$\{ADDON_NAME\}\s+C\s+CXX\)", source, re.MULTILINE)
    assert project is not None
    runtime = "set (CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)"
    assert source.index(runtime) < project.start() < source.index("include (")
    assert "/NODEFAULTLIB" not in source.upper()
    assert "/IGNORE:4098" not in source.upper()
