// The Planting tab's area table (#278): per-host class areas from the raw hours,
// with unavailable area beside them and never counted as shade.

#include "SunStudy/SunStudyPlantingAreas.hpp"

#include <gtest/gtest.h>

using namespace evp::sunstudy;

TEST (SunStudyPlantingAreas, ClassesAreExactAndPerHost)
{
    const std::vector<double> hours { 1.0, 3.0, 6.0, 2.0, 7.5 };
    const std::vector<double> areas { 2.0, 1.0, 4.0, 0.5, 3.0 };
    const std::vector<uint32_t> groups { 4, 4, 4, 9, 9 };
    PlantingAreaTable table;
    std::string error;
    ASSERT_TRUE (BuildPlantingAreaTable (hours, areas, groups, {}, { 3.0, 6.0 }, table, error)) << error;
    ASSERT_EQ (table.hosts.size (), 2u);
    EXPECT_EQ (table.hosts[0].group, 4u);
    EXPECT_EQ (table.hosts[0].classAreas, (std::vector<double> { 2.0, 1.0, 4.0 })) << "3.0 h and 6.0 h go up";
    EXPECT_EQ (table.hosts[1].classAreas, (std::vector<double> { 0.5, 0.0, 3.0 }));
    EXPECT_EQ (table.total.classAreas, (std::vector<double> { 2.5, 1.0, 7.0 }));
    EXPECT_DOUBLE_EQ (table.total.evaluatedArea, 10.5);
    EXPECT_EQ (table.total.samples, 5u);
}

TEST (SunStudyPlantingAreas, UnavailableAreaStaysOutOfTheClasses)
{
    const std::vector<double> hours { 1.0, 7.0 };
    const std::vector<double> areas { 1.0, 1.0 };
    const std::vector<uint32_t> groups { 2, 2 };
    // Host 5 is wholly under a balcony: no samples, a row all the same.
    const std::vector<ReceptorHostLoss> losses { { 2, 0.5, 0.0, 0.0 }, { 5, 0.0, 6.0, 0.0 } };
    PlantingAreaTable table;
    std::string error;
    ASSERT_TRUE (BuildPlantingAreaTable (hours, areas, groups, losses, { 3.0, 6.0 }, table, error)) << error;
    ASSERT_EQ (table.hosts.size (), 2u);
    EXPECT_DOUBLE_EQ (table.hosts[0].steepArea, 0.5);
    EXPECT_EQ (table.hosts[1].group, 5u);
    EXPECT_DOUBLE_EQ (table.hosts[1].evaluatedArea, 0.0);
    EXPECT_DOUBLE_EQ (table.hosts[1].blockedArea, 6.0);
    EXPECT_DOUBLE_EQ (table.total.UnavailableArea (), 6.5);
    EXPECT_DOUBLE_EQ (table.total.classAreas[0], 1.0) << "unavailable is never shade";
    EXPECT_DOUBLE_EQ (table.total.evaluatedArea, 2.0);
}

TEST (SunStudyPlantingAreas, MismatchedArraysAndBadEdgesAreRefused)
{
    PlantingAreaTable table;
    std::string error;
    EXPECT_FALSE (BuildPlantingAreaTable ({ 1.0 }, { 1.0, 2.0 }, { 0 }, {}, { 3.0, 6.0 }, table, error));
    EXPECT_FALSE (BuildPlantingAreaTable ({ 1.0 }, { 1.0 }, { 0 }, {}, { 6.0, 3.0 }, table, error));
    EXPECT_TRUE (table.hosts.empty ());
}
