#include "ArchViz/RoofSlope.hpp"
#include "ArchViz/RoofSlopeHud.hpp"
#include "ArchViz/OverlayHud.hpp"
#include <imgui.h>
#include <gtest/gtest.h>
#include <clipper2/clipper.h>
#include <cmath>
#include <limits>

namespace rs = geomsrv::archviz::roofslope;
namespace hud = geomsrv::archviz::overlayhud;
namespace rh = geomsrv::archviz::roofhud;
namespace cp = Clipper2Lib;
namespace {
rs::Data Plan (std::vector<rs::Ring> rings, double top = 5)
{
    rs::Data d;
    std::string error;
    EXPECT_TRUE (rs::Plan (rings, top, 2, 200, d, error)) << error;
    return d;
}
cp::PathsD Shapes (const rs::Data& d)
{
    cp::PathsD shapes;
    for (const auto& t : d.triangles) {
        cp::PathD p;
        for (const auto& n : t.vertices)
            p.emplace_back (n.xy.x, n.xy.y);
        shapes.push_back (p);
    }
    return shapes;
}
double Area (const rs::Data& d)
{
    double sum = 0;
    for (const auto& t : d.triangles)
        sum += t.area;
    return sum;
}
} // namespace
TEST (RoofSlope, FourCornersCentralXAndFourPlanarFacesStartAtSlabTop)
{
    auto d = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    ASSERT_EQ (d.cells.size (), 1u);
    ASSERT_EQ (d.triangles.size (), 4u);
    EXPECT_EQ (d.cells[0].rim.size (), 4u);
    const auto& drain = d.nodes[d.cells[0].drain];
    EXPECT_DOUBLE_EQ (drain.z, 5);
    EXPECT_NEAR (rs::World (d, drain.xy).x, 5, 1e-6);
    EXPECT_NEAR (rs::World (d, drain.xy).y, 5, 1e-6);
    EXPECT_NEAR (Area (d), 100, 1e-6);
    for (const auto& t : d.triangles) {
        EXPECT_NEAR (std::hypot (t.gx, t.gy) * 100, 2, 1e-5);
        for (const auto& n : t.vertices)
            EXPECT_GE (n.z, d.topZ - 1e-6);
    }
}
TEST (RoofSlope, StrictlyLessThan200NotLessOrEqual)
{
    auto d = Plan ({ { { 0, 0 }, { 20, 0 }, { 20, 10 }, { 0, 10 } } });
    EXPECT_GE (d.cells.size (), 2u);
    for (const auto& c : d.cells) {
        EXPECT_LT (c.area, 200);
        EXPECT_LT ((c.box[2] - c.box[0]) * (c.box[3] - c.box[1]), 200);
        EXPECT_EQ (c.rim.size (), 4u);
    }
}
TEST (RoofSlope, ClipTheXAgainstConcaveOutlineAndCutoutsWithoutGaps)
{
    auto d = Plan ({ { { 0, 0 }, { 30, 0 }, { 30, 12 }, { 16, 12 }, { 16, 24 }, { 0, 24 } },
                     { { 4, 4 }, { 4, 8 }, { 8, 8 }, { 8, 4 } } });
    EXPECT_NEAR (Area (d), 536, 1e-4);
    const auto shapes = Shapes (d);
    EXPECT_NEAR (std::abs (cp::Area (cp::Union (shapes, cp::FillRule::NonZero, 6))), 536, 1e-4);
    const auto hole = cp::Intersect (shapes, { { { 4, 4 }, { 8, 4 }, { 8, 8 }, { 4, 8 } } }, cp::FillRule::NonZero, 6);
    EXPECT_NEAR (std::abs (cp::Area (hole)), 0, 1e-6);
    for (const auto& c : d.cells)
        EXPECT_EQ (c.rim.size (), 4u);
}
TEST (RoofSlope, SkewedBoundaryRetainsFourControlPointsButClipsFaces)
{
    auto d = Plan ({ { { 0, 0 }, { 15, 0 }, { 12, 10 }, { 0, 10 } } });
    EXPECT_NEAR (Area (d), 135, 1e-4);
    bool clipped = false;
    for (const auto& c : d.cells)
        clipped = clipped || !c.rectangle;
    EXPECT_TRUE (clipped);
    for (const auto& c : d.cells)
        EXPECT_EQ (c.rim.size (), 4u);
}
TEST (RoofSlope, CapturedUnevenRoofUsesSignificantCornersWithoutSliverGridStrips)
{
    auto d = Plan ({ { { -86.471534064, -242.721183691 }, { -86.471534064, -257.172818637 },
                       { -66.678385363, -247.484105727 }, { -67.1136407, -246.594919137 },
                       { -59.679863799, -243.050734935 }, { -59.196632204, -243.821800172 },
                       { -27.608065492, -228.359250037 }, { -33.31474657, -216.701025854 },
                       { -40.868341658, -220.398497912 }, { -38.92068319, -224.377382993 },
                       { -42.980404246, -226.364610553 }, { -44.928062338, -222.385724339 },
                       { -56.792865223, -228.193525343 }, { -54.845207506, -232.172410792 },
                       { -58.904928099, -234.159637176 }, { -60.836915994, -230.178647815 },
                       { -60.791586245, -230.15089254 },  { -76.16635487, -237.676815854 },
                       { -74.218697152, -241.655701303 }, { -78.278418362, -243.642927989 },
                       { -80.203494479, -239.652988884 } } });
    EXPECT_NEAR (Area (d), 741.610547711, 1e-3);
    EXPECT_LT (d.cells.size (), 60u);
    for (const auto& c : d.cells)
        EXPECT_EQ (c.rim.size (), 4u);
}
TEST (RoofSlope, RotationAndGeoreferencedCoordinatesPreserveArea)
{
    rs::Ring ring { { 0, 0 }, { 25, 0 }, { 25, 16 }, { 0, 16 } };
    for (auto& p : ring) {
        const double x = p.x, y = p.y;
        p = { 500000 + x * std::cos (0.455) - y * std::sin (0.455),
              6000000 + x * std::sin (0.455) + y * std::cos (0.455) };
    }
    auto d = Plan ({ ring });
    EXPECT_NEAR (Area (d), 400, 1e-3);
}
TEST (RoofSlope, ElevationAndDrainLocksRejectWithoutChangingGeometry)
{
    auto d = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    const int drain = d.cells[0].drain;
    std::string error;
    EXPECT_FALSE (rs::SetElevation (d, drain, 4.9, error));
    EXPECT_DOUBLE_EQ (d.nodes[drain].z, 5);
    d.nodes[drain].lockXY = true;
    EXPECT_FALSE (rs::MoveDrain (d, 0, { 6, 5 }, error));
    d.nodes[drain].lockZ = true;
    EXPECT_FALSE (rs::SetElevation (d, drain, 5.01, error));
}
TEST (RoofSlope, SharedDividerMovesWithoutLosingCoverOrPerpendicularity)
{
    auto d = Plan ({ { { 0, 0 }, { 30, 0 }, { 30, 10 }, { 0, 10 } } });
    const double area = Area (d);
    bool moved = false;
    for (int ci = 0; ci < int (d.cells.size ()) && !moved; ++ci)
        for (int side = 0; side < 4 && !moved; ++side) {
            auto copy = d;
            std::string error;
            if (rs::MoveDivider (copy, ci, side, copy.cells[ci].box[side] + 0.25, error)) {
                d = std::move (copy);
                moved = true;
            }
        }
    EXPECT_TRUE (moved);
    EXPECT_NEAR (Area (d), area, 1e-4);
    for (const auto& c : d.cells)
        EXPECT_EQ (c.rim.size (), 4u);
}
TEST (RoofSlope, OneGestureOneUndoAndCancelRestoresLastValidSurface)
{
    rs::Editor e;
    e.data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    const int n = e.data.cells[0].drain;
    e.Begin ();
    auto copy = e.before;
    std::string error;
    ASSERT_TRUE (rs::SetElevation (copy, n, 5.02, error));
    e.Apply (copy);
    e.Commit ();
    ASSERT_EQ (e.undo.size (), 1u);
    EXPECT_TRUE (e.Undo (true));
    EXPECT_DOUBLE_EQ (e.data.nodes[n].z, 5);
    EXPECT_TRUE (e.Undo (false));
    EXPECT_DOUBLE_EQ (e.data.nodes[n].z, 5.02);
    e.Begin ();
    e.data.nodes[n].z = 5.04;
    e.Cancel ();
    EXPECT_DOUBLE_EQ (e.data.nodes[n].z, 5.02);
}
TEST (RoofSlope, NativeSessionBorrowsLockAndDoesNotRepublishForCameraMotion)
{
    auto state = hud::NewState ();
    auto data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    auto layer = rs::Layer (data);
    EXPECT_EQ (geomsrv::archviz::overlaylayers::Validate (layer), "");
    geomsrv::archviz::overlaylayers::Set (layer);
    hud::StartRoofEditor (*state, data);
    EXPECT_TRUE (hud::EditLocked (*state));
    bool clear = false;
    EXPECT_FALSE (hud::FollowRoofEditor (*state, layer, clear));
    geomsrv::archviz::overlaylayers::ClearCommandScoped ();
    EXPECT_FALSE (hud::FollowRoofEditor (*state, layer, clear));
    EXPECT_FALSE (hud::RoofEditorActive (*state));
    EXPECT_FALSE (hud::EditLocked (*state));
    hud::SetEditLocked (*state, true);
    hud::StartRoofEditor (*state, data);
    hud::StopRoofEditor (*state);
    EXPECT_TRUE (hud::EditLocked (*state));
}
TEST (RoofSlope, InverseViewProjectionUsesTheActualTransformAndRefusesEdgeOn)
{
    auto d = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    hud::Input v;
    v.planar = true;
    v.plan[0] = 20, v.plan[2] = 100, v.plan[4] = -20, v.plan[5] = 500;
    rs::Vec out;
    ASSERT_TRUE (rh::OntoPlane (v, d, 160, 360, 5, {}, out));
    auto world = rs::World (d, out);
    EXPECT_NEAR (world.x, 3, 0.001);
    EXPECT_NEAR (world.y, 7, 0.001);
    v.plan[4] = 0;
    EXPECT_FALSE (rh::OntoPlane (v, d, 160, 360, 5, {}, out));
}
TEST (RoofSlope, NativeCanvasDragsTheDrainAndCommitsOneVirtualUndo)
{
    rh::Session s;
    s.editor.data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    hud::Input v;
    v.planar = true;
    v.plan[0] = 20, v.plan[2] = 100, v.plan[4] = -20, v.plan[5] = 500;
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 800 };
    io.DeltaTime = 1.0f / 60;
    ASSERT_NE (io.Fonts->AddFontFromFileTTF (EVP_SCENE_TEXT_FONT, 16), nullptr);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    auto frame = [&] (ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 1000, 800 });
        ImGui::Begin ("##roofTest", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
        rh::OnView (s, v, true, 1);
        ImGui::End ();
        ImGui::Render ();
    };
    frame ({ 200, 400 }, false);
    frame ({ 200, 400 }, true);
    ASSERT_EQ (s.drag, 1);
    frame ({ 220, 400 }, true);
    frame ({ 220, 400 }, false);
    EXPECT_EQ (s.drag, 0);
    ASSERT_EQ (s.editor.undo.size (), 1u);
    const auto& c = s.editor.data.cells[0];
    const auto at = rs::World (s.editor.data, s.editor.data.nodes[c.drain].xy);
    EXPECT_NEAR (at.x, 6, 0.01);
    EXPECT_NEAR (at.y, 5, 0.01);
    EXPECT_TRUE (s.editor.Undo (true));
    ImGui::DestroyContext (context);
    ImGui::SetCurrentContext (previous);
}
TEST (RoofSlope, AnExactDimensionCanMoveTheOppositeDividerWhenTheOuterSideIsFixed)
{
    rh::Session s;
    s.editor.data = Plan ({ { { 0, 0 }, { 30, 0 }, { 30, 10 }, { 0, 10 } } });
    int cell = -1;
    for (int i = 0; i < int (s.editor.data.cells.size ()); ++i)
        if (std::abs (s.editor.data.cells[i].box[2] - 30) < 1e-5)
            cell = i;
    ASSERT_GE (cell, 0);
    const auto box = s.editor.data.cells[cell].box;
    rh::NumberEdit edit { s.editor.revision, -1, cell, 2, box[2] - box[0], "width" };
    EXPECT_TRUE (rh::Answer (s, edit, box[2] - box[0] - 0.25)) << s.editor.message;
    EXPECT_NEAR (s.editor.data.cells[cell].box[2] - s.editor.data.cells[cell].box[0], box[2] - box[0] - 0.25, 1e-5);
}
TEST (RoofSlope, AClippedOutCentreIsFlaggedWithoutPublishingAnEmptyMarkerSet)
{
    auto d = Plan ({ { { 0, 0 }, { 10, 0 }, { 0, 10 } } });
    std::string error;
    ASSERT_TRUE (rs::Reorient (d, 0, error)) << error;
    ASSERT_EQ (d.cells.size (), 1u);
    EXPECT_FALSE (d.cells[0].drainInside);
    auto layer = rs::Layer (d);
    EXPECT_TRUE (layer.points.empty ());
    EXPECT_EQ (geomsrv::archviz::overlaylayers::Validate (layer), "");
    EXPECT_NEAR (Area (d), 50, 1e-5);
}
