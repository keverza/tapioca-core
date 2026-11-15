"""S1 native controller seams; lifecycle behavior is covered by ControllerFixture."""

import json
import re
from pathlib import Path

import jsonschema

ROOT = Path(__file__).parents[1] / "Sources/AddOn"
SOURCE = (ROOT / "NativeCommands/SurfaceTakeoffControllerCommands.cpp").read_text(encoding="utf-8")


def schema(name):
    return json.loads(re.search(rf'const char {name}\[\]\s*=\s*R"json\((.*?)\)json"', SOURCE, re.S)[1])


def test_controller_commands_have_strict_schemas_and_registration():
    for name in (
        "kPrepareInput",
        "kPrepareOutput",
        "kSeededStartInput",
        "kCapturedStartInput",
        "kStartOutput",
        "kPollOutput",
        "kEmptyInput",
        "kPublishInput",
        "kPublishOutput",
        "kCancelInput",
        "kCancelOutput",
    ):
        data = schema(name)
        jsonschema.Draft202012Validator.check_schema(data)
        assert data["additionalProperties"] is False
    assert schema("kPollOutput")["properties"]["state"]["enum"] == [
        "empty",
        "pending",
        "current",
        "stale",
        "refused",
        "cancelled",
    ]
    registry = (ROOT / "NativeCommands/CommandRegistry.cpp").read_text(encoding="utf-8")
    assert "&GetSurfaceTakeoffControllerCommandRegistrations" in registry
    for command in (
        "PrepareExposedSurfaceTakeoff",
        "StartExposedSurfaceTakeoff",
        "StartSeededExposedSurfaceTakeoff",
        "StartCapturedExposedSurfaceTakeoff",
        "PollExposedSurfaceTakeoff",
        "PublishExposedSurfaceTakeoff",
        "CancelExposedSurfaceTakeoff",
    ):
        assert f'return "{command}"' in SOURCE


def test_preparation_schema_requires_explicit_bounded_policies():
    validate = jsonschema.Draft202012Validator(schema("kPrepareInput"))
    guided = {
        "regionMode": "allElementFaces",
        "coverPolicy": "constructionWithOpenings",
        "extraCoverGuids": [],
        "surfaceNames": [],
    }
    assert validate.is_valid(guided)
    assert not validate.is_valid({**guided, "coverPolicy": "all"})
    assert not validate.is_valid({**guided, "extraCoverGuids": ["one"] * 129})
    assert not validate.is_valid({**guided, "snapshotId": 1})


def test_seeded_start_accepts_a_pinned_scope_and_seed_count_only():
    input_schema = jsonschema.Draft202012Validator(schema("kSeededStartInput"))
    seeded = {
        "snapshotId": 1,
        "captureStamp": 2,
        "regionMode": "seededFacade",
        "seedCount": 1,
        "targetGuids": ["wall"],
        "coverGuids": [],
        "coverScopeId": "test",
        "surfaceAttributeIndices": [],
        "compareNative": False,
        "cancelWithRun": True,
    }
    assert input_schema.is_valid(seeded)
    assert not input_schema.is_valid({**seeded, "seeds": []})
    assert not input_schema.is_valid({**seeded, "seedCount": 9})
    assert not input_schema.is_valid({**seeded, "regionMode": "allWallFaces"})


def test_controller_has_no_overlay_dependency_and_checks_capture_before_publication():
    assert "Overlay3D" not in SOURCE and "ArchViz/Dxgi/" not in SOURCE
    controller = (ROOT / "Geometry/SurfaceTakeoffController.cpp").read_text(encoding="utf-8")
    assert "ticket->generation == generation_" in controller
    assert "Current (ticket->request.snapshot)" in controller
    assert "lastGood_" in controller and "ControllerState::Stale" in controller
    assert "ticket->cancelled.load ()" in controller


def test_generation_only_publication_uses_immutable_result_and_main_thread_sink():
    assert 'return "PublishExposedSurfaceTakeoff"' in SOURCE
    publish = SOURCE[SOURCE.index("class PublishTakeoffCommand") : SOURCE.index("class CancelTakeoffCommand")]
    assert "view.lastGood->generation != uint64_t (generation)" in publish
    assert "PublishExteriorSurfaceResult (*result, uint64_t (generation))" in publish
    assert "controller.CommitPublication" in publish
    assert "lastGoodResult" not in schema("kPublishInput")["properties"]
    takeoff = (ROOT / "NativeCommands/SurfaceTakeoffCommands.cpp").read_text(encoding="utf-8")
    assert "view::BuildFromResult (*snapshot, finalized, presentation, error)" in takeoff
    assert "ReadSurfaceCriteriaMetadata (*snapshot, targets, presentation.elements)" in takeoff


def test_document_launcher_is_stable_and_dispatches_only_after_layout():
    host = (ROOT / "ArchViz/OverlayHudHost.cpp").read_text(encoding="utf-8")
    owner = (ROOT / "ArchViz/OverlayControllerHud.cpp").read_text(encoding="utf-8")
    page = (ROOT / "ArchViz/SurfaceTakeoffPage.cpp").read_text(encoding="utf-8")
    viewer = (ROOT / "ArchViz/DiligentHudShell.cpp").read_text(encoding="utf-8")
    viewer_owner = (ROOT / "ArchViz/ViewerEditor.cpp").read_text(encoding="utf-8")
    assert 'kWorkflowKey, "Exposed surfaces"' in page
    assert "hudshell::WorkflowFamily::Document" in page
    for renderer in (host, viewer):
        assert "surfacetakeoff::WorkflowTab ()" in renderer
        assert "surfacetakeoff::DrawPage (" in renderer
    assert "store->exposedSurfaceAction = *action" in host
    assert 'DispatchCommandControlVerb ("EvP", "RunCommand"' not in host
    assert "TakeExposedSurfaceRequest" in owner
    assert 'DispatchCommandControlVerb ("EvP", "RunCommand"' in owner
    assert '"ExposedSurfaceCalculator"' in owner
    assert "StartCapturedExposedSurfaceTakeoff" not in viewer
    assert "vieweredit::RequestSurfaceTakeoff" in viewer
    assert "MainThreadGate::Get ().PostDeferred" in viewer_owner
    assert "StartCapturedExposedSurfaceTakeoff" in viewer_owner
    dispatch = (ROOT / "Python/CommandControlDispatch.cpp").read_text(encoding="utf-8")
    assert "SelectionSetStore::Get ().Configure (info.path, info.selectionSets" in dispatch
    assert "CameraSetStore::Get ().Configure (info.path, info.cameraSets)" in dispatch
    assert "const auto previousSelections" in dispatch and "const auto previousCameras" in dispatch
    assert "Restore (previousSelections)" in dispatch and "Restore (previousCameras)" in dispatch
    assert "run.generation == finishedGeneration" in dispatch
    assert "selections.Revision () == selectionRevision" in dispatch
    assert "cameras.Revision () == cameraRevision" in dispatch
    completion = dispatch[dispatch.index("const bool posted = MainThreadGate::Get ().Post") :]
    assert completion.index("Restore (previousSelections)") < completion.index("Finish (finishedGeneration")
    assert dispatch.index("SelectionSetStore::Get ().Configure") < dispatch.index("LaunchCommand (")


def test_native_comparison_is_deferred_and_cleanup_is_stop_safe_and_scoped():
    takeoff = (ROOT / "NativeCommands/SurfaceTakeoffCommands.cpp").read_text(encoding="utf-8")
    assert "input.compareNative && !t_controllerExecution" in takeoff
    assert "FinalizeNativeComparison (finalized, error)" in takeoff
    publish = takeoff[
        takeoff.index("NativeCommandResult PublishResult") : takeoff.index(
            "NativeCommandResult SurfaceTakeoffCommand::ExecuteNative"
        )
    ]
    assert "MainThreadGate" not in publish
    assert 'output.Add ("result", ToNativeResult (finalized).data)' in publish
    assert "MakeRegisteredNativeCommand<ClearSurfaceTakeoffCommand>, true, kClearInput" in takeoff
    assert "MakeRegisteredNativeCommand<CancelTakeoffCommand>, true, kCancelInput" in SOURCE
    controller = (ROOT / "Geometry/SurfaceTakeoffController.cpp").read_text(encoding="utf-8")
    assert "generation != 0 && generation_ != generation" in controller
    assert "ViewLocked ().state != ControllerState::Current" in controller


def test_clear_close_and_unload_revoke_native_controller():
    takeoff = (ROOT / "NativeCommands/SurfaceTakeoffCommands.cpp").read_text(encoding="utf-8")
    main = (ROOT / "AddOnMain.cpp").read_text(encoding="utf-8")
    assert "SharedSurfaceTakeoffController ().Clear (uint64_t (generation))" in takeoff
    assert main.count("SharedSurfaceTakeoffController ().OnProjectClosed ();") >= 3
    assert "SharedSurfaceTakeoffController ().Shutdown ();" in main
    assert main.count("ResetCommandRolesForProject ();") >= 3
    dispatch = (ROOT / "Python/CommandControlDispatch.cpp").read_text(encoding="utf-8")
    assert "if (s_pendingRoles)" in dispatch
    project_reset = dispatch[dispatch.index("void ResetCommandRolesForProject") :]
    assert project_reset.index("Restore (s_pendingRoles->selections)") < project_reset.index("ResetValues ()")
    assert "selections.Revision () == s_pendingRoles->selectionRevision" in project_reset
    assert "cameras.Revision () == s_pendingRoles->cameraRevision" in project_reset
    assert "if (temporaryCurrent)" in project_reset
