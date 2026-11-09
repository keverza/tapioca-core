#include "ArchViz/DiligentHud.hpp"
#include "Geometry/MeshStore.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "MeshFixtures.hpp"

#include <gtest/gtest.h>
#include <imgui.h>

namespace geomsrv::sunstudyhud {
static std::shared_ptr<SunPath> s_path;
static std::shared_ptr<const SunPathPeriodData> s_pathPeriod;
static int s_pathRequests = 0;
void RequestSunPath (int, int, int)
{
    ++s_pathRequests;
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

    void Draw (ImVec2 mouse = { -100, -100 }, bool down = false)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (mouse.x, mouse.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
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
