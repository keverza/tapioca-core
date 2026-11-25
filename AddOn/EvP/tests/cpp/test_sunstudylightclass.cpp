// Area-weighted sunlight classes (#278 Stage 1B): the C++ wrapper over the
// Rust `light_class` kernel, exact boundaries and refusals.

#include "SunStudy/SunStudyLightClass.hpp"

#include <gtest/gtest.h>

using namespace evp::sunstudy;

TEST (SunStudyLightClass, ExactEdgesBelongToTheUpperClassAndAreasAreWeighted)
{
    // 2.999 h is shade even though the display band would draw it partial.
    const std::vector<double> hours { 2.999, 3.0, 5.999, 6.0, 0.0 };
    const std::vector<double> areas { 1.0, 2.0, 4.0, 8.0, 16.0 };
    LightClassAreas classes;
    std::string error;
    ASSERT_TRUE (ClassifyLightAreas (hours, areas, { 3.0, 6.0 }, classes, error)) << error;
    ASSERT_EQ (classes.areas.size (), 3u);
    EXPECT_DOUBLE_EQ (classes.areas[0], 17.0);
    EXPECT_DOUBLE_EQ (classes.areas[1], 6.0);
    EXPECT_DOUBLE_EQ (classes.areas[2], 8.0);
    EXPECT_EQ (classes.counts, (std::vector<size_t> { 2, 2, 1 }));
    EXPECT_DOUBLE_EQ (classes.evaluatedArea, 31.0);
    EXPECT_EQ (classes.unclassified, 0u);
}

TEST (SunStudyLightClass, BadEdgesAreRefusedWithNothingPartial)
{
    LightClassAreas classes;
    classes.evaluatedArea = 9.0;
    std::string error;
    EXPECT_FALSE (ClassifyLightAreas ({ 1.0 }, { 1.0 }, { 6.0, 3.0 }, classes, error));
    EXPECT_NE (error.find ("ClassAreasEdges"), std::string::npos) << error;
    EXPECT_TRUE (classes.areas.empty ());
    EXPECT_EQ (classes.evaluatedArea, 0.0);
    EXPECT_FALSE (ClassifyLightAreas ({ 1.0, 2.0 }, { 1.0 }, { 3.0 }, classes, error));
    EXPECT_NE (error.find ("ClassAreasShape"), std::string::npos) << error;
}
