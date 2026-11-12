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
    # #168: the solar cut is the default; its Shadow at clock replaced Time step.
    assert '"Solar cut##sunobstruction"' in controls
    assert '"Whole day##sunobstruction"' in controls
    assert "Time step##sunobstruction" not in controls


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


def test_solar_cut_uses_geometry_not_cell_hole_filling_and_masks_the_kept_scene():
    controller = source("ArchViz/SunStudyObstructionController.cpp")
    solar = controller.split("SolarCutFrame (", 1)[1].split("struct SunObstructionController::Impl", 1)[0]
    assert "ComputeSolarExposure (snapshot, cut" in solar
    assert "AppendSolarExposure (frame->exposure" in solar
    assert "AppendSolarCutWonCells" not in solar
    # The after scene is the study's BVHs with the cut elements masked out plus
    # the cut elements as they remain -- never a copied scene and a rebuilt
    # index per point move (#168), and never a moved-from kept mesh.
    assert "RaycastMasked (origin, direction, 0.0, mask)" in solar
    assert "keptScene.meshes.push_back (kept)" in solar
    assert "geomsrv::Snapshot remains = snapshot" not in solar
    assert "BuildSunStudyOccluders" not in solar
    assert "std::move (kept)" not in solar
    assert "record.patchGrid.patchOfTriangle" in solar
    assert "receivers = record.sampleGrid.faces" in solar


def test_solar_cut_hover_is_xray_scoped_fresh_and_invalidated_by_a_new_frame():
    inspector = source("ArchViz/DiligentHudSunInspector.cpp")
    solar = inspector.split("void ServiceSolarCutReading", 1)[1].split("void ServiceSunStudyInspector", 1)[0]
    assert "state.analysisFocus.tab != analysistabs::kSunTab" in solar
    assert "state.sunView != kSunObstructionView" in solar
    assert "state.sunObstructionMode != 3" in solar
    assert "SunObstructionFrameCurrent (frame, scene.ShownSunStudyId ())" in solar
    assert "frame->cutStage != SunSolarCutStage::Done" in solar
    assert "generation == frame->generation" in solar
    assert "ReadSolarExposure (frame->exposure" in solar
    # GPU picks see the original model, not the semantic x-ray. The won layer
    # must remain inspectable through it and must retire outside its own view.
    assert "state.hover" not in solar
    assert "state.sunCutReadingMinute = -1" in solar


def test_solar_cut_does_not_repaint_whole_object_wire_over_its_own_cut_boundary():
    view = source("ArchViz/DiligentHudAnalysisView.cpp")
    highlight = view.split("scene.SetObstructionHighlight", 1)[1].split("if (!current)", 1)[0]
    assert "frame->mode != SunObstructionMode::SolarCut" in highlight
