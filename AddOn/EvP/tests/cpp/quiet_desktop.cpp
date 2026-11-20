// The whole run reads a quiet desktop (quiet_desktop.hpp): installed before the first test, the
// real one put back after the last.
#include "quiet_desktop.hpp"
#include <gtest/gtest.h>

namespace {

class QuietDesktopEnvironment : public ::testing::Environment {
  public:
    void SetUp () override
    {
        geomsrv::archviz::overlayinput::desktop = hudtest::QuietDesktop ();
    }
    void TearDown () override
    {
        geomsrv::archviz::overlayinput::desktop = {};
    }
};

// Registered at static initialisation, before gtest_main runs the tests.
[[maybe_unused]] ::testing::Environment* const kQuietDesktop =
    ::testing::AddGlobalTestEnvironment (new QuietDesktopEnvironment);

} // namespace
