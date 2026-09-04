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
    return BuildSampleGrid (quad.vertices.data (), quad.vertices.size () / 3, quad.triangles.data (),
                            quad.triangles.size () / 3, nullptr, options);
}

} // namespace

TEST (SunStudyAtlas, WithoutLayoutsThereIsNoAtlas)
{
    const Quad quad = MakeQuad (10.0);
    SamplerOptions options;
    options.spacing = 1.0; // wantLayouts stays false
    const SampleGrid grid =
        BuildSampleGrid (quad.vertices.data (), quad.vertices.size () / 3, quad.triangles.data (), 2, nullptr, options);

    EXPECT_FALSE (BuildSunStudyAtlas (grid).valid);
}

TEST (SunStudyAtlas, EveryGriddedFaceIsPlaced)
{
    const SampleGrid grid = Sample (MakeQuad (10.0), 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);

    ASSERT_TRUE (atlas.valid);
    EXPECT_EQ (atlas.placedFaces, 2u);
    EXPECT_GT (atlas.width, 0u);
    EXPECT_EQ (atlas.width, atlas.height);
}

TEST (SunStudyAtlas, TheAtlasIsAPowerOfTwoSquare)
{
    const SampleGrid grid = Sample (MakeQuad (40.0), 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);

    ASSERT_TRUE (atlas.valid);
    EXPECT_EQ (atlas.width & (atlas.width - 1), 0u) << "atlas width is not a power of two";
}

// ⚠️ THE FAILURE THIS CATCHES PAINTS ONE SURFACE WITH ANOTHER'S HOURS. Two tiles
// sharing a texel is not a crash and not a visible glitch -- it is a facade
// reporting the roof's sun.
TEST (SunStudyAtlas, TilesNeverOverlap)
{
    const SampleGrid grid = Sample (MakeQuad (30.0), 0.5);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
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
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
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
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
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
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid, options);
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
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
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
    const SunStudyAtlas first = BuildSunStudyAtlas (grid);
    const SunStudyAtlas second = BuildSunStudyAtlas (grid);

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
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid, options);

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
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    std::vector<double> hours (grid.Count ());
    for (size_t i = 0; i < hours.size (); ++i)
        hours[i] = static_cast<double> (i);

    const std::vector<float> image = ScatterToAtlas (atlas, hours);
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
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    const std::vector<double> hours (grid.Count (), 0.0);
    const std::vector<float> image = ScatterToAtlas (atlas, hours, -1.0f);

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
    EXPECT_TRUE (ScatterToAtlas (atlas, std::vector<double> { 1.0, 2.0 }).empty ());
}

// ---------------------------------------------------------------------------
// AtlasUv — the map that lets continuous geometry read a point-sampled study
// ---------------------------------------------------------------------------

TEST (SunStudyAtlas, AUvLandsInsideItsOwnTile)
{
    const Quad quad = MakeQuad (10.0);
    const SampleGrid grid = Sample (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    // A point in the middle of face 0.
    const double point[3] = { 3.0, 1.0, 0.0 };
    float uv[2] = { 0.0f, 0.0f };
    ASSERT_TRUE (AtlasUv (atlas, grid, 0, point, 1.0, uv));

    const AtlasTile& tile = atlas.tiles[0];
    const double x = uv[0] * atlas.width;
    const double y = uv[1] * atlas.height;
    EXPECT_GE (x, tile.x - 1e-6);
    EXPECT_LE (x, tile.x + tile.width + 1e-6);
    EXPECT_GE (y, tile.y - 1e-6);
    EXPECT_LE (y, tile.y + tile.height + 1e-6);
}

// A sample's own position must map onto the texel the scatter wrote it to.
// If this drifts, every drawn value is off by a cell and the picture is subtly,
// consistently wrong.
TEST (SunStudyAtlas, ASamplePositionMapsBackToItsOwnTexel)
{
    const Quad quad = MakeQuad (10.0);
    const SampleGrid grid = Sample (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    size_t checked = 0;
    for (size_t i = 0; i < grid.Count (); ++i) {
        const uint32_t face = grid.faces[i];
        float uv[2] = { 0.0f, 0.0f };
        if (!AtlasUv (atlas, grid, face, grid.positions.data () + i * 3, 1.0, uv))
            continue;

        const uint32_t x = static_cast<uint32_t> (uv[0] * atlas.width);
        const uint32_t y = static_cast<uint32_t> (uv[1] * atlas.height);
        EXPECT_EQ (y * atlas.width + x, atlas.texels[i]) << "sample " << i << " mapped to the wrong texel";
        ++checked;
    }
    EXPECT_GT (checked, 0u);
}

TEST (SunStudyAtlas, AnUnplacedFaceRefusesRatherThanReturningTexelZero)
{
    // A quad far below the grid: both faces fall back to a centroid, so neither
    // is gridded and neither can be mapped.
    const SampleGrid grid = Sample (MakeQuad (0.05), 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    const double point[3] = { 0.01, 0.01, 0.0 };
    float uv[2] = { 9.0f, 9.0f };
    EXPECT_FALSE (AtlasUv (atlas, grid, 0, point, 1.0, uv));
    EXPECT_FLOAT_EQ (uv[0], 9.0f) << "the output was written despite refusing";
}

TEST (SunStudyAtlas, AnOutOfRangeFaceIsRefused)
{
    const SampleGrid grid = Sample (MakeQuad (10.0), 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    const double point[3] = { 1.0, 1.0, 0.0 };
    float uv[2] = { 0.0f, 0.0f };
    EXPECT_FALSE (AtlasUv (atlas, grid, 999, point, 1.0, uv));
}

TEST (SunStudyAtlas, ANonPositiveSpacingIsRefused)
{
    const SampleGrid grid = Sample (MakeQuad (10.0), 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    const double point[3] = { 1.0, 1.0, 0.0 };
    float uv[2] = { 0.0f, 0.0f };
    EXPECT_FALSE (AtlasUv (atlas, grid, 0, point, 0.0, uv));
}

// A point beyond the face is clamped into its own tile, never wrapped: wrapping
// would send it to the opposite edge of the SAME face, so one texel of a facade
// would read its far corner's hours.
TEST (SunStudyAtlas, APointOutsideTheFaceIsClampedNotWrapped)
{
    const Quad quad = MakeQuad (10.0);
    const SampleGrid grid = Sample (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    const double far[3] = { 1000.0, 1000.0, 0.0 };
    float uv[2] = { 0.0f, 0.0f };
    ASSERT_TRUE (AtlasUv (atlas, grid, 0, far, 1.0, uv));

    const AtlasTile& tile = atlas.tiles[0];
    EXPECT_LE (uv[0] * atlas.width, tile.x + tile.width + 1e-6);
    EXPECT_LE (uv[1] * atlas.height, tile.y + tile.height + 1e-6);
}
