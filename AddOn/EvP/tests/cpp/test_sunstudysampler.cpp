// Tests for SunStudy/SunStudySampler.
//
// The properties asserted here are the ones whose failure produces a study that
// looks right: a facade that silently sampled once, a small element that
// vanished from the totals, an area percentage that no longer divides by the
// real area. Each of those was a real defect in the engine this replaces.

#include <cmath>
#include <map>
#include <vector>

#include "SunStudy/SunStudySampler.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

// A unit square in the XY plane at z = 0, as two triangles.
struct Quad {
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
};

Quad MakeQuad (double size, bool vertical = false)
{
    Quad quad;
    const double s = size;
    if (vertical) {
        // In the XZ plane, so its world-XY footprint is a line.
        quad.vertices = { 0, 0, 0, s, 0, 0, s, 0, s, 0, 0, s };
    }
    else {
        quad.vertices = { 0, 0, 0, s, 0, 0, s, s, 0, 0, s, 0 };
    }
    quad.triangles = { 0, 1, 2, 0, 2, 3 };
    return quad;
}

SampleGrid Build (const Quad& quad, const SamplerOptions& options, const uint32_t* groups = nullptr)
{
    return BuildSampleGrid (quad.vertices.data (), quad.vertices.size () / 3, quad.triangles.data (),
                            quad.triangles.size () / 3, groups, options);
}

} // namespace

TEST (SunStudySampler, EmptyInputIsNotValid)
{
    SamplerOptions options;
    EXPECT_FALSE (BuildSampleGrid (nullptr, 0, nullptr, 0, nullptr, options).valid);
}

TEST (SunStudySampler, NonPositiveSpacingIsRefused)
{
    const Quad quad = MakeQuad (10.0);
    SamplerOptions options;
    options.spacing = 0.0;
    EXPECT_FALSE (Build (quad, options).valid);
}

TEST (SunStudySampler, ParallelArraysStayInStep)
{
    const Quad quad = MakeQuad (10.0);
    SamplerOptions options;
    options.spacing = 1.0;

    const SampleGrid grid = Build (quad, options);
    ASSERT_TRUE (grid.valid);
    EXPECT_GT (grid.Count (), 0u);
    EXPECT_EQ (grid.positions.size (), grid.Count () * 3);
    EXPECT_EQ (grid.normals.size (), grid.Count () * 3);
    EXPECT_EQ (grid.areas.size (), grid.Count ());
    EXPECT_EQ (grid.groups.size (), grid.Count ());
}

// The invariant every area percentage in the report rests on.
TEST (SunStudySampler, SampleAreasSumToTheMeshArea)
{
    const Quad quad = MakeQuad (10.0);
    SamplerOptions options;
    options.spacing = 0.7;

    const SampleGrid grid = Build (quad, options);
    ASSERT_TRUE (grid.valid);

    double total = 0.0;
    for (double a : grid.areas)
        total += a;
    EXPECT_NEAR (total, 100.0, 1e-9);
}

TEST (SunStudySampler, SampleCountTracksTheRequestedSpacing)
{
    const Quad quad = MakeQuad (10.0);

    SamplerOptions coarse;
    coarse.spacing = 2.0;
    SamplerOptions fine;
    fine.spacing = 1.0;

    const size_t coarseCount = Build (quad, coarse).Count ();
    const size_t fineCount = Build (quad, fine).Count ();

    // Halving the pitch quarters the cell, so roughly four times the samples.
    EXPECT_NEAR (static_cast<double> (fineCount) / static_cast<double> (coarseCount), 4.0, 0.6);
}

// ⚠️ THE FACADE TEST. Gridding in world XY rather than in the plane of the face
// would give a vertical wall a zero-width footprint, so it would fall back to
// one centroid sample per triangle and every facade in the study would be
// measured at two points. That reads as a working study.
TEST (SunStudySampler, VerticalFacesAreSampledAsDenselyAsHorizontalOnes)
{
    SamplerOptions options;
    options.spacing = 1.0;

    const size_t flat = Build (MakeQuad (10.0, false), options).Count ();
    const size_t wall = Build (MakeQuad (10.0, true), options).Count ();

    EXPECT_EQ (flat, wall);
    EXPECT_GT (wall, 50u);
}

// ⚠️ SMALL ELEMENTS MUST NOT DISAPPEAR. A mullion or a tread is smaller than any
// sensible grid; dropping it deletes it from the study while the totals stay
// plausible.
TEST (SunStudySampler, FacesBelowTheGridStillEmitOneSample)
{
    const Quad quad = MakeQuad (0.05);
    SamplerOptions options;
    options.spacing = 1.0;

    const SampleGrid grid = Build (quad, options);
    ASSERT_TRUE (grid.valid);
    EXPECT_EQ (grid.Count (), 2u);
    EXPECT_EQ (grid.undersizedFaces, 2u);

    double total = 0.0;
    for (double a : grid.areas)
        total += a;
    EXPECT_NEAR (total, 0.05 * 0.05, 1e-12);
}

TEST (SunStudySampler, EveryFaceIsRepresented)
{
    const Quad quad = MakeQuad (10.0);
    SamplerOptions options;
    options.spacing = 1.0;

    const SampleGrid grid = Build (quad, options);
    std::map<uint32_t, size_t> perFace;
    for (uint32_t f : grid.faces)
        ++perFace[f];

    EXPECT_EQ (perFace.size (), 2u);
    EXPECT_GT (perFace[0], 0u);
    EXPECT_GT (perFace[1], 0u);
}

TEST (SunStudySampler, DegenerateFacesAreCountedAndSkipped)
{
    std::vector<double> vertices = { 0, 0, 0, 1, 0, 0, 2, 0, 0 }; // collinear
    std::vector<uint32_t> triangles = { 0, 1, 2 };

    SamplerOptions options;
    options.spacing = 0.5;
    const SampleGrid grid = BuildSampleGrid (vertices.data (), 3, triangles.data (), 1, nullptr, options);

    EXPECT_TRUE (grid.valid);
    EXPECT_EQ (grid.Count (), 0u);
    EXPECT_EQ (grid.degenerateFaces, 1u);
}

TEST (SunStudySampler, OutOfRangeIndicesAreRejectedNotDereferenced)
{
    std::vector<double> vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    std::vector<uint32_t> triangles = { 0, 1, 99 };

    SamplerOptions options;
    options.spacing = 0.5;
    const SampleGrid grid = BuildSampleGrid (vertices.data (), 3, triangles.data (), 1, nullptr, options);

    EXPECT_EQ (grid.Count (), 0u);
    EXPECT_EQ (grid.degenerateFaces, 1u);
}

TEST (SunStudySampler, NormalsAreUnitAndFaceTheSourceWinding)
{
    const Quad quad = MakeQuad (4.0);
    SamplerOptions options;
    options.spacing = 1.0;

    const SampleGrid grid = Build (quad, options);
    ASSERT_GT (grid.Count (), 0u);
    for (size_t i = 0; i < grid.Count (); ++i) {
        const double* n = grid.normals.data () + i * 3;
        EXPECT_NEAR (std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1.0, 1e-12);
        EXPECT_NEAR (n[2], 1.0, 1e-12);
    }
}

TEST (SunStudySampler, SamplesAreLiftedAlongTheirNormal)
{
    const Quad quad = MakeQuad (4.0);
    SamplerOptions options;
    options.spacing = 1.0;
    options.normalOffset = 0.25;

    const SampleGrid grid = Build (quad, options);
    ASSERT_GT (grid.Count (), 0u);
    for (size_t i = 0; i < grid.Count (); ++i)
        EXPECT_NEAR (grid.positions[i * 3 + 2], 0.25, 1e-12);
}

TEST (SunStudySampler, GroupIdsArePassedThroughPerFace)
{
    const Quad quad = MakeQuad (4.0);
    const uint32_t groups[2] = { 7u, 42u };
    SamplerOptions options;
    options.spacing = 1.0;

    const SampleGrid grid = Build (quad, options, groups);
    ASSERT_GT (grid.Count (), 0u);
    for (size_t i = 0; i < grid.Count (); ++i)
        EXPECT_EQ (grid.groups[i], groups[grid.faces[i]]);
}

// ⚠️ DETERMINISM IS A TEST REQUIREMENT, NOT A PREFERENCE. Jitter exists to break
// aliasing against regular architecture; if it also made the sample set vary run
// to run, no study could be regression-tested and the two engines could never be
// diffed sample for sample.
TEST (SunStudySampler, JitterIsDeterministic)
{
    const Quad quad = MakeQuad (10.0);
    SamplerOptions options;
    options.spacing = 1.0;
    options.jitter = 0.8;

    const SampleGrid first = Build (quad, options);
    const SampleGrid second = Build (quad, options);

    ASSERT_EQ (first.Count (), second.Count ());
    EXPECT_EQ (first.positions, second.positions);
}

TEST (SunStudySampler, JitterActuallyMovesTheSamples)
{
    const Quad quad = MakeQuad (10.0);
    SamplerOptions plain;
    plain.spacing = 1.0;
    SamplerOptions jittered = plain;
    jittered.jitter = 0.8;

    const SampleGrid a = Build (quad, plain);
    const SampleGrid b = Build (quad, jittered);
    EXPECT_NE (a.positions, b.positions);
}

TEST (SunStudySampler, JitteredSamplesStayInsideTheirFace)
{
    const Quad quad = MakeQuad (10.0);
    SamplerOptions options;
    options.spacing = 1.0;
    options.jitter = 1.0;
    options.normalOffset = 0.0;

    const SampleGrid grid = Build (quad, options);
    ASSERT_GT (grid.Count (), 0u);
    for (size_t i = 0; i < grid.Count (); ++i) {
        EXPECT_GE (grid.positions[i * 3 + 0], -1e-9);
        EXPECT_LE (grid.positions[i * 3 + 0], 10.0 + 1e-9);
        EXPECT_GE (grid.positions[i * 3 + 1], -1e-9);
        EXPECT_LE (grid.positions[i * 3 + 1], 10.0 + 1e-9);
    }
}

TEST (SunStudySampler, TheBudgetRefusesRatherThanTruncates)
{
    const Quad quad = MakeQuad (100.0);
    SamplerOptions options;
    options.spacing = 0.5;
    options.maxSamples = 100;

    const SampleGrid grid = Build (quad, options);
    EXPECT_FALSE (grid.valid);
}

TEST (SunStudySampler, TriangleNormalReportsArea)
{
    double area = 0.0;
    const Vec3 n = TriangleNormal (Vec3 { 0, 0, 0 }, Vec3 { 3, 0, 0 }, Vec3 { 0, 4, 0 }, &area);
    EXPECT_NEAR (area, 6.0, 1e-12);
    EXPECT_NEAR (n[2], 1.0, 1e-12);
}

TEST (SunStudySampler, TriangleNormalOfADegenerateFaceIsZeroNotNan)
{
    double area = 0.0;
    const Vec3 n = TriangleNormal (Vec3 { 0, 0, 0 }, Vec3 { 1, 0, 0 }, Vec3 { 2, 0, 0 }, &area);
    EXPECT_DOUBLE_EQ (area, 0.0);
    EXPECT_DOUBLE_EQ (n[0], 0.0);
    EXPECT_DOUBLE_EQ (n[1], 0.0);
    EXPECT_DOUBLE_EQ (n[2], 0.0);
}

TEST (SunStudySampler, CellJitterStaysWithinOneCell)
{
    for (uint32_t f = 0; f < 8; ++f) {
        for (uint32_t u = 0; u < 8; ++u) {
            double out[2] = { 0.0, 0.0 };
            CellJitter (f, u, u * 3 + 1, out);
            EXPECT_GE (out[0], -0.5);
            EXPECT_LE (out[0], 0.5);
            EXPECT_GE (out[1], -0.5);
            EXPECT_LE (out[1], 0.5);
        }
    }
}

// A slab modelled as a box: the sampler must not merge the top and the bottom.
// The engine this replaces welded vertices and needed a normal-aware variant to
// avoid exactly this, at a cost of a quarter more samples.
TEST (SunStudySampler, OppositeFacesOfASlabKeepSeparateSamples)
{
    // Two coincident-footprint quads, one facing up at z=0, one facing down.
    std::vector<double> vertices = {
        0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, // up
        0, 0, 0, 0, 4, 0, 4, 4, 0, 4, 0, 0, // down (reverse winding)
    };
    std::vector<uint32_t> triangles = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };

    SamplerOptions options;
    options.spacing = 1.0;
    options.normalOffset = 0.01;

    const SampleGrid grid = BuildSampleGrid (vertices.data (), 8, triangles.data (), 4, nullptr, options);
    ASSERT_TRUE (grid.valid);

    size_t up = 0, down = 0;
    for (size_t i = 0; i < grid.Count (); ++i) {
        if (grid.normals[i * 3 + 2] > 0.0)
            ++up;
        else
            ++down;
    }
    EXPECT_GT (up, 0u);
    EXPECT_EQ (up, down);
}
