#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/Curves.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <numbers>

namespace {
namespace rust = tapioca::geometry;
namespace cpp = geomsrv::engine;
constexpr double kPi = std::numbers::pi;

void Compare (const cpp::Vector3& centre, const cpp::Vector3& normal, double radius, double start, double sweep,
              int segments, double tolerance = 1e-9)
{
    std::vector<cpp::Vector3> expected;
    std::vector<double> actual { 99 };
    std::string error;
    ASSERT_TRUE (cpp::MakeArc (centre, normal, radius, start, sweep, segments, expected, error)) << error;
    const rust::ArcRequest request { centre, normal, radius, start, sweep, segments };
    ASSERT_TRUE (rust::MakeArc (request, actual, error)) << error;
    EXPECT_TRUE (error.empty ());
    ASSERT_EQ (actual.size (), expected.size () * 3);
    for (size_t i = 0; i < expected.size (); ++i) {
        EXPECT_NEAR (actual[i * 3], expected[i].x, tolerance) << i;
        EXPECT_NEAR (actual[i * 3 + 1], expected[i].y, tolerance) << i;
        EXPECT_NEAR (actual[i * 3 + 2], expected[i].z, tolerance) << i;
    }
    std::vector<double> repeated;
    ASSERT_TRUE (rust::MakeArc (request, repeated, error));
    EXPECT_EQ (actual, repeated);
}
} // namespace

TEST (GeometryRustArc, HorizontalObliqueAndSeedThresholdFramesMatchCompiledOracle)
{
    for (const auto& normal : { cpp::Vector3 { 0, 0, 1 },
                                { 0, 0, -1 },
                                { 1, 0, 0 },
                                { 0, 1, 0 },
                                { 1, 2, 3 },
                                { std::sqrt (0.19), 0, 0.9 },
                                { std::sqrt (0.19), 0, std::nextafter (0.9, 0.0) } })
        for (double sweep : { kPi / 2, -kPi, 2 * kPi, -4 * kPi })
            Compare ({ 3, 5, 7 }, normal, 12, 0.37, sweep, 37);
}

TEST (GeometryRustArc, SegmentBoundsSurveyCoordinatesAndFiniteExtremeAngles)
{
    Compare ({ 700000, 6000000, 120 }, { 1, -2, 3 }, 10, 0.2, -kPi, 4096);
    Compare ({ 0, 0, 0 }, { 0, 0, 1e-150 }, 2, 0, kPi, 2);
    Compare ({ 0, 0, 0 }, { 0, 0, 1 }, 1e-200, 1e100, 1e99, 2, 1e-210);
}

TEST (GeometryRustArc, SeededCasesPreserveRadiusPlaneAndWinding)
{
    uint32_t state = 0xA2C2026;
    const auto random = [&] () {
        state = state * 1664525u + 1013904223u;
        return double (state) / 4294967296.0;
    };
    for (int i = 0; i < 64; ++i) {
        const cpp::Vector3 centre { random () * 100, random () * 100, random () * 100 };
        const cpp::Vector3 normal { random () - 0.5, random () - 0.5, random () - 0.5 };
        const double radius = 0.1 + random () * 50, start = random () * 6, sweep = 0.1 + random () * 3;
        Compare (centre, normal, radius, start, sweep, 23);
        std::vector<double> points;
        std::string error;
        ASSERT_TRUE (rust::MakeArc ({ centre, normal, radius, start, sweep, 23 }, points, error));
        for (size_t j = 0; j < points.size (); j += 3) {
            const double x = points[j] - centre.x, y = points[j + 1] - centre.y, z = points[j + 2] - centre.z;
            EXPECT_NEAR (std::sqrt (x * x + y * y + z * z), radius, 1e-10);
            EXPECT_NEAR (x * normal.x + y * normal.y + z * normal.z, 0, 1e-10);
        }
        const cpp::Vector3 a { points[0] - centre.x, points[1] - centre.y, points[2] - centre.z };
        const cpp::Vector3 b { points[3] - centre.x, points[4] - centre.y, points[5] - centre.z };
        EXPECT_GT (cpp::Dot (cpp::Cross (a, b), normal), 0);
    }
}

TEST (GeometryRustArc, RefusalsPreservePriorResultAndInputs)
{
    const cpp::Vector3 centre { 0, 0, 0 }, normal { 0, 0, 1 };
    for (int segments : { -1, 0, 1, 4097, std::numeric_limits<int>::max () }) {
        std::vector<double> result { 77 };
        std::string error;
        EXPECT_FALSE (rust::MakeArc ({ centre, normal, 2, 0, kPi, segments }, result, error));
        EXPECT_EQ (result, (std::vector<double> { 77 }));
        EXPECT_FALSE (error.empty ());
    }
    for (int kind = 0; kind < 9; ++kind) {
        auto c = centre, n = normal;
        double radius = 2, start = 0, sweep = kPi;
        switch (kind) {
            case 0:
                radius = -1;
                break;
            case 1:
                sweep = 0;
                break;
            case 2:
                c.x = std::numeric_limits<double>::quiet_NaN ();
                break;
            case 3:
                n.z = std::numeric_limits<double>::infinity ();
                break;
            case 4:
                n.z = 0;
                break;
            case 5:
                n.z = 1e-200;
                break;
            case 6:
                n.z = 1e200;
                break;
            case 7:
                start = sweep = std::numeric_limits<double>::max ();
                break;
            default:
                c.y = radius = std::numeric_limits<double>::max ();
                break;
        }
        std::vector<double> result { 77 };
        std::string error;
        EXPECT_FALSE (rust::MakeArc ({ c, n, radius, start, sweep, 2 }, result, error)) << kind;
        EXPECT_EQ (result, (std::vector<double> { 77 }));
        EXPECT_FALSE (error.empty ());
    }
}
