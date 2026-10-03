// Settings' Displays (ArchViz/OverlayHudDisplays.cpp): the add-on's own displays switched and
// styled from the overlay HUD (the user, 2026-10-03: options for the additional information
// displays -- the storey slices on and off, and their style). The engine shows what the owner
// says they are and hands over what the user makes them.

#include "hud_fixture.hpp"

#include "ArchViz/HudShell.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

using namespace hudtest;

namespace shell = geomsrv::archviz::hudshell;

namespace {

hud::OwnPages Own (bool slicesOn)
{
    hud::OwnPages pages;
    pages.standalone = true;
    pages.displays.slicesOn = slicesOn;
    pages.displays.slicesSaid = "on: 2 slab(s) cut by storeys into 7 slice(s), 4120.0 m2";
    return pages;
}

// Where the pointer shows a hand down the page at `x`, under the tab row: its controls, in
// order, as runs of rows.
std::vector<std::pair<float, float>> Presses (Watched& hud, float x, float bottom)
{
    std::vector<std::pair<float, float>> runs;
    bool in = false;
    for (float y = 52.0f; y < bottom; y += 2.0f) {
        const bool hand = hud.Lay ({}, At (x, y)).hand;
        if (hand && !in)
            runs.push_back ({ y, y });
        if (hand)
            runs.back ().second = y;
        in = hand;
    }
    return runs;
}

} // namespace

TEST (OverlayHudDisplays, SettingsSwitchesTheStoreySlicesOnAndShowsTheirStyle)
{
    Watched hud;
    hud.engine.SetOwnPages (Own (false));
    hud::SelectKey (*hud.state, shell::kSettingsKey);
    const hud::Layout off = hud.Lay ({}, At (600.0f, 600.0f));
    ASSERT_GT (off.host.height, 0.0f);
    const float left = off.host.fraction[0] * 1200.0f + off.host.offset[0];
    const float top = off.host.fraction[1] * 800.0f + off.host.offset[1];
    // Down the checkboxes' column: Show overlay, Hover readout, Storey slices, Watch annotations.
    const std::vector<std::pair<float, float>> boxes = Presses (hud, left + 14.0f, top + off.host.height);
    ASSERT_GE (boxes.size (), 4u);
    const float slices = (boxes[boxes.size () - 2].first + boxes[boxes.size () - 2].second) * 0.5f;
    hud.Click ({}, left + 14.0f, slices);

    hud::Displays wanted;
    ASSERT_TRUE (hud::TakeDisplays (*hud.state, wanted)) << "the owner is handed what the user set";
    EXPECT_TRUE (wanted.slicesOn);
    EXPECT_FALSE (wanted.annotationsOn);
    ASSERT_FALSE (hud.heard.empty ());
    EXPECT_EQ (hud.heard.back ().kind, "display");
    EXPECT_EQ (hud.heard.back ().text, "slices on");
    EXPECT_FALSE (hud::TakeDisplays (*hud.state, wanted)) << "taken once";

    // The owner applied it: on, its style below -- source, line, width, hidden part, fill, labels.
    hud.engine.SetOwnPages (Own (true));
    const hud::Layout on = hud.Lay ({}, At (600.0f, 600.0f));
    EXPECT_GT (on.host.height, off.host.height + 5.0f * 13.0f) << "the slices' style is on the page";
}
