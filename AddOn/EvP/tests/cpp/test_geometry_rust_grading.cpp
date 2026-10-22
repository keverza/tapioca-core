#include "Geometry/GeometryRustTerrain.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <vector>

namespace {
using namespace tapioca::geometry;

// Smoothness rows x[i] - x[i+1] = 0 over a chain of `count` unknowns.
struct Chain {
    std::vector<std::array<GradingTerm, 2>> pairs;
    std::vector<GradingRow> rows;
    explicit Chain (uint32_t count)
    {
        pairs.reserve (count);
        for (uint32_t i = 0; i + 1 < count; ++i)
            pairs.push_back ({ GradingTerm { i, 1.0 }, GradingTerm { i + 1, -1.0 } });
        for (const auto& pair : pairs)
            rows.push_back ({ pair, 0.0, 1.0 });
    }
};

TEST (GeometryRustGrading, FixedEndsGiveTheLinearTransition)
{
    Chain chain (9);
    const std::array<GradingFixed, 3> fixed { GradingFixed { 0, 0.0 }, GradingFixed { 4, 2.0 },
                                              GradingFixed { 8, 0.0 } };
    GradingRequest request;
    request.unknowns = 9;
    request.rows = chain.rows;
    request.fixed = fixed;
    GradingResult result;
    std::string error;
    ASSERT_TRUE (SolveGrading (request, result, error)) << error;
    ASSERT_EQ (result.values.size (), 9U);
    for (uint32_t i = 0; i < 9; ++i)
        EXPECT_NEAR (result.values[i], 2.0 * (1.0 - std::abs (double (i) - 4.0) / 4.0), 1e-12) << i;
    EXPECT_EQ (result.factorizations, 1U);
    EXPECT_TRUE (error.empty ());
}

TEST (GeometryRustGrading, BoundsSelectTheConstrainedSolve)
{
    Chain chain (5);
    const std::array<GradingFixed, 2> fixed { GradingFixed { 0, 0.0 }, GradingFixed { 4, 4.0 } };
    const std::array<GradingBound, 1> bounds { GradingBound { 2, -10.0, 1.5 } };
    GradingRequest request;
    request.unknowns = 5;
    request.rows = chain.rows;
    request.fixed = fixed;
    request.bounds = bounds;
    GradingResult result;
    std::string error;
    ASSERT_TRUE (SolveGrading (request, result, error)) << error;
    EXPECT_NEAR (result.values[2], 1.5, 1e-10);
    EXPECT_NEAR (result.values[1], 0.75, 1e-10);
    EXPECT_NEAR (result.values[3], 2.75, 1e-10);
    EXPECT_GE (result.active, 1U);
}

TEST (GeometryRustGrading, ContradictionsAndBudgetsRefuseWithoutTouchingTheResult)
{
    Chain chain (3);
    const std::array<GradingFixed, 2> contradictory { GradingFixed { 0, 0.0 }, GradingFixed { 0, 1.0 } };
    GradingRequest request;
    request.unknowns = 3;
    request.rows = chain.rows;
    request.fixed = contradictory;
    GradingResult result;
    result.values = { -5.0 };
    std::string error;
    EXPECT_FALSE (SolveGrading (request, result, error));
    EXPECT_EQ (error.rfind ("SolveInconsistent", 0), 0U) << error;
    EXPECT_EQ (result.values, std::vector<double> { -5.0 });
    // An unanchored chain is underdetermined.
    request.fixed = {};
    EXPECT_FALSE (SolveGrading (request, result, error));
    EXPECT_EQ (error.rfind ("SolveUnderdetermined", 0), 0U) << error;
    // Infeasible limits.
    const std::array<GradingBound, 2> bounds { GradingBound { 0, -1.0, 0.0 }, GradingBound { 1, 5.0, 6.0 } };
    const std::array<GradingDifference, 1> differences { GradingDifference { 1, 0, -1.0, 1.0 } };
    const std::array<GradingFixed, 1> anchor { GradingFixed { 2, 0.0 } };
    request.fixed = anchor;
    request.bounds = bounds;
    request.differences = differences;
    EXPECT_FALSE (SolveGrading (request, result, error));
    EXPECT_EQ (error.rfind ("SolveInconsistent", 0), 0U) << error;
    EXPECT_EQ (result.values, std::vector<double> { -5.0 });
}
} // namespace
