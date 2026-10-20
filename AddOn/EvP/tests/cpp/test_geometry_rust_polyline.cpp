#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"
#include "Geometry/Curves.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
namespace oracle = geomsrv::engine;
using namespace tapioca::geometry;

void Near (double actual, double expected)
{
    const double tolerance = 32 * std::numeric_limits<double>::epsilon () * std::max (1.0, std::abs (expected));
    EXPECT_NEAR (actual, expected, tolerance);
}

void Compare (const std::vector<oracle::Vector3>& points, const std::vector<double>& parameters)
{
    std::vector<double> flat;
    for (const auto& point : points)
        flat.insert (flat.end (), { point.x, point.y, point.z });
    const auto before = flat;
    PolylineSamples result;
    std::string error;
    ASSERT_TRUE (SamplePolyline (flat, parameters, result, error)) << error;
    EXPECT_TRUE (error.empty ());
    EXPECT_EQ (flat, before);
    std::vector<double> cumulative;
    Near (result.totalLength, oracle::PolylineLength (points, cumulative));
    ASSERT_EQ (result.cumulative.size (), cumulative.size ());
    for (size_t i = 0; i < cumulative.size (); ++i)
        Near (result.cumulative[i], cumulative[i]);
    ASSERT_EQ (result.points.size (), parameters.size () * 3);
    ASSERT_EQ (result.tangents.size (), result.points.size ());
    for (size_t i = 0; i < parameters.size (); ++i) {
        oracle::Vector3 position, tangent;
        ASSERT_TRUE (oracle::PointOnPolyline (points, parameters[i], position, tangent, error));
        Near (result.points[3 * i], position.x);
        Near (result.points[3 * i + 1], position.y);
        Near (result.points[3 * i + 2], position.z);
        Near (result.tangents[3 * i], tangent.x);
        Near (result.tangents[3 * i + 1], tangent.y);
        Near (result.tangents[3 * i + 2], tangent.z);
    }
    PolylineSamples repeated;
    ASSERT_TRUE (SamplePolyline (flat, parameters, repeated, error)) << error;
    EXPECT_EQ (result.cumulative, repeated.cumulative);
    EXPECT_EQ (result.points, repeated.points);
    EXPECT_EQ (result.tangents, repeated.tangents);
}
} // namespace

TEST (GeometryRustPolyline, Uneven3dLengthsClampingAndJoinTangentsMatchCpp)
{
    Compare ({ { 0, 0, 0 }, { 3, 4, 0 }, { 3, 4, 15 } }, { -10, 0, 0.25, 0.5, 1, 10 });
    Compare ({ { 0, 0, 0 }, { 0, 0, 0 }, { 0, 2, 0 }, { 0, 2, 0 } }, { 0, 0.5, 1 });
    Compare ({ { 2, 3, 4 }, { 2, 3, 4 }, { 2, 3, 4 } }, { -1, 0, 0.5, 1, 2 });
}

TEST (GeometryRustPolyline, SurveyClosedCrossingTinyAndMeasurementOnlyMatchCpp)
{
    Compare ({ { 1e9, -2e8, 5 }, { 1e9 + 3, -2e8 + 4, 5 } }, { 0, 0.125, 0.5, 1 });
    Compare ({ { 0, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 0 } }, { 0, 0.3, 1 });
    Compare ({ { 0, 0, 0 }, { 1e-200, 0, 0 } }, { 0, 1 });
    // C++ accumulation loses the tiny final segment; t=1 selects its incoming
    // predecessor, not an independently forced last endpoint. Preserve parity.
    Compare ({ { 0, 0, 0 }, { 1e16, 0, 0 }, { 1e16, 1, 0 } }, { 0.5, 1 });
    Compare ({ { 0, 0, 0 }, { 3, 4, 0 } }, {});
}

TEST (GeometryRustPolyline, TessellatedArcAndDivisionParametersMatchCpp)
{
    std::vector<oracle::Vector3> points;
    std::string error;
    ASSERT_TRUE (oracle::MakeArc ({ 10, 20, 30 }, { 1, 2, 3 }, 4, -0.3, 5.1, 48, points, error));
    Compare (points, { 0, 0.1, 0.25, 0.5, 0.75, 0.9, 1 });
    for (bool includeEnds : { false, true }) {
        std::vector<double> parameters;
        for (int i = includeEnds ? 0 : 1; i <= (includeEnds ? 7 : 6); ++i)
            parameters.push_back (static_cast<double> (i) / 7);
        Compare (points, parameters);
        std::vector<oracle::Vector3> divided;
        ASSERT_TRUE (oracle::DividePolyline (points, 7, includeEnds, divided, error));
        ASSERT_EQ (divided.size (), parameters.size ());
        std::vector<double> flat;
        for (const auto& point : points)
            flat.insert (flat.end (), { point.x, point.y, point.z });
        PolylineSamples result;
        ASSERT_TRUE (SamplePolyline (flat, parameters, result, error)) << error;
        for (size_t i = 0; i < divided.size (); ++i) {
            Near (result.points[3 * i], divided[i].x);
            Near (result.points[3 * i + 1], divided[i].y);
            Near (result.points[3 * i + 2], divided[i].z);
        }
    }
}

TEST (GeometryRustPolyline, SeededSpatialCurvesAndParameterOrderMatchCpp)
{
    uint32_t state = 0x3194A11U;
    auto next = [&] () {
        state = state * 1664525U + 1013904223U;
        return static_cast<double> (state % 2001) / 10 - 100;
    };
    for (int trial = 0; trial < 64; ++trial) {
        std::vector<oracle::Vector3> points;
        for (int i = 0; i < 32; ++i)
            points.push_back ({ next (), next (), next () });
        Compare (points, { 1, 0.125, -1, 0.875, 0.5, 0, 2, 0.5 });
    }
}

TEST (GeometryRustPolyline, RefusalsPreserveAllPriorOutputs)
{
    for (const auto& points : { std::vector<double> {},
                                { 0, 0, 0 },
                                { 0, 0, 0, 1, 2 },
                                { 0, 0, 0, 1e200, 0, 0 },
                                { 0, 0, 0, std::numeric_limits<double>::infinity (), 0, 0 } }) {
        PolylineSamples result { { 77 }, 88, { 99 }, { 111 } };
        std::string error;
        EXPECT_FALSE (SamplePolyline (points, {}, result, error));
        EXPECT_FALSE (error.empty ());
        EXPECT_EQ (result.cumulative, (std::vector<double> { 77 }));
        EXPECT_EQ (result.totalLength, 88);
        EXPECT_EQ (result.points, (std::vector<double> { 99 }));
        EXPECT_EQ (result.tangents, (std::vector<double> { 111 }));
    }
}

TEST (GeometryRustPolyline, OwnedFacadeMayBorrowPriorResultArrays)
{
    PolylineSamples result { { 0, 0.5, 1 }, 0, { 0, 0, 0, 0, 0, 8 }, {} };
    std::string error;
    ASSERT_TRUE (SamplePolyline (result.points, result.cumulative, result, error)) << error;
    EXPECT_EQ (result.cumulative, (std::vector<double> { 0, 8 }));
    EXPECT_EQ (result.points, (std::vector<double> { 0, 0, 0, 0, 0, 4, 0, 0, 8 }));
}

TEST (GeometryRustPolyline, NonFiniteParameterRefusesWithoutPartialPublication)
{
    const std::vector<double> points { 0, 0, 0, 0, 0, 8 };
    const std::vector<double> parameters { 0, std::numeric_limits<double>::quiet_NaN () };
    PolylineSamples result { { 77 }, 88, { 99 }, { 111 } };
    std::string error;
    EXPECT_FALSE (SamplePolyline (points, parameters, result, error));
    EXPECT_NE (error.find ("PolylineNonFinite:"), std::string::npos);
    EXPECT_EQ (result.cumulative, (std::vector<double> { 77 }));
    EXPECT_EQ (result.totalLength, 88);
    EXPECT_EQ (result.points, (std::vector<double> { 99 }));
    EXPECT_EQ (result.tangents, (std::vector<double> { 111 }));
}
