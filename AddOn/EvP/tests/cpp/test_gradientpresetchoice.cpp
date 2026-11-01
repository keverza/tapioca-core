// ArchViz/GradientPresetChoice: the gradient picker (#102), driven through the real Dear
// ImGui with mouse and keyboard. A pick applies at once; Cancel and Escape put back
// what the popup opened on; Done keeps the pick.

#include "ArchViz/GradientPresetChoice.hpp"

#include <gtest/gtest.h>

#include <imgui.h>

#include <memory>
#include <string>

namespace choice = geomsrv::archviz::gradientchoice;
namespace layers = geomsrv::archviz::overlaylayers;

namespace {

struct Harness {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    std::string preset = "sunhours";
    bool changed = false;

    Harness ()
    {
        ImGuiIO& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = { 800, 900 };
        io.DeltaTime = 1.0f / 60;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~Harness ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }

    // One frame with the pointer at `mouse`, the left button `down`, and `key` pressed.
    void Frame (ImVec2 mouse = { 700, 800 }, bool down = false, ImGuiKey key = ImGuiKey_None)
    {
        ImGuiIO& io = ImGui::GetIO ();
        io.AddMousePosEvent (mouse.x, mouse.y);
        io.AddMouseButtonEvent (0, down);
        if (key != ImGuiKey_None)
            io.AddKeyEvent (key, true);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 400, 200 });
        ImGui::Begin ("picker", nullptr, ImGuiWindowFlags_NoDecoration);
        changed = choice::GradientPresetChoice ("##ramp", preset, 300.0f) || changed;
        ImGui::End ();
        ImGui::Render ();
        if (key != ImGuiKey_None)
            io.AddKeyEvent (key, false);
    }
    void Click (ImVec2 at)
    {
        Frame (at, true);
        Frame (at, false);
    }
    void Key (ImGuiKey key)
    {
        Frame ({ 700, 800 }, false, key);
        Frame ();
    }
    bool PopupOpen () const
    {
        return ImGui::IsPopupOpen ("", ImGuiPopupFlags_AnyPopupId);
    }
};

// The button sits where the window's padding puts it.
constexpr ImVec2 kButton { 40, 15 };

} // namespace

TEST (GradientPresetChoice, ArrowsAndEnterPickTheNextPresetAtOnce)
{
    Harness hud;
    hud.Frame ();
    hud.Click (kButton);
    ASSERT_TRUE (hud.PopupOpen ());
    hud.Frame ();
    hud.Key (ImGuiKey_DownArrow);
    hud.Key (ImGuiKey_Enter);
    EXPECT_TRUE (hud.changed);
    EXPECT_NE (hud.preset, "sunhours");
    const layers::GradientPreset* picked = layers::FindGradientPreset (hud.preset);
    ASSERT_NE (picked, nullptr);
    EXPECT_EQ (picked->category, layers::GradientCategory::Sun) << "the row under the current one, sunhours";
    EXPECT_TRUE (hud.PopupOpen ()) << "a pick is live: the popup stays for the next one";
}

TEST (GradientPresetChoice, EscapePutsBackWhatThePopupOpenedOn)
{
    Harness hud;
    hud.Frame ();
    hud.Click (kButton);
    hud.Frame ();
    hud.Key (ImGuiKey_DownArrow);
    hud.Key (ImGuiKey_Enter);
    ASSERT_NE (hud.preset, "sunhours");
    hud.changed = false;
    hud.Key (ImGuiKey_Escape);
    EXPECT_FALSE (hud.PopupOpen ());
    EXPECT_TRUE (hud.changed);
    EXPECT_EQ (hud.preset, "sunhours");
}

TEST (GradientPresetChoice, AClickOutsideKeepsThePick)
{
    Harness hud;
    hud.Frame ();
    hud.Click (kButton);
    hud.Frame ();
    hud.Key (ImGuiKey_DownArrow);
    hud.Key (ImGuiKey_Enter);
    const std::string picked = hud.preset;
    ASSERT_NE (picked, "sunhours");
    hud.Click ({ 790, 890 });
    EXPECT_FALSE (hud.PopupOpen ());
    EXPECT_EQ (hud.preset, picked);
}

TEST (GradientPresetChoice, CancelFromTheCallersOwnColoursGoesBackToNone)
{
    Harness hud;
    hud.preset.clear ();
    hud.Frame ();
    hud.Click (kButton);
    hud.Frame ();
    hud.Key (ImGuiKey_DownArrow);
    hud.Key (ImGuiKey_Enter);
    ASSERT_FALSE (hud.preset.empty ()) << "a preset was picked over the two colours";
    hud.Key (ImGuiKey_Escape);
    EXPECT_TRUE (hud.preset.empty ()) << "Escape restores the caller's own colours";
}

TEST (GradientPresetChoice, ColourAtInterpolatesBetweenExactStops)
{
    const layers::ColourStop stops[] = { { 0.0f, 0x000000FFu }, { 0.5f, 0xFF0000FFu }, { 1.0f, 0xFFFFFFFFu } };
    EXPECT_EQ (choice::ColourAt (stops, 0.0f), 0x000000FFu);
    EXPECT_EQ (choice::ColourAt (stops, 0.5f), 0xFF0000FFu);
    EXPECT_EQ (choice::ColourAt (stops, 0.75f), 0xFF8080FFu);
    EXPECT_EQ (choice::ColourAt (stops, 2.0f), 0xFFFFFFFFu);
}
