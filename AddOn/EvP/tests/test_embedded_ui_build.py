"""Exercise the isolated CMake UI workspace without npm/network or the DevKit."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

_CMAKE_DIR = Path(__file__).parents[1] / "cmake"


@pytest.fixture
def cmake():
    executable = os.environ.get("CMAKE_EXECUTABLE") or shutil.which("cmake")
    if not executable:
        pytest.skip("CMake is not available (set CMAKE_EXECUTABLE)")
    return executable


def _run(cmake, *args, check=True):
    return subprocess.run([cmake, *map(str, args)], capture_output=True, text=True, check=check)


def _stage(cmake, source, build, manifest, **kwargs):
    return _run(
        cmake,
        f"-DUI_SOURCE_DIR={source}",
        f"-DUI_BUILD_DIR={build}",
        f"-DUI_MANIFEST={manifest}",
        "-P",
        _CMAKE_DIR / "StageUISources.cmake",
        **kwargs,
    )


def test_staging_removes_only_obsolete_sources_and_preserves_dependencies(cmake, tmp_path):
    source = tmp_path / "Source UI"
    build = tmp_path / "Build UI"
    for root in (source, build):
        (root / "src").mkdir(parents=True)
    (source / "src" / "old.ts").write_text("old", encoding="utf-8")
    manifest = build / "sources.txt"
    manifest.write_text("src/old.ts\n", encoding="utf-8")
    _stage(cmake, source, build, manifest)
    original_time = (build / "src" / "old.ts").stat().st_mtime_ns
    _stage(cmake, source, build, manifest)
    assert (build / "src" / "old.ts").stat().st_mtime_ns == original_time

    for name in ("node_modules/native.node", "dist/index.html", "package-lock.json"):
        file = build / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text("keep", encoding="utf-8")
    (source / "src" / "new.ts").write_text("new", encoding="utf-8")
    manifest.write_text("src/new.ts\n", encoding="utf-8")
    _stage(cmake, source, build, manifest)
    assert not (build / "src" / "old.ts").exists()
    assert (build / "src" / "new.ts").read_text(encoding="utf-8") == "new"
    for name in ("node_modules/native.node", "dist/index.html", "package-lock.json"):
        assert (build / name).read_text(encoding="utf-8") == "keep"


@pytest.mark.parametrize(
    "entry", ["../outside.txt", "src/../../outside.txt", "node_modules/native.node", "dist/index.html"]
)
def test_staging_refuses_unsafe_paths_before_removing_any_files(cmake, tmp_path, entry):
    manifest = tmp_path / "sources.txt"
    manifest.write_text(entry + "\n", encoding="utf-8")
    (tmp_path / ".tapioca-staged-sources.txt").write_text("src/old.ts\n", encoding="utf-8")
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "old.ts").write_text("keep", encoding="utf-8")
    result = _stage(cmake, tmp_path, tmp_path, manifest, check=False)
    assert result.returncode != 0
    assert "Unsafe UI source path" in result.stderr
    assert (tmp_path / "src" / "old.ts").exists()


def test_build_installs_off_tree_and_source_changes_do_not_reinstall(cmake, tmp_path):
    # This fixture has no compiler or DevKit; use the same small cross-platform
    # generator in CI instead of inheriting an installed Visual Studio version.
    ninja = shutil.which("ninja")
    if not ninja and os.name == "nt":
        bundled = Path(cmake).parents[2] / "Ninja" / "ninja.exe"
        if bundled.is_file():
            ninja = str(bundled)
    if not ninja:
        pytest.skip("Ninja is required for the isolated build integration fixture")
    source = tmp_path / "Project UI"
    source.mkdir()
    (source / "src").mkdir()
    (source / "node_modules").mkdir()
    locked = source / "node_modules" / "locked.node"
    locked.write_text("untouched", encoding="utf-8")
    (source / "package.json").write_text('{"name":"fixture"}', encoding="utf-8")
    lockfile = source / "package-lock.json"
    lockfile.write_text('{"lockfileVersion":3}', encoding="utf-8")
    (source / "src" / "first.ts").write_text("first", encoding="utf-8")

    fake_npm = tmp_path / "fake_npm.py"
    fake_npm.write_text(
        "import pathlib, sys\n"
        "root = pathlib.Path.cwd()\n"
        "with (root / 'npm-calls.txt').open('a') as log:\n"
        "    log.write(' '.join(sys.argv[1:]) + '\\n')\n"
        "if sys.argv[1:] == ['ci']:\n"
        "    (root / 'node_modules').mkdir(exist_ok=True)\n"
        "if sys.argv[1:] == ['run', 'build']:\n"
        "    (root / 'dist').mkdir(exist_ok=True)\n"
        "    (root / 'dist' / 'index.html').write_text('bundle')\n",
        encoding="utf-8",
    )
    npm = tmp_path / ("fake-npm.cmd" if os.name == "nt" else "fake-npm.sh")
    npm.write_text(
        f'@echo off\n"{sys.executable}" "{fake_npm}" %*\n'
        if os.name == "nt"
        else f'#!/bin/sh\nexec "{sys.executable}" "{fake_npm}" "$@"\n',
        encoding="utf-8",
    )
    npm.chmod(0o755)
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(UIFixture NONE)\n"
        f'set(NPM_EXECUTABLE "{npm.as_posix()}")\n'
        f'set(TAPIOCA_EMBEDDED_UI_CMAKE_DIR "{_CMAKE_DIR.as_posix()}")\n'
        f'include("{(_CMAKE_DIR / "EmbeddedUI.cmake").as_posix()}")\n'
        'file(GLOB_RECURSE uiFiles CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*")\n'
        'TapiocaAddEmbeddedUI(GraphUI "${CMAKE_CURRENT_SOURCE_DIR}" ${uiFiles})\n',
        encoding="utf-8",
    )
    build = tmp_path / "Build Tree"
    _run(cmake, "-S", source, "-B", build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}")

    def rebuild():
        _run(cmake, "--build", build, "--config", "Release", "--target", "EvPGraphUI")

    rebuild()
    workspace = build / "EmbeddedUI" / "GraphUI"
    calls = workspace / "npm-calls.txt"
    assert calls.read_text().splitlines() == ["ci", "run typecheck", "run build"]
    assert locked.read_text() == "untouched"
    rebuild()
    assert calls.read_text().splitlines().count("ci") == 1
    assert calls.read_text().splitlines().count("run build") == 1

    (source / "src" / "first.ts").write_text("edited", encoding="utf-8")
    rebuild()
    assert (workspace / "src" / "first.ts").read_text() == "edited"
    assert calls.read_text().splitlines().count("ci") == 1
    assert calls.read_text().splitlines().count("run build") == 2

    # A renamed input must remove the stale copy, rebuild, but not reinstall.
    (source / "src" / "first.ts").rename(source / "src" / "second.ts")
    rebuild()
    assert not (workspace / "src" / "first.ts").exists()
    assert (workspace / "src" / "second.ts").exists()
    assert calls.read_text().splitlines().count("ci") == 1
    assert calls.read_text().splitlines().count("run build") == 3

    with (source / "CMakeLists.txt").open("a", encoding="utf-8") as cmake_lists:
        cmake_lists.write("# unrelated native reconfiguration\n")
    rebuild()
    assert calls.read_text().splitlines().count("ci") == 1
    assert calls.read_text().splitlines().count("run build") == 3

    lockfile.write_text('{"lockfileVersion":3,"changed":true}', encoding="utf-8")
    rebuild()
    assert calls.read_text().splitlines().count("ci") == 2
    assert locked.read_text() == "untouched"

    # The stamp belongs to the installation, not the CMake tree: a missing
    # installation cannot be mistaken for a satisfied dependency.
    (workspace / "node_modules" / ".tapioca-install.stamp").unlink()
    rebuild()
    assert calls.read_text().splitlines().count("ci") == 3
