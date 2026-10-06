#include "ArchViz/HudMassingDiagram.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

namespace diagram = geomsrv::archviz::hudmassingrules;
namespace av = geomsrv::archviz;
namespace {
struct DiagramWidget {
    ImGuiContext* context = ImGui::CreateContext ();
    ImDrawList* draw = nullptr;
    DiagramWidget ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = { 500, 500 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 300, 300 });
        ImGui::Begin ("diagram-drawing");
        draw = ImGui::GetWindowDrawList ();
    }
    ~DiagramWidget ()
    {
        ImGui::End ();
        ImGui::Render ();
        ImGui::DestroyContext (context);
    }
    float AlphaAt (ImU32 colour, ImVec2 point) const
    {
        const auto cross = [] (ImVec2 a, ImVec2 b, ImVec2 c) {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        };
        float result = 0;
        for (int i = 0; i + 2 < draw->IdxBuffer.Size; i += 3) {
            const auto& a = draw->VtxBuffer[draw->IdxBuffer[i]];
            const auto& b = draw->VtxBuffer[draw->IdxBuffer[i + 1]];
            const auto& c = draw->VtxBuffer[draw->IdxBuffer[i + 2]];
            if ((a.col & 0xFFFFFF) != (colour & 0xFFFFFF) || (b.col & 0xFFFFFF) != (colour & 0xFFFFFF) ||
                (c.col & 0xFFFFFF) != (colour & 0xFFFFFF))
                continue;
            const float area = cross (a.pos, b.pos, c.pos);
            if (std::abs (area) < 1e-6f)
                continue;
            const float wa = cross (b.pos, c.pos, point) / area;
            const float wb = cross (c.pos, a.pos, point) / area;
            const float wc = cross (a.pos, b.pos, point) / area;
            if (wa < -1e-5f || wb < -1e-5f || wc < -1e-5f)
                continue;
            result =
                (std::max) (result, wa * float (a.col >> 24) + wb * float (b.col >> 24) + wc * float (c.col >> 24));
        }
        return result;
    }
};
} // namespace

TEST (HudMassingDiagram, OffsetOutlineIsDashedWithContinuousPhaseAtCornersAndWholePathCollisionReservations)
{
    DiagramWidget widget;
    av::ProjectedDrawList occupied;
    diagram::DrawDiagramOffset (*widget.draw, { { 30, 40 }, { 143, 40 }, { 143, 153 }, { 30, 153 } }, 13, &occupied);
    ASSERT_EQ (occupied.lines.size (), 4u);
    EXPECT_EQ (occupied.lines[0].from.x, 30);
    EXPECT_EQ (occupied.lines[0].to.x, 143);
    constexpr ImU32 colour = IM_COL32 (166, 98, 38, 255);
    EXPECT_GT (widget.AlphaAt (colour, { 32, 40 }), 200);
    EXPECT_EQ (widget.AlphaAt (colour, { 36.5f, 40 }), 0);
    EXPECT_GT (widget.AlphaAt (colour, { 143, 42 }), 200);
    EXPECT_EQ (widget.AlphaAt (colour, { 143, 45.5f }), 0);
}

TEST (HudMassingDiagram, PropertyLineIsDashDotWithRedRoundDotsAndContinuousCornerPhase)
{
    DiagramWidget widget;
    av::ProjectedDrawList occupied;
    double arc = 0;
    diagram::DrawDiagramProperty (*widget.draw, { 30, 40 }, { 143, 40 }, 2, 13, arc, &occupied);
    diagram::DrawDiagramProperty (*widget.draw, { 143, 40 }, { 143, 153 }, 2, 13, arc, &occupied);
    constexpr ImU32 red = IM_COL32 (170, 68, 101, 255);
    EXPECT_GT (widget.AlphaAt (red, { 35, 40 }), 200);
    EXPECT_EQ (widget.AlphaAt (red, { 45, 40 }), 0);
    EXPECT_GT (widget.AlphaAt (red, { 49, 40 }), 200);
    EXPECT_EQ (widget.AlphaAt (red, { 53, 40 }), 0);
    EXPECT_GT (widget.AlphaAt (red, { 60, 40 }), 200);
    EXPECT_GT (widget.AlphaAt (red, { 143, 50 }), 200);
    EXPECT_EQ (widget.AlphaAt (red, { 143, 46 }), 0);
    EXPECT_EQ (occupied.lines.size (), 2u);
    EXPECT_EQ (arc, 226);
}

TEST (HudMassingDiagram, OffsetDashesScaleWithFontAndInvalidPathsPublishNoGeometry)
{
    DiagramWidget widget;
    diagram::DrawDiagramOffset (*widget.draw, { { 30, 40 }, { 230, 40 }, { 230, 240 }, { 30, 240 } }, 26);
    constexpr ImU32 colour = IM_COL32 (166, 98, 38, 255);
    EXPECT_GT (widget.AlphaAt (colour, { 38, 40 }), 200);
    EXPECT_EQ (widget.AlphaAt (colour, { 43, 40 }), 0);
    const int before = widget.draw->VtxBuffer.Size;
    diagram::DrawDiagramOffset (*widget.draw, {}, 13);
    diagram::DrawDiagramOffset (*widget.draw, { { 30, 40 }, { 230, 40 }, { 230, 240 } }, 0);
    EXPECT_EQ (widget.draw->VtxBuffer.Size, before);
}

TEST (HudMassingDiagram, ZeroOffsetWarmYellowFadesOutwardInBothWindingsAndKeepsTheRedSegment)
{
    for (bool reverse : { false, true }) {
        DiagramWidget widget;
        std::vector<av::ScreenPoint> points { { 50, 50 }, { 150, 50 }, { 150, 150 }, { 50, 150 } };
        std::vector<bool> zero (4, false);
        if (reverse)
            std::reverse (points.begin (), points.end ());
        zero[reverse ? 2 : 0] = true;
        diagram::DrawDiagramZeroOffset (*widget.draw, points, zero, 13);
        constexpr ImU32 yellow = IM_COL32 (255, 193, 70, 170);
        const auto near = widget.AlphaAt (yellow, { 100, 46 });
        const auto far = widget.AlphaAt (yellow, { 100, 34 });
        EXPECT_GT (near, 100);
        EXPECT_GT (far, 0);
        EXPECT_LT (far, near);
        EXPECT_EQ (widget.AlphaAt (yellow, { 100, 30 }), 0);
        EXPECT_EQ (widget.AlphaAt (yellow, { 100, 60 }), 0);
        EXPECT_EQ (widget.AlphaAt (yellow, { 160, 100 }), 0) << "nonzero neighbor has no yellow band";
        constexpr ImU32 red = IM_COL32 (170, 68, 101, 255);
        widget.draw->AddLine ({ 50, 50 }, { 150, 50 }, red, 3);
        EXPECT_GT (widget.AlphaAt (red, { 100, 50 }), 200);
    }
}

TEST (HudMassingDiagram, ZeroOffsetBandNeverPaintsInsideTheOppositeWallOfANarrowConcavity)
{
    DiagramWidget widget;
    const std::vector<av::ScreenPoint> points { { 50, 50 },  { 170, 50 }, { 170, 170 }, { 150, 170 },
                                                { 150, 70 }, { 140, 70 }, { 140, 170 }, { 50, 170 } };
    std::vector<bool> zero (points.size (), false);
    zero[3] = true;
    diagram::DrawDiagramZeroOffset (*widget.draw, points, zero, 13);
    constexpr ImU32 yellow = IM_COL32 (255, 193, 70, 170);
    EXPECT_GT (widget.AlphaAt (yellow, { 145, 120 }), 0);
    EXPECT_EQ (widget.AlphaAt (yellow, { 135, 120 }), 0);
    EXPECT_EQ (widget.AlphaAt (yellow, { 155, 120 }), 0);
}

TEST (HudMassingDiagram, ZeroOffsetCornerJoinsRemainOutsideAndNonzeroAssignmentsEmitNoHighlight)
{
    DiagramWidget widget;
    const std::vector<av::ScreenPoint> points { { 50, 50 }, { 150, 50 }, { 150, 150 }, { 50, 150 } };
    const int before = widget.draw->VtxBuffer.Size;
    diagram::DrawDiagramZeroOffset (*widget.draw, points, { false, false, false, false }, 13);
    EXPECT_EQ (widget.draw->VtxBuffer.Size, before);
    diagram::DrawDiagramZeroOffset (*widget.draw, points, { true, true, false, false }, 13);
    constexpr ImU32 yellow = IM_COL32 (255, 193, 70, 170);
    EXPECT_GT (widget.AlphaAt (yellow, { 156, 44 }), 0);
    EXPECT_EQ (widget.AlphaAt (yellow, { 144, 56 }), 0);
    bool transparentOuterVertex = false;
    for (int i = before; i < widget.draw->VtxBuffer.Size; ++i)
        if (widget.draw->VtxBuffer[i].col == IM_COL32 (255, 193, 70, 0))
            transparentOuterVertex = true;
    EXPECT_TRUE (transparentOuterVertex);
}
