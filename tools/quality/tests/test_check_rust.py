"""The Rust foundation gates must reject broken policy, not merely pass valid crates."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tomllib
import types
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import check_rust


@pytest.fixture
def workspace(tmp_path):
    destination = tmp_path / "rust"
    shutil.copytree(check_rust.WORKSPACE, destination, ignore=shutil.ignore_patterns("target", "build"))
    return destination


def test_workspace_structure_and_aggregate_staticlib_identity(workspace):
    errors, _ = check_rust.structure(workspace)
    assert errors == []  # Cohesive owned modules may legitimately emit advisories.
    ffi = tomllib.loads((workspace / "ffi/Cargo.toml").read_text(encoding="utf-8"))
    assert ffi["package"]["name"] == "tapioca-core-ffi"
    assert ffi["lib"]["crate-type"] == ["staticlib", "rlib"]
    assert ffi["dependencies"] == {"tapioca-geometry": {"path": "../geometry"}}
    geometry = tomllib.loads((workspace / "geometry/Cargo.toml").read_text(encoding="utf-8"))
    assert geometry["package"]["name"] == "tapioca-geometry"
    assert geometry.get("dependencies", {}) == {}


@pytest.mark.parametrize("crate", ["geometry", "ffi"])
def test_unsafe_crate_guards_cannot_disappear(workspace, crate):
    (workspace / crate / "src/lib.rs").write_text("//! Scaffold.\n", encoding="utf-8")
    errors, _ = check_rust.structure(workspace)
    assert any("unsafe_code" in error for error in errors)


@pytest.mark.parametrize(
    "addition",
    [
        '\n[dependencies]\nwindows = "0.1"\n',
        '\n[dev-dependencies]\nproptest = "1"\n',
        '\n[build-dependencies]\ncc = "1"\n',
        "\n[target.'cfg(windows)'.dependencies]\nwindows = \"0.1\"\n",
    ],
)
def test_unapproved_dependencies_fail_including_platform_and_test_crates(workspace, addition):
    path = workspace / "geometry/Cargo.toml"
    path.write_text(path.read_text(encoding="utf-8") + addition, encoding="utf-8")
    errors, _ = check_rust.structure(workspace)
    assert errors


def test_reverse_dependency_is_rejected(workspace):
    path = workspace / "geometry/Cargo.toml"
    path.write_text(
        path.read_text(encoding="utf-8") + '\n[dependencies]\ntapioca-core-ffi = { path = "../ffi" }\n',
        encoding="utf-8",
    )
    errors, _ = check_rust.structure(workspace)
    assert any("ffi -> geometry" in error for error in errors)


def test_build_script_is_rejected(workspace):
    (workspace / "geometry/build.rs").write_text("fn main() {}\n", encoding="utf-8")
    errors, _ = check_rust.structure(workspace)
    assert any("build.rs" in error for error in errors)


def test_abort_profile_is_rejected(workspace):
    path = workspace / "Cargo.toml"
    path.write_text(path.read_text(encoding="utf-8").replace('panic = "unwind"', 'panic = "abort"'), encoding="utf-8")
    errors, _ = check_rust.structure(workspace)
    assert any("panic must be unwind" in error for error in errors)


@pytest.mark.parametrize("count, prefix", [(600, None), (601, "WARNING"), (1001, "REVIEW")])
def test_file_size_is_an_advisory_not_a_build_failure(workspace, count, prefix):
    _, baseline = check_rust.structure(workspace)
    (workspace / "geometry/src/large.rs").write_text("// cohesive algorithm\n" * count, encoding="utf-8")
    errors, reviews = check_rust.structure(workspace)
    assert errors == []
    added = [review for review in reviews if review not in baseline]
    assert len(added) == (1 if prefix else 0)
    if prefix:
        assert added[0].startswith(prefix)
        assert "geometry/src/large.rs" in added[0]


@pytest.mark.parametrize("directory", ["target", "build", "vendor", "generated"])
def test_generated_and_vendored_rust_are_not_size_scanned(workspace, directory):
    baseline = check_rust.structure(workspace)
    path = workspace / directory
    path.mkdir()
    (path / "large.rs").write_text("// not owned\n" * 1001, encoding="utf-8")
    assert check_rust.structure(workspace) == baseline


def test_new_untracked_rust_is_secret_scanned(workspace):
    (workspace / "geometry/src/new.rs").write_text('const URL: &str = "api.openai.com";\n', encoding="utf-8")
    errors, _ = check_rust.structure(workspace)
    assert any("provider configuration" in error for error in errors)


def test_full_gate_commands_are_offline_locked_and_test_both_profiles():
    commands = check_rust.cargo_commands("x86_64-pc-windows-msvc")
    assert [command[0] for command in commands] == ["fmt", "check", "clippy", "clippy", "test", "test", "build"]
    for command in commands:
        if command[0] != "fmt":
            assert "--locked" in command and "--offline" in command
            assert "x86_64-pc-windows-msvc" in command
    tests = [command for command in commands if command[0] == "test"]
    assert len(tests) == 2
    assert "--release" not in tests[0] and "--release" in tests[1]
    assert commands[-1][:4] == ["build", "--package", "tapioca-core-ffi", "--release"]


@pytest.mark.parametrize(
    "target, platform",
    [(None, []), ("x86_64-pc-windows-msvc", ["--target", "x86_64-pc-windows-msvc"])],
)
def test_build_mode_selects_only_the_aggregate_release_command(target, platform):
    assert check_rust.cargo_commands(target, build_only=True) == [
        ["build", "--package", "tapioca-core-ffi", "--release", "--locked", "--offline", *platform]
    ]


def test_build_and_structure_only_are_mutually_exclusive(capsys):
    with pytest.raises(SystemExit) as error:
        check_rust.main(["--build", "--structure-only"])
    assert error.value.code == 2
    assert "not allowed with argument" in capsys.readouterr().err


def _fake_cargo(monkeypatch):
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return types.SimpleNamespace(returncode=0)

    monkeypatch.setattr(check_rust.subprocess, "run", run)
    return calls


def test_missing_rustup_is_an_error_not_a_green_skip(monkeypatch):
    monkeypatch.setattr(check_rust, "find_rustup", lambda: None)
    assert check_rust.main([]) == 2
    assert check_rust.main(["--structure-only"]) == 0


@pytest.mark.parametrize("override", ["RUSTUP_TOOLCHAIN", "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "RUSTC_BOOTSTRAP"])
def test_inherited_overrides_cannot_bypass_the_pin_or_flags(monkeypatch, override):
    monkeypatch.setattr(check_rust, "find_rustup", lambda: "rustup")
    monkeypatch.setenv(override, "override")
    assert check_rust.main([]) == 2


@pytest.mark.parametrize(
    "relative, contents",
    [
        ("geometry/src/lib.rs", "//! Missing unsafe guard.\n"),
        ("geometry/src/new.rs", 'const URL: &str = "api.openai.com";\n'),
    ],
)
def test_build_mode_rejects_structure_and_secret_violations_before_cargo(workspace, monkeypatch, relative, contents):
    monkeypatch.setattr(check_rust, "WORKSPACE", workspace)
    monkeypatch.setattr(check_rust, "find_rustup", lambda: "rustup")
    calls = _fake_cargo(monkeypatch)
    (workspace / relative).write_text(contents, encoding="utf-8")
    assert check_rust.main(["--build"]) == 1
    assert calls == []


def test_build_mode_still_requires_rustup(workspace, monkeypatch):
    monkeypatch.setattr(check_rust, "WORKSPACE", workspace)
    calls = _fake_cargo(monkeypatch)
    monkeypatch.setattr(check_rust, "find_rustup", lambda: None)
    assert check_rust.main(["--build"]) == 2
    assert calls == []


@pytest.mark.parametrize("override", ["RUSTUP_TOOLCHAIN", "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "RUSTC_BOOTSTRAP"])
def test_build_mode_refuses_inherited_overrides(workspace, monkeypatch, override):
    monkeypatch.setattr(check_rust, "WORKSPACE", workspace)
    calls = _fake_cargo(monkeypatch)
    monkeypatch.setattr(check_rust, "find_rustup", lambda: "rustup")
    monkeypatch.setenv(override, "override")
    assert check_rust.main(["--build"]) == 2
    assert calls == []


def test_build_mode_uses_pinned_toolchain_for_one_command(workspace, monkeypatch):
    monkeypatch.setattr(check_rust, "WORKSPACE", workspace)
    monkeypatch.setattr(check_rust, "find_rustup", lambda: "rustup")
    calls = _fake_cargo(monkeypatch)
    assert check_rust.main(["--build", "--target", "x86_64-pc-windows-msvc"]) == 0
    version = tomllib.loads((workspace / "rust-toolchain.toml").read_text(encoding="utf-8"))["toolchain"]["channel"]
    assert calls == [
        [
            "rustup",
            "run",
            version,
            "cargo",
            "build",
            "--package",
            "tapioca-core-ffi",
            "--release",
            "--locked",
            "--offline",
            "--target",
            "x86_64-pc-windows-msvc",
        ]
    ]


@pytest.mark.parametrize(
    "body, diagnostic",
    [
        ("pub fn bad() { unsafe {} }", "unsafe"),
        ("pub fn bad(value: Option<u8>) -> u8 { value.unwrap() }", "unwrap_used"),
        ('pub fn bad(value: Option<u8>) -> u8 { value.expect("required") }', "expect_used"),
        ("pub fn bad(value: &[u8]) -> u8 { value[0] }", "indexing_slicing"),
        ("pub fn bad(a: f64, b: f64) -> bool { a == b }", "float_cmp"),
        ("pub fn bad(a: u8, b: u8, c: u8, d: u8, e: u8, f: u8) -> u8 { a ^ b ^ c ^ d ^ e ^ f }", "too_many_arguments"),
    ],
)
def test_real_compiler_lints_reject_seeded_violations(workspace, body, diagnostic):
    rustup = check_rust.find_rustup()
    if not rustup:
        pytest.skip("Rust toolchain not installed; CI runs the compiler-enforcement tests")
    version = tomllib.loads((workspace / "rust-toolchain.toml").read_text(encoding="utf-8"))["toolchain"]["channel"]
    source = workspace / "geometry/src/lib.rs"
    source.write_text(source.read_text(encoding="utf-8") + "\n/// Negative gate fixture.\n" + body, encoding="utf-8")
    result = subprocess.run(
        [
            rustup,
            "run",
            version,
            "cargo",
            "clippy",
            "--workspace",
            "--all-targets",
            "--locked",
            "--offline",
            "--",
            "-D",
            "warnings",
        ],
        cwd=workspace,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert diagnostic in result.stderr


def test_long_cohesive_function_is_advisory_not_a_strict_failure(workspace):
    rustup = check_rust.find_rustup()
    if not rustup:
        pytest.skip("Rust toolchain not installed; CI runs the compiler-enforcement tests")
    version = tomllib.loads((workspace / "rust-toolchain.toml").read_text(encoding="utf-8"))["toolchain"]["channel"]
    source = workspace / "geometry/src/lib.rs"
    source.write_text(
        source.read_text(encoding="utf-8")
        + "\n/// Cohesive review fixture.\n#[must_use]\npub fn long(mut value: u32) -> u32 {\n"
        + "    value = value.wrapping_add(1);\n" * 110
        + "    value\n}\n",
        encoding="utf-8",
    )
    base = [rustup, "run", version, "cargo", "clippy", "--workspace", "--all-targets", "--locked", "--offline"]
    strict = subprocess.run([*base, "--", "-D", "warnings"], cwd=workspace, capture_output=True, text=True, check=False)
    assert strict.returncode == 0, strict.stderr
    advisory = subprocess.run(
        [*base, "--", "-A", "warnings", "--force-warn", "clippy::too_many_lines"],
        cwd=workspace,
        capture_output=True,
        text=True,
        check=False,
    )
    assert advisory.returncode == 0
    assert "too_many_lines" in advisory.stderr
