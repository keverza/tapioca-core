// ArchViz/OverlayHudEvents: what the user changed on the HUD, as a numbered sequence
// Python polls. A lost event is a checkbox a script never hears about, and a tail that
// hides its gap is a script acting on half a story -- so both are pinned here.

#include "ArchViz/OverlayHudEvents.hpp"

#include <gtest/gtest.h>

namespace hudevents = geomsrv::archviz::overlayhudevents;

namespace {

hudevents::Event Dock (const char* title)
{
    hudevents::Event event;
    event.view = "3d";
    event.kind = "dock";
    event.title = title;
    event.value = 1.0;
    return event;
}

} // namespace

// Each event is one above the last, stamped with the wall clock; a caller asks for those
// after the last it saw and gets them oldest first, as many as it asked for.
TEST (OverlayHudEvents, ACallerReadsWhatCameAfterTheLastItSaw)
{
    hudevents::Clear ();
    const uint64_t start = hudevents::LastSeq ();
    const uint64_t first = hudevents::Push (Dock ("Area metrics"));
    const uint64_t second = hudevents::Push (Dock ("Sun hours"));
    EXPECT_EQ (first, start + 1);
    EXPECT_EQ (second, first + 1);
    const hudevents::Tail all = hudevents::Since (start, 16);
    ASSERT_EQ (all.events.size (), 2u);
    EXPECT_FALSE (all.gap);
    EXPECT_EQ (all.lastSeq, second);
    EXPECT_EQ (all.events[0].title, "Area metrics");
    EXPECT_GT (all.events[0].timeMs, 1700000000000ull) << "milliseconds since 1970";
    const hudevents::Tail newer = hudevents::Since (first, 16);
    ASSERT_EQ (newer.events.size (), 1u);
    EXPECT_EQ (newer.events[0].seq, second);
    EXPECT_EQ (hudevents::Since (start, 1).events.size (), 1u) << "no more than asked";
    EXPECT_TRUE (hudevents::Since (second, 16).events.empty ()) << "nothing new";
}

// ⚠️ A CALLER THAT FELL BEHIND IS TOLD SO. Past the ring's capacity the oldest events go,
// and one whose last number is older than what is held reads `gap`, not a tail that looks
// whole. A project closing empties the ring and keeps the numbering.
TEST (OverlayHudEvents, ACallerBehindTheRingReadsAGap)
{
    hudevents::Clear ();
    const uint64_t start = hudevents::LastSeq ();
    for (size_t k = 0; k < hudevents::kCapacity + 10; ++k)
        hudevents::Push (Dock ("Area metrics"));
    const hudevents::Tail behind = hudevents::Since (start, 1000);
    EXPECT_TRUE (behind.gap);
    EXPECT_EQ (behind.events.size (), hudevents::kCapacity);
    EXPECT_EQ (behind.events.front ().seq, start + 11);
    const hudevents::Tail current = hudevents::Since (start + 10, 1000);
    EXPECT_FALSE (current.gap) << "the one after it is the oldest held";

    const uint64_t last = hudevents::LastSeq ();
    hudevents::Clear ();
    EXPECT_EQ (hudevents::LastSeq (), last);
    EXPECT_FALSE (hudevents::Since (last, 16).gap) << "a caller that had seen them all";
    EXPECT_TRUE (hudevents::Since (last - 1, 16).gap) << "one behind them";
    EXPECT_EQ (hudevents::Push (Dock ("Sun hours")), last + 1);
}
