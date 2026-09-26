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

    assert len(registry) == generator.EXPECTED_REGISTRY_COMMANDS
    assert len(local) == generator.EXPECTED_LOCAL_COMMANDS
    assert len(catalog.commands) == generator.EXPECTED_TOTAL_COMMANDS
    assert len({item.name for item in catalog.commands}) == generator.EXPECTED_TOTAL_COMMANDS
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
        "nativeRegistry": generator.EXPECTED_REGISTRY_COMMANDS,
        "dispatcherLocal": generator.EXPECTED_LOCAL_COMMANDS,
        "total": generator.EXPECTED_TOTAL_COMMANDS,
    }
    assert all(item["name"].startswith("Tapioca.") for item in document["commands"])
    # Header, separator, and one row per command.
    assert (
        len(markdown_path.read_text(encoding="utf-8").splitlines())
        == generator.EXPECTED_TOTAL_COMMANDS + 8
    )


def test_pass_provenance_schema_exposes_ambiguity_causes():
    catalog = generator.extract_catalog(REPO_ROOT)
    command = next(item for item in catalog.commands if item.name == "ViewerPassProvenance")
    output = command.output_scheme

    transition_fields = {
        "nonCameraDrawTransitions",
        "sampledAmbiguousTransitions",
        "conflictingSampledPassTransitions",
        "partialCopyTransitions",
        "resourceWriteTransitions",
        "unsupportedGpuWorkTransitions",
        "secondaryCameraTargetTransitions",
        "resourceAmbiguousPresents",
        "presentContextOverlaps",
    }
    assert transition_fields <= set(output["properties"])
    assert transition_fields <= set(output["required"])

    first_draw = output["properties"]["firstKnownToAmbiguousDraw"]
    assert "firstKnownToAmbiguousDraw" in output["required"]
    assert first_draw["properties"]["drawKind"]["enum"] == [
        "INDEXED", "DIRECT", "INDEXED_INSTANCED", "INSTANCED", "AUTO",
        "INDEXED_INSTANCED_INDIRECT", "INSTANCED_INDIRECT", "UNKNOWN",
    ]
    assert first_draw["properties"]["sampledLineage"]["enum"] == [
        "NONE", "KNOWN", "AMBIGUOUS", "CONFLICTING_KNOWN_PASSES",
    ]
    assert {
        "valid", "drawsSinceCamera", "drawKind", "drawCount",
        "renderTargetSlot", "targetResource", "targetScenePass", "cameraPass",
        "sampledLineage", "shaderResourceSlot", "sampledResource",
        "sampledScenePass", "sampledAmbiguityMask", "resultingAmbiguityMask",
    } == set(first_draw["required"])
    assert first_draw["properties"]["sampledAmbiguityMask"] == {
        "type": "integer", "minimum": 0, "maximum": 127
    }
    assert first_draw["properties"]["resultingAmbiguityMask"] == {
        "type": "integer", "minimum": 0, "maximum": 127
    }

    row = output["properties"]["rows"]["items"]
    assert row["properties"]["resourceState"]["enum"] == [
        "UNKNOWN", "KNOWN", "AMBIGUOUS"
    ]
    assert row["properties"]["resourceAmbiguityMask"] == {
        "type": "integer", "minimum": 0, "maximum": 127
    }
    assert row["properties"]["presentContextOverlap"] == {"type": "boolean"}
    assert {"resourceState", "resourceAmbiguityMask", "presentContextOverlap"} <= set(
        row["required"]
    )

    pairing_fields = {
        "pairingEnabled", "provenanceEpoch", "imageDrawsCommitted",
        "cameraSnapshots", "cameraBindings", "pairingPresents",
        "pairingMatched", "pairingMismatched", "pairingUnknown",
        "pairingAmbiguous", "uniqueImagePassesObserved",
        "uniqueImagePassesClassified", "uniqueMatched", "uniqueMismatched",
        "uniqueUnknown", "uniqueAmbiguous", "duplicatePresents",
        "pairingRowsOverwritten", "pairingRows",
    }
    assert pairing_fields <= set(output["properties"])
    assert pairing_fields <= set(output["required"])

    pairing_row = output["properties"]["pairingRows"]["items"]
    assert {
        "provenanceEpoch", "eventSerial", "presentSerial", "imagePass",
        "imageRootEventSerial", "imageSourceResource", "imageModelGeneration",
        "overlayCameraSerial", "cameraSourcePass", "cameraSnapshotEventSerial",
        "cameraAdoptEventSerial", "ambiguityEventSerial", "cameraMetadataPass", "backBuffer", "delta",
        "relation", "imageOnBackBuffer", "cameraCoherent",
        "presentContextOverlap", "handoffSerial", "imageGeneration",
        "imageRooted", "cameraImageGeneration", "metadataStale",
        "backBufferState",
    } == set(pairing_row["required"])
    assert pairing_row["properties"]["backBufferState"]["enum"] == [
        "NONE", "HANDOFF", "CLEARED", "MIXED",
    ]
    assert pairing_row["properties"]["relation"]["enum"] == [
        "MATCH", "MISMATCH", "UNKNOWN", "AMBIGUOUS",
    ]
    assert pairing_row["additionalProperties"] is False


def test_pass_provenance_schema_exposes_handoff_pairing_and_present_profile():
    catalog = generator.extract_catalog(REPO_ROOT)
    command = next(item for item in catalog.commands if item.name == "ViewerPassProvenance")
    output = command.output_scheme

    handoff_fields = {
        "handoffs", "handoffsUnrooted", "handoffsSameGeneration",
        "generationsWithMultipleRoots", "sceneColourChanges",
        "backBufferClears", "backBufferOtherWrites", "presentsWithoutBinding",
        "imageGeneration", "uniqueDelta", "repeatMatched", "repeatMismatched",
        "repeatUnknown", "repeatAmbiguous", "repeatDelta", "presentProfile",
    }
    assert handoff_fields <= set(output["properties"])
    assert handoff_fields <= set(output["required"])
    for histogram in ("uniqueDelta", "repeatDelta"):
        assert output["properties"][histogram]["minItems"] == 7
        assert output["properties"][histogram]["maxItems"] == 7

    profile = output["properties"]["presentProfile"]
    assert profile["additionalProperties"] is False
    assert set(profile["required"]) == {
        "enabled", "presents", "present1Calls", "syncInterval", "doNotSequence",
        "restart", "doNotWait", "restrictToOutput", "useDuration", "allowTearing",
        "otherFlags", "flagsSeen", "descQueries", "descFailures", "swapChain",
    }
    # A raw UINT flag word must never be range-checked as a small integer: an
    # undocumented bit would fail the call inside Archicad.
    assert profile["properties"]["flagsSeen"] == {"type": "string"}
    chain = profile["properties"]["swapChain"]
    assert chain["additionalProperties"] is False
    assert set(chain["required"]) == {
        "known", "swapChain", "bufferCount", "swapEffect", "bufferUsage", "flags",
        "format", "width", "height", "sampleCount", "windowed", "desc1Known",
        "scaling", "alphaMode",
    }


def test_pass_provenance_schema_exposes_camera_agreement():
    catalog = generator.extract_catalog(REPO_ROOT)
    command = next(item for item in catalog.commands if item.name == "ViewerPassProvenance")
    output = command.output_scheme

    assert "cameraAgreement" in output["required"]
    agreement = output["properties"]["cameraAgreement"]
    assert agreement["additionalProperties"] is False
    assert {
        "imagesMoving", "rootSame", "rootPrevious", "rootAhead", "rootNeither",
        "rootMissing", "rootPoseMissing", "referenceMissing", "readbacksPending",
        "readbackFailures", "keys", "dump",
    } <= set(agreement["required"])
    key = agreement["properties"]["keys"]["items"]
    assert key["additionalProperties"] is False
    assert {
        "count", "ordinal", "kind", "formsB1", "formsB2", "moving", "rootSame",
        "rootPrevious", "rootAhead", "rootNeither", "vsReferenceSame",
        "vsReferencePrevious", "wasRoot", "wasReference",
    } <= set(key["required"])
    # One count per Form enumerator, in wire order.
    assert key["properties"]["formsB1"]["minItems"] == key["properties"]["formsB1"]["maxItems"] == 8
    window = agreement["properties"]["dump"]["items"]["properties"]["draws"]["items"]["properties"]["windows"]["items"]
    assert window["additionalProperties"] is False
    # Raw matrix values travel as %.9g strings so a float round-trips.
    assert window["properties"]["values"]["items"] == {"type": "string"}
    assert window["properties"]["values"]["minItems"] == window["properties"]["values"]["maxItems"] == 16


def test_pass_provenance_schema_exposes_presented_content():
    catalog = generator.extract_catalog(REPO_ROOT)
    command = next(item for item in catalog.commands if item.name == "ViewerPassProvenance")
    output = command.output_scheme

    assert "presentedContent" in output["required"]
    content = output["properties"]["presentedContent"]
    assert content["additionalProperties"] is False
    assert {
        "presentsProcessed", "composited", "compositedChanged", "firstRepeats", "firstRepeatSame",
        "firstRepeatOther", "trueDelta", "screenSeconds", "bookkeepingDisagrees", "frames",
    } <= set(content["required"])
    # One bucket per true delta: <=-2, -1, 0, +1, >=+2.
    assert content["properties"]["trueDelta"]["minItems"] == content["properties"]["trueDelta"]["maxItems"] == 5
    frame = content["properties"]["frames"]["items"]
    assert frame["additionalProperties"] is False
    assert frame["properties"]["relation"]["enum"] == [
        "UNKNOWN", "COMPOSITED", "SAME_BUFFER", "OTHER_BUFFER", "UNDECIDABLE", "UNIDENTIFIED",
    ]
    # The camera agreement dump no longer carries a field nothing measures.
    draw = output["properties"]["cameraAgreement"]["properties"]["dump"]["items"]["properties"]["draws"]["items"]
    assert "vertexShader" not in draw["properties"]
    # Stage 76: what the hooks saw before each Present, and the A/B switch.
    assert {"coverage", "presentsWithRepairAfterCalls", "slotRepairs"} <= set(content["required"])
    coverage = content["properties"]["coverage"]["items"]
    assert coverage["additionalProperties"] is False
    assert set(coverage["required"]) == {"relation", "presents", "calls", "draws", "noDraws", "repairs"}
    assert command.input_scheme["properties"]["repairAfterCalls"] == {"type": "boolean"}


def test_pass_provenance_schema_exposes_image_transfer():
    catalog = generator.extract_catalog(REPO_ROOT)
    command = next(item for item in catalog.commands if item.name == "ViewerPassProvenance")
    output = command.output_scheme

    assert "imageTransfer" in output["properties"]
    assert "imageTransfer" in output["required"]
    image_transfer = output["properties"]["imageTransfer"]
    assert image_transfer["additionalProperties"] is False
    assert set(image_transfer["required"]) == {"stats", "captures", "events"}

    stats = image_transfer["properties"]["stats"]
    assert set(stats["required"]) == {
        "enabled", "epoch", "capturesOpened", "capturesClosed",
        "capturesTruncated", "capturesAbortedByResize", "rootsSeen",
        "eventsRecorded", "eventsDropped", "eventsAfterClose",
        "unmodelledWork", "trackedOverflow", "lastBackBuffer",
    }
    assert stats["properties"]["capturesOpened"] == {
        "type": "integer", "minimum": 0, "maximum": 4
    }

    capture_row = image_transfer["properties"]["captures"]["items"]
    assert set(capture_row["required"]) == {
        "capture", "rootPass", "rootResource", "rootEventSerial",
        "openOrderSerial", "closeOrderSerial", "closeReason", "presentsSeen",
        "drawsSinceRoot", "rootWrites", "unmodelledWork", "trackedCount",
        "trackedOverflow", "eventsRecorded", "eventsDropped",
    }
    assert capture_row["properties"]["closeReason"]["enum"] == [
        "OPEN", "PRESENTS", "FULL", "RESIZE",
    ]
    assert capture_row["additionalProperties"] is False

    event_row = image_transfer["properties"]["events"]["items"]
    assert set(event_row["required"]) == {
        "epoch", "capture", "orderSerial", "role", "kind", "drawKind",
        "drawCount", "drawsSinceRoot", "scenePass", "rtv0", "rtvCount",
        "srv0", "srv1", "srv2", "srv3", "trackedSrvMask", "trackedSrvHits",
        "source", "destination", "resource", "backBuffer", "writesBackBuffer",
        "readsTracked", "readsRoot", "trackedResource", "trackedAdded",
        "parentResource", "succeeded",
    }
    assert event_row["properties"]["role"]["enum"] == ["CONTEXT", "PRESENT"]
    assert event_row["properties"]["kind"]["enum"] == [
        "ROOT", "NEXT_ROOT", "DRAW", "COPY", "PARTIAL_COPY", "CLEAR",
        "RESOURCE_WRITE", "UNMODELLED_WORK", "PRESENT_BEGIN", "PRESENT_END",
    ]
    assert event_row["properties"]["drawKind"]["enum"] == [
        "INDEXED", "DIRECT", "INDEXED_INSTANCED", "INSTANCED", "AUTO",
        "INDEXED_INSTANCED_INDIRECT", "INSTANCED_INDIRECT", "UNKNOWN",
    ]
    assert event_row["additionalProperties"] is False


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


def test_adjacent_raw_literals_are_one_schema(tmp_path, monkeypatch):
    # MSVC caps one string literal at 16380 bytes, so a large response schema is
    # written as adjacent pieces; the generator must read what the compiler builds.
    monkeypatch.setattr(generator, "EXPECTED_REGISTRY_COMMANDS", 1)
    native_dir = tmp_path / "NativeCommands"
    native_dir.mkdir()
    (native_dir / "SplitCommands.cpp").write_text(
        '{ "Split", &MakeRegisteredNativeCommand<SplitCommand>, false, '
        'R"json({"type": "object"})json",\n'
        '  R"json({"type": "object", "properties": {\n'
        '    "a": {"type": "string"},\n'
        ')json" R"json(\n'
        '    "b": {"type": "boolean"}}})json" },\n',
        encoding="utf-8",
    )

    (command,) = generator.extract_registry_commands(native_dir)
    assert set(command.output_scheme["properties"]) == {"a", "b"}
    assert command.input_scheme == {"type": "object"}


def test_completeness_checks_reject_duplicates_and_unexpected_counts():
    with pytest.raises(generator.CatalogError, match="duplicate registry commands"):
        generator._validate_unique_count(["Same", "Same"], 2, "registry")
    with pytest.raises(generator.CatalogError, match="unexpected registry command count"):
        generator._validate_unique_count(["Only"], 101, "registry")


def test_cli_returns_nonzero_for_missing_catalog(tmp_path):
    assert generator.main(["--repo-root", str(tmp_path)]) == 1
