import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
from evp import cameras


def test_camera_set_get_returns_capture_ready_rows(monkeypatch):
    camera = {
        "valid": True,
        "source": "perspective",
        "orthographic": False,
        "viewMoving": False,
        "eyeX": 1.0,
        "eyeY": 2.0,
        "eyeZ": 3.0,
        "targetX": 4.0,
        "targetY": 5.0,
        "targetZ": 6.0,
        "viewConeDegreesHorizontal": 60.0,
        "sun": {"enabled": True, "azimuthDegrees": 130.0, "altitudeDegrees": 35.0},
    }
    seen = []

    def fake_call(command, params):
        seen.append((command, params))
        return SimpleNamespace(data={"cameras": [camera], "count": 1})

    monkeypatch.setattr(cameras, "call", fake_call)

    assert cameras.sets.get("Views") == [camera]
    assert seen == [("Tapioca.GetCameraSet", {"name": "Views"})]
