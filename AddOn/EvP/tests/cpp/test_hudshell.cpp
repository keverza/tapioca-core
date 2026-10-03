// ArchViz/HudShell: the text in the HUD's panel (the user, 2026-10-03). A panel of a set width
// wraps its text inside its padding instead of cutting it off at its edge.

#include "hud_fixture.hpp"

#include "ArchViz/HudShell.hpp"

#include <gtest/gtest.h>

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
    return pages;
}

} // namespace

// ⚠️ THE USER: text in the panel overflowed and was cut off -- it must flow inside the panel.
// A figure and a note far wider than the plain card wrap inside its padding.
TEST (HudText, TextFlowsInsideAPanelOfASetWidth)
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
    // ⚠️ WRAPPED, NOT CLIPPED: a cell clips what overflows it, so the value's glyphs stopping at
    // the edge prove nothing -- its lines must be more than one.
    ASSERT_TRUE (Box (out.host, kValueRgba, box));
    EXPECT_LE (box[2], out.host.width - look.paddingPixels + 1.0f) << "the value wraps inside the padding";
    EXPECT_GT (box[3] - box[1], 1.5f * 13.0f) << "the value on more than one line";
    // The note, in the card's muted colour.
    ASSERT_TRUE (Box (out.host, shell::WithAlpha (look.textRgba, 0.6f), box));
    EXPECT_LE (box[2], out.host.width - look.paddingPixels + 1.0f) << "the note wraps inside the padding";
    EXPECT_GT (box[3] - box[1], 1.5f * 13.0f) << "on more than one line";
}
