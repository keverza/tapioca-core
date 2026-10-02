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

// ---- the content sweep's diff: what an update must read again ----------------------------------

namespace {

watch::Signatures Walk (std::initializer_list<std::pair<const char*, watch::Signature>> elements)
{
    watch::Signatures walk;
    for (const auto& element : elements)
        walk[element.first] = element.second;
    return walk;
}

} // namespace

TEST (ModelContentWatch, AWalkThatReadsTheSameIsNoChange)
{
    const watch::Signatures walk = Walk ({ { "A", { 1, 100 } }, { "B", { 2, 200 } } });
    EXPECT_FALSE (watch::Compare (walk, walk).Any ());
}

// 12:30:41: three elements hidden, the model's count unchanged -- only a walk sees which.
TEST (ModelContentWatch, AHiddenElementVanishesOrEmptiesAndIsReadAgain)
{
    const watch::Signatures before = Walk ({ { "A", { 1, 100 } }, { "B", { 2, 200 } }, { "C", { 3, 300 } } });
    const watch::Signatures after = Walk ({ { "A", { 1, 100 } }, { "C", { 3, 0 } }, { "D", { 4, 40 } } });
    const watch::SweepDiff diff = watch::Compare (before, after);
    EXPECT_EQ (diff.vanished, (std::vector<std::string> { "B" }));
    EXPECT_EQ (diff.appeared, (std::vector<std::string> { "D" }));
    EXPECT_EQ (diff.reshaped, (std::vector<std::string> { "C" }));
    EXPECT_EQ (diff.emptied, 1u);
    EXPECT_EQ (diff.Changed (), (std::set<std::string> { "B", "C", "D" }));
}

// 12:31:43: an edit moved the element's change stamp and its vertices.
TEST (ModelContentWatch, AnEditedElementIsReadAgainByItsStampOrItsVertices)
{
    const watch::Signatures before = Walk ({ { "A", { 1, 100 } }, { "B", { 2, 200 } } });
    const watch::Signatures after = Walk ({ { "A", { 7, 100 } }, { "B", { 2, 260 } } });
    const watch::SweepDiff diff = watch::Compare (before, after);
    EXPECT_EQ (diff.stamped, (std::vector<std::string> { "A" }));
    EXPECT_EQ (diff.reshaped, (std::vector<std::string> { "B" }));
    EXPECT_EQ (diff.stampedAndReshaped, 0u);
    EXPECT_EQ (diff.Changed (), (std::set<std::string> { "A", "B" }));
}

// 12:31:43: the null GUID's stamp moved beside the real edit; it names nothing to read.
TEST (ModelContentWatch, TheNullGuidIsNeverAChange)
{
    const watch::Signatures before = Walk ({ { watch::NullGuid (), { 1, 0 } }, { "A", { 1, 100 } } });
    const watch::Signatures after = Walk ({ { watch::NullGuid (), { 9, 5 } }, { "A", { 1, 100 } } });
    EXPECT_FALSE (watch::Compare (before, after).Any ());
    EXPECT_FALSE (watch::Compare (before, Walk ({ { "A", { 1, 100 } } })).Any ()) << "nor its vanishing";
}
