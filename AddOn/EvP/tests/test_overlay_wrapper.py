import os
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
from evp import overlay


def _capture(monkeypatch):
    seen = []

    def fake_call(command, params):
        seen.append((command, params))
        return SimpleNamespace(data={"layer": params.get("layer", ""), "cleared": 1})

    monkeypatch.setattr(overlay, "call", fake_call)
    return seen


def test_colours_read_every_way_a_script_writes_them():
    assert overlay.colour("#fec44f") == "FEC44FFF"
    assert overlay.colour("FEC44F80") == "FEC44F80"
    assert overlay.colour((1, 0, 0)) == "FF0000FF"
    assert overlay.colour((1.0, 0.5, 0.0, 0.5)) == "FF8000" + "80"
    assert overlay.colour((255, 128, 0)) == "FF8000FF"
    with pytest.raises(ValueError):
        overlay.colour("#12345")


def test_a_panel_is_its_items_in_order():
    panel = (
        overlay.Panel(title="Sun study", anchor="top-right", width=260)
        .row("Date", "21 June")
        .separator()
        .ramp("sunhours", 0, 8, unit="h", title="Sun hours", ticks=5)
        .swatch("#FEC44F", "over 6 h")
        .progress(0.42, "42 %")
        .plot([1, 3, 2], caption="hours")
        .table(["Block", "Area"], [["A", 812]])
    )
    out = panel.to_dict()
    assert out["anchor"] == "top-right"
    assert out["widthPixels"] == 260.0
    assert [item["kind"] for item in out["items"]] == [
        "row",
        "separator",
        "ramp",
        "swatch",
        "progress",
        "plot",
        "table",
    ]
    ramp = out["items"][2]
    assert ramp["colormap"] == {"preset": "sunhours", "min": 0.0, "max": 8.0}
    assert ramp["unit"] == "h" and ramp["ticks"] == 5 and ramp["text"] == "Sun hours"
    assert out["items"][3]["color"] == "FEC44FFF"
    assert out["items"][6]["rows"] == [["A", "812"]]
    with pytest.raises(ValueError):
        overlay.Panel(anchor="middle")


def test_a_panel_folds_and_its_sections_fold_what_follows():
    panel = (
        overlay.Panel(title="Area metrics", collapsed=True)
        .section("GFA", "19 821 m2")
        .row("Healthcare", "3 630 m2")
        .section("GIA", 17839, open=False)
        .row("Healthcare", "3 267 m2")
    )
    out = panel.to_dict()
    assert out["collapsed"] is True
    assert out["items"][0] == {"kind": "section", "text": "GFA", "value": "19 821 m2", "open": True}
    assert out["items"][2] == {"kind": "section", "text": "GIA", "value": "17839", "open": False}
    assert "collapsed" not in overlay.Panel(title="Plain").to_dict()


def test_the_design_card_is_the_wire_names():
    panel = (
        overlay.Panel(title="AREA METRICS", theme="light")
        .metrics([("Site area", "11 214 m2"), ("FAR", 1.77)])
        .stack([("Built", 20, "#8FD3F7"), ("Open", 80, "#19D9A0")], decimals=0)
        .section("Spacing", info="Shares by use.")
        .swatch("#2A7F86", "Healthcare", "18.3%", per_row=2)
        .bars([12, 4, 2.5], labels=[1, 2, 3], colormap="sunhours", caption="hours", unit="%")
    )
    out = panel.to_dict()
    assert out["theme"] == "light"
    metrics, stack, section, swatch, bars = out["items"]
    assert metrics == {"kind": "metrics", "rows": [["Site area", "11 214 m2"], ["FAR", "1.77"]]}
    assert stack["values"] == [20.0, 80.0] and stack["labels"] == ["Built", "Open"]
    assert stack["colors"] == ["8FD3F7FF", "19D9A0FF"] and stack["decimals"] == 0
    assert section["info"] == "Shares by use."
    assert swatch == {"kind": "swatch", "color": "2A7F86FF", "text": "Healthcare", "value": "18.3%", "perRow": 2}
    assert bars["colormap"] == {"preset": "sunhours"} and bars["labels"] == ["1", "2", "3"]
    assert bars["text"] == "hours" and bars["unit"] == "%"
    with pytest.raises(ValueError):
        overlay.Panel(theme="blue")
    with pytest.raises(ValueError):
        overlay.Panel().stack([("a", 1, "#FF0000"), ("b", 2)])


def test_set_layer_sends_reals_except_where_the_wire_wants_integers(monkeypatch):
    seen = _capture(monkeypatch)
    overlay.set_layer(
        "sun",
        texts=[overlay.text("Origin", at=(0, 0, 0), offset=(6, -6))],
        meshes=[overlay.heatmap([0, 0, 0, 1, 0, 0, 1, 1, 0], [0, 1, 2], [2, 8, 5], "sunhours", bands=8)],
        legends=[overlay.legend(None, mesh=0, title="Sun hours", decimals=1, horizontal=True, screen=(0.5, 1))],
        panels=[overlay.Panel(title="HUD").row("GFA", "2633 m2")],
    )
    command, params = seen[0]
    assert command == "Tapioca.SetOverlayLayer"
    assert params["layer"] == "sun" and params["views"] == "both"
    assert "occlusion" not in params  # None is left out, not sent
    label = params["texts"][0]
    # A single coordinate is a record on the wire (SPEC.md), every component a real.
    assert label["at"] == {"x": 0.0, "y": 0.0, "z": 0.0} and all(isinstance(v, float) for v in label["at"].values())
    assert label["offsetPixels"] == {"x": 6.0, "y": -6.0}
    mesh = params["meshes"][0]
    assert mesh["indices"] == [0, 1, 2] and all(isinstance(i, int) for i in mesh["indices"])
    assert all(isinstance(v, float) for v in mesh["values"])
    assert mesh["colormap"]["bands"] == 8 and isinstance(mesh["colormap"]["bands"], int)
    legend = params["legends"][0]
    assert legend["mesh"] == 0 and isinstance(legend["mesh"], int)
    assert legend["screen"] == {"x": 0.5, "y": 1.0} and legend["horizontal"] is True
    assert legend["decimals"] == 1 and isinstance(legend["decimals"], int)
    assert params["panels"][0]["items"][0] == {"kind": "row", "text": "GFA", "value": "2633 m2"}


def test_a_text_on_a_plane_carries_its_plane():
    label = overlay.text("Hall", at=(10, 20, 3), plane=((0, 1, 0), (0, 0, 1), 0.5), align="left")
    assert label["plane"] == {
        "direction": {"x": 0.0, "y": 1.0, "z": 0.0},
        "normal": {"x": 0.0, "y": 0.0, "z": 1.0},
        "sizeMetres": 0.5,
    }
    assert label["at"] == overlay.point3({"x": 10, "y": 20, "z": 3})
    with pytest.raises(ValueError):
        overlay.text("both", at=(0, 0, 0), screen=(0, 0))
    with pytest.raises(ValueError):
        overlay.text("short", at=(0, 0))


def test_occlusion_speaks_the_text_labels_vocabulary(monkeypatch):
    seen = _capture(monkeypatch)
    label = overlay.text("Behind a wall", at=(0, 0, 0), occlusion="fade")
    mesh = overlay.heatmap([0, 0, 0, 1, 0, 0, 1, 1, 0], [0, 1, 2], [1, 2, 3], occlusion="always")
    overlay.set_layer("walls", occlusion="hide", texts=[label], meshes=[mesh])
    params = seen[0][1]
    assert params["occlusion"] == "hide"
    assert params["texts"][0]["occlusion"] == "fade"
    assert params["meshes"][0]["style"] == {"occlusion": "always"}
    with pytest.raises(ValueError):
        overlay.text("x", at=(0, 0, 0), occlusion="show")


def test_a_halo_grows_with_its_text_unless_fixed():
    assert "haloPixels" not in overlay.text("auto", at=(0, 0, 0))
    assert overlay.text("smaller", at=(0, 0, 0), halo_scale=0.5)["haloScale"] == 0.5
    assert overlay.text("fixed", at=(0, 0, 0), halo_size=2)["haloPixels"] == 2.0


def test_clear_and_hud(monkeypatch):
    seen = _capture(monkeypatch)
    assert overlay.clear("sun") == 1
    overlay.hud("status", overlay.Panel().text("ready"), views="3d")
    assert seen[0] == ("Tapioca.ClearOverlayLayer", {"layer": "sun"})
    assert seen[1][1]["views"] == "3d"
    assert seen[1][1]["panels"][0]["items"] == [{"kind": "text", "text": "ready"}]


def test_every_item_kind_carries_its_style():
    line = overlay.polyline(
        [(0, 0, 0), (1, 0, 0), (1, 1, 0)],
        color="#FF0000",
        width=3,
        dash=[1.0, 0.25, 0.1, 0.25],
        hidden_color="#D03030",
        hidden_width=1.5,
        hidden_dash=[0.6, 0.3],
        start_arrow="dot",
        end_arrow="arrow",
        arrow_size=12,
        occlusion="dash",
    )
    assert line["points"] == [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0]
    assert line["color"] == "FF0000FF" and line["startArrow"] == "dot" and line["arrowSizePixels"] == 12.0
    # Dashes are model metres: on, off, on, off...; the hidden part has its own style.
    assert line["dashMetres"] == [1.0, 0.25, 0.1, 0.25]
    assert line["hidden"] == {"color": "D03030FF", "widthPixels": 1.5, "dashMetres": [0.6, 0.3]}
    ghost = overlay.mesh(
        [0, 0, 0, 1, 0, 0, 0, 1, 0],
        [0, 1, 2],
        color=(0.2, 0.6, 1.0, 0.5),
        shading="ghost",
        edges="#103060",
        edge_width=1.5,
    )
    assert ghost["style"] == {"shading": "ghost", "edges": {"color": "103060FF", "widthPixels": 1.5}}
    measure = overlay.dimension(
        (0, 0, 0),
        (5, 0, 0),
        offset=0.6,
        direction=(0, -1, 0),
        text_color="#202020",
        halo="#FFFFFFC0",
        terminator="arrow",
        terminator_size=8,
        show_unit=True,
    )
    assert measure["from"] == {"x": 0.0, "y": 0.0, "z": 0.0} and measure["direction"]["y"] == -1.0
    assert measure["textColor"] == "202020FF" and measure["terminatorSizePixels"] == 8.0
    marks = overlay.points([(1, 2, 3)], color="#00FF00", size=6)
    assert marks == {"points": [1.0, 2.0, 3.0], "color": "00FF00FF", "sizePixels": 6.0}
    with pytest.raises(ValueError):
        overlay.polyline([0, 0, 0, 1, 1, 1], end_arrow="hook")


def test_the_add_ons_own_layers_take_their_style(monkeypatch):
    seen = _capture(monkeypatch)
    overlay.story_slices(
        source="elements",
        elements=["{ABC}"],
        cut="storeys",
        storeys=[1, 2],
        outline={"color": "#303030", "dashMetres": [0.5, 0.3], "hidden": {"color": "#C02020", "widthPixels": 2}},
        label={"halo": (1, 1, 1, 0.8)},
    )
    overlay.annotations(style={"lineWidthPixels": 3, "halo": "#000000", "colors": {"add": "#00C000"}})
    command, params = seen[0]
    assert command == "Tapioca.OverlayStorySlices"
    assert params["elements"] == [{"elementId": {"guid": "{ABC}"}}]
    assert params["storeys"] == [1, 2] and all(isinstance(s, int) for s in params["storeys"])
    assert params["outline"] == {
        "color": "303030FF",
        "dashMetres": [0.5, 0.3],
        "hidden": {"color": "C02020FF", "widthPixels": 2.0},
    }
    assert params["label"]["halo"] == "FFFFFFCC"
    overlay.story_slices(fill={"colors": ["#FF0000", (0, 1, 0)], "opacity": 0.4, "colormap": "viridis"})
    fill = seen[-1][1]["fill"]
    assert fill["colors"] == ["FF0000FF", "00FF00FF"] and fill["opacity"] == 0.4
    assert fill["colormap"] == {"preset": "viridis"}
    command, params = seen[1]
    assert command == "Tapioca.OverlayAnnotations" and params["action"] == "on"
    assert params["style"] == {"lineWidthPixels": 3.0, "halo": "000000FF", "colors": {"add": "00C000FF"}}


def test_a_font_is_named_on_the_layer_or_on_an_item(monkeypatch):
    seen = _capture(monkeypatch)
    overlay.set_layer(
        "fonts",
        font="Segoe UI",
        texts=[overlay.text("Room", at=(0, 0, 0), font="Consolas")],
        dimensions=[overlay.dimension((0, 0, 0), (1, 0, 0), font="Arial")],
        legends=[overlay.legend("viridis", 0, 1, font="Arial")],
        panels=[overlay.Panel(title="HUD", font="C:/Fonts/own.ttf")],
    )
    params = seen[0][1]
    assert params["font"] == "Segoe UI"
    assert params["texts"][0]["font"] == "Consolas"
    assert params["dimensions"][0]["font"] == "Arial" and params["legends"][0]["font"] == "Arial"
    assert params["panels"][0]["font"] == "C:/Fonts/own.ttf"


def test_the_hud_events_are_asked_for_after_the_last_seen(monkeypatch):
    seen = []

    def fake_call(command, params):
        seen.append((command, params))
        return SimpleNamespace(data={"lastSeq": 7, "gap": False, "events": [{"seq": 7, "kind": "dock"}]})

    monkeypatch.setattr(overlay, "call", fake_call)
    got = overlay.events(since=5)
    assert seen[-1] == ("Tapioca.OverlayHudEvents", {"sinceSeq": 5, "maxEvents": 256})
    assert got["lastSeq"] == 7 and got["events"][0]["kind"] == "dock"
    # Never a negative number, never more than the ring holds.
    overlay.events(since=-3, max_events=10000)
    assert seen[-1][1] == {"sinceSeq": 0, "maxEvents": 512}
    # Waiting returns at the first event.
    assert overlay.wait_events(5, timeout=0.5)["events"]


def test_waiting_for_hud_events_gives_up_at_its_timeout(monkeypatch):
    monkeypatch.setattr(
        overlay, "call", lambda command, params: SimpleNamespace(data={"lastSeq": 3, "gap": False, "events": []})
    )
    got = overlay.wait_events(3, timeout=0.05, interval=0.01)
    assert got["events"] == [] and got["lastSeq"] == 3


def test_the_text_size_is_a_real_on_the_wire(monkeypatch):
    seen = []
    monkeypatch.setattr(
        overlay, "call", lambda command, params: seen.append((command, params)) or SimpleNamespace(data={})
    )
    overlay.set_text_size(1)
    overlay.hud_state()
    assert seen == [("Tapioca.OverlayHud", {"fontScale": 1.0}), ("Tapioca.OverlayHud", {})]
    assert isinstance(seen[0][1]["fontScale"], float)


def test_the_controls_are_the_wire_names():
    panel = (
        overlay.Panel(title="Study")
        .tab("Area", id="view", selected=1)
        .checkbox("grid", "Show grid", checked=True)
        .tab("Sun")
        .slider("hour", "Hour", 12, 6, 20, step=0.5, decimals=1, unit="h")
        .combo("date", "Date", ["21 March", "21 June", "21 December"], selected=1)
        .button("export", "Export")
    )
    items = panel.to_dict()["items"]
    assert items[0] == {"kind": "tab", "text": "Area", "id": "view", "selected": 1}
    assert items[1] == {"kind": "checkbox", "id": "grid", "text": "Show grid", "checked": True}
    assert items[2] == {"kind": "tab", "text": "Sun"}
    assert items[3] == {
        "kind": "slider",
        "id": "hour",
        "text": "Hour",
        "number": 12.0,
        "min": 6.0,
        "max": 20.0,
        "step": 0.5,
        "decimals": 1,
        "unit": "h",
    }
    assert items[4]["labels"] == ["21 March", "21 June", "21 December"] and items[4]["selected"] == 1
    assert items[5] == {"kind": "button", "id": "export", "text": "Export"}
    # On the wire the selection is an integer, the slider's numbers reals.
    wire = overlay._reals({"panels": [panel.to_dict()]})
    assert isinstance(wire["panels"][0]["items"][4]["selected"], int)
    assert isinstance(wire["panels"][0]["items"][3]["number"], float)
    with pytest.raises(ValueError):
        overlay.Panel().slider("x", "X", 1, 5, 5)
    with pytest.raises(ValueError):
        overlay.Panel().combo("x", "X", [])


def test_a_grids_cells_to_a_row_are_an_integer_on_the_wire():
    # The schema says integer and the validator refuses a real: sent as 2.0, a panel with a
    # grid or a key of two to a row failed its whole layer.
    panel = overlay.Panel().metrics([("Site area", "11 214 m2"), ("BCR", "20 %")], per_row=2).swatch(
        "#2A7F86", "Healthcare", value="18.3 %", per_row=2
    )
    items = overlay._reals({"panels": [panel.to_dict()]})["panels"][0]["items"]
    assert items[0]["perRow"] == 2 and isinstance(items[0]["perRow"], int)
    assert isinstance(items[1]["perRow"], int)
