#include "ArchViz/RoofSlope.hpp"
#include "ArchViz/RoofSlopeHud.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayScene.hpp"
#include "hud_fixture.hpp"
#include <imgui.h>
#include <gtest/gtest.h>
#include <clipper2/clipper.h>
#include <cmath>
#include <limits>
#include <set>
#include <chrono>

namespace rs = geomsrv::archviz::roofslope;
namespace hud = geomsrv::archviz::overlayhud;
namespace rh = geomsrv::archviz::roofhud;
namespace cp = Clipper2Lib;
namespace {
rs::Data Plan (std::vector<rs::Ring> rings, double top = 5, double cap = 200)
{
    rs::Data d;
    std::string error;
    EXPECT_TRUE (rs::Plan (rings, top, 2, cap, d, error)) << error;
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
TEST (RoofSlope, CapturedOctober13SlabOpensFourFaceDrainageCells)
{
    auto d = Plan ({ { { -69.464325192, -14.118220441 },
                       { -40.040897839, 2.183307455 },
                       { -41.285217693, 4.429241398 },
                       { -14.922616675, 19.034972769 },
                       { -22.553701043, 32.808691278 },
                       { -56.568367899, 13.963468592 },
                       { -55.446954068, 11.939373758 },
                       { -77.218315582, -0.122662823 } } });
    ASSERT_FALSE (d.cells.empty ());
    EXPECT_NEAR (Area (d), 1033.010698253, 1e-3);
    for (size_t ci = 0; ci < d.cells.size (); ++ci) {
        const auto& c = d.cells[ci];
        ASSERT_EQ (c.rim.size (), 4u);
        const auto& drain = d.nodes[c.drain];
        EXPECT_NEAR (drain.xy.x, (c.box[0] + c.box[2]) / 2, 1e-6);
        EXPECT_NEAR (drain.xy.y, (c.box[1] + c.box[3]) / 2, 1e-6);
        for (int n : c.rim)
            EXPECT_GT (d.nodes[n].z, drain.z);
        std::set<std::array<int, 3>> faces;
        for (const auto& t : d.triangles)
            if (t.cell == int (ci)) {
                faces.insert (t.nodes);
                rs::Vec center {};
                for (const auto& n : t.vertices)
                    center.x += n.xy.x / 3, center.y += n.xy.y / 3;
                EXPECT_LT (t.gx * (drain.xy.x - center.x) + t.gy * (drain.xy.y - center.y), 0);
            }
        EXPECT_EQ (faces.size (), 4u) << "rectangle " << ci;
    }
    const auto shapes = Shapes (d);
    const auto united = cp::Union (shapes, cp::FillRule::NonZero, 6);
    cp::PathsD outline;
    for (const auto& ring : d.outline) {
        cp::PathD p;
        for (const auto& n : ring)
            p.emplace_back (n.x, n.y);
        outline.push_back (p);
    }
    EXPECT_LT (std::abs (cp::Area (cp::Xor (united, outline, cp::FillRule::NonZero, 6))), 1e-4);
    EXPECT_NEAR (Area (d), std::abs (cp::Area (united)), 1e-4);
    const auto layer = rs::Layer (d);
    EXPECT_EQ (layer.texts.size (), 4 * d.cells.size ());
    size_t arrows = 0;
    for (const auto& line : layer.polylines)
        if (line.endArrow == geomsrv::archviz::overlaylayers::Terminator::Arrow) {
            ++arrows;
            EXPECT_LT (line.points[5], line.points[2]);
        }
    EXPECT_EQ (arrows, 4 * d.cells.size ());
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
TEST (RoofSlope, October13EveningCaptureAndLongRoofsNeverProduceElongatedEnvelopes)
{
    for (const auto& rings : std::vector<std::vector<rs::Ring>> { { { { -43.853246409, -2.878364638 },
                                                                      { -11.898320409, 4.222730029 },
                                                                      { -15.369207734, 19.841722992 },
                                                                      { -45.781482134, 13.083439792 },
                                                                      { -46.173266667, 14.846470192 },
                                                                      { -72.563627967, 8.981945458 },
                                                                      { -69.294675767, -5.728339442 },
                                                                      { -44.446966067, -0.206626175 } } },
                                                                  { { { 0, 0 }, { 80, 0 }, { 80, 4 }, { 0, 4 } } } }) {
        auto d = Plan (rings);
        ASSERT_FALSE (d.cells.empty ());
        const double expected = rings[0][0].x < 0 ? 910.172019233 : 320;
        EXPECT_NEAR (Area (d), expected, 1e-3);
        const auto measures = rs::Measure (d);
        EXPECT_LE (measures.maxAspect, rs::kMaxAspect + 1e-6);
        EXPECT_LT (measures.maxBorderRise, 0.2);
        for (const auto& c : d.cells) {
            const double w = c.box[2] - c.box[0], h = c.box[3] - c.box[1];
            EXPECT_LE ((std::max) (w, h) / (std::min) (w, h), rs::kMaxAspect + 1e-6);
            EXPECT_LT (w * h, 200);
        }
    }
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
    ASSERT_NE (state->roofEditor, nullptr);
    EXPECT_EQ (state->roofEditor->editor.selectedCell, 0);
    EXPECT_EQ (state->roofEditor->editor.selectedNode, data.cells[0].drain);
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
TEST (RoofSlope, OpeningTheEditorDrawsItsGripsAndSelectedDrainGumballWithoutAClick)
{
    hudtest::Watched view;
    auto data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    auto layer = rs::Layer (data);
    geomsrv::archviz::overlaylayers::Set (layer);
    ASSERT_TRUE (hud::StartRoofEditor (*view.state, data));
    view.engine.SetLayers ({ rs::kLayer });
    auto input = hudtest::At (950, 700);
    input.planar = true;
    input.plan[0] = 20, input.plan[2] = 600, input.plan[4] = -20, input.plan[5] = 500;
    hud::Layout out;
    std::string error;
    ASSERT_TRUE (view.engine.Build ({ &layer.panels[0] }, { rs::kLayer }, 1, input, {}, out, error)) << error;
    auto control = [&] (float x, float y, uint32_t rgba) {
        for (const auto& v : out.overlay.vertices)
            if (std::abs (v.x - x) <= 6 && std::abs (v.y - y) <= 6 && v.rgba == rgba)
                return true;
        return false;
    };
    EXPECT_TRUE (control (600, 500, 0x5A7F62FF)); // corner grip
    EXPECT_TRUE (control (748, 400, 0x5A7F62FF)); // selected drain U gumball
    EXPECT_TRUE (control (700, 352, 0x5A7F62FF)); // selected drain V gumball
    size_t anchors = 0, axes = 0;
    for (const auto& v : out.overlay.vertices) {
        anchors += v.modelAnchored;
        axes += v.modelAnchored && v.turn;
    }
    EXPECT_GT (anchors, 0u);
    EXPECT_GT (axes, 0u);
    const auto drainWorld = rs::World (data, data.nodes[data.cells[0].drain].xy);
    bool areaButton = false;
    for (const auto& v : out.overlay.vertices)
        if (v.modelAnchored && std::abs (v.model[0] - drainWorld.x) < 1e-6 &&
            std::abs (v.model[1] - drainWorld.y) < 1e-6 && v.rgba == 0xF0F5F1FF && v.modelOffset[0] > 8 &&
            v.modelOffset[1] < -16 && v.modelOffset[1] > -26)
            areaButton = true;
    EXPECT_FALSE (areaButton) << "net area is a readout, not an editable button";
    const auto plan = geomsrv::archviz::overlayscene::PreparePlanHud (
        { std::make_shared<const geomsrv::archviz::overlaylayers::Layer> (layer) }, &view.engine, 1, input);
    size_t projected = 0;
    for (const auto& glyph : plan.glyphs)
        projected += (glyph.flags & geomsrv::archviz::overlayscene::kPlainTexture) != 0 &&
                     (glyph.flags & geomsrv::archviz::overlayscene::kScreenAnchored) == 0;
    EXPECT_GT (projected, 0u) << "roof furniture uses the current Present's projection, not layout screen pixels";
    hud::StopRoofEditor (*view.state);
    geomsrv::archviz::overlaylayers::Clear (rs::kLayer);
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
TEST (RoofSlope, InlineNumberReceivesKeyboardAndEnterWithoutANativePopup)
{
    hudtest::Watched view;
    auto data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    auto layer = rs::Layer (data);
    geomsrv::archviz::overlaylayers::Set (layer);
    ASSERT_TRUE (hud::StartRoofEditor (*view.state, data));
    auto& s = *view.state->roofEditor;
    const int node = data.cells[0].drain;
    s.number = rh::NumberEdit { s.editor.revision, node, 0, -1, 5, "Elevation", s.token };
    std::snprintf (s.numberText, sizeof (s.numberText), "5");
    s.focusNumber = true;
    auto input = hudtest::At (950, 700);
    input.planar = true;
    input.plan[0] = 20, input.plan[2] = 600, input.plan[4] = -20, input.plan[5] = 500;
    auto frame = [&] () {
        hud::Layout out;
        std::string error;
        EXPECT_TRUE (
            view.engine.Build ({ &layer.panels[0] }, { std::string (rs::kLayer) + "#0" }, 1, input, {}, out, error))
            << error;
        input.keys.clear (), input.characters.clear ();
    };
    frame ();
    input.characters = u"5.02";
    frame ();
    input.keys.push_back ({ 0x2E, true });
    frame ();
    EXPECT_TRUE (s.number.has_value ());
    EXPECT_TRUE (s.editor.undo.empty ()) << "Delete edits the active text field, not virtual geometry";
    input.keys.push_back ({ 0x2E, false });
    frame ();
    input.keys.push_back ({ 0x0D, true });
    frame ();
    EXPECT_FALSE (s.number.has_value ());
    EXPECT_NEAR (s.editor.data.nodes[node].z, 5.02, 1e-8);
    EXPECT_EQ (s.editor.undo.size (), 1u);
    input.keys.push_back ({ 0x0D, false });
    frame ();
    s.number = rh::NumberEdit { s.editor.revision, node, 0, -1, 5.02, "Elevation", s.token };
    std::snprintf (s.numberText, sizeof (s.numberText), "5.02");
    s.focusNumber = true;
    frame ();
    input.characters = u"5.04";
    frame ();
    input.keys.push_back ({ 0x1B, true });
    frame ();
    EXPECT_FALSE (s.number.has_value ());
    EXPECT_FALSE (s.close) << "Esc cancels a field, not the whole editor";
    EXPECT_NEAR (s.editor.data.nodes[node].z, 5.02, 1e-8);
    hud::StopRoofEditor (*view.state);
    geomsrv::archviz::overlaylayers::Clear (rs::kLayer);
}
TEST (RoofSlope, DeleteChangesOnlyTheVirtualFootprintAndCanBeUndone)
{
    rh::Session s;
    s.editor.data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    s.footprint = true, s.ring = 0, s.vertex = 1;
    ASSERT_TRUE (rh::DeleteSelection (s)) << s.editor.message;
    EXPECT_EQ (s.editor.data.outline[0].size (), 3u);
    EXPECT_EQ (s.editor.undo.size (), 1u);
    ASSERT_TRUE (s.editor.Undo (true));
    EXPECT_EQ (s.editor.data.outline[0].size (), 4u);
    s.footprint = false;
    EXPECT_FALSE (rh::DeleteSelection (s));
    EXPECT_EQ (s.editor.data.outline[0].size (), 4u);
}
TEST (RoofSlope, FloatingPanelKeepsDefaultImGuiAndCssIsOnlyTheEditingCanvas)
{
    const auto panel = rs::PanelStyle (), canvas = rs::EditorStyle ();
    EXPECT_TRUE (panel.defaultImGuiStyle);
    EXPECT_TRUE (panel.font.empty ());
    EXPECT_FALSE (canvas.defaultImGuiStyle);
    EXPECT_EQ (canvas.font, "tapioca:roof-inter");
}
TEST (RoofSlope, CapturedRoofGeometryEditsFitAnInteractiveFrameBudget)
{
    auto d = Plan ({ { { -43.853246409, -2.878364638 },
                       { -11.898320409, 4.222730029 },
                       { -15.369207734, 19.841722992 },
                       { -45.781482134, 13.083439792 },
                       { -46.173266667, 14.846470192 },
                       { -72.563627967, 8.981945458 },
                       { -69.294675767, -5.728339442 },
                       { -44.446966067, -0.206626175 } } });
    int cell = -1;
    for (size_t i = 0; i < d.cells.size (); ++i)
        if (d.cells[i].drainInside) {
            cell = int (i);
            break;
        }
    ASSERT_GE (cell, 0);
    const auto start = d.nodes[d.cells[cell].drain].xy;
    const auto began = std::chrono::steady_clock::now ();
    for (int i = 0; i < 40; ++i) {
        std::string error;
        ASSERT_TRUE (rs::MoveDrain (d, cell, { start.x + 0.01 * std::sin (i), start.y }, error)) << error;
        EXPECT_EQ (geomsrv::archviz::overlaylayers::Validate (rs::Layer (d)), "");
    }
    const double ms =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - began).count () / 40;
    RecordProperty ("meanRoofGeometryEditMs", std::to_string (ms));
    RecordProperty ("roofRectangles", int (d.cells.size ()));
    EXPECT_LT (ms, 16) << "CPU geometry/publication preparation, not a live GPU or navigation measurement";
}
TEST (RoofSlope, VirtualDividerDeletionMergesOnlyWithinAreaAspectAndLockLimits)
{
    const auto original = Plan ({ { { 0, 0 }, { 14, 0 }, { 14, 10 }, { 0, 10 } } }, 5, 100);
    ASSERT_EQ (original.cells.size (), 2u);
    int cell = -1, side = -1;
    for (int ci = 0; ci < 2; ++ci)
        for (int s = 0; s < 4; ++s)
            if (std::abs (original.cells[ci].box[s] - 7) < 1e-6)
                cell = ci, side = s;
    ASSERT_GE (cell, 0);
    std::string error;
    auto data = original;
    EXPECT_FALSE (rs::RemoveDivider (data, cell, side, error));
    EXPECT_EQ (data.cells.size (), 2u);
    // A larger project cap makes this pair eligible without silently relaxing
    // the aspect rule or losing controls on an unrelated rectangle.
    data.cap = 200;
    data.nodes[data.cells[cell].drain].lockXY = true;
    EXPECT_FALSE (rs::RemoveDivider (data, cell, side, error));
    EXPECT_EQ (data.cells.size (), 2u);
    data.nodes[data.cells[cell].drain].lockXY = false;
    ASSERT_TRUE (rs::RemoveDivider (data, cell, side, error)) << error;
    EXPECT_EQ (data.cells.size (), 1u);
    EXPECT_NEAR (Area (data), 140, 1e-4);
    EXPECT_LE (rs::Measure (data).maxAspect, rs::kMaxAspect);
    EXPECT_FALSE (rs::RemoveDivider (data, 0, 0, error));
    auto longRoof = Plan ({ { { 0, 0 }, { 16, 0 }, { 16, 8 }, { 0, 8 } } });
    for (int ci = 0; ci < int (longRoof.cells.size ()); ++ci)
        for (int s = 0; s < 4; ++s)
            EXPECT_FALSE (rs::RemoveDivider (longRoof, ci, s, error));
}
TEST (RoofSlope, HidingOrUnlockingCancelsInlineFields)
{
    auto state = hud::NewState ();
    auto data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    auto layer = rs::Layer (data);
    geomsrv::archviz::overlaylayers::Set (layer);
    ASSERT_TRUE (hud::StartRoofEditor (*state, data));
    auto& s = *state->roofEditor;
    s.number = rh::NumberEdit { s.editor.revision, data.cells[0].drain, 0, -1, 5, "Elevation", s.token };
    hud::SetEditLocked (*state, false);
    bool clear = false;
    hud::FollowRoofEditor (*state, layer, clear);
    EXPECT_FALSE (s.number.has_value ());
    hud::SetEditLocked (*state, true);
    s.number = rh::NumberEdit { s.editor.revision, data.cells[0].drain, 0, -1, 5, "Elevation", s.token };
    state->shown = false;
    hud::FollowRoofEditor (*state, layer, clear);
    EXPECT_FALSE (s.number.has_value ());
    hud::StopRoofEditor (*state);
    geomsrv::archviz::overlaylayers::Clear (rs::kLayer);
}
TEST (RoofSlope, PlanHudFingerprintIncludesTheModelAnchorPackingOrigin)
{
    geomsrv::archviz::overlayscene::Plan before;
    before.originX = 100, before.originY = 200;
    const auto hash = geomsrv::archviz::overlayscene::Fingerprint (before);
    auto after = before;
    after.originX += 10;
    EXPECT_NE (hash, geomsrv::archviz::overlayscene::Fingerprint (after));
    after = before;
    after.originY += 10;
    EXPECT_NE (hash, geomsrv::archviz::overlayscene::Fingerprint (after));
}
