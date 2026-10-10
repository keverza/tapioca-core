#include "ArchViz/FloorSchemeEdit.hpp"
#include "ArchViz/HudFloorSchemeEdit.hpp"
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
    floor.outlineKnown = true;
    floor.outline = { Chain (x0, 0, x1, 16) };
    floor.contours = { floor.outline };
    plan.floors = { floor };
    return plan;
}
// Screen bounds of what was drawn in `colour` this frame.
ImVec4 Bounds (ImU32 colour)
{
    ImVec4 b { 1e6f, 1e6f, -1e6f, -1e6f };
    for (const auto& v : ImGui::GetWindowDrawList ()->VtxBuffer)
        if (v.col == colour)
            b = { (std::min) (b.x, v.pos.x), (std::min) (b.y, v.pos.y), (std::max) (b.z, v.pos.x),
                  (std::max) (b.w, v.pos.y) };
    return b;
}
} // namespace

TEST (HudFloorSchemeEdit, TwoBuildingsEditAsOneFloorAndAStairDragsWithoutStallingTheFrame)
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

    // Two buildings side by side at one elevation: one massing floor, 60 x 16.
    std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 36) }, { "B", Building ("B", 36, 60) } };
    const auto programme = fp::Default ();
    hf::EditorPtr editor;
    ImVec4 outline {}, cores {};
    const auto frame = [&] (ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 960, 1380 });
        ImGui::Begin ("Plan", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        hf::Section (editor, plans, plans["A"].floors[0], "A", programme, 1);
        outline = Bounds (IM_COL32 (47, 93, 138, 255));
        cores = Bounds (IM_COL32 (109, 114, 118, 255));
        ImGui::End ();
        ImGui::Render ();
    };
    const ImVec2 button { ImGui::GetStyle ().WindowPadding.x + 20, ImGui::GetStyle ().WindowPadding.y + 8 };
    frame (button, false);
    frame (button, false);
    frame (button, true);
    frame (button, false);
    ASSERT_TRUE (editor) << "Edit massing floor was not pressed";
    // The worker plans the floor; the frames go on meanwhile.
    for (int n = 0; n < 400 && cores.z < 0; ++n) {
        frame ({ 5, 5 }, false);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    ASSERT_GT (cores.z, 0) << "no stairs drawn: the floor was never planned";
    ASSERT_GT (outline.z - outline.x, 100);
    // The same plan offline, for where its stairs are.
    std::vector<fs::Ring> floor { { { 0, 0 }, { 36, 0 }, { 36, 16 }, { 0, 16 } },
                                  { { 36, 0 }, { 60, 0 }, { 60, 16 }, { 36, 16 } } };
    const auto scheme = fe::Run (floor, programme, {});
    ASSERT_EQ (scheme.cores.size (), 2u);
    EXPECT_EQ (scheme.flats.size (), 12u) << "the two buildings are one 60 x 16 floor";
    const double k = (outline.z - outline.x) / 60.0;
    const auto screen = [&] (fs::Vec p) { return ImVec2 (outline.x + float (p.x * k), outline.w - float (p.y * k)); };
    const fs::Vec from = scheme.cores[0].centre, to { from.x + 2, from.y };
    frame (screen (from), false);
    frame (screen (from), true);
    const auto start = std::chrono::steady_clock::now ();
    for (int n = 1; n <= 20; ++n)
        frame (screen ({ from.x + 2.0 * n / 20, from.y }), true);
    const double ms =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count () / 20;
    RecordProperty ("meanDragFrameMs", ms);
    EXPECT_LT (ms, 100) << "a drag frame waited for the generator";
    frame (screen (to), false);
    // Released: the moved stair is drawn where it was dropped once the worker is done.
    bool moved = false;
    for (int n = 0; n < 400 && !moved; ++n) {
        frame ({ 5, 5 }, false);
        const double x0 = (cores.x - outline.x) / k; // the lowest stair's left side, in metres
        moved = std::abs (x0 - (to.x - scheme.cores[0].width / 2)) < 0.7;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    EXPECT_TRUE (moved);
}
