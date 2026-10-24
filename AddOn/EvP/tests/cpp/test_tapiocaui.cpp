// ArchViz/TapiocaUI: the library's foundation and its gallery (HANDOFF-TapiocaUI-Library.md, P0).
// A number typed with either decimal separator reads the same and is printed the same whatever
// the process's locale; a style pushed through ScopedStyle is popped on every path; the gallery
// draws today's controls at every density and in both themes and leaves ImGui exactly as it
// found it; and its log tells a commit from a cancel.

#include "hud_fixture.hpp"

#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudShellTabs.hpp"
#include "ArchViz/TapiocaUI/Demo/WidgetGallery.hpp"
#include "ArchViz/TapiocaUI/Foundation/IconData.hpp"
#include "ArchViz/TapiocaUI/Foundation/Icons.hpp"
#include "ArchViz/TapiocaUI/Foundation/ScopedStyle.hpp"
#include "ArchViz/TapiocaUI/Foundation/Units.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
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

// ⚠️ THE OVERLAY'S DEBUG PAGE HAS IT TOO (the user, 2026-10-21: looked for it in the 3D view's HUD).
// Through the overlays' real layout engine: folded, the gallery is one header row at the page's
// end; pressing that row unfolds the whole gallery, laid out in the overlay's own font atlas.
TEST (TapiocaUiGallery, TheOverlaysDebugPageUnfoldsTheGalleryFromItsLastRow)
{
    using namespace hudtest;
    Watched hud;
    hud::OwnPages pages;
    pages.standalone = true;
    hud.engine.SetOwnPages (pages);
    hud::SelectKey (*hud.state, geomsrv::archviz::hudshell::kDebugKey);
    const hud::Layout folded = hud.Lay ({}, At (600, 600));
    ASSERT_FALSE (folded.host.vertices.empty ());
    const float left = folded.host.fraction[0] * 1200 + folded.host.offset[0];
    const float top = folded.host.fraction[1] * 800 + folded.host.offset[1];

    // The page's last pressable row, from its bottom up: the gallery's header.
    float row = -1.0f;
    for (float y = top + folded.host.height - 36; y > top && row < 0; y -= 2)
        if (hud.Lay ({}, At (left + 30, y)).hand)
            row = y;
    ASSERT_GT (row, 0.0f);
    hud.Click ({}, left + 30, row);
    const hud::Layout open = hud.Lay ({}, At (600, 600));
    // The gallery adds its controls to the page: thousands of vertices (5300 at this writing),
    // while the page's own content is capped by the view's height, so a ratio would not hold.
    EXPECT_GT (open.host.vertices.size (), folded.host.vertices.size () + 1000)
        << "the gallery's controls are drawn once its section is open";
}

namespace {

// WCAG's relative luminance of an sRGB colour, and the contrast ratio between two.
double Relative (const ImVec4& c)
{
    const auto linear = [] (double v) { return v <= 0.04045 ? v / 12.92 : std::pow ((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear (c.x) + 0.7152 * linear (c.y) + 0.0722 * linear (c.z);
}
double Ratio (const ImVec4& a, const ImVec4& b)
{
    const double la = Relative (a), lb = Relative (b);
    return ((std::max) (la, lb) + 0.05) / ((std::min) (la, lb) + 0.05);
}
// `over` laid on an opaque `ground` by its alpha.
ImVec4 Over (const ImVec4& over, const ImVec4& ground)
{
    const float a = over.w;
    return { over.x * a + ground.x * (1 - a), over.y * a + ground.y * (1 - a), over.z * a + ground.z * (1 - a), 1.0f };
}

} // namespace

// ⚠️ THE USER, 2026-10-21, FROM THE GALLERY'S BASELINE SCREENSHOTS: tabs and sections are grey,
// not blue, and nothing on either card may be illegible -- the light card's title read dark on
// dark blue, and the dark card's radio circles, small buttons and muted text all but vanished.
TEST (TapiocaUiLook, TabsAndSectionsAreTheCardsGreyInkAndBothCardsStayLegible)
{
    namespace shell = geomsrv::archviz::hudshell;
    namespace layers = geomsrv::archviz::overlaylayers;
    Context context;
    layers::Panel darkLook;
    darkLook.sizePixels = shell::PlainLook ().sizePixels;
    const layers::Panel* const looks[] = { &shell::PlainLook (), &darkLook };
    for (const layers::Panel* look : looks) {
        const bool dark = look == &darkLook;
        ImGui::NewFrame ();
        const int colours = shell::PushLook (*look, 1.0f);
        const ImVec4 ink = shell::Colour (look->textRgba);
        const ImVec4 ground = shell::Colour (look->backgroundRgba | 0xFFu);
        const auto colour = [] (ImGuiCol index) { return ImGui::GetStyleColorVec4 (index); };
        const ImGuiCol greys[] = { ImGuiCol_Header,     ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive,     ImGuiCol_Tab,
                                   ImGuiCol_TabHovered, ImGuiCol_TabSelected,   ImGuiCol_TabDimmedSelected };
        for (const ImGuiCol grey : greys) {
            const ImVec4 c = colour (grey);
            EXPECT_TRUE (c.x == ink.x && c.y == ink.y && c.z == ink.z)
                << ImGui::GetStyleColorName (grey) << (dark ? " dark" : " light") << " is not the card's ink";
        }
        EXPECT_EQ (colour (ImGuiCol_TitleBgActive).w, 1.0f);
        EXPECT_GE (Ratio (ink, colour (ImGuiCol_TitleBgActive)), 4.5) << (dark ? "dark" : "light");
        EXPECT_GE (Ratio (Over (colour (ImGuiCol_TextDisabled), ground), ground), 3.0) << (dark ? "dark" : "light");
        if (dark) {
            EXPECT_GE (Ratio (Over (colour (ImGuiCol_FrameBg), ground), ground), 1.6)
                << "a radio circle on the dark card";
            EXPECT_GE (Ratio (Over (colour (ImGuiCol_Button), ground), ground), 1.3)
                << "a small button on the dark card";
        }
        ImGui::PopStyleColor (colours);
        ImGui::PopStyleVar (shell::kLookVars);
        ImGui::Render ();
    }
}

// ⚠️ EVERY ID THE REGISTRY GIVES IS A GLYPH (HANDOFF-TapiocaUI-Library.md section 7): a file named,
// strokes in the table, every point on the 24 grid's 0 to 1, and an id past the end draws the
// placeholder rather than reading past the table.
TEST (TapiocaUiIcons, EveryRegisteredIdIsAGlyphOnItsGrid)
{
    namespace data = ui::icondata;
    ASSERT_GE (ui::IconCount (), 21u);
    EXPECT_STREQ (ui::IconFile (ui::IconId::None), "");
    EXPECT_STREQ (ui::IconName (ui::IconId::BooleanSubtract), "BooleanSubtract");
    EXPECT_STREQ (ui::IconFile (ui::IconId::BooleanSubtract), "substract.svg"); // upstream's spelling
    EXPECT_STREQ (ui::IconFile (ui::IconId (60000)), "");                       // past the end: None
    uint32_t nextStroke = 0;
    for (uint32_t id = 0; id < ui::IconCount (); ++id) {
        const data::Glyph& glyph = data::GlyphAt (id);
        EXPECT_EQ (glyph.first, nextStroke) << glyph.name;
        EXPECT_GT (glyph.count, 0u) << glyph.name;
        nextStroke = glyph.first + glyph.count;
        if (id > 0)
            EXPECT_NE (glyph.file[0], 0) << glyph.name;
        for (uint16_t s = 0; s < glyph.count; ++s) {
            const data::Stroke& stroke = data::Strokes ()[glyph.first + s];
            ASSERT_GT (stroke.count, 0u) << glyph.name;
            for (uint32_t i = stroke.first; i < stroke.first + stroke.count; ++i)
                for (int axis = 0; axis < 2; ++axis) {
                    const float v = data::Points ()[2 * i + axis];
                    EXPECT_TRUE (v >= 0.0f && v <= 1.0f) << glyph.name << " point " << i;
                }
        }
    }
}

// A glyph is drawn in the colour asked, inside its square (by at most half a stroke), at any size.
TEST (TapiocaUiIcons, AGlyphIsDrawnInItsColourInsideItsSquare)
{
    Context context;
    for (const float size : { 12.0f, 16.0f, 24.0f, 48.0f }) {
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos (ImVec2 (0, 0));
        ImGui::Begin ("icons", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
        ImDrawList* const draw = ImGui::GetWindowDrawList ();
        const ImU32 colour = IM_COL32 (12, 34, 56, 255);
        const int before = draw->VtxBuffer.Size;
        const ImVec2 at (100.0f, 200.0f);
        ui::Icon (ui::IconId::BooleanUnion, at, size, colour);
        ASSERT_GT (draw->VtxBuffer.Size, before) << size;
        const float margin = (std::max) (1.5f / 24.0f * size, 1.0f) * 0.5f + 1.5f; // half a stroke, and AA
        int inColour = 0;
        for (int v = before; v < draw->VtxBuffer.Size; ++v) {
            const ImDrawVert& vertex = draw->VtxBuffer[v];
            if ((vertex.col & 0x00FFFFFFu) != (colour & 0x00FFFFFFu))
                continue;
            ++inColour;
            EXPECT_GE (vertex.pos.x, at.x - margin) << size;
            EXPECT_LE (vertex.pos.x, at.x + size + margin) << size;
            EXPECT_GE (vertex.pos.y, at.y - margin) << size;
            EXPECT_LE (vertex.pos.y, at.y + size + margin) << size;
        }
        EXPECT_GT (inColour, 0) << size;
        ImGui::End ();
        ImGui::Render ();
    }
}

// An icon button answers a press with true once, and never while disabled.
TEST (TapiocaUiIcons, AnIconButtonIsPressedOnceAndNeverWhileDisabled)
{
    Context context;
    ImGuiIO& io = ImGui::GetIO ();
    ImVec2 centre;
    const auto frame = [&] (bool disabled, bool down) {
        io.AddMousePosEvent (centre.x, centre.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos (ImVec2 (10, 10));
        ImGui::Begin ("buttons");
        ImGui::BeginDisabled (disabled);
        const ImVec2 at = ImGui::GetCursorScreenPos ();
        const bool pressed = ui::IconButton ("cut", ui::IconId::Cut);
        ImGui::EndDisabled ();
        const float side = ImGui::GetFrameHeight ();
        centre = ImVec2 (at.x + side * 0.5f, at.y + side * 0.5f);
        ImGui::End ();
        ImGui::Render ();
        return pressed;
    };
    frame (false, false); // lays the button out: now its centre is known
    frame (false, false);
    EXPECT_FALSE (frame (false, true));
    EXPECT_TRUE (frame (false, false)) << "a click is a press and a release on the button";
    EXPECT_FALSE (frame (false, false));
    EXPECT_FALSE (frame (true, true));
    EXPECT_FALSE (frame (true, false)) << "disabled, the same click does nothing";
}

// ⚠️ THE USER, 2026-10-21: "Icons in light mode should be dark gray. Black should be for selected
// icon only." A glyph at rest is the card's ink made lighter, the selected one is the full ink,
// and neither is the accent.
TEST (TapiocaUiIcons, AGlyphAtRestIsGreyAndOnlyTheSelectedOneIsFullInk)
{
    Context context;
    ImGui::StyleColorsLight ();
    ImGui::NewFrame ();
    ImGui::SetNextWindowPos (ImVec2 (0, 0));
    ImGui::Begin ("ink", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
    const ImU32 rest = ui::IconInk (false), selected = ui::IconInk (true);
    EXPECT_EQ (rest & 0x00FFFFFFu, selected & 0x00FFFFFFu) << "the same ink";
    EXPECT_LT (rest >> 24, selected >> 24) << "lighter at rest";
    EXPECT_EQ (selected, ImGui::GetColorU32 (ImGuiCol_Text));
    EXPECT_NE (selected & 0x00FFFFFFu, ImGui::GetColorU32 (ImGuiCol_CheckMark) & 0x00FFFFFFu) << "not the accent";
    ImDrawList* const draw = ImGui::GetWindowDrawList ();
    const auto drawnIn = [draw] (int from, ImU32 colour) {
        for (int v = from; v < draw->VtxBuffer.Size; ++v)
            if (draw->VtxBuffer[v].col == colour)
                return true;
        return false;
    };
    int from = draw->VtxBuffer.Size;
    ui::IconButton ("rest", ui::IconId::Cut);
    EXPECT_TRUE (drawnIn (from, rest));
    EXPECT_FALSE (drawnIn (from, selected));
    from = draw->VtxBuffer.Size;
    ui::IconButton ("selected", ui::IconId::Cut, true);
    EXPECT_TRUE (drawnIn (from, selected));
    EXPECT_FALSE (drawnIn (from, rest));
    ImGui::End ();
    ImGui::Render ();
}

// ⚠️ THE HUD'S OWN TABS ARE GLYPHS (HANDOFF-TapiocaUI-Library.md section 7.8): Selection, Settings
// and Debug draw the pointer, the gear and the bug, at rest in the card's grey ink and the shown
// one in its full ink; Stats keeps its title. Through the overlays' real layout engine, on the
// plain light card.
TEST (TapiocaUiIcons, TheHudsOwnTabsAreGlyphsGreyAtRest)
{
    using namespace hudtest;
    namespace shell = geomsrv::archviz::hudshell;
    const uint32_t ink = shell::PlainLook ().textRgba;
    const uint32_t rest = (ink & 0xFFFFFF00u) | uint32_t (std::lround (float (ink & 0xFFu) * ui::kIconRestAlpha));
    const auto inTabRow = [] (const hud::Built& host, uint32_t rgba, float rowBottom) {
        int count = 0;
        for (const hud::Vertex& v : host.vertices)
            if (v.rgba == rgba && v.y < rowBottom)
                ++count;
        return count;
    };
    Watched hud;
    hud::OwnPages pages;
    pages.standalone = true;
    hud.engine.SetOwnPages (pages);
    hud::SelectKey (*hud.state, shell::kStatsKey);
    const hud::Layout stats = hud.Lay ({}, At (600, 600));
    ASSERT_FALSE (stats.host.vertices.empty ());
    float top = FLT_MAX;
    for (const hud::Vertex& v : stats.host.vertices)
        top = (std::min) (top, v.y);
    const float rowBottom = top + 100.0f; // workflow selector and utility navigation
    EXPECT_GT (inTabRow (stats.host, rest, rowBottom), 0) << "Selection, Settings and Debug at rest";
    hud::SelectKey (*hud.state, shell::kSettingsKey);
    const hud::Layout settings = hud.Lay ({}, At (600, 600));
    EXPECT_GT (inTabRow (settings.host, rest, rowBottom), 0) << "Selection and Debug still at rest";
}

// ⚠️ THE USER, 2026-10-21: "Tabs that have subtabs should have same selected color for whole group
// to indicate that it is child tabs. Selected subtab then should be darker." The child row sits on
// a band in the panel's selected-tab colour; the shown child is the darker grey laid over it, and
// no child carries the accent line. The style is as it was afterwards.
TEST (TapiocaUiTabs, ChildTabsSitOnTheirParentsColourAndTheShownOneIsDarker)
{
    namespace shell = geomsrv::archviz::hudshell;
    Context context;
    ImGui::NewFrame ();
    const int colours = shell::PushLook (shell::PlainLook (), 1.0f);
    const ImU32 parent = ImGui::GetColorU32 (ImGuiCol_TabSelected);
    const ImU32 darker = ImGui::GetColorU32 (ImGuiCol_HeaderActive);
    const ImU32 accent = ImGui::GetColorU32 (ImGuiCol_TabSelectedOverline);
    ASSERT_NE (parent, darker);
    ImGui::SetNextWindowPos (ImVec2 (0, 0));
    ImGui::SetNextWindowSize (ImVec2 (400, 200));
    ImGui::Begin ("children", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
    ImDrawList* const draw = ImGui::GetWindowDrawList ();
    const int from = draw->VtxBuffer.Size;
    const Stacks before = Stacks::Now ();
    ASSERT_TRUE (shell::BeginSubTabs ("##children"));
    if (ImGui::BeginTabItem ("Sun study"))
        ImGui::EndTabItem ();
    if (ImGui::BeginTabItem ("View quality"))
        ImGui::EndTabItem ();
    shell::EndSubTabs ();
    EXPECT_TRUE (Stacks::Now () == before);
    bool band = false, shown = false, line = false;
    for (int v = from; v < draw->VtxBuffer.Size; ++v) {
        band = band || draw->VtxBuffer[v].col == parent;
        shown = shown || draw->VtxBuffer[v].col == darker;
        line = line || draw->VtxBuffer[v].col == accent;
    }
    EXPECT_TRUE (band) << "the row's band in the parent tab's colour";
    EXPECT_TRUE (shown) << "the shown child, darker";
    EXPECT_FALSE (line) << "no accent line on a child";
    ImGui::End ();
    ImGui::PopStyleColor (colours);
    ImGui::PopStyleVar (shell::kLookVars);
    ImGui::Render ();
}
