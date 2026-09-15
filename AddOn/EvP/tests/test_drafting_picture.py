import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
from evp import drafting


def test_place_picture_normalizes_native_element_id(monkeypatch):
    response = {
        "elementId": {"guid": "11111111-2222-3333-4444-555555555555"},
        "pixelWidth": 1920,
        "pixelHeight": 1080,
        "placedWidth": 4.0,
        "placedHeight": 2.25,
        "databaseId": {"guid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"},
        "layer": "Reports",
        "verified": True,
    }
    monkeypatch.setattr(drafting, "call", lambda *args, **kwargs: SimpleNamespace(data=response))

    placed = drafting.place_picture("frame.png", 1.0, 2.0, width=4.0, height=2.25)

    assert placed == {
        "ok": True,
        "guid": "11111111-2222-3333-4444-555555555555",
        "pixel_width": 1920,
        "pixel_height": 1080,
        "placed_width": 4.0,
        "placed_height": 2.25,
        "database_guid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        "layer": "Reports",
        "verified": True,
        "error": "",
    }


def test_place_picture_sends_database_anchor():
    sent = {}

    class Tx:
        def call(self, command, params):
            sent.update({"command": command, "params": params})
            return "handle"

    result = drafting.place_picture(
        "frame.png",
        1.0,
        2.0,
        layer="Reports",
        database_anchor="11111111-2222-3333-4444-555555555555",
        tx=Tx(),
    )

    assert result == "handle"
    assert sent["command"] == "EvP.PlacePicture"
    assert sent["params"]["layer"] == "Reports"
    assert sent["params"]["databaseAnchorElementId"] == {
        "guid": "11111111-2222-3333-4444-555555555555"
    }


def test_create_text_normalizes_native_result_and_sends_strict_anchor(monkeypatch):
    response = {
        "count": 1,
        "results": [{
            "succeeded": True,
            "elementId": {"guid": "11111111-2222-3333-4444-555555555555"},
            "databaseId": {"guid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"},
            "layer": "Reports",
            "verified": True,
        }],
    }
    captured = {}

    def fake_call(command, params):
        captured.update({"command": command, "params": params})
        return SimpleNamespace(data=response)

    monkeypatch.setattr(drafting, "call", fake_call)
    created = drafting.create_text(
        {"text": "Metrics", "x": 1.0, "y": 2.0},
        layer="Reports",
        database_anchor="99999999-2222-3333-4444-555555555555",
        fail_on_error=True,
    )

    assert created == [{
        "ok": True,
        "guid": "11111111-2222-3333-4444-555555555555",
        "database_guid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        "layer": "Reports",
        "verified": True,
        "error": "",
    }]
    assert captured["params"]["databaseAnchorElementId"] == {
        "guid": "99999999-2222-3333-4444-555555555555"
    }
    assert captured["params"]["failOnError"] is True


def test_create_polyline_sends_closed_points_layer_and_anchor():
    sent = {}

    class Tx:
        def call(self, command, params):
            sent.update({"command": command, "params": params})
            return "polyline-handle"

    result = drafting.create_polyline(
        [(0, 0), (2, 0), (2, 1), (0, 1), (0, 0)],
        layer="Reports",
        database_anchor="11111111-2222-3333-4444-555555555555",
        tx=Tx(),
    )

    assert result == "polyline-handle"
    assert sent["command"] == "Tapioca.CreateDraftingPolyline"
    assert sent["params"]["coordinates"][0] == {"x": 0.0, "y": 0.0}
    assert sent["params"]["coordinates"][-1] == {"x": 0.0, "y": 0.0}
    assert sent["params"]["layer"] == "Reports"
    assert sent["params"]["databaseAnchorElementId"] == {
        "guid": "11111111-2222-3333-4444-555555555555"
    }


def test_polyline_rectangles_normalizes_database_and_bounds(monkeypatch):
    response = {
        "databaseId": {"guid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"},
        "count": 1,
        "polylines": [{
            "elementId": {"guid": "11111111-2222-3333-4444-555555555555"},
            "x": 1.0,
            "y": 2.0,
            "width": 3.0,
            "height": 4.0,
            "layer": "Reports",
        }],
    }
    monkeypatch.setattr(drafting, "call", lambda *args, **kwargs: SimpleNamespace(data=response))

    assert drafting.polyline_rectangles("anchor") == [{
        "guid": "11111111-2222-3333-4444-555555555555",
        "rect": (1.0, 2.0, 3.0, 4.0),
        "layer": "Reports",
        "database_guid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
    }]
