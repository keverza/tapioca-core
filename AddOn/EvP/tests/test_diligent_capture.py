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
    assert calls[0][1]["dpi"] == 96.0


def test_text_label_helpers_are_thin_retained_api_calls(monkeypatch):
    calls = []

    def fake_call(command, params, raise_on_error=False):
        calls.append((command, params, raise_on_error))
        return result({"count": len(params.get("labels", []))} if params else {"cleared": True})

    monkeypatch.setattr(api, "call", fake_call)
    label = {"x": 1.0, "y": 2.0, "z": 3.0, "text": "Plotas 42 m\u00b2"}
    assert diligent.set_text_labels(label for label in [label]) == {"count": 1}
    assert diligent.clear_text_labels() == {"cleared": True}
    assert calls == [
        ("Tapioca.SetDiligentTextLabels", {"labels": [label]}, False),
        ("Tapioca.ClearDiligentTextLabels", {}, False),
    ]


def test_capture_batch_sends_every_camera_once_and_returns_the_written_paths(monkeypatch, tmp_path):
    """The batch is ONE start for N cameras - that is the whole reason it exists.

    A loop over diligent_capture would re-extract the model per camera; this
    asserts the wrapper issues a single StartDiligentCaptureBatch carrying all of
    them, and hands back the paths the renderer reported rather than inventing
    them from the directory.
    """
    calls = []
    states = iter([
        result({"id": 4, "status": "running", "stage": "extracting",
                "frameCount": 2, "framesDone": 0, "paths": []}),
        result({"id": 4, "status": "running", "stage": "rendering 2 of 2",
                "frameCount": 2, "framesDone": 1, "paths": ["a"]}),
        result({"id": 4, "status": "completed", "stage": "completed",
                "frameCount": 2, "framesDone": 2,
                "paths": [str(tmp_path / "00.png"), str(tmp_path / "01.png")]}),
    ])

    def fake_call(command, params=None, **kwargs):
        calls.append((command, params))
        if command == "Tapioca.StartDiligentCaptureBatch":
            return result({"id": 4, "status": "running", "frameCount": 2})
        if command == "Tapioca.DiligentCaptureState":
            return next(states)
        raise AssertionError("unexpected command %s" % command)

    monkeypatch.setattr(api, "call", fake_call)
    monkeypatch.setattr(outputs._time, "sleep", lambda seconds: None)

    lit = camera(1.0)
    lit["sun"] = {"enabled": True, "azimuthDegrees": 135.0, "altitudeDegrees": 40.0}
    written = outputs.diligent_capture_batch([lit, camera(2.0)], 800, 600,
                                             directory=str(tmp_path))

    assert written == [str(tmp_path / "00.png"), str(tmp_path / "01.png")]

    starts = [params for command, params in calls
              if command == "Tapioca.StartDiligentCaptureBatch"]
    assert len(starts) == 1, "a batch must be ONE start, not one per camera"
    assert len(starts[0]["cameras"]) == 2
    # The sun rides with its own camera. Without this the renderer lights every
    # frame the same way and the result looks entirely plausible.
    assert starts[0]["cameras"][0]["sun"]["azimuthDegrees"] == 135.0
    assert "sun" not in starts[0]["cameras"][1]
    assert starts[0]["outputDirectory"] == str(tmp_path)
    assert starts[0]["dpi"] == 96.0


def test_capture_forwards_explicit_target_dpi(monkeypatch):
    calls = []

    def fake_call(command, params, raise_on_error=False):
        calls.append((command, params))
        if command == "Tapioca.StartDiligentCapture":
            return result({"id": 9, "status": "running"})
        return result({"id": 9, "status": "cancelled", "stage": "cancelled",
                       "failureMessage": "test"})

    monkeypatch.setattr(api, "call", fake_call)
    with pytest.raises(outputs.OutputError):
        outputs.diligent_capture("dpi", camera(), 800, 600, dpi=300.0, save=False)
    assert calls[0][1]["dpi"] == 300.0


def test_capture_batch_reports_how_far_it_got_when_it_times_out(monkeypatch, tmp_path):
    """A batch runs for minutes; "timed out" alone does not say whether it moved."""
    monkeypatch.setattr(outputs._time, "sleep", lambda seconds: None)

    def fake_call(command, params=None, **kwargs):
        if command == "Tapioca.StartDiligentCaptureBatch":
            return result({"id": 5, "status": "running", "frameCount": 8})
        if command == "Tapioca.DiligentCaptureState":
            return result({"id": 5, "status": "running", "stage": "rendering 3 of 8",
                           "frameCount": 8, "framesDone": 2, "paths": []})
        if command == "Tapioca.CancelDiligentCapture":
            return result({"cancelled": True})
        raise AssertionError("unexpected command %s" % command)

    monkeypatch.setattr(api, "call", fake_call)
    with pytest.raises(outputs.OutputError) as failure:
        outputs.diligent_capture_batch([camera(1.0)], 800, 600,
                                       directory=str(tmp_path), timeout=0.0)
    assert "2 of 8" in str(failure.value)


def test_capture_batch_refuses_a_failed_run_with_the_stage_it_died_in(monkeypatch, tmp_path):
    monkeypatch.setattr(outputs._time, "sleep", lambda seconds: None)

    def fake_call(command, params=None, **kwargs):
        if command == "Tapioca.StartDiligentCaptureBatch":
            return result({"id": 6, "status": "running", "frameCount": 3})
        if command == "Tapioca.DiligentCaptureState":
            return result({"id": 6, "status": "failed", "stage": "rendering 2 of 3",
                           "frameCount": 3, "framesDone": 1, "paths": [],
                           "failureMessage": "the device was removed"})
        raise AssertionError("unexpected command %s" % command)

    monkeypatch.setattr(api, "call", fake_call)
    with pytest.raises(outputs.OutputError) as failure:
        outputs.diligent_capture_batch([camera(1.0)], 800, 600, directory=str(tmp_path))
    # Both halves: WHERE it died and WHY. One without the other sends the reader
    # to the wrong half of a long pipeline.
    assert "rendering 2 of 3" in str(failure.value)
    assert "device was removed" in str(failure.value)

