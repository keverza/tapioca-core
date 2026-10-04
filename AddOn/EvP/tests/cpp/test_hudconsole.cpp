// ArchViz/HudConsole: the Debug tab's console (the user, 2026-10-03: not a wordy log -- the very
// important things to check when something is failing). One entry for a thing said again in a
// row; the last few kept; the Debug tab's title counting what the HUD has not shown; each HUD's
// Clear its own.

#include "hud_fixture.hpp"

#include "ArchViz/HudConsole.hpp"
#include "ArchViz/HudShell.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hudtest;

namespace hc = geomsrv::archviz::hudconsole;
namespace shell = geomsrv::archviz::hudshell;

namespace {

// Every test starts from an empty console and no listener.
struct Console : ::testing::Test {
    void SetUp () override
    {
        hc::Clear ();
        hc::SetListener ({});
    }
    void TearDown () override
    {
        hc::Clear ();
        hc::SetListener ({});
    }
};

} // namespace

TEST_F (Console, TheSameThingSaidAgainIsOneEntryWithItsCount)
{
    int woken = 0;
    hc::SetListener ([&] () { ++woken; });
    hc::Error ("Overlay", "the 3D overlay did not start (NoSwapChain): no 3D window");
    hc::Error ("Overlay", "the 3D overlay did not start (NoSwapChain): no 3D window");
    hc::Error ("Overlay", "the 3D overlay did not start (NoSwapChain): no 3D window");
    hc::Warning ("Model", "edits are not followed: the generator failed");
    const std::vector<hc::Entry> entries = hc::Entries ();
    ASSERT_EQ (entries.size (), 2u);
    EXPECT_EQ (entries[0].repeats, 3u);
    EXPECT_EQ (entries[0].level, hc::Level::Error);
    EXPECT_EQ (entries[0].time.size (), 8u) << "local hh:mm:ss";
    EXPECT_GT (entries[1].sequence, entries[0].sequence);
    // ⚠️ A REPEAT WAKES NOBODY: said at every layout, it would lay the HUD out for ever.
    EXPECT_EQ (woken, 2) << "the two new entries only";
}

TEST_F (Console, OnlyTheLastFewAreKept)
{
    for (size_t k = 0; k < hc::kKept + 10; ++k)
        hc::Note ("Viewer", "opened " + std::to_string (k));
    const std::vector<hc::Entry> entries = hc::Entries ();
    ASSERT_EQ (entries.size (), hc::kKept);
    EXPECT_EQ (entries.front ().text, "opened 10") << "the oldest went first";
}

// The Debug tab: its title counts the errors and warnings not shown yet; shown, they are seen;
// its Clear hides what it showed -- what comes next is shown, and counted.
TEST_F (Console, TheDebugTabCountsWhatItHasNotShownAndClearsItsOwn)
{
    hc::Note ("Overlay", "3D on");
    hc::Error ("Metadata", "Usage: the schema refused it");
    hc::Warning ("Viewer", "picking is unavailable");
    Watched hud;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.console = hc::Entries ();
    hud.engine.SetOwnPages (pages);
    EXPECT_EQ (hc::Unseen (pages.console, 0, 0), 2u) << "a note is not counted";

    // Shown: the Debug tab, its entries in the card's words.
    hud::SelectKey (*hud.state, shell::kDebugKey);
    const hud::Layout shown = hud.Lay ({}, At (600.0f, 600.0f));
    ASSERT_GT (shown.host.height, 0.0f);
    float box[4] = {};
    EXPECT_TRUE (Box (shown.host, shell::kErrorRgba, box)) << "the error's mark, in its colour";
    EXPECT_TRUE (Box (shown.host, shell::kBusyRgba, box)) << "the warning's";

    // Seen now: a new warning is the only one counted.
    hc::Warning ("Model", "edits may not be followed");
    pages.console = hc::Entries ();
    hud.engine.SetOwnPages (pages);
    hud::SelectKey (*hud.state, shell::kStatsKey);
    hud.Lay ({}, At (600.0f, 600.0f));
    EXPECT_EQ (hud::SelectedKey (*hud.state), shell::kStatsKey);
    uint64_t seen = 0, cleared = 0;
    for (const hc::Entry& entry : pages.console)
        if (entry.text != "edits may not be followed")
            seen = (std::max) (seen, entry.sequence);
    EXPECT_EQ (hc::Unseen (pages.console, seen, cleared), 1u);

    // Cleared: nothing older is shown on this HUD; the console itself keeps them.
    cleared = pages.console.back ().sequence;
    EXPECT_EQ (hc::Unseen (pages.console, seen, cleared), 0u);
    EXPECT_EQ (hc::Entries ().size (), 4u);
}
