#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/SunPathSync.hpp"
#include "ArchViz/SunPathBandDraw.hpp"
#include "Geometry/MeshStore.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "MeshFixtures.hpp"
#include "SunStudyHudTestServices.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <functional>

namespace geomsrv::sunstudyhud {
static std::shared_ptr<SunPath> s_path;
static std::shared_ptr<const SunPathPeriodData> s_pathPeriod;
static int s_pathRequests = 0;
static std::vector<std::array<int, 3>> s_pathRequestDates;
void RequestSunPath (int year, int month, int day)
{
    ++s_pathRequests;
    s_pathRequestDates.push_back ({ year, month, day });
}
std::shared_ptr<const SunPath> LatestSunPath ()
{
    return s_path;
}
std::shared_ptr<const SunPathPeriodData> LatestSunPathPeriod ()
{
    return s_pathPeriod;
}
} // namespace geomsrv::sunstudyhud

namespace tapioca::test {
void SetSunPathTestCapture (std::shared_ptr<geomsrv::sunstudyhud::SunPath> path)
{
    geomsrv::sunstudyhud::s_path = std::move (path);
    geomsrv::sunstudyhud::s_pathRequestDates.clear ();
}

void SetSunPathTestPeriod (std::shared_ptr<const geomsrv::sunstudyhud::SunPathPeriodData> period)
{
    geomsrv::sunstudyhud::s_pathPeriod = std::move (period);
}

const std::vector<std::array<int, 3>>& SunPathTestCaptureDates ()
{
    return geomsrv::sunstudyhud::s_pathRequestDates;
}
} // namespace tapioca::test

using namespace geomsrv::archviz;
using namespace geomsrv::sunstudyhud;

namespace {
struct SunPathGui : testing::Test {
    ImGuiContext* previous = nullptr;
    std::shared_ptr<const geomsrv::Snapshot> previousMesh;
    HudState state;
    float matrix[16] = { 0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0.3f, 1 };

    void SetUp () override
    {
        previous = ImGui::GetCurrentContext ();
        ImGui::CreateContext ();
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 800, 600 };
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
        previousMesh = geomsrv::MeshStore::Get ().Current ();
        auto mesh = std::make_shared<geomsrv::Snapshot> ();
        mesh->id = geomsrv::MeshStore::Get ().NextId ();
        mesh->meshes = { evptest::MakeBox ("scene", 0, 0, 0) };
        geomsrv::MeshStore::Get ().Publish (mesh);
        s_path = std::make_shared<SunPath> ();
        s_path->ready = true;
        s_path->year = 2028;
        s_path->month = 2;
        s_path->day = 29;
        s_pathPeriod.reset ();
        s_pathRequests = 0;
        state.sunPathShown = true;
        state.sunPathDay.available = true;
        state.sunPathDay.studyId = "shown";
        state.sunPathDay.revision = 17;
        state.sunPathDay.year = 2028;
        state.sunPathDay.month = 2;
        state.sunPathDay.day = 29;
        evp::sunstudy::SunStep early, late;
        early.time = { 10, 5 };
        early.direction[0] = 0.6;
        early.direction[2] = 0.8;
        late.time = { 11, 5 };
        late.direction[1] = 0.8;
        late.direction[2] = 0.6;
        state.sunPathDay.steps = { early, late };
        state.sunStep = 0;
    }

    void TearDown () override
    {
        s_path.reset ();
        s_pathPeriod.reset ();
        geomsrv::MeshStore::Get ().Publish (previousMesh);
        ImGui::DestroyContext ();
        ImGui::SetCurrentContext (previous);
    }

    void Draw (ImVec2 mouse = { -100, -100 }, bool down = false, const std::function<void ()>& chart = {})
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (mouse.x, mouse.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        if (chart)
            chart ();
        DrawSunStudySunPath (state, matrix, 800, 600);
        ImGui::Render ();
    }
};
} // namespace

TEST_F (SunPathGui, ExactStepFollowsSliderWithoutLatticeOrSunOwnership)
{
    state.sunRunYear = 2030; // pending form is deliberately not the shown date
    state.sunRunMonth = 12;
    state.sunOverride = true;
    state.sunAzimuthDegrees = 17.0f;
    state.sunAltitudeDegrees = 23.0f;
    Draw ();
    ASSERT_TRUE (state.sunPathMarkerValid);
    EXPECT_EQ (state.sunPathMinute, 605); // exact stored 10:05, no lattice rounding
    EXPECT_FLOAT_EQ (state.sunPathDirection[0], 0.6f);
    const ImVec2 marker (state.sunPathMarker[0], state.sunPathMarker[1]);
    Draw (marker, true); // a press on the disc is not a drag or model input
    Draw ({ marker.x + 80, marker.y + 20 }, true);
    EXPECT_EQ (state.sunStep, 0);
    EXPECT_EQ (state.sunRunYear, 2030);
    EXPECT_EQ (state.sunRunMonth, 12);
    EXPECT_TRUE (state.sunOverride);
    EXPECT_FLOAT_EQ (state.sunAzimuthDegrees, 17.0f);
    EXPECT_FLOAT_EQ (state.sunAltitudeDegrees, 23.0f);
    EXPECT_FALSE (state.viewpointOwnsMouse);
    state.sunStep = 1;
    Draw ();
    EXPECT_EQ (state.sunPathMinute, 665);
    EXPECT_FLOAT_EQ (state.sunPathDirection[1], 0.8f);
}

TEST_F (SunPathGui, PeriodDiscUsesOnlyAppliedEnvelopeAndAggregateDoesNotInventNoon)
{
    auto geometry = std::make_shared<SunPathPeriodGeometry> ();
    geometry->year = 2028;
    geometry->studyId = "shown";
    geometry->revision = 17;
    geometry->days = { { 2, 29, 1.0, 0.25, state.sunPathDay.steps } };
    state.sunShowsPeriod = true;
    state.sunPathPeriod = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, { 1, 0, 0, 0 } });
    state.sunPeriodStep = 1; // pending intent must not win
    state.sunShadowMonth = 6;
    state.sunShadowDay = 21;
    Draw ();
    EXPECT_EQ (state.sunPathMinute, 605);
    state.sunPathPeriod = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, { 1, 0, 0, 1 } });
    Draw ();
    EXPECT_EQ (state.sunPathMinute, 665);
    state.sunPathPeriod = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, { 0, 2, 0, 0 } });
    Draw ();
    EXPECT_FALSE (state.sunPathMarkerValid);
    EXPECT_EQ (state.sunPathMinute, -1);
}

TEST_F (SunPathGui, StaleStudyIdentityAndMissingSceneCannotDisplayTheOldDisc)
{
    auto geometry = std::make_shared<SunPathPeriodGeometry> ();
    geometry->studyId = "another study";
    geometry->revision = 99;
    s_pathPeriod = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, {} });
    RefreshSunStudySunPathData (state, "missing");
    EXPECT_FALSE (state.sunPathDay.available);
    EXPECT_EQ (state.sunPathPeriod, nullptr);
    Draw ();
    EXPECT_FALSE (state.sunPathMarkerValid);
}

TEST_F (SunPathGui, CurrentChartHoverIsTransientAndNeverChangesCommittedSelection)
{
    auto geometry = std::make_shared<SunPathPeriodGeometry> ();
    geometry->year = 2028;
    geometry->days = { { 2, 15, 28.0, 0.25, { state.sunPathDay.steps[1] } },
                       { 2, 29, 1.0, 0.25, state.sunPathDay.steps } };
    state.sunShowsPeriod = true;
    state.sunPathPeriod = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, { 1, 0, 1, 1 } });
    std::string tooltip;
    Draw ({ -100, -100 }, false, [&] { tooltip = SunPathHoverMonth (state, 2, 605, 600, 660); });
    EXPECT_EQ (state.sunPathMinute, 605);
    EXPECT_EQ (state.sunPathHover.day, 29); // the first traced day has no step in that hour bin
    EXPECT_NE (tooltip.find ("2028-02-29"), std::string::npos);
    EXPECT_EQ (state.sunPathPeriod->applied.step, 1);
    EXPECT_EQ (state.sunStep, 0);
    EXPECT_FALSE (state.sunOverride);
    Draw ();
    EXPECT_EQ (state.sunPathMinute, 665); // leaving the chart restores the committed instant
    Draw ({ -100, -100 }, false, [&] { SunPathHoverMonth (state, 2, 300, 300, 360); });
    EXPECT_FALSE (state.sunPathMarkerValid); // an empty hovered bin never invents noon
    Draw ();
    EXPECT_EQ (state.sunPathMinute, 665);
}

TEST_F (SunPathGui, PointPlayerRequiresReadyMatchingRevisionAndCurrentSamples)
{
    auto geometry = std::make_shared<SunPathPeriodGeometry> ();
    geometry->year = 2028;
    geometry->studyId = "shown";
    geometry->revision = 17;
    geometry->days = { { 2, 29, 1.0, 0.25, state.sunPathDay.steps } };
    state.sunPathPeriod = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, {} });
    state.sunShowsPeriod = true;
    state.sunPointSamples = { 3, 7 };
    PeriodProfile profile;
    profile.ready = true;
    profile.studyId = "shown";
    profile.revision = 17;
    profile.samples = { 3, 7 };
    profile.dayMonth = { 2 };
    profile.dayOfMonth = { 29 };
    profile.dayMinutes = { { 605, 665 } };
    state.sunPointPlayDay = 0;
    state.sunPointPlayStep = 1;
    Draw ({ -100, -100 }, false, [&] {
        auto before = SunPathPointControlOf (state);
        before.playing = false;
        state.sunPointPlay.playing = true; // explicit play transition owns selection
        PublishSunPathPointPlayback (state, profile, -1, before);
    });
    EXPECT_EQ (state.sunPathMinute, 665);
    EXPECT_EQ (state.sunPathSyncSource, SunPathSyncSource::PointPlayer);
    profile.revision = 18;
    Draw ({ -100, -100 }, false,
          [&] { PublishSunPathPointPlayback (state, profile, 0, SunPathPointControlOf (state)); });
    EXPECT_FALSE (state.sunPathMarkerValid);
    EXPECT_EQ (state.sunPathSyncSource, SunPathSyncSource::None);
    profile.revision = 17;
    profile.samples = { 7, 3 }; // ordered sample identity matters
    EXPECT_FALSE (SunPathProfileMatches (state, profile));
    profile.samples = { 3, 7 };
    profile.ready = false;
    EXPECT_FALSE (SunPathProfileMatches (state, profile));
}

TEST_F (SunPathGui, ActiveShadowControlWinsOverPausedPointAndPendingPeriodIntent)
{
    auto geometry = std::make_shared<SunPathPeriodGeometry> ();
    geometry->year = 2028;
    geometry->days = { { 2, 29, 1.0, 0.25, state.sunPathDay.steps } };
    state.sunShowsPeriod = true;
    state.sunPathPeriod = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, { 1, 0, 0, 0 } });
    state.sunStepMinutes = { 605, 665 };
    state.sunStep = 1;
    state.sunPeriodStep = 0;
    Draw ({ -100, -100 }, false, [&] {
        state.sunPathShadowFrame = ImGui::GetFrameCount ();
        state.sunPathPointFrame = ImGui::GetFrameCount ();
        state.sunPathPointMoment = SunPathStepMoment (2028, 2, 29, state.sunPathDay.steps, 0);
        state.sunPathSyncSource = SunPathSyncSource::Shadow;
    });
    EXPECT_EQ (state.sunPathMinute, 665);
    EXPECT_EQ (state.sunPathPeriod->applied.step, 0);
    Draw ();
    EXPECT_EQ (state.sunPathSyncSource, SunPathSyncSource::None); // hidden controls cannot retain ownership
    EXPECT_EQ (state.sunPathMinute, 605);
}

TEST_F (SunPathGui, BandUsesImmutableAnalysisWindowWhileSliceScaleAndPointOnlyAffectDisplay)
{
    state.sunPathDay.hourFrom = 10;
    state.sunPathDay.hourTo = 12;
    state.sunPathDay.timestepMinutes = 60;
    Draw ();
    EXPECT_EQ (state.sunPathBand.Data ().steps, 2u);
    EXPECT_NE (state.sunPathBandNote.find ("All 2"), std::string::npos);
    const auto builds = state.sunPathBand.Builds ();
    const auto markerX = state.sunPathMarker[0], markerY = state.sunPathMarker[1];
    state.sunTimeFrom = 11.0f;
    state.sunTimeTo = 12.0f;
    state.sunPointPlaced = true;
    state.sunPointCentre[0] = 300.0;
    state.sunPointCentre[2] = 80.0;
    Draw ();
    EXPECT_EQ (state.sunPathBand.Builds (), builds);
    EXPECT_EQ (state.sunPathBand.Data ().steps, 2u);
    EXPECT_FLOAT_EQ (state.sunPathMarker[0], markerX);
    EXPECT_FLOAT_EQ (state.sunPathMarker[1], markerY);
    state.sunPathScale = 2.0f;
    Draw ();
    EXPECT_EQ (state.sunPathBand.Builds (), builds);
    EXPECT_EQ (state.sunPathDay.steps.size (), 2u);
}

TEST_F (SunPathGui, DrawingLodDisclosesOmissionsAndStillBreaksAtClippedSkippedSamples)
{
    SunPathBand band;
    SunPathBandDay day;
    day.runs.emplace_back ();
    for (int i = 0; i < 300; ++i) {
        SunPathMoment moment;
        moment.valid = true;
        moment.minute = i;
        moment.direction[0] = float (i);
        day.runs[0].push_back (moment);
    }
    band.days.push_back (day);
    band.steps = 300;
    SunPathBandDrawing result;
    Draw ({ 15, 100 }, false, [&] {
        result = DrawSunPathBand (ImGui::GetBackgroundDrawList (), band, {}, 0, 0, false, 0, 1440,
                                  [] (const float direction[3], ImVec2& screen) {
                                      screen = { direction[0] * 10.0f, 100.0f };
                                      return direction[0] != 1.0f; // skipped by LOD but must still split the ribbon
                                  });
    });
    EXPECT_TRUE (result.decimated);
    EXPECT_LT (result.shown, result.total);
    EXPECT_FALSE (result.hot); // no bridge from x=0 to x=30 through the clipped sample
    EXPECT_NE (SunPathBandCaption (result, true).find ("LOD preview:"), std::string::npos);
    EXPECT_NE (SunPathBandCaption (result, true).find ("calendar weights"), std::string::npos);
}

TEST_F (SunPathGui, AutomaticPlayerTicksCannotStealAnotherControlsExplicitOwnership)
{
    auto geometry = std::make_shared<SunPathPeriodGeometry> ();
    geometry->year = 2028;
    geometry->studyId = "shown";
    geometry->revision = 17;
    geometry->days = { { 2, 29, 1.0, 0.25, state.sunPathDay.steps } };
    state.sunShowsPeriod = true;
    state.sunPathPeriod = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, { 1, 0, 0, 0 } });
    state.sunStepMinutes = { 605, 665 };
    state.sunStep = 1;
    state.sunPointPlayStep = 0;
    PeriodProfile profile;
    profile.studyId = "shown";
    profile.revision = 17;
    profile.ready = true;
    profile.dayMonth = { 2 };
    profile.dayOfMonth = { 29 };
    profile.dayMinutes = { { 605, 665 } };
    Draw ({ -100, -100 }, false, [&] {
        state.sunPathShadowFrame = ImGui::GetFrameCount ();
        state.sunPathSyncSource = SunPathSyncSource::Shadow;
        PublishSunPathMapPlayback (state, true, false, false); // a tick, not input in its own controls
        auto before = SunPathPointControlOf (state);
        before.step = 1;
        PublishSunPathPointPlayback (state, profile, -1, before, false);
        EXPECT_EQ (state.sunPathSyncSource, SunPathSyncSource::Shadow);
    });
    EXPECT_EQ (state.sunPathMinute, 665);
    Draw ({ -100, -100 }, false, [&] {
        PublishSunPathMapPlayback (state, true, false, true); // an explicit edit in the map controls
    });
    EXPECT_EQ (state.sunPathMinute, 605);
}
