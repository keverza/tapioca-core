from types import SimpleNamespace
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
from evp import elements


def test_ids_maps_typed_records_to_existing_wrapper_shape(monkeypatch):
    seen = []

    def fake_call(command, params):
        seen.append((command, params))
        return SimpleNamespace(data={"identities": [{
            "elementId": {"guid": "guid-a"}, "found": True, "value": "W-01",
            "typeName": "Wall", "typeId": 1,
        }], "count": 1})

    monkeypatch.setattr(elements, "call", fake_call)

    assert elements.ids(["guid-a"]) == [{
        "guid": "guid-a", "found": True, "element_id": "W-01",
        "type_name": "Wall", "type_id": 1, "reason": "",
    }]
    assert seen == [("Tapioca.GetElementIds", {
        "elements": [{"elementId": {"guid": "guid-a"}}],
    })]


def test_set_ids_maps_per_item_succeeded_to_ok(monkeypatch):
    seen = []

    def fake_call(command, params):
        seen.append((command, params))
        return SimpleNamespace(data={"results": [{
            "elementId": {"guid": "guid-a"}, "succeeded": True,
        }], "count": 1, "changed": 1})

    monkeypatch.setattr(elements, "call", fake_call)

    assert elements.set_ids({"guid-a": "W-02"}) == [
        {"guid": "guid-a", "ok": True, "error": ""}
    ]
    assert seen == [("Tapioca.SetElementIds", {"identities": [{
        "elementId": {"guid": "guid-a"}, "value": "W-02",
    }]})]
