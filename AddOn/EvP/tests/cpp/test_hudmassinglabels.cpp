#include "ArchViz/HudMassingLabels.hpp"
#include "ArchViz/AnnotationScreenLayout.hpp"
#include <gtest/gtest.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace av = geomsrv::archviz;
namespace labels = av::hudmassingrules;
namespace {
const av::ScreenTextMeasure kMeasure = [] (std::string_view text, float size, av::ScreenTextExtent& extent) {
    size_t width = 0, current = 0, rows = 1;
    for (char c : text) {
        if (c == '\n') {
            width = (std::max) (width, current);
            current = 0;
            ++rows;
        }
        else
            ++current;
    }
    extent = { float ((std::max) (width, current)) * size * 0.5f, float (rows) * size };
    return true;
};
av::ScreenPoint Center (const av::ScreenLabel& label)
{
    av::ScreenTextExtent extent;
    kMeasure (label.text, label.fontSize, extent);
    return { label.anchor.x, label.anchor.y + extent.height / 2 };
}
} // namespace

TEST (HudMassingLabels, SegmentAndOffsetTextFollowHorizontalVerticalAndDiagonalEdgesUprightInEitherWinding)
{
    for (const auto direction :
         { av::ScreenPoint { 1, 0 }, av::ScreenPoint { 0, 1 }, av::ScreenPoint { 1, 1 }, av::ScreenPoint { -1, 1 } }) {
        av::ProjectedDrawList occupied, reversed;
        const labels::DiagramLabel input { { 150, 150 }, direction, "S1  3.00 m\nL 10.00 m" };
        const auto label = labels::PlaceDiagramLabel (occupied, input, { 0, 0 }, { 300, 300 }, 13, kMeasure);
        ASSERT_TRUE (label);
        EXPECT_GE (label->rotationRadians, -1.570797f);
        EXPECT_LE (label->rotationRadians, 1.570797f);
        EXPECT_NEAR (std::sin (label->rotationRadians) * direction.x - std::cos (label->rotationRadians) * direction.y,
                     0, 1e-6);
        auto reverse = input;
        reverse.direction = { -direction.x, -direction.y };
        const auto other = labels::PlaceDiagramLabel (reversed, reverse, { 0, 0 }, { 300, 300 }, 13, kMeasure);
        ASSERT_TRUE (other);
        EXPECT_FLOAT_EQ (label->rotationRadians, other->rotationRadians);
        EXPECT_FLOAT_EQ (label->anchor.x, other->anchor.x);
        EXPECT_FLOAT_EQ (label->anchor.y, other->anchor.y);
    }
}

TEST (HudMassingLabels, CandidatesAvoidEveryParcelInsetAndPreviouslyPlacedLabelAndStayInsideCanvas)
{
    av::ProjectedDrawList occupied;
    occupied.lines = { { { 30, 50 }, { 270, 50 } },
                       { { 30, 16 }, { 270, 16 } },
                       { { 30, 85 }, { 270, 85 } },
                       { { 140, 10 }, { 140, 140 } } };
    for (int i = 0; i < 3; ++i) {
        const labels::DiagramLabel input { { 150, 50 }, { 1, 0 }, "S" + std::to_string (i) + "  3m" };
        auto before = occupied;
        const auto label = labels::PlaceDiagramLabel (occupied, input, { 0, 0 }, { 300, 200 }, 13, kMeasure);
        ASSERT_TRUE (label);
        av::ScreenTextExtent extent;
        kMeasure (label->text, label->fontSize, extent);
        EXPECT_EQ (av::AnnotationCandidateOccupancyPenalty (before, Center (*label), extent, label->rotationRadians, 5,
                                                            kMeasure),
                   0);
        EXPECT_GE (Center (*label).x - extent.width / 2, 2);
        EXPECT_LE (Center (*label).x + extent.width / 2, 298);
        EXPECT_GE (label->anchor.y, 2);
        EXPECT_LE (label->anchor.y + extent.height, 198);
    }
}

TEST (HudMassingLabels, DenseOrShortCanvasOmitsLabelsInsteadOfCoveringLineworkOrClippingText)
{
    av::ProjectedDrawList occupied;
    for (int y = 0; y <= 100; y += 4)
        occupied.lines.push_back ({ { 0, float (y) }, { 300, float (y) } });
    const labels::DiagramLabel input { { 150, 50 }, { 1, 0 }, "S1  3.00 m" };
    EXPECT_FALSE (labels::PlaceDiagramLabel (occupied, input, { 0, 0 }, { 300, 100 }, 13, kMeasure));
    EXPECT_TRUE (occupied.labels.empty ());
    occupied.lines.clear ();
    EXPECT_FALSE (labels::PlaceDiagramLabel (occupied, input, { 145, 0 }, { 155, 100 }, 13, kMeasure));
    EXPECT_TRUE (occupied.labels.empty ());
}

TEST (HudMassingLabels, ReportedRectangleKeepsAllFourSegmentOffsetsAndEndpointIdsOutsideCloseInsetLines)
{
    av::ProjectedDrawList occupied;
    constexpr float halfWidth = 85.34f * 136 / 93.82f / 2;
    constexpr float inset = 3 * 136 / 93.82f;
    const av::ScreenPoint corners[] = {
        { 130 - halfWidth, 168 }, { 130 + halfWidth, 168 }, { 130 + halfWidth, 32 }, { 130 - halfWidth, 32 }
    };
    const av::ScreenPoint insetCorners[] = { { corners[0].x + inset, 168 - inset },
                                             { corners[1].x - inset, 168 - inset },
                                             { corners[2].x - inset, 32 + inset },
                                             { corners[3].x + inset, 32 + inset } };
    for (int i = 0; i < 4; ++i) {
        occupied.lines.push_back ({ corners[i], corners[(i + 1) % 4] });
        occupied.lines.push_back ({ insetCorners[i], insetCorners[(i + 1) % 4] });
        occupied.lines.push_back ({ corners[i], corners[i] });
    }
    for (int i = 0; i < 4; ++i) {
        const auto a = corners[i], b = corners[(i + 1) % 4];
        const auto label = labels::PlaceDiagramLabel (occupied,
                                                      { { (a.x + b.x) / 2, (a.y + b.y) / 2 },
                                                        { b.x - a.x, b.y - a.y },
                                                        "S" + std::to_string (i + 1) + "  3.00 m" },
                                                      { 0, 0 }, { 260, 200 }, 13, kMeasure);
        ASSERT_TRUE (label) << "Segment " << i + 1;
        EXPECT_EQ (label->text.find ('\n'), std::string::npos);
    }
    for (int i = 0; i < 4; ++i) {
        const auto label = labels::PlaceDiagramLabel (occupied, { corners[i], { 1, 0 }, "P" + std::to_string (i + 1) },
                                                      { 0, 0 }, { 260, 200 }, 13, kMeasure);
        ASSERT_TRUE (label) << "Endpoint " << i + 1;
    }
    EXPECT_EQ (occupied.labels.size (), 8u);
}

TEST (HudMassingLabels, DiagonalLabelsAvoidSampledCurvesAndPlacementScalesWithTheHudFont)
{
    av::ProjectedDrawList occupied;
    // Curved inset and crossing parcel edge, as the miniGUI's sampled paths supply them.
    for (int i = 0; i < 16; ++i) {
        const float a = float (i) * 3.14159265f / 16, b = float (i + 1) * 3.14159265f / 16;
        occupied.lines.push_back ({ { 150 + 45 * std::cos (a), 150 + 45 * std::sin (a) },
                                    { 150 + 45 * std::cos (b), 150 + 45 * std::sin (b) } });
    }
    occupied.lines.push_back ({ { 50, 50 }, { 250, 250 } });
    const labels::DiagramLabel input { { 150, 150 }, { 1, 1 }, "S1  3.00 m" };
    const auto before = occupied;
    const auto label = labels::PlaceDiagramLabel (occupied, input, { 0, 0 }, { 300, 300 }, 13, kMeasure);
    ASSERT_TRUE (label);
    av::ScreenTextExtent extent;
    kMeasure (label->text, label->fontSize, extent);
    EXPECT_EQ (
        av::AnnotationCandidateOccupancyPenalty (before, Center (*label), extent, label->rotationRadians, 5, kMeasure),
        0);
    auto doubled = before;
    for (auto& line : doubled.lines) {
        line.from.x *= 2;
        line.from.y *= 2;
        line.to.x *= 2;
        line.to.y *= 2;
    }
    auto larger = input;
    larger.anchor = { 300, 300 };
    const auto scaled = labels::PlaceDiagramLabel (doubled, larger, { 0, 0 }, { 600, 600 }, 26, kMeasure);
    ASSERT_TRUE (scaled);
    EXPECT_FLOAT_EQ (scaled->rotationRadians, label->rotationRadians);
    EXPECT_NEAR (scaled->anchor.x, 2 * label->anchor.x, 1e-5);
    EXPECT_NEAR (scaled->anchor.y, 2 * label->anchor.y, 1e-5);
}

TEST (HudMassingLabels, VerticalGlyphsAreRotatedBeforeCanvasClippingWithoutLosingCharacters)
{
    auto* context = ImGui::CreateContext ();
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
    ImGui::Begin ("rotated-labels");
    auto* draw = ImGui::GetWindowDrawList ();
    draw->PushClipRect ({ 5, 5 }, { 35, 295 }, true);
    av::ScreenLabel label;
    label.text = "S1 3.00m";
    label.fontSize = ImGui::GetFontSize ();
    label.anchor = { 20, 150 - label.fontSize / 2 };
    label.rotationRadians = -1.57079632679f;
    const int first = draw->VtxBuffer.Size;
    labels::DrawDiagramLabel (*draw, label);
    EXPECT_EQ (draw->VtxBuffer.Size - first, 7 * 4);
    for (int i = first; i < draw->VtxBuffer.Size; ++i) {
        EXPECT_GE (draw->VtxBuffer[i].pos.x, 5);
        EXPECT_LE (draw->VtxBuffer[i].pos.x, 35);
        EXPECT_GE (draw->VtxBuffer[i].pos.y, 5);
        EXPECT_LE (draw->VtxBuffer[i].pos.y, 295);
    }
    EXPECT_FLOAT_EQ (draw->CmdBuffer.back ().ClipRect.x, 5);
    EXPECT_FLOAT_EQ (draw->CmdBuffer.back ().ClipRect.z, 35);
    draw->PopClipRect ();
    ImGui::End ();
    ImGui::Render ();
    ImGui::DestroyContext (context);
}
