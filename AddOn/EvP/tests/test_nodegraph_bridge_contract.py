"""Source-level contracts for the self-contained node-graph editor host."""

import re
from pathlib import Path


_SOURCES = Path(__file__).resolve().parents[1] / "Sources" / "AddOn"


def test_the_editor_page_is_served_not_pushed_through_navigatetostring():
    host = (_SOURCES / "Palette" / "WebView2GraphHost.cpp").read_text(encoding="utf-8")
    assert not re.search(r"->\s*NavigateToString\s*\(", host)
    assert "add_WebResourceRequested" in host


def test_palette_items_exist_before_dg_event_processing_starts():
    palette = (_SOURCES / "Palette" / "GraphEditorPalette.cpp").read_text(encoding="utf-8")
    body = palette.split("GraphEditorPalette::GraphEditorPalette ()")[1].split(chr(10) + "}")[0]
    begin = body.index("BeginEventProcessing ()")
    for member in ("surface = std::make_unique", "webView = std::make_unique"):
        assert member in body
        assert body.index(member) < begin

    resized = palette.split("GraphEditorPalette::PanelResized")[1].split(chr(10) + "}")[0]
    assert "surface == nullptr" in resized
