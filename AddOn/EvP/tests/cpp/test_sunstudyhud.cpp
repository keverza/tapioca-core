// The sun study section as the viewer draws it (ArchViz/DiligentHudSunStudy.cpp
// and the run form, DiligentHudSunRun.cpp): its widget IDs and the shadow
// views' hover text (#116).
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <string>
#include <vector>

// The run service is the add-on's; the section only asks it for a status.
namespace geomsrv::sunstudyhud {
bool Submit (const Request&, std::string&)
{
    return true;
}
void Cancel ()
{
}
Status Current ()
{
    return {};
}
void Shutdown ()
{
}
} // namespace geomsrv::sunstudyhud

using namespace geomsrv::archviz;

namespace {
struct SectionGui {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    HudState state;
    DiligentSceneStats scene;
    SectionGui ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 500, 1400 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
        SunStudyOverlayStatus& study = scene.sunStudy;
        study.studyId = "sun-1";
        study.drawing = true;
        study.stepCount = 4;
        study.noonStep = 2;
        study.stepMinutes = { 480, 540, 600, 660 };
        study.quantumHours = 1.0f;
    }
    ~SectionGui ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    void Frame (ImVec2 pointer = { -1, -1 })
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, false);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 400, 1350 });
        ImGui::Begin ("sun study", nullptr, ImGuiWindowFlags_NoSavedSettings);
        // The run form open beside a study on screen: its Roles node and the
        // view selector's Roles button in one window, as the live HUD has them.
        ImGui::GetStateStorage ()->SetInt (ImGui::GetID ("Run sun study"), 1);
        DrawSunStudyHudSection (state, scene);
        ImGui::End ();
        ImGui::Render ();
    }
};
} // namespace

TEST (SunStudyHud, ShadowAndRoleViewsSubmitNoWidgetIdTwice)
{
    // sunView / sunFanInterval for the roles, single, morning / evening and fan views.
    const struct {
        int view, fan, mode;
    } views[] = { { 4, 5, 4 }, { 2, 5, 5 }, { 3, 5, 6 }, { 3, 2, 7 } };
    for (const auto& chosen : views) {
        SectionGui gui;
        gui.state.sunSeenCommandedMode = 0; // the command already seen: the choice stands
        gui.state.sunView = chosen.view;
        gui.state.sunFanInterval = chosen.fan;
        gui.Frame ();
        gui.Frame ();
        size_t hovered = 0;
        for (float y = 8; y < 1300; y += 6)
            for (float x : { 20.0f, 60.0f, 140.0f, 260.0f, 360.0f }) {
                gui.Frame ({ x, y });
                if (gui.context->HoveredId == 0)
                    continue;
                ++hovered;
                gui.Frame ({ x, y });
                EXPECT_LE (gui.context->HoveredIdPreviousFrameItemCount, 1)
                    << "mode " << chosen.mode << " at " << x << ", " << y;
            }
        EXPECT_GT (hovered, 0u) << "mode " << chosen.mode;
    }
}

TEST (SunStudyHud, ShadowRunsMergeConsecutiveStepsAndEndAQuantumLater)
{
    const std::vector<uint16_t> minutes = { 480, 495, 510, 525, 540, 555 };
    const std::vector<uint8_t> lit = { 0, 0, 1, 0, 1, 0 };
    const auto runs = SunStudyShadowRuns (lit, minutes, 0, 6, 15);
    ASSERT_EQ (runs.size (), 3u);
    EXPECT_EQ (runs[0].from, 480u);
    EXPECT_EQ (runs[0].to, 510u);
    EXPECT_EQ (runs[2].from, 555u);
    EXPECT_EQ (runs[2].to, 570u);
    EXPECT_EQ (SunStudyShadowTimes (runs, 480, 570), "Shadow 08:00-08:30, 08:45-09:00, 09:15-09:30");
    EXPECT_EQ (SunStudyShadowTimes (runs, 480, 570, 2), "Shadow 08:00-08:30, 08:45-09:00, +1 more");
    // The views' Hours range: only steps in [first, end) count.
    EXPECT_EQ (SunStudyShadowTimes (SunStudyShadowRuns (lit, minutes, 2, 3, 15), 510, 525), "No shadow");
    EXPECT_EQ (SunStudyShadowTimes (SunStudyShadowRuns (lit, minutes, 0, 2, 15), 480, 510),
               "Shadow all of 08:00-08:30");
    EXPECT_EQ (SunStudyShadowedSteps (lit, 0, 3), 2u);
    EXPECT_EQ (SunStudyShadowedSteps (lit, 3, 6), 2u);
    // Fewer flags than steps (a study still converging) never reads past them.
    EXPECT_EQ (SunStudyShadowRuns ({ 0 }, minutes, 0, 6, 15).size (), 1u);
}

TEST (SunStudyHud, TheOverlapSwatchIsMorningAndEveningInEqualShares)
{
    // Rounded halves of 0x796AB1 and 0xDF9792: the legend's old 0xAC80A2 to a step.
    EXPECT_EQ (SunStudyAmPmOverlapRgba (kAmPmMorningRgba, kAmPmEveningRgba), 0xAC81A2FFu);
    EXPECT_EQ (SunStudyAmPmOverlapRgba (0xFF000080u, 0x00FF0010u) & 0xffu, 0xffu) << "opaque";
}
