// What the extraction pass does with a slice whose gate Invoke timed out
// (extractionslice::JudgeTimedOutSlice, #97).

#include "ArchViz/ExtractionSlice.hpp"

#include <gtest/gtest.h>

using geomsrv::archviz::extractionslice::JudgeTimedOutSlice;
using geomsrv::archviz::extractionslice::kUnstartedGraceMs;
using geomsrv::archviz::extractionslice::TimedOutSlice;

namespace {
constexpr int64_t kDeadline = 12000;
}

TEST (ExtractionSliceWait, ASliceThatFinishedLateIsHarvested)
{
    EXPECT_EQ (JudgeTimedOutSlice (true, true, 0, kDeadline), TimedOutSlice::Harvest);
    EXPECT_EQ (JudgeTimedOutSlice (true, true, kDeadline + 1, kDeadline), TimedOutSlice::Harvest);
}

TEST (ExtractionSliceWait, ARunningSliceIsWaitedForUntilTheDeadline)
{
    EXPECT_EQ (JudgeTimedOutSlice (true, false, kUnstartedGraceMs, kDeadline), TimedOutSlice::Wait);
    EXPECT_EQ (JudgeTimedOutSlice (true, false, kDeadline - 1, kDeadline), TimedOutSlice::Wait);
    EXPECT_EQ (JudgeTimedOutSlice (true, false, kDeadline, kDeadline), TimedOutSlice::Abandon);
}

// The 2026-10-31 load: a revoked slice was waited on for the full 12 s.
TEST (ExtractionSliceWait, ARevokedSliceIsRetriedAfterTheGraceNotTheDeadline)
{
    EXPECT_EQ (JudgeTimedOutSlice (false, false, 0, kDeadline), TimedOutSlice::Wait);
    EXPECT_EQ (JudgeTimedOutSlice (false, false, kUnstartedGraceMs - 1, kDeadline), TimedOutSlice::Wait);
    EXPECT_EQ (JudgeTimedOutSlice (false, false, kUnstartedGraceMs, kDeadline), TimedOutSlice::Retry);
    EXPECT_LT (kUnstartedGraceMs, kDeadline / 10);
}
