#include "Geometry/GeometryRustTerrain.hpp"
#include "Edit/EditSnap.hpp"

#include <gtest/gtest.h>
#include <bit>
#include <cmath>
#include <limits>

namespace {
namespace rust = tapioca::geometry;
namespace ed = geomsrv::edit;
ed::Vec3 Vector (const std::array<double, 3>& value)
{
    return { value[0], value[1], value[2] };
}
void Exact (const std::array<double, 3>& actual, const std::array<double, 3>& expected)
{
    for (size_t i = 0; i < 3; ++i)
        EXPECT_EQ (std::bit_cast<uint64_t> (actual[i]), std::bit_cast<uint64_t> (expected[i]));
}
void Near (const std::array<double, 3>& actual, ed::Vec3 expected)
{
    EXPECT_NEAR (actual[0], expected.x, 1e-12);
    EXPECT_NEAR (actual[1], expected.y, 1e-12);
    EXPECT_NEAR (actual[2], expected.z, 1e-12);
}
void Compare (const rust::PlaneValue& plane, const std::vector<double>& points)
{
    const ed::Plane oracle { Vector (plane.origin), Vector (plane.normal), Vector (plane.u) };
    ed::Vec3 u, v;
    ed::PlaneAxes (oracle, u, v);
    rust::PlaneProjection actual;
    std::string error;
    ASSERT_TRUE (rust::ProjectToPlane (plane, points, actual, error)) << error;
    EXPECT_TRUE (error.empty ());
    Exact (actual.plane.origin, plane.origin);
    Exact (actual.plane.normal, plane.normal);
    Exact (actual.plane.u, plane.u);
    Near (actual.u, u);
    Near (actual.v, v);
    ASSERT_EQ (actual.points.size (), points.size ());
    for (size_t i = 0; i < points.size (); i += 3) {
        const auto expected = ed::ProjectToPlane (oracle, { points[i], points[i + 1], points[i + 2] });
        EXPECT_NEAR (actual.points[i], expected.x, 1e-9);
        EXPECT_NEAR (actual.points[i + 1], expected.y, 1e-9);
        EXPECT_NEAR (actual.points[i + 2], expected.z, 1e-9);
    }
    rust::PlaneProjection repeated;
    ASSERT_TRUE (rust::ProjectToPlane (plane, points, repeated, error));
    EXPECT_EQ (actual.u, repeated.u);
    EXPECT_EQ (actual.v, repeated.v);
    EXPECT_EQ (actual.points, repeated.points);
}
} // namespace

TEST (GeometryRustPlane, LegacyFramesAuthoredTangentsSeedThresholdAndProjectionMatchCompiledOracle)
{
    for (const std::array<double, 3> normal : { std::array<double, 3> { 0, 0, 1 },
                                                { 0, 0, -7 },
                                                { 1, 0, 0 },
                                                { 1, 2, 3 },
                                                { 0.9, std::sqrt (0.19), 0 },
                                                { std::nextafter (0.9, 0.0), std::sqrt (0.19), 0 } }) {
        for (const std::array<double, 3> tangent : { std::array<double, 3> {}, { 3, 4, 5 }, normal })
            Compare ({ { 3, 5, 7 }, normal, tangent }, { 4, 6, -10, 10, -20, 30 });
    }
}

TEST (GeometryRustPlane, FiniteShortNormalAndTangentFallbacksPreserveExactThreshold)
{
    for (double length : { 0.0, 1e-200, 0.5e-12, 1e-12, std::nextafter (1e-12, 0.0) }) {
        Compare ({ {}, { length, 0, 0 }, {} }, { 1, 2, 3 });
        Compare ({ {}, { 0, 0, 1 }, { 0, length, 0 } }, { 1, 2, 3 });
    }
}

TEST (GeometryRustPlane, SeededSurveyPlanesHaveOrthogonalFramesAndIdempotentProjections)
{
    uint32_t state = 0x91A2026u;
    const auto random = [&] () {
        state = state * 1664525u + 1013904223u;
        return double (state) / 4294967296.0;
    };
    for (int i = 0; i < 64; ++i) {
        const rust::PlaneValue plane { { 700000 + random () * 100, 6000000 + random () * 100, 120 + random () },
                                       { 0.1 + random (), -0.1 - random (), 0.1 + random () },
                                       i % 2 == 0 ? std::array<double, 3> {}
                                                  : std::array<double, 3> { random (), random (), random () } };
        std::vector<double> points;
        for (int j = 0; j < 4; ++j)
            points.insert (points.end (),
                           { 700000 + random () * 100, 6000000 + random () * 100, 120 + random () * 100 });
        Compare (plane, points);
        rust::PlaneProjection result, twice;
        std::string error;
        ASSERT_TRUE (rust::ProjectToPlane (plane, points, result, error));
        ASSERT_TRUE (rust::ProjectToPlane (plane, result.points, twice, error));
        EXPECT_NEAR (ed::Dot (Vector (result.u), Vector (result.v)), 0, 1e-12);
        EXPECT_GT (ed::Dot (ed::Cross (Vector (result.u), Vector (result.v)), Vector (plane.normal)), 0);
        for (size_t j = 0; j < points.size (); ++j)
            EXPECT_NEAR (result.points[j], twice.points[j], 2e-9);
    }
}

TEST (GeometryRustPlane, FrameOnlyRoundtripAndOwnedFacadeAliasingPreserveRawValues)
{
    const rust::PlaneValue plane { { -0.0, 6000000, 5 }, { -0.0, 0, 7 }, { 3, 4, -0.0 } };
    Compare (plane, {});
    rust::PlaneProjection result;
    std::string error;
    ASSERT_TRUE (rust::ProjectToPlane (plane, std::array<double, 6> { 1, 2, 9, 3, 4, -7 }, result, error));
    const auto saved = result;
    ASSERT_TRUE (rust::ProjectToPlane (result.plane, result.points, result, error));
    Exact (result.plane.origin, saved.plane.origin);
    Exact (result.plane.normal, saved.plane.normal);
    Exact (result.plane.u, saved.plane.u);
    EXPECT_EQ (result.points, saved.points);
}

TEST (GeometryRustPlane, RefusalsLeaveEveryOwnedOutputUnchanged)
{
    for (int kind = 0; kind < 6; ++kind) {
        rust::PlaneValue plane;
        std::vector<double> points { 1, 2, 3 };
        switch (kind) {
            case 0:
                points.push_back (4);
                break;
            case 1:
                plane.u[0] = std::numeric_limits<double>::quiet_NaN ();
                break;
            case 2:
                plane.normal[2] = 1e200;
                break;
            case 3:
                plane.u[0] = 1e200;
                break;
            case 4:
                plane.origin[0] = -std::numeric_limits<double>::max ();
                points[0] = std::numeric_limits<double>::max ();
                break;
            default:
                plane.normal = { 1, 1, 1 };
                points.insert (points.end (), 3, std::numeric_limits<double>::max ());
                break;
        }
        rust::PlaneProjection result {
            { { 77, 77, 77 }, { 88, 88, 88 }, { 99, 99, 99 } }, { 111, 111, 111 }, { 222, 222, 222 }, { 333 }
        };
        const auto before = result;
        std::string error;
        EXPECT_FALSE (rust::ProjectToPlane (plane, points, result, error)) << kind;
        EXPECT_FALSE (error.empty ());
        Exact (result.plane.origin, before.plane.origin);
        Exact (result.plane.normal, before.plane.normal);
        Exact (result.plane.u, before.plane.u);
        Exact (result.u, before.u);
        Exact (result.v, before.v);
        EXPECT_EQ (result.points, before.points);
    }
}
