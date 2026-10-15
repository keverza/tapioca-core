// The Plan view headless: one building's floor on a canvas the panel's width, its neighbour as
// grey context with a thick blind wall between them, a stair dragged in 0.4 m steps without the
// frame waiting for the generator, and the moved stair the building's own.
#include "ArchViz/FloorSchemeEdit.hpp"
#include "ArchViz/HudFloorSchemeEdit.hpp"
#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <thread>
#include "imgui.h"
#include "imgui_internal.h"

namespace bp = geomsrv::archviz::buildingplan;
namespace fs = geomsrv::archviz::floorscheme;
namespace fp = geomsrv::archviz::floorprogramme;
namespace hf = geomsrv::archviz::hudfloorscheme;
namespace hud = geomsrv::archviz::overlayhud;

namespace {
geomsrv::archviz::SliceChain Chain (double x0, double y0, double x1, double y1)
{
    geomsrv::archviz::SliceChain c;
    c.closed = true;
    c.xy = { x0, y0, x1, y0, x1, y1, x0, y1 };
    return c;
}
bp::Plan Building (const char* key, double x0, double x1)
{
    bp::Plan plan;
    plan.key = key;
    bp::Floor floor;
    floor.z = 0;
    floor.height = 3;
    floor.areaM2 = (x1 - x0) * 16;
    floor.outlineKnown = true;
    floor.outline = { Chain (x0, 0, x1, 16) };
    floor.contours = { floor.outline };
    floor.outlineKey = key;
    plan.floors = { floor };
    return plan;
}
// Screen bounds of what was drawn in `colour` this frame, inside `within` when given.
ImVec4 Bounds (ImU32 colour, ImVec4 within = { -1e6f, -1e6f, 1e6f, 1e6f })
{
    ImVec4 b { 1e6f, 1e6f, -1e6f, -1e6f };
    for (const auto& v : ImGui::GetWindowDrawList ()->VtxBuffer)
        if (v.col == colour && v.pos.x >= within.x && v.pos.y >= within.y && v.pos.x <= within.z && v.pos.y <= within.w)
            b = { (std::min) (b.x, v.pos.x), (std::min) (b.y, v.pos.y), (std::max) (b.z, v.pos.x),
                  (std::max) (b.w, v.pos.y) };
    return b;
}
} // namespace

TEST (HudFloorSchemeEdit, UnitCaptionsUseRoomCountAndActualNetAreaWithoutProgrammeRanges)
{
    fs::Flat flat;
    flat.rooms = 3;
    flat.net = 61.25;
    flat.type = 2; // The repeated 3R type's name contains an area range; the caption must not.
    const auto [rooms, area] = hf::detail::FlatLabel (flat);
    EXPECT_EQ (rooms, "3R");
    EXPECT_EQ (area, "61.2 m2");
    flat.rooms = 1.5;
    flat.manual = true;
    EXPECT_EQ (hf::detail::FlatLabel (flat).first, "1.5R");
}

TEST (HudFloorSchemeEdit, TwoLineCaptionsFitActualPolygonsNotJustFrontage)
{
    hf::View view;
    const fs::Ring square { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } };
    EXPECT_TRUE (hf::detail::LabelFits (square, view, { 50, 50 }, { 50, 28 }, 2));
    EXPECT_FALSE (hf::detail::LabelFits (square, view, { 50, 50 }, { 100, 28 }, 2));
    const fs::Ring thin { { 0, 0 }, { 100, 0 }, { 100, 20 }, { 0, 20 } };
    EXPECT_FALSE (hf::detail::LabelFits (thin, view, { 50, 10 }, { 50, 28 }, 2));
    const fs::Ring rotated { { 0, 50 }, { 50, 0 }, { 100, 50 }, { 50, 100 } };
    EXPECT_FALSE (hf::detail::LabelFits (rotated, view, { 50, 50 }, { 80, 40 }, 2));
    const fs::Ring concave { { 0, 0 },   { 100, 0 }, { 100, 100 }, { 55, 100 },
                             { 55, 40 }, { 45, 40 }, { 45, 100 },  { 0, 100 } };
    EXPECT_FALSE (hf::detail::LabelFits (concave, view, { 50, 50 }, { 50, 28 }, 2));
}

TEST (HudFloorSchemeEdit, SelectedUnitDimensionsMeasureFinalShapeAndPutLengthAtTheFacade)
{
    fs::Scheme scheme;
    scheme.outline = { { { 0, 0 }, { 20, 0 }, { 20, 16 }, { 0, 16 } } };
    fs::Flat flat;
    flat.shape = { { 4, 0 }, { 10.4, 0 }, { 10.4, 7.2 }, { 4, 7.2 } };
    flat.frontage = 5, flat.depth = 6; // The mould changed the shape after division.
    scheme.flats = { flat };
    const auto dims = hf::detail::FlatDimensions (scheme, 0);
    ASSERT_TRUE (dims.has_value ());
    EXPECT_EQ (hf::detail::DimensionText ((*dims)[0], "L"), "L 6.40 m");
    EXPECT_EQ (hf::detail::DimensionText ((*dims)[1], "D"), "D 7.20 m");
    EXPECT_NEAR ((*dims)[0].a.y, 0, 1e-9);
    EXPECT_NEAR ((*dims)[0].b.y, 0, 1e-9);
    EXPECT_LT ((*dims)[0].outward.y, 0) << "outside, not towards the corridor";
    EXPECT_NEAR ((*dims)[1].a.x, 10.4, 1e-9);
    EXPECT_NEAR ((*dims)[1].b.x, 10.4, 1e-9);
    EXPECT_GT ((*dims)[1].outward.x, 0);
    std::reverse (scheme.flats[0].shape.begin (), scheme.flats[0].shape.end ());
    scheme.flats[0].axis = { -3, 0 };
    const auto reversed = hf::detail::FlatDimensions (scheme, 0);
    ASSERT_TRUE (reversed.has_value ());
    EXPECT_NEAR ((*reversed)[0].a.x, (*dims)[0].a.x, 1e-9);
    EXPECT_NEAR ((*reversed)[0].b.x, (*dims)[0].b.x, 1e-9);
    EXPECT_EQ ((*reversed)[0].outward, (*dims)[0].outward);
}

TEST (HudFloorSchemeEdit, DimensionsFollowRotatedUnitsOnBothFacades)
{
    for (double angle : { 0.0, 0.37, 1.57079632679, 2.4 })
        for (bool upper : { false, true }) {
            const fs::Vec u { std::cos (angle), std::sin (angle) }, n { -u.y, u.x };
            const auto world = [&] (fs::Vec p) {
                return fs::Vec { 100000 + u.x * p.x + n.x * p.y, 600000 + u.y * p.x + n.y * p.y };
            };
            fs::Scheme scheme;
            scheme.outline = { { { 0, 0 }, { 20, 0 }, { 20, 16 }, { 0, 16 } } };
            fs::Flat flat;
            const double y0 = upper ? 9 : 0, y1 = upper ? 16 : 7;
            flat.shape = { { 4, y0 }, { 10, y0 }, { 10, y1 }, { 4, y1 } };
            flat.axis = u;
            for (auto& p : scheme.outline[0])
                p = world (p);
            for (auto& p : flat.shape)
                p = world (p);
            scheme.flats = { flat };
            const auto dims = hf::detail::FlatDimensions (scheme, 0);
            ASSERT_TRUE (dims.has_value ()) << angle;
            const auto& length = (*dims)[0];
            EXPECT_NEAR (std::hypot (length.b.x - length.a.x, length.b.y - length.a.y), 6, 1e-7);
            const auto& depth = (*dims)[1];
            EXPECT_NEAR (std::hypot (depth.b.x - depth.a.x, depth.b.y - depth.a.y), 7, 1e-7);
            EXPECT_NEAR (length.outward.x * n.x + length.outward.y * n.y, upper ? 1 : -1, 1e-9);
            EXPECT_NEAR ((length.b.x - length.a.x) * u.y - (length.b.y - length.a.y) * u.x, 0, 1e-7);
        }
}

TEST (HudFloorSchemeEdit, EquallyExposedFacadesDoNotFlipWhenRingOrAxisIsReversed)
{
    fs::Scheme scheme;
    fs::Flat flat;
    flat.shape = { { 0, 0 }, { 6, 0 }, { 6, 7 }, { 0, 7 } };
    flat.through = true;
    scheme.outline = { flat.shape };
    scheme.flats = { flat };
    const auto before = hf::detail::FlatDimensions (scheme, 0);
    ASSERT_TRUE (before);
    std::reverse (scheme.flats[0].shape.begin (), scheme.flats[0].shape.end ());
    scheme.flats[0].axis = { -1, 0 };
    const auto after = hf::detail::FlatDimensions (scheme, 0);
    ASSERT_TRUE (after);
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ ((*before)[i].a, (*after)[i].a);
        EXPECT_EQ ((*before)[i].b, (*after)[i].b);
        EXPECT_EQ ((*before)[i].outward, (*after)[i].outward);
    }
}

TEST (HudFloorSchemeEdit, ASteppedUnitUsesOverallExtentsRatherThanTwiceItsVertexMean)
{
    fs::Scheme scheme;
    scheme.outline = { { { 0, 0 }, { 20, 0 }, { 20, 16 }, { 0, 16 } } };
    fs::Flat flat;
    flat.shape = { { 4, 0 }, { 10, 0 }, { 10, 7 }, { 8, 7 }, { 8, 9 }, { 4, 9 } };
    scheme.flats = { flat };
    const auto dims = hf::detail::FlatDimensions (scheme, 0);
    ASSERT_TRUE (dims.has_value ());
    EXPECT_EQ (hf::detail::DimensionText ((*dims)[0], "L"), "L 6.00 m");
    EXPECT_EQ (hf::detail::DimensionText ((*dims)[1], "D"), "D 9.00 m");
    EXPECT_LT ((*dims)[0].outward.y, 0);
}

TEST (HudFloorSchemeEdit, CourtyardFacadesPointIntoTheHoleNotIntoTheBuilding)
{
    fs::Scheme scheme;
    scheme.outline = { { { 0, 0 }, { 20, 0 }, { 20, 20 }, { 0, 20 } }, { { 6, 6 }, { 6, 14 }, { 14, 14 }, { 14, 6 } } };
    fs::Flat flat;
    flat.shape = { { 7, 2 }, { 13, 2 }, { 13, 6 }, { 7, 6 } };
    scheme.flats = { flat };
    const auto dims = hf::detail::FlatDimensions (scheme, 0);
    ASSERT_TRUE (dims.has_value ());
    EXPECT_NEAR ((*dims)[0].a.y, 6, 1e-9);
    EXPECT_GT ((*dims)[0].outward.y, 0);
    EXPECT_FALSE (hf::detail::Inside (scheme.outline, { 10, 6.1 }));
}

TEST (HudFloorSchemeEdit, ABlindPartyWallDoesNotWinOverTheUnitsOtherFacade)
{
    fs::Scheme scheme;
    scheme.outline = { { { 0, 0 }, { 20, 0 }, { 20, 16 }, { 0, 16 } } };
    scheme.party = { { fs::Vec { 4, 0 }, fs::Vec { 10, 0 } } };
    fs::Flat flat;
    flat.shape = { { 4, 0 }, { 10, 0 }, { 10, 16 }, { 4, 16 } };
    scheme.flats = { flat };
    const auto dims = hf::detail::FlatDimensions (scheme, 0);
    ASSERT_TRUE (dims.has_value ());
    EXPECT_NEAR ((*dims)[0].a.y, 16, 1e-9);
    EXPECT_GT ((*dims)[0].outward.y, 0);
}

TEST (HudFloorSchemeEdit, AnEndCapWithOnlyAGableFacadeDimensionsAlongThatFacade)
{
    fs::Scheme scheme;
    scheme.outline = { { { 0, 0 }, { 20, 0 }, { 20, 16 }, { 0, 16 } } };
    fs::Flat flat;
    flat.shape = { { 0, 2 }, { 5, 2 }, { 5, 14 }, { 0, 14 } };
    flat.cap = true;
    scheme.flats = { flat };
    const auto dims = hf::detail::FlatDimensions (scheme, 0);
    ASSERT_TRUE (dims.has_value ());
    EXPECT_EQ (hf::detail::DimensionText ((*dims)[0], "L"), "L 12.00 m");
    EXPECT_EQ (hf::detail::DimensionText ((*dims)[1], "D"), "D 5.00 m");
    EXPECT_NEAR ((*dims)[0].a.x, 0, 1e-9);
    EXPECT_LT ((*dims)[0].outward.x, 0);
}

TEST (HudFloorSchemeEdit, InvalidOrEmptyUnitSelectionsHaveNoDimensions)
{
    fs::Scheme scheme;
    EXPECT_FALSE (hf::detail::FlatDimensions (scheme, -1));
    EXPECT_FALSE (hf::detail::FlatDimensions (scheme, 0));
    scheme.flats.emplace_back ();
    EXPECT_FALSE (hf::detail::FlatDimensions (scheme, 0));
    scheme.flats[0].shape = { { 0, 0 }, { 1, 0 }, { 2, 0 } };
    EXPECT_FALSE (hf::detail::FlatDimensions (scheme, 0));
}

TEST (HudFloorSchemeEdit, FlatsOfTheSameRoomCountUseTheirOwnProgrammeColours)
{
    auto programme = fp::Default ();
    ASSERT_TRUE (fp::SetColour (programme, 2, 0x102040FFu));
    ASSERT_TRUE (fp::SetColour (programme, 3, 0xEF5050FFu));
    fs::Scheme scheme;
    fs::Flat first, second;
    first.type = 2, second.type = 3;
    first.rooms = second.rooms = 3;
    first.shape = second.shape = { { 0, 0 }, { 5, 0 }, { 5, 5 }, { 0, 5 } };
    scheme.flats = { first, second };
    const auto fills = hf::detail::Fills (scheme, programme);
    ASSERT_EQ (fills.size (), 3u);
    EXPECT_EQ (fills[1].colour, IM_COL32 (16, 32, 64, 255));
    EXPECT_EQ (fills[2].colour, IM_COL32 (239, 80, 80, 255));
}

TEST (HudFloorSchemeEdit, CanvasDrawsRoomAndAreaOnSeparateCentredLines)
{
    auto* previous = ImGui::GetCurrentContext ();
    auto* context = ImGui::CreateContext ();
    const auto cleanup = [previous] (ImGuiContext* value) {
        ImGui::DestroyContext (value);
        ImGui::SetCurrentContext (previous);
    };
    std::unique_ptr<ImGuiContext, decltype (cleanup)> held (context, cleanup);
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 1400 };
    io.DeltaTime = 1.0f / 60;
    ASSERT_NE (io.Fonts->AddFontFromFileTTF (EVP_SCENE_TEXT_FONT, 14), nullptr);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    bp::Planner planner;
    const std::map<std::string, bp::Plan> plans;
    std::map<std::string, bp::Draft> drafts;
    const auto programme = fp::Default ();
    const fs::Options options;
    hf::Context c { planner, plans, drafts, programme, options };
    hf::Editor editor;
    editor.input.owned = { { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } };
    fs::Flat flat;
    flat.rooms = 3, flat.net = 61.25, flat.type = 2;
    flat.shape = { { 1, 1 }, { 9, 1 }, { 9, 9 }, { 1, 9 } };
    editor.scheme.flats = { flat };
    ImGui::NewFrame ();
    ImGui::SetNextWindowPos ({ 0, 0 });
    ImGui::SetNextWindowSize ({ 600, 1200 });
    ImGui::Begin ("labels", nullptr, ImGuiWindowFlags_NoSavedSettings);
    hf::detail::Canvas (editor, c, 1);
    const auto lo = ImGui::GetItemRectMin (), hi = ImGui::GetItemRectMax ();
    const ImVec2 middle { (lo.x + hi.x) / 2, (lo.y + hi.y) / 2 };
    const ImVec4 above = Bounds (IM_COL32 (20, 25, 28, 255), { 0, 0, 1000, middle.y });
    const ImVec4 below = Bounds (IM_COL32 (20, 25, 28, 255), { 0, middle.y, 1000, 1400 });
    EXPECT_GT (above.z, above.x);
    EXPECT_GT (below.z, below.x);
    EXPECT_LT (above.w, below.y);
    EXPECT_NEAR ((above.x + above.z) / 2, middle.x, 1);
    EXPECT_NEAR ((below.x + below.z) / 2, middle.x, 1);
    ImGui::End ();
    ImGui::Render ();
}

TEST (HudFloorSchemeEdit, ABuildingIsEditedAmongItsNeighbourAndAStairDragsWithoutStallingTheFrame)
{
    auto* previous = ImGui::GetCurrentContext ();
    auto* context = ImGui::CreateContext ();
    const auto cleanup = [previous] (ImGuiContext* value) {
        ImGui::DestroyContext (value);
        ImGui::SetCurrentContext (previous);
    };
    std::unique_ptr<ImGuiContext, decltype (cleanup)> held (context, cleanup);
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 1400 };
    io.DeltaTime = 1.0f / 60;
    ASSERT_NE (io.Fonts->AddFontFromFileTTF (EVP_SCENE_TEXT_FONT, 14), nullptr);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);

    // Two buildings side by side at one elevation: A is shown, B is its context.
    const std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 36) }, { "B", Building ("B", 36, 60) } };
    std::map<std::string, bp::Draft> drafts;
    bp::Planner planner;
    hf::EditorPtr editor;
    const auto programme = fp::Default ();
    ImVec4 floor {}, around {}, cores {}, party {};
    const auto frame = [&] (ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 960, 1380 });
        ImGui::Begin ("Plan", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        hf::PlanView (editor, planner, plans, drafts, "A", programme, {}, 1);
        floor = Bounds (IM_COL32 (236, 238, 232, 255));
        around = Bounds (IM_COL32 (205, 207, 203, 255));
        cores = Bounds (IM_COL32 (109, 114, 118, 255), floor);
        party = Bounds (IM_COL32 (40, 44, 48, 255), { floor.x - 20, floor.y - 20, floor.z + 20, floor.w + 20 });
        ImGui::End ();
        ImGui::Render ();
    };
    // The worker plans the floor; the frames go on meanwhile.
    for (int n = 0; n < 400 && (n == 0 || cores.z < 0); ++n) {
        frame ({ 5, 5 }, false);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    ASSERT_GT (cores.z, 0) << "no stairs drawn: the floor was never planned";
    ASSERT_GT (floor.z - floor.x, 600) << "the floor fits the panel's width";
    EXPECT_GT (around.z, floor.z - 1) << "the neighbour drawn beside it as context";
    EXPECT_NEAR ((party.x + party.z) / 2, floor.z, 2) << "the wall against the neighbour, thick";
    EXPECT_GT (party.z - party.x, 4) << "drawn thick";
    const double k = (floor.z - floor.x) / 36.0;
    const auto screen = [&] (fs::Vec p) { return ImVec2 (floor.x + float (p.x * k), floor.w - float (p.y * k)); };
    const auto* planned = planner.Latest (bp::FloorId ("A", 0));
    ASSERT_NE (planned, nullptr);
    ASSERT_FALSE (planned->scheme.cores.empty ());
    EXPECT_NEAR (planned->scheme.gross, 576, 1) << "only A's floor is planned for A";
    ASSERT_FALSE (planned->scheme.party.empty ());

    // A stair dragged 2.1 m along the building: it lands 2.0 m on, in 0.4 m steps.
    const fs::Vec from = planned->scheme.cores[0].centre;
    frame (screen (from), false);
    frame (screen (from), true);
    const auto start = std::chrono::steady_clock::now ();
    for (int n = 1; n <= 20; ++n)
        frame (screen ({ from.x + 2.1 * n / 20, from.y }), true);
    const double ms =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count () / 20;
    RecordProperty ("meanDragFrameMs", ms);
    EXPECT_LT (ms, 100) << "a drag frame waited for the generator";
    frame (screen ({ from.x + 2.1, from.y }), false);
    ASSERT_FALSE (drafts["A"].cores.empty ()) << "the moved stair is the building's own";
    double near = 1e9;
    for (const auto& core : drafts["A"].cores)
        near = (std::min) (near, std::hypot (core.center.x - (from.x + 2.0), core.center.y - from.y));
    EXPECT_LT (near, 1e-6);
    EXPECT_TRUE (bp::Dirty (drafts["A"])) << "Save has something to write";
}

// The flat clicked in the Plan view is drawn white on the overlay, outlines shown or not.
TEST (HudFloorSchemeEdit, TheSelectedFlatIsHighlightedOnTheOverlay)
{
    auto* previous = ImGui::GetCurrentContext ();
    auto* context = ImGui::CreateContext ();
    const auto cleanup = [previous] (ImGuiContext* value) {
        ImGui::DestroyContext (value);
        ImGui::SetCurrentContext (previous);
    };
    std::unique_ptr<ImGuiContext, decltype (cleanup)> held (context, cleanup);
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 1400 };
    io.DeltaTime = 1.0f / 60;
    ASSERT_NE (io.Fonts->AddFontFromFileTTF (EVP_SCENE_TEXT_FONT, 14), nullptr);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);

    auto state = hud::NewState ();
    state->floorPlanSnapshots = { { "A", Building ("A", 0, 36) } };
    ImVec4 floor {}, coloured {}, lightInk {};
    const auto frame = [&] (ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 960, 1380 });
        ImGui::Begin ("Plan", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        hf::PlanView (state->planEditors["A"], state->floorPlanner, state->floorPlanSnapshots, state->buildingPlans,
                      "A", state->massingProgramme, {}, 1);
        floor = Bounds (IM_COL32 (236, 238, 232, 255));
        coloured = Bounds (IM_COL32 (16, 32, 64, 255));
        lightInk = Bounds (IM_COL32 (243, 247, 248, 255));
        ImGui::End ();
        ImGui::Render ();
    };
    const fs::Scheme* scheme = nullptr;
    for (int n = 0; n < 400 && !scheme; ++n) {
        frame ({ 5, 5 }, false);
        if (const auto* planned = state->floorPlanner.Latest (bp::FloorId ("A", 0)))
            scheme = &planned->scheme;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    ASSERT_NE (scheme, nullptr);
    frame ({ 5, 5 }, false);
    ASSERT_FALSE (scheme->flats.empty ());
    const double k = (floor.z - floor.x) / 36.0;
    fs::Vec mid;
    for (const auto& p : scheme->flats.front ().shape)
        mid.x += p.x / scheme->flats.front ().shape.size (), mid.y += p.y / scheme->flats.front ().shape.size ();
    const ImVec2 at (floor.x + float (mid.x * k), floor.w - float (mid.y * k));
    frame (at, false);
    frame (at, true);
    frame (at, false);
    const auto selected = hf::Selected (state->planEditors["A"]);
    ASSERT_TRUE (selected.has_value ());
    EXPECT_EQ (selected->building, "A");
    geomsrv::archviz::overlaylayers::Layer stairs, units;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_TRUE (std::any_of (units.polylines.begin (), units.polylines.end (), [] (const auto& line) {
        return line.rgba == 0xFFFFFFFFu && line.widthPixels >= 5;
    })) << "the selected flat, white, though outlines are off";
    // Colour-only edits refresh the cached canvas fills while keeping the same planned floor.
    const auto revision = state->floorPlanner.Revision ();
    for (size_t i = 0; i < state->massingProgramme.types.size (); ++i)
        ASSERT_TRUE (fp::SetColour (state->massingProgramme, i, 0x102040FFu));
    frame ({ 5, 5 }, false);
    EXPECT_EQ (state->floorPlanner.Revision (), revision);
    EXPECT_FALSE (state->floorPlanner.Busy ());
    EXPECT_GT (coloured.z, 0) << "the new colour is drawn without another worker";
    EXPECT_GT (lightInk.z, 0) << "labels on a dark custom colour remain readable";
}

// In a narrow panel the tools wrap and the canvas takes the width: nothing runs past the edge.
TEST (HudFloorSchemeEdit, ANarrowPanelFitsTheToolsAndTheCanvas)
{
    auto* previous = ImGui::GetCurrentContext ();
    auto* context = ImGui::CreateContext ();
    const auto cleanup = [previous] (ImGuiContext* value) {
        ImGui::DestroyContext (value);
        ImGui::SetCurrentContext (previous);
    };
    std::unique_ptr<ImGuiContext, decltype (cleanup)> held (context, cleanup);
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 2000 };
    io.DeltaTime = 1.0f / 60;
    ASSERT_NE (io.Fonts->AddFontFromFileTTF (EVP_SCENE_TEXT_FONT, 14), nullptr);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    const std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 36) } };
    std::map<std::string, bp::Draft> drafts;
    bp::Planner planner;
    hf::EditorPtr editor;
    for (int n = 0; n < 200; ++n) {
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 240, 1800 });
        ImGui::Begin ("Narrow", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
        hf::PlanView (editor, planner, plans, drafts, "A", fp::Default (), {}, 1);
        EXPECT_LE (ImGui::GetCurrentWindow ()->DC.CursorMaxPos.x, 240 - ImGui::GetStyle ().WindowPadding.x + 1) << n;
        ImGui::End ();
        ImGui::Render ();
        if (n > 2 && !planner.Busy ())
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
}

namespace {
// A headless HUD frame over a 1000 x 1400 view: one full-view window, as the locked view is.
struct OnViewFrame {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    OnViewFrame ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1000, 1400 };
        io.DeltaTime = 1.0f / 60;
        EXPECT_NE (io.Fonts->AddFontFromFileTTF (EVP_SCENE_TEXT_FONT, 14), nullptr);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~OnViewFrame ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    template <typename F> void operator() (ImVec2 mouse, bool left, bool right, F body)
    {
        auto& io = ImGui::GetIO ();
        io.MousePos = mouse;
        io.MouseDown[0] = left;
        io.MouseDown[1] = right;
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 1000, 1400 });
        ImGui::Begin ("##locked", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
        body (ImGui::IsWindowHovered ());
        ImGui::End ();
        ImGui::Render ();
    }
};
// The plan's transform in these tests: 10 pixels a metre, y up, the origin at (100, 600).
hf::ViewOnto Plan ()
{
    hf::ViewOnto onto;
    onto.planar = true;
    const double m[6] = { 10, 0, 100, 0, -10, 600 };
    std::copy (std::begin (m), std::end (m), onto.plan);
    return onto;
}
ImVec2 OnPlan (fs::Vec p)
{
    return { float (100 + 10 * p.x), float (600 - 10 * p.y) };
}
} // namespace

// ⚠️ THE USER (2026-10-10): locked, the floor plan is edited on the view itself -- a flat clicked
// there is selected, a stair dragged moves as on the Plan view's canvas.
TEST (HudFloorSchemeEdit, OnTheLockedPlanAFlatIsSelectedAndAStairDragged)
{
    OnViewFrame frame;
    const std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 36) } };
    std::map<std::string, bp::Draft> drafts;
    std::map<std::string, hf::EditorPtr> editors;
    bp::Planner planner;
    const auto programme = fp::Default ();
    const auto onto = Plan ();
    const auto view = [&] (ImVec2 mouse, bool left) {
        frame (mouse, left, false,
               [&] (bool hovered) { hf::OnView (editors, planner, plans, drafts, programme, {}, onto, hovered, 1); });
    };
    const fs::Scheme* scheme = nullptr;
    for (int n = 0; n < 400 && !scheme; ++n) {
        view (OnPlan ({ 18, 8 }), false);
        if (const auto* planned = planner.Latest (bp::FloorId ("A", 0)))
            scheme = &planned->scheme;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    ASSERT_NE (scheme, nullptr);
    view (OnPlan ({ 18, 8 }), false);
    fs::Vec mid;
    for (const auto& p : scheme->flats.front ().shape)
        mid.x += p.x / scheme->flats.front ().shape.size (), mid.y += p.y / scheme->flats.front ().shape.size ();
    view (OnPlan (mid), false);
    view (OnPlan (mid), true);
    view (OnPlan (mid), false);
    const auto selected = hf::Selected (editors["A"]);
    ASSERT_TRUE (selected.has_value ()) << "the flat clicked on the view is selected";
    // A stair dragged 2.1 m on the view lands 2.0 m on, the building's own.
    const fs::Vec from = scheme->cores.front ().centre;
    view (OnPlan (from), false);
    view (OnPlan (from), true);
    for (int k = 1; k <= 10; ++k)
        view (OnPlan ({ from.x + 2.1 * k / 10, from.y }), true);
    view (OnPlan ({ from.x + 2.1, from.y }), false);
    ASSERT_FALSE (drafts["A"].cores.empty ());
    double near = 1e9;
    for (const auto& core : drafts["A"].cores)
        near = (std::min) (near, std::hypot (core.center.x - (from.x + 2.0), core.center.y - from.y));
    EXPECT_LT (near, 1e-6);
}

// In 3D the view selects: a flat picked through the camera is selected, its building's Plan view
// turned to its floor -- the highest floor under the pointer.
TEST (HudFloorSchemeEdit, OnTheLocked3DViewAFlatPickedTurnsThePlanViewToItsFloor)
{
    OnViewFrame frame;
    auto tower = Building ("A", 0, 36);
    auto upper = tower.floors.front ();
    upper.story = 1, upper.z = 3;
    tower.floors.push_back (upper);
    const std::map<std::string, bp::Plan> plans { { "A", tower } };
    std::map<std::string, bp::Draft> drafts;
    std::map<std::string, hf::EditorPtr> editors;
    bp::Planner planner;
    const auto programme = fp::Default ();
    for (int round = 0; round < 2; ++round)
        for (int n = 0; n < 400; ++n) {
            bp::WantFloors (planner, plans, drafts, "A", programme, 0);
            planner.Poll ();
            if (!planner.Busy () && planner.Latest (bp::FloorId ("A", 1)))
                break;
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    const auto* upperPlanned = planner.Latest (bp::FloorId ("A", 1));
    ASSERT_NE (upperPlanned, nullptr);
    hf::ViewOnto onto; // a camera looking straight down: the plan's transform at every height
    onto.project = [] (double x, double y, double, float& px, float& py) {
        px = float (100 + 10 * x), py = float (600 - 10 * y);
        return true;
    };
    fs::Vec mid;
    const auto& flat = upperPlanned->scheme.flats.front ().shape;
    for (const auto& p : flat)
        mid.x += p.x / flat.size (), mid.y += p.y / flat.size ();
    const auto view = [&] (ImVec2 mouse, bool left) {
        frame (mouse, left, false,
               [&] (bool hovered) { hf::OnView (editors, planner, plans, drafts, programme, {}, onto, hovered, 1); });
    };
    view (OnPlan (mid), false);
    view (OnPlan (mid), true);
    view (OnPlan (mid), false);
    EXPECT_EQ (drafts["A"].story, 1) << "the Plan view turns to the floor picked";
    const auto selected = hf::Selected (editors["A"]);
    ASSERT_TRUE (selected.has_value ());
    EXPECT_EQ (selected->story, 1);
}

TEST (HudFloorSchemeEdit, SelectedDimensionsStayInsideNarrowAndLargeFloorCanvasesAtEveryAngle)
{
    OnViewFrame frame;
    bp::Planner planner;
    const std::map<std::string, bp::Plan> plans;
    std::map<std::string, bp::Draft> drafts;
    const auto programme = fp::Default ();
    const fs::Options options;
    hf::Context c { planner, plans, drafts, programme, options };
    const ImU32 dimensionColour = IM_COL32 (255, 219, 112, 255);
    for (float panelWidth : { 240.0f, 600.0f })
        for (float scale : { 1.0f, 1.5f })
            for (double length : { 20.0, 1000.0 })
                for (double angle : { 0.0, 0.65, 1.57079632679, 2.4 }) {
                    hf::Editor editor;
                    const fs::Vec u { std::cos (angle), std::sin (angle) };
                    const auto world = [&] (fs::Vec p) {
                        return fs::Vec { u.x * p.x - u.y * p.y, u.y * p.x + u.x * p.y };
                    };
                    editor.input.owned = { { { 0, 0 }, { length, 0 }, { length, 16 }, { 0, 16 } } };
                    fs::Flat flat;
                    flat.shape = { { 0, 0 }, { 6, 0 }, { 6, 7 }, { 0, 7 } };
                    flat.axis = u;
                    for (auto& p : editor.input.owned[0])
                        p = world (p);
                    for (auto& p : flat.shape)
                        p = world (p);
                    editor.scheme.outline = editor.input.owned;
                    editor.scheme.flats = { flat };
                    editor.fills = hf::detail::Fills (editor.scheme, programme);
                    const auto canvas = [&] (hf::Target selected, bool expected) {
                        editor.selected = selected;
                        frame ({ 5, 5 }, false, false, [&] (bool) {
                            ImGui::SetWindowSize ({ panelWidth, 1200 });
                            hf::detail::Canvas (editor, c, scale);
                            const auto lo = ImGui::GetItemRectMin (), hi = ImGui::GetItemRectMax ();
                            const auto bounds = Bounds (dimensionColour);
                            if (!expected) {
                                EXPECT_LT (bounds.z, bounds.x);
                                return;
                            }
                            ASSERT_GT (bounds.z, bounds.x)
                                << panelWidth << "/" << scale << "/" << length << "/" << angle;
                            EXPECT_GE (bounds.x, lo.x);
                            EXPECT_GE (bounds.y, lo.y);
                            EXPECT_LE (bounds.z, hi.x);
                            EXPECT_LE (bounds.w, hi.y);
                            int glyphVertices = 0;
                            std::vector<ImVec2> glyphUvs;
                            for (char letter : std::string ("LD67.0m")) {
                                const auto* glyph = ImGui::GetFontBaked ()->FindGlyph (ImWchar (letter));
                                ASSERT_NE (glyph, nullptr);
                                for (float x : { glyph->U0, glyph->U1 })
                                    for (float y : { glyph->V0, glyph->V1 })
                                        glyphUvs.push_back ({ x, y });
                            }
                            for (const auto& vertex : ImGui::GetWindowDrawList ()->VtxBuffer)
                                if (vertex.col == dimensionColour &&
                                    std::any_of (glyphUvs.begin (), glyphUvs.end (), [&] (ImVec2 uv) {
                                        return uv.x == vertex.uv.x && uv.y == vertex.uv.y;
                                    }))
                                    ++glyphVertices;
                            EXPECT_EQ (glyphVertices, 48) << "all glyphs in L 6.00 m and D 7.00 m survive rotation";
                        });
                    };
                    canvas ({}, false);
                    canvas ({ hf::Kind::Flat, 0, {}, {} }, true);
                    canvas ({ hf::Kind::Stair, 0, {}, {} }, false);
                    canvas ({ hf::Kind::Flat, 1, {}, {} }, false);
                }
    EXPECT_EQ (planner.Revision (), 0u) << "selection dimensions do not queue a planning job";
    EXPECT_TRUE (drafts.empty ()) << "selection dimensions do not dirty the design";
}

// User, 2026-10-14: no silent failure. A flat dragged off the floor that cannot go (it ends the
// corridor) stays, and the Plan view says why under its tools.
TEST (HudFloorSchemeEdit, AnEditThatCannotBeMadeIsSaidUnderTheTools)
{
    auto* previous = ImGui::GetCurrentContext ();
    auto* context = ImGui::CreateContext ();
    const auto cleanup = [previous] (ImGuiContext* value) {
        ImGui::DestroyContext (value);
        ImGui::SetCurrentContext (previous);
    };
    std::unique_ptr<ImGuiContext, decltype (cleanup)> held (context, cleanup);
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 1400 };
    io.DeltaTime = 1.0f / 60;
    ASSERT_NE (io.Fonts->AddFontFromFileTTF (EVP_SCENE_TEXT_FONT, 14), nullptr);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    const std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 36) } };
    std::map<std::string, bp::Draft> drafts;
    bp::Planner planner;
    hf::EditorPtr editor;
    ImVec4 floor {}, notice {};
    const ImU32 orange = ImGui::ColorConvertFloat4ToU32 (ImVec4 (0.86f, 0.52f, 0.13f, 1));
    const auto frame = [&] (ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 960, 1380 });
        ImGui::Begin ("Plan", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        hf::PlanView (editor, planner, plans, drafts, "A", fp::Default (), {}, 1);
        floor = Bounds (IM_COL32 (236, 238, 232, 255));
        notice = Bounds (orange);
        ImGui::End ();
        ImGui::Render ();
    };
    const fs::Scheme* scheme = nullptr;
    for (int n = 0; n < 400 && !scheme; ++n) {
        frame ({ 5, 5 }, false);
        if (const auto* planned = planner.Latest (bp::FloorId ("A", 0)))
            scheme = &planned->scheme;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    ASSERT_NE (scheme, nullptr);
    frame ({ 5, 5 }, false);
    int cap = -1;
    for (size_t i = 0; i < scheme->flats.size (); ++i)
        if (scheme->flats[i].band < 0)
            cap = int (i);
    ASSERT_GE (cap, 0) << "the bar has a flat at a corridor's end";
    const size_t flats = scheme->flats.size ();
    const double k = (floor.z - floor.x) / 36.0;
    const auto screen = [&] (fs::Vec p) { return ImVec2 (floor.x + float (p.x * k), floor.w - float (p.y * k)); };
    fs::Vec mid;
    for (const auto& p : scheme->flats[cap].shape)
        mid.x += p.x / scheme->flats[cap].shape.size (), mid.y += p.y / scheme->flats[cap].shape.size ();
    EXPECT_GT (notice.x, notice.z) << "nothing to say yet";
    frame (screen (mid), false);
    frame (screen (mid), true);
    for (int n = 1; n <= 8; ++n)
        frame (screen ({ mid.x, mid.y - 4.0 * n }), true); // off the floor, below it
    frame (screen ({ mid.x, mid.y - 32 }), false);
    ASSERT_NE (editor, nullptr);
    EXPECT_FALSE (editor->notice.empty ()) << "the refusal is said";
    EXPECT_FALSE (bp::Dirty (drafts["A"])) << "nothing changed";
    frame ({ 5, 5 }, false);
    EXPECT_LT (notice.x, notice.z) << "the reason is drawn";
    EXPECT_LT (notice.w, floor.y) << "under the tools, above the plan";
    EXPECT_EQ (planner.Latest (bp::FloorId ("A", 0))->scheme.flats.size (), flats);
}

// User, 2026-10-14: "Wall selection hitbox needs to larger." A wall between two flats is picked
// within 12 pixels of it; further in, the flat.
TEST (HudFloorSchemeEdit, AWallIsPickedWithinTwelvePixels)
{
    fs::Scheme s;
    for (double x0 : { 0.0, 6.0 }) {
        fs::Flat flat;
        flat.shape = { { x0, 0 }, { x0 + 6, 0 }, { x0 + 6, 7 }, { x0, 7 } };
        flat.band = 0;
        flat.axis = { 1, 0 };
        s.flats.push_back (flat);
    }
    const double px = 0.1; // ten pixels a metre
    EXPECT_EQ (hf::detail::HitAt (s, { 6 + 1.0, 3.5 }, px).kind, hf::Kind::Wall) << "10 px off the wall";
    EXPECT_EQ (hf::detail::HitAt (s, { 6 - 1.1, 3.5 }, px).kind, hf::Kind::Wall) << "11 px, the other side";
    EXPECT_EQ (hf::detail::HitAt (s, { 6 + 1.5, 3.5 }, px).kind, hf::Kind::Flat) << "15 px: the flat";
}

// User, 2026-10-14: "When dragging unit or stair the selected previous position should dashed and
// ghostly." While a flat is held its old place shows faint and dashed; let go, the ghost goes.
TEST (HudFloorSchemeEdit, ADraggedFlatLeavesADashedGhostWhereItWas)
{
    auto* previous = ImGui::GetCurrentContext ();
    auto* context = ImGui::CreateContext ();
    const auto cleanup = [previous] (ImGuiContext* value) {
        ImGui::DestroyContext (value);
        ImGui::SetCurrentContext (previous);
    };
    std::unique_ptr<ImGuiContext, decltype (cleanup)> held (context, cleanup);
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 1400 };
    io.DeltaTime = 1.0f / 60;
    ASSERT_NE (io.Fonts->AddFontFromFileTTF (EVP_SCENE_TEXT_FONT, 14), nullptr);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    const std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 36) } };
    std::map<std::string, bp::Draft> drafts;
    bp::Planner planner;
    hf::EditorPtr editor;
    ImVec4 floor {}, ghost {};
    const auto frame = [&] (ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 960, 1380 });
        ImGui::Begin ("Plan", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        hf::PlanView (editor, planner, plans, drafts, "A", fp::Default (), {}, 1);
        floor = Bounds (IM_COL32 (236, 238, 232, 255));
        ghost = Bounds (hf::detail::kGhostLine);
        ImGui::End ();
        ImGui::Render ();
    };
    const fs::Scheme* scheme = nullptr;
    for (int n = 0; n < 400 && !scheme; ++n) {
        frame ({ 5, 5 }, false);
        if (const auto* planned = planner.Latest (bp::FloorId ("A", 0)))
            scheme = &planned->scheme;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    ASSERT_NE (scheme, nullptr);
    frame ({ 5, 5 }, false);
    const double k = (floor.z - floor.x) / 36.0;
    const auto screen = [&] (fs::Vec p) { return ImVec2 (floor.x + float (p.x * k), floor.w - float (p.y * k)); };
    const auto& shape = scheme->flats.front ().shape;
    fs::Vec mid, lo { 1e9, 1e9 }, hi { -1e9, -1e9 };
    for (const auto& p : shape) {
        mid.x += p.x / shape.size (), mid.y += p.y / shape.size ();
        lo = { (std::min) (lo.x, p.x), (std::min) (lo.y, p.y) },
        hi = { (std::max) (hi.x, p.x), (std::max) (hi.y, p.y) };
    }
    frame (screen (mid), false);
    frame (screen (mid), true);
    EXPECT_GT (ghost.x, ghost.z) << "no ghost before it moves";
    for (int n = 1; n <= 6; ++n)
        frame (screen ({ mid.x + 0.5 * n, mid.y }), true);
    ASSERT_LT (ghost.x, ghost.z) << "a ghost while it is held";
    EXPECT_NEAR (ghost.x, screen (lo).x, 3);
    EXPECT_NEAR (ghost.z, screen (hi).x, 3);
    EXPECT_NEAR (ghost.y, screen (hi).y, 3) << "where the flat was, not where the pointer is";
    frame (screen (mid), false);
    frame ({ 5, 5 }, false);
    EXPECT_GT (ghost.x, ghost.z) << "let go, the ghost goes";
}

// User, 2026-10-14: the selected contour lagged behind navigation, and a flat moved on the locked
// view drew a second outline over the panel's canvas. On the view the selected flat is left to the
// overlay's own outline (drawn with the transform read at the plan's Present); a gesture begun on
// the view is carried on, and drawn, there only.
TEST (HudFloorSchemeEdit, TheLockedViewLeavesTheSelectionToTheOverlayAndKeepsItsGesture)
{
    OnViewFrame frame;
    const std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 36) } };
    std::map<std::string, bp::Draft> drafts;
    std::map<std::string, hf::EditorPtr> editors;
    bp::Planner planner;
    const auto programme = fp::Default ();
    const auto onto = Plan ();
    ImVec4 solid {};
    const auto view = [&] (ImVec2 mouse, bool left) {
        frame (mouse, left, false, [&] (bool hovered) {
            hf::OnView (editors, planner, plans, drafts, programme, {}, onto, hovered, 1);
            solid = Bounds (IM_COL32 (255, 255, 255, 255));
        });
    };
    const fs::Scheme* scheme = nullptr;
    for (int n = 0; n < 400 && !scheme; ++n) {
        view (OnPlan ({ 18, 8 }), false);
        if (const auto* planned = planner.Latest (bp::FloorId ("A", 0)))
            scheme = &planned->scheme;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    ASSERT_NE (scheme, nullptr);
    view (OnPlan ({ 18, 8 }), false);
    fs::Vec mid;
    for (const auto& p : scheme->flats.front ().shape)
        mid.x += p.x / scheme->flats.front ().shape.size (), mid.y += p.y / scheme->flats.front ().shape.size ();
    view (OnPlan (mid), false);
    view (OnPlan (mid), true);
    view (OnPlan (mid), false);
    ASSERT_TRUE (hf::Selected (editors["A"]).has_value ());
    view (OnPlan ({ -20, -20 }), false);
    EXPECT_GT (solid.x, solid.z) << "no ImGui copy of the selected flat on the view";
    // A drag begun on the view; the canvas surface, with its own transform, leaves it alone.
    view (OnPlan (mid), false);
    view (OnPlan (mid), true);
    view (OnPlan ({ mid.x + 2, mid.y }), true);
    auto& e = *editors["A"];
    ASSERT_NE (e.drag, hf::Drag::None);
    const fs::Vec at = e.at;
    fs::Options options;
    hf::Context c { planner, plans, drafts, programme, options };
    hf::View canvas; // a fit of its own: the same pointer is somewhere else on it
    canvas.m[0] = 3, canvas.m[2] = 40, canvas.m[4] = -3, canvas.m[5] = 300;
    frame (OnPlan ({ mid.x + 5, mid.y }), true, false,
           [&] (bool) { hf::detail::Interact (e, c, canvas, true, hf::Surface::Canvas); });
    EXPECT_EQ (e.at, at) << "the canvas did not take the view's gesture";
    EXPECT_NE (e.drag, hf::Drag::None);
    view (OnPlan ({ mid.x + 2, mid.y }), false);
    EXPECT_EQ (e.drag, hf::Drag::None) << "let go on the view, where it began";
}
