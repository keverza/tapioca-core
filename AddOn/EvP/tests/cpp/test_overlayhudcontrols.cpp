// ArchViz/OverlayHud, stage 2: what the user presses on the HUD -- the tint and the hand
// over it, the dock the panels close to, the font size, and the controls whose values the
// add-on holds and reports to Python. Laid out by the vendored Dear ImGui, as the add-on
// does, and pressed as the input layer hands the presses over.

#include "hud_fixture.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace hudtest;

namespace {

uint32_t WithAlpha (uint32_t rgba, float factor)
{
    const uint32_t a = uint32_t (std::lround (float (rgba & 0xFFu) * factor));
    return (rgba & 0xFFFFFF00u) | (std::min) (a, 255u);
}

} // namespace

// ⚠️ THE USER, 2026-09-29: a hand over what the HUD can press, and a tint on it. A
// section's row is pressable: pointed at, it is tinted with the panel's accent and the
// layout asks for the hand; over the panel's still parts, and off it, an arrow.
TEST (OverlayHudControls, APressableRowIsTintedAndShowsTheHand)
{
    Fresh hud;
    layers::Panel panel;
    panel.items.push_back (Item (layers::ItemKind::Section, "Spacing metrics"));
    layers::PanelItem gap = Item (layers::ItemKind::Spacing);
    gap.heightPixels = 40.0f;
    panel.items.push_back (gap);
    const uint32_t tint = WithAlpha (panel.accentRgba, 0.16f);
    // The panel at (16, 16), its padding in: the section's row, then the gap under it.
    const float left = 16.0f + panel.paddingPixels, top = 16.0f + panel.paddingPixels;
    const hud::Layout away = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_FALSE (away.hand);
    EXPECT_FALSE (Drawn (away, tint));
    const hud::Layout row = hud.Lay ({ &panel }, At (left + 30.0f, top + 6.0f));
    EXPECT_TRUE (row.hand);
    EXPECT_TRUE (Drawn (row, tint)) << "the row tinted with the accent";
    const hud::Layout still = hud.Lay ({ &panel }, At (left + 30.0f, top + 45.0f));
    EXPECT_FALSE (still.hand) << "the gap under it is the HUD's, but nothing to press";
    EXPECT_FALSE (Drawn (still, tint));
    // The light card's accent is its own.
    layers::Panel light;
    layers::ApplyTheme (light, layers::PanelTheme::Light);
    EXPECT_EQ (light.accentRgba, 0x2F6FEBFFu);
}

// The title bar's arrow is pressable too: the hand over it.
TEST (OverlayHudControls, TheTitleBarsArrowShowsTheHand)
{
    Fresh hud;
    layers::Panel panel;
    panel.title = "Area metrics";
    panel.items.push_back (Item (layers::ItemKind::Row, "Site area", "11 214 m\xC2\xB2"));
    // The arrow sits at the title bar's start, a frame's padding in.
    const hud::Layout arrow = hud.Lay ({ &panel }, At (16.0f + 4.0f + 8.0f, 16.0f + 3.0f + 8.0f));
    EXPECT_TRUE (arrow.hand);
    const hud::Layout title = hud.Lay ({ &panel }, At (16.0f + 80.0f, 16.0f + 3.0f + 8.0f));
    EXPECT_FALSE (title.hand) << "the title itself moves nothing: an arrow";
}
