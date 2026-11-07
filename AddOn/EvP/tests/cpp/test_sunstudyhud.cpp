// The sun study section as the viewer draws it (ArchViz/DiligentHudSunStudy.cpp
// and the run form, DiligentHudSunRun.cpp): its widget IDs and the shadow
// views' hover text (#116).
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "MeshFixtures.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "NativeCommands/SunStudyFollowerDriver.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyPointProfile.hpp"
#include "SunStudy/SunStudyRustStudy.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "sunstudy_core_fixture.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <mutex>
#include <string>
#include <thread>
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
static std::shared_ptr<PeriodProfile> s_profile = std::make_shared<PeriodProfile> ();
std::shared_ptr<const PeriodProfile> LatestPeriodProfile ()
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
    int pointOpen = -1;
    SectionGui ()
    {
        geomsrv::sunstudyhud::s_status = {};
        geomsrv::sunstudyhud::s_cancels = geomsrv::sunstudyhud::s_submissions = 0;
        geomsrv::sunstudyhud::s_period = {};
        geomsrv::sunstudyhud::s_profile = std::make_shared<geomsrv::sunstudyhud::PeriodProfile> ();
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
        if (pointOpen >= 0)
            ImGui::GetStateStorage ()->SetInt (ImGui::GetID ("Study point"), pointOpen);
        DrawSunStudyHudSection (state, scene);
        runOpen = ImGui::GetStateStorage ()->GetInt (ImGui::GetID ("Run sun study")) != 0;
        ImGui::End ();
        ImGui::Render ();
    }
};

std::string InsertPointDayStudy (bool advance = true, evp::sunstudy::StudyRecord** rawRecord = nullptr)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 81;
    snapshot->meshes = { evptest::MakeBox ("roof", 0, 0, 5) };
    auto record = std::make_unique<evp::sunstudy::StudyRecord> ();
    record->snapshot = snapshot;
    record->snapshotId = snapshot->id;
    record->positions = { 0.0, 0.0, 0.0, 10.0, 0.0, 0.0 };
    record->normals = { 0.0, 0.0, 1.0, 0.0, 0.0, 1.0 };
    record->timestepMinutes = 60;
    record->occluders =
        evp::sunstudy::BuildSunStudyOccluders (*snapshot, evp::sunstudy::ResolveElementRoles (*snapshot, {}, {}, {}));
    record->traversal = std::make_shared<evp::sunstudy::CpuTraversal> (record->occluders->analysis);
    std::vector<evp::sunstudy::SunStep> steps (4);
    for (size_t index = 0; index < steps.size (); ++index) {
        steps[index].time = evp::sunstudy::TimeOfDay { 6 + int (index), 0 };
        steps[index].direction[2] = 1.0;
        steps[index].altitudeDegrees = 45.0;
    }
    record->series = evp::sunstudy::SunSeries::FromSteps (steps, 60);
    evp::sunstudy::StudyInputs inputs;
    inputs.geometryVersion = snapshot->id;
    inputs.sunVersion = record->series.Version ();
    inputs.gridVersion = 1;
    evp::sunstudy::fixture::AttachTestCore (*record, inputs);
    auto& store = evp::sunstudy::SunStudyStore::Get ();
    if (rawRecord != nullptr)
        *rawRecord = record.get ();
    const std::string id = store.Insert (std::move (record));
    if (!advance)
        return id;
    size_t advanced = 0;
    std::string error;
    return store.Advance (id, 4, 1, 0.001, 0.0, advanced, error) ? id : "";
}
} // namespace

TEST (SunStudyHud, DayArcRunsFromFirstSunOnTheLeftOverTheTopToLastSunOnTheRight)
{
    constexpr double pi = 3.14159265358979323846;
    EXPECT_NEAR (SunStudyArcAngle (360, 360, 1080), pi, 1.0e-12);
    EXPECT_NEAR (SunStudyArcAngle (720, 360, 1080), 1.5 * pi, 1.0e-12);
    EXPECT_NEAR (SunStudyArcAngle (1080, 360, 1080), 2.0 * pi, 1.0e-12);
    EXPECT_NEAR (SunStudyArcAngle (0, 360, 1080), pi, 1.0e-12); // clamped, never wrapped
    EXPECT_NEAR (SunStudyArcMinute (-1.0, 0.0, 360, 1080), 360.0, 1.0e-9);
    EXPECT_NEAR (SunStudyArcMinute (0.0, -1.0, 360, 1080), 720.0, 1.0e-9);
    EXPECT_NEAR (SunStudyArcMinute (1.0, 0.0, 360, 1080), 1080.0, 1.0e-9);
    EXPECT_NEAR (SunStudyArcMinute (-1.0, 0.5, 360, 1080), 360.0, 1.0e-9); // below: the nearer end
    EXPECT_NEAR (SunStudyArcMinute (1.0, 0.5, 360, 1080), 1080.0, 1.0e-9);
    evp::sunstudy::PointDay day;
    day.minutes = { 375, 390, 1065 };
    day.hoursPerStep = 0.25;
    double from = 0.0, to = 0.0;
    ASSERT_TRUE (SunStudyDayDomain (day, from, to));
    EXPECT_DOUBLE_EQ (from, 375.0); // non-hour first sun
    EXPECT_DOUBLE_EQ (to, 1080.0);
    EXPECT_FALSE (SunStudyDayDomain (evp::sunstudy::PointDay (), from, to));
}

TEST (SunStudyHud, TimeOfDayGradientRunsMorningDayEvening)
{
    EXPECT_EQ (SunStudyTimeOfDayRgba (0), SunStudyTimeOfDayRgba (1440));
    EXPECT_EQ (SunStudyTimeOfDayRgba (360), kAmPmMorningRgba);
    EXPECT_EQ (SunStudyTimeOfDayRgba (720), kSingleShadowLitRgba);
    EXPECT_NE (SunStudyTimeOfDayRgba (480), SunStudyTimeOfDayRgba (1020));
    EXPECT_STREQ (SunStudyDayPhase (420), "morning");
    EXPECT_STREQ (SunStudyDayPhase (720), "day");
    EXPECT_STREQ (SunStudyDayPhase (1000), "evening");
}

TEST (SunStudyHud, HoverDayLatchesForPanelAndStationaryPointReadsOnlyOnResultChanges)
{
    auto& store = evp::sunstudy::SunStudyStore::Get ();
    store.Clear ();
    const std::string id = InsertPointDayStudy ();
    ASSERT_FALSE (id.empty ());
    SectionGui gui;
    gui.scene.sunStudy.studyId = id;
    gui.scene.sunStudy.version = 11;
    gui.state.sunReadingState = 1;
    gui.state.sunReadingStudy = id;
    gui.state.sunReadingSample = 1; // open cell, unlike the roof-covered cell 0
    gui.Frame ();
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointDayReads, 1u);
    ASSERT_TRUE (gui.state.sunPointDayReady);
    EXPECT_EQ (gui.state.sunHoverDaySample, 1u);
    EXPECT_EQ (gui.state.sunPointDay.lit, (std::vector<double> { 1.0, 1.0, 1.0, 1.0 }));

    gui.state.sunReadingState = 0; // crossing into the HUD must not erase the preview
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointDayReads, 1u);
    EXPECT_TRUE (gui.state.sunPointDayReady);

    gui.state.sunPointPlaced = true; // a pinned point wins over a different hovered cell
    gui.state.sunPointAt[0] = 0.0;
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointDayReads, 2u);
    EXPECT_EQ (gui.state.sunPointDaySamples, (std::vector<size_t> { 0 }));
    EXPECT_EQ (gui.state.sunPointDay.lit, (std::vector<double> { 0.0, 0.0, 0.0, 0.0 }));

    gui.state.sunPointPlaced = false; // removing the pin restores the latched hover preview
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointDayReads, 3u);
    EXPECT_EQ (gui.state.sunPointDaySamples, (std::vector<size_t> { 1 }));
    gui.scene.sunStudy.version = 12; // a new displayed result invalidates the same selection
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointDayReads, 4u);
    store.Clear ();
}

TEST (SunStudyHud, CollapsedPointStillLatchesHoverButStaleStudyReadingIsNotRelabelled)
{
    auto& store = evp::sunstudy::SunStudyStore::Get ();
    store.Clear ();
    const std::string id = InsertPointDayStudy ();
    ASSERT_FALSE (id.empty ());
    SectionGui gui;
    gui.scene.sunStudy.studyId = id;
    gui.state.sunReadingState = 1;
    gui.state.sunReadingStudy = id;
    gui.state.sunReadingSample = 1;
    gui.pointOpen = 0;
    gui.Frame ();
    EXPECT_EQ (gui.state.sunHoverDayStudy, id);
    EXPECT_EQ (gui.state.sunHoverDaySample, 1u);
    EXPECT_EQ (gui.state.sunPointDayReads, 0u);
    gui.pointOpen = 1;
    gui.state.sunReadingState = 0;
    gui.Frame ();
    EXPECT_EQ (gui.state.sunPointDayReads, 1u);
    EXPECT_TRUE (gui.state.sunPointDayReady);

    gui.state.sunReadingState = 1; // a stale reading from the previous displayed study
    gui.state.sunReadingStudy = "another-study";
    gui.state.sunReadingSample = 0;
    gui.scene.sunStudy.studyId = "new-study";
    gui.state.sunHoverDayStudy.clear ();
    gui.state.sunHoverDaySample = SIZE_MAX;
    gui.Frame ();
    EXPECT_EQ (gui.state.sunHoverDaySample, SIZE_MAX);
    EXPECT_FALSE (gui.state.sunPointDayReady && gui.state.sunPointDayStudy == "new-study");
    store.Clear ();
}

TEST (SunStudyHud, PointDayRetriesAfterATransientComputingRead)
{
    auto& store = evp::sunstudy::SunStudyStore::Get ();
    store.Clear ();
    evp::sunstudy::StudyRecord* raw = nullptr;
    const std::string id = InsertPointDayStudy (false, &raw);
    ASSERT_FALSE (id.empty ());
    ASSERT_NE (raw, nullptr);
    SectionGui gui;
    gui.scene.sunStudy.studyId = id;
    gui.state.sunReadingState = 1;
    gui.state.sunReadingStudy = id;
    gui.state.sunReadingSample = 1;

    std::unique_lock<std::timed_mutex> hold (raw->sessionMutex);
    size_t advanced = 0;
    std::string advanceError;
    std::thread thread ([&] { store.Advance (id, 4, 1, 0.001, 0.0, advanced, advanceError); });
    evp::sunstudy::PointDay probe;
    std::string error;
    bool computing = false;
    for (int attempt = 0; attempt < 10000; ++attempt) {
        if (!store.PointDayOf (id, { 1 }, probe, error) && error == "computing") {
            computing = true;
            break;
        }
        std::this_thread::yield ();
    }
    if (!computing) {
        hold.unlock ();
        thread.join ();
    }
    ASSERT_TRUE (computing) << error;
    gui.Frame (); // the advancing store refuses; this failed read must not become a cache key
    EXPECT_EQ (gui.state.sunPointDayReads, 1u);
    EXPECT_FALSE (gui.state.sunPointDayReady);
    hold.unlock ();
    thread.join ();
    ASSERT_TRUE (advanceError.empty ()) << advanceError;
    gui.Frame (); // identical selection/version now succeeds after Advance ends
    EXPECT_EQ (gui.state.sunPointDayReads, 2u);
    EXPECT_TRUE (gui.state.sunPointDayReady);
    store.Clear ();
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

TEST (SunStudyHud, ArcClickSelectsAStepDragSelectsASliceAndOffBarsClears)
{
    SectionGui gui;
    evp::sunstudy::PointDay day;
    day.minutes = { 480, 540 };
    day.lit = { 1.0, 0.0 };
    day.facing = { 1.0, 1.0 };
    day.samples = 1;
    day.hoursPerStep = 1.0; // domain 08:00-10:00
    ImVec2 origin {};
    float width = 0.0f, line = 0.0f;
    int vertices = 0;
    SunStudyClockShown shown { 0.0, 1440.0 };
    const auto frame = [&] (ImVec2 pointer, bool down, SunStudyClockSelection& selection) {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 400, 400 });
        ImGui::Begin ("clock", nullptr, ImGuiWindowFlags_NoSavedSettings);
        origin = ImGui::GetCursorScreenPos ();
        width = ImGui::GetContentRegionAvail ().x;
        line = ImGui::GetTextLineHeight ();
        DrawSunStudyDayClock (gui.state, &day, "##testclock", -1, "", &selection, 220.0f, &shown);
        vertices = ImGui::GetWindowDrawList ()->VtxBuffer.Size;
        ImGui::End ();
        ImGui::Render ();
    };
    const auto atMinute = [&] (double minute, float radiusShare = 0.265f) {
        const float size = (std::min) (220.0f, width);
        const ImVec2 centre (origin.x + width * 0.5f, origin.y + size * 0.46f + line * 0.9f);
        const double angle = SunStudyArcAngle (minute, 480.0, 600.0);
        return ImVec2 (centre.x + size * radiusShare * float (std::cos (angle)),
                       centre.y + size * radiusShare * float (std::sin (angle)));
    };
    SunStudyClockSelection selection;
    frame ({ -100, -100 }, false, selection); // establish the chart rectangle
    const int unhoveredVertices = vertices;
    const ImVec2 step = atMinute (570);
    frame (step, false, selection);
    EXPECT_GT (vertices, unhoveredVertices); // the hovered step is outlined immediately
    frame (step, true, selection);
    frame (step, false, selection);
    EXPECT_EQ (selection.step, 1);
    EXPECT_FALSE (selection.slice);
    EXPECT_FALSE (gui.state.sunClockBrushActive);

    // Right to left is a slice too: the arc never wraps.
    const ImVec2 from = atMinute (585), to = atMinute (495);
    frame (from, false, selection);
    frame (from, true, selection);
    frame (to, true, selection);
    frame (to, false, selection);
    EXPECT_EQ (selection.step, -1);
    EXPECT_TRUE (selection.slice);
    EXPECT_NEAR (selection.fromMinute, 495.0, 2.0);
    EXPECT_NEAR (selection.toMinute, 585.0, 2.0);
    EXPECT_FALSE (gui.state.sunClockBrushActive);

    // The committed slice stays drawn without a gesture.
    frame ({ -100, -100 }, false, selection);
    const int plain = vertices;
    shown = { 495.0, 585.0 };
    frame ({ -100, -100 }, false, selection);
    EXPECT_GT (vertices, plain);

    // A click inside the diagram, off the bars, clears it.
    const ImVec2 off = atMinute (540, 0.05f);
    frame (off, false, selection);
    frame (off, true, selection);
    EXPECT_TRUE (selection.clear);
    frame (off, false, selection);
    EXPECT_FALSE (gui.state.sunClockBrushActive);
}

TEST (SunStudyHud, DaySelectionSyncsTheSliderAndClearRestoresTheDay)
{
    SectionGui gui;
    gui.state.sunStepMinutes = { 480, 540, 600, 660 };
    gui.state.sunStep = 0;
    SunStudyClockSelection slice;
    slice.slice = true;
    slice.fromMinute = 530.0;
    slice.toMinute = 650.0;
    ApplySunStudyDaySelection (gui.state, slice);
    EXPECT_EQ (gui.state.sunStep, 1); // the slider moves into the slice
    SunStudyClockSelection inside;
    inside.step = 2;
    ApplySunStudyDaySelection (gui.state, inside);
    EXPECT_FLOAT_EQ (gui.state.sunTimeTo, float (650.0 / 60.0)); // a step inside keeps the slice
    SunStudyClockSelection outside;
    outside.step = 3;
    ApplySunStudyDaySelection (gui.state, outside);
    EXPECT_FLOAT_EQ (gui.state.sunTimeFrom, 0.0f);
    EXPECT_FLOAT_EQ (gui.state.sunTimeTo, 24.0f);
    ApplySunStudyDaySelection (gui.state, slice);
    SunStudyClockSelection clear;
    clear.clear = true;
    ApplySunStudyDaySelection (gui.state, clear);
    EXPECT_FLOAT_EQ (gui.state.sunTimeFrom, 0.0f);
    EXPECT_FLOAT_EQ (gui.state.sunTimeTo, 24.0f);
}

TEST (SunStudyHud, ClockGesturesNeverChangeTheCurrentView)
{
    SectionGui gui;
    for (const int view : { 0, 1, 2, 3, 4, kSunObstructionView }) {
        gui.state.sunView = view;
        gui.state.sunFanInterval = 5;
        SunStudyClockSelection click;
        click.step = 1;
        ApplySunStudyDaySelection (gui.state, click);
        EXPECT_EQ (gui.state.sunView, view);
        EXPECT_EQ (gui.state.sunStep, 1);
        EXPECT_EQ (gui.state.sunFanInterval, 5);
        SunStudyClockSelection slice;
        slice.slice = true;
        slice.fromMinute = 480.0;
        slice.toMinute = 600.0;
        ApplySunStudyDaySelection (gui.state, slice);
        EXPECT_EQ (gui.state.sunView, view);
        EXPECT_EQ (gui.state.sunFanInterval, 5);
        EXPECT_FLOAT_EQ (gui.state.sunTimeFrom, 8.0f);
        EXPECT_FLOAT_EQ (gui.state.sunTimeTo, 10.0f);
    }
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

    auto& profile = *geomsrv::sunstudyhud::s_profile;
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
    // sunView / sunFanInterval for roles, single, morning/evening, fan and obstruction.
    const struct {
        int view, fan, mode;
    } views[] = { { 4, 5, 4 }, { 2, 5, 5 }, { 3, 5, 6 }, { 3, 2, 7 }, { kSunObstructionView, 5, 5 } };
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
