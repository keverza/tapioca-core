from pathlib import Path


_RUNTIME = (
    Path(__file__).resolve().parents[1]
    / "Sources"
    / "AddOn"
    / "ArchViz"
    / "InjectedOverlayRuntime.cpp"
)


def test_stop_unconditionally_shuts_down_camera_sync():
    source = _RUNTIME.read_text(encoding="utf-8")
    stop = source.split("void Stop ()", maxsplit=1)[1]

    assert "ShutDownCameraSync ();" in stop
    assert "SetCameraSyncMode (CameraSyncMode::Legacy" not in stop
