// The sun study section as the viewer draws it (ArchViz/DiligentHudSunStudy.cpp
// and the run form, DiligentHudSunRun.cpp): its widget IDs and the shadow
// views' hover text (#116).
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "NativeCommands/SunStudyFollowerDriver.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <string>
#include <vector>

// The run service is the add-on's; the section only asks it for a status.
namespace geomsrv::sunstudyhud {
static Status s_status;
static int s_cancels = 0;
static int s_submissions = 0;
bool Submit (const Request&, std::string&)
{
    ++s_submissions;
    return true;
}
void Cancel ()
{
}
Status Current ()
{
    return s_status;
}
Status CurrentJob ()
{
    return s_status;
}
void CancelActive ()
{
    ++s_cancels;
}
bool ResumeFollowing (std::string&)
{
    return true;
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
    bool collapsed = false;
    bool runOpen = false;
    SectionGui ()
    {
        geomsrv::sunstudyhud::s_status = {};
        geomsrv::sunstudyhud::s_cancels = geomsrv::sunstudyhud::s_submissions = 0;
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
    void Frame (ImVec2 pointer = { -1, -1 }, bool down = false)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 400, 1350 });
        ImGui::Begin ("sun study", nullptr, ImGuiWindowFlags_NoSavedSettings);
        // The run form open beside a study on screen: its Roles node and the
        // view selector's Roles button in one window, as the live HUD has them.
        ImGui::GetStateStorage ()->SetInt (ImGui::GetID ("Run sun study"), collapsed ? 0 : 1);
        DrawSunStudyHudSection (state, scene);
        runOpen = ImGui::GetStateStorage ()->GetInt (ImGui::GetID ("Run sun study")) != 0;
        ImGui::End ();
        ImGui::Render ();
    }
};
} // namespace

TEST (SunStudyHud, CollapsedSetupStillOffersCancelForManualAndFollowerStages)
{
    for (bool following : { false, true })
        for (const char* stage :
             { "queued", "waiting", "snapshot", "starting", "preparing", "calculating", "displaying", "showing" }) {
            SectionGui gui;
            gui.collapsed = true;
            auto& status = geomsrv::sunstudyhud::s_status;
            status.running = true;
            status.following = following;
            status.stage = stage;
            status.totalSteps = 50;
            status.resolvedSteps = 10;
            gui.Frame ();
            gui.Frame ({ 200, 40 });
            gui.Frame ({ 200, 40 }, true);
            gui.Frame ({ 200, 40 }, false);
            EXPECT_EQ (geomsrv::sunstudyhud::s_cancels, 1) << stage;
            EXPECT_EQ (geomsrv::sunstudyhud::s_submissions, 0);
            EXPECT_TRUE (gui.state.sunRunBusy);
            EXPECT_FALSE (gui.runOpen);
        }
}

TEST (SunStudyHud, CancellingDisablesThePrimaryActionAndAcknowledgementReopensSetup)
{
    SectionGui gui;
    gui.collapsed = true;
    auto& status = geomsrv::sunstudyhud::s_status;
    status.running = status.cancelling = true;
    status.stage = "cancelling";
    gui.Frame ();
    gui.Frame ({ 200, 40 }, true);
    gui.Frame ({ 200, 40 });
    EXPECT_EQ (geomsrv::sunstudyhud::s_cancels, 0);
    EXPECT_EQ (geomsrv::sunstudyhud::s_submissions, 0);
    status.running = status.cancelling = false;
    status.stage = "cancelled";
    gui.Frame ();
    EXPECT_FALSE (gui.state.sunRunBusy);
    EXPECT_TRUE (gui.runOpen);
}

TEST (SunStudyHud, ManualOwnershipAlwaysWinsAndCancelledFollowerProgressIsCleared)
{
    using namespace geomsrv;
    sunstudyhud::Status manual;
    manual.running = true;
    manual.stage = "calculating";
    manual.resolvedSteps = 3;
    manual.totalSteps = 50;
    sunfollow::FollowerStats follower;
    follower.autoFollow = follower.workerBusy = true;
    follower.stage = "preparing";
    follower.totalSteps = 99;
    auto status = sunstudyhud::JobStatusOf (manual, follower);
    EXPECT_FALSE (status.following);
    EXPECT_EQ (status.stage, "calculating");
    EXPECT_EQ (status.totalSteps, 50u);
    manual.running = false;
    status = sunstudyhud::JobStatusOf (manual, follower);
    EXPECT_TRUE (status.following);
    EXPECT_EQ (status.stage, "preparing");
    follower.autoFollow = follower.workerBusy = false;
    follower.paused = true;
    follower.stage = "cancelled";
    status = sunstudyhud::JobStatusOf (manual, follower);
    EXPECT_FALSE (status.running);
    EXPECT_TRUE (status.paused);
    EXPECT_EQ (status.totalSteps, 0u);
}

TEST (SunStudyHud, FailedFollowerOffersRetryRatherThanAnUnendingCancel)
{
    geomsrv::sunfollow::FollowerStats follower;
    follower.autoFollow = follower.dirty = true;
    follower.stage = "failed";
    follower.state = evp::sunstudy::SunStudyFollowState::Failed;
    const auto status = geomsrv::sunstudyhud::JobStatusOf ({}, follower);
    EXPECT_FALSE (status.running);
    EXPECT_EQ (status.stage, "failed");
}

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
