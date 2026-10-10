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
