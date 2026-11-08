"""Native palette menu checks must follow explicit opens, not host modal hides."""

from pathlib import Path

import pytest
from test_native_menu_contract import _body

ADDON = Path(__file__).resolve().parents[1] / "Sources/AddOn"


@pytest.mark.parametrize(
    "path,owner,show,sync,resource,item_index",
    [
        (
            "PaletteRegistration.cpp",
            "ControlPalette",
            "bool focusSearch",
            "SyncPanelMenuCheck",
            "GeometryServerMenuResId",
            "GeometryServerMenuItemIndex",
        ),
        (
            "GraphEditorPalette.cpp",
            "GraphEditorPalette",
            "",
            "SyncMenuCheck",
            "GraphEditorMenuResId",
            "GraphEditorMenuItemIndex",
        ),
    ],
)
def test_palette_show_and_hide_sync_the_check_from_logical_open_state(path, owner, show, sync, resource, item_index):
    source = (ADDON / "Palette" / path).read_text(encoding="utf-8")
    opened = _body(source, f"void {owner}::Show ({show})" if show else f"void {owner}::Show ()")
    closed = _body(source, f"void {owner}::Hide ()")
    assert opened.index("paletteState.RequestOpen ()") < opened.index(sync)
    assert closed.index("paletteState.RequestClose ()") < closed.index(sync)
    helper = _body(source, f"void {sync}" if owner == "ControlPalette" else f"void {owner}::{sync}")
    assert f"item.menuResID = {resource};" in helper
    assert f"item.itemIndex = {item_index};" in helper
    assert "ACAPI_MenuItem_GetMenuItemFlags (&item, &flags) != NoError" in helper
    assert "flags | API_MenuItemChecked" in helper
    assert "flags & ~GSFlags (API_MenuItemChecked)" in helper
    assert "ACAPI_MenuItem_SetMenuItemFlags (&item, &wanted)" in helper
    assert "paletteState.IsOpen ()" in source


@pytest.mark.parametrize(
    "path,owner", [("PaletteRegistration.cpp", "ControlPalette"), ("GraphEditorPalette.cpp", "GraphEditorPalette")]
)
def test_host_close_clears_the_check_but_temporary_hides_preserve_intent(path, owner):
    source = (ADDON / "Palette" / path).read_text(encoding="utf-8")
    callback = _body(source, f"GSErrCode {owner}::PaletteControlCallBack")
    close = callback.split("case APIPalMsg_ClosePalette:", 1)[1].split("break;", 1)[0]
    assert "GetInstance ().Hide ();" in close
    temporary = callback.split("case APIPalMsg_HidePalette_Begin:", 1)[1]
    temporary = temporary.split("case APIPalMsg_DisableItems_Begin:", 1)[0]
    assert "paletteState.BeginHostHide (" in temporary
    assert "paletteState.EndHostHide ()" in temporary
    assert "DG::Palette::Hide ()" in temporary and "DG::Palette::Show ()" in temporary
    assert "GetInstance ().Hide ()" not in temporary
    assert "GetInstance ().Show ()" not in temporary
    assert "RequestClose" not in temporary and "SyncMenuCheck" not in temporary


def test_x_close_routes_through_the_checked_hide_path():
    for name in ("ControlPalette", "GraphEditorPalette"):
        source = (ADDON / "Palette" / f"{name}.cpp").read_text(encoding="utf-8")
        assert "Hide ();" in _body(source, f"void {name}::PanelCloseRequested")
