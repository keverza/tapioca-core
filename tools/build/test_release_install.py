"""Offline release/installer regression tests; never touch the real user runtime."""

import ast
import os
import shutil
import subprocess
import zipfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
POWERSHELL = shutil.which("powershell") if os.name == "nt" else None
pytestmark = pytest.mark.skipif(not POWERSHELL, reason="Windows PowerShell required")


def ps_quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def run_ps(tmp_path, command, *, archicad_running=False):
    local = tmp_path / "user profile" / "Local"
    local.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, LOCALAPPDATA=str(local))
    # Host processes must not affect deterministic tests. All writes go to tmp_path.
    process_result = "[pscustomobject]@{Name='Archicad'}" if archicad_running else "@()"
    script = (
        "$ErrorActionPreference = 'Stop'; "
        "function Get-Process { [CmdletBinding()] param([string[]] $Name) " + process_result + " }; " + command
    )
    return subprocess.run(
        [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )


def write_file(root, relative, content="fixture"):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


@pytest.fixture
def release(tmp_path):
    root = tmp_path / "extracted release" / "Tapioca"
    root.mkdir(parents=True)
    shutil.copy2(REPO / "dist/Install-Tapioca.ps1", root)
    for relative in ("Tapioca.apx", "EvPPy.dll", "PyPackage/tapioca/__init__.py"):
        write_file(root, relative)
    write_file(root, "Commands/HelloCommand/command.py", "# bundled hello")
    write_file(root, "Commands/SelectionReport/command.py", "# bundled selection")
    write_file(
        root,
        "Install-Runtime.ps1",
        "param([switch] $Force)\nSet-Content -LiteralPath "
        "(Join-Path $env:LOCALAPPDATA 'runtime-called') -Value $Force\n",
    )
    return root


def test_installer_preserves_existing_commands_and_can_update(tmp_path, release):
    commands = tmp_path / "user profile/Local/Tapioca/Commands"
    hello = write_file(commands, "HelloCommand/command.py", "# user edit")
    custom = write_file(commands, "MyCommand/command.py", "# custom command")
    result = run_ps(tmp_path, f"& {ps_quote(release / 'Install-Tapioca.ps1')}")
    assert result.returncode == 0, result.stdout + result.stderr
    assert hello.read_text() == "# user edit"
    assert custom.read_text() == "# custom command"
    assert (commands / "SelectionReport/command.py").read_text() == "# bundled selection"
    assert (tmp_path / "user profile/Local/runtime-called").read_text().strip() == "False"

    result = run_ps(tmp_path, f"& {ps_quote(release / 'Install-Tapioca.ps1')} -UpdateExamples -ForceRuntime")
    assert result.returncode == 0, result.stdout + result.stderr
    assert hello.read_text() == "# bundled hello"
    assert custom.read_text() == "# custom command"
    assert (tmp_path / "user profile/Local/runtime-called").read_text().strip() == "True"


def test_installer_skip_runtime(tmp_path, release):
    result = run_ps(tmp_path, f"& {ps_quote(release / 'Install-Tapioca.ps1')} -SkipRuntime")
    assert result.returncode == 0, result.stdout + result.stderr
    assert not (tmp_path / "user profile/Local/runtime-called").exists()


@pytest.mark.parametrize("missing", ["EvPPy.dll", "PyPackage/tapioca/__init__.py", "Commands/HelloCommand/command.py"])
def test_installer_rejects_incomplete_release(tmp_path, release, missing):
    (release / missing).unlink()
    result = run_ps(tmp_path, f"& {ps_quote(release / 'Install-Tapioca.ps1')}")
    assert result.returncode != 0
    assert "Incomplete release" in result.stderr
    assert not (tmp_path / "user profile/Local/runtime-called").exists()


def test_installers_require_archicad_closed(tmp_path, release):
    for script in (release / "Install-Tapioca.ps1", REPO / "dist/Install-Runtime.ps1"):
        result = run_ps(tmp_path, f"& {ps_quote(script)}", archicad_running=True)
        assert result.returncode != 0
        assert "Close every Archicad session" in result.stderr
    assert not (tmp_path / "user profile/Local/Tapioca/runtime").exists()


def test_installer_rejects_contradictory_switches(tmp_path, release):
    result = run_ps(tmp_path, f"& {ps_quote(release / 'Install-Tapioca.ps1')} -SkipRuntime -ForceRuntime")
    assert result.returncode != 0
    assert "cannot be combined" in result.stderr


def test_runtime_rejects_other_python_abi(tmp_path):
    result = run_ps(tmp_path, f"& {ps_quote(REPO / 'dist/Install-Runtime.ps1')} -Version 3.13.1")
    assert result.returncode != 0
    assert "ParameterArgumentValidationError" in result.stderr
    assert not (tmp_path / "user profile/Local/Tapioca/runtime").exists()


def test_runtime_force_does_not_erase_unrelated_directory(tmp_path):
    target = tmp_path / "unrelated"
    keep = write_file(target, "keep.txt", "user data")
    result = run_ps(tmp_path, f"& {ps_quote(REPO / 'dist/Install-Runtime.ps1')} -Target {ps_quote(target)} -Force")
    assert result.returncode != 0
    assert "Refusing to erase" in result.stderr
    assert keep.read_text() == "user data"


def test_runtime_rejects_protected_target(tmp_path):
    result = run_ps(tmp_path, f"& {ps_quote(REPO / 'dist/Install-Runtime.ps1')} -Target $env:LOCALAPPDATA -Force")
    assert result.returncode != 0
    assert "protected directory" in result.stderr


def test_runtime_rejects_incomplete_existing_runtime(tmp_path):
    target = tmp_path / "runtime"
    write_file(target, "python312.dll")
    result = run_ps(tmp_path, f"& {ps_quote(REPO / 'dist/Install-Runtime.ps1')} -Target {ps_quote(target)}")
    assert result.returncode != 0
    assert "Existing runtime is incomplete" in result.stderr


def test_stage_release_is_standalone_and_excludes_private_commands(tmp_path):
    repo = tmp_path / "source repo"
    stage_script = repo / "tools/build/Stage-Release.ps1"
    stage_script.parent.mkdir(parents=True)
    shutil.copy2(REPO / "tools/build/Stage-Release.ps1", stage_script)
    for name in ("README.md", "Install-Tapioca.ps1", "Install-Runtime.ps1"):
        destination = repo / "dist" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / "dist" / name, destination)
    for relative in (
        "LICENSE",
        "NOTICE",
        "Examples/HelloCommand/command.py",
        "AddOn/EvP/build_29/Tapioca.apx",
        "AddOn/EvP/build_29/EvPPy.dll",
        "AddOn/EvP/build_29/PyPackage/tapioca/__init__.py",
        "AddOn/EvP/build_29/PyPackage/evp/_env.py",
        "AddOn/EvP/build_29/PyPackage/test_webui.py",
        "AddOn/EvP/build_29/PyPackage/evp/__pycache__/cached.pyc",
        "AddOn/EvP/build_29/unused.pdb",
        "private/Commands/PrivateCommand/command.py",
    ):
        write_file(repo, relative)
    write_file(repo, "README.md", "Repository-only instructions must not ship")
    result = run_ps(tmp_path, f"& {ps_quote(stage_script)}")
    assert result.returncode == 0, result.stdout + result.stderr
    stage = repo / "dist/Tapioca"
    assert (stage / "README.md").read_bytes() == (REPO / "dist/README.md").read_bytes()
    assert (stage / "Install-Tapioca.ps1").is_file()
    assert (stage / "Commands/HelloCommand/command.py").is_file()
    assert not (stage / "PyPackage/test_webui.py").exists()
    assert not list(stage.rglob("__pycache__"))
    with zipfile.ZipFile(repo / "dist/Tapioca-AC29.zip") as archive:
        names = {name.replace("\\", "/") for name in archive.namelist()}
    assert "Tapioca/Install-Tapioca.ps1" in names
    assert "Tapioca/Commands/HelloCommand/command.py" in names
    assert not any("PrivateCommand" in name or name.endswith(".pdb") for name in names)


def test_runtime_baseline_matches_package():
    tree = ast.parse((REPO / "AddOn/EvP/Sources/PyPackage/evp/_env.py").read_text(encoding="utf-8"))
    baseline = next(
        ast.literal_eval(node.value)
        for node in tree.body
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "BASELINE" for t in node.targets)
    )
    for relative in ("dist/Install-Runtime.ps1", "AddOn/EvP/Install-Runtime.ps1"):
        script = (REPO / relative).read_text(encoding="utf-8")
        assert "@(" + ", ".join(f"'{spec}'" for spec in baseline) + ")" in script
