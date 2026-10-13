"""The independently packed plan-hover stream must keep its own model-space origin."""

from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "Sources/AddOn/ArchViz/Dxgi/PlanGuest.cpp"


def test_hover_upload_includes_cell_triangles_and_retains_their_packing_origin():
    source = SOURCE.read_text(encoding="utf-8")
    upload = source[source.index("bool PrepareHud (") : source.index("bool NeedsGuest2D (")]
    assert "arrays.fills = panels.fills.data ()" in upload
    assert "arrays.fillCount = panels.fills.size ()" in upload
    assert "s_hudOriginX = panels.originX" in upload
    assert "s_hudOriginY = panels.originY" in upload


def test_hover_draw_rejoins_coordinates_in_its_own_origin_at_the_current_present():
    source = SOURCE.read_text(encoding="utf-8")
    draw = source[source.index("void Draw (") : source.index("void Release (")]
    origin = draw.index("MakeViewConstants (transform, s_hudOriginX, s_hudOriginY")
    frame = draw.index("std::memcpy (frame.view, view.view", origin)
    submit = draw.index("g_hudPages, g_hudContent", frame)
    assert origin < frame < submit
    assert "s_hudOriginX = s_hudOriginY = 0" in source[source.index("void Release (") :]
