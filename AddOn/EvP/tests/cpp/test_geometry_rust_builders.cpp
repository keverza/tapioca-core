#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/Transforms.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace {
namespace cpp = geomsrv::engine;
namespace rust = tapioca::geometry;
using Kind = rust::TransformBuilderKind;
constexpr double kPi = std::numbers::pi;

void MatrixNear (const cpp::Transform& actual, const cpp::Transform& expected, double tolerance)
{
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 4; ++column)
            EXPECT_NEAR (actual.m[row][column], expected.m[row][column],
                         column == 3 ? tolerance : std::min (tolerance, 1e-12))
                << row << ',' << column;
}
bool Oracle (const rust::TransformBuilderRequest& request, cpp::Transform& result, std::string& error)
{
    switch (request.kind) {
        case Kind::Translation:
            result = cpp::Translation (request.vector);
            return true;
        case Kind::Rotation:
            return cpp::Rotation (request.origin, request.vector, request.radians, result, error);
        case Kind::Scaling:
            return cpp::Scaling (request.origin, request.vector, result, error);
        case Kind::Mirroring:
            return cpp::Mirroring (request.origin, request.vector, result, error);
    }
    return false;
}
void Compare (const rust::TransformBuilderRequest& request, double tolerance = 1e-12)
{
    cpp::Transform expected, actual;
    std::string error;
    ASSERT_TRUE (Oracle (request, expected, error)) << error;
    ASSERT_TRUE (rust::BuildTransform (request, actual, error)) << error;
    EXPECT_TRUE (error.empty ());
    MatrixNear (actual, expected, tolerance);
    cpp::Transform repeat;
    ASSERT_TRUE (rust::BuildTransform (request, repeat, error));
    MatrixNear (repeat, actual, 0);
    bool flipped = false;
    std::vector<double> output;
    ASSERT_TRUE (rust::TransformVectors ({ actual, rust::VectorTransformMode::Point, {} }, output, flipped, error));
    EXPECT_EQ (flipped, cpp::FlipsOrientation (expected));
}
} // namespace

TEST (GeometryRustBuilders, CompiledOracleMatchesAllBuildersAndSurveyPivots)
{
    for (const cpp::Vector3 origin : { cpp::Vector3 { 0, 0, 0 }, { 3, 5, 7 }, { 700000, 6000000, 120 } }) {
        const double tolerance = 1e-8; // Absolute metres for pivot translations, never world-relative.
        Compare ({ Kind::Translation, origin, { -10, 2, 3 } }, tolerance);
        Compare ({ Kind::Scaling, origin, { -2, 3, 0.5 } }, tolerance);
        Compare ({ Kind::Mirroring, origin, { 1, -2, 3 } }, tolerance);
        for (double radians : { 0.0, -kPi / 2, 2 * kPi, 1e100 })
            Compare ({ Kind::Rotation, origin, { 1, -2, 3 }, radians }, tolerance);
    }
    Compare ({ Kind::Rotation, {}, { 0, 0, 1e-150 }, kPi / 4 });
    Compare ({ Kind::Scaling, {}, { 1e-200, 1e-200, 1e-200 } }, 0);
    Compare ({ Kind::Scaling, {}, { -1e-200, 1e-200, 1e-200 } }, 0);
}

TEST (GeometryRustBuilders, SeededOriginsAxesFactorsAndAnglesMatchCompiledOracle)
{
    uint32_t state = 0xB17D2026u;
    const auto random = [&] () {
        state = state * 1664525u + 1013904223u;
        return double (state) / 4294967296.0;
    };
    for (int i = 0; i < 64; ++i) {
        const cpp::Vector3 origin { random () * 100, random () * 100, random () * 100 };
        const cpp::Vector3 vector { random () + 0.1, -random () - 0.1, random () + 0.1 };
        const double radians = (random () - 0.5) * 12;
        for (auto kind : { Kind::Translation, Kind::Rotation, Kind::Scaling, Kind::Mirroring })
            Compare ({ kind, origin, vector, radians });
    }
}

TEST (GeometryRustBuilders, RotationAndReflectionPreservePivotAndComposeWithTheirInverse)
{
    const cpp::Vector3 origin { 3, 5, 7 }, axis { 1, -2, 3 };
    std::string error;
    cpp::Transform forward, backward, mirror, identity;
    ASSERT_TRUE (rust::BuildTransform ({ Kind::Rotation, origin, axis, 0.73 }, forward, error));
    ASSERT_TRUE (rust::BuildTransform ({ Kind::Rotation, origin, axis, -0.73 }, backward, error));
    const std::array<cpp::Transform, 2> rotations { forward, backward };
    ASSERT_TRUE (rust::ComposeTransforms (rotations, identity, error));
    MatrixNear (identity, cpp::Identity (), 1e-12);
    ASSERT_TRUE (rust::BuildTransform ({ Kind::Mirroring, origin, axis }, mirror, error));
    const std::array<cpp::Transform, 2> mirrors { mirror, mirror };
    ASSERT_TRUE (rust::ComposeTransforms (mirrors, identity, error));
    MatrixNear (identity, cpp::Identity (), 1e-12);
    const auto p = cpp::ApplyToPoint (forward, origin);
    EXPECT_NEAR (p.x, origin.x, 1e-12);
    EXPECT_NEAR (p.y, origin.y, 1e-12);
    EXPECT_NEAR (p.z, origin.z, 1e-12);
    EXPECT_TRUE (cpp::FlipsOrientation (mirror));
}

TEST (GeometryRustBuilders, DomainAndArithmeticRefusalsPreserveEveryPriorCoefficient)
{
    const double max = std::numeric_limits<double>::max (), nan = std::numeric_limits<double>::quiet_NaN ();
    const cpp::Vector3 zero {}, z { 0, 0, 1 };
    struct Case {
        Kind kind;
        cpp::Vector3 origin, vector;
        double radians = 0;
    };
    const std::array<Case, 12> cases { { { static_cast<Kind> (99), zero, z },
                                         { Kind::Translation, zero, { nan, 0, 0 } },
                                         { Kind::Rotation, zero, zero, 0 },
                                         { Kind::Rotation, zero, { 0, 0, 1e-200 }, 1 },
                                         { Kind::Rotation, zero, { 0, 0, 1e200 }, 1 },
                                         { Kind::Rotation, zero, z, nan },
                                         { Kind::Scaling, zero, { 1, -0.0, 1 } },
                                         { Kind::Scaling, { max, 0, 0 }, { -2, 1, 1 } },
                                         { Kind::Scaling, zero, { 1e200, 1e200, 1 } },
                                         { Kind::Mirroring, zero, zero },
                                         { Kind::Mirroring, { max, 0, 0 }, { 1, 0, 0 } },
                                         { Kind::Rotation, { max, 0, 0 }, z, kPi } } };
    for (const auto& request : cases) {
        cpp::Transform result;
        for (auto& row : result.m)
            for (auto& coefficient : row)
                coefficient = 77;
        std::string error;
        EXPECT_FALSE (
            rust::BuildTransform ({ request.kind, request.origin, request.vector, request.radians }, result, error));
        EXPECT_FALSE (error.empty ());
        for (const auto& row : result.m)
            for (auto coefficient : row)
                EXPECT_EQ (std::bit_cast<uint64_t> (coefficient), std::bit_cast<uint64_t> (77.0));
    }
}

TEST (GeometryRustBuilders, TaggedUnusedFieldsAreNotReinterpretedOrValidated)
{
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    const cpp::Vector3 bad { nan, nan, nan }, by { 1, 2, 3 }, origin {};
    Compare ({ Kind::Translation, bad, by, nan });
    Compare ({ Kind::Scaling, origin, by, nan });
    Compare ({ Kind::Mirroring, origin, by, nan });
}
