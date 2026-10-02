"""Protect SDK/main-thread seams that cannot be linked into the offline suite."""

import json
import re
from pathlib import Path

_ADDON = Path(__file__).parents[1] / "Sources" / "AddOn"


def test_follower_submits_calculation_without_native_advance_or_worker_join():
    source = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    assert 'ExecuteNativeCommand ("AdvanceSunStudy"' not in source
    assert "AdvanceWorker ().Submit" in source
    assert "AdvanceWorker ().Poll" in source
    assert "AdvanceWorker ().Cancel" in source
    disable = source.split("void Disable ()", 1)[1].split("void Shutdown ()", 1)[0]
    assert "Shutdown" not in disable
    assert "join" not in disable
    assert "completion.request.sessionGeneration != s_sessionGeneration" in source
    assert "completion.request.recordRevision != s_runRevision" in source


def test_worker_never_enters_the_sdk_or_main_thread_gate():
    source = (_ADDON / "SunStudy" / "SunStudyAdvanceWorker.cpp").read_text(encoding="utf-8")
    assert "ACAPI_" not in source
    assert "MainThreadGate" not in source
    assert "store.Advance" in source
    assert "request.recordRevision" in source
    assert "ticket->cancelled" in source


def test_adoption_is_gated_and_a_closed_session_cannot_rearm_following():
    source = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    adopt = source.split("void Adopt (", 1)[1].split("uint64_t SessionGeneration ()", 1)[0]
    assert "IsMainThread ()" in adopt
    assert "[studyId, config, sessionGeneration]" in adopt
    assert "sessionGeneration != s_sessionGeneration" in adopt
    assert "++s_sessionGeneration" in source
    display = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    assert "sunfollow::Adopt (id, config, sessionGeneration)" in display
    assert display.index("sunfollow::SessionGeneration ()") < display.index("PushSunStudyAtlas")


def test_worker_is_joined_on_both_quit_and_unload():
    source = (_ADDON / "AddOnMain.cpp").read_text(encoding="utf-8")
    quit_handler = source.split("case APINotify_Quit:", 1)[1].split("break;", 1)[0]
    unload = source.split("GSErrCode FreeData (void)", 1)[1]
    assert "geomsrv::sunfollow::Shutdown ()" in quit_handler
    assert "geomsrv::sunfollow::Shutdown ()" in unload


def test_follower_diagnostic_fields_are_declared_in_the_strict_response_schema():
    source = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    registration = source.split('{ "SunStudyFollowerState",', 1)[1]
    schemas = re.findall(r'R"json\((.*?)\)json"', registration, re.DOTALL)
    response = json.loads(schemas[1])
    for field in (
        "sessionGeneration",
        "cancelledRuns",
        "sliceSubmissions",
        "sliceCompletions",
        "workerBusy",
        "stage",
        "navigationDeferred",
        "completionWakes",
        "completionWakeFailures",
        "resolvedSteps",
        "totalSteps",
        "tickThread",
        "workerThread",
    ):
        assert field in response["properties"]
        assert f'os.Add ("{field}"' in source
    assert response["additionalProperties"] is False


def test_follower_preparation_uses_owned_capture_not_main_thread_start_command():
    source = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    assert 'ExecuteNativeCommand ("StartSunStudy"' not in source
    assert "CaptureSunStudyInputs (StartParams (gConfig), captured)" in source
    assert "[captured, output, reuseSource]" in source
    assert "PrepareCapturedSunStudy (captured, cancelled, reuseSource)" in source
    assert "PreparationWorker ().Cancel" in source
    assert "PreparationWorker ().Shutdown" in source
    commands = (_ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")
    prepare = commands.split("class StartSunStudyCommand", 1)[1].split("class AdvanceSunStudyCommand", 1)[0]
    assert "ACAPI_GeoLocation_" not in prepare
    assert "record->series = captured->series" in prepare
    capture = (_ADDON / "NativeCommands" / "SunStudyCapture.cpp").read_text(encoding="utf-8")
    assert "IsMainThread ()" in capture
    assert "QueryIndexCache" not in capture
    assert "BuildSampleGrid" not in capture


def test_interactive_calculation_reserves_cpu_and_defers_capture_during_navigation():
    source = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    assert "request.maxParallel = 1" in source
    assert "request.maxMilliseconds = 40.0" in source
    assert "s_refreshSchedule.Ready (now, s_navigationDeferred)" in source
    task = (_ADDON / "SunStudy" / "SunStudyTaskWorker.cpp").read_text(encoding="utf-8")
    assert "ACAPI_" not in task
    assert "MainThreadGate" not in task
    assert "UseInteractiveStudyPriority ()" in task
    policy = (_ADDON / "SunStudy" / "SunStudyThreadPolicy.hpp").read_text(encoding="utf-8")
    assert "THREAD_PRIORITY_BELOW_NORMAL" in policy
    assert "THREAD_MODE_BACKGROUND_BEGIN" not in policy


def test_completions_post_a_session_guarded_main_thread_wake():
    source = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    assert "s_completionTickPending.exchange (true)" in source
    assert "[sessionGeneration]" in source
    assert "SessionGeneration () == sessionGeneration" in source
    assert "WakeFollower (sessionGeneration)" in source
    for filename in ("SunStudyAdvanceWorker.cpp", "SunStudyTaskWorker.cpp"):
        worker = (_ADDON / "SunStudy" / filename).read_text(encoding="utf-8")
        assert "NotifyStudyReady (ticket->request.onReady)" in worker


def test_automatic_replacement_never_coarsens_and_reuses_only_compatible_accepted_cache():
    source = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    assert "s_previewSpacing" not in source
    assert "MakeSunStudyPreviewPlan" not in source
    assert "accepted.sun == signature.sun && accepted.sampling == signature.sampling" in source
    assert "CompletedRecord (studyId)" in source
    assert "CompletedRecord (gRunStudyId)" in source
    assert 'show.Add ("preview", false)' in source
    display = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    assert "upload->preview && !converged" in display
    assert '"preview":{"type":"boolean"}' in display


def test_incremental_publication_uses_the_stale_guard_and_commits_after_enqueue():
    source = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    check = source.index("gFollower.CanPublishResult (gRunGeneration, gRunSignature)")
    show = source.index('ExecuteNativeCommand ("ShowSunStudy", show)')
    accept = source.index("gFollower.NoteCompleted (gRunGeneration, gRunStudyId, gRunSignature, now)")
    assert check < show < accept
    assert "!shown.ok || !enqueuedToViewer" in source
    assert "s_reuseRecord.reset ();" in source.split("void DisableLocked ()", 1)[1]


def test_pipeline_telemetry_covers_capture_conversion_queue_upload_and_each_sun_step():
    archviz = _ADDON / "ArchViz"
    extraction = (archviz / "ExtractionThread.cpp").read_text(encoding="utf-8")
    assert "stage=geometry-acquire" in extraction
    assert "stage=geometry-slice" in extraction
    assert "CaptureElementPacket (*model, i, st->meshes)" in extraction
    assert "MakeElementPacket (packet)" in extraction
    conversion = (archviz / "ElementPacket.cpp").read_text(encoding="utf-8")
    for field in ("sourceBytes=", "payloadBytes=", "captureMs=", "convertMs="):
        assert field in conversion
    apply = (archviz / "ScenePacketTrace.cpp").read_text(encoding="utf-8")
    assert "queueWaitMs=" in apply and "apiApplyMs=" in apply
    upload = (archviz / "DiligentSceneSunStudy.cpp").read_text(encoding="utf-8")
    for stage in ("sun-atlas-upload", "sun-steps-upload", "sun-face-upload"):
        assert f"stage={stage}" in upload
    prepare = (_ADDON / "NativeCommands" / "SunStudyPreparation.cpp").read_text(encoding="utf-8")
    assert "SetStepObserver" in prepare
    for field in ("step=", "dirty=", "rays=", "rayPacketBytes=", "traceMs=", "compactMs="):
        assert field in prepare
    reuse = (_ADDON / "SunStudy" / "SunStudyReuse.cpp").read_text(encoding="utf-8")
    assert "ACAPI_" not in reuse and "MainThreadGate" not in reuse


def test_geometry_batches_wait_for_worker_drain_before_capture_and_do_not_debounce_twice():
    source = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    pending = source.split("if (s_refreshSchedule.Pending ()) {", 1)[1].split("// ---- 2.", 1)[0]
    assert pending.index("AdvanceWorker ().Busy () || PreparationWorker ().Busy ()") < pending.index(
        "if (!RefreshSnapshot ())"
    )
    assert "signatureObservedMs = s_refreshSchedule.LastEditMs ()" in pending
    assert "gFollower.Observe (world, signatureObservedMs)" in source
    assert "s_refreshSchedule.MillisecondsUntilReady (NowMs ())" in source
    for stage in ("sun-batch-pending", "sun-batch-capture"):
        assert f"stage={stage}" in source
    for field in ("signals=", "observations=", "quietMs=", "waitMs=", "pendingTargets="):
        assert field in source


def test_gpu_selection_is_only_for_owned_automatic_preparation_and_uses_role_filtered_scene():
    source = (_ADDON / "NativeCommands" / "SunStudyPreparation.cpp").read_text(encoding="utf-8")
    assert "if (cancelled != nullptr)" in source
    assert "std::make_shared<archviz::SunStudyGpuTraversal> (" in source
    assert "record.occluders != nullptr ? record.occluders->context : nullptr, previousGpu" in source
    commands = (_ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")
    assert "FinishSunStudyPreparation (*record, snapshot, reuseSource_.get (), cancelled_, occluders)" in commands
    assert "D3D11" not in commands


def test_context_index_reuse_does_not_build_a_combined_scene_first():
    commands = (_ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")
    preparation = (_ADDON / "NativeCommands" / "SunStudyPreparation.cpp").read_text(encoding="utf-8")
    separated, combined = preparation.split("if (roles.context > 0)", 1)[1].split("auto parts", 1)
    assert "BuildSunStudyOccluders" in separated
    assert "previous->occluders.get ()" in separated
    assert "QueryIndexCache" not in separated
    assert "QueryIndexCache::Get ().For (snapshot)" in combined
    assert "PrepareSunStudyOccluders (" in commands
    assert "record->occluders = occluderParts" in commands
    assert "inputs.geometryVersion = snapshot->id" in commands
    source = (_ADDON / "SunStudy" / "SunStudyOccluders.cpp").read_text(encoding="utf-8")
    assert "ACAPI_" not in source and "MainThreadGate" not in source
    assert "CanonicalGuid (mesh.guid)" in source
    assert "mesh->vertices != found->second->vertices" in source
    assert "mesh->triangles != found->second->triangles" in source
    assert "parts->context = previous->context" in source


def test_gpu_context_buffers_reuse_only_matching_immutable_index_on_private_device():
    source = (_ADDON / "ArchViz" / "SunStudyGpuTraversal.cpp").read_text(encoding="utf-8")
    assert "contextEngine == previous->contextEngine" in source
    assert "contextScene = previous->contextScene" in source
    assert "deviceMutex = previous->deviceMutex" in source
    assert "deviceLock (*impl_->deviceMutex)" in source
    for field in ("contextReused=", "contextUploadedBytes=", "deviceReused=", "gpuComputeMs=", "timedPackets="):
        assert field in source


def test_gpu_backend_owns_context_and_has_bounded_cancellable_packets_and_explicit_fallback():
    source = (_ADDON / "ArchViz" / "SunStudyGpuTraversal.cpp").read_text(encoding="utf-8")
    for forbidden in ("ACAPI_", "MainThreadGate", "DiligentViewport", "D3D_DRIVER_TYPE_WARP"):
        assert forbidden not in source
    for required in (
        "D3D11CreateDevice",
        "D3D11_FEATURE_DOUBLES",
        "D3D11_ASYNC_GETDATA_DONOTFLUSH",
        "D3D11_MAP_FLAG_DO_NOT_WAIT",
        "kPacketRays = 4096",
        "kRayWorkLimit = 4096",
        "isCancelled ()",
        "CPU/GPU parity mismatch",
        "TAPIOCA_SUNSTUDY_GPU",
        "stage=sun-gpu-init",
        "stage=sun-gpu-step",
        "stage=sun-gpu-fallback",
    ):
        assert required in source
    assert "OccludeDirectionalCancellable" in (_ADDON / "SunStudy" / "SunStudyOcclusion.cpp").read_text(
        encoding="utf-8"
    )
