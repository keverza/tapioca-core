from pathlib import Path


_ARCHVIZ = Path(__file__).resolve().parents[1] / "Sources" / "AddOn" / "ArchViz"


def test_first_census_draw_bootstraps_current_constant_buffer_windows():
    census = (_ARCHVIZ / "Dxgi" / "CameraCensus.cpp").read_text(encoding="utf-8")
    on_draw = census.split("void OnDraw (", maxsplit=1)[1]

    bootstrap = on_draw.index("contextstate::BootstrapVSConstantBuffers (context);")
    snapshot = on_draw.index("contextstate::Snapshot ();")
    assert bootstrap < snapshot


def test_binding_bootstrap_reads_d3d11_windows_and_releases_references():
    tracker = (_ARCHVIZ / "Dxgi" / "ContextStateTracker.cpp").read_text(
        encoding="utf-8"
    )
    bootstrap = tracker.split("void BootstrapVSConstantBuffers (", maxsplit=1)[1]
    bootstrap = bootstrap.split("void OnRenderTargets (", maxsplit=1)[0]

    assert "VSGetConstantBuffers1" in bootstrap
    assert "OnVSConstantBuffers" in bootstrap
    assert "buffer->Release ();" in bootstrap
    assert "context1->Release ();" in bootstrap
