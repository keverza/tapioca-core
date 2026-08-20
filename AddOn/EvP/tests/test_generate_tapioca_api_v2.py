"""Focused extraction and completeness tests for the Tapioca v2 generator."""

import importlib.util
import json
import sys
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]
GENERATOR_PATH = REPO_ROOT / "AddOn" / "EvP" / "tools" / "generate_tapioca_api_v2.py"
SPEC = importlib.util.spec_from_file_location("generate_tapioca_api_v2", GENERATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
generator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generator
SPEC.loader.exec_module(generator)


def test_extracts_complete_unique_cpp_catalog():
    catalog = generator.extract_catalog(REPO_ROOT)
    registry = [item for item in catalog.commands if item.implementation == "native-registry"]
    local = [item for item in catalog.commands if item.implementation == "dispatcher-local"]

    assert len(registry) == 130
    assert len(local) == 19
    assert len(catalog.commands) == 149
    assert len({item.name for item in catalog.commands}) == 149
    assert all(item.input_scheme and item.output_scheme for item in catalog.commands)


def test_generated_artifacts_are_deterministic_and_canonical(tmp_path):
    json_path, markdown_path = generator.generate(REPO_ROOT, tmp_path)
    first_json = json_path.read_bytes()
    first_markdown = markdown_path.read_bytes()
    generator.generate(REPO_ROOT, tmp_path)

    assert json_path.read_bytes() == first_json
    assert markdown_path.read_bytes() == first_markdown
    document = json.loads(first_json)
    assert document["metadata"]["counts"] == {
        "nativeRegistry": 130,
        "dispatcherLocal": 19,
        "total": 149,
    }
    assert all(item["name"].startswith("Tapioca.") for item in document["commands"])
    assert len(markdown_path.read_text(encoding="utf-8").splitlines()) == 157


def test_unparseable_registered_schema_fails(tmp_path):
    native_dir = tmp_path / "NativeCommands"
    native_dir.mkdir()
    (native_dir / "BrokenCommands.cpp").write_text(
        '{ "Broken", &MakeRegisteredNativeCommand<BrokenCommand>, false, '
        'R"json({bad})json", R"json({})json" },\n',
        encoding="utf-8",
    )

    with pytest.raises(generator.CatalogError, match="unparseable JSON"):
        generator.extract_registry_commands(native_dir)


def test_completeness_checks_reject_duplicates_and_unexpected_counts():
    with pytest.raises(generator.CatalogError, match="duplicate registry commands"):
        generator._validate_unique_count(["Same", "Same"], 2, "registry")
    with pytest.raises(generator.CatalogError, match="unexpected registry command count"):
        generator._validate_unique_count(["Only"], 101, "registry")


def test_cli_returns_nonzero_for_missing_catalog(tmp_path):
    assert generator.main(["--repo-root", str(tmp_path)]) == 1
