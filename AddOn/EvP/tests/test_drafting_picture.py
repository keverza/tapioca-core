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
        "error": "",
    }
