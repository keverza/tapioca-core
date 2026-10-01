"""Protect main-thread palette lifecycle integration outside the SDK-free tests."""

from pathlib import Path

_ADDON = Path(__file__).parents[1] / "Sources" / "AddOn"


def test_project_changes_close_both_palettes():
    source = (_ADDON / "AddOnMain.cpp").read_text(encoding="utf-8")
    for event in ("APINotify_Open", "APINotify_Close"):
        handler = source.split(f"case {event}:", 1)[1].split("break;", 1)[0]
        assert "ControlPalette::GetInstance ().Hide ()" in handler
        assert "ArchVizPanel::CloseViewer ()" in handler
    assert "case APINotify_New:" in source
    assert "case APINotify_NewAndReset:" in source


def test_host_workspace_open_callbacks_cannot_create_palettes():
    for path in ("Palette/PaletteRegistration.cpp", "ArchViz/ArchVizPanel.cpp"):
        source = (_ADDON / path).read_text(encoding="utf-8")
        opened = source.split("case APIPalMsg_OpenPalette:", 1)[1].split("break;", 1)[0]
        assert "CreateInstance" not in opened
        assert "paletteState.CanShow ()" in opened


def test_command_palette_restores_only_a_balanced_explicit_open():
    source = (_ADDON / "Palette" / "PaletteRegistration.cpp").read_text(encoding="utf-8")
    assert "paletteState.RequestOpen ()" in source
    assert "paletteState.RequestClose ()" in source
    assert "paletteState.BeginHostHide (panel.IsVisible ())" in source
    assert "paletteState.EndHostHide ()" in source
    restored = source.split("case APIPalMsg_HidePalette_End:", 1)[1].split("break;", 1)[0]
    assert "DG::Palette::Show ()" in restored
    assert "Show (true)" not in restored
