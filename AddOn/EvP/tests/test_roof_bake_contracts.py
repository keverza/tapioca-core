"""SDK glue contracts; live roof/annotation Undo and save/reopen remain separate gates."""

from pathlib import Path

ROOT = Path(__file__).parents[1] / "Sources/AddOn"


def test_bake_is_posted_and_dispatcher_owns_the_only_undo_scope():
    source = (ROOT / "ArchViz/RoofSlopeBake.cpp").read_text(encoding="utf-8")
    assert "MainThreadGate::Get ().Post" in source
    assert "DispatchApiCall" in source
    assert 'Call ("Tapioca.CommitTransaction"' in source
    assert "ACAPI_CallUndoableCommand" not in source
    assert "ACAPI_Element_Create" not in source
    assert "session->editor.revision != revision" in source
    assert "state != guesttext::HudState ()" in source
    assert "state->roofEditor != session || session->close" in source
    assert "RestoreDatabase" in source


def test_cancel_bake_and_edge_actions_do_not_execute_model_writes_in_imgui():
    source = (ROOT / "ArchViz/RoofSlopeHud.cpp").read_text(encoding="utf-8")
    for label in ("Cancel", "Bake", "Add edge", "Delete edge"):
        assert f'ImGui::MenuItem ("{label}"' in source
    assert 'ImGui::SmallButton ("Finish")' not in source
    assert 'ImGui::SmallButton ("Cancel")' in source
    assert 'ImGui::SmallButton ("Bake")' in source
    assert "s.bakeRequested = true" in source
    assert "ACAPI_" not in source and "DispatchApiCall" not in source
    frame = (ROOT / "ArchViz/OverlayHud.cpp").read_text(encoding="utf-8")
    assert "hudshell::BaseStyle (ui);" in frame
    assert "BaseStyle (ui, store->roofEditor" not in frame


def test_create_roof_preserves_openings_and_associative_levels_inherit_home_story():
    roof = (ROOT / "NativeCommands/RoofCreateCommands.cpp").read_text(encoding="utf-8")
    assert 'params.Get ("holes", holes)' in roof
    assert "plane.poly.nSubPolys = Int32 (holes.GetSize ()) + 1" in roof
    assert "(*pendsH)[ringIndex++] = cursor - 1" in roof
    dimension = (ROOT / "NativeCommands/CreateCommands.cpp").read_text(encoding="utf-8")
    assert 'params.Get ("floorInd", requestedFloor)' in dimension
    assert "element.header.floorInd = short (requestedFloor)" in dimension
