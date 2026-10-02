// ArchViz/ModelContentWatch: when a change to what the 3D window shows is acted on. The watch
// reads Archicad's live model every tick of the overlay's runtime; a reading that differs from
// the baseline re-extracts once it has come back the same on two ticks in a row.

#include "ArchViz/ModelContentWatch.hpp"

#include <gtest/gtest.h>

namespace watch = geomsrv::archviz::modelcontentwatch;

namespace {

watch::Reading Of (int32_t count, const char* first = "A", const char* middle = "M", const char* last = "Z")
{
    watch::Reading reading;
    reading.count = count;
    reading.first = first;
    reading.middle = middle;
    reading.last = last;
    return reading;
}

} // namespace

TEST (ModelContentWatch, TheSameContentIsNeverAChange)
{
    watch::Settle settle;
    settle.Reset (Of (3889));
    for (int tick = 0; tick < 10; ++tick)
        EXPECT_FALSE (settle.Observe (Of (3889)));
}

// The 2026-10-02 isolation: 3889 elements shown, then 7.
TEST (ModelContentWatch, AnIsolationIsActedOnOnceItHoldsForTwoTicks)
{
    watch::Settle settle;
    settle.Reset (Of (3889));
    EXPECT_FALSE (settle.Observe (Of (7, "B", "C", "D"))) << "one tick could be a toggle in progress";
    EXPECT_TRUE (settle.Observe (Of (7, "B", "C", "D")));
    EXPECT_EQ (settle.Baseline ().count, 7);
    EXPECT_FALSE (settle.Observe (Of (7, "B", "C", "D"))) << "acted on once, not every tick after";
    // Show all again: the change back is a change too.
    EXPECT_FALSE (settle.Observe (Of (3889)));
    EXPECT_TRUE (settle.Observe (Of (3889)));
}

TEST (ModelContentWatch, AToggleThatComesStraightBackIsNotReExtracted)
{
    watch::Settle settle;
    settle.Reset (Of (3889));
    EXPECT_FALSE (settle.Observe (Of (3500)));
    EXPECT_FALSE (settle.Observe (Of (3889)));
    EXPECT_FALSE (settle.Observe (Of (3889)));
}

TEST (ModelContentWatch, AChangeStillMovingIsActedOnWhereItSettles)
{
    watch::Settle settle;
    settle.Reset (Of (3889));
    EXPECT_FALSE (settle.Observe (Of (3500)));
    EXPECT_FALSE (settle.Observe (Of (3200)));
    EXPECT_TRUE (settle.Observe (Of (3200)));
    EXPECT_EQ (settle.Baseline ().count, 3200);
}

TEST (ModelContentWatch, ALayerSwappedForOneOfTheSameSizeIsAChange)
{
    watch::Settle settle;
    settle.Reset (Of (120, "A", "M", "Z"));
    EXPECT_FALSE (settle.Observe (Of (120, "A", "N", "Z")));
    EXPECT_TRUE (settle.Observe (Of (120, "A", "N", "Z")));
}
