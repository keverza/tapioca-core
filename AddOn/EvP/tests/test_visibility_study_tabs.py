from pathlib import Path


_ARCHVIZ = Path(__file__).parents[1] / "Sources" / "AddOn" / "ArchViz"


def test_analysis_has_separate_surface_and_point_visibility_tabs():
    tabs = (_ARCHVIZ / "DiligentHudTabs.cpp").read_text(encoding="utf-8")
    assert 'ImGui::BeginTabItem ("Sun study"' in tabs
    surface = tabs.split('ImGui::BeginTabItem ("Surface visibility"', 1)[1].split(
        "ImGui::EndTabItem ();", 1
    )[0]
    point = tabs.split('ImGui::BeginTabItem ("Point visibility"', 1)[1].split(
        "ImGui::EndTabItem ();", 1
    )[0]
    assert "DrawVisibilityHudSection (state, scene, 0);" in surface
    assert "DrawVisibilityHudSection (state, scene, 1);" in point


def test_visibility_tabs_set_the_submitted_study_origin():
    source = (_ARCHVIZ / "DiligentHudVisibility.cpp").read_text(encoding="utf-8")
    assert "state.visibilityOrigin = origin;" in source
    assert "bool changed = originChanged;" in source
    assert "request.options.origin = state.visibilityOrigin == 0" in source


def test_clearing_a_study_does_not_send_the_analysis_page_to_its_first_tab():
    # A role edit clears the display; only a study that ARRIVES may pick a tab.
    tabs = (_ARCHVIZ / "DiligentHudTabs.cpp").read_text(encoding="utf-8")
    page = tabs.split("void SunStudyPage", 1)[1].split("hudshell::BeginSubTabs", 1)[0]
    assert "const bool changed = !shown.empty () && shown != state.analysisSeenStudy;" in page
    assert "analysisKind" not in page
