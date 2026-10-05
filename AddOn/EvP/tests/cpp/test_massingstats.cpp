#include "ArchViz/HudMassingStats.hpp"
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
