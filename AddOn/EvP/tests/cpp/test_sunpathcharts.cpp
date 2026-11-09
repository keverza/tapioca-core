#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "Geometry/MeshStore.hpp"
#include "MeshFixtures.hpp"
#include "SunStudyHudTestServices.hpp"

#include <gtest/gtest.h>
#include <imgui_internal.h>

#include <cmath>

using namespace geomsrv::archviz;
using namespace geomsrv::sunstudyhud;
using namespace tapioca::test;

namespace {
evp::sunstudy::SunStep ChartSun (int minute)
{
    evp::sunstudy::SunStep result;
    result.time = { minute / 60, minute % 60 };
    result.direction[0] = 0.6;
    result.direction[2] = 0.8;
    return result;
}

struct SunPathCharts : testing::Test {
    ImGuiContext* previous = nullptr;
    std::shared_ptr<const geomsrv::Snapshot> previousMesh;
    std::shared_ptr<SunPathPeriodGeometry> geometry;
    HudState state;
    ImDrawList* chartDraw = nullptr;
    float chartWidth = 0.0f;
    int pointOpen = 1;
    float matrix[16] = { 0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0.3f, 1 };

    void SetUp () override
    {
        ResetSunStudyHudServices ();
        previous = ImGui::GetCurrentContext ();
        ImGui::CreateContext ();
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 800, 1800 };
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
        previousMesh = geomsrv::MeshStore::Get ().Current ();
        auto mesh = std::make_shared<geomsrv::Snapshot> ();
        mesh->id = geomsrv::MeshStore::Get ().NextId ();
        mesh->meshes = { evptest::MakeBox ("scene", 0, 0, 0) };
        geomsrv::MeshStore::Get ().Publish (mesh);

        static unsigned next = 0;
        geometry = std::make_shared<SunPathPeriodGeometry> ();
        geometry->studyId = "chart-period-" + std::to_string (++next);
        geometry->revision = 17;
        geometry->year = 2028;
        geometry->estimate = true;
        geometry->days = { { 2, 15, 28.0, 0.25, { ChartSun (660), ChartSun (675) } },
                           { 2, 29, 1.0, 0.25, { ChartSun (600), ChartSun (615) } },
                           { 3, 15, 30.0, 0.25, { ChartSun (780), ChartSun (795) } },
                           { 3, 31, 1.0, 0.25, {} } };
        auto& period = SunStudyTestPeriod ();
        period.available = true;
        period.daysKept = true;
        period.studyId = geometry->studyId;
        period.representedDays = 60.0;
        period.monthDays[1] = 29.0;
        period.monthDays[2] = 31.0;
        period.siteDaylight[1] = period.siteDaylight[2] = 14.5;
        period.applied.frame = 1;
        period.applied.step = 1;
        auto kept = std::make_shared<std::vector<KeptDay>> ();
        for (const auto& traced : geometry->days) {
            KeptDay day;
            day.month = traced.month;
            day.day = traced.day;
            day.hoursPerStep = traced.hoursPerStep;
            for (const auto& step : traced.steps)
                day.minutes.push_back (uint16_t (step.time.hour * 60 + step.time.minute));
            kept->push_back (std::move (day));
        }
        SetSunStudyTestKeptDays (kept);
        auto& profile = SunStudyTestProfile ();
        profile.ready = true;
        profile.studyId = geometry->studyId;
        profile.revision = geometry->revision;
        profile.samples = { 0 };
        profile.dayMonth = { 2, 2, 3, 3 };
        profile.dayOfMonth = { 15, 29, 15, 31 };
        profile.dayMinutes = { { 660, 675 }, { 600, 615 }, { 780, 795 }, {} };
        profile.dayHoursPerStep = { 0.25, 0.25, 0.25, 0.25 };
        profile.dayWeights = { 28.0, 1.0, 30.0, 1.0 };
        profile.dayUp = profile.dayFacing = profile.dayLit = { 0.5, 0.5, 0.5, 0.0 };
        profile.dayStepFacing = profile.dayStepLit = { { 1.0, 1.0 }, { 1.0, 1.0 }, { 1.0, 1.0 }, {} };
        profile.up[1][10] = profile.up[1][11] = profile.up[2][13] = 0.5;
        profile.facing[1][10] = profile.lit[1][10] = 0.5;

        state.sunShowsPeriod = state.sunPathShown = state.sunPeriodProfile = state.sunPointPlaced = true;
        state.sunPeriodStudyId = geometry->studyId;
        state.sunPointSamples = { 0 };
        state.sunPointSamplesFor = geometry->studyId + "|0.000";
        state.sunPeriodPlayMode = 1;
        state.sunPeriodClock = 675;
        state.sunPointPlayClock = 600;
        state.sunPointPlayMode = 1;
        state.sunPathDay.available = true;
        state.sunPathDay.studyId = geometry->studyId;
        state.sunPathDay.revision = geometry->revision;
        state.sunPathDay.year = 2028;
        state.sunPathDay.month = 2;
        state.sunPathDay.day = 15;
        state.sunPathDay.steps = geometry->days.front ().steps;
        auto path = std::make_shared<SunPath> ();
        path->ready = true;
        path->year = 2028;
        path->month = 2;
        path->day = 15;
        SetSunPathTestCapture (path);
        SetApplied ({ 1, 0, 0, 1 });
        Frame ();
        Frame ();
    }

    void TearDown () override
    {
        SetSunPathTestCapture (nullptr);
        SetSunPathTestPeriod (nullptr);
        ResetSunStudyHudServices ();
        geomsrv::MeshStore::Get ().Publish (previousMesh);
        ImGui::DestroyContext ();
        ImGui::SetCurrentContext (previous);
    }

    void SetApplied (SunPathAppliedFrame applied)
    {
        state.sunPathPeriod = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, applied });
        SetSunPathTestPeriod (state.sunPathPeriod);
    }

    void Frame (ImVec2 pointer = { -100, -100 }, bool down = false)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 500, 1700 });
        ImGui::Begin ("period chart integration", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::GetStateStorage ()->SetInt (ImGui::GetID ("Study point"), pointOpen);
        chartWidth = ImGui::GetContentRegionAvail ().x;
        DrawSunStudyPeriodView (state);
        chartDraw = ImGui::GetWindowDrawList ();
        ImGui::End ();
        DrawSunStudySunPath (state, matrix, 800, 1800);
        ImGui::Render ();
    }

    // Locate the actual full-width ChartFrame background, not a guessed widget
    // position or a production-only test hook. Each chart has a distinct height.
    ImRect Chart (float height)
    {
        const ImU32 colour = ImGui::GetColorU32 (ImGuiCol_FrameBg);
        const auto near = [] (float a, float b) { return std::abs (a - b) < 0.1f; };
        for (int i = 0; i + 3 < chartDraw->VtxBuffer.Size; ++i) {
            const auto& a = chartDraw->VtxBuffer[i];
            const auto& b = chartDraw->VtxBuffer[i + 1];
            const auto& c = chartDraw->VtxBuffer[i + 2];
            const auto& d = chartDraw->VtxBuffer[i + 3];
            if (a.col == colour && b.col == colour && c.col == colour && d.col == colour &&
                near (b.pos.x - a.pos.x, chartWidth) && near (d.pos.y - a.pos.y, height) && near (a.pos.x, d.pos.x) &&
                near (a.pos.y, b.pos.y) && near (b.pos.x, c.pos.x) && near (c.pos.y, d.pos.y))
                return { a.pos, c.pos };
        }
        ADD_FAILURE () << "No actual chart background of height " << height;
        return {};
    }
};
} // namespace

TEST_F (SunPathCharts, MonthBarHoverMovesTheDiscAndLeavingRestoresTheAppliedInstant)
{
    state.sunPeriodClock = 600;
    Frame ();
    const auto bars = Chart (ImGui::GetTextLineHeight () * 7.0f);
    const ImVec2 february (bars.Min.x + bars.GetWidth () * 1.5f / 12.0f, bars.GetCenter ().y);
    const auto requests = SunStudyTestViewRequests ();
    Frame (february);
    EXPECT_EQ (state.sunPathHoverFrame, ImGui::GetFrameCount ());
    EXPECT_TRUE (state.sunPathHover.valid);
    EXPECT_EQ (state.sunPathHover.year, 2028);
    EXPECT_EQ (state.sunPathHover.day, 29);
    EXPECT_EQ (state.sunPathMinute, 600);
    EXPECT_EQ (SunStudyTestViewRequests (), requests);
    EXPECT_EQ (SunStudyTestSubmissions (), 0);
    EXPECT_FALSE (state.sunOverride);
    Frame ();
    EXPECT_EQ (state.sunPathMinute, 675);
    for (const auto& date : SunPathTestCaptureDates ())
        EXPECT_EQ (date, (std::array<int, 3> { 2028, 2, 15 }));
}

TEST_F (SunPathCharts, ProfileCellUsesARealStepInsideItsHourEvenOnALaterTracedDay)
{
    const float line = ImGui::GetTextLineHeight ();
    const auto grid = Chart (line * 0.8f * 12.0f + line);
    const float label = ImGui::CalcTextSize ("M").x + 6.0f;
    const float cell = (grid.GetWidth () - label) / 4.0f; // hours 10 through 13
    const ImVec2 februaryTen (grid.Min.x + label + cell * 0.5f, grid.Min.y + line * 0.8f * 1.5f);
    Frame (februaryTen);
    EXPECT_EQ (state.sunPathHover.day, 29);
    EXPECT_EQ (state.sunPathMinute, 615);
    EXPECT_GE (state.sunPathMinute, 600);
    EXPECT_LT (state.sunPathMinute, 660);
    const ImVec2 emptyBin (grid.Min.x + label + cell * 2.5f, februaryTen.y);
    Frame (emptyBin);
    EXPECT_NE (state.sunPathHoverFrame, ImGui::GetFrameCount ());
    EXPECT_EQ (state.sunPathMinute, 675);
}

TEST_F (SunPathCharts, DayStripHoverKeepsExactDateAndSunlessDayHasNoDisc)
{
    const auto strip = Chart (ImGui::GetTextLineHeight () * 3.0f);
    Frame ({ strip.Min.x + strip.GetWidth () * 1.5f / 4.0f, strip.GetCenter ().y });
    EXPECT_EQ (state.sunPathHover.day, 29);
    EXPECT_EQ (state.sunPathMinute, 600);
    Frame ({ strip.Min.x + strip.GetWidth () * 3.5f / 4.0f, strip.GetCenter ().y });
    EXPECT_EQ (state.sunPathHoverFrame, ImGui::GetFrameCount ());
    EXPECT_FALSE (state.sunPathHover.valid);
    EXPECT_FALSE (state.sunPathMarkerValid);
    Frame ();
    EXPECT_EQ (state.sunPathMinute, 675);
}

TEST_F (SunPathCharts, HistogramOnlyHighlightsTheBandAndCollapsedChartsReleaseHover)
{
    state.sunPeriodQuantity = 4;
    state.sunPeriodPlayMode = 3;
    SetApplied ({});
    Frame ();
    const auto histogram = Chart (ImGui::GetTextLineHeight () * 7.0f);
    const ImVec2 pointer (histogram.Min.x + histogram.GetWidth () * 0.25f, histogram.GetCenter ().y);
    Frame (pointer);
    EXPECT_EQ (state.sunPathBandHoverFrame, ImGui::GetFrameCount ());
    EXPECT_NE (state.sunPathHoverFrame, ImGui::GetFrameCount ());
    EXPECT_FALSE (state.sunPathMarkerValid);
    pointOpen = 0;
    Frame (pointer);
    EXPECT_NE (state.sunPathBandHoverFrame, ImGui::GetFrameCount ());
    EXPECT_FALSE (state.sunPathMarkerValid);
}
