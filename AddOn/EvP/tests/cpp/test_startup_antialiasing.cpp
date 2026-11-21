#include "ArchViz/StartupAntiAliasing.hpp"

#include <gtest/gtest.h>

using geomsrv::archviz::StartupAntiAliasing;

namespace {

constexpr double kQuiet = StartupAntiAliasing::kQuietSeconds;

} // namespace

// #14: off while the model streams in, on once it has been quiet for kQuietSeconds, exactly once.
TEST (StartupAntiAliasing, TurnsOnOnceTheModelHasArrivedAndStayedQuiet)
{
    StartupAntiAliasing s;
    EXPECT_FALSE (s.Step (false, true, 0, 0, 0.0));    // nothing yet
    EXPECT_FALSE (s.Step (false, true, 100, 40, 1.0)); // streaming
    EXPECT_FALSE (s.Step (false, true, 300, 0, 2.0));  // queue empty, count still moving
    EXPECT_FALSE (s.Step (false, true, 300, 0, 2.5));  // quiet, not long enough
    EXPECT_FALSE (s.Step (false, true, 300, 0, 2.5 + kQuiet - 0.1));
    EXPECT_TRUE (s.Step (false, true, 300, 0, 2.5 + kQuiet));
    EXPECT_FALSE (s.armed);
    EXPECT_FALSE (s.Step (false, true, 300, 0, 100.0)); // one-shot: the user may turn it off again
}

TEST (StartupAntiAliasing, ABatchDuringTheQuietWaitStartsItAgain)
{
    StartupAntiAliasing s;
    EXPECT_FALSE (s.Step (false, true, 300, 0, 0.0));
    EXPECT_FALSE (s.Step (false, true, 300, 0, 1.0));
    EXPECT_FALSE (s.Step (false, true, 320, 0, 2.0)); // another batch landed
    EXPECT_FALSE (s.Step (false, true, 320, 0, 3.0));
    EXPECT_FALSE (s.Step (false, true, 320, 0, 3.0 + kQuiet - 0.25));
    EXPECT_TRUE (s.Step (false, true, 320, 0, 3.0 + kQuiet));
}

TEST (StartupAntiAliasing, ThePreviewOrTheCheckboxFirstDisarmsIt)
{
    StartupAntiAliasing chosen;
    EXPECT_FALSE (chosen.Step (true, true, 0, 5, 0.0)); // a preview turned TAA on while loading
    EXPECT_FALSE (chosen.armed);

    StartupAntiAliasing wireframe;
    EXPECT_FALSE (wireframe.Step (false, false, 300, 0, 0.0)); // not Shaded: leave it alone
    EXPECT_FALSE (wireframe.Step (false, true, 300, 0, 10.0));
}
