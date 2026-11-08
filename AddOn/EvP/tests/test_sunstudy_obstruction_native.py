"""Single-day obstruction's native scope and lifecycle contracts (#152)."""

from pathlib import Path

ADDON = Path(__file__).resolve().parents[1] / "Sources" / "AddOn"


def source(name):
    return (ADDON / name).read_text(encoding="utf-8")


def test_single_day_scope_refreshes_samples_even_when_the_header_is_collapsed():
    view = source("ArchViz/DiligentHudAnalysisView.cpp").split("void UpdateAndDrawObstructionOverlay", 1)[1]
    assert "analysistabs::kSunTab" in view
    assert "scene.ShownAnalysisKind () == 0" in view
    assert "!state.sunShowsPeriod" in view
    assert "state.sunView == kSunObstructionView" in view
    assert "RefreshSunStudyPointSamples (state, shown)" in view
    request = view.split("SunObstructionRequest request;", 1)[1].split("tasks.Request", 1)[0]
    assert "request.samples = state.sunPointSamples" in request
    assert request.index("RefreshSunStudyPointSamples") < request.index("request.samples")


def test_obstruction_is_a_view_not_a_study_point_footer():
    panel = source("ArchViz/DiligentHudSunStudy.cpp")
    assert 'Button ("Obstruction##sunview"' in panel
    assert "DrawSunStudyObstructionControls (state, study.studyId)" in panel
    point = source("ArchViz/DiligentHudSunPoint.cpp")
    day = point.split("void DrawSunStudyPointDay", 1)[1].split("void DrawSunStudyObstructionControls", 1)[0]
    assert 'RadioButton ("Off##sunobstruction"' not in point
    assert "sunView =" not in day
    controls = source("ArchViz/DiligentHudSunObstruction.cpp")
    assert 'RadioButton ("Time step##sunobstruction"' in controls
    assert 'RadioButton ("Whole day##sunobstruction"' in controls


def test_time_step_refusal_and_shared_capture_freshness_are_not_dropped():
    controller = source("ArchViz/SunStudyObstructionController.cpp")
    assert "day.error = day.steps[0].error" in controller
    freshness = controller.split("bool SunObstructionFrameCurrent", 1)[1]
    assert "MeshStore::Get ().Shared ()" in freshness
    assert "MeshStore::Get ().Current ()" not in freshness
    snapshot = controller.split("bool SunObstructionSnapshotCurrent", 1)[1].split("bool SunObstructionFrameCurrent", 1)[0]
    assert "shared->captureStamp == frame.captureStamp" in snapshot
    assert "captureStamp == frame.captureStamp" in snapshot
    assert "shared->id == frame.snapshotId" in snapshot


def test_lifted_point_keeps_its_suncut_radius_editable_and_resettable():
    point = source("ArchViz/DiligentHudSunPoint.cpp")
    header = point.split("bool DrawSunStudyPointHeader", 1)[1].split("void ArcSector", 1)[0]
    assert "BeginDisabled (state.sunPointHeight" not in header
    assert 'SliderFloat ("##sunpointradius", &state.sunPointRadius, 0.0f, 10.0f' in header
    assert "Suncut's half-width" in header

    view = source("ArchViz/DiligentHudAnalysisView.cpp")
    solar_cut = view.split("if (state.sunObstructionMode == 3 && state.sunCutTo > state.sunCutFrom)", 1)[1]
    solar_cut = solar_cut.split("else if (state.sunObstructionMode == 3)", 1)[0]
    assert "request.radius = double (state.sunPointRadius)" in solar_cut


def test_project_transitions_reset_without_permanently_stopping_the_worker():
    main = source("AddOnMain.cpp")
    assert main.count("ResetSunObstructionTasks ()") == 2
    assert main.count("ShutdownSunObstructionTasks ()") == 2
    controller = source("ArchViz/SunStudyObstructionController.cpp")
    cancel = controller.split("void SunObstructionController::Cancel", 1)[1].split("void SunObstructionController::Shutdown", 1)[0]
    assert "impl_->Clear ()" in cancel
    assert "stopping = true" not in cancel
