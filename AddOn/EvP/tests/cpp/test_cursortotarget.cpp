// Tests for CursorToTarget (ArchViz/InputRingBuffer.hpp) -- the cursor mapped
// into render-target pixels.
//
// ⚠️ A SCALE ERROR IS INVISIBLE AT THE TOP-LEFT. The cursor under Windows
// display scaling can arrive in LOGICAL pixels over a PHYSICAL swap chain; the
// error is zero at (0, 0) and largest at the far edges, which is exactly where
// a test that only tried the origin would pass.

#include "ArchViz/InputRingBuffer.hpp"
#include "gtest/gtest.h"

using namespace geomsrv::archviz;

TEST (CursorToTarget, AtOneHundredPercentItIsTheIdentity)
{
    EXPECT_EQ (CursorToTarget (0, 1200, 1200), 0);
    EXPECT_EQ (CursorToTarget (1199, 1200, 1200), 1199);
}

TEST (CursorToTarget, AtOneHundredFiftyPercentTheFarEdgeLandsOnTheFarEdge)
{
    // A 1200-logical-pixel client over an 1800-pixel swap chain.
    EXPECT_EQ (CursorToTarget (0, 1200, 1800), 0);
    EXPECT_EQ (CursorToTarget (600, 1200, 1800), 900) << "the centre";
    EXPECT_EQ (CursorToTarget (1199, 1200, 1800), 1798) << "the far edge, not 1199";
}

TEST (CursorToTarget, AtOneHundredTwentyFivePercentTruncationStaysInside)
{
    for (int32_t x = 0; x < 1000; ++x) {
        const int32_t mapped = CursorToTarget (x, 1000, 1250);
        ASSERT_GE (mapped, 0);
        ASSERT_LT (mapped, 1250) << "cursor " << x << " left the target";
    }
}

TEST (CursorToTarget, AnUnknownClientSizeLeavesTheCursorAlone)
{
    // No measurement is not a reason to invent one.
    EXPECT_EQ (CursorToTarget (345, 0, 1800), 345);
    EXPECT_EQ (CursorToTarget (345, 1200, 0), 345);
}

TEST (CursorToTarget, TheSnapshotHelpersUseTheirOwnAxis)
{
    InputSnapshot input;
    input.x = 600;
    input.y = 300;
    input.clientWidth = 1200;
    input.clientHeight = 600;
    EXPECT_EQ (CursorTargetX (input, 1800), 900);
    EXPECT_EQ (CursorTargetY (input, 900), 450);
}
