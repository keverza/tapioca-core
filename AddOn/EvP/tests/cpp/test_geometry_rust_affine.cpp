#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"
#include "Geometry/Transforms.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {
using namespace tapioca::geometry;
namespace oracle = geomsrv::engine;

void Compare (const oracle::Transform& transform, VectorTransformMode mode, const std::vector<double>& input)
{
    const auto before = input;
    std::vector<double> output { 77 };
    bool flipped = !oracle::FlipsOrientation (transform);
    std::string error;
    ASSERT_TRUE (TransformVectors ({ transform, mode, input }, output, flipped, error)) << error;
    ASSERT_EQ (output.size (), input.size ());
    EXPECT_EQ (flipped, oracle::FlipsOrientation (transform));
    EXPECT_EQ (input, before);
    for (size_t offset = 0; offset < input.size (); offset += 3) {
        const oracle::Vector3 point { input[offset], input[offset + 1], input[offset + 2] };
        oracle::Vector3 expected;
        if (mode == VectorTransformMode::Point)
            expected = oracle::ApplyToPoint (transform, point);
        else if (mode == VectorTransformMode::Direction)
            expected = oracle::ApplyToDirection (transform, point);
        else
            expected = oracle::ApplyToNormal (transform, point);
        const std::array<double, 3> values { expected.x, expected.y, expected.z };
        for (size_t axis = 0; axis < 3; ++axis) {
            // Same f64 arithmetic ordering; permit 16 rounding units at the
            // result scale for compiler/platform expression differences.
            const double tolerance =
                16 * std::numeric_limits<double>::epsilon () * std::max (1.0, std::abs (values[axis]));
            EXPECT_NEAR (output[offset + axis], values[axis], tolerance);
        }
    }
}
} // namespace

TEST (GeometryRustAffine, IdentityTranslationAndGeoreferencedPointsMatchCpp)
{
    const std::vector<double> input { 1, 2, 3, -4, 5, -6, 1000000000.125, -200000000.25, 8000.5 };
    for (auto mode : { VectorTransformMode::Point, VectorTransformMode::Direction, VectorTransformMode::Normal }) {
        Compare (oracle::Identity (), mode, input);
        Compare (oracle::Translation ({ 10, -20, 30 }), mode, input);
    }
}

TEST (GeometryRustAffine, RotationScaleMirrorCompositionAndShearMatchCpp)
{
    oracle::Transform rotation, scale, mirror;
    std::string error;
    ASSERT_TRUE (oracle::Rotation ({ 4, -3, 2 }, { 1, 2, 3 }, 0.75, rotation, error));
    ASSERT_TRUE (oracle::Scaling ({ 2, 3, -1 }, { 2, 0.5, 3 }, scale, error));
    ASSERT_TRUE (oracle::Mirroring ({ 7, 1, 2 }, { 1, -2, 3 }, mirror, error));
    oracle::Transform shear;
    shear.m[0][1] = 0.75;
    shear.m[1][2] = -0.5;
    shear.m[2][0] = 0.25;
    const std::vector<double> input { 1, 2, 3, -4, 5, -6, 0, 0, 0 };
    for (const auto& transform :
         { rotation, scale, mirror, shear, oracle::Compose (mirror, oracle::Compose (rotation, scale)) })
        for (auto mode : { VectorTransformMode::Point, VectorTransformMode::Direction, VectorTransformMode::Normal })
            Compare (transform, mode, input);
}

TEST (GeometryRustAffine, SingularNormalAndEmptyBatchPreserveLegacySemantics)
{
    oracle::Transform singular;
    singular.m[2][2] = 0;
    Compare (singular, VectorTransformMode::Normal, { 1, 2, 3 });
    Compare (singular, VectorTransformMode::Point, {});
    oracle::Transform mirrored;
    mirrored.m[0][0] = -1;
    Compare (mirrored, VectorTransformMode::Point, {});
}

TEST (GeometryRustAffine, ValidationAndDerivedOverflowRefuseWithoutOutputCommit)
{
    for (int kind = 0; kind < 5; ++kind) {
        auto transform = oracle::Identity ();
        std::vector<double> input { 1, 2, 3 };
        auto mode = VectorTransformMode::Point;
        if (kind == 0)
            input.pop_back ();
        else if (kind == 1)
            mode = static_cast<VectorTransformMode> (99);
        else if (kind == 2)
            transform.m[1][2] = std::numeric_limits<double>::quiet_NaN ();
        else if (kind == 3)
            input.back () = std::numeric_limits<double>::infinity ();
        else {
            transform.m[0][3] = std::numeric_limits<double>::max ();
            input.front () = std::numeric_limits<double>::max ();
        }
        std::vector<double> output { 77, 88 };
        bool flipped = true;
        std::string error;
        EXPECT_FALSE (TransformVectors ({ transform, mode, input }, output, flipped, error));
        EXPECT_EQ (output, (std::vector<double> { 77, 88 }));
        EXPECT_TRUE (flipped);
        EXPECT_FALSE (error.empty ());
    }
}

TEST (GeometryRustAffine, SeededValidatedMatricesMatchCppAndRepeatDeterministically)
{
    uint32_t state = 0x74A9103U;
    auto next = [&] () {
        state = state * 1664525U + 1013904223U;
        return static_cast<double> (state % 2001) / 1000.0 - 1.0;
    };
    for (int trial = 0; trial < 64; ++trial) {
        auto transform = oracle::Identity ();
        for (size_t row = 0; row < 3; ++row)
            for (size_t column = 0; column < 4; ++column)
                transform.m[row][column] += next () * 0.125;
        std::vector<double> input;
        for (int point = 0; point < 8; ++point)
            input.insert (input.end (), { next () * 1000, next () * 1000, next () * 1000 });
        for (auto mode : { VectorTransformMode::Point, VectorTransformMode::Direction, VectorTransformMode::Normal }) {
            Compare (transform, mode, input);
            std::vector<double> a, b;
            bool flipA = false, flipB = false;
            std::string error;
            ASSERT_TRUE (TransformVectors ({ transform, mode, input }, a, flipA, error));
            ASSERT_TRUE (TransformVectors ({ transform, mode, input }, b, flipB, error));
            EXPECT_EQ (a, b);
            EXPECT_EQ (flipA, flipB);
        }
    }
}

TEST (GeometryRustAffine, MatrixAndScalarBufferLayoutsMatch)
{
    static_assert (sizeof (TapiocaAffineTransform) == 96);
    static_assert (sizeof (TapiocaAffineRequest) == 120);
    static_assert (sizeof (TapiocaAffineOutput) == 24);
    static_assert (offsetof (TapiocaAffineRequest, values) == 104);
}

TEST (GeometryRustAffine, CompositionOrderEmptyAndAliasedFacadeMatchCpp)
{
    const auto translation = oracle::Translation ({ 10, -20, 30 });
    oracle::Transform scale;
    std::string error;
    ASSERT_TRUE (oracle::Scaling ({ 4, 5, 6 }, { 2, 3, -4 }, scale, error));
    for (const auto& chain : { std::vector<oracle::Transform> {}, { translation, scale }, { scale, translation } }) {
        auto expected = oracle::Identity ();
        for (const auto& transform : chain)
            expected = oracle::Compose (transform, expected);
        oracle::Transform actual;
        ASSERT_TRUE (tapioca::geometry::ComposeTransforms (chain, actual, error)) << error;
        EXPECT_TRUE (error.empty ());
        for (size_t row = 0; row < 3; ++row)
            for (size_t column = 0; column < 4; ++column)
                EXPECT_DOUBLE_EQ (actual.m[row][column], expected.m[row][column]);
    }
    std::vector<oracle::Transform> aliased { translation, scale };
    const auto expected = oracle::Compose (scale, translation);
    ASSERT_TRUE (tapioca::geometry::ComposeTransforms (aliased, aliased.front (), error)) << error;
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 4; ++column)
            EXPECT_DOUBLE_EQ (aliased.front ().m[row][column], expected.m[row][column]);
}

TEST (GeometryRustAffine, SeededCompositionChainsMatchCppAndDoNotChangeInputs)
{
    uint32_t state = 0x32A9B711U;
    auto next = [&] () {
        state = state * 1664525U + 1013904223U;
        return static_cast<double> (state % 2001) / 1000.0 - 1.0;
    };
    for (int trial = 0; trial < 64; ++trial) {
        std::vector<oracle::Transform> chain (8);
        for (auto& transform : chain)
            for (size_t row = 0; row < 3; ++row)
                for (size_t column = 0; column < 4; ++column)
                    transform.m[row][column] += next () * 0.125;
        const auto before = chain;
        auto expected = oracle::Identity ();
        for (const auto& transform : chain)
            expected = oracle::Compose (transform, expected);
        oracle::Transform actual, repeated;
        std::string error;
        ASSERT_TRUE (tapioca::geometry::ComposeTransforms (chain, actual, error)) << error;
        ASSERT_TRUE (tapioca::geometry::ComposeTransforms (chain, repeated, error)) << error;
        for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 4; ++column) {
                const double tolerance =
                    16 * std::numeric_limits<double>::epsilon () * std::max (1.0, std::abs (expected.m[row][column]));
                EXPECT_NEAR (actual.m[row][column], expected.m[row][column], tolerance);
                EXPECT_DOUBLE_EQ (actual.m[row][column], repeated.m[row][column]);
                for (size_t i = 0; i < chain.size (); ++i)
                    EXPECT_DOUBLE_EQ (chain[i].m[row][column], before[i].m[row][column]);
            }
        }
    }
}

TEST (GeometryRustAffine, CompositionRefusalsPreservePriorMatrix)
{
    for (int kind = 0; kind < 3; ++kind) {
        std::vector<oracle::Transform> chain (2);
        if (kind == 0)
            chain.back ().m[1][2] = std::numeric_limits<double>::quiet_NaN ();
        else if (kind == 1)
            for (auto& transform : chain)
                transform.m[0][3] = std::numeric_limits<double>::max ();
        else
            for (auto& transform : chain)
                for (size_t axis = 0; axis < 3; ++axis)
                    transform.m[axis][axis] = 1.0e100;
        auto output = oracle::Translation ({ 77, 88, 99 });
        const auto before = output;
        std::string error;
        EXPECT_FALSE (tapioca::geometry::ComposeTransforms (chain, output, error));
        EXPECT_FALSE (error.empty ());
        for (size_t row = 0; row < 3; ++row)
            for (size_t column = 0; column < 4; ++column)
                EXPECT_DOUBLE_EQ (output.m[row][column], before.m[row][column]);
    }
}
