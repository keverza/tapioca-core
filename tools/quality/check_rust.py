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

# S3a.0 dependency policy. The only third-party edge is geometry -> spade, pinned exactly.
# THIRD_PARTY is its complete transitive set: each crate's exact version and licence as
# vendored. A crate added, removed, re-versioned or relicensed is a policy change.
DIRECT = {
    "geometry": {"spade": "=2.15.1"},
    "ffi": {"tapioca-geometry": {"path": "../geometry"}},
}
THIRD_PARTY = {
    "spade": ("2.15.1", "MIT OR Apache-2.0"),
    "robust": ("1.2.0", "MIT OR Apache-2.0"),
    "smallvec": ("1.16.3", "MIT OR Apache-2.0"),
    "num-traits": ("0.2.19", "MIT OR Apache-2.0"),
    "autocfg": ("1.5.1", "Apache-2.0 OR MIT"),
    "hashbrown": ("0.16.1", "MIT OR Apache-2.0"),
    "allocator-api2": ("0.2.21", "MIT OR Apache-2.0"),
    "equivalent": ("1.0.3", "Apache-2.0 OR MIT"),
    "foldhash": ("0.2.0", "Zlib"),
}
LICENCES = {"MIT", "Apache-2.0", "Zlib"}  # permissive, GPL-3.0-or-later compatible
# A vendored build script runs on the build machine; each one is reviewed by name.
# num-traits probes the compiler for float/i128 support through autocfg.
BUILD_SCRIPTS = {"num-traits"}
CRATES_IO = "registry+https://github.com/rust-lang/crates.io-index"


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
        if package.get("dependencies", {}) != DIRECT[directory]:
            errors.append(
                f"{directory}: only the approved ffi -> geometry path and geometry -> spade (=2.15.1) "
                "dependencies are allowed"
            )
        if package.get("build-dependencies") or package.get("dev-dependencies"):
            errors.append(f"{directory}: new crates (including test/build crates) require dependency review")
        source = (workspace / directory / "src/lib.rs").read_text(encoding="utf-8")
        guard = "forbid" if directory == "geometry" else "deny"
        if not re.search(rf"^#!\[{guard}\(unsafe_code\)\]$", source, re.MULTILINE):
            errors.append(f"{directory}: crate root must retain #![{guard}(unsafe_code)]")
    errors.extend(third_party(workspace))
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


def third_party(workspace: Path) -> list[str]:
    """The lockfile, source replacement, vendor tree and notice must all name THIRD_PARTY."""
    errors = []
    lock = tomllib.loads((workspace / "Cargo.lock").read_text(encoding="utf-8"))
    locked = {}
    for package in lock.get("package", []):
        source = package.get("source")
        if source is None:
            if package["name"] not in PACKAGES.values():
                errors.append(f"Cargo.lock: unexpected local package {package['name']}")
            continue
        if source != CRATES_IO or not package.get("checksum"):
            errors.append(f"Cargo.lock: {package['name']} must come from crates.io with a checksum")
        locked[package["name"]] = package["version"]
    if locked != {name: version for name, (version, _) in THIRD_PARTY.items()}:
        errors.append(f"Cargo.lock: third-party set {sorted(locked.items())} differs from the reviewed allow-list")
    config = tomllib.loads((workspace / ".cargo/config.toml").read_text(encoding="utf-8")).get("source", {})
    if config.get("crates-io", {}).get("replace-with") != "vendored-sources" or config.get("vendored-sources", {}) != {
        "directory": "vendor"
    }:
        errors.append(".cargo/config.toml: crates.io must be replaced by the tracked vendor/ directory")
    vendor = workspace / "vendor"
    present = sorted(p.name for p in vendor.iterdir() if p.is_dir()) if vendor.is_dir() else []
    wanted = sorted(f"{name}-{version}" for name, (version, _) in THIRD_PARTY.items())
    if present != wanted:
        errors.append(f"vendor/: crates {present} differ from the reviewed allow-list {wanted}")
    # Cargo checks each vendored file's bytes; a checkout that rewrites line endings breaks it.
    attributes = vendor / ".gitattributes"
    if not attributes.is_file() or "* -text" not in attributes.read_text(encoding="utf-8").splitlines():
        errors.append("vendor/.gitattributes must declare '* -text' so checkouts keep the checksummed bytes")
    notice = vendor / "THIRD-PARTY.md"
    notice_text = notice.read_text(encoding="utf-8") if notice.is_file() else ""
    for name, (version, licence) in THIRD_PARTY.items():
        if not all(atom.strip() in LICENCES for atom in licence.split(" OR ")):
            errors.append(f"{name}: licence {licence} is outside {sorted(LICENCES)}")
        if f"| {name} | {version} | {licence} |" not in notice_text:
            errors.append(f"vendor/THIRD-PARTY.md: missing the row for {name} {version} ({licence})")
        crate = vendor / f"{name}-{version}"
        if not crate.is_dir():
            continue
        if not (crate / ".cargo-checksum.json").is_file():
            errors.append(f"vendor/{crate.name}: .cargo-checksum.json is required")
        if not any(p.name.upper().startswith(("LICENSE", "LICENCE", "COPYING")) for p in crate.iterdir()):
            errors.append(f"vendor/{crate.name}: the licence text must be vendored")
        manifest = tomllib.loads((crate / "Cargo.toml").read_text(encoding="utf-8"))["package"]
        if (manifest["name"], manifest["version"], manifest.get("license")) != (name, version, licence):
            errors.append(f"vendor/{crate.name}: manifest identity or licence differs from the allow-list")
        builds = bool(manifest.get("build")) or (crate / "build.rs").is_file()
        if builds and name not in BUILD_SCRIPTS:
            errors.append(f"vendor/{crate.name}: an unreviewed build script would run on the build machine")
    return errors


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
