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
    # Switching tabs hides the other tab's result; it must not clear it.
    assert "originChanged" not in source
    assert " ".join(source.split()).count("if (ownResult) visibilitystudy::ClearDisplay ();") == 1
    assert "request.options.origin = state.visibilityOrigin == 0" in source


def test_clearing_a_study_does_not_send_the_analysis_page_to_its_first_tab():
    # A role edit clears the display; only the focus (AnalysisTabs) may pick a tab,
    # and test_analysistabs.cpp holds that a clear never moves it.
    tabs = (_ARCHVIZ / "DiligentHudTabs.cpp").read_text(encoding="utf-8")
    page = tabs.split("void SunStudyPage", 1)[1].split("void DebugPage", 1)[0]
    assert "analysisKind" not in page
    assert page.count("ImGuiTabItemFlags_SetSelected") == 1
    assert "focus.selectTab == tab" in page
