#include "ArchViz/TapiocaUI/Foundation/AnalysisRunControl.hpp"

#include <gtest/gtest.h>
#include <imgui_internal.h>

using namespace tapioca::ui;

namespace {
struct RunGui {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    ImVec2 size, position;
    AnalysisRunView view;
    RunGui ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 500, 500 };
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~RunGui ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    AnalysisRunAction Frame (ImVec2 pointer = { -1, -1 }, bool down = false, bool focus = false)
    {
        auto& io = ImGui::GetIO ();
        io.DeltaTime = 1.0f / 60;
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 400, 400 });
        ImGui::Begin ("run control", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (focus)
            ImGui::SetKeyboardFocusHere ();
        const auto result = AnalysisRunControl ("run", view);
        size = ImGui::GetItemRectSize ();
        position = ImGui::GetItemRectMin ();
        ImGui::End ();
        ImGui::Render ();
        return result;
    }
};
} // namespace

TEST (AnalysisRunControl, AllStagesOccupyExactlyTheSameRectangle)
{
    RunGui gui;
    gui.Frame ();
    const ImVec2 size = gui.size, position = gui.position;
    for (auto stage : { AnalysisRunStage::Idle, AnalysisRunStage::Waiting, AnalysisRunStage::Capturing,
                        AnalysisRunStage::Preparing, AnalysisRunStage::Calculating, AnalysisRunStage::Cancelling,
                        AnalysisRunStage::Publishing, AnalysisRunStage::Failed }) {
        gui.view.stage = stage;
        gui.view.resolved = 10;
        gui.view.total = 50;
        gui.Frame ();
        EXPECT_EQ (gui.size.x, size.x);
        EXPECT_EQ (gui.size.y, size.y);
        EXPECT_EQ (gui.position.x, position.x);
        EXPECT_EQ (gui.position.y, position.y);
    }
}

TEST (AnalysisRunControl, EntireBarActivatesAndCancellingNeverActivates)
{
    for (auto stage : { AnalysisRunStage::Idle, AnalysisRunStage::Calculating, AnalysisRunStage::Cancelling })
        for (float fraction : { 0.01f, 0.5f, 0.99f }) {
            RunGui gui;
            gui.view.stage = stage;
            gui.Frame ();
            const ImVec2 point (gui.position.x + gui.size.x * fraction, gui.position.y + gui.size.y * 0.5f);
            gui.Frame (point);
            gui.Frame (point, true);
            const auto action = gui.Frame (point);
            EXPECT_EQ (action, stage == AnalysisRunStage::Idle         ? AnalysisRunAction::Run
                               : stage == AnalysisRunStage::Cancelling ? AnalysisRunAction::None
                                                                       : AnalysisRunAction::Cancel);
        }
}

TEST (AnalysisRunControl, AtomicPublishingDisablesCancel)
{
    RunGui gui;
    gui.view.stage = AnalysisRunStage::Publishing;
    gui.view.cancellable = false;
    gui.Frame ();
    const ImVec2 point (gui.position.x + 10, gui.position.y + 10);
    gui.Frame (point, true);
    EXPECT_EQ (gui.Frame (point), AnalysisRunAction::None);
}

TEST (AnalysisRunControl, FocusedControlSupportsEnterAndSpace)
{
    for (auto key : { ImGuiKey_Enter, ImGuiKey_Space }) {
        RunGui gui;
        gui.Frame ();
        gui.Frame ({ -1, -1 }, false, true);
        gui.Frame ();
        ImGui::GetIO ().AddKeyEvent (key, true);
        EXPECT_EQ (gui.Frame (), AnalysisRunAction::Run);
        ImGui::GetIO ().AddKeyEvent (key, false);
        gui.Frame ();
    }
}
