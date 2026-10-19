"""Check the Rust kernel foundation; no add-on build or deployment.

Default: architecture/secrets, fmt, check, strict Clippy, advisory complexity,
debug + release tests/doctests, and a release aggregate staticlib. --build only
compiles that release staticlib; --structure-only needs only Python 3.11+.
Exit 0 = clean, 1 = violation, 2 = missing/broken tool.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

import check_secrets

REPO = Path(__file__).resolve().parents[2]
WORKSPACE = REPO / "AddOn/EvP/rust"
SKIP_DIRS = {"target", "build", "vendor", "generated", ".git"}
PACKAGES = {"geometry": "tapioca-geometry", "ffi": "tapioca-core-ffi"}


def rust_sources(workspace: Path) -> list[Path]:
    """Walk owned code including untracked sources, excluding linked directories."""
    result = []
    for directory, dirs, files in os.walk(workspace, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not (Path(directory) / d).is_symlink())
        result.extend(Path(directory) / name for name in sorted(files) if name.endswith(".rs"))
    return result


def structure(workspace: Path) -> tuple[list[str], list[str]]:
    """Hard dependency/ownership rules; size is deliberately advisory."""
    errors, reviews = [], []
    manifest = tomllib.loads((workspace / "Cargo.toml").read_text(encoding="utf-8"))
    if sorted(manifest["workspace"]["members"]) != sorted(PACKAGES):
        errors.append("workspace members changed: review the crate ownership/dependency policy")
    if manifest["workspace"].get("dependencies"):
        errors.append("shared external dependencies require an explicit dependency-policy review")
    for profile in ("dev", "release"):
        if manifest.get("profile", {}).get(profile, {}).get("panic") != "unwind":
            errors.append(f"profile.{profile}.panic must be unwind for future FFI panic containment")
    for directory, name in PACKAGES.items():
        package = tomllib.loads((workspace / directory / "Cargo.toml").read_text(encoding="utf-8"))
        if package["package"]["name"] != name or package.get("lints", {}).get("workspace") is not True:
            errors.append(f"{directory}: package identity and workspace lint inheritance are required")
        if "target" in package or package["package"].get("build"):
            errors.append(f"{directory}: platform dependencies/build scripts require architecture review")
        expected = {"tapioca-geometry": {"path": "../geometry"}} if directory == "ffi" else {}
        if package.get("dependencies", {}) != expected:
            errors.append(f"{directory}: only the approved ffi -> geometry path dependency is allowed")
        if package.get("build-dependencies") or package.get("dev-dependencies"):
            errors.append(f"{directory}: new crates (including test/build crates) require dependency review")
        source = (workspace / directory / "src/lib.rs").read_text(encoding="utf-8")
        guard = "forbid" if directory == "geometry" else "deny"
        if not re.search(rf"^#!\[{guard}\(unsafe_code\)\]$", source, re.MULTILINE):
            errors.append(f"{directory}: crate root must retain #![{guard}(unsafe_code)]")
    for path in rust_sources(workspace):
        text = path.read_text(encoding="utf-8")
        relative = path.relative_to(workspace).as_posix()
        if path.name == "build.rs":
            errors.append(f"{relative}: build.rs is not part of the approved scaffold")
        # Secrets are checked before staging, unlike the repository-wide tracked scan.
        for _, line, message in check_secrets.scan_file(f"AddOn/EvP/rust/{relative}", text):
            errors.append(f"{relative}:{line}: {message}")
        count = len(text.splitlines())
        if count > 1000:
            reviews.append(f"REVIEW {relative}: {count} lines (>1000); justify cohesion in the change")
        elif count > 600:
            reviews.append(f"WARNING {relative}: {count} lines (>600); target 200-500, do not split mechanically")
    return errors, reviews


def find_rustup() -> str | None:
    found = shutil.which("rustup")
    if found:
        return found
    home = Path(os.environ.get("CARGO_HOME", Path.home() / ".cargo"))
    proxy = home / "bin" / ("rustup.exe" if os.name == "nt" else "rustup")
    return str(proxy) if proxy.is_file() else None


def cargo_commands(target: str | None, build_only: bool = False) -> list[list[str]]:
    platform = ["--target", target] if target else []
    common = ["--workspace", "--all-targets", "--locked", "--offline", *platform]
    tests = ["--workspace", "--locked", "--offline", *platform]
    aggregate = ["build", "--package", PACKAGES["ffi"], "--release", "--locked", "--offline", *platform]
    if build_only:
        return [aggregate]
    return [
        ["fmt", "--all", "--", "--check"],
        ["check", *common],
        ["clippy", *common, "--", "-D", "warnings"],
        [
            "clippy",
            *common,
            "--",
            "-A",
            "warnings",
            "--force-warn",
            "clippy::too_many_lines",
            "--force-warn",
            "clippy::cognitive_complexity",
            "--force-warn",
            "clippy::excessive_nesting",
        ],
        ["test", *tests],
        ["test", *tests, "--release"],
        aggregate,
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--structure-only", action="store_true", help="do not require Rust or compile anything")
    mode.add_argument("--build", action="store_true", help="only build the release aggregate static library")
    parser.add_argument("--target", choices=["x86_64-pc-windows-msvc"], help="otherwise test on the native host")
    parser.add_argument("--target-dir", type=Path, help="Cargo build output directory (default: rust/target)")
    args = parser.parse_args(argv)
    try:
        errors, reviews = structure(WORKSPACE)
        for review in reviews:
            print(review)
        if errors:
            for error in errors:
                print(error, file=sys.stderr)
            return 1
        if args.structure_only:
            print("Rust structure/secrets passed (compilation not requested).")
            return 0
        rustup = find_rustup()
        if not rustup:
            print("Rustup missing: run tools/build/Setup-Rust.ps1 -InstallRustup.", file=sys.stderr)
            return 2
        version = tomllib.loads((WORKSPACE / "rust-toolchain.toml").read_text(encoding="utf-8"))["toolchain"]["channel"]
        env = os.environ.copy()
        # Do not silently bypass the pin, config, or lint command with inherited flags.
        for key in ("RUSTUP_TOOLCHAIN", "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "RUSTC_BOOTSTRAP"):
            if env.get(key):
                print(f"Unset {key} before running the reproducible Rust gate.", file=sys.stderr)
                return 2
        if args.target_dir:
            env["CARGO_TARGET_DIR"] = str(args.target_dir.resolve())
        for command in cargo_commands(args.target, build_only=args.build):
            print("+ cargo " + " ".join(command), flush=True)
            result = subprocess.run([rustup, "run", version, "cargo", *command], cwd=WORKSPACE, env=env, check=False)
            if result.returncode:
                return 1
    except (OSError, ValueError, KeyError) as error:
        print(f"Rust gate could not run: {error}", file=sys.stderr)
        return 2
    if args.build:
        print("Rust core release build passed.")
        return 0
    print("Rust gates passed; the aggregate core static library compiled.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
