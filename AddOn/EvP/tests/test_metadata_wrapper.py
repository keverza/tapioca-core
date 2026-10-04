"""evp.metadata: Tapioca's metadata verbs as a script uses them -- JSON text on the wire, dicts
in Python, a refusal raised with the schema's reasons."""

import json
import os
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
from evp import metadata


def _fake(monkeypatch, answer):
    seen = []

    def fake_call(command, params):
        seen.append((command, params))
        return SimpleNamespace(data=answer(command, params))

    monkeypatch.setattr(metadata, "call", fake_call)
    return seen


def test_get_parses_each_elements_json(monkeypatch):
    stored = {"entityId": "E1", "properties": [metadata.prop("program.usage", "office", "enum")]}
    seen = _fake(
        monkeypatch,
        lambda command, params: {
            "count": 2,
            "items": [
                {"elementId": {"guid": "A"}, "found": True, "metadata": json.dumps(stored)},
                {"elementId": {"guid": "B"}, "found": False, "metadata": json.dumps({"entityId": "E2"})},
            ],
        },
    )
    got = metadata.get(["A", "B"])
    assert seen == [
        ("Tapioca.GetElementMetadata", {"elements": [{"elementId": {"guid": "A"}}, {"elementId": {"guid": "B"}}]})
    ]
    assert got["A"]["properties"][0]["value"] == {"type": "enum", "value": "office"}
    assert got["B"] == {"entityId": "E2"}


def test_a_read_failure_is_raised_by_get_and_said_by_read(monkeypatch):
    _fake(
        monkeypatch,
        lambda command, params: {
            "count": 1,
            "items": [{"elementId": {"guid": "A"}, "found": False, "metadata": "", "error": "gone"}],
        },
    )
    with pytest.raises(metadata.MetadataError, match="gone"):
        metadata.get(["A"])
    assert metadata.read(["A"])[0]["metadata"] == {}


def test_merge_sends_the_models_json_text(monkeypatch):
    seen = _fake(
        monkeypatch,
        lambda command, params: {
            "count": 1,
            "changed": 1,
            "results": [{"elementId": {"guid": "A"}, "succeeded": True}],
        },
    )
    usage = metadata.prop("program.usage", "residential", "enum")
    metadata.merge("A", metadata.entity(ranges=[metadata.floors(1, 7, [usage])], tags=["review"]))
    command, params = seen[0]
    assert command == "Tapioca.SetElementMetadata"
    assert params["mode"] == "merge"
    sent = json.loads(params["items"][0]["metadata"])
    assert sent["ranges"] == [{"domain": "floor", "from": 1, "to": 7, "properties": [usage]}]
    assert sent["tags"] == ["review"]
    assert "properties" not in sent, "only what is given is said"


def test_a_refusal_raises_with_the_schemas_reasons(monkeypatch):
    _fake(
        monkeypatch,
        lambda command, params: {
            "count": 1,
            "changed": 0,
            "results": [
                {
                    "elementId": {"guid": "A"},
                    "succeeded": False,
                    "error": "the project's schema refuses it",
                    "problems": ["program.usage: 'hotel' is not an option of building-usage"],
                }
            ],
        },
    )
    with pytest.raises(metadata.MetadataError, match="hotel") as refused:
        metadata.replace("A", metadata.entity(properties=[metadata.prop("program.usage", "hotel", "enum")]))
    assert refused.value.guid == "A" and len(refused.value.problems) == 1


def test_a_computed_value_says_its_generator():
    p = metadata.prop(
        "analysis.sunHours.mean", 5.5, "double", source="analysis", state="computed", unit="h", generator="sunstudy"
    )
    assert p["state"] == "computed" and p["generator"] == "sunstudy" and p["unit"] == "h"
    assert p["provenance"] == {"source": "analysis"}


def test_the_schema_reads_and_extends(monkeypatch):
    seen = _fake(
        monkeypatch,
        lambda command, params: (
            {"schema": json.dumps({"revision": 3, "properties": []}), "stored": True, "revision": 3}
            if command == "Tapioca.GetMetadataSchema"
            else {"revision": 4, "added": 1}
        ),
    )
    whole = metadata.schema()
    assert whole["stored"] is True and whole["revision"] == 3
    assert metadata.extend_schema({"tags": ["parking"]}) == {"revision": 4, "added": 1}
    assert seen[-1] == ("Tapioca.SetMetadataSchema", {"schema": json.dumps({"tags": ["parking"]}), "mode": "extend"})
    metadata.replace_schema(whole)
    assert "stored" not in json.loads(seen[-1][1]["schema"]), "what the read added is not stored"
    assert seen[-1][1]["mode"] == "replace"
