import os
import sys

import pytest

_PACKAGE = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage")
if _PACKAGE not in sys.path:
    sys.path.insert(0, _PACKAGE)

from evp import api, diligent, outputs  # noqa: E402


@pytest.fixture(autouse=True)
def tapioca_root(tmp_path, monkeypatch):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))


def camera(eye_x=1.0):
    return {
        "valid": True,
        "source": "viewer",
        "orthographic": False,
        "viewMoving": False,
        "eyeX": eye_x,
        "eyeY": 2.0,
        "eyeZ": 3.0,
        "targetX": 4.0,
        "targetY": 5.0,
        "targetZ": 6.0,
        "viewConeDegreesHorizontal": 60.0,
    }


def result(data=None, error=None):
    return api.Result({"ok": error is None, "data": data, "error": error})


def test_camera_preset_round_trips_every_field_and_replaces_explicitly():
    diligent.save_camera("Hero", camera())
    assert diligent.list_cameras() == ["Hero"]
    assert diligent.load_camera("Hero") == camera()

    with pytest.raises(FileExistsError):
        diligent.save_camera("Hero", camera(9.0))

    diligent.save_camera("Hero", camera(9.0), replace=True)
    assert diligent.load_camera("Hero")["eyeX"] == 9.0
    assert diligent.load_camera("hero")["eyeX"] == 9.0
    assert not any(name.endswith(".tmp-%d" % os.getpid())
                   for name in os.listdir(os.path.dirname(diligent.save_camera(
                       "Second", camera()))))


def test_camera_names_and_projection_are_validated_before_writing():
    with pytest.raises(ValueError):
        diligent.save_camera("../outside", camera())
    invalid = camera()
    invalid["orthographic"] = True
    with pytest.raises(ValueError):
        diligent.save_camera("Axono", invalid)
    invalid = camera()
    invalid.update({"targetX": 1.0, "targetY": 2.0, "targetZ": 3.0})
    with pytest.raises(ValueError):
        diligent.save_camera("Coincident", invalid)
    invalid = camera(1e300)
    with pytest.raises(ValueError):
        diligent.save_camera("Overflow", invalid)


def test_delete_camera_reports_whether_a_preset_existed():
    diligent.save_camera("DeleteMe", camera())
    assert diligent.delete_camera("deleteme") is True
    assert diligent.delete_camera("DeleteMe") is False


def test_diligent_capture_polls_then_fetches_and_writes_through_outputs(monkeypatch):
    calls = []
    states = iter([
        result({"id": 7, "status": "running", "stage": "extracting"}),
        result({"id": 7, "status": "completed", "stage": "completed",
                "url": "http://127.0.0.1:19191/screenshot/diligent"}),
    ])

    def fake_call(command, params, raise_on_error=False):
        calls.append((command, params))
        if command == "Tapioca.StartDiligentCapture":
            return result({"id": 7, "status": "running"})
        return next(states)

    class Response:
        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False

        def read(self):
            return b"\x89PNG\r\n\x1a\nbytes"

    monkeypatch.setattr(api, "call", fake_call)
    monkeypatch.setattr(outputs._request, "urlopen", lambda _url: Response())
    monkeypatch.setattr(outputs._time, "sleep", lambda _seconds: None)

    artifact, png = outputs.diligent_capture("hero", camera(), 800, 600)

    assert png.startswith(b"\x89PNG")
    assert open(artifact.path, "rb").read() == png
    assert [entry[0] for entry in calls] == [
        "Tapioca.StartDiligentCapture",
        "Tapioca.DiligentCaptureState",
        "Tapioca.DiligentCaptureState",
    ]
