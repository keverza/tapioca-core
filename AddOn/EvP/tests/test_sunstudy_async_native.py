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
    assert display.index("sunfollow::SessionGeneration ()") < display.index("SubmitManualSunStudyDisplay")
    preparation = (_ADDON / "NativeCommands" / "SunStudyDisplayPreparation.cpp").read_text(encoding="utf-8")
    assert "sunfollow::Adopt (request.studyId, request.config, request.sessionGeneration)" in preparation
    assert preparation.index("PublishInSession") < preparation.index("PushSunStudyAtlas")


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
    assert "options.preview = false" in source
    display = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    assert "options.preview && !progress.converged" in display
    assert '"preview":{"type":"boolean"}' in display


def test_incremental_publication_uses_the_stale_guard_and_commits_after_enqueue():
    source = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    check = source.index("gFollower.CanPublishResult (gRunGeneration, gRunSignature)")
    show = source.index("PushSunStudyAtlas (std::move (output->upload))")
    accept = source.index("gFollower.NoteCompleted (gRunGeneration, gRunStudyId, gRunSignature, now)")
    assert check < show < accept
    assert "completion.sessionGeneration != s_sessionGeneration || completion.runGeneration != gRunGeneration" in source
    assert "output->upload == nullptr || !archviz::DiligentViewport::Get ().IsRunning ()" in source
    assert "s_reuseRecord.reset ();" in source.split("void DisableLocked ()", 1)[1]


def test_pipeline_telemetry_covers_capture_conversion_queue_upload_and_each_sun_step():
    archviz = _ADDON / "ArchViz"
    extraction = (archviz / "ExtractionThread.cpp").read_text(encoding="utf-8")
    assert "stage=geometry-acquire" in extraction
    assert "stage=geometry-slice" in extraction
    assert "extractionslice::Run (*model, count, *st, *wanted" in extraction
    slice_source = (archviz / "ExtractionSlice.cpp").read_text(encoding="utf-8")
    assert "CaptureElementPacket (model, i, st.meshes)" in slice_source
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


def test_gpu_selection_is_explicit_for_both_producers_and_uses_role_filtered_scene():
    source = (_ADDON / "NativeCommands" / "SunStudyPreparation.cpp").read_text(encoding="utf-8")
    assert 'if (record.backend == "gpu")' in source
    assert "std::make_shared<archviz::SunStudyGpuTraversal> (" in source
    assert "record.occluders != nullptr ? record.occluders->context : nullptr, previousGpu" in source
    commands = (_ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")
    assert "FinishSunStudyPreparation (*record, snapshot, reuseSource.get (), cancelled_, occluders)" in commands
    assert 'ReadString (params, "backend", cancelled_ != nullptr ? "gpu" : "cpu")' in commands
    assert "sunfollow::Disable ()" in commands
    request = re.findall(r'R"json\((.*?)\)json"', commands.split('{ "StartSunStudy",', 1)[1], re.DOTALL)[0]
    assert json.loads(request)["properties"]["backend"]["enum"] == ["cpu", "gpu"]
    driver = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    assert 'params.Add ("backend"' in driver
    display = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    assert "config.backend = metadata.backend" in display
    assert "reuseSource != nullptr && cancelled != nullptr" in source  # manual CPU baseline traces a fresh day
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
        "kMinGpuRays = 4096",
        "kPacketRays = 16384",
        "kInFlightPackets = 3",
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


def test_gpu_pipeline_keeps_exact_cpu_guards_and_avoids_duplicate_serial_checks():
    source = (_ADDON / "ArchViz" / "SunStudyGpuTraversal.cpp").read_text(encoding="utf-8")
    assert "cpu.Occluded (" not in source
    assert "cpu.OccludeDirectional (&checkPositions" in source
    assert "answers[i] >= 2 || validate" in source
    assert "CopySubresourceRegion (packet.staging" in source
    assert "ReadPacket (packet" in source and "SubmitPacket (packet" in source
    for field in ("ambiguousRays=", "workLimitRays=", "cpuCheckRays=", "maxInFlight="):
        assert field in source


def test_bound_role_sets_are_captured_on_main_and_followed_before_completion_can_publish():
    capture = (_ADDON / "NativeCommands" / "SunStudyCapture.cpp").read_text(encoding="utf-8")
    assert capture.index("IsMainThread ()") < capture.index("SelectionSetStore::Get ()")
    for field in ("contextSelectionSet", "ignoredSelectionSet"):
        assert field in capture
    assert "binding.generation = selections.Generation ()" in capture
    assert "binding.revision = selections.Revision ()" in capture
    assert 'params.Contains ("contextElements")' in capture
    assert 'params.Contains ("ignoredElements")' in capture
    commands = (_ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")
    assert "SelectionSetStore" not in commands
    assert "record->selectionBinding = binding" in commands
    assert "captured->contextElements" in commands
    assert "captured->ignoredElements" in commands
    source = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    tick = source.split("void Tick ()", 1)[1].split("FollowerStats State ()", 1)[0]
    assert tick.index("RefreshRoleSelections ()") < tick.index("BuildCurrentSignature (gConfig)")
    assert tick.index("RefreshRoleSelections ()") < tick.index("AdvanceOneSlice (now)")
    assert "binding.generation == store.Generation () && binding.revision == store.Revision ()" in source
    assert "binding.Refresh (" in source
    start_params = source.split("GS::ObjectState StartParams (", 1)[1].split("void HideOverlay ()", 1)[0]
    for role in ("context", "ignored"):
        assert f'params.Add ("{role}SelectionSet"' in start_params
        assert f'params.Add ("{role}Elements"' in start_params
    store = (_ADDON / "NativeCommands" / "SelectionSetStore.cpp").read_text(encoding="utf-8")
    configure = store.split("void SelectionSetStore::Configure (", 1)[1].split("void SelectionSetStore::Clear ()", 1)[0]
    clear = store.split("void SelectionSetStore::Clear ()", 1)[1].split("SelectionSetStore::Entry*", 1)[0]
    for declaration_change in (configure, clear):
        assert "++generation;" in declaration_change
        assert "++revision;" in declaration_change
    mutation = store.split("bool SelectionSetStore::Mutate (", 1)[1]
    assert "++revision;" in mutation
    assert "++generation;" not in mutation
    display = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    assert "config.selectionBinding = metadata.selectionBinding" in display


def test_pause_retires_old_producer_but_does_not_clear_display_or_join():
    source = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    pause = source.split("class PauseSunStudyFollowingCommand", 1)[1].split("class SunStudyFollowerStateCommand", 1)[0]
    assert "sunfollow::Disable ()" in pause
    assert "PushClearSunStudy" not in pause
    assert "join" not in pause
    registration = source.split('{ "PauseSunStudyFollowing",', 1)[1].split('{ "ShowSunStudy",', 1)[0]
    schemas = re.findall(r'R"json\((.*?)\)json"', registration, re.DOTALL)
    assert json.loads(schemas[0])["additionalProperties"] is False
    assert json.loads(schemas[1])["required"] == ["autoFollow"]


def test_surface_sampling_reuse_remains_owned_sdk_free_and_separate_from_result_reuse():
    commands = (_ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")
    assert re.search(
        r"BuildSurfaceSampling\s*\(\s*\*snapshot,\s*sampleMask,\s*options,\s*reuseSource\.get\s*\(\s*\)", commands
    )
    assert "record->samplingLayout = std::move (sampling.layout)" in commands
    sampling = (_ADDON / "SunStudy" / "SunStudySurfaceSampling.cpp").read_text(encoding="utf-8")
    assert "ACAPI_" not in sampling and "MainThreadGate" not in sampling
    assert "item.vertices == oldMesh.vertices && item.triangles == oldMesh.triangles" in sampling
    assert "oldSpan.sampleFaces == span.sampleFaces" in sampling
    preparation = (_ADDON / "NativeCommands" / "SunStudyPreparation.cpp").read_text(encoding="utf-8")
    for field in ("stage=sun-grid-reuse", "reusedMeshes=", "rebuiltMeshes=", "reusedGridSamples=", "generatedSamples="):
        assert field in preparation


def test_presets_glass_filter_and_live_analysis_survive_follower_adoption_and_strict_schemas():
    commands = (_ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")
    registration = commands.split('{ "StartSunStudy",', 1)[1].split('{ "AdvanceSunStudy",', 1)[0]
    schemas = re.findall(r'R"json\((.*?)\)json"', registration, re.DOTALL)
    request, response = map(json.loads, schemas)
    for field in ("preset", "glassThreshold", "analysisSelectionSet"):
        assert field in request["properties"]
    assert request["properties"]["preset"]["enum"] == ["early", "late"]
    for field in ("preset", "presetReason", "analysisFaceCount", "contextFaceCount", "unknownMaterialFaces"):
        assert field in response["properties"]
        assert f'os.Add ("{field}"' in commands
    assert request["additionalProperties"] is False and response["additionalProperties"] is False
    assert "analysisRestricted && analysisPicked.empty ()" in commands
    capture = (_ADDON / "NativeCommands" / "SunStudyCapture.cpp").read_text(encoding="utf-8")
    assert "inputs->materialTransparency = inputs->snapshot->materialTransparency" in capture
    assert "AcquireCurrentModel" not in capture
    extraction = (_ADDON / "Geometry" / "GeometryExtractor.cpp").read_text(encoding="utf-8")
    snapshot = extraction.split("std::shared_ptr<const Snapshot> BuildSnapshot", 1)[1].split("return snap;", 1)[0]
    assert "snap->materialTransparency = ReadMaterialTransparency (model)" in snapshot
    assert snapshot.index("ReadMaterialTransparency (model)") < snapshot.index("GetElementCount ()")
    assert "result.emplace (index, material.GetTransparency ())" in extraction
    filtered = (_ADDON / "NativeCommands" / "SnapshotCommands.cpp").read_text(encoding="utf-8")
    assert "keep->materialTransparency = snap->materialTransparency" in filtered
    driver = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    display = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    for field in ("preset", "glassThreshold", "analysisRestricted"):
        assert f"config.{field} = metadata.{field}" in display
        assert f"config.{field}" in driver
    assert "read (binding.analysisSet)" in driver
    assert 'params.Add ("analysisSelectionSet"' in driver
    assert "RecommendSunStudyPreset" not in driver  # fixed after initial adoption, no oscillation during edits


def test_viewer_and_follower_share_only_complete_revision_guarded_sliced_captures():
    extraction = (_ADDON / "ArchViz" / "ExtractionThread.cpp").read_text(encoding="utf-8")
    assert "assembly.Add (std::move (packet.mesh))" in extraction
    assert "PublishShared (snapshot)" in extraction
    assert "SnapshotAssembly::CanUpdate" in extraction
    assert "modelwatch::CaptureStamp () != handle->captureStamp" in extraction
    assert "!gaveUp && !stopFlag_.load () && changedTo < 0" in extraction
    driver = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    refresh = driver.split("bool RefreshSnapshot ()", 1)[1].split("void StartReplacement", 1)[0]
    assert "MeshStore::Get ().Shared ()" in refresh
    assert "ExtractionWorker::Get ().Start (true)" in refresh
    assert 'params.Add ("reuseShared", true)' in refresh
    assert "CaptureStamp ()" in driver.split("void Tick ()", 1)[1]
    commands = (_ADDON / "NativeCommands" / "SnapshotCommands.cpp").read_text(encoding="utf-8")
    assert "snap->captureStamp == stamp && snap->completeModel" in commands
    assert "keep->id = MeshStore::Get ().NextId ()" in commands
    assert '"reuseShared":{"type":"boolean"}' in commands
    assembly = (_ADDON / "Geometry" / "SnapshotAssembly.cpp").read_text(encoding="utf-8")
    assert "ACAPI_" not in assembly and "MainThreadGate" not in assembly


def test_replacements_reuse_allocations_and_renderer_uploads_exact_base_regions_with_full_fallback():
    commands = (_ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")
    assert "record->patchAtlas = reuseSource->patchAtlas" in commands
    assert "BuildStableTriangleAtlas" in commands
    render = (_ADDON / "ArchViz" / "DiligentSceneSunStudy.cpp").read_text(encoding="utf-8")
    assert "CanApplySunAtlasRegions (*impl_->sunStudyPayload, *study)" in render
    assert "CanApplySunStepRegions (*impl_->sunStudyPayload, *study)" in render
    assert "context->UpdateTexture (texture, 0, region.layer" in render
    assert "device->CreateTexture (desc, &data, &atlasTexture)" in render
    assert render.index("device->CreateTexture (stepDesc") < render.index(
        "ClearSunStudy ();", render.index("void DiligentScene::ApplySunStudy")
    )
    assert "SameSunStudyElementMap" in render
    assert "study->baseTexels.reset ()" in render


def test_display_assembly_is_worker_owned_and_publication_is_guarded_after_preparation():
    display = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    show = display.split("NativeCommandResult ShowSunStudyCommand::ExecuteNative", 1)[1].split(
        "class SunStudyOverlayStateCommand", 1
    )[0]
    assert "SubmitManualSunStudyDisplay" in show
    for forbidden in ("DisplayData (", "StepMasks (", "BuildSunStudyElementMap", "ScatterToAtlas"):
        assert forbidden not in show
    driver = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    advance = driver.split("void AdvanceOneSlice", 1)[1].split("void DisableLocked", 1)[0]
    assert 'ExecuteNativeCommand ("ShowSunStudy"' not in advance
    assert "PrepareSunStudyDisplay (id, revision, snapshot, options, *output, cancelled)" in driver
    assert "PollDisplay (now)" in driver
    service = (_ADDON / "NativeCommands" / "SunStudyDisplayPreparation.cpp").read_text(encoding="utf-8")
    assert "ReadDisplayRecord" in service
    assert "MainThreadGate::Get ().Post ([request, generation]" in service
    for guard in (
        "generation == s_manualGeneration",
        "request.revision",
        "request.captureStamp",
        "IsRunning ()",
        "PublishInSession",
    ):
        assert guard in service
    assert "CancelManualSunStudyDisplays" in show
    assert "ShutdownManualSunStudyDisplays ()" in driver
    for filename in ("SunStudy/SunStudyDisplayData.cpp", "ArchViz/SunStudyDisplayAssembler.cpp"):
        pure = (_ADDON / filename).read_text(encoding="utf-8")
        assert "ACAPI_" not in pure and "MainThreadGate" not in pure
    cache = (_ADDON / "SunStudy" / "SunStudyDisplayData.cpp").read_text(encoding="utf-8")
    assert "record.displayCache->resolvedSteps == progress.resolvedSteps" in cache
    assert "LitStepCount (sample)" in cache  # no repeated full SunHours vector in scatter


def test_compact_result_requests_skip_unrequested_sample_and_atlas_transfers():
    source = (_ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")
    result = source.split("class GetSunStudyResultsCommand", 1)[1].split("class CancelSunStudyCommand", 1)[0]
    assert re.search(r"error,\s*wantPositions,\s*&progress\)", result)
    assert "summaryOnly && (wantPositions || wantAtlas || wantSteps)" in result
    assert "SunStudyStore::Get ().Summary" in result
    schemas = re.findall(r'R"json\((.*?)\)json"', source.split('{ "GetSunStudyResults",', 1)[1], re.DOTALL)
    request, response = map(json.loads, schemas[:2])
    assert request["properties"]["summaryOnly"] == {"type": "boolean"}
    for field in ("minHours", "meanHours", "maxHours", "daylightHours", "fullyLit", "fullyShaded"):
        assert field in response["properties"]
        assert f'os.Add ("{field}"' in result
    display = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    assert '"preparationError":{"type":"string"}' in display
    assert '"preparing":{"type":"boolean"}' in display


def test_manual_publication_rechecks_sun_roles_and_capture_and_linearizes_record_cancellation():
    driver = (_ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    publish = driver.split("bool PublishInSession", 1)[1].split("void Disable ()", 1)[0]
    assert "IsMainThread ()" in publish
    assert "ACAPI_GeoLocation_GetPlaceSets" in publish
    assert "PlaceInputHash (place) != config.placeInputHash" in publish
    assert "binding.generation != SelectionSetStore::Get ().Generation ()" in publish
    assert "binding.revision != SelectionSetStore::Get ().Revision ()" in publish
    commands = (_ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")
    assert "record->placeInputHash = sunstudysupport::PlaceInputHash (place)" in commands
    display = (_ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    assert "snapshot->captureStamp != 0 ? snapshot->captureStamp" in display
    for filename in ("SunStudyDisplayPreparation.cpp", "SunStudyFollowerDriver.cpp"):
        source = (_ADDON / "NativeCommands" / filename).read_text(encoding="utf-8")
        assert "PublishDisplayRecord" in source
