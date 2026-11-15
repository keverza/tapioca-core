// The skeleton on the Plan view's canvas, headless: the wings the floor is planned on drawn over
// the flats in the one view, a wing's side dragged in 0.4 m steps and the floor planned again on
// the user's wing, a drag the floor cannot take said and not made, a corridor's leg dragged across
// its wing, a stair's side dragged -- and the flats still held by the canvas under them.
#include "ArchViz/FloorSchemeEdit.hpp"
#include "ArchViz/HudFloorSchemeEdit.hpp"
#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <thread>
#include "imgui.h"
#include "imgui_internal.h"

namespace bp = geomsrv::archviz::buildingplan;
namespace fs = geomsrv::archviz::floorscheme;
namespace fe = geomsrv::archviz::floorscheme::edit;
namespace fp = geomsrv::archviz::floorprogramme;
namespace hf = geomsrv::archviz::hudfloorscheme;

namespace {
bp::Plan Bar (const char* key, double length)
{
    geomsrv::archviz::SliceChain chain;
    chain.closed = true;
    chain.xy = { 0, 0, length, 0, length, 16, 0, 16 };
    bp::Plan plan;
    plan.key = key;
    bp::Floor floor;
    floor.z = 0;
    floor.height = 3;
    floor.areaM2 = length * 16;
    floor.outlineKnown = true;
    floor.outline = { chain };
    floor.contours = { floor.outline };
    floor.outlineKey = key;
    plan.floors = { floor };
    return plan;
}
ImVec4 Bounds (ImU32 colour)
{
    ImVec4 b { 1e6f, 1e6f, -1e6f, -1e6f };
    for (const auto& v : ImGui::GetWindowDrawList ()->VtxBuffer)
        if (v.col == colour)
            b = { (std::min) (b.x, v.pos.x), (std::min) (b.y, v.pos.y), (std::max) (b.z, v.pos.x),
                  (std::max) (b.w, v.pos.y) };
    return b;
}
// One 60 x 16 m bar in a Plan view, driven frame by frame.
struct Panel {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    const std::map<std::string, bp::Plan> plans { { "A", Bar ("A", 60) } };
    std::map<std::string, bp::Draft> drafts;
    bp::Planner planner;
    hf::EditorPtr editor;
    ImVec4 floor {}, found {}, mine {};
    Panel ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1000, 1400 };
        io.DeltaTime = 1.0f / 60;
        io.Fonts->AddFontFromFileTTF (EVP_SCENE_TEXT_FONT, 14);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~Panel ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    void Frame (ImVec2 mouse, bool down)
    {
        auto& io = ImGui::GetIO ();
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 960, 1380 });
        ImGui::Begin ("Plan", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        hf::PlanView (editor, planner, plans, drafts, "A", fp::Default (), {}, 1);
        floor = Bounds (IM_COL32 (236, 238, 232, 255));
        found = Bounds (IM_COL32 (47, 93, 138, 255));
        mine = Bounds (IM_COL32 (255, 186, 0, 255));
        ImGui::End ();
        ImGui::Render ();
    }
    // Frames until the planner has nothing left to plan and the editor shows its newest scheme.
    void Settle ()
    {
        for (int n = 0; n < 600; ++n) {
            Frame ({ 5, 5 }, false);
            if (editor && editor->have && !planner.Busy () && n > 3)
                return;
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    }
    ImVec2 Screen (fs::Vec p) const
    {
        const double k = (floor.z - floor.x) / 60.0;
        return { floor.x + float (p.x * k), floor.w - float (p.y * k) };
    }
    void Click (fs::Vec at)
    {
        Frame (Screen (at), false);
        Frame (Screen (at), true);
        Frame (Screen (at), false);
    }
    void Drag (fs::Vec from, fs::Vec to)
    {
        Frame (Screen (from), false);
        Frame (Screen (from), true);
        for (int n = 1; n <= 10; ++n)
            Frame (Screen ({ from.x + (to.x - from.x) * n / 10, from.y + (to.y - from.y) * n / 10 }), true);
        Frame (Screen (to), false);
    }
    const fe::Design& Design ()
    {
        return drafts["A"].designs.floors[0];
    }
};
} // namespace

TEST (HudFloorSchemeSkeleton, TheWingsAreDrawnOverTheFlatsInTheOneView)
{
    Panel p;
    p.Settle ();
    ASSERT_NE (p.editor, nullptr);
    ASSERT_TRUE (p.editor->have);
    p.Frame ({ 5, 5 }, false);
    ASSERT_GT (p.floor.z, p.floor.x) << "the plan is drawn";
    // The generator's one wing, in blue, round the whole floor.
    EXPECT_NEAR (p.found.x, p.floor.x, 3);
    EXPECT_NEAR (p.found.z, p.floor.z, 3);
    EXPECT_EQ (p.editor->sketch.items.size (),
               p.editor->scheme.wings.size () + p.editor->scheme.corridors.size () + p.editor->scheme.cores.size ());
    EXPECT_FALSE (p.editor->graph.edges.empty ()) << "how the flats are entered is drawn";
    // Inside a flat the pointer is the canvas's: the flat is selected, nothing of the skeleton held.
    p.Click ({ 30, 4 });
    EXPECT_EQ (p.editor->selected.kind, hf::Kind::Flat);
    EXPECT_FALSE (p.editor->hold.Held ());
    EXPECT_FALSE (bp::Dirty (p.drafts["A"])) << "looking changes nothing";
}

TEST (HudFloorSchemeSkeleton, AWingsSideIsDraggedInStepsAndTheFloorPlannedOnIt)
{
    Panel p;
    p.Settle ();
    ASSERT_NE (p.editor, nullptr);
    // The end at x = 60 pulled in 4.1 m: it lands 4.0 m in.
    p.Drag ({ 59.9, 4 }, { 55.8, 4 });
    ASSERT_EQ (p.Design ().pins.wings.size (), 1u) << p.editor->notice;
    const auto& wing = p.Design ().pins.wings[0];
    EXPECT_NEAR ((std::max) (wing.a.x, wing.b.x), 56, 1e-6);
    EXPECT_NEAR ((std::min) (wing.a.x, wing.b.x), 0, 1e-6);
    EXPECT_TRUE (bp::Dirty (p.drafts["A"]));
    EXPECT_EQ (p.editor->undo.size (), 1u) << "one gesture, one undo step";
    p.Settle ();
    const auto* planned = p.planner.Latest (bp::FloorId ("A", 0));
    ASSERT_NE (planned, nullptr);
    EXPECT_TRUE (planned->scheme.authored);
    ASSERT_EQ (planned->scheme.wings.size (), 1u);
    EXPECT_NEAR (planned->scheme.wings[0].length, 56, 1e-6);
    p.Frame ({ 5, 5 }, false);
    EXPECT_LT (p.mine.x, p.mine.z) << "the wing is the user's: drawn in the accent";
    EXPECT_LT (p.mine.z, p.floor.z - 20) << "and stops short of the floor's end";
}

TEST (HudFloorSchemeSkeleton, ADragTheFloorCannotTakeIsSaidUnderThePlanAndNotMade)
{
    Panel p;
    p.Settle ();
    ASSERT_NE (p.editor, nullptr);
    const ImVec4 before = p.floor;
    // The wing's end pushed 2 m out of the floor.
    p.Drag ({ 59.9, 4 }, { 62.0, 4 });
    EXPECT_NE (p.editor->notice.find ("outside the floor"), std::string::npos) << p.editor->notice;
    EXPECT_FALSE (bp::Dirty (p.drafts["A"]));
    EXPECT_TRUE (p.editor->undo.empty ());
    EXPECT_EQ (p.editor->drag, hf::Drag::None);
    // Said under the plan: the plan has not moved.
    p.Frame ({ 5, 5 }, false);
    EXPECT_NEAR (p.floor.y, before.y, 0.5);
    EXPECT_NEAR (p.floor.w, before.w, 0.5);
}

TEST (HudFloorSchemeSkeleton, ACorridorsLegIsDraggedAcrossItsWing)
{
    Panel p;
    p.Settle ();
    ASSERT_NE (p.editor, nullptr);
    const auto lane = fe::CorridorOf (p.editor->scheme, 0);
    ASSERT_TRUE (lane.has_value ()) << "the bar has a centre corridor";
    EXPECT_NEAR (lane->y, 8, 1e-6);
    // Held by its middle line and moved 1.3 m across the wing: 1.2 m, in steps.
    p.Drag ({ lane->x, 8 }, { lane->x, 9.3 });
    ASSERT_EQ (p.Design ().pins.corridors.size (), 1u) << p.editor->notice;
    EXPECT_NEAR (p.Design ().pins.corridors[0].at.y, 9.2, 1e-6);
    EXPECT_TRUE (p.Design ().pins.wings.empty ()) << "a corridor moved does not make the wings the user's";
    p.Settle ();
    const auto moved = fe::CorridorOf (p.planner.Latest (bp::FloorId ("A", 0))->scheme, 0);
    ASSERT_TRUE (moved.has_value ());
    EXPECT_NEAR (moved->y, 9.2, 1e-6);
    // Too far: a row of flats would be under 5.5 m deep. Said, and the corridor stays.
    p.Drag ({ lane->x, 9.2 }, { lane->x, 12.5 });
    EXPECT_NE (p.editor->notice.find ("at least 5.5 m"), std::string::npos) << p.editor->notice;
    EXPECT_NEAR (p.Design ().pins.corridors[0].at.y, 9.2, 1e-6);
}

TEST (HudFloorSchemeSkeleton, AStairsSideIsDraggedAndItsBodyIsStillTheCanvass)
{
    Panel p;
    p.Settle ();
    ASSERT_NE (p.editor, nullptr);
    ASSERT_FALSE (p.editor->scheme.cores.empty ());
    const auto core = p.editor->scheme.cores[0];
    // Inside the stair the canvas holds it, as before.
    p.Click (core.centre);
    EXPECT_EQ (p.editor->selected.kind, hf::Kind::Stair);
    // Its side facing +x, pulled 0.8 m out: the stair is 0.8 m wider.
    p.Drag ({ core.centre.x + core.width / 2, core.centre.y }, { core.centre.x + core.width / 2 + 0.8, core.centre.y });
    EXPECT_EQ (p.editor->undo.size (), 1u) << p.editor->notice;
    p.Settle ();
    const auto& cores = p.planner.Latest (bp::FloorId ("A", 0))->scheme.cores;
    EXPECT_TRUE (std::any_of (cores.begin (), cores.end (), [&] (const fs::Core& c) {
        return std::abs (c.width - (core.width + 0.8)) < 0.45;
    })) << "the stair is wider";
}
