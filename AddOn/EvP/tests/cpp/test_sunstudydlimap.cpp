// The DLI map's values (#278 Stage 5, #155): one month or the mean of the
// months a sample has weather for, a missing value counted and drawn as 0,
// never mistaken for zero light in the ramp's top.

#include "SunStudy/SunStudyDliMap.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

using namespace evp::sunstudy;

namespace {

using Months = std::array<double, 12>;

Months Filled (double value)
{
    Months m;
    m.fill (value);
    return m;
}

} // namespace

TEST (SunStudyDliMap, AMonthIsReadAsItIs)
{
    Months a = Filled (10.0), b = Filled (2.0);
    a[5] = 41.0; // June
    b[5] = 3.5;
    DliMap map;
    std::string error;
    ASSERT_TRUE (BuildDliMap ({ a, b }, 6, map, error)) << error;
    EXPECT_EQ (map.values, (std::vector<double> { 41.0, 3.5 }));
    EXPECT_DOUBLE_EQ (map.largest, 41.0);
    EXPECT_DOUBLE_EQ (map.top, 45.0);
    EXPECT_EQ (map.missing, 0u);
}

TEST (SunStudyDliMap, TheMeanSkipsMonthsWithoutWeather)
{
    const double nan = std::nan ("");
    Months a = Filled (nan);
    a[0] = 4.0;
    a[6] = 20.0; // two months with weather: the mean is 12, not 2
    DliMap map;
    std::string error;
    ASSERT_TRUE (BuildDliMap ({ a }, 0, map, error)) << error;
    ASSERT_EQ (map.values.size (), 1u);
    EXPECT_DOUBLE_EQ (map.values[0], 12.0);
    EXPECT_DOUBLE_EQ (map.top, 15.0);
}

TEST (SunStudyDliMap, AMissingSampleIsCountedAndDrawnAsZero)
{
    const double nan = std::nan ("");
    DliMap map;
    std::string error;
    ASSERT_TRUE (BuildDliMap ({ Filled (8.0), Filled (nan), Filled (12.0) }, 3, map, error)) << error;
    EXPECT_EQ (map.values, (std::vector<double> { 8.0, 0.0, 12.0 }));
    EXPECT_EQ (map.missing, 1u);
    EXPECT_DOUBLE_EQ (map.largest, 12.0); // the missing sample does not move the ramp
}

TEST (SunStudyDliMap, NothingToDrawIsRefused)
{
    DliMap map;
    std::string error;
    EXPECT_FALSE (BuildDliMap ({ Filled (std::nan ("")) }, 4, map, error));
    EXPECT_FALSE (error.empty ());
    EXPECT_TRUE (map.values.empty ());
    EXPECT_FALSE (BuildDliMap ({}, 0, map, error));
    EXPECT_FALSE (BuildDliMap ({ Filled (1.0) }, 13, map, error));
    EXPECT_FALSE (BuildDliMap ({ Filled (1.0) }, -1, map, error));
}

TEST (SunStudyDliMap, TheRampTopIsTheNextStepOfFive)
{
    EXPECT_DOUBLE_EQ (DliRampTop (0.0), 5.0);
    EXPECT_DOUBLE_EQ (DliRampTop (0.3), 5.0);
    EXPECT_DOUBLE_EQ (DliRampTop (5.0), 5.0);
    EXPECT_DOUBLE_EQ (DliRampTop (5.01), 10.0);
    EXPECT_DOUBLE_EQ (DliRampTop (40.0), 40.0);
    EXPECT_DOUBLE_EQ (DliRampTop (std::nan ("")), 5.0);
}
