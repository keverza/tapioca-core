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
    assert "[captured, output]" in source
    assert "PrepareCapturedSunStudy (captured, cancelled)" in source
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
