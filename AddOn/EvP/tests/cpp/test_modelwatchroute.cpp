// ArchViz/ModelWatch: what a poll's change list is answered with -- a few elements updated by
// GUID, a large list a full pass, and a large list that a full pass started since the previous
// poll already read, nothing.

#include "ArchViz/ModelWatch.hpp"

#include <gtest/gtest.h>

namespace watch = geomsrv::archviz::modelwatch;

// 2026-10-02 11:45: three hidden of 3889 re-extracted the whole model in 4.9 s.
TEST (ModelWatchRoute, AFewChangedElementsAreUpdatedOneByOne)
{
    EXPECT_EQ (watch::RouteChange (3, 3889, 0, 0), watch::Route::Update);
    EXPECT_EQ (watch::RouteChange (watch::kLargeChange, 3889, 0, 0), watch::Route::Update);
}

TEST (ModelWatchRoute, ALargeListOrMostOfASmallModelIsAFullPass)
{
    EXPECT_EQ (watch::RouteChange (watch::kLargeChange + 1, 100000, 0, 0), watch::Route::Full);
    EXPECT_EQ (watch::RouteChange (30, 100, 0, 0), watch::Route::Full) << "a quarter of the model and more";
    EXPECT_EQ (watch::RouteChange (25, 100, 0, 0), watch::Route::Update);
}

// 11:44:12: `3883 new`, the show-all the content watch had re-extracted 27 s earlier.
TEST (ModelWatchRoute, ALargeListAFullPassAlreadyReadIsNotReadAgain)
{
    const uint64_t previousPoll = 1000;
    EXPECT_EQ (watch::RouteChange (3883, 3889, /*full started*/ 2500, previousPoll), watch::Route::AlreadyRead);
    EXPECT_EQ (watch::RouteChange (3883, 3889, /*before the poll*/ 900, previousPoll), watch::Route::Full);
    EXPECT_EQ (watch::RouteChange (3883, 3889, 2500, /*no previous poll*/ 0), watch::Route::Full);
    EXPECT_EQ (watch::RouteChange (3, 3889, 2500, previousPoll), watch::Route::Update)
        << "a few elements are cheap to read again, whatever ran";
}

TEST (ModelWatchRoute, AnUnknownModelSizeReadsEverything)
{
    // No model held yet (ModelContentWatch has not taken one): any change is a full pass.
    EXPECT_EQ (watch::RouteChange (1, 0, 0, 0), watch::Route::Full);
}

// 2026-10-31 12:23:36: with the palette viewer alone the content watch never
// counts (-1), and one new element re-extracted 3,770 in 56.6 s. The published
// snapshot's mesh count stands in for the unknown model size.
TEST (ModelWatchRoute, AnUncountedModelIsSizedByItsPublishedSnapshot)
{
    EXPECT_EQ (watch::RouteModelSize (-1, 2085), 2085u);
    EXPECT_EQ (watch::RouteModelSize (0, 2085), 2085u);
    EXPECT_EQ (watch::RouteModelSize (3770, 2085), 3770u) << "the content watch's count wins when it has one";
    EXPECT_EQ (watch::RouteChange (1, watch::RouteModelSize (-1, 2085), 0, 0), watch::Route::Update);
    EXPECT_EQ (watch::RouteChange (1, watch::RouteModelSize (-1, 0), 0, 0), watch::Route::Full)
        << "nothing published yet: a full pass";
}

// 2026-10-31: a 3,169 ms generator backed the watch off to 30 s, and edits sat
// unseen that long. A project-database event brings the next poll forward --
// bounded so a chatty event can never turn polls into a stall loop.
TEST (ModelWatchEarlyPoll, ABurstIsWaitedOutButNotForever)
{
    const int64_t scheduled = 100000;
    // A lone event: the settle delay, long after the last poll.
    EXPECT_EQ (watch::EarlyPollDue (50000, 50000, 0, scheduled), 50000 + watch::kEarlyPollSettleMs);
    // Events keep coming: still polled by the burst's deadline.
    EXPECT_EQ (watch::EarlyPollDue (51900, 50000, 0, scheduled), 50000 + watch::kEarlyPollMaxDeferMs);
    EXPECT_EQ (watch::EarlyPollDue (60000, 50000, 0, scheduled), 60000) << "never in the past";
}

TEST (ModelWatchEarlyPoll, PollsStaySpacedAndNeverLaterThanScheduled)
{
    // A poll just ended: the next one waits out the spacing.
    EXPECT_EQ (watch::EarlyPollDue (50000, 50000, 49000, 100000), 49000 + watch::kEarlyPollSpacingMs);
    // The regular tick is sooner anyway: leave the timer alone.
    EXPECT_EQ (watch::EarlyPollDue (50000, 50000, 0, 50100), 50100);
    EXPECT_EQ (watch::EarlyPollDue (50000, 50000, 49000, 52000), 52000);
}

// A 3 s diff on a large model must not be bought every 5 s, and a hint that
// keeps finding nothing backs off instead of switching itself off (#97).
TEST (ModelWatchEarlyPoll, SpacingScalesWithTheDiffAndBacksOffOnMisses)
{
    EXPECT_EQ (watch::EarlyPollSpacing (40, 0), watch::kEarlyPollSpacingMs);
    EXPECT_EQ (watch::EarlyPollSpacing (3000, 0), 3000 * watch::kEarlyPollDutyFactor);
    EXPECT_EQ (watch::EarlyPollSpacing (40, 1), 2 * watch::kEarlyPollSpacingMs);
    EXPECT_EQ (watch::EarlyPollSpacing (40, watch::kEarlyPollMaxBackoff),
               watch::kEarlyPollSpacingMs << watch::kEarlyPollMaxBackoff);
    EXPECT_EQ (watch::EarlyPollSpacing (40, 50), watch::EarlyPollSpacing (40, watch::kEarlyPollMaxBackoff)) << "capped";
    EXPECT_EQ (watch::EarlyPollDue (50000, 50000, 49000, 100000, 20000), 69000);
}

// The project stamp moves for selection changes too; a click must not buy a diff.
TEST (ModelWatchStamp, OnlyAMoveThatIsNotTheSelectionOrTheWatchItselfIsAHint)
{
    using watch::JudgeStamp;
    using watch::StampVerdict;
    EXPECT_EQ (JudgeStamp (false, false, true, true), StampVerdict::Unchanged);
    EXPECT_EQ (JudgeStamp (true, true, false, false), StampVerdict::DuringPass);
    EXPECT_EQ (JudgeStamp (true, true, true, true), StampVerdict::DuringPass);
    EXPECT_EQ (JudgeStamp (true, false, true, false), StampVerdict::SelectionOnly);
    EXPECT_EQ (JudgeStamp (true, false, false, false), StampVerdict::Hint) << "an edit of what stays selected";
    EXPECT_EQ (JudgeStamp (true, false, true, true), StampVerdict::Hint) << "a delete of the selection";
}

// The 3 s diff froze Archicad every 30 s with nothing changed (2026-10-31).
TEST (ModelWatchStamp, AQuietStampSkipsTheDiffUntilTheBackstop)
{
    using watch::DiffWanted;
    EXPECT_FALSE (DiffWanted (false, true, false, 30000));
    EXPECT_TRUE (DiffWanted (false, true, true, 30000)) << "moved since the last poll";
    EXPECT_TRUE (DiffWanted (true, true, false, 0)) << "an early poll was asked for";
    EXPECT_TRUE (DiffWanted (false, false, false, 0)) << "no stamp: the cadence is all there is";
    EXPECT_TRUE (DiffWanted (false, true, false, watch::kQuietBackstopMs));
}
