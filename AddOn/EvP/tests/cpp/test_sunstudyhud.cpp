// The sun study section as the viewer draws it (ArchViz/DiligentHudSunStudy.cpp
// and the run form, DiligentHudSunRun.cpp): its widget IDs and the shadow
// views' hover text (#116).
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "NativeCommands/SunStudyFollowerDriver.hpp"
#include "SunStudy/SunStudyPointProfile.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <string>
#include <vector>

// The run service is the add-on's; the section only asks it for a status.
namespace geomsrv::sunstudyhud {
static Status s_status;
static int s_cancels = 0;
static int s_submissions = 0;
bool Submit (const Request&, std::string&)
{
    ++s_submissions;
    return true;
}
void Cancel ()
{
}
Status Current ()
{
    return s_status;
}
Status CurrentJob ()
{
    return s_status;
}
void CancelActive ()
{
    ++s_cancels;
}
bool ResumeFollowing (std::string&)
{
    return true;
}
void Shutdown ()
{
}
// The retained period (#152): a test sets what CurrentPeriod reports and reads
// what the panel asked to show.
static PeriodInfo s_period;
static PeriodView s_shownView;
static int s_viewRequests = 0;
static int s_cacheClears = 0;
PeriodInfo CurrentPeriod ()
{
    return s_period;
}
bool ShowPeriodView (const PeriodView& view, std::string&)
{
    s_shownView = view;
    ++s_viewRequests;
    return true;
}
std::shared_ptr<const std::vector<KeptDay>> KeptPeriodDays ()
{
    return nullptr;
}
bool ReadPeriodPoints (const std::vector<size_t>&, PeriodPoint& point)
{
    for (int m = 0; m < 12; ++m) {
        point.unoccluded[m] = 31.0 * 2.0;
        point.facing[m] = 31.0 * 4.0;
    }
    return true;
}
void RequestPeriodProfile (const std::vector<size_t>&)
{
}
static PeriodProfile s_profile;
PeriodProfile LatestPeriodProfile ()
{
    return s_profile;
}
bool ClearPeriodCache (std::string&)
{
    ++s_cacheClears;
    s_period = {};
    return true;
}
} // namespace geomsrv::sunstudyhud

using namespace geomsrv::archviz;

namespace {
struct SectionGui {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    HudState state;
    DiligentSceneStats scene;
    bool collapsed = false;
    bool runOpen = false;
    SectionGui ()
    {
        geomsrv::sunstudyhud::s_status = {};
        geomsrv::sunstudyhud::s_cancels = geomsrv::sunstudyhud::s_submissions = 0;
        geomsrv::sunstudyhud::s_period = {};
        geomsrv::sunstudyhud::s_profile = {};
        geomsrv::sunstudyhud::s_shownView = {};
        geomsrv::sunstudyhud::s_viewRequests = 0;
        geomsrv::sunstudyhud::s_cacheClears = 0;
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 500, 1400 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
        SunStudyOverlayStatus& study = scene.sunStudy;
        study.studyId = "sun-1";
        study.drawing = true;
        study.stepCount = 4;
        study.noonStep = 2;
        study.stepMinutes = { 480, 540, 600, 660 };
        study.quantumHours = 1.0f;
    }
    ~SectionGui ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    void Frame (ImVec2 pointer = { -1, -1 }, bool down = false)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 400, 1350 });
        ImGui::Begin ("sun study", nullptr, ImGuiWindowFlags_NoSavedSettings);
        // The run form open beside a study on screen: its Roles node and the
        // view selector's Roles button in one window, as the live HUD has them.
        ImGui::GetStateStorage ()->SetInt (ImGui::GetID ("Run sun study"), collapsed ? 0 : 1);
        DrawSunStudyHudSection (state, scene);
        runOpen = ImGui::GetStateStorage ()->GetInt (ImGui::GetID ("Run sun study")) != 0;
        ImGui::End ();
        ImGui::Render ();
    }
};
} // namespace

TEST (SunStudyHud, DayClockPutsBothMidnightsAtTheTopAndRunsClockwise)
{
    constexpr double pi = 3.14159265358979323846;
    EXPECT_NEAR (SunStudyClockAngle (0), -pi / 2.0, 1.0e-12);
    EXPECT_NEAR (SunStudyClockAngle (6 * 60), 0.0, 1.0e-12);
    EXPECT_NEAR (SunStudyClockAngle (12 * 60), pi / 2.0, 1.0e-12);
    EXPECT_NEAR (SunStudyClockAngle (18 * 60), pi, 1.0e-12);
    EXPECT_NEAR (SunStudyClockAngle (24 * 60), 3.0 * pi / 2.0, 1.0e-12);
    EXPECT_NEAR (SunStudyClockMinute (0.0, -1.0), 0.0, 1.0e-12);
    EXPECT_NEAR (SunStudyClockMinute (1.0, 0.0), 360.0, 1.0e-12);
    EXPECT_NEAR (SunStudyClockMinute (0.0, 1.0), 720.0, 1.0e-12);
    EXPECT_NEAR (SunStudyClockMinute (-1.0, 0.0), 1080.0, 1.0e-12);
}

TEST (SunStudyHud, PeriodPointClockKeepsMeanStepsAndExactCellRange)
{
    geomsrv::sunstudyhud::PeriodProfile profile;
    profile.samples = { 0, 1 };
    profile.dayHoursPerStep = { 0.25 };
    profile.dayMinutes = { { 480, 495 } };
    profile.dayStepLit = { { 0.5, 1.0 } };
    profile.dayStepFacing = { { 1.0, 1.0 } };
    profile.dayMinLit = { 0.0 };
    profile.dayMaxLit = { 0.25 };
    const auto day = geomsrv::sunstudyhud::PeriodPointDay (profile, 0);
    EXPECT_EQ (day.samples, 2u);
    EXPECT_EQ (day.lit, (std::vector<double> { 0.5, 1.0 }));
    EXPECT_EQ (day.facing, (std::vector<double> { 1.0, 1.0 }));
    EXPECT_DOUBLE_EQ (day.minDirectHours, 0.0);
    EXPECT_DOUBLE_EQ (day.maxDirectHours, 0.25);
}

TEST (SunStudyHud, DisplayOnlyClockDoesNotMutateAnotherClocksBrush)
{
    SectionGui gui;
    evp::sunstudy::PointDay day;
    day.minutes = { 480 };
    day.lit = day.facing = { 1.0 };
    day.samples = 1;
    ImGui::NewFrame ();
    ImGui::Begin ("season", nullptr, ImGuiWindowFlags_NoSavedSettings);
    gui.state.sunClockBrushActive = true;
    gui.state.sunClockBrushId = 123;
    gui.state.sunClockBrushFrame = ImGui::GetFrameCount () - 1;
    DrawSunStudyDayClock (gui.state, &day, "##displayonly", -1, "", nullptr);
    EXPECT_TRUE (gui.state.sunClockBrushActive);
    EXPECT_EQ (gui.state.sunClockBrushId, 123u);
    ImGui::End ();
    ImGui::Render ();
}

TEST (SunStudyHud, ClockClickSelectsAStepAndClockwiseDragSelectsASlice)
{
    SectionGui gui;
    evp::sunstudy::PointDay day;
    day.minutes = { 480, 540 };
    day.lit = { 1.0, 0.0 };
    day.facing = { 1.0, 1.0 };
    day.samples = 1;
    day.hoursPerStep = 1.0;
    ImVec2 itemMin {}, itemMax {};
    const auto frame = [&] (ImVec2 pointer, bool down, SunStudyClockSelection& selection) {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 400, 400 });
        ImGui::Begin ("clock", nullptr, ImGuiWindowFlags_NoSavedSettings);
        DrawSunStudyDayClock (gui.state, &day, "##testclock", -1, "", &selection);
        ImGui::End ();
        const ImGuiWindow* window = ImGui::FindWindowByName ("clock");
        itemMin = window->ContentRegionRect.Min;
        itemMax = ImVec2 (window->ContentRegionRect.Max.x, itemMin.y + 220.0f);
        ImGui::Render ();
    };
    const auto atMinute = [&] (double minute) {
        const double angle = SunStudyClockAngle (minute);
        const float radius = (itemMax.y - itemMin.y) * 0.27f;
        return ImVec2 ((itemMin.x + itemMax.x) * 0.5f + radius * float (std::cos (angle)),
                       itemMin.y + (itemMax.y - itemMin.y) * 0.5f + radius * float (std::sin (angle)));
    };
    SunStudyClockSelection selection;
    frame ({ -100, -100 }, false, selection); // establish the chart rectangle
    const ImVec2 step = atMinute (540);
    frame (step, false, selection);
    frame (step, true, selection);
    frame (step, false, selection);
    EXPECT_EQ (selection.step, 1);
    EXPECT_FALSE (selection.slice);
    EXPECT_FALSE (gui.state.sunClockBrushActive);

    const ImVec2 from = atMinute (480), to = atMinute (540);
    frame (from, false, selection);
    frame (from, true, selection);
    frame (to, true, selection);
    frame (to, false, selection);
    EXPECT_EQ (selection.step, -1);
    EXPECT_TRUE (selection.slice);
    EXPECT_NEAR (selection.fromMinute, 480.0, 2.0);
    EXPECT_NEAR (selection.toMinute, 540.0, 2.0);
    EXPECT_FALSE (gui.state.sunClockBrushActive);

    const ImVec2 late = atMinute (1380), early = atMinute (60);
    frame (late, false, selection);
    frame (late, true, selection);
    frame (early, true, selection);
    frame (early, false, selection);
    EXPECT_TRUE (selection.slice);
    EXPECT_NEAR (selection.fromMinute, 1380.0, 2.0);
    EXPECT_EQ (selection.toMinute, 1440.0); // explicitly constrained, not wrapped into another range
}

TEST (SunStudyHud, TimelineLabelsObserveAutomaticAdvanceAndLoopInTheSameFrame)
{
    SectionGui gui;
    int index = 0, observed = -1;
    HudTimeline play;
    play.playing = true;
    play.playedAt = -10.0;
    play.loop = false;
    const auto frame = [&] {
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 400, 200 });
        ImGui::Begin ("timeline", nullptr, ImGuiWindowFlags_NoSavedSettings);
        DrawSunStudyTimeline ("test", index, 0, 2, play, 0.1, [&] {
            observed = index;
            return std::to_string (index);
        });
        ImGui::End ();
        ImGui::Render ();
    };
    frame ();
    EXPECT_EQ (index, 1);
    EXPECT_EQ (observed, 1);
    index = 2;
    play.playing = true;
    play.playedAt = -10.0;
    play.loop = true;
    frame ();
    EXPECT_EQ (index, 0);
    EXPECT_EQ (observed, 0);
}

TEST (SunStudyHud, DetailedPointPlaybackNeedsNoWholeMapDayArchive)
{
    SectionGui gui;
    auto& period = geomsrv::sunstudyhud::s_period;
    period.available = true;
    period.studyId = "sun-1";
    period.representedDays = 3.0;
    period.monthDays[2] = period.monthDays[3] = period.monthDays[4] = 1.0;
    period.siteDaylight[2] = period.siteDaylight[3] = 2.0;
    gui.state.sunPeriodStudyId = "sun-1";
    gui.state.sunPeriodProfile = true;
    gui.state.sunPointPlaced = true;
    gui.state.sunPointSamples = { 0 };
    gui.state.sunPointSamplesFor = "sun-1|0.000";

    auto& profile = geomsrv::sunstudyhud::s_profile;
    profile.samples = { 0 };
    profile.ready = true;
    profile.dayMonth = { 3, 4, 5 };
    profile.dayOfMonth = { 15, 15, 15 };
    profile.dayUp = { 2.0, 2.0, 0.0 };
    profile.dayFacing = { 2.0, 2.0, 0.0 };
    profile.dayLit = { 1.0, 1.0, 0.0 };
    profile.dayHoursPerStep = { 1.0, 1.0, 1.0 };
    profile.dayMinutes = { { 480, 540 }, { 480, 540 }, {} };
    profile.dayStepFacing = { { 1.0, 1.0 }, { 1.0, 1.0 }, {} };
    profile.dayStepLit = { { 1.0, 0.0 }, { 0.0, 1.0 }, {} };

    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointPlayDay, 0);
    EXPECT_EQ (gui.state.sunPointPlayStep, 0);
    gui.state.sunPointPlay.playing = true;
    gui.state.sunPointPlay.playedAt = -10.0;
    gui.state.sunPointPlay.loop = false;
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointPlayDay, 0);
    EXPECT_EQ (gui.state.sunPointPlayStep, 1);
    gui.state.sunPointPlay.playing = false;
    gui.state.sunPointPlayMode = 1; // fixed hour
    gui.state.sunPointPlayClock = 540;
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointPlayStep, 1);
    gui.state.sunPointPlayClock = 720; // after this two-step day's measured sun
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointPlayStep, -1);
    gui.state.sunPointPlayMode = 2; // day by day
    gui.state.sunPointPlayDay = 1;
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointPlayDay, 1);
    EXPECT_EQ (gui.state.sunPointPlayStep, -1);
    gui.state.sunPointPlayMode = 0;
    gui.state.sunPointPlayDay = 2; // a trailing sunless day has no step frame
    gui.state.sunPointPlayStep = 0;
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointPlayDay, 1);
    EXPECT_EQ (gui.state.sunPointPlayStep, 1);
    gui.state.sunPointPlayFrom = 500.0; // a clock-brushed slice excludes 08:00, keeps 09:00
    gui.state.sunPointPlayTo = 600.0;
    gui.state.sunPointPlayDay = 0;
    gui.state.sunPointPlayStep = -1;
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointPlayDay, 0);
    EXPECT_EQ (gui.state.sunPointPlayStep, 1);
    gui.state.sunPointPlay.playing = true;
    gui.state.sunPointPlay.playedAt = -10.0;
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointPlayDay, 1);
    EXPECT_EQ (gui.state.sunPointPlayStep, 1);
    EXPECT_FALSE (period.daysKept);
    EXPECT_EQ (geomsrv::sunstudyhud::s_shownView.frame, 0); // the map stayed on its totals
    gui.state.sunPeriodSeasons = true; // display-only small multiples do not alter the map or point selection
    gui.Frame ();
    EXPECT_EQ (geomsrv::sunstudyhud::s_shownView.frame, 0);
    EXPECT_EQ (gui.state.sunPointPlayDay, 1);
    EXPECT_EQ (gui.state.sunPointPlayStep, 1);
}

TEST (SunStudyHud, FreePeriodCacheKeepsTheFrameButRetiresBrowsingAndPlayback)
{
    SectionGui gui;
    auto& status = geomsrv::sunstudyhud::s_status;
    status.stage = "done";
    status.period = true;
    status.studyId = "sun-cache";
    gui.scene.sunStudy.studyId = "sun-cache";
    auto& period = geomsrv::sunstudyhud::s_period;
    period.available = true;
    period.studyId = "sun-cache";
    period.monthDays[3] = 30.0;
    period.siteDaylight[3] = 360.0;
    gui.state.sunPeriodProfile = true;
    gui.state.sunPeriodQuantity = 3;
    gui.state.sunPeriodMonth = 4;
    gui.state.sunPeriodPresetsFor = 3;
    gui.state.sunGradientPreset = "cividis";
    gui.state.sunPeriodPlay.playing = true;
    gui.state.sunPointPlay.playing = true;
    gui.state.sunClockBrushActive = gui.state.sunClockBrushDragged = true;
    gui.state.sunClockBrushId = 42;
    gui.state.sunClockBrushFrame = 10;
    gui.Frame ();
    gui.Frame ();

    // Activate the real SmallButton, wherever the fixed run-result lines put it.
    for (float y = 20; y < 500 && geomsrv::sunstudyhud::s_cacheClears == 0; y += 6)
        for (float x = 20; x < 390 && geomsrv::sunstudyhud::s_cacheClears == 0; x += 12) {
            gui.Frame ({ x, y });
            gui.Frame ({ x, y }, true);
            gui.Frame ({ x, y }, false);
        }
    ASSERT_EQ (geomsrv::sunstudyhud::s_cacheClears, 1);
    EXPECT_EQ (gui.state.sunPeriodStudyId, "sun-cache"); // displayed-period identity survives
    EXPECT_TRUE (gui.state.sunShowsPeriod);
    EXPECT_FALSE (gui.state.sunPeriodProfile);
    EXPECT_FALSE (gui.state.sunPeriodPlay.playing);
    EXPECT_FALSE (gui.state.sunPointPlay.playing);
    EXPECT_FALSE (gui.state.sunClockBrushActive);
    EXPECT_FALSE (gui.state.sunClockBrushDragged);
    EXPECT_EQ (gui.state.sunClockBrushId, 0u);
    EXPECT_EQ (gui.state.sunClockBrushFrame, -1);
    EXPECT_EQ (gui.state.sunPeriodQuantity, 3);
    EXPECT_EQ (gui.state.sunPeriodMonth, 4);
    EXPECT_EQ (gui.state.sunGradientPreset, "cividis");
    EXPECT_EQ (SunLegendUnit (), SunValueUnit::Share);
    gui.Frame ();
    EXPECT_TRUE (gui.state.sunShowsPeriod); // retired frame is not reclassified as a day study
    EXPECT_EQ (gui.state.sunPeriodQuantity, 3);
    EXPECT_EQ (SunLegendUnit (), SunValueUnit::Share);
}

TEST (SunStudyHud, CollapsedSetupStillOffersCancelForManualAndFollowerStages)
{
    for (bool following : { false, true })
        for (const char* stage :
             { "queued", "waiting", "snapshot", "starting", "preparing", "calculating", "displaying", "showing" }) {
            SectionGui gui;
            gui.collapsed = true;
            auto& status = geomsrv::sunstudyhud::s_status;
            status.running = true;
            status.following = following;
            status.stage = stage;
            status.totalSteps = 50;
            status.resolvedSteps = 10;
            gui.Frame ();
            gui.Frame ({ 200, 40 });
            gui.Frame ({ 200, 40 }, true);
            gui.Frame ({ 200, 40 }, false);
            EXPECT_EQ (geomsrv::sunstudyhud::s_cancels, 1) << stage;
            EXPECT_EQ (geomsrv::sunstudyhud::s_submissions, 0);
            EXPECT_TRUE (gui.state.sunRunBusy);
            EXPECT_FALSE (gui.runOpen);
        }
}

TEST (SunStudyHud, CancellingDisablesThePrimaryActionAndAcknowledgementReopensSetup)
{
    SectionGui gui;
    gui.collapsed = true;
    auto& status = geomsrv::sunstudyhud::s_status;
    status.running = status.cancelling = true;
    status.stage = "cancelling";
    gui.Frame ();
    gui.Frame ({ 200, 40 }, true);
    gui.Frame ({ 200, 40 });
    EXPECT_EQ (geomsrv::sunstudyhud::s_cancels, 0);
    EXPECT_EQ (geomsrv::sunstudyhud::s_submissions, 0);
    status.running = status.cancelling = false;
    status.stage = "cancelled";
    gui.Frame ();
    EXPECT_FALSE (gui.state.sunRunBusy);
    EXPECT_TRUE (gui.runOpen);
}

TEST (SunStudyHud, ManualOwnershipAlwaysWinsAndCancelledFollowerProgressIsCleared)
{
    using namespace geomsrv;
    sunstudyhud::Status manual;
    manual.running = true;
    manual.stage = "calculating";
    manual.resolvedSteps = 3;
    manual.totalSteps = 50;
    sunfollow::FollowerStats follower;
    follower.autoFollow = follower.workerBusy = true;
    follower.stage = "preparing";
    follower.totalSteps = 99;
    auto status = sunstudyhud::JobStatusOf (manual, follower);
    EXPECT_FALSE (status.following);
    EXPECT_EQ (status.stage, "calculating");
    EXPECT_EQ (status.totalSteps, 50u);
    manual.running = false;
    status = sunstudyhud::JobStatusOf (manual, follower);
    EXPECT_TRUE (status.following);
    EXPECT_EQ (status.stage, "preparing");
    follower.autoFollow = follower.workerBusy = false;
    follower.paused = true;
    follower.stage = "cancelled";
    status = sunstudyhud::JobStatusOf (manual, follower);
    EXPECT_FALSE (status.running);
    EXPECT_TRUE (status.paused);
    EXPECT_EQ (status.totalSteps, 0u);
}

TEST (SunStudyHud, FailedFollowerOffersRetryRatherThanAnUnendingCancel)
{
    geomsrv::sunfollow::FollowerStats follower;
    follower.autoFollow = follower.dirty = true;
    follower.stage = "failed";
    follower.state = evp::sunstudy::SunStudyFollowState::Failed;
    const auto status = geomsrv::sunstudyhud::JobStatusOf ({}, follower);
    EXPECT_FALSE (status.running);
    EXPECT_EQ (status.stage, "failed");
}

TEST (SunStudyHud, ShadowAndRoleViewsSubmitNoWidgetIdTwice)
{
    // sunView / sunFanInterval for the roles, single, morning / evening and fan views.
    const struct {
        int view, fan, mode;
    } views[] = { { 4, 5, 4 }, { 2, 5, 5 }, { 3, 5, 6 }, { 3, 2, 7 } };
    for (const auto& chosen : views) {
        SectionGui gui;
        gui.state.sunSeenCommandedMode = 0; // the command already seen: the choice stands
        gui.state.sunView = chosen.view;
        gui.state.sunFanInterval = chosen.fan;
        gui.Frame ();
        gui.Frame ();
        size_t hovered = 0;
        for (float y = 8; y < 1300; y += 6)
            for (float x : { 20.0f, 60.0f, 140.0f, 260.0f, 360.0f }) {
                gui.Frame ({ x, y });
                if (gui.context->HoveredId == 0)
                    continue;
                ++hovered;
                gui.Frame ({ x, y });
                EXPECT_LE (gui.context->HoveredIdPreviousFrameItemCount, 1)
                    << "mode " << chosen.mode << " at " << x << ", " << y;
            }
        EXPECT_GT (hovered, 0u) << "mode " << chosen.mode;
    }
}

TEST (SunStudyHud, ShadowRunsMergeConsecutiveStepsAndEndAQuantumLater)
{
    const std::vector<uint16_t> minutes = { 480, 495, 510, 525, 540, 555 };
    const std::vector<uint8_t> lit = { 0, 0, 1, 0, 1, 0 };
    const auto runs = SunStudyShadowRuns (lit, minutes, 0, 6, 15);
    ASSERT_EQ (runs.size (), 3u);
    EXPECT_EQ (runs[0].from, 480u);
    EXPECT_EQ (runs[0].to, 510u);
    EXPECT_EQ (runs[2].from, 555u);
    EXPECT_EQ (runs[2].to, 570u);
    EXPECT_EQ (SunStudyShadowTimes (runs, 480, 570), "Shadow 08:00-08:30, 08:45-09:00, 09:15-09:30");
    EXPECT_EQ (SunStudyShadowTimes (runs, 480, 570, 2), "Shadow 08:00-08:30, 08:45-09:00, +1 more");
    // The views' Hours range: only steps in [first, end) count.
    EXPECT_EQ (SunStudyShadowTimes (SunStudyShadowRuns (lit, minutes, 2, 3, 15), 510, 525), "No shadow");
    EXPECT_EQ (SunStudyShadowTimes (SunStudyShadowRuns (lit, minutes, 0, 2, 15), 480, 510),
               "Shadow all of 08:00-08:30");
    EXPECT_EQ (SunStudyShadowedSteps (lit, 0, 3), 2u);
    EXPECT_EQ (SunStudyShadowedSteps (lit, 3, 6), 2u);
    // Fewer flags than steps (a study still converging) never reads past them.
    EXPECT_EQ (SunStudyShadowRuns ({ 0 }, minutes, 0, 6, 15).size (), 1u);
}

TEST (SunStudyHud, TheOverlapSwatchIsMorningAndEveningInEqualShares)
{
    // Rounded halves of 0x796AB1 and 0xDF9792: the legend's old 0xAC80A2 to a step.
    EXPECT_EQ (SunStudyAmPmOverlapRgba (kAmPmMorningRgba, kAmPmEveningRgba), 0xAC81A2FFu);
    EXPECT_EQ (SunStudyAmPmOverlapRgba (0xFF000080u, 0x00FF0010u) & 0xffu, 0xffu) << "opaque";
}

// #152: a finished period on screen offers its views; each quantity asks the
// runner once, shares are stretched over the ramp and read as percentages,
// and the colours follow the quantity and come back with it.
TEST (SunStudyHud, APeriodViewAsksOnceAndSharesReadAsPercent)
{
    SectionGui gui;
    auto& status = geomsrv::sunstudyhud::s_status;
    status.stage = "done";
    status.period = true;
    status.studyId = "sun-1";
    auto& period = geomsrv::sunstudyhud::s_period;
    period.available = true;
    period.studyId = "sun-1";
    period.representedDays = 365.0;
    for (int m = 0; m < 12; ++m) {
        period.monthDays[m] = 30.0;
        period.siteDaylight[m] = 30.0 * 12.0;
    }
    period.edges[3] = 2.0;
    gui.Frame ();
    gui.Frame ();
    EXPECT_TRUE (gui.state.sunShowsPeriod);
    EXPECT_EQ (geomsrv::sunstudyhud::s_viewRequests, 0); // the applied view is the default
    const std::string direct = gui.state.sunGradientPreset;

    gui.state.sunPeriodQuantity = 3; // as the Access button does
    gui.state.sunPeriodPresetsFor = 0;
    gui.state.sunGradientPreset = gui.state.sunPeriodPresets[3];
    gui.state.sunPeriodPresetsFor = 3;
    gui.Frame ();
    gui.Frame ();
    EXPECT_EQ (geomsrv::sunstudyhud::s_viewRequests, 1);
    EXPECT_EQ (geomsrv::sunstudyhud::s_shownView.quantity, 3);
    EXPECT_DOUBLE_EQ (geomsrv::sunstudyhud::s_shownView.scale, double (kSunRampTop));
    EXPECT_EQ (SunLegendUnit (), SunValueUnit::Share);
    EXPECT_EQ (SunStudyValueText (5.0), "50%");

    // Leaving the period: hours again, and the direct-sun colours back.
    status.period = false;
    status.studyId = "sun-2";
    period.available = false;
    gui.Frame ();
    gui.Frame ();
    EXPECT_FALSE (gui.state.sunShowsPeriod);
    EXPECT_EQ (SunLegendUnit (), SunValueUnit::Hours);
    EXPECT_EQ (gui.state.sunGradientPreset, direct);
}
