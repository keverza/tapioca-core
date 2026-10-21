// ArchViz/TapiocaUI: the library's foundation and its gallery (HANDOFF-TapiocaUI-Library.md, P0).
// A number typed with either decimal separator reads the same and is printed the same whatever
// the process's locale; a style pushed through ScopedStyle is popped on every path; the gallery
// draws today's controls at every density and in both themes and leaves ImGui exactly as it
// found it; and its log tells a commit from a cancel.

#include "ArchViz/TapiocaUI/Demo/WidgetGallery.hpp"
#include "ArchViz/TapiocaUI/Foundation/ScopedStyle.hpp"
#include "ArchViz/TapiocaUI/Foundation/Units.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <limits>
#include <string>

namespace ui = geomsrv::archviz::ui;

namespace {

// A context of its own, laid out in ImGui's default font, put back as it was afterwards.
struct Context {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    Context ()
    {
        ImGuiIO& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1200, 900 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~Context ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
};

// What a scope that leaked a push would leave behind.
struct Stacks {
    int colours = 0, vars = 0, fonts = 0;
    float fontSizeBase = 0.0f;
    ImVec4 text;
    ImVec2 framePadding;
    static Stacks Now ()
    {
        const ImGuiContext& g = *ImGui::GetCurrentContext ();
        return { g.ColorStack.Size,    g.StyleVarStack.Size,          g.FontStack.Size,
                 g.Style.FontSizeBase, g.Style.Colors[ImGuiCol_Text], g.Style.FramePadding };
    }
    bool operator== (const Stacks& o) const
    {
        return colours == o.colours && vars == o.vars && fonts == o.fonts && fontSizeBase == o.fontSizeBase &&
               text.x == o.text.x && text.y == o.text.y && text.z == o.text.z && text.w == o.text.w &&
               framePadding.x == o.framePadding.x && framePadding.y == o.framePadding.y;
    }
};

} // namespace

TEST (TapiocaUiUnits, EitherDecimalSeparatorReadsTheSameNumber)
{
    double value = 0.0;
    ASSERT_TRUE (ui::ParseNumber ("1,5", value));
    EXPECT_DOUBLE_EQ (value, 1.5);
    ASSERT_TRUE (ui::ParseNumber ("1.5", value));
    EXPECT_DOUBLE_EQ (value, 1.5);
    ASSERT_TRUE (ui::ParseNumber ("  -273,15\t", value));
    EXPECT_DOUBLE_EQ (value, -273.15);
    ASSERT_TRUE (ui::ParseNumber ("+2", value));
    EXPECT_DOUBLE_EQ (value, 2.0);
    ASSERT_TRUE (ui::ParseNumber ("1e9", value));
    EXPECT_DOUBLE_EQ (value, 1.0e9);
    ASSERT_TRUE (ui::ParseNumber ("0,000001", value));
    EXPECT_DOUBLE_EQ (value, 1.0e-6);
}

TEST (TapiocaUiUnits, AnythingButOneFiniteNumberIsRefusedAndLeavesTheValue)
{
    for (const char* text : { "", "   ", "abc", "1,2,3", "1.2.3", "1,234.5", "12 m", "1e", "+-5", "--5", "inf", "-inf",
                              "nan", "1e400", "0x10", "+" }) {
        double value = 42.0;
        EXPECT_FALSE (ui::ParseNumber (text, value)) << '"' << text << '"';
        EXPECT_EQ (value, 42.0) << '"' << text << '"';
    }
}

TEST (TapiocaUiUnits, FormattingIsFixedAndNeverPrintsNegativeZero)
{
    EXPECT_EQ (ui::FormatNumber (3.14159, 2), "3.14");
    EXPECT_EQ (ui::FormatNumber (-1.5, 1), "-1.5");
    EXPECT_EQ (ui::FormatNumber (1.0e9, 0), "1000000000");
    EXPECT_EQ (ui::FormatNumber (-0.001, 2), "0.00");
    EXPECT_EQ (ui::FormatNumber (-0.0, 0), "0");
    EXPECT_EQ (ui::FormatNumber (1.0, 20), "1.000000000"); // at most kMaxDecimals
    EXPECT_EQ (ui::FormatNumber (std::numeric_limits<double>::quiet_NaN (), 2), "--");
    EXPECT_EQ (ui::FormatNumber (std::numeric_limits<double>::infinity (), 2), "--");
    // What is printed reads back as itself.
    double value = 0.0;
    ASSERT_TRUE (ui::ParseNumber (ui::FormatNumber (-273.15, 2), value));
    EXPECT_DOUBLE_EQ (value, -273.15);
}

TEST (TapiocaUiScopedStyle, EveryPushIsPoppedWhenTheScopeEnds)
{
    Context context;
    ImGui::NewFrame ();
    ImGui::Begin ("scoped");
    const Stacks before = Stacks::Now ();
    {
        ui::ScopedStyle style;
        style.Colour (ImGuiCol_Text, IM_COL32 (255, 0, 0, 255))
            .Colour (ImGuiCol_Button, ImVec4 (0, 1, 0, 1))
            .Var (ImGuiStyleVar_FramePadding, ImVec2 (9, 9))
            .Var (ImGuiStyleVar_Alpha, 0.5f)
            .Font (nullptr, ImGui::GetStyle ().FontSizeBase * 2.0f);
        // A legacy helper's own pushes, adopted: hudshell::PushLook's way of returning a count.
        ImGui::PushStyleColor (ImGuiCol_Border, IM_COL32 (0, 0, 255, 255));
        ImGui::PushStyleVar (ImGuiStyleVar_FrameRounding, 3.0f);
        style.Adopt (1, 1);
        EXPECT_EQ (style.Colours (), 3);
        EXPECT_EQ (style.Vars (), 3);
        EXPECT_EQ (style.Fonts (), 1);
        EXPECT_FALSE (Stacks::Now () == before);
    }
    EXPECT_TRUE (Stacks::Now () == before);
    ImGui::End ();
    ImGui::Render ();
}

TEST (TapiocaUiScopedStyle, AnEarlyPopLeavesTheDestructorNothing)
{
    Context context;
    ImGui::NewFrame ();
    const Stacks before = Stacks::Now ();
    {
        ui::ScopedStyle style;
        style.Colour (ImGuiCol_Text, IM_COL32_WHITE).Var (ImGuiStyleVar_Alpha, 0.5f);
        style.Pop ();
        EXPECT_TRUE (Stacks::Now () == before);
        EXPECT_EQ (style.Colours () + style.Vars () + style.Fonts (), 0);
        style.Adopt (-3, -1); // a negative count is nothing to pop
    }
    EXPECT_TRUE (Stacks::Now () == before);
    ImGui::Render ();
}

TEST (TapiocaUiGallery, DrawsAtEveryDensityInBothThemesAndLeavesImGuiAsItFoundIt)
{
    Context context;
    for (uint32_t density = 0; density < ui::tokens::kDensityStepCount; ++density)
        for (const bool dark : { false, true }) {
            ui::GalleryState state;
            state.density = density;
            state.dark = dark;
            bool open = true;
            ImGui::NewFrame ();
            const Stacks before = Stacks::Now ();
            ui::GalleryWindow (state, open);
            EXPECT_TRUE (Stacks::Now () == before) << "density " << density << (dark ? " dark" : " light");
            ImGui::Render ();
            EXPECT_TRUE (open);
            EXPECT_GT (ImGui::GetDrawData ()->TotalVtxCount, 0);
            // Nothing was edited, so nothing was logged.
            EXPECT_TRUE (state.log.empty ());
        }
}

TEST (TapiocaUiGallery, DensityScalesTheTextItDraws)
{
    Context context;
    float heights[2] = {};
    for (int i = 0; i < 2; ++i) {
        ui::GalleryState state;
        state.density = i == 0 ? 0u : ui::tokens::kDensityStepCount - 1; // 80 % and 200 %
        bool open = true;
        ImGui::NewFrame ();
        ui::GalleryWindow (state, open);
        ImGuiWindow* const window = ImGui::FindWindowByName ("###tapioca.ui.gallery");
        ASSERT_NE (window, nullptr);
        heights[i] = window->ContentSize.y;
        ImGui::Render ();
    }
    EXPECT_GT (heights[1], heights[0] * 2.0f);
}

TEST (TapiocaUiGallery, TheLogTellsACommitFromACancel)
{
    Context context;
    ImGuiIO& io = ImGui::GetIO ();
    ui::GalleryState state;
    char text[32] = "1,5";
    // A frame: the field asked for focus when `focus`, `key` pressed and released, `typed` typed.
    const auto frame = [&] (bool focus, ImGuiKey key, const char* typed) {
        if (key != ImGuiKey_None)
            io.AddKeyEvent (key, true);
        if (typed != nullptr)
            io.AddInputCharactersUTF8 (typed);
        ImGui::NewFrame ();
        ImGui::Begin ("log");
        if (focus)
            ImGui::SetKeyboardFocusHere ();
        ImGui::InputText ("##typed", text, sizeof text);
        ui::ObserveLastItem (state, "typed");
        ImGui::End ();
        ImGui::Render ();
        if (key != ImGuiKey_None)
            io.AddKeyEvent (key, false);
    };

    // SetKeyboardFocusHere resolves through a navigation request: active one frame later.
    frame (true, ImGuiKey_None, nullptr);
    frame (false, ImGuiKey_None, nullptr);
    frame (false, ImGuiKey_None, "2");
    ASSERT_EQ (state.log.size (), 1u);
    EXPECT_TRUE (state.log[0].result.changed);
    EXPECT_FALSE (state.log[0].result.committed); // typing never commits
    frame (false, ImGuiKey_Enter, nullptr);
    frame (false, ImGuiKey_None, nullptr);
    // Enter in a single-line field leaves it, after an edit: the commit.
    ASSERT_EQ (state.log.size (), 1u);
    EXPECT_TRUE (state.log[0].result.committed);
    EXPECT_FALSE (state.log[0].result.canceled);
    EXPECT_EQ (std::string (text), "2");

    frame (true, ImGuiKey_None, nullptr);
    frame (false, ImGuiKey_None, nullptr);
    frame (false, ImGuiKey_None, "9");
    frame (false, ImGuiKey_Escape, nullptr);
    ASSERT_EQ (state.log.size (), 1u);
    EXPECT_TRUE (state.log[0].result.canceled);
    // Keyboard focus selects the whole field, so each edit replaced it; Escape put back what the
    // field held before the cancelled one.
    EXPECT_EQ (std::string (text), "2");
}

TEST (TapiocaUiGallery, RecordKeepsOneEntryPerControlTheLatestFirst)
{
    Context context;
    ImGui::NewFrame ();
    ui::GalleryState state;
    ui::EditResult nothing;
    ui::Record (state, "a", nothing);
    EXPECT_TRUE (state.log.empty ());
    ui::EditResult changed;
    changed.changed = true;
    ui::Record (state, "a", changed);
    ui::Record (state, "b", changed);
    ui::EditResult committed;
    committed.committed = true;
    ui::Record (state, "a", committed);
    ASSERT_EQ (state.log.size (), 2u);
    EXPECT_EQ (state.log[0].control, "a");
    EXPECT_TRUE (state.log[0].result.committed);
    EXPECT_FALSE (state.log[0].result.changed);
    EXPECT_EQ (state.log[1].control, "b");
    ImGui::Render ();
}
