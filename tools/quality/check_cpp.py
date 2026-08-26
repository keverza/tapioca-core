"""Run the C++ architecture gate and clang-format on touched files.

The architecture checks cover the whole add-on and remain the build's blocking
gate. The clang-format check is intentionally file-oriented: unconverted legacy
sources do not block an unrelated change. Missing local developer tools warn and
pass, matching the repository's incremental-adoption policy.

    check_cpp.py                       architecture gate + style of staged files
    check_cpp.py --style-only <file>   style of named files, no architecture gate
    check_cpp.py --style-only --fix <file>
                                       format those files in place
    check_cpp.py --report              tree-wide conversion progress, always 0

The style covers layout only. The identifier convention it cannot check --
PascalCase types and functions, camelCase variables, API_ reserved to
Graphisoft -- is written in AGENTS.md.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ADDON_SRC = REPO_ROOT / "AddOn" / "EvP" / "Sources" / "AddOn"
EXCLUDED_PARTS = {"_deps", "archive", "build_29", "dist", "node_modules", "reference"}
CPP_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
SOFT_CAP = 1000

# path relative to Sources/AddOn -> (max lines allowed, why it is allowed)
# A file here may NOT grow. Shrink the number when you shrink the file.
OVERSIZED = {
    "ArchViz/ExtractionThread.cpp": (
        1011,
        "the extraction pass's control flow: the main-thread gate hops, the element "
        "cursor, the slice budget, and the abandonment rules that make a timed-out job "
        "safe. Everything in it that answers a QUESTION rather than schedules one has "
        "already been extracted along that seam -- ExtractionEnvironment.cpp (the "
        "surface pool and the sun), ExtractionSubstance.cpp (the building-material "
        "vote), and ExtractionStorySlices.cpp (the storey cut and its union, added "
        "2026-08-24). What is left is the pass itself, and splitting THAT would create "
        "a second description of the gate protocol -- the same fault "
        "DiligentViewport.cpp's entry refuses for pass ordering. This entry freezes "
        "its size; the next feature extracts a seam rather than growing it"
    ),
    "ArchViz/DiligentViewport.cpp": (
        1093,
        "the single render-thread lifecycle and frame-order authority. Device and "
        "target control, support algorithms, scene storage/draw passes, and offscreen "
        "target ownership already live in separate translation units; splitting the "
        "ordered frame body would create a second description of pass ordering. This "
        "entry freezes its current size, so future work must extract rather than grow it. "
        "+3 for the storey section overlay (2026-08-24): its 48-line frame-loop body was "
        "extracted to DiligentViewportSupport.cpp::UpdateAndDrawStorySlices, leaving only "
        "the ordered call site, which is irreducible for anything that draws in the frame",
    ),
    "Palette/ControlPalette.cpp": (
        675,
        "the palette shell - one concern; splitting it would cut the DG event "
        "routing in two (cpp-architecture-plan.md section 4 predicted ~1,100). "
        "Room has come from moving work to its own home each time, and the "
        "number here only ever goes DOWN: palette.json -> PalettePlacement, the "
        "scan -> CommandScan, the results-row click -> ResultsTable, (F3) the "
        "modeless-window callback -> PaletteRegistration.cpp, (F4) the "
        "command-list band -> CommandListPanel, the run composition -> "
        "Python/CommandLaunch, the server -> ServerBand, (PLAT-F13) Layout's "
        "band computation -> ControlPaletteLayout.cpp, and now the RUN STATE -> "
        "ControlPaletteRun.cpp, which the previous entry here named as the one "
        "owed next. That took the file from 890 to 659, and the action bar spent 16 "
        "of the 231 it freed, which is what an extraction is FOR. What remains is "
        "the DG event routing, the splitters, the placement and the command block, "
        "and CLAUDE.md names all of them as the shell's own. Nothing obvious is "
        "left to extract, so the next feature that needs a lot of lines needs a "
        "sub-object, not another shell file",
    ),
    "ArchViz/DiligentScene.cpp": (
        1185,
        "the scene's lifecycle file - shaders, pipeline states, settings. The "
        "class is already split over four TUs (DiligentSceneGeometry.cpp for the "
        "element map, DiligentSceneDraw.cpp for the passes, "
        "DiligentSceneGBuffer.cpp for the deferred prepass). This file is the "
        "PSO creation that Init owns, and the HDR scene-colour target added 233 "
        "lines of pipeline-state objects that are the same shape as the 360 "
        "lines of LDR ones above them. The natural next split is a "
        "DiligentScenePso.cpp carrying the HDR/resolve PSO creation out of Init; "
        "that is a mechanical extraction that does not change behaviour and "
        "should land when the next PSO family arrives rather than mid-feature.",
    ),
    "Palette/ControlPaletteRun.cpp": (
        320,
        "the run state, extracted from the shell. Still the shell (it defines "
        "ControlPalette methods) because CLAUDE.md assigns the run orchestration "
        "to the palette and the one Run/Cancel/Stopping button is the palette's "
        "own - see the header comment. One file so that everything about a run "
        "in flight reads in one place",
    ),
    "Palette/ControlPaletteLayout.cpp": (
        260,
        "the band layout, extracted from the shell. One top-to-bottom pass over "
        "the bands is exactly what a reader opens this file for, so it is not "
        "split further; a band that needs real logic gets its own sub-object "
        "instead, the way DescriptionPanel and ResultsTable did",
    ),
}

# Every directory below Sources/AddOn is assigned explicitly. The empty key is the
# lifecycle root: it contains only entry-point, resource, version, and facade files.
# Nested directories inherit nothing by accident; adding one requires a deliberate
# tier entry and therefore shows up in the architecture-gate diff.
TIERS = ("lifecycle", "UI", "features", "services")
TIER_ASSIGNMENTS = {
    "": "lifecycle",
    "Annotation": "services",
    "ArchViz": "features",
    "ArchViz/Dxgi": "features",
    # Registry history lives under the ArchViz module; it is not compiled, but it
    # still needs an explicit assignment because the tier scan covers every folder.
    "ArchViz/task-history": "features",
    "Diagnostics": "features",
    # Dynamo 4 is an optional external runtime lifecycle. It owns no palette or
    # document behavior and remains load-safe when the runtime is absent.
    "Dynamo": "services",
    "Geometry": "services",
    # The in-process Rhino/Grasshopper host: a runtime lifecycle owned on behalf
    # of whatever UI eventually drives it, in the same tier as Python/ for the
    # same reason. It knows nothing about palettes and nothing about documents.
    "Grasshopper": "services",
    "Metadata": "services",
    "NodeGraph": "services",
    "NativeCommands": "features",
    "Notebook": "UI",
    "Notify": "services",
    "Palette": "UI",
    "PlanOverlay": "features",
    "Preview": "services",
    "Python": "services",
    "Screenshot": "services",
    "Server": "services",
}

# The private workspace keeps registry history beside the ArchViz sources. The
# public core intentionally exports no task history, so that optional directory
# must not become a false architecture failure there.
if not (ADDON_SRC / "ArchViz" / "task-history").is_dir():
    TIER_ASSIGNMENTS.pop("ArchViz/task-history", None)

ROOT_FILES = {
    "APIEnvir.h",
    "AddOnCommands.hpp",
    "AddOnMain.cpp",
    "AddOnVersion.hpp",
    "ResourceIds.hpp",
}

# These headers are the small lifecycle-owned foundation that every tier may use.
# They carry the SDK prelude, generated/resource IDs, product metadata, and the
# native facade; treating them as boundary headers avoids making every consumer
# depend on the lifecycle implementation itself.
FOUNDATION_HEADERS = ROOT_FILES - {"AddOnMain.cpp"}

LOCAL_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')

# Existing integration adapters cross a boundary to register or dispatch a domain.
# They are intentionally file-specific: a new sideways feature include must be
# added here with its reason, rather than silently weakening the tier rule.
BOUNDARY_INCLUDE_EXCEPTIONS = {
    ("NativeCommands/ArchVizCommands.cpp", "ArchViz/ArchVizPanel.hpp"),
    ("NativeCommands/ArchVizCommands.cpp", "ArchViz/DiligentFxLink.hpp"),
    ("NativeCommands/ArchVizCommands.cpp", "ArchViz/DiligentProbe.hpp"),
    ("NativeCommands/ArchVizCommands.cpp", "ArchViz/D3D12FeasibilityProbe.hpp"),
    ("NativeCommands/ArchVizCommands.cpp", "ArchViz/DiligentViewport.hpp"),
    # Extracted OUT of ArchVizCommands.cpp (2026-08-24) when that file reached the
    # size cap, and inherits its neighbour's reason unchanged: the capture command
    # decodes its parameters straight into the viewport's own CameraStart and
    # CaptureOverlays structs, so it must name them. Restating those two structs on
    # this side of the boundary would create a second definition of the capture
    # contract, which is worse than the sideways include.
    ("NativeCommands/ArchVizCaptureParams.hpp", "ArchViz/DiligentViewport.hpp"),
    ("NativeCommands/ArchVizCaptureParams.cpp", "ArchViz/DiligentViewport.hpp"),
    ("NativeCommands/ArchVizCommands.cpp", "ArchViz/SelectionBridge.hpp"),
    ("NativeCommands/ArchVizCommands.cpp", "ArchViz/ViewportOverlayWindow.hpp"),
    ("NativeCommands/CommandBase.hpp", "Diagnostics/ApiError.hpp"),
    ("NativeCommands/CommandUtils.cpp", "Diagnostics/ApiError.hpp"),
    ("NativeCommands/PlanGeometryCommands.cpp", "ArchViz/DiligentViewport.hpp"),
    ("NativeCommands/PlanOverlayCommands.cpp", "Diagnostics/ApiError.hpp"),
    ("NativeCommands/PlanOverlayCommands.cpp", "PlanOverlay/OverlayWindow.hpp"),
    ("NativeCommands/PlanTrackCommands.cpp", "Diagnostics/ApiError.hpp"),
    ("NativeCommands/PlanTrackCommands.cpp", "PlanOverlay/OverlayWindow.hpp"),
    # Narrow bus adapter: parsing and render-queue publication remain owned by
    # ArchViz rather than moving into the native command registry.
    ("NativeCommands/PointCloudCommands.cpp", "ArchViz/PointCloudLoader.hpp"),
    ("Notify/BackgroundArm.cpp", "Diagnostics/ApiError.hpp"),
    ("Notify/ChangeTracker.cpp", "Diagnostics/ApiError.hpp"),
    ("Notify/ModelDiff.cpp", "Diagnostics/ApiError.hpp"),
    ("Python/ApiCommandCatalog.cpp", "NativeCommands/CommandSchemas.hpp"),
    ("Python/ApiCommandCatalog.cpp", "NativeCommands/SchemaValidator.hpp"),
    ("Python/ApiDispatcher.cpp", "Diagnostics/ApiError.hpp"),
    ("Python/ApiDispatcher.cpp", "NativeCommands/CommandSchemas.hpp"),
    ("Python/ApiDispatcher.cpp", "NativeCommands/SchemaValidator.hpp"),
    ("Python/ApiDispatcher.cpp", "Palette/ControlPalette.hpp"),
    ("Python/CommandRunner.cpp", "Diagnostics/ApiError.hpp"),
    ("ArchViz/ExtractionThread.cpp", "Diagnostics/ApiError.hpp"),
    # Extracted OUT of ExtractionThread.cpp (2026-08-24) and inherits its reason
    # unchanged: it makes one ACAPI call, and CLAUDE.md forbids reporting a bare
    # GSErrCode. Decoding it is the whole purpose of the include.
    ("ArchViz/ExtractionStorySlices.cpp", "Diagnostics/ApiError.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/ArchVizPanel.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/CameraSyncMode.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/CameraWake.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/DiligentViewport.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/Dxgi/HookMarker.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/Dxgi/HostComposite.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/Dxgi/PresentHook.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/ExperimentGuard.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/ModelWatch.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/NavLog.hpp"),
    ("NativeCommands/ViewerSyncCommands.cpp", "ArchViz/SelectionBridge.hpp"),
}

EVPPY_FORBIDDEN_INCLUDES = {
    "APIEnvir.h",
    "ACAPinc.h",
    "DGModule.hpp",
}


def _staged_paths() -> list[str]:
    result = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMRTUXB"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return [line for line in result.stdout.splitlines() if line]


def _cpp_files(paths: list[str]) -> list[str]:
    selected: list[str] = []
    for value in paths:
        path = Path(value)
        if path.is_absolute():
            try:
                path = path.resolve().relative_to(REPO_ROOT)
            except ValueError:
                continue
        path = Path(*path.parts)
        if path.suffix.lower() not in CPP_EXTENSIONS:
            continue
        if any(part in EXCLUDED_PARTS for part in path.parts):
            continue
        if (REPO_ROOT / path).is_file():
            selected.append(path.as_posix())
    return sorted(set(selected))


def _clang_format_executable() -> str | None:
    """Find clang-format on PATH, else in this Python environment.

    `pip install -r dev-requirements.txt` brings the clang-format wheel, which
    drops its binary in a Scripts/bin directory that is usually NOT on PATH.
    Resolving the wheel too is what makes the documented one-line setup enough
    on a fresh machine; without it the check silently skips everywhere.
    """
    found = shutil.which("clang-format")
    if found is not None:
        return found
    try:
        import clang_format
    except ImportError:
        return None
    try:
        executable = clang_format.get_executable("clang-format")
    except Exception:
        return None
    return str(executable) if Path(executable).is_file() else None


def _check_format(paths: list[str], fix: bool) -> int:
    files = _cpp_files(paths)
    if not files:
        print("No touched C/C++ files to check.")
        return 0

    clang_format = _clang_format_executable()
    if clang_format is None:
        print("WARNING: clang-format was not found; C/C++ style check skipped.")
        print("         Install it with `pip install -r dev-requirements.txt`.")
        return 0

    if fix:
        print(f"Formatting {len(files)} C/C++ file(s) in place.")
        command = [clang_format, "--style=file", "-i", *files]
        return subprocess.run(command, cwd=REPO_ROOT, check=False).returncode

    print(f"Checking clang-format for {len(files)} touched C/C++ file(s).")
    command = [clang_format, "--style=file", "--dry-run", "-Werror", *files]
    status = subprocess.run(command, cwd=REPO_ROOT, check=False).returncode
    if status != 0:
        print(
            "\nThese files are not in the repository C++ style. Format the files "
            "THIS commit touches and nothing else:\n"
            "  python tools/quality/check_cpp.py --style-only --fix <file> ...\n"
            "Legacy files stay unconverted on purpose; never mass-format to get "
            "a commit through."
        )
    return status


def _report_format(fix: bool) -> int:
    """Count tree-wide non-conforming files. Informational: always exits 0.

    A blocking whole-tree gate on a partly converted tree fails every commit,
    gets bypassed once, and is then bypassed forever. The blocking check is
    per-file (_check_format); this is the progress number for a cleanup session.
    """
    clang_format = _clang_format_executable()
    if clang_format is None:
        print("WARNING: clang-format was not found; the style report cannot run.")
        return 0

    files = [_display_repo_path(path) for path in _first_party_cpp_files()]
    if fix:
        print(f"Formatting all {len(files)} first-party C/C++ file(s) in place.")
        subprocess.run([clang_format, "--style=file", "-i", *files], cwd=REPO_ROOT, check=False)
        print("Record the resulting commit in .git-blame-ignore-revs.")
        return 0

    unconverted = [
        path
        for path in files
        if subprocess.run(
            [clang_format, "--style=file", "--dry-run", "-Werror", path],
            cwd=REPO_ROOT,
            capture_output=True,
            check=False,
        ).returncode
        != 0
    ]
    print(f"{len(files) - len(unconverted)} of {len(files)} first-party C/C++ files are in style.")
    if unconverted:
        print(f"{len(unconverted)} not yet converted (informational; this check never fails a commit):")
        for path in unconverted:
            print(f"  {path}")
    return 0


def _display_repo_path(path: Path) -> str:
    return path.relative_to(REPO_ROOT).as_posix()


def _first_party_cpp_files() -> list[Path]:
    sources = REPO_ROOT / "AddOn" / "EvP" / "Sources"
    return sorted(
        path
        for path in sources.rglob("*")
        if path.suffix.lower() in CPP_EXTENSIONS
        and path.is_file()
        and not any(part in EXCLUDED_PARTS for part in path.relative_to(REPO_ROOT).parts)
    )


def _relative_addon_path(path: Path) -> str:
    return path.relative_to(ADDON_SRC).as_posix()


def _source_files() -> list[Path]:
    return sorted(
        path
        for root, _dirs, files in os.walk(ADDON_SRC)
        for name in files
        if (path := Path(root) / name).suffix.lower() in CPP_EXTENSIONS
    )


def _source_directories() -> set[str]:
    directories = set()
    for root, _dirs, _files in os.walk(ADDON_SRC):
        directories.add(Path(root).relative_to(ADDON_SRC).as_posix())
    directories.discard(".")
    directories.add("")
    return directories


def _tier_for_path(path: Path) -> str | None:
    directory = path.relative_to(ADDON_SRC).parent.as_posix()
    return TIER_ASSIGNMENTS.get("" if directory == "." else directory)


def _feature_module(path: Path) -> str:
    parts = path.relative_to(ADDON_SRC).parts
    return parts[0] if parts else ""


def _check_architecture_root_and_tiers(failures: list[str]) -> None:
    root_files = {path.name for path in ADDON_SRC.iterdir() if path.is_file()}
    for name in sorted(root_files - ROOT_FILES):
        failures.append(
            f"ROOT: Sources/AddOn/{name} is not in the lifecycle root allowlist. "
            "Move it to its owning tier directory or add a justified lifecycle entry."
        )

    actual_directories = _source_directories()
    for directory in sorted(actual_directories - set(TIER_ASSIGNMENTS)):
        label = directory or "."
        failures.append(
            f"TIER: Sources/AddOn/{label} has no declared tier. Add it to "
            "TIER_ASSIGNMENTS before adding source files there."
        )
    for directory in sorted(set(TIER_ASSIGNMENTS) - actual_directories):
        label = directory or "."
        failures.append(f"TIER: declared directory Sources/AddOn/{label} does not exist.")
    for directory, tier in sorted(TIER_ASSIGNMENTS.items()):
        if tier not in TIERS:
            label = directory or "."
            failures.append(
                f"TIER: Sources/AddOn/{label} declares unknown tier {tier!r}; choose one of {', '.join(TIERS)}."
            )


def _resolve_local_include(source: Path, included: str) -> Path | None:
    sources_root = ADDON_SRC.parent
    candidates = (
        source.parent / included,
        ADDON_SRC / included,
        sources_root / included,
        sources_root / "EvPPy" / included,
    )
    for candidate in candidates:
        if not candidate.is_file():
            continue
        try:
            candidate.relative_to(sources_root)
        except ValueError:
            continue
        return candidate.resolve()
    return None


def _check_architecture_include_direction(failures: list[str]) -> None:
    tier_order = {tier: index for index, tier in enumerate(TIERS)}
    for source in _source_files():
        source_rel = _relative_addon_path(source)
        source_tier = _tier_for_path(source)
        if source_tier is None:
            continue
        text = source.read_text(encoding="utf-8")
        for number, line in enumerate(text.splitlines(), 1):
            match = LOCAL_INCLUDE.match(line)
            if match is None:
                continue
            target = _resolve_local_include(source, match.group(1))
            if target is None:
                continue

            try:
                target_rel = _relative_addon_path(target)
            except ValueError:
                continue
            target_tier = _tier_for_path(target)
            if target_tier is None:
                continue
            if target_rel in FOUNDATION_HEADERS and target.parent == ADDON_SRC:
                continue
            if (source_rel, target_rel) in BOUNDARY_INCLUDE_EXCEPTIONS:
                continue

            if (
                source_tier == "features"
                and target_tier == "features"
                and _feature_module(source) != _feature_module(target)
            ):
                failures.append(
                    f"INCLUDE: {source_rel}:{number} includes {target_rel}; "
                    "feature modules may not include sideways. Record a narrow "
                    "boundary adapter exception or move the dependency down."
                )
                continue

            if tier_order[source_tier] > tier_order[target_tier]:
                failures.append(
                    f"INCLUDE: {source_rel}:{number} ({source_tier}) includes "
                    f"{target_rel} ({target_tier}); includes point down only."
                )


def _without_comments(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def _check_architecture_boundaries(failures: list[str]) -> None:
    sources_root = ADDON_SRC.parent
    evppy_root = sources_root / "EvPPy"
    for path in sorted(evppy_root.rglob("*")):
        if path.suffix.lower() not in CPP_EXTENSIONS or not path.is_file():
            continue
        body = path.read_text(encoding="utf-8")
        for number, line in enumerate(body.splitlines(), 1):
            match = LOCAL_INCLUDE.match(line)
            if match and match.group(1) in EVPPY_FORBIDDEN_INCLUDES:
                failures.append(
                    f"SDK: Sources/EvPPy/{path.name}:{number} includes "
                    f"{match.group(1)}; EvPPy.dll is Archicad-header-free."
                )
        code = _without_comments(body)
        for token in (r"\bACAPI_[A-Za-z0-9_]+", r"\bGS::", r"\bAPI_[A-Za-z0-9_]+"):
            if re.search(token, code):
                failures.append(
                    f"SDK: Sources/EvPPy/{path.name} contains {token}; the CPython "
                    "binary may expose only the POD extern-C contract."
                )

    for path in _source_files():
        source_rel = _relative_addon_path(path)
        body = path.read_text(encoding="utf-8")
        for number, line in enumerate(body.splitlines(), 1):
            match = LOCAL_INCLUDE.match(line)
            if match is None:
                continue
            included = match.group(1)
            if included in ("Python.h", "PythonEmbed.h"):
                failures.append(
                    f"ABI: {source_rel}:{number} includes {included}; only "
                    "Sources/EvPPy may include the CPython headers."
                )
            if included == "EvPPyApi.h" and source_rel != "Python/PythonHost.cpp":
                failures.append(
                    f"ABI: {source_rel}:{number} includes EvPPyApi.h; the add-on "
                    "side of the bridge is limited to Python/PythonHost.cpp."
                )

    api_header = evppy_root / "EvPPyApi.h"
    if api_header.is_file():
        code = _without_comments(api_header.read_text(encoding="utf-8"))
        if 'extern "C"' not in code or "uint16_t" not in code:
            failures.append("ABI: Sources/EvPPy/EvPPyApi.h must declare an extern-C, uint16_t/POD bridge contract.")
        for token in (r"\bGS::", r"\bstd::", r"\bAPI_[A-Za-z0-9_]+", r"\bACAPI_"):
            if re.search(token, code):
                failures.append(
                    f"ABI: Sources/EvPPy/EvPPyApi.h contains {token}; no C++ or "
                    "Archicad types may cross the binary boundary."
                )

    cmake = REPO_ROOT / "AddOn" / "EvP" / "CMakeLists.txt"
    if cmake.is_file():
        text = cmake.read_text(encoding="utf-8")
        addon_links = re.findall(r"target_link_libraries\s*\(\s*\$\{ADDON_LIB\}(.*?)\)", text, re.S)
        if any("python312" in block.lower() for block in addon_links):
            failures.append(
                "ABI: the EvP.apx target links python312 directly; CPython belongs only to the separate EvPPy target."
            )


def _check_architecture_sizes(failures: list[str]) -> None:
    for root, _dirs, files in os.walk(ADDON_SRC):
        for name in sorted(files):
            if not name.endswith(".cpp"):
                continue
            path = Path(root) / name
            key = _relative_addon_path(path)
            lines = len(path.read_text(encoding="utf-8").splitlines())
            if key in OVERSIZED:
                allowed, why = OVERSIZED[key]
                if lines > allowed:
                    failures.append(
                        f"SIZE: {key} is {lines} lines, above its recorded {allowed}. "
                        f"It is already a stated exception ({why}) - it may not grow. "
                        "Split it, or move code out."
                    )
            elif lines > SOFT_CAP:
                failures.append(
                    f"SIZE: {key} is {lines} lines, over the ~{SOFT_CAP}-line soft "
                    "cap. Split it in this commit series, or add it to "
                    "OVERSIZED in tools/quality/check_cpp.py with the reason."
                )


def _capture_output(function):
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        result = function()
    return result, buffer.getvalue()


def _check_architecture_palette(failures: list[str]) -> None:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import check_structure

    shell = ADDON_SRC / "Palette" / "ControlPalette.cpp"
    result, _output = _capture_output(lambda: check_structure.main(str(shell)))
    if result != 0:
        failures.append(
            "PALETTE: a ControlPalette method touches both sub-object member groups. "
            "Run `python tools/quality/check_structure.py` for the list - the shell "
            "must call the sub-object, not reach into it."
        )


def _check_architecture_subobjects(failures: list[str]) -> None:
    palette = ADDON_SRC / "Palette"
    # The shell is deliberately more than one .cpp: ControlPaletteParams.cpp set
    # that precedent; ControlPaletteLayout.cpp and ControlPaletteRun.cpp follow
    # it. These files DEFINE
    # ControlPalette methods, so the "never call into the shell" rule below
    # cannot apply to them — they are the shell.
    shell_implementation_files = {
        "ControlPalette.cpp",
        "ControlPalette.hpp",
        "ControlPaletteAutoPreview.cpp",
        "ControlPaletteLayout.cpp",
        "ControlPaletteMenu.cpp",
        "ControlPaletteParams.cpp",
        "ControlPaletteRun.cpp",
        "PaletteRegistration.cpp",
    }
    for path in sorted(palette.iterdir()):
        if path.name in shell_implementation_files:
            continue
        if path.suffix not in (".cpp", ".hpp"):
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            if line.lstrip().startswith(("//", "#include")):
                continue
            if "ControlPalette::" in line or re.search(r"\bshell\s*\.", line):
                failures.append(
                    f"SUBOBJECT: Palette/{path.name}:{number} calls into the shell. "
                    "A sub-object answers the shell (PlaceAt/Handle*/Collect*); it "
                    "never drives it. Return what the shell needs instead."
                )


def _check_architecture_registry(failures: list[str]) -> None:
    native = ADDON_SRC / "NativeCommands"
    exported = {}
    for path in sorted(native.glob("*Commands.cpp")):
        body = path.read_text(encoding="utf-8")
        providers = sorted(set(re.findall(r"\b(Get\w+CommandRegistrations)\s*\(\s*\)", body)))
        if len(providers) != 1:
            failures.append(
                f"REGISTRY: NativeCommands/{path.name} defines {len(providers)} domain "
                f"providers {providers}; a domain file exports exactly one "
                "Get<Domain>CommandRegistrations."
            )
            continue
        exported[providers[0]] = path.name

    registry = native / "CommandRegistry.cpp"
    text = registry.read_text(encoding="utf-8")
    array = re.search(r"domainProviders\s*\[\]\s*=\s*\{(.*?)\}", text, re.S)
    if array is None:
        failures.append("REGISTRY: CommandRegistry.cpp has no domainProviders[] array.")
        return
    registered = set(re.findall(r"&(Get\w+CommandRegistrations)", array.group(1)))

    for provider, name in exported.items():
        if provider not in registered:
            failures.append(
                f"REGISTRY: {provider} (NativeCommands/{name}) is not in "
                "CommandRegistry.cpp's domainProviders[] - its commands can never be "
                "reached. Adding a domain is a file pair PLUS one registry line."
            )
    for provider in sorted(registered - set(exported)):
        failures.append(
            f"REGISTRY: CommandRegistry.cpp registers {provider}, which no NativeCommands/*Commands.cpp defines."
        )


def _check_architecture_citations(failures: list[str]) -> None:
    pattern = re.compile(r"`?([A-Za-z0-9_]+\.(?:cpp|hpp))[:.](\d+)")
    for name in ("CLAUDE.md", "AGENTS.md"):
        path = REPO_ROOT / name
        if not path.exists():
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            match = pattern.search(line)
            if match:
                failures.append(
                    f"CITATIONS: {name}:{number} cites {match.group(1)}:{match.group(2)}. "
                    "These two docs are read every session - cite `file -> symbol`, "
                    "never a line number."
                )


# ⚠️ SHADER STAGES WHOSE TEXTURES ARE *ALL* BOUND THROUGH AN SRB, so every one
# of them must be declared DYNAMIC or MUTABLE in its pipeline's resource layout.
#
# This is a hand-maintained table and that is deliberate, exactly as
# check_hlsl.py's PRELUDES is: a stage added here that does not belong fails
# noisily, which is far better than the table silently drifting from
# DiligentScene::Init and the rule quietly checking nothing.
#
# ⚠️ THE MESH PIXEL SHADER IS NOT IN THIS TABLE AND MUST NOT BE. It reads
# `g_shadowMap` and `g_envMap`, which are correctly STATIC -- each is allocated
# once and refilled, which is the whole reason EnvironmentMap fixes its own
# size -- alongside `g_ambientOcclusion`, which is correctly DYNAMIC because
# DiligentFX reallocates it on resize. Sweeping that stage in would demand the
# wrong thing of two of the three.
SRB_BOUND_SHADER_STAGES = (
    "kArchVizGBufferDebugPS",
    "kArchVizAmbientOcclusionDebugPS",
    "kArchVizResolvePS",
)


def _check_architecture_srb_variables(failures: list[str]) -> None:
    """A shader variable reached through an SRB must be MUTABLE or DYNAMIC.

    ⚠️ THIS RULE EXISTS BECAUSE BREAKING IT CRASHED ARCHICAD ON 2026-08-21.
    Diligent puts STATIC shader variables on the PIPELINE and everything else on
    the SRB, so `srb->GetVariableByName(...)` returns NULL for a variable left
    STATIC -- and the ArchViz bind sites dereferenced that result directly. A
    texture added to a shader without the matching one-line entry in its PSO's
    `ShaderResourceVariableDesc[]` therefore took the host process down, on the
    first frame that had geometry to draw, with a single "No resource is
    assigned to static shader variable" line in archviz.log as the only warning.

    The bind sites are defensive now, so the same mistake would log and disable
    a debug view instead. This catches it one step earlier, offline, for free.

    ⚠️ IT LOOKS AT THE SHADER SOURCE, NOT AT THE BIND SITES, AND THE FIRST
    VERSION OF THIS RULE DID THE OPPOSITE AND CAUGHT NOTHING. Scanning for
    `GetVariableByName(stage, "literal")` misses every bind that goes through a
    helper taking the name as a parameter -- which is precisely the shape the
    crash fix introduced. The declarations in the HLSL are the durable place to
    read the list from.
    """
    archviz = REPO_ROOT / "AddOn/EvP/Sources/AddOn/ArchViz"
    shaders = archviz / "DiligentShaders.hpp"
    if not archviz.is_dir() or not shaders.is_file():
        return

    declare_pattern = re.compile(
        r"Diligent::SHADER_TYPE_[A-Z_]+\s*,\s*\"([A-Za-z0-9_]+)\"\s*,\s*"
        r"Diligent::SHADER_RESOURCE_VARIABLE_TYPE_(?:DYNAMIC|MUTABLE)"
    )
    comment_pattern = re.compile(r"//[^\n]*")

    declared: set[str] = set()
    for path in sorted(archviz.glob("*.cpp")):
        text = comment_pattern.sub("", path.read_text(encoding="utf-8"))
        declared.update(match.group(1) for match in declare_pattern.finditer(text))

    source = shaders.read_text(encoding="utf-8")
    blocks = dict(re.findall(r'(\w+)\s*=\s*R"hlsl\((.*?)\)hlsl";', source, re.S))
    # A resource declaration at the top level of a stage: `Texture2D<float> g_x;`,
    # `Texture2D g_x;`, `Buffer<uint> g_x;`. Samplers are excluded -- they are
    # immutable samplers here, which live in a different table entirely.
    resource_pattern = re.compile(
        r"^\s*(?:Texture2D|Texture2DArray|TextureCube|Buffer|StructuredBuffer)"
        r"(?:<[^>]*>)?\s+([A-Za-z0-9_]+)\s*;",
        re.M,
    )

    for stage in SRB_BOUND_SHADER_STAGES:
        body = blocks.get(stage)
        if body is None:
            failures.append(
                f"SRB: SRB_BOUND_SHADER_STAGES names `{stage}`, which is not a shader "
                "literal in DiligentShaders.hpp. Either the stage was renamed or the "
                "table in check_cpp.py is stale; both mean this rule is not checking "
                "what it claims to."
            )
            continue
        for name in resource_pattern.findall(body):
            if name not in declared:
                failures.append(
                    f"SRB: {stage} declares `{name}`, and every resource that stage "
                    "reads is bound through a shader resource binding - but no PSO "
                    "declares it DYNAMIC or MUTABLE, so it is STATIC, it is NOT on the "
                    "SRB, and GetVariableByName returns null there. Add a "
                    "ShaderResourceVariableDesc row for it in DiligentScene::Init. "
                    "(This exact omission crashed Archicad on 2026-08-21.)"
                )


def _run_architecture(verbose: bool) -> int:
    failures: list[str] = []
    checks = [
        ("ROOT      lifecycle allowlist and explicit directory tiers", _check_architecture_root_and_tiers),
        ("INCLUDE   downward dependency direction and feature seams", _check_architecture_include_direction),
        ("BOUNDARY  Archicad SDK and EvPPy ABI separation", _check_architecture_boundaries),
        ("SIZE      soft cap, and no listed exception grows", _check_architecture_sizes),
        ("PALETTE   no method straddles both sub-objects", _check_architecture_palette),
        ("SUBOBJECT sub-objects never call the shell", _check_architecture_subobjects),
        ("REGISTRY  one provider per domain, all registered", _check_architecture_registry),
        ("CITATIONS entry-point docs cite symbols, not lines", _check_architecture_citations),
        ("SRB       SRB-bound shader variables are never STATIC", _check_architecture_srb_variables),
    ]
    for label, check in checks:
        before = len(failures)
        check(failures)
        if verbose and len(failures) == before:
            print(f"  ok   {label}")

    if failures:
        print(f"\nARCHITECTURE CHECK FAILED - {len(failures)} problem(s):\n")
        for failure in failures:
            print(f"  * {failure}\n")
        print("These are the rules in CLAUDE.md's 'Never grow a new monolith' /")
        print("'Never put palette work back in the shell' block. If a rule is wrong")
        print("rather than the code, change it in tools/quality/check_cpp.py and say")
        print("why in the commit.")
        return 1

    print("Architecture check passed.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "--style-only",
        action="store_true",
        help="skip the whole-tree architecture gate and check only touched files",
    )
    parser.add_argument(
        "--staged",
        action="store_true",
        help="read files staged for commit (the default when no paths are given)",
    )
    parser.add_argument(
        "--fix",
        action="store_true",
        help="rewrite the selected files in place instead of reporting on them",
    )
    parser.add_argument(
        "--report",
        action="store_true",
        help="count tree-wide style conversion progress; informational, never fails",
    )
    parser.add_argument("paths", nargs="*", help="C or C++ files to check")
    args = parser.parse_args(argv)

    if args.report:
        return _report_format(args.fix)

    architecture_status = 0 if args.style_only else _run_architecture(args.verbose)
    paths = args.paths or _staged_paths()
    style_status = _check_format(paths, args.fix)
    return architecture_status or style_status


if __name__ == "__main__":
    raise SystemExit(main())
