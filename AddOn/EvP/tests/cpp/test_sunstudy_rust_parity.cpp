// Differential tests: the Rust sun-study core against the C++ implementation it
// replaces. The C++ side is the oracle; every case runs both on the same input.

#include "SunStudy/SunStudyRust.hpp"

#include <gtest/gtest.h>

namespace {

TEST (SunStudyRustParity, AbiVersionHandshake)
{
    uint32_t version = 0;
    std::string error = "stale";
    ASSERT_TRUE (evp::sunstudy::rust::ReadAbiVersion (version, error)) << error;
    EXPECT_EQ (version, 1u);
    EXPECT_TRUE (error.empty ());
}

} // namespace
