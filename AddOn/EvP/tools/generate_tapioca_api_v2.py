"""Generate the canonical Tapioca v2 native API catalog from C++ schemas."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


# ⚠️ THESE ARE A TRIPWIRE, NOT BOOKKEEPING — a surface change must be
# DELIBERATE, so adding a command is meant to fail here until the number is
# updated in the same commit. On 2026-08-15 they were found eleven behind
# (registry read 126 against an expected 115), which means eleven commands had
# landed without anyone re-running the generator. Bump them WITH the change that
# moves them, never in a sweep afterwards.
#
# 2026-08-17, PLAT-9: +3, and only TWO of them are this change's.
# ListLibraryParts (LibraryObjectCommands) and ListFavorites (FavoriteCommands)
# are new here. The third was ALREADY THERE: a clean HEAD read 127 against the
# expected 126 before any of this work, so one command had again landed without
# the generator being re-run — exactly the drift the note above describes,
# recurring. Recorded rather than quietly absorbed, because a tripwire that
# swallows an unexplained count stops being one.
#
# 2026-08-17, PLAT-9 again: +1 for GetLibraryPartPreviewInfo, which reports one
# part's preview MIME type so the thumbnail question is answered by a real
# library instead of by a guess.
# 2026-08-21, RE51.G1: +1 for RunCloudCompare, the out-of-process point-cloud
# preprocessing boundary used by the CloudCompare crop probe.
# 2026-08-22, PLAT-RE52/53: +4 for the asynchronous Diligent capture lifecycle
# and the visible viewer camera snapshot used by reusable presets.
# 2026-08-24, RE51.G2-G5: +1 for LoadDiligentPointCloud, the worker-side PLY,
# hierarchy and owning render-queue producer.
# 2026-08-25, PLAT-WATCH-API: +2 for retained preview-scene and watch-trace
# ingestion. Both are host-independent native store writes, not model writes.
# 2026-08-31, PLAT-NODEGRAPH-RUNTIME Stage E1: +4 for the node-graph workflow
# library — GraphLibrarySave/Load/Delete/List. Graph documents only; no run
# history and no model writes, so these stay host-independent.
# 2026-08-31, PLAT-NODEGRAPH-RUNTIME: +1 for GraphSelectionAction, the selection
# set node's five buttons. It reads and sets Archicad's SELECTION, which is host
# UI state rather than model state, so it is not a model write.
# 2026-09-02, PLAT-NODEGRAPH-RUNTIME: +1 for GraphDescribeElements, the
# classification-sensitive element settings read. Strictly a READ - ADR-007
# excludes model writes from this track and there is no companion setter - so it
# is host-independent in the same sense every other element read here is.
# 2026-09-02, Diligent text rendering: +2 for SetDiligentTextLabels and
# ClearDiligentTextLabels. Both write the overlay's own label list - viewer
# state, not model state - so they stay host-independent.
# 2026-09-02, the library browser port: +1 for GetLibraryPartPreview, one part's
# preview picture as a data URI. A pure READ of a library file, and the only
# reason it exists is that a browser-hosted picker has a bus between it and the
# library where the palette has none.
# 2026-09-02, the node-graph script node: +5 for GraphScriptStatus,
# GraphScriptReload, GraphScriptCreate, GraphScriptOpen and GraphScriptReveal.
# Three READ the script file a node runs; two hand it to the shell - the editor
# associated with its extension, or Explorer with the file selected. Create writes
# one and REFUSES an existing file, which is what admits it here at all.
# 2026-09-03, the embedded script editor: +2 for GraphScriptRead and
# GraphScriptWrite. Read hands the source text to the palette's editor together
# with a hash of exactly those bytes; Write saves a buffer back and REFUSES when
# disk no longer matches that hash, returning the other version instead. Both act
# on a file the user chose, never on the model, so they stay host-independent -
# and the guard is what makes a second editor safe alongside VSCode: neither side
# can overwrite the other silently, in either direction.
# 2026-09-04, the node-as-folder model: +1 for GraphScriptAddFile, one empty
# helper created INSIDE the node's own folder. It refuses an existing file, a
# path, and the shared library root, so like GraphScriptCreate it can bring a
# file into existence and can never destroy one.
EXPECTED_REGISTRY_COMMANDS = 172
EXPECTED_LOCAL_COMMANDS = 19
EXPECTED_TOTAL_COMMANDS = 191

RAW_JSON_PATTERN = r'R"json\((.*?)\)json"'
SCHEMA_EXPRESSION_PATTERN = rf'(?:R"json\(.*?\)json"|[A-Za-z_]\w*)'
REGISTRATION_PATTERN = re.compile(
    rf'\{{\s*"(?P<name>[A-Za-z0-9]+)"\s*,\s*'
    rf'&MakeRegisteredNativeCommand<[^>]+>\s*,\s*(?:true|false)\s*,\s*'
    rf'(?P<input>{SCHEMA_EXPRESSION_PATTERN})\s*,\s*'
    rf'(?P<output>{SCHEMA_EXPRESSION_PATTERN})\s*\}}',
    re.DOTALL,
)
LOCAL_SCHEMA_PATTERN = re.compile(
    rf'\{{\s*"(?P<name>[A-Za-z0-9]+)"\s*,\s*'
    rf'(?P<input>{SCHEMA_EXPRESSION_PATTERN})\s*,\s*'
    rf'(?P<output>{SCHEMA_EXPRESSION_PATTERN})\s*\}}',
    re.DOTALL,
)
CONSTANT_PATTERN = re.compile(
    rf'(?:constexpr\s+)?const\s+char\s+(?P<name>[A-Za-z_]\w*)\[\]\s*=\s*'
    rf'R"json\((?P<json>.*?)\)json"\s*;',
    re.DOTALL,
)
REGISTRY_NAME_PATTERN = re.compile(
    r'\{\s*"([A-Za-z0-9]+)"\s*,\s*&MakeRegisteredNativeCommand<'
)
LOCAL_VERB_PATTERN = re.compile(
    r'\{\s*"([A-Za-z0-9]+)"\s*,\s*DispatcherExecutionKind::'
)


class CatalogError(RuntimeError):
    """Raised when the C++ catalog cannot be extracted completely and exactly."""


@dataclass(frozen=True)
class Command:
    name: str
    implementation: str
    input_scheme: dict[str, Any]
    output_scheme: dict[str, Any]


@dataclass(frozen=True)
class Catalog:
    api_version: str
    definitions: dict[str, Any]
    commands: tuple[Command, ...]


def _load_json(text: str, label: str) -> dict[str, Any]:
    try:
        value = json.loads(text)
    except json.JSONDecodeError as error:
        raise CatalogError(f"unparseable JSON for {label}: {error}") from error
    if not isinstance(value, dict):
        raise CatalogError(f"schema for {label} must be a JSON object")
    return value


def _constants(source: str, path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for match in CONSTANT_PATTERN.finditer(source):
        name = match.group("name")
        if name in values:
            raise CatalogError(f"duplicate schema constant {name} in {path}")
        values[name] = match.group("json")
    return values


def _resolve_schema(expression: str, constants: dict[str, str], label: str) -> dict[str, Any]:
    raw_match = re.fullmatch(RAW_JSON_PATTERN, expression, re.DOTALL)
    if raw_match is not None:
        raw = raw_match.group(1)
    elif expression in constants:
        raw = constants[expression]
    else:
        raise CatalogError(f"missing or unsupported schema for {label}: {expression}")
    return _load_json(raw, label)


def extract_registry_commands(native_dir: Path) -> list[Command]:
    commands: list[Command] = []
    discovered_names: list[str] = []
    for path in sorted(native_dir.glob("*Commands.cpp")):
        source = path.read_text(encoding="utf-8")
        discovered_names.extend(REGISTRY_NAME_PATTERN.findall(source))
        constants = _constants(source, path)
        for match in REGISTRATION_PATTERN.finditer(source):
            name = match.group("name")
            commands.append(Command(
                name=name,
                implementation="native-registry",
                input_scheme=_resolve_schema(
                    match.group("input"), constants, f"{path.name}:{name} input"
                ),
                output_scheme=_resolve_schema(
                    match.group("output"), constants, f"{path.name}:{name} output"
                ),
            ))

    parsed_names = [command.name for command in commands]
    if parsed_names != discovered_names:
        missing = sorted(set(discovered_names) - set(parsed_names))
        raise CatalogError(f"registry rows were not fully parsed; missing: {missing}")
    _validate_unique_count(parsed_names, EXPECTED_REGISTRY_COMMANDS, "registry")
    return commands


def extract_local_commands(schemas_path: Path, verbs_path: Path) -> tuple[dict[str, Any], list[Command]]:
    source = schemas_path.read_text(encoding="utf-8")
    constants = _constants(source, schemas_path)
    definitions_match = re.search(
        rf'kSchemaDefinitions\[\]\s*=\s*R"json\((.*?)\)json"', source, re.DOTALL
    )
    if definitions_match is None:
        raise CatalogError(f"missing shared definitions in {schemas_path}")
    definitions = _load_json(definitions_match.group(1), "shared definitions")

    table_match = re.search(
        r'constexpr\s+CommandSchema\s+schemas\[\]\s*=\s*\{(.*?)\n\};',
        source,
        re.DOTALL,
    )
    if table_match is None:
        raise CatalogError(f"missing dispatcher schema table in {schemas_path}")
    table = table_match.group(1)
    commands = [
        Command(
            name=match.group("name"),
            implementation="dispatcher-local",
            input_scheme=_resolve_schema(
                match.group("input"), constants, f"{schemas_path.name}:{match.group('name')} input"
            ),
            output_scheme=_resolve_schema(
                match.group("output"), constants, f"{schemas_path.name}:{match.group('name')} output"
            ),
        )
        for match in LOCAL_SCHEMA_PATTERN.finditer(table)
    ]
    schema_names = [command.name for command in commands]
    _validate_unique_count(schema_names, EXPECTED_LOCAL_COMMANDS, "dispatcher schema")

    verb_names = LOCAL_VERB_PATTERN.findall(verbs_path.read_text(encoding="utf-8"))
    _validate_unique_count(verb_names, EXPECTED_LOCAL_COMMANDS, "dispatcher verb")
    if set(schema_names) != set(verb_names):
        raise CatalogError(
            "dispatcher schemas and local verbs differ: "
            f"schemas-only={sorted(set(schema_names) - set(verb_names))}, "
            f"verbs-only={sorted(set(verb_names) - set(schema_names))}"
        )
    return definitions, commands


def _validate_unique_count(names: list[str], expected: int, label: str) -> None:
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise CatalogError(f"duplicate {label} commands: {duplicates}")
    if len(names) != expected:
        raise CatalogError(f"unexpected {label} command count: {len(names)} (expected {expected})")


def extract_catalog(repo_root: Path) -> Catalog:
    evp_root = repo_root / "AddOn" / "EvP"
    native_dir = evp_root / "Sources" / "AddOn" / "NativeCommands"
    registry = extract_registry_commands(native_dir)
    definitions, local = extract_local_commands(
        native_dir / "CommandSchemas.cpp",
        evp_root / "Sources" / "AddOn" / "Python" / "DispatcherVerbs.cpp",
    )
    commands = registry + local
    names = [command.name for command in commands]
    _validate_unique_count(names, EXPECTED_TOTAL_COMMANDS, "total")

    api_source = (evp_root / "Sources" / "PyPackage" / "evp" / "api.py").read_text(
        encoding="utf-8"
    )
    version_match = re.search(r'^API_VERSION\s*=\s*"([^"]+)"', api_source, re.MULTILINE)
    if version_match is None:
        raise CatalogError("missing API_VERSION in evp/api.py")
    return Catalog(version_match.group(1), definitions, tuple(sorted(commands, key=lambda item: item.name)))


def build_document(catalog: Catalog) -> dict[str, Any]:
    registry_count = sum(command.implementation == "native-registry" for command in catalog.commands)
    local_count = sum(command.implementation == "dispatcher-local" for command in catalog.commands)
    return {
        "metadata": {
            "title": "Tapioca API V2",
            "apiVersion": catalog.api_version,
            "namespace": "Tapioca",
            "schemaDialect": "JSON Schema",
            "counts": {
                "nativeRegistry": registry_count,
                "dispatcherLocal": local_count,
                "total": len(catalog.commands),
            },
            "sources": [
                "AddOn/EvP/Sources/AddOn/NativeCommands/*Commands.cpp",
                "AddOn/EvP/Sources/AddOn/NativeCommands/CommandSchemas.cpp",
                "AddOn/EvP/Sources/AddOn/Python/DispatcherVerbs.cpp",
            ],
        },
        "definitions": catalog.definitions,
        "commands": [
            {
                "name": f"Tapioca.{command.name}",
                "implementation": command.implementation,
                "inputScheme": command.input_scheme,
                "outputScheme": command.output_scheme,
            }
            for command in catalog.commands
        ],
    }


def _type_label(schema: Any) -> str:
    if not isinstance(schema, dict):
        return "unknown"
    if "$ref" in schema:
        return str(schema["$ref"]).rsplit("/", 1)[-1].lstrip("#") or "reference"
    if "const" in schema:
        return json.dumps(schema["const"], ensure_ascii=False)
    if "enum" in schema:
        return " | ".join(json.dumps(value, ensure_ascii=False) for value in schema["enum"])
    schema_type = schema.get("type", "value")
    if schema_type == "array":
        return f"{_type_label(schema.get('items', {}))}[]"
    return str(schema_type)


def _schema_columns(schema: dict[str, Any]) -> tuple[str, str]:
    properties = schema.get("properties", {})
    if not isinstance(properties, dict):
        properties = {}
    required = set(schema.get("required", []))
    alternatives = schema.get("oneOf", schema.get("anyOf", []))
    conditional = {
        name
        for branch in alternatives
        if isinstance(branch, dict)
        for name in branch.get("required", [])
    }
    required_items = [
        f"`{name}`: {_type_label(value)}" for name, value in properties.items() if name in required
    ]
    if alternatives:
        groups = [
            " + ".join(f"`{name}`" for name in branch.get("required", []))
            for branch in alternatives
            if isinstance(branch, dict) and branch.get("required")
        ]
        if groups:
            required_items.append("one of: " + " OR ".join(groups))
    optional_items = [
        f"`{name}`: {_type_label(value)}"
        for name, value in properties.items()
        if name not in required and name not in conditional
    ]
    return _cell(required_items), _cell(optional_items)


def _cell(items: list[str]) -> str:
    if not items:
        return "-"
    return "<br>".join(items).replace("|", "\\|")


def _output_column(schema: dict[str, Any]) -> str:
    alternatives = schema.get("oneOf", schema.get("anyOf", []))
    if alternatives:
        variants = [
            _output_column(branch)
            for branch in alternatives
            if isinstance(branch, dict)
        ]
        return " OR ".join(variants)
    properties = schema.get("properties", {})
    if not isinstance(properties, dict) or not properties:
        return _type_label(schema)
    required = set(schema.get("required", []))
    return _cell([
        f"`{name}{'' if name in required else '?'}`: {_type_label(value)}"
        for name, value in properties.items()
    ])


def render_markdown(catalog: Catalog) -> str:
    lines = [
        "# Tapioca API V2",
        "",
        f"API version: `{catalog.api_version}`. Canonical namespace: `Tapioca.*`.",
        "Generated from the native C++ registry and dispatcher-local schema table.",
        "In Output, `?` marks a property not listed as required by its schema.",
        "",
        "| Call | Required Inputs | Optional Inputs | Output |",
        "|---|---|---|---|",
    ]
    for command in catalog.commands:
        required, optional = _schema_columns(command.input_scheme)
        lines.append(
            f"| `Tapioca.{command.name}` | {required} | {optional} | "
            f"{_output_column(command.output_scheme)} |"
        )
    return "\n".join(lines) + "\n"


def generate(repo_root: Path, output_dir: Path | None = None) -> tuple[Path, Path]:
    catalog = extract_catalog(repo_root)
    destination = output_dir or repo_root / "dist"
    destination.mkdir(parents=True, exist_ok=True)
    json_path = destination / "TAPIOCA-API-V2.json"
    markdown_path = destination / "TAPIOCA-API-V2.md"
    json_path.write_text(
        json.dumps(build_document(catalog), indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    markdown_path.write_text(render_markdown(catalog), encoding="utf-8", newline="\n")
    return json_path, markdown_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_root = Path(__file__).resolve().parents[3]
    parser.add_argument("--repo-root", type=Path, default=default_root)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args(argv)
    try:
        json_path, markdown_path = generate(args.repo_root.resolve(), args.output_dir)
    except (CatalogError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"Wrote {EXPECTED_TOTAL_COMMANDS} commands to {json_path}")
    print(f"Wrote contract table to {markdown_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
