"""Native orchestration contracts; worker/ticket races also execute in C++."""

from pathlib import Path


ADDON = Path(__file__).resolve().parents[1] / "Sources" / "AddOn"


def source(name):
    return (ADDON / name).read_text(encoding="utf-8")


def test_manual_uses_one_owned_token_for_preparation_rays_and_publication():
    runner = source("NativeCommands/SunStudyHudRunner.cpp")
    assert "PrepareCapturedSunStudy (capture->captured, gCancel)" in runner
    assert "&gCancel))" in runner
    assert "ShowSunStudyForHud (show, ticket)" in runner
    assert 'Call ("CancelSunStudy"' not in runner
    assert "ticket->cancelled.load ()" in runner
    assert "ManualSunStudyDisplayBusy ()" in runner
    assert "sunfollow::ReserveManualRun (error)" in runner
    assert "sunfollow::ReleaseManualRun ()" in runner
    assert "request.execute" not in runner  # no second preparation implementation


def test_manual_display_completion_and_cancel_share_an_atomic_fence():
    display = source("NativeCommands/SunStudyDisplayPreparation.cpp")
    assert "request.ticket->Publish (enqueue)" in display
    assert display.index("request.ticket->Publish (enqueue)") < display.index(
        "s_manualState.preparing = false"
    )
    assert "return accepted && enqueued" in display
    assert "generation == s_manualGeneration" in display


def test_explicit_follower_pause_retains_config_and_blocks_all_new_edits_until_resume():
    driver = source("NativeCommands/SunStudyFollowerDriver.cpp")
    cancel = driver.split("void CancelActiveRun ()", 1)[1].split("bool ReserveManualRun", 1)[0]
    assert "gAutoFollow = false" in cancel
    assert "s_paused = true" in cancel
    assert "++gRunGeneration" in cancel
    assert 'RetireRun ("user cancelled; following paused")' in cancel
    assert "gConfig =" not in cancel and "gFollower.Clear" not in cancel
    assert "HideOverlay" not in cancel
    tick = driver.split("void Tick ()", 1)[1].split("FollowerStats State", 1)[0]
    assert tick.index("if (!gAutoFollow || !gConfig.valid)") < tick.index("RefreshRoleSelections ()")
    resume = driver.split("bool Resume (", 1)[1].split("void Shutdown", 1)[0]
    assert "s_manualActive" in resume and "PreparationWorker ().Busy ()" in resume
    assert "s_paused = false" in resume and "gAutoFollow = true" in resume
    assert 's_stage = "cancelled"' in tick


def test_manual_status_cannot_be_overwritten_by_cancelled_progress():
    runner = source("NativeCommands/SunStudyHudRunner.cpp")
    assert 'gStatus.stage = cancelled ? "cancelled" : stage' in runner
    assert "if (!gStatus.cancelling)" in runner
    assert 'gStatus.stage = "cancelling"' in runner
    assert "gStatus.resolvedSteps = gStatus.totalSteps = 0" in runner
    assert "sun-cancel source=manual" in runner
    assert "sun-cancel source=follower" in source("NativeCommands/SunStudyFollowerDriver.cpp")
