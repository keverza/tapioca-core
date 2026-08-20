"""The C++ style gate must block misformatted staged files and nothing else.

Three properties are load-bearing and each has already been specified as a rule
somewhere in the repository, so each is pinned here:

  * a misformatted file fails, or the gate is decorative;
  * an unconverted file that the commit did not touch is never even looked at,
    because a whole-tree gate on a partly converted tree fails every commit and
    is then bypassed forever;
  * a missing clang-format warns and passes, for the same reason.

The style itself is asserted only where the codebase measurably disagreed with
the DevKit samples -- spaces, and the space after a C-style cast. Those two are
the settings a future editor is most likely to "restore" by accident.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import check_cpp

WELL_FORMATTED = """\
#include "APIEnvir.h"

namespace geomsrv {

void PlaceAt (int topEdge)
{
    const int usedHeight = (int) ComputeHeight (topEdge);
    if (usedHeight > 0) {
        Report (usedHeight);
    }
}

} // namespace geomsrv
"""

# Tabs, no space before the call paren, no space after the cast, attached brace:
# every one of these is a deliberate violation of a measured setting.
MISFORMATTED = """\
#include "APIEnvir.h"

namespace geomsrv {

void PlaceAt(int topEdge) {
\tconst int usedHeight = (int)ComputeHeight(topEdge);
\tif(usedHeight>0){ Report(usedHeight); }
}

}   // namespace geomsrv
"""


@pytest.fixture
def clang_format() -> str:
    executable = check_cpp._clang_format_executable()
    if executable is None:
        pytest.skip("clang-format is not installed; install dev-requirements.txt")
    return executable


def _format_with_repo_style(clang_format: str, source: str, tmp_path: Path) -> str:
    """Format a snippet with the repository's real .clang-format."""
    path = tmp_path / "Sample.cpp"
    path.write_text(source, encoding="utf-8")
    style = check_cpp.REPO_ROOT / ".clang-format"
    result = subprocess.run(
        [clang_format, f"--style=file:{style}", str(path)],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def test_repo_style_indents_with_spaces(clang_format: str, tmp_path: Path) -> None:
    formatted = _format_with_repo_style(clang_format, MISFORMATTED, tmp_path)
    assert "\t" not in formatted
    assert "\n    const int usedHeight" in formatted


def test_repo_style_keeps_the_space_after_a_c_style_cast(clang_format: str, tmp_path: Path) -> None:
    formatted = _format_with_repo_style(clang_format, MISFORMATTED, tmp_path)
    assert "(int) ComputeHeight" in formatted


def test_repo_style_is_idempotent(clang_format: str, tmp_path: Path) -> None:
    once = _format_with_repo_style(clang_format, MISFORMATTED, tmp_path)
    twice = _format_with_repo_style(clang_format, once, tmp_path)
    assert once == twice


def _run(monkeypatch: pytest.MonkeyPatch, tmp_path: Path, source: str, name: str) -> int:
    """Point the checker at a throwaway tree holding one file."""
    (tmp_path / ".clang-format").write_text(
        (check_cpp.REPO_ROOT / ".clang-format").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    (tmp_path / name).write_text(source, encoding="utf-8")
    monkeypatch.setattr(check_cpp, "REPO_ROOT", tmp_path)
    return check_cpp._check_format([name], fix=False)


def test_a_misformatted_staged_file_fails(clang_format: str, monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    assert _run(monkeypatch, tmp_path, MISFORMATTED, "Bad.cpp") != 0


def test_a_conforming_staged_file_passes(clang_format: str, monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    assert _run(monkeypatch, tmp_path, WELL_FORMATTED, "Good.cpp") == 0


def test_fix_rewrites_the_file_in_place(clang_format: str, monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    (tmp_path / ".clang-format").write_text(
        (check_cpp.REPO_ROOT / ".clang-format").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    path = tmp_path / "Bad.cpp"
    path.write_text(MISFORMATTED, encoding="utf-8")
    monkeypatch.setattr(check_cpp, "REPO_ROOT", tmp_path)

    assert check_cpp._check_format(["Bad.cpp"], fix=True) == 0
    assert "\t" not in path.read_text(encoding="utf-8")
    assert check_cpp._check_format(["Bad.cpp"], fix=False) == 0


def test_untouched_files_are_not_checked(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    """The gate is file-oriented; an unstaged legacy file must not block a commit."""
    (tmp_path / ".clang-format").write_text(
        (check_cpp.REPO_ROOT / ".clang-format").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    (tmp_path / "Legacy.cpp").write_text(MISFORMATTED, encoding="utf-8")
    (tmp_path / "Touched.cpp").write_text(WELL_FORMATTED, encoding="utf-8")
    monkeypatch.setattr(check_cpp, "REPO_ROOT", tmp_path)

    assert check_cpp._check_format(["Touched.cpp"], fix=False) == 0


def test_vendored_and_build_paths_are_excluded() -> None:
    selected = check_cpp._cpp_files(
        [
            "AddOn/reference/DiligentEngine-master/Sample.cpp",
            "AddOn/EvP/build_29/Generated.cpp",
            "archive/probes/2026/Old.cpp",
        ]
    )
    assert selected == []


def test_non_cpp_paths_are_ignored() -> None:
    assert check_cpp._cpp_files(["tools/quality/check_cpp.py", "README.md"]) == []


def test_a_missing_clang_format_warns_and_passes(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    (tmp_path / "Bad.cpp").write_text(MISFORMATTED, encoding="utf-8")
    monkeypatch.setattr(check_cpp, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(check_cpp, "_clang_format_executable", lambda: None)

    assert check_cpp._check_format(["Bad.cpp"], fix=False) == 0


def test_the_tree_report_never_fails(
    clang_format: str,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """Informational by contract: it counts progress, it does not gate.

    Run against a two-file stand-in tree, not the real 275 -- the real walk
    spawns one clang-format per file and belongs in a cleanup session, not in
    every `pytest -q`.
    """
    (tmp_path / ".clang-format").write_text(
        (check_cpp.REPO_ROOT / ".clang-format").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    sources = tmp_path / "AddOn" / "EvP" / "Sources" / "AddOn"
    sources.mkdir(parents=True)
    (sources / "Good.cpp").write_text(WELL_FORMATTED, encoding="utf-8")
    (sources / "Legacy.cpp").write_text(MISFORMATTED, encoding="utf-8")
    monkeypatch.setattr(check_cpp, "REPO_ROOT", tmp_path)

    assert check_cpp._report_format(fix=False) == 0
    output = capsys.readouterr().out
    assert "1 of 2 first-party C/C++ files are in style" in output
    assert "Legacy.cpp" in output


def test_the_style_file_is_the_only_one_in_first_party_code() -> None:
    """A second copy is the drift this repository keeps paying for elsewhere."""
    sources = check_cpp.REPO_ROOT / "AddOn" / "EvP" / "Sources"
    assert list(sources.rglob(".clang-format")) == []
    assert not (check_cpp.REPO_ROOT / "AddOn" / "clang-format").exists()
    assert (check_cpp.REPO_ROOT / ".clang-format").is_file()
