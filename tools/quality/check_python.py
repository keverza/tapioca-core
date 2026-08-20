"""Scan commands and run Ruff on explicitly touched Python files.

The command scan reads every command folder the way the palette does and fails
loudly when a command would silently vanish. Ruff remains file-oriented:
unconverted legacy sources do not block an unrelated change. Missing Ruff warns
and passes, matching the repository's incremental-adoption policy.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path, PureWindowsPath

REPO_ROOT = Path(__file__).resolve().parents[2]
EVP_ROOT = REPO_ROOT / "AddOn" / "EvP"
RUFF_CONFIG = REPO_ROOT / "pyproject.toml"
SYNC_MANIFEST = EVP_ROOT / "command-sync.json"
SYNC_OVERLAY = EVP_ROOT / "command-sync.local.json"
EXCLUDED_PARTS = {"_deps", "archive", "build_29", "dist", "node_modules", "reference"}

sys.path.insert(0, str(EVP_ROOT / "Sources" / "PyPackage"))
from evp import _scanner, _schemacache  # noqa: E402


def _staged_paths() -> list[str]:
    result = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMRTUXB"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return [line for line in result.stdout.splitlines() if line]


def _python_files(paths: list[str]) -> list[str]:
    selected: list[str] = []
    for value in paths:
        path = Path(value)
        if path.is_absolute():
            try:
                path = path.resolve().relative_to(REPO_ROOT)
            except ValueError:
                continue
        path = Path(*path.parts)
        if path.suffix.lower() != ".py" or any(part in EXCLUDED_PARTS for part in path.parts):
            continue
        if (REPO_ROOT / path).is_file():
            selected.append(path.as_posix())
    return sorted(set(selected))


def _run_ruff(arguments: list[str], files: list[str]) -> int:
    command = [
        sys.executable,
        "-m",
        "ruff",
        *arguments,
        "--config",
        str(RUFF_CONFIG),
        *files,
    ]
    try:
        return subprocess.run(command, cwd=REPO_ROOT, check=False).returncode
    except FileNotFoundError:
        print("WARNING: Python is unavailable; Ruff check skipped.")
        return 0


def _is_absolute_path(value: str) -> bool:
    return Path(value).is_absolute() or PureWindowsPath(value).is_absolute()


def _resolve_command_root(value: str, label: str, allow_outside_repo: bool = False) -> Path:
    normalized = value.replace("\\", "/")
    if not normalized or _is_absolute_path(normalized):
        raise ValueError(f"invalid {label}: {value!r}")
    path = (EVP_ROOT / Path(normalized)).resolve()
    if not allow_outside_repo and REPO_ROOT not in path.parents:
        raise ValueError(f"{label} must resolve inside the repository: {value!r}")
    if not path.is_dir():
        raise ValueError(f"{label} does not exist: {path}")
    return path


def _manifest_command_roots() -> list[Path]:
    with SYNC_MANIFEST.open(encoding="utf-8") as handle:
        manifest = json.load(handle)
    roots = manifest.get("source_roots")
    if not isinstance(roots, list) or not roots:
        raise ValueError("command-sync.json must define a non-empty source_roots list")

    entries = [(root, "command sync source root", False) for root in roots]
    if SYNC_OVERLAY.is_file():
        with SYNC_OVERLAY.open(encoding="utf-8") as handle:
            overlay = json.load(handle)
        if overlay.get("version") != 1:
            raise ValueError("command-sync.local.json must have version 1")
        extra_roots = overlay.get("extra_source_roots")
        if not isinstance(extra_roots, list):
            raise ValueError("command-sync.local.json must define extra_source_roots as a list")
        entries.extend((root, "extra command sync source root", True) for root in extra_roots)

    paths = []
    seen = set()
    for root, label, allow_outside_repo in entries:
        if not isinstance(root, str):
            raise ValueError(f"{label} must be a string")
        path = _resolve_command_root(root, label, allow_outside_repo=allow_outside_repo)
        key = str(path).casefold()
        if key in seen:
            raise ValueError(f"duplicate command sync source root: {root!r}")
        seen.add(key)
        paths.append(path)
    return paths


def _scan_commands(commands: Path, verbose: bool) -> tuple[int, int]:
    records, failures = _scan_metadata(commands)

    for record in records:
        if verbose:
            metadata = record["metadata"]
            print(f"  ok   {record['folder']:<30} {metadata.get('category', '?')} / {metadata.get('title', '?')}")

    for failure in failures:
        print(f"  FAIL {failure['folder']:<30} {failure['error']}")

    print(f"{len(records) + len(failures)} command(s) scanned, {len(failures)} would be MISSING from the palette.")
    if failures:
        print(
            "A command the scanner cannot read is not an error in Archicad -- it "
            "simply does not appear. Fix these before syncing."
        )
        return len(records), len(failures)
    return len(records), 0


def _scan_metadata(commands: Path) -> tuple[list[dict[str, object]], list[dict[str, str]]]:
    if not commands.is_dir():
        return [], [{"folder": commands.name, "path": str(commands), "error": "no such folder"}]

    records: list[dict[str, object]] = []
    failures: list[dict[str, str]] = []
    if (commands / "command.py").is_file():
        command_files = [(commands.name, commands / "command.py")]
    else:
        command_files = [
            (name, commands / name / "command.py")
            for name in sorted(os.listdir(commands))
        ]
    for name, path in command_files:
        if not path.is_file():
            continue
        command_folder = path.parent
        try:
            meta = _scanner.scan_file(str(path), name)
        except Exception as exc:
            failures.append(
                {
                    "folder": name,
                    "path": _display_path(path),
                    "error": str(exc),
                }
            )
            continue
        if meta.get("inputs"):
            # A schema command's controls only exist once its module runs, so the
            # AST pass above proves nothing about them. Generate them the same way
            # scan_root does -- in a subprocess, cached -- or this gate would
            # report "0 would be MISSING" about a command that reaches the palette
            # as a diagnostic with no controls at all.
            generated = _schemacache.ports_for(str(command_folder))
            if not generated.get("ok"):
                failures.append(
                    {
                        "folder": name,
                        "path": _display_path(path),
                        "error": "inputs=%s declared, but its ports could not be "
                                 "generated, so this command reaches the palette "
                                 "with NO CONTROLS and cannot be run. %s"
                                 % (meta["inputs"], generated.get("error", "unknown")),
                    }
                )
                continue
            if generated.get("params") is not None:
                meta["params"] = generated["params"]

        palette_metadata = dict(meta)
        # The scanner's diagnostic line moves when formatting adds or removes blank lines;
        # it is not part of the palette contract being protected by this diff.
        palette_metadata.pop("line", None)
        records.append(
            {
                "folder": name,
                "path": _display_path(path),
                "metadata": palette_metadata,
            }
        )
    return records, failures


def _display_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def _scan_json(command_roots: list[Path]) -> int:
    records: list[dict[str, object]] = []
    failures: list[dict[str, str]] = []
    for command_root in command_roots:
        root_records, root_failures = _scan_metadata(command_root)
        records.extend(root_records)
        failures.extend(root_failures)

    print(
        json.dumps(
            {"commands": records, "failures": failures},
            ensure_ascii=True,
            indent=2,
            sort_keys=True,
        )
    )
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--commands", help="one command root to scan instead of the sync manifest")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "--no-scan",
        action="store_true",
        help="skip the command scan and run only the touched-file Ruff check",
    )
    parser.add_argument(
        "--scan-only",
        action="store_true",
        help="run the command scan without the touched-file Ruff check",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit deterministic parsed command metadata as JSON",
    )
    parser.add_argument("paths", nargs="*", help="Python files to check with Ruff")
    args = parser.parse_args(argv)

    scan_status = 0
    if not args.no_scan:
        try:
            command_roots = [Path(args.commands)] if args.commands else _manifest_command_roots()
            if args.json:
                scan_status = _scan_json(command_roots)
            else:
                scanned = 0
                failures = 0
                for command_root in command_roots:
                    root_scanned, root_failures = _scan_commands(command_root, args.verbose)
                    scanned += root_scanned
                    failures += root_failures
                print(f"{scanned + failures} command(s) scanned, {failures} would be MISSING from the palette.")
                scan_status = 1 if failures else 0
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            print(f"command scan did not run: {exc}")
            scan_status = 2
    if args.json:
        return scan_status
    if args.scan_only:
        return scan_status

    if importlib.util.find_spec("ruff") is None:
        print("WARNING: Ruff is not installed in this Python environment; check skipped.")
        return scan_status

    paths = args.paths or _staged_paths()
    files = _python_files(paths)
    if not files:
        print("No touched Python files to check.")
        return scan_status

    print(f"Checking Ruff format and lint for {len(files)} touched Python file(s).")
    format_status = _run_ruff(["format", "--check"], files)
    lint_status = _run_ruff(["check"], files)
    return scan_status or format_status or lint_status


if __name__ == "__main__":
    raise SystemExit(main())
