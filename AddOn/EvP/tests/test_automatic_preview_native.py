"""Catalog and palette source contracts for automatic selection previews."""

from pathlib import Path

_ADDON = Path(__file__).parents[1] / "Sources" / "AddOn"


def test_catalog_reads_and_reemits_the_opt_in_contract():
    header = (_ADDON / "Python" / "CommandCatalog.hpp").read_text(encoding="utf-8")
    source = (_ADDON / "Python" / "CommandCatalog.cpp").read_text(encoding="utf-8")

    assert "bool previewOnSelection = false" in header
    assert 'os.Get ("preview_on_selection", info.previewOnSelection)' in source
    assert 'os.Get ("preview_overrides_json", info.previewOverridesJson)' in source
    assert 'command.Add ("preview_on_selection", info.previewOnSelection)' in source
    assert 'command.Add ("preview_overrides_json", info.previewOverridesJson)' in source


def test_runner_forces_overrides_and_arms_watch_on_automatic_runs():
    source = (_ADDON / "Palette" / "ControlPaletteRun.cpp").read_text(encoding="utf-8")

    assert "MergeForcedParams" in source
    assert "const bool watchArmed = automaticPreviewRun ||" in source
    assert 'SetCommandStatus ("Generating preview for "' in source


def test_selection_mutations_schedule_only_when_the_native_result_changed():
    panel = (_ADDON / "Palette" / "SelectionSetPanel.cpp").read_text(encoding="utf-8")
    palette = (_ADDON / "Palette" / "ControlPalette.cpp").read_text(encoding="utf-8")

    assert "contentsChanged = action != Action::Reselect && changed > 0" in panel
    assert "if (selectionContentsChanged)" in palette
    assert "automaticPreview.SelectionChanged" in palette


def test_cancelled_preview_clears_owner_before_requesting_cancel():
    source = (_ADDON / "Palette" / "ControlPaletteAutoPreview.cpp").read_text(encoding="utf-8")
    clear = source.index("automaticPreviewFolder.Clear ();")
    cancel = source.index("RunCancel::Get ().Request")

    assert clear < cancel
