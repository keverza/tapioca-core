// The Plan view headless: one building's floor on a canvas the panel's width, its neighbour as
// grey context with a thick blind wall between them, a stair dragged in 0.4 m steps without the
// frame waiting for the generator, and the moved stair the building's own.
#include "ArchViz/FloorSchemeEdit.hpp"
#include "ArchViz/HudFloorSchemeEdit.hpp"
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
    ImVec4 floor {};
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
