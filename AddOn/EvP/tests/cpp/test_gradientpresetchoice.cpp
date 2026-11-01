// ArchViz/GradientPresetChoice: the gradient picker (#102), driven through the real Dear
// ImGui with mouse and keyboard. A pick applies at once; Cancel and Escape put back
// what the popup opened on; Done keeps the pick.

#include "ArchViz/GradientPresetChoice.hpp"

#include <gtest/gtest.h>

#include <imgui.h>

#include <memory>
#include <string>
#include <vector>

namespace choice = geomsrv::archviz::gradientchoice;
namespace layers = geomsrv::archviz::overlaylayers;

namespace {

struct Harness {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    std::string preset = "sunhours";
    std::vector<layers::ColourStop> custom;
    const char* none = nullptr;
    uint32_t swatch = 0x336699FFu;
    bool swatchMode = false; // draw a ColourChoice instead of the picker
    bool editMode = false;   // draw the custom stop editor instead
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
        if (editMode)
            changed = choice::EditStops (custom, 300.0f) || changed;
        else if (swatchMode)
            changed = choice::ColourChoice ("##swatch", swatch, false, "colour") || changed;
        else
            changed = choice::GradientPresetChoice ("##ramp", preset, custom, 300.0f, layers::GradientCategory::General,
                                                    none) ||
                      changed;
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
    // Pressed at `from`, moved to `to`, released there -- hovered first, as a pointer is.
    void Drag (ImVec2 from, ImVec2 to)
    {
        Frame (from, false);
        Frame (from, true);
        Frame (to, true);
        Frame (to, false);
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

TEST (GradientPresetChoice, CustomStartsAsAnEditableRampAndCancelRestoresPresetAndStops)
{
    Harness hud;
    hud.preset.clear ();
    hud.Frame ();
    hud.Click (kButton);
    hud.Frame ();
    // Nothing selected: down to the first preset, then up to the row above it, Custom...
    hud.Key (ImGuiKey_DownArrow);
    hud.Key (ImGuiKey_UpArrow);
    hud.Key (ImGuiKey_Enter);
    EXPECT_EQ (hud.preset, layers::kCustomGradientId);
    ASSERT_EQ (hud.custom.size (), 2u) << "a fresh custom ramp is two stops to edit";
    EXPECT_FLOAT_EQ (hud.custom.front ().at, 0.0f);
    EXPECT_FLOAT_EQ (hud.custom.back ().at, 1.0f);
    hud.Key (ImGuiKey_Escape);
    EXPECT_TRUE (hud.preset.empty ());
    EXPECT_TRUE (hud.custom.empty ()) << "Escape puts the stops back too";
}

TEST (GradientPresetChoice, TheCallersOwnColoursAreARowToGoBackTo)
{
    Harness hud;
    hud.preset.clear ();
    hud.none = "Two colours";
    hud.Frame ();
    hud.Click (kButton);
    hud.Frame ();
    hud.Key (ImGuiKey_DownArrow); // Two colours -> Custom...
    hud.Key (ImGuiKey_DownArrow); // -> the first preset
    hud.Key (ImGuiKey_Enter);
    ASSERT_FALSE (hud.preset.empty ());
    ASSERT_NE (hud.preset, layers::kCustomGradientId);
    hud.Key (ImGuiKey_UpArrow);
    hud.Key (ImGuiKey_UpArrow);
    hud.Key (ImGuiKey_Enter);
    EXPECT_TRUE (hud.preset.empty ()) << "picked back without Reset, so the caller's colours are kept";
    EXPECT_TRUE (hud.PopupOpen ());
}

TEST (GradientPresetChoice, ASwatchCanAlwaysBeRolledBack)
{
    Harness hud;
    hud.swatchMode = true;
    hud.Frame ();
    hud.Frame ({ 15, 15 }); // hovered first, as a pointer is
    hud.Click ({ 15, 15 });
    ASSERT_TRUE (hud.PopupOpen ());
    hud.Frame ({ 15, 15 });
    // Into the picker's saturation/value square, under the popup's corner.
    hud.Click ({ 60, 70 });
    ASSERT_TRUE (hud.changed);
    ASSERT_NE (hud.swatch, 0x336699FFu);
    hud.Key (ImGuiKey_Escape);
    EXPECT_FALSE (hud.PopupOpen ());
    EXPECT_EQ (hud.swatch, 0x336699FFu) << "Escape is the rollback";
}

// ---- the custom stop editor ---------------------------------------------------------

namespace {

std::vector<layers::ColourStop> BlackToWhite ()
{
    return { { 0.0f, 0x000000FFu }, { 1.0f, 0xFFFFFFFFu } };
}

// EditStops' geometry in the harness window: the bar 1.4 frame heights tall at the
// window padding, its circles kept inside 300 px, the arrows under it.
struct Bar {
    float frame = ImGui::GetFrameHeight ();
    float bar = 1.4f * frame, radius = 0.4f * bar;
    float left = 8.0f + radius, span = 300.0f - 2.0f * radius;
    float middle = 8.0f + 0.5f * bar, arrows = 8.0f + bar + 6.0f;
    ImVec2 At (float t, float y) const
    {
        return { left + span * t, y };
    }
};

} // namespace

TEST (GradientStops, AddingSplitsASegmentInTheColourShownAndDropsItsBlend)
{
    std::vector<layers::ColourStop> stops = BlackToWhite ();
    ASSERT_TRUE (choice::SetBlendMidpoint (stops, 0, 0.25f));
    ASSERT_EQ (stops.size (), 3u);
    EXPECT_TRUE (stops[1].hint);
    EXPECT_EQ (stops[1].rgba, 0x808080FFu) << "a blend midpoint is its two colours half and half";
    const uint32_t shown = choice::ColourAt (stops, 0.6f);
    const int added = choice::AddColourStop (stops, 0.6f);
    ASSERT_EQ (added, 1);
    EXPECT_EQ (stops[1].rgba, shown);
    EXPECT_EQ (stops.size (), 3u) << "the split segment's blend goes";
    EXPECT_EQ (choice::ColourStopCount (stops), 3);
}

TEST (GradientStops, MovingAColourKeepsItsBlendsInPlaceAndTheEndsStay)
{
    std::vector<layers::ColourStop> stops = BlackToWhite ();
    ASSERT_EQ (choice::AddColourStop (stops, 0.5f), 1);
    ASSERT_TRUE (choice::SetBlendMidpoint (stops, 0, 0.125f)); // a quarter into [0, 0.5]
    ASSERT_TRUE (stops[1].hint);
    ASSERT_TRUE (choice::MoveColourStop (stops, 2, 0.8f));
    EXPECT_FLOAT_EQ (stops[2].at, 0.8f);
    EXPECT_NEAR (stops[1].at, 0.2f, 1e-6f) << "still a quarter into its segment";
    EXPECT_FALSE (choice::MoveColourStop (stops, 0, 0.3f)) << "the ends stay at 0 and 1";
    EXPECT_FALSE (choice::MoveColourStop (stops, 1, 0.3f)) << "a blend is not a colour";
    // Back to the middle, a blend is even again and needs no hint.
    ASSERT_TRUE (choice::SetBlendMidpoint (stops, 0, 0.4f));
    EXPECT_EQ (stops.size (), 3u);
    EXPECT_EQ (choice::ColourStopCount (stops), 3);
}

TEST (GradientStops, RemovingAColourTakesItsBlendsAndAnEndHandsOver)
{
    std::vector<layers::ColourStop> stops = BlackToWhite ();
    ASSERT_EQ (choice::AddColourStop (stops, 0.5f), 1);
    ASSERT_TRUE (choice::SetBlendMidpoint (stops, 0, 0.1f));
    ASSERT_TRUE (choice::SetBlendMidpoint (stops, 2, 0.9f));
    ASSERT_EQ (stops.size (), 5u);
    ASSERT_TRUE (choice::RemoveColourStop (stops, 2));
    EXPECT_EQ (stops.size (), 2u) << "both blends went with it";
    EXPECT_FALSE (choice::RemoveColourStop (stops, 0)) << "a ramp keeps two colours";
    ASSERT_EQ (choice::AddColourStop (stops, 0.3f), 1);
    ASSERT_TRUE (choice::RemoveColourStop (stops, 0));
    EXPECT_FLOAT_EQ (stops.front ().at, 0.0f) << "the next colour becomes the end";
    EXPECT_EQ (stops.front ().rgba, choice::ColourAt (BlackToWhite (), 0.3f));
}

TEST (GradientStops, NeverMoreThanSixteen)
{
    std::vector<layers::ColourStop> stops = BlackToWhite ();
    for (int i = 1; i < 30; ++i)
        choice::AddColourStop (stops, float (i) / 30.0f);
    EXPECT_EQ (stops.size (), layers::kMaxStops);
    EXPECT_FALSE (choice::SetBlendMidpoint (stops, 0, 0.01f));
}

TEST (GradientStopEditor, ClickTheBarToAddDragOffToRemove)
{
    Harness hud;
    hud.editMode = true;
    hud.custom = BlackToWhite ();
    hud.Frame ();
    const Bar bar;
    hud.Click (bar.At (0.5f, bar.middle));
    ASSERT_EQ (choice::ColourStopCount (hud.custom), 3) << "a click on the bar adds a colour";
    EXPECT_FALSE (hud.PopupOpen ()) << "adding is not picking a colour";
    hud.Drag (bar.At (0.5f, bar.middle), bar.At (0.7f, bar.middle));
    EXPECT_NEAR (hud.custom[1].at, 0.7f, 0.01f) << "dragged along, it moves";
    hud.Drag (bar.At (0.7f, bar.middle), bar.At (0.7f, bar.middle + 4.0f * bar.bar));
    EXPECT_EQ (choice::ColourStopCount (hud.custom), 2) << "dragged off the bar, it goes";
}

TEST (GradientStopEditor, ClickAColourForItsPickerDragAnArrowForTheBlend)
{
    Harness hud;
    hud.editMode = true;
    hud.custom = BlackToWhite ();
    hud.Frame ();
    const Bar bar;
    hud.Drag (bar.At (0.5f, bar.arrows), bar.At (0.25f, bar.arrows));
    ASSERT_EQ (hud.custom.size (), 3u);
    EXPECT_TRUE (hud.custom[1].hint);
    EXPECT_NEAR (hud.custom[1].at, 0.25f, 0.01f) << "the blend is half way at the arrow";
    EXPECT_FALSE (hud.PopupOpen ());
    hud.Click (bar.At (1.0f, bar.middle));
    EXPECT_TRUE (hud.PopupOpen ()) << "a click on a colour opens its picker";
}
