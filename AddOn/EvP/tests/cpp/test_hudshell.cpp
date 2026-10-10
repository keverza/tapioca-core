// ArchViz/HudShell: the HUD's tips and the text in its panel (the user, 2026-10-03). A tip is
// said beside what it names on a light ground, its arrow at it, not at the pointer -- to the
// left of the dock's circles, turned to the other side where the view has no room. A panel of
// a set width wraps its text inside its padding instead of cutting it off at its edge. And the
// panel is never taller than the view: a page longer than it scrolls under the tab row.

#include "hud_fixture.hpp"

#include "ArchViz/HudShell.hpp"

#include <gtest/gtest.h>
#include <imgui_internal.h>

#include <utility>

using namespace hudtest;

namespace shell = geomsrv::archviz::hudshell;

namespace {

// A figure's own colour, so its glyphs are told from the rest of the card's.
constexpr uint32_t kValueRgba = 0x2E7D32FFu;

hud::OwnPages Standalone ()
{
    hud::OwnPages pages;
    pages.standalone = true;
    pages.overlay.phase = shell::Phase::Ready;
    pages.overlay.tip = "The overlay: press to hide it and its HUD";
    pages.viewer.tip = "The separate viewer: press to switch to it";
    return pages;
}

// The dock's top circle, in view pixels (test_overlayhudown.cpp's).
std::pair<float, float> TopCircle (const hud::Built& dock)
{
    return { 1200.0f + dock.offset[0] + dock.width * 0.5f, 400.0f + dock.offset[1] + dock.width * 0.5f };
}

} // namespace

// ⚠️ THE USER: the vertical tab's hover text on a light background, as a tooltip to the left
// side, not where the pointer is.
TEST (HudTips, TheDocksCircleSaysItsTipToItsLeftOnALightGround)
{
    Watched hud;
    hud.engine.SetOwnPages (Standalone ());
    const hud::Layout out = hud.Lay ({}, At (600.0f, 600.0f));
    EXPECT_FALSE (Drawn (out, shell::kTipGroundRgba)) << "nothing pointed at, no tip";

    const auto top = TopCircle (out.dock);
    const hud::Layout pointed = hud.Lay ({}, At (top.first, top.second));
    float box[4] = {};
    ASSERT_TRUE (Box (pointed.overlay, shell::kTipGroundRgba, box)) << "the tip, on its light ground";
    const float dockLeft = 1200.0f + pointed.dock.offset[0];
    EXPECT_LT (box[2], dockLeft) << "left of the dock, its arrow's point included";
    EXPECT_GT (box[2], dockLeft - 8.0f) << "its arrow at the dock";
    EXPECT_LT (box[1], top.second);
    EXPECT_GT (box[3], top.second) << "level with the circle";
    EXPECT_TRUE (Drawn (pointed, shell::kTipInkRgba)) << "its text, dark";

    // Not at the pointer: the same tip wherever on the circle the pointer is.
    float again[4] = {};
    ASSERT_TRUE (Box (hud.Lay ({}, At (top.first - 3.0f, top.second + 3.0f)).overlay, shell::kTipGroundRgba, again));
    for (int k = 0; k < 4; ++k)
        EXPECT_FLOAT_EQ (again[k], box[k]);
}

// A section's (i) near the view's left edge: no room for the tip on its left, so it is on its
// right -- and in the view either way.
TEST (HudTips, ATipTurnsToTheSideWithRoom)
{
    Fresh hud;
    layers::Panel panel;
    layers::PanelItem section = Item (layers::ItemKind::Section, "Sun");
    section.info = "What the study measures: the direct sun each surface receives over the day";
    panel.items.push_back (section);
    panel.items.push_back (Item (layers::ItemKind::Text, "Below the section"));
    // Along the section's row until the pointer finds the marker.
    float x = 0.0f, y = 0.0f;
    float box[4] = {};
    for (float row = 20.0f; row < 48.0f && x == 0.0f; row += 2.0f)
        for (float across = 16.0f; across < 240.0f; across += 1.0f)
            if (Box (hud.Lay ({ &panel }, At (across, row)).overlay, shell::kTipGroundRgba, box)) {
                x = across;
                y = row;
                break;
            }
    ASSERT_GT (x, 0.0f) << "the marker says its tip";
    EXPECT_GT (box[0], x) << "on the marker's right: its left has no room";
    EXPECT_GE (box[0], 0.0f);
    EXPECT_LE (box[2], 1200.0f) << "in the view";
    EXPECT_LT (box[1], y);
    EXPECT_GT (box[3], y);
}

// ⚠️ THE USER: text in the panel overflowed and was cut off -- it must flow inside the panel.
// Figures stay on one line; descriptive notes still wrap inside the padding.
TEST (HudText, FiguresStayOnOneLineAndNotesFlowInsideAPanelOfASetWidth)
{
    Fresh hud;
    hud::OwnPages pages = Standalone ();
    shell::Card card;
    card.title = "GPU";
    card.figures.push_back (
        { "Adapter", "NVIDIA GeForce RTX 4070 Ti, 12.6 GB, textures up to 16384 on a side", kValueRgba });
    card.note = "max 196 M rays per study (49 steps, patch domain), far longer than the card is wide";
    pages.stats = { card };
    hud.engine.SetOwnPages (pages);
    const hud::Layout out = hud.Lay ({}, At (600.0f, 600.0f));
    const layers::Panel& look = shell::PlainLook ();
    ASSERT_GT (out.host.width, 0.0f);
    EXPECT_LT (out.host.width, 2.0f * look.widthPixels) << "the card keeps its width: the lines do not widen it";
    float box[4] = {};
    ASSERT_TRUE (Box (out.host, kValueRgba, box));
    EXPECT_LE (box[2], out.host.width - look.paddingPixels + 1.0f) << "the value fits inside the padding";
    EXPECT_LT (box[3] - box[1], 1.5f * 13.0f) << "the value stays on one line";
    // The note, in the card's muted colour.
    ASSERT_TRUE (Box (out.host, shell::WithAlpha (look.textRgba, 0.6f), box));
    EXPECT_LE (box[2], out.host.width - look.paddingPixels + 1.0f) << "the note wraps inside the padding";
    EXPECT_GT (box[3] - box[1], 1.5f * 13.0f) << "on more than one line";
}

TEST (HudText, StatsReserveFullNumericValueAndRevealTruncatedNameOnHover)
{
    auto* context = ImGui::CreateContext ();
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = { 1000, 600 };
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    const std::string name = "Allowed residential floor area with a very long function name";
    shell::Card card;
    card.figures.push_back ({ name, "123456789.12 m2", kValueRgba, "coverage.built" });
    const auto& look = shell::PlainLook ();
    ImVec2 labelPoint;
    ImVec2 valuePoint;
    const auto frame = [&] (ImVec2 pointer) {
        io.AddMousePosEvent (pointer.x, pointer.y);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 260, 180 });
        ImGui::Begin ("single-line-stats");
        ImGui::PushTextWrapPos (80); // Reproduce the host's inherited wrapping.
        const auto hovered = shell::Cards ({ card }, look, 1);
        ImGui::PopTextWrapPos ();
        ImGui::End ();
        ImGui::Render ();
        return hovered;
    };
    frame ({ 800, 500 });
    frame ({ 800, 500 });
    const auto* window = ImGui::FindWindowByName ("single-line-stats");
    const auto labelColour = shell::Packed (shell::WithAlpha (look.textRgba, 0.72f));
    size_t valueVertices = 0;
    float low = 1000, high = 0;
    for (const auto& vertex : window->DrawList->VtxBuffer) {
        if (vertex.col == shell::Packed (kValueRgba)) {
            ++valueVertices;
            low = (std::min) (low, vertex.pos.y);
            high = (std::max) (high, vertex.pos.y);
            valuePoint = { vertex.pos.x - 1, vertex.pos.y - 1 };
        }
        if (vertex.col == labelColour)
            labelPoint = { vertex.pos.x - 1, vertex.pos.y - 1 };
    }
    EXPECT_EQ (valueVertices, 14u * 4) << "all digits, decimal point and unit glyphs, not an ellipsis";
    EXPECT_LT (high - low, ImGui::GetTextLineHeight ());
    EXPECT_GT (labelPoint.x, 0);
    EXPECT_EQ (frame (labelPoint), "coverage.built");
    frame (labelPoint);
    bool fullNameTooltip = false;
    for (const auto* candidate : context->Windows)
        if ((candidate->Flags & ImGuiWindowFlags_Tooltip) && candidate->Active)
            fullNameTooltip = candidate->Size.x >= ImGui::CalcTextSize (name.c_str ()).x;
    EXPECT_TRUE (fullNameTooltip);
    EXPECT_EQ (frame (valuePoint), "coverage.built");
    EXPECT_TRUE (frame ({ 800, 500 }).empty ());
    ImGui::DestroyContext (context);
}

TEST (HudText, StorySliceFiguresAlignDecimalDotsAcrossUnitsPrecisionAndSignedValues)
{
    auto* context = ImGui::CreateContext ();
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = { 1000, 600 };
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    shell::Card card;
    card.alignDecimals = true;
    const std::vector<std::string> values { "123456.12 m2", "6.789", "-42.00 m3", "8", "n/a" };
    for (size_t i = 0; i < values.size (); ++i)
        card.figures.push_back (
            { "A long descriptive story-slice figure name", values[i], 0x123456FFu + uint32_t (i) * 0x100 });
    for (float fontScale : { 1.0f, 1.4f })
        for (float panelWidth : { 260.0f, 420.0f }) {
            for (int frame = 0; frame < 2; ++frame) {
                ImGui::NewFrame ();
                ImGui::SetNextWindowPos ({ 0, 0 });
                ImGui::SetNextWindowSize ({ panelWidth, 300 });
                ImGui::Begin ("decimal-stats");
                ImGui::SetWindowFontScale (fontScale);
                shell::Cards ({ card }, shell::PlainLook (), fontScale);
                ImGui::End ();
                ImGui::Render ();
            }
            const auto* window = ImGui::FindWindowByName ("decimal-stats");
            float anchorX = 0;
            for (size_t row = 0; row < 3; ++row) {
                std::vector<ImDrawVert> vertices;
                for (const auto& vertex : window->DrawList->VtxBuffer)
                    if (vertex.col == shell::Packed (card.figures[row].rgba))
                        vertices.push_back (vertex);
                const size_t dot = values[row].find ('.');
                const size_t glyphs =
                    size_t (std::count_if (values[row].begin (), values[row].end (), [] (char c) { return c != ' '; }));
                ASSERT_EQ (vertices.size (), glyphs * 4) << "Values and units are not truncated";
                if (row == 0)
                    anchorX = vertices[dot * 4].pos.x;
                else
                    EXPECT_NEAR (vertices[dot * 4].pos.x, anchorX, 0.02f);
                EXPECT_LT (vertices.back ().pos.y - vertices.front ().pos.y, 13 * fontScale);
            }
        }
    ImGui::DestroyContext (context);
}

namespace {

constexpr uint32_t kFirstRgba = 0x1565C0FFu;
constexpr uint32_t kLastRgba = 0xAD1457FFu;

// Stats far taller than a short view: thirty cards, the first figure and the last in colours
// of their own.
hud::OwnPages LongStats ()
{
    hud::OwnPages pages = Standalone ();
    for (int k = 0; k < 30; ++k) {
        shell::Card card;
        card.title = "Card " + std::to_string (k);
        card.figures.push_back ({ "Figure", "value " + std::to_string (k),
                                  k == 0    ? kFirstRgba
                                  : k == 29 ? kLastRgba
                                            : 0u });
        pages.stats.push_back (card);
    }
    return pages;
}

hud::Input Short (float x, float y)
{
    hud::Input input = At (x, y);
    input.height = 400.0f;
    return input;
}

} // namespace

// ⚠️ THE USER: the Settings page ran off the view and could not be read -- the panel is never
// taller than the view; what its page does not hold is not drawn past it.
TEST (HudScroll, ScrollbarsAreOneThirdOfTheDefaultWidthAtEveryUiScaleWithoutCompounding)
{
    auto* context = ImGui::CreateContext ();
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = { 1000, 1000 };
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    for (float scale : { 0.8f, 1.0f, 1.4f, 2.0f }) {
        ImGuiStyle previous;
        previous.ScaleAllSizes (scale);
        const float expected = previous.ScrollbarSize / 3;
        for (int frame = 0; frame < 2; ++frame) {
            shell::BaseStyle (scale);
            EXPECT_FLOAT_EQ (ImGui::GetStyle ().ScrollbarSize, expected);
            ImGui::NewFrame ();
            ImGui::SetNextWindowPos ({ 0, 0 });
            ImGui::SetNextWindowSize ({ 260, 180 });
            ImGui::Begin ("slim-settings-scrollbar");
            for (int row = 0; row < 100; ++row)
                ImGui::Text ("Setting %d", row);
            ImGui::End ();
            ImGui::Render ();
        }
        const auto* window = ImGui::FindWindowByName ("slim-settings-scrollbar");
        EXPECT_TRUE (window->ScrollbarY);
        EXPECT_FALSE (window->ScrollbarX);
        EXPECT_FLOAT_EQ (window->ScrollbarSizes.x, expected);
        EXPECT_GT (window->ScrollMax.y, 0);
    }
    ImGui::DestroyContext (context);
}

TEST (HudScroll, ThePanelIsNeverTallerThanTheView)
{
    Fresh hud;
    hud.engine.SetOwnPages (LongStats ());
    const hud::Layout out = hud.Lay ({}, Short (100.0f, 380.0f));
    ASSERT_GT (out.host.height, 0.0f);
    const float top = out.host.fraction[1] * 400.0f + out.host.offset[1];
    EXPECT_GE (top, 0.0f);
    EXPECT_LE (top + out.host.height, 400.0f - shell::kViewMargin + 0.5f) << "a margin above the view's bottom";
    EXPECT_GT (out.host.height, 400.0f * 0.75f) << "the panel takes the room it has";
    EXPECT_TRUE (Drawn (out, kFirstRgba) || [&] () {
        float box[4] = {};
        return Box (out.host, kFirstRgba, box);
    }()) << "the page's top is shown";
    float box[4] = {};
    EXPECT_FALSE (Box (out.host, kLastRgba, box)) << "its bottom is past the panel: not drawn over the view";
    // Every triangle of the panel inside it -- its border's antialiased fringe a pixel out.
    for (const hud::Vertex& v : out.host.vertices) {
        EXPECT_GE (v.y, -1.5f);
        EXPECT_LE (v.y, out.host.height + 1.5f);
    }
}

// The wheel over the panel scrolls its page; its tab row stays where it was. The input layer
// takes the wheel there only because the panel says its page scrolls.
TEST (HudScroll, TheWheelScrollsThePageUnderATabRowThatStays)
{
    Fresh hud;
    hud.engine.SetOwnPages (LongStats ());
    const hud::Layout top = hud.Lay ({}, Short (100.0f, 380.0f));
    ASSERT_GT (top.host.height, 0.0f);
    EXPECT_TRUE (top.host.scrolls) << "the input layer is told the wheel is the panel's here";
    const float x = top.host.fraction[0] * 1200.0f + top.host.offset[0] + top.host.width * 0.5f;
    const float y = top.host.fraction[1] * 400.0f + top.host.offset[1] + top.host.height * 0.5f;
    const layers::Panel& look = shell::PlainLook ();
    float before[4] = {}, after[4] = {};
    ASSERT_TRUE (Box (top.host, look.textRgba, before));

    hud::Input wheel = Short (x, y);
    wheel.wheel = -60.0f; // far past the page's end: it stops there
    hud.Lay ({}, wheel);
    const hud::Layout scrolled = hud.Lay ({}, Short (x, y));
    float box[4] = {};
    EXPECT_TRUE (Box (scrolled.host, kLastRgba, box)) << "the page's end is shown";
    EXPECT_FALSE (Box (scrolled.host, kFirstRgba, box)) << "its top scrolled out, not drawn over the tab row";
    EXPECT_FLOAT_EQ (scrolled.host.height, top.host.height) << "the panel itself did not move";
    ASSERT_TRUE (Box (scrolled.host, look.textRgba, after));
    EXPECT_FLOAT_EQ (after[1], before[1]) << "the tab row's titles where they were";
}

// ⚠️ THE USER (2026-10-10): switched from a very long tab to a short one, the panel moved as if
// centred: dragged into the view's lower half, it was held from the bottom. Its top edge stays.
TEST (HudHost, ATabSwitchKeepsThePanelsTopWhereItWas)
{
    for (const float view : { 2400.0f, 1000.0f }) {
        // 2400: room to drag the long page into the view's lower half, where a bottom corner is
        // nearest; 1000: the long page is pushed up by the view's bottom and keeps that top.
        Fresh hud;
        auto state = hud::NewState ();
        hud.engine.UseState (state);
        hud.engine.SetOwnPages (LongStats ());
        const auto lay = [&] (float x, float y, std::vector<hud::Input::Button> buttons = {}) {
            hud::Input step = At (x, y, std::move (buttons));
            step.height = view;
            return hud.Lay ({}, step);
        };
        lay (5.0f, 5.0f);
        hud::SelectKey (*state, shell::kStatsKey);
        lay (5.0f, 5.0f);
        hud::Layout tall = lay (5.0f, 5.0f);
        // Dragged down by its padding.
        const float x = tall.host.fraction[0] * 1200.0f + tall.host.offset[0] + 3.0f;
        const float y = tall.host.fraction[1] * view + tall.host.offset[1] + tall.host.height - 3.0f;
        lay (x, y);
        lay (x, y, { { 0, true } });
        for (int k = 1; k <= 16; ++k)
            lay (x, y + 50.0f * k);
        lay (x, y + 800.0f, { { 0, false } });
        lay (5.0f, 5.0f);
        tall = lay (5.0f, 5.0f);
        const float top = tall.host.fraction[1] * view + tall.host.offset[1];
        ASSERT_GT (tall.host.height, 400.0f);
        if (view > 2000.0f)
            ASSERT_GT (top + tall.host.height * 0.5f, view * 0.5f) << "dragged into the lower half";
        hud::SelectKey (*state, shell::kSettingsKey);
        lay (5.0f, 5.0f);
        const hud::Layout small = lay (5.0f, 5.0f);
        ASSERT_LT (small.host.height, tall.host.height - 50.0f) << "the other page is shorter";
        EXPECT_NEAR (small.host.fraction[1] * view + small.host.offset[1], top, 1.0f) << "view " << view;
    }
}
