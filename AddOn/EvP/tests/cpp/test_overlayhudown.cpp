// ArchViz/OverlayHud: the HUD's own pages (the user, 2026-10-03). With the overlay running in a
// view the HUD is there -- its dock, its floating panel and its own tabs, Stats, Selection,
// Settings and Debug -- whether or not a caller set a layer; a caller's titled panels sit
// between Selection and Settings, and one that asks for Stats is a card there. Laid out by the
// vendored Dear ImGui, pressed as the input layer hands the presses over.

#include "hud_fixture.hpp"

#include <gtest/gtest.h>

#include <utility>

using namespace hudtest;

namespace shell = geomsrv::archviz::hudshell;

namespace {

layers::Panel Titled (const char* title, uint32_t colour)
{
    layers::Panel panel;
    panel.title = title;
    layers::PanelItem swatch = Item (layers::ItemKind::Swatch, title);
    swatch.rgba = colour;
    panel.items.push_back (swatch);
    return panel;
}

hud::OwnPages Standalone ()
{
    hud::OwnPages pages;
    pages.standalone = true;
    shell::Card card;
    card.title = "Model";
    card.figures.push_back ({ "Elements", "3889" });
    pages.stats.push_back (card);
    shell::Card cost;
    cost.title = "Composition";
    cost.figures.push_back ({ "GPU a Present", "0.7 ms", 0xC0392BFFu });
    pages.debug.push_back (cost);
    return pages;
}

// The host's tab row: the first height down its left part where the pointer shows the hand.
float RowY (Fresh& hud, const std::vector<const layers::Panel*>& panels, const hud::Built& host)
{
    for (float y = 16.0f; y < 16.0f + host.height; y += 1.0f)
        if (hud.Lay (panels, At (16.0f + 24.0f, y)).hand)
            return y + 4.0f;
    return 0.0f;
}

// Where the pointer shows the hand along the tab row, left to right.
std::vector<std::pair<float, float>> Presses (Fresh& hud, const std::vector<const layers::Panel*>& panels,
                                              const hud::Built& host, float y)
{
    std::vector<std::pair<float, float>> runs;
    bool in = false;
    for (float x = 16.0f; x < 16.0f + host.width; x += 2.0f) {
        const bool hand = hud.Lay (panels, At (x, y)).hand;
        if (hand && !in)
            runs.push_back ({ x, x });
        if (hand)
            runs.back ().second = x;
        in = hand;
    }
    return runs;
}

float Middle (const std::pair<float, float>& run)
{
    return (run.first + run.second) * 0.5f;
}

} // namespace

// ⚠️ THE USER: "It should always start when overlay is toggled from main menu." No layer at
// all, and the dock and the floating panel are there, open on Stats.
TEST (OverlayHudOwn, AStandaloneHudIsThereWithoutALayer)
{
    Watched hud;
    hud.engine.SetOwnPages (Standalone ());
    const hud::Layout out = hud.Lay ({}, At (600.0f, 600.0f));
    EXPECT_GT (out.dock.height, 0.0f);
    EXPECT_GT (out.host.height, 0.0f);
    EXPECT_TRUE (hud.engine.Open ());
    EXPECT_EQ (hud.engine.Selected (), shell::kStatsKey);
    EXPECT_EQ (out.hostKey, shell::kStatsKey);
}

// Without own pages the HUD is the callers' panels and Settings, as it was: no layer, nothing.
TEST (OverlayHudOwn, WithoutOwnPagesNoLayerIsNoHud)
{
    Fresh hud;
    const hud::Layout out = hud.Lay ({}, At (600.0f, 600.0f));
    EXPECT_FLOAT_EQ (out.dock.width, 0.0f);
    EXPECT_FLOAT_EQ (out.host.width, 0.0f);
}

// Stats, Selection, the caller's panel, Settings, Debug and the close button, along the row.
// A panel that just arrived is shown -- once: the user's own choice after that is kept when
// the layer is set again, and the next new panel is shown in its turn.
TEST (OverlayHudOwn, TheOwnTabsSurroundTheCallersPanelsAndANewPanelIsShownOnce)
{
    Watched hud;
    hud.engine.SetOwnPages (Standalone ());
    hud.Lay ({}, At (600.0f, 600.0f));
    EXPECT_EQ (hud.engine.Selected (), shell::kStatsKey);
    const layers::Panel sun = Titled ("Sun study", 0x11AA22FFu);
    const hud::Layout arrived = hud.Lay ({ &sun }, At (600.0f, 600.0f));
    EXPECT_EQ (hud.engine.Selected (), "hud#0") << "the panel that just arrived";
    float box[4] = {};
    EXPECT_TRUE (Box (arrived.host, 0x11AA22FFu, box));
    const float y = RowY (hud, { &sun }, arrived.host);
    ASSERT_GT (y, 0.0f);
    const std::vector<std::pair<float, float>> runs = Presses (hud, { &sun }, arrived.host, y);
    ASSERT_EQ (runs.size (), 6u) << "Stats, Selection, the panel, Settings, Debug, close";
    hud.Click ({ &sun }, Middle (runs[0]), y);
    EXPECT_EQ (hud.engine.Selected (), shell::kStatsKey);
    ASSERT_FALSE (hud.heard.empty ());
    EXPECT_EQ (hud.heard.back ().kind, "panel");
    EXPECT_EQ (hud.heard.back ().text, "Stats");
    EXPECT_TRUE (hud.heard.back ().key.empty ()) << "an own page names no layer's panel";
    const layers::Panel sunAgain = Titled ("Sun study", 0x11AA22FFu);
    hud.Lay ({ &sunAgain }, At (600.0f, 600.0f));
    EXPECT_EQ (hud.engine.Selected (), shell::kStatsKey) << "set again, it does not take the tab back";
    const layers::Panel area = Titled ("Area", 0x3355CCFFu);
    hud.Lay ({ &sunAgain, &area }, At (600.0f, 600.0f));
    EXPECT_EQ (hud.engine.Selected (), "hud#1") << "the next new panel is shown in its turn";
}

// A panel that asks for Stats is a card there: no tab and no window of its own, its items on
// the Stats page under the owner's cards. Without own pages it is what it was -- a tab.
TEST (OverlayHudOwn, APanelThatAsksForStatsIsACardThere)
{
    Watched hud;
    hud.engine.SetOwnPages (Standalone ());
    layers::Panel massing = Titled ("Massing", 0x8E44ADFFu);
    massing.tab = "stats";
    const hud::Layout out = hud.Lay ({ &massing }, At (600.0f, 600.0f));
    EXPECT_TRUE (out.panels[0].vertices.empty ());
    EXPECT_EQ (hud.engine.Selected (), shell::kStatsKey) << "a card arrives on Stats, not as a tab";
    float box[4] = {};
    EXPECT_TRUE (Box (out.host, 0x8E44ADFFu, box)) << "its items on the Stats page";
    const float y = RowY (hud, { &massing }, out.host);
    EXPECT_EQ (Presses (hud, { &massing }, out.host, y).size (), 5u) << "the own tabs and close only";

    Fresh before;
    const hud::Layout legacy = before.Lay ({ &massing }, At (600.0f, 600.0f));
    EXPECT_EQ (legacy.hostKey, "hud#0") << "without own pages, a tab";
}

// What the owner said is what the pages show: Debug's figures in their own colours, and the
// selection's rows.
TEST (OverlayHudOwn, ThePagesShowWhatTheOwnerSaid)
{
    Watched hud;
    hud::OwnPages pages = Standalone ();
    hud.engine.SetOwnPages (pages);
    const hud::Layout stats = hud.Lay ({}, At (600.0f, 600.0f));
    const float y = RowY (hud, {}, stats.host);
    std::vector<std::pair<float, float>> runs = Presses (hud, {}, stats.host, y);
    ASSERT_EQ (runs.size (), 5u) << "Stats, Selection, Settings, Debug, close";
    hud.Click ({}, Middle (runs[3]), y);
    EXPECT_EQ (hud.engine.Selected (), shell::kDebugKey);
    float box[4] = {};
    EXPECT_TRUE (Box (hud.Lay ({}, At (600.0f, 600.0f)).host, 0xC0392BFFu, box)) << "the figure's own colour";

    hud.Click ({}, Middle (runs[1]), y);
    EXPECT_EQ (hud.engine.Selected (), shell::kSelectionKey);
    const float empty = hud.Lay ({}, At (600.0f, 600.0f)).host.height;
    pages.selection.known = true;
    pages.selection.count = 5;
    pages.selection.elements.push_back ({ "A", "Slab", "S-01", "Massing", "Ground" });
    pages.selection.elements.push_back ({ "B", "Slab", "S-02", "Massing", "Ground" });
    hud.engine.SetOwnPages (pages);
    const float listed = hud.Lay ({}, At (600.0f, 600.0f)).host.height;
    EXPECT_GT (listed, empty) << "a row per element, and a line for the rest";
}
