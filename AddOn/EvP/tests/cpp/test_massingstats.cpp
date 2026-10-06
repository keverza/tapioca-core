#include "ArchViz/HudMassingStats.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include <limits>
#include <gtest/gtest.h>
#include <imgui_internal.h>

namespace stats = geomsrv::archviz::hudmassingstats;
namespace slices = geomsrv::archviz::massingslices;
namespace {
struct StatsWidget {
    ImGuiContext* context = ImGui::CreateContext ();
    slices::Result result;
    StatsWidget ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = { 1000, 1000 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
        slices::Row commercial, residential;
        commercial.function = "commercial";
        commercial.rgba = 0xE53935FF;
        commercial.allowedArea = 75;
        commercial.rawArea = 900;
        residential.function = "residential";
        residential.rgba = 0xFFCA28FF;
        residential.allowedArea = 25;
        residential.rawArea = 900;
        result.rows = { commercial, residential };
        result.clipped = true;
        Frame ();
        Frame ();
    }
    ~StatsWidget ()
    {
        ImGui::DestroyContext (context);
    }
    std::string Frame (ImVec2 pointer = { 800, 800 })
    {
        ImGui::GetIO ().AddMousePosEvent (pointer.x, pointer.y);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 500, 600 });
        ImGui::Begin ("stats-graphs");
        const auto hovered = stats::Draw (result);
        ImGui::End ();
        ImGui::Render ();
        return hovered;
    }
    ImRect Rectangle (ImU32 colour, size_t& count) const
    {
        ImRect bounds { { FLT_MAX, FLT_MAX }, { -FLT_MAX, -FLT_MAX } };
        count = 0;
        const auto white = ImGui::GetIO ().Fonts->TexUvWhitePixel;
        for (const auto& vertex : ImGui::FindWindowByName ("stats-graphs")->DrawList->VtxBuffer)
            if (vertex.col == colour && vertex.uv.x == white.x && vertex.uv.y == white.y) {
                bounds.Add (vertex.pos);
                ++count;
            }
        return bounds;
    }
};
} // namespace

TEST (MassingStats, AllowedFunctionsShareOneRedAndYellowGraphWithNoSeparateFunctionBars)
{
    StatsWidget widget;
    size_t redCount = 0, yellowCount = 0;
    const auto red = widget.Rectangle (IM_COL32 (229, 57, 53, 255), redCount);
    const auto yellow = widget.Rectangle (IM_COL32 (255, 202, 40, 255), yellowCount);
    ASSERT_EQ (redCount, 4u);
    ASSERT_EQ (yellowCount, 4u);
    EXPECT_FLOAT_EQ (red.Min.y, yellow.Min.y);
    EXPECT_FLOAT_EQ (red.Max.y, yellow.Max.y);
    EXPECT_FLOAT_EQ (red.Max.x, yellow.Min.x);
    EXPECT_NEAR (red.GetWidth () / (red.GetWidth () + yellow.GetWidth ()), 0.75, 1e-6);
    EXPECT_EQ (widget.Frame (red.GetCenter ()), "commercial");
    EXPECT_EQ (widget.Frame (yellow.GetCenter ()), "residential");
    EXPECT_TRUE (widget.Frame ().empty ());
}

TEST (MassingStats, ParcelCoverageUsesGreyBuiltAndGreenUnbuiltAndRestoresTheTheme)
{
    StatsWidget widget;
    const auto histogram = ImGui::GetStyleColorVec4 (ImGuiCol_PlotHistogram);
    const auto frame = ImGui::GetStyleColorVec4 (ImGuiCol_FrameBg);
    widget.result.hasCoverage = true;
    widget.result.parcelArea = 200;
    widget.result.builtArea = 80;
    widget.result.unbuiltArea = 120;
    widget.Frame ();
    size_t greyCount = 0, greenCount = 0;
    const auto grey = widget.Rectangle (IM_COL32 (154, 160, 166, 255), greyCount);
    const auto green = widget.Rectangle (IM_COL32 (102, 187, 106, 255), greenCount);
    EXPECT_GE (greyCount, 4u);
    EXPECT_GE (greenCount, 4u);
    EXPECT_NEAR (grey.GetWidth () / green.GetWidth (), 0.4, 1e-6);
    EXPECT_FLOAT_EQ (grey.Min.x, green.Min.x);
    EXPECT_FLOAT_EQ (grey.Min.y, green.Min.y);
    EXPECT_FLOAT_EQ (grey.Max.y, green.Max.y);
    EXPECT_GT (green.Max.x, grey.Max.x);
    EXPECT_EQ (widget.Frame (grey.GetCenter ()), slices::kBuiltHover);
    EXPECT_EQ (widget.Frame ({ (grey.Max.x + green.Max.x) / 2, green.GetCenter ().y }), slices::kUnbuiltHover);
    EXPECT_TRUE (widget.Frame ().empty ());
    EXPECT_EQ (ImGui::ColorConvertFloat4ToU32 (ImGui::GetStyleColorVec4 (ImGuiCol_PlotHistogram)),
               ImGui::ColorConvertFloat4ToU32 (histogram));
    EXPECT_EQ (ImGui::ColorConvertFloat4ToU32 (ImGui::GetStyleColorVec4 (ImGuiCol_FrameBg)),
               ImGui::ColorConvertFloat4ToU32 (frame));
    widget.result.builtArea = 0;
    widget.Frame ();
    widget.Rectangle (IM_COL32 (154, 160, 166, 255), greyCount);
    EXPECT_EQ (greyCount, 0u);
    widget.result.builtArea = 200;
    widget.Frame ();
    const auto fullGrey = widget.Rectangle (IM_COL32 (154, 160, 166, 255), greyCount);
    EXPECT_FLOAT_EQ (fullGrey.GetWidth (), green.GetWidth ());
}

TEST (MassingStats, AreaCoefficientsUseTheRequestedDefaultsAndKeepFractionalUnitEstimates)
{
    namespace areas = geomsrv::archviz::massingareas;
    const areas::Coefficients c;
    const auto out = areas::Calculate (1000, c);
    EXPECT_DOUBLE_EQ (out.gross, 780);
    EXPECT_DOUBLE_EQ (out.sellable, 710);
    EXPECT_DOUBLE_EQ (out.units, 15.6);
    EXPECT_DOUBLE_EQ (out.parking, 468);
    const auto figures = areas::Figures (1000, c);
    ASSERT_EQ (figures.size (), 5u);
    EXPECT_EQ (figures[1].value, "780.00 m2");
    EXPECT_EQ (figures[3].value, "15.60");
    EXPECT_EQ (figures[4].value, "468.00 m2");
}

TEST (MassingStats, EditedCoefficientsAreSharedByStatsAndSelectedBuildingAreasWithoutGeometryRequests)
{
    namespace hud = geomsrv::archviz::overlayhud;
    namespace areas = geomsrv::archviz::massingareas;
    namespace section = geomsrv::archviz::hudsection;
    auto state = hud::NewState ();
    areas::Coefficients c { 0.8, 0.6, 40, 25 };
    const auto revision = hud::Revision (*state);
    ASSERT_TRUE (hud::SetMassingCoefficients (*state, c));
    EXPECT_GT (hud::Revision (*state), revision);
    const auto shared = hud::MassingCoefficients (*state);
    section::Section building;
    section::Floor floor;
    floor.areaM2 = 1000;
    building.floors.push_back (floor);
    const auto selected = section::BuildingAreas (building, shared);
    const auto stats = areas::Figures (1000, shared);
    for (size_t i = 0; i < stats.size (); ++i)
        EXPECT_EQ (selected.figures[i].value, stats[i].value);
    EXPECT_EQ (stats[1].value, "800.00 m2");
    EXPECT_EQ (stats[2].value, "600.00 m2");
    EXPECT_EQ (stats[3].value, "20.00");
    EXPECT_EQ (stats[4].value, "500.00 m2");
    EXPECT_NE (stats[1].label.find ("0.8"), std::string::npos);
    EXPECT_TRUE (hud::TakeMassingCalculations (*state).empty ());
    EXPECT_FALSE (hud::SetMassingCoefficients (*state, c));
    EXPECT_EQ (hud::Revision (*state), revision + 1);
}

TEST (MassingStats, InvalidAreaCoefficientsNeverReplaceTheCurrentSettingsOrDivideByZero)
{
    namespace hud = geomsrv::archviz::overlayhud;
    namespace areas = geomsrv::archviz::massingareas;
    auto state = hud::NewState ();
    const auto original = hud::MassingCoefficients (*state);
    for (const auto bad : { areas::Coefficients { -0.1, 0.71, 50, 30 },
                            { 0.78, 1.1, 50, 30 },
                            { 0.78, 0.71, 0, 30 },
                            { 0.78, 0.71, 50, -1 },
                            { std::numeric_limits<double>::quiet_NaN (), 0.71, 50, 30 },
                            { 0.78, 0.71, 50, std::numeric_limits<double>::infinity () } }) {
        EXPECT_FALSE (hud::SetMassingCoefficients (*state, bad));
        EXPECT_TRUE (areas::Same (hud::MassingCoefficients (*state), original));
    }
    EXPECT_EQ (hud::Revision (*state), 0u);
    EXPECT_EQ (areas::Calculate (0, original).units, 0);
    EXPECT_EQ (areas::Calculate (1000, { 0, 0, 50, 0 }).parking, 0);
}

TEST (MassingStats, CoefficientSetQueuesNativeNumberEntryWithStaleAndInvalidAnswerGuards)
{
    StatsWidget widget;
    geomsrv::archviz::massingareas::Coefficients coefficients;
    std::vector<geomsrv::archviz::massingareas::NumberEdit> numbers;
    ImVec2 target;
    const auto frame = [&] () {
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 500, 600 });
        ImGui::Begin ("coefficient-inputs");
        const bool edited = stats::CoefficientInputs (coefficients, numbers);
        const auto* table = widget.context->Tables.GetByKey (ImGui::GetID ("##massing.coefficients"));
        if (table)
            target = { table->Columns[1].WorkMaxX - 10, table->OuterRect.Min.y + ImGui::GetFrameHeight () / 2 };
        ImGui::End ();
        ImGui::Render ();
        return edited;
    };
    frame ();
    frame ();
    ASSERT_GT (target.x, 0);
    auto& io = ImGui::GetIO ();
    io.AddMousePosEvent (target.x, target.y);
    frame ();
    io.AddMouseButtonEvent (0, true);
    frame ();
    io.AddMouseButtonEvent (0, false);
    frame ();
    ASSERT_EQ (numbers.size (), 1u);
    EXPECT_EQ (numbers[0].key, "Gross area factor");
    EXPECT_DOUBLE_EQ (coefficients.grossFactor, 0.78) << "Opening or cancelling Set does not change settings";
    namespace hud = geomsrv::archviz::overlayhud;
    auto state = hud::NewState ();
    EXPECT_FALSE (hud::AnswerMassingCoefficientNumber (*state, numbers[0], 2));
    EXPECT_TRUE (hud::AnswerMassingCoefficientNumber (*state, numbers[0], 0.85));
    EXPECT_DOUBLE_EQ (hud::MassingCoefficients (*state).grossFactor, 0.85);
    EXPECT_FALSE (hud::AnswerMassingCoefficientNumber (*state, numbers[0], 0.9)) << "Stale prompt rejected";
    EXPECT_TRUE (hud::TakeMassingCalculations (*state).empty ());
}
