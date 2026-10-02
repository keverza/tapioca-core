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
