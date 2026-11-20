// Tests for SunStudy/SunStudyAtlas.
//
// The atlas is what makes a display change free and what unties sample density
// from mesh topology. Its failure modes are all quiet: tiles that overlap paint
// one surface with another's hours, a missing gutter bleeds sun across an
// element edge in a way that looks like real shading, and a non-deterministic
// packing makes a cached study unreusable without ever looking wrong.

#include <algorithm>
#include <set>
#include <vector>

#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

struct Quad {
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
};

Quad MakeQuad (double size)
{
    Quad quad;
    const double s = size;
    quad.vertices = { 0, 0, 0, s, 0, 0, s, s, 0, 0, s, 0 };
    quad.triangles = { 0, 1, 2, 0, 2, 3 };
    return quad;
}

SampleGrid Sample (const Quad& quad, double spacing)
{
    SamplerOptions options;
    options.spacing = spacing;
    options.normalOffset = 0.0;
    options.wantLayouts = true;
    return kernels::BuildSampleGrid (quad.vertices.data (), quad.vertices.size () / 3, quad.triangles.data (),
                                     quad.triangles.size () / 3, nullptr, options);
}

} // namespace

TEST (SunStudyAtlas, WithoutLayoutsThereIsNoAtlas)
{
    const Quad quad = MakeQuad (10.0);
    SamplerOptions options;
    options.spacing = 1.0; // wantLayouts stays false
    const SampleGrid grid = kernels::BuildSampleGrid (quad.vertices.data (), quad.vertices.size () / 3,
                                                      quad.triangles.data (), 2, nullptr, options);

    EXPECT_FALSE (kernels::BuildSunStudyAtlas (grid).valid);
}

TEST (SunStudyAtlas, EveryGriddedFaceIsPlaced)
{
    const SampleGrid grid = Sample (MakeQuad (10.0), 1.0);
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid);

    ASSERT_TRUE (atlas.valid);
    EXPECT_EQ (atlas.placedFaces, 2u);
    EXPECT_GT (atlas.width, 0u);
    EXPECT_EQ (atlas.width, atlas.height);
}

TEST (SunStudyAtlas, TheAtlasIsAPowerOfTwoSquare)
{
    const SampleGrid grid = Sample (MakeQuad (40.0), 1.0);
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid);

    ASSERT_TRUE (atlas.valid);
    EXPECT_EQ (atlas.width & (atlas.width - 1), 0u) << "atlas width is not a power of two";
}

// ⚠️ THE FAILURE THIS CATCHES PAINTS ONE SURFACE WITH ANOTHER'S HOURS. Two tiles
// sharing a texel is not a crash and not a visible glitch -- it is a facade
// reporting the roof's sun.
TEST (SunStudyAtlas, TilesNeverOverlap)
{
    const SampleGrid grid = Sample (MakeQuad (30.0), 0.5);
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    std::vector<uint8_t> claimed (atlas.TexelCount (), 0);
    for (const AtlasTile& tile : atlas.tiles) {
        if (!tile.Placed ())
            continue;
        for (uint32_t row = 0; row < tile.height; ++row) {
            for (uint32_t column = 0; column < tile.width; ++column) {
                const size_t texel = (tile.y + row) * atlas.width + (tile.x + column);
                ASSERT_LT (texel, claimed.size ());
                EXPECT_EQ (claimed[texel], 0u) << "two tiles claim texel " << texel;
                claimed[texel] = 1;
            }
        }
    }
}

TEST (SunStudyAtlas, EveryTileFitsInsideTheAtlas)
{
    const SampleGrid grid = Sample (MakeQuad (30.0), 0.5);
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    for (const AtlasTile& tile : atlas.tiles) {
        if (!tile.Placed ())
            continue;
        EXPECT_LE (tile.x + tile.width, atlas.width);
        EXPECT_LE (tile.y + tile.height, atlas.height);
    }
}

// ⚠️ WITHOUT A GUTTER A BILINEAR SAMPLE AT A TILE EDGE READS THE NEIGHBOUR. The
// artefact appears along element boundaries, exactly where a person expects
// shading to change, so it reads as real shadow.
TEST (SunStudyAtlas, TilesAreSeparatedByAtLeastOneTexel)
{
    const SampleGrid grid = Sample (MakeQuad (20.0), 1.0);
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    std::vector<const AtlasTile*> placed;
    for (const AtlasTile& tile : atlas.tiles) {
        if (tile.Placed ())
            placed.push_back (&tile);
    }
    ASSERT_GE (placed.size (), 2u);

    for (size_t i = 0; i < placed.size (); ++i) {
        for (size_t j = i + 1; j < placed.size (); ++j) {
            const AtlasTile& a = *placed[i];
            const AtlasTile& b = *placed[j];
            // Expanded by one texel, they must still not intersect.
            const bool separated = (a.x + a.width + 1 <= b.x) || (b.x + b.width + 1 <= a.x) ||
                                   (a.y + a.height + 1 <= b.y) || (b.y + b.height + 1 <= a.y);
            EXPECT_TRUE (separated) << "tiles " << i << " and " << j << " touch";
        }
    }
}

TEST (SunStudyAtlas, ZeroGutterIsRaisedToOne)
{
    AtlasOptions options;
    options.gutter = 0;
    const SampleGrid grid = Sample (MakeQuad (20.0), 1.0);
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid, options);
    ASSERT_TRUE (atlas.valid);

    // Same assertion as above: a zero gutter must not actually be honoured.
    std::vector<const AtlasTile*> placed;
    for (const AtlasTile& tile : atlas.tiles) {
        if (tile.Placed ())
            placed.push_back (&tile);
    }
    ASSERT_GE (placed.size (), 2u);
    const AtlasTile& a = *placed[0];
    const AtlasTile& b = *placed[1];
    const bool separated = (a.x + a.width + 1 <= b.x) || (b.x + b.width + 1 <= a.x) || (a.y + a.height + 1 <= b.y) ||
                           (b.y + b.height + 1 <= a.y);
    EXPECT_TRUE (separated);
}

TEST (SunStudyAtlas, EverySampleGetsADistinctTexel)
{
    const SampleGrid grid = Sample (MakeQuad (20.0), 1.0);
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);
    ASSERT_EQ (atlas.texels.size (), grid.Count ());

    std::set<uint32_t> seen;
    for (uint32_t texel : atlas.texels) {
        EXPECT_LT (texel, atlas.TexelCount ());
        EXPECT_TRUE (seen.insert (texel).second) << "two samples share texel " << texel;
    }
}

// ⚠️ DETERMINISM IS A CACHING AND DIFFING REQUIREMENT. An atlas that repacks
// differently between runs never looks wrong, it just makes a stored study
// unreusable and a two-engine comparison meaningless.
TEST (SunStudyAtlas, PackingIsDeterministic)
{
    const SampleGrid grid = Sample (MakeQuad (25.0), 0.7);
    const SunStudyAtlas first = kernels::BuildSunStudyAtlas (grid);
    const SunStudyAtlas second = kernels::BuildSunStudyAtlas (grid);

    ASSERT_TRUE (first.valid);
    EXPECT_EQ (first.width, second.width);
    EXPECT_EQ (first.texels, second.texels);
    for (size_t i = 0; i < first.tiles.size (); ++i) {
        EXPECT_EQ (first.tiles[i].x, second.tiles[i].x);
        EXPECT_EQ (first.tiles[i].y, second.tiles[i].y);
    }
}

TEST (SunStudyAtlas, AModelTooLargeForTheCeilingIsRefusedNotTruncated)
{
    AtlasOptions options;
    options.maxDimension = 16; // absurdly small on purpose
    const SampleGrid grid = Sample (MakeQuad (50.0), 0.25);
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid, options);

    EXPECT_FALSE (atlas.valid);
    for (const AtlasTile& tile : atlas.tiles)
        EXPECT_FALSE (tile.Placed ()) << "a refused atlas still placed a tile";
}

// ---------------------------------------------------------------------------
// ScatterToAtlas
// ---------------------------------------------------------------------------

TEST (SunStudyAtlas, ScatteredValuesLandOnTheirOwnTexels)
{
    const SampleGrid grid = Sample (MakeQuad (10.0), 1.0);
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    std::vector<double> hours (grid.Count ());
    for (size_t i = 0; i < hours.size (); ++i)
        hours[i] = static_cast<double> (i);

    const std::vector<float> image = kernels::ScatterToAtlas (atlas, hours);
    ASSERT_EQ (image.size (), atlas.TexelCount ());
    for (size_t i = 0; i < hours.size (); ++i)
        EXPECT_FLOAT_EQ (image[atlas.texels[i]], static_cast<float> (i));
}

// ⚠️ ZERO IS A LEGITIMATE RESULT -- "never sunlit" -- so an untouched texel
// drawn as zero is a shadow that is not there. The sentinel must be
// distinguishable.
TEST (SunStudyAtlas, UntouchedTexelsCarryTheSentinelNotZero)
{
    const SampleGrid grid = Sample (MakeQuad (10.0), 1.0);
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    const std::vector<double> hours (grid.Count (), 0.0);
    const std::vector<float> image = kernels::ScatterToAtlas (atlas, hours, -1.0f);

    size_t sentinels = 0;
    for (float value : image) {
        if (value < 0.0f)
            ++sentinels;
    }
    EXPECT_GT (sentinels, 0u) << "the gutters were filled with a real value";
    EXPECT_EQ (image.size () - sentinels, grid.Count ());
}

TEST (SunStudyAtlas, ScatteringAnEmptyAtlasIsEmptyNotACrash)
{
    SunStudyAtlas atlas;
    EXPECT_TRUE (kernels::ScatterToAtlas (atlas, std::vector<double> { 1.0, 2.0 }).empty ());
}

// ---------------------------------------------------------------------------
// AtlasUv — the map that lets continuous geometry read a point-sampled study
// ---------------------------------------------------------------------------
