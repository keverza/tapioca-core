"""Offline resource/registration contract for the native Tapioca menu (#172)."""

import re
from pathlib import Path

SOURCES = Path(__file__).resolve().parents[1] / "Sources"
ADDON = SOURCES / "AddOn"
EXPECTED = [
    ("GeometryServerMenuResId", "Tapioca Panel", "MenuFlag_Default"),
    ("GraphEditorMenuResId", "Node Graph", "MenuFlag_Default"),
    ("ArchVizMenuResId", "Vision Window", "MenuFlag_SeparatorBefore"),
    ("Overlay2DMenuResId", "2D Vision Overlay", "MenuFlag_Default"),
    ("ArchVizOverlayMenuResId", "3D Vision Overlay", "MenuFlag_Default"),
    ("GrasshopperMenuResId", "Close Grasshopper", "MenuFlag_SeparatorBefore"),
    ("GrasshopperEditorMenuResId", "Grasshopper Editor", "MenuFlag_Default"),
    ("RhinoComputeStartMenuResId", "Start rhino.compute", "MenuFlag_Default"),
    ("RhinoComputeStopMenuResId", "Stop rhino.compute", "MenuFlag_Default"),
    ("GrasshopperRunMenuResId", "Run Grasshopper Definition", "MenuFlag_Default"),
    ("AboutMenuResId", "About Tapioca...", "MenuFlag_SeparatorBefore"),
]


def test_registration_order_and_group_separators():
    main = (ADDON / "AddOnMain.cpp").read_text(encoding="utf-8")
    registrations = re.findall(
        r"ACAPI_MenuItem_RegisterMenu\s*\(\s*(\w+),\s*0,\s*MenuCode_UserDef,\s*(\w+)\s*\)",
        main,
    )
    assert registrations == [(resource, flags) for resource, _, flags in EXPECTED]
    handlers = re.findall(
        r"ACAPI_MenuItem_InstallMenuHandler\s*\(\s*(\w+),\s*MenuCommandHandler\s*\)",
        main,
    )
    assert handlers == [resource for resource, _, _ in EXPECTED]


def test_each_menu_resource_has_one_item_and_keeps_view_enable_codes():
    ids = dict(
        re.findall(
            r"constexpr short (\w+) = (\d+);",
            (ADDON / "ResourceIds.hpp").read_text(encoding="utf-8"),
        )
    )
    grc = (SOURCES / "AddOnResources/RINT/AddOn.grc").read_text(encoding="utf-8")
    grc = re.sub(r"/\*.*?\*/", "", grc, flags=re.DOTALL)
    resources = dict(re.findall(r"'STR#'\s+(\d+)\s+\"[^\"]*\"\s*\{(.*?)\}", grc, re.DOTALL))
    for resource, label, _ in EXPECTED:
        assert re.findall(r'"([^\"]*)"', resources[ids[resource]]) == ["Tapioca", label + "^E3^ES^EE^EI^ED^EW^EL"]
    assert re.search(r"'GDLG'\s+32540[^\n]*\"Vision Window\"", grc)


def _body(source, signature):
    """Extract a function using balanced braces (including nested branches)."""
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_viewer_toggle_and_check_use_the_same_logical_open_state():
    main = (ADDON / "AddOnMain.cpp").read_text(encoding="utf-8")
    route = main.split("case ArchVizMenuResId:", 1)[1].split("break;", 1)[0]
    assert "ArchVizPanel::ToggleViewer ();" in route
    panel = (ADDON / "ArchViz/ArchVizPanel.cpp").read_text(encoding="utf-8")
    toggle = _body(panel, "void ArchVizPanel::ToggleViewer")
    assert "if (IsViewerOpen ())" in toggle
    assert "CloseViewer ();" in toggle and "OpenViewer ();" in toggle
    assert "paletteState.IsOpen ()" in _body(panel, "bool ArchVizPanel::IsViewerOpen")
    sync = _body(panel, "void ArchVizPanel::SyncMenuCheck")
    assert "paletteState.IsOpen ()" in sync
    assert "ACAPI_MenuItem_GetMenuItemFlags (&item, &flags) != NoError" in sync
    assert "flags | API_MenuItemChecked" in sync
    assert "flags & ~GSFlags (API_MenuItemChecked)" in sync
    assert "ACAPI_MenuItem_SetMenuItemFlags (&item, &wanted)" in sync


def test_viewer_lifecycle_syncs_checks_but_temporary_host_hides_do_not():
    panel = (ADDON / "ArchViz/ArchVizPanel.cpp").read_text(encoding="utf-8")
    for method, intent in (("Show", "RequestOpen"), ("Hide", "RequestClose")):
        body = _body(panel, f"void ArchVizPanel::{method} ()")
        assert body.index(f"paletteState.{intent} ()") < body.index("SyncMenuCheck ()")
    assert "Hide ();" in _body(panel, "void ArchVizPanel::CloseViewer")
    assert "Hide ();" in _body(panel, "void ArchVizPanel::PanelCloseRequested")
    callback = _body(panel, "GSErrCode ArchVizPanel::PaletteControlCallBack")
    close = callback.split("case APIPalMsg_ClosePalette:", 1)[1].split("break;", 1)[0]
    assert "CloseViewer ();" in close
    temporary = callback.split("case APIPalMsg_HidePalette_Begin:", 1)[1]
    temporary = temporary.split("case APIPalMsg_IsPaletteVisible:", 1)[0]
    assert "RequestClose" not in temporary and "SyncMenuCheck" not in temporary
    switch = (ADDON / "ArchViz/SurfaceSwitch.cpp").read_text(encoding="utf-8")
    overlay = _body(switch, "void BeforeOverlayStarts")
    assert "!ViewerOpen () && !ArchVizPanel::IsViewerOpen ()" in overlay
    assert "ArchVizPanel::CloseViewer ();" in overlay
    viewer = _body(switch, "void BeforeViewerOpens")
    assert "control::Overlay::ThreeD, control::Overlay::TwoD" in viewer
    assert 'control::SetWanted (which, false, "viewer")' in viewer


def test_project_close_and_replacement_close_the_viewer_and_reset_overlays():
    main = (ADDON / "AddOnMain.cpp").read_text(encoding="utf-8")
    events = _body(main, "static GSErrCode ProjectEventHandler")
    for event in ("APINotify_Open", "APINotify_Close"):
        body = events.split(f"case {event}:", 1)[1].split("break;", 1)[0]
        assert "ArchVizPanel::CloseViewer ();" in body
        assert "overlaycontrol::OnProjectClosed ();" in body


def test_overlay_checks_retain_independent_enabled_intent():
    controller = (ADDON / "ArchViz/OverlayController.cpp").read_text(encoding="utf-8")
    sync = _body(controller, "void SyncMenuChecks")
    assert "ArchVizOverlayMenuResId, ArchVizOverlayMenuItemIndex, g_intent[0].wanted" in sync
    assert "Overlay2DMenuResId, Overlay2DMenuItemIndex, g_intent[1].wanted" in sync
    assert "SyncMenuChecks ();" in _body(controller, "void OnProjectClosed")
