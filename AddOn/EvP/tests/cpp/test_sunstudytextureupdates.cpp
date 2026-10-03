#include "SunStudy/SunStudyAtlasReuse.hpp"
#include "SunStudy/SunStudyTextureDelta.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudySurfaceSampling.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "MeshFixtures.hpp"

#include <gtest/gtest.h>
#include <random>

using namespace evp::sunstudy;

namespace {
SampleGrid Sample (const geomsrv::Snapshot& scene, std::vector<uint8_t> mask = {})
{
    if (mask.empty ())
        mask.assign (scene.meshes.size (), 1u);
    SurfaceSamplingOptions options;
    options.spacing = 0.3;
    return BuildSurfaceSampling (scene, mask, options).triangles;
}
} // namespace

TEST (SunStudyTextureUpdates, TriangleTilesSurviveMeshReorderCreationAndReceiverRemoval)
{
    auto scene = evptest::MakeSnapshot ({ evptest::MakeBox ("a", 0, 0, 0), evptest::MakeBox ("b", 5, 0, 0) });
    const auto grid = Sample (scene);
    SunStudyPatchAtlas original;
    const auto atlas = BuildStableTriangleAtlas (grid, scene, original);
    ASSERT_TRUE (atlas.valid);
    std::swap (scene.meshes[0], scene.meshes[1]);
    scene.meshes.push_back (evptest::MakeBox ("new", 10, 0, 0, 0.1, 0.1, 0.1));
    const auto nextGrid = Sample (scene);
    SunStudyPatchAtlas next;
    const auto nextAtlas = BuildStableTriangleAtlas (nextGrid, scene, next, &original);
    ASSERT_TRUE (nextAtlas.valid);
    ASSERT_EQ (nextAtlas.width, atlas.width);
    for (size_t face = 0; face < 12; ++face) {
        EXPECT_EQ (nextAtlas.tiles[face].x, atlas.tiles[face + 12].x);
        EXPECT_EQ (nextAtlas.tiles[face].y, atlas.tiles[face + 12].y);
    }
    const auto excluded = Sample (scene, { 0, 1, 1 });
    SunStudyPatchAtlas third;
    const auto thirdAtlas = BuildStableTriangleAtlas (excluded, scene, third, &next);
    ASSERT_TRUE (thirdAtlas.valid);
    EXPECT_FALSE (thirdAtlas.tiles[0].Placed ());
    EXPECT_EQ (thirdAtlas.tiles[12].x, nextAtlas.tiles[12].x);
    EXPECT_EQ (original.AllocationCount (), 24u); // copy-on-replacement, source unchanged
}

TEST (SunStudyTextureUpdates, GrowthIsCompleteAndAmbiguousGuidsNeverAliasAllocations)
{
    auto scene = evptest::MakeSnapshot ({ evptest::MakeBox ("a", 0, 0, 0) });
    SunStudyPatchAtlas original, next;
    const auto atlas = BuildStableTriangleAtlas (Sample (scene), scene, original);
    ASSERT_TRUE (atlas.valid);
    scene.meshes.push_back (evptest::MakeBox ("big", 5, 0, 0, 30, 30, 1));
    const auto grown = BuildStableTriangleAtlas (Sample (scene), scene, next, &original);
    ASSERT_TRUE (grown.valid);
    EXPECT_GT (grown.width, atlas.width);
    EXPECT_EQ (grown.placedFaces, 24u);
    EXPECT_EQ (original.Width (), atlas.width);
    scene.meshes[1].guid = "A"; // duplicate canonical identity forces the baseline packer
    const auto ambiguous = BuildStableTriangleAtlas (Sample (scene), scene, next, &original);
    ASSERT_TRUE (ambiguous.valid);
    EXPECT_EQ (next.AllocationCount (), 0u);
    EXPECT_EQ (ambiguous.placedFaces, 24u);
}

TEST (SunStudyTextureUpdates, RegionUpdatesRequireTheExactRetainedBaseAndCompatibleDimensions)
{
    using namespace geomsrv::archviz;
    SunStudyAtlasUpload previous, update;
    previous.width = update.width = 8;
    previous.height = update.height = 8;
    previous.texels = std::make_shared<const std::vector<float>> (64, -1.0f);
    update.texels = std::make_shared<const std::vector<float>> (64, 1.0f);
    update.baseTexels = previous.texels;
    update.atlasRegions = { { 0, 0, 8, 8, 0 } };
    EXPECT_TRUE (CanApplySunAtlasRegions (previous, update));
    auto skipped = previous;
    skipped.texels = update.texels;
    EXPECT_FALSE (CanApplySunAtlasRegions (skipped, update));
    update.width = 16;
    EXPECT_FALSE (CanApplySunAtlasRegions (previous, update));
    update.width = 8;
    update.atlasRegions[0].width = 9;
    EXPECT_FALSE (CanApplySunAtlasRegions (previous, update));
    update.atlasRegions.clear (); // unchanged texels are a valid zero-byte update
    EXPECT_TRUE (CanApplySunAtlasRegions (previous, update));
    update.texels = std::make_shared<const std::vector<float>> (1, 0.0f);
    EXPECT_FALSE (CanApplySunAtlasRegions (previous, update));
    previous.stepWords = update.stepWords = 2;
    previous.stepMasks = std::make_shared<const std::vector<uint32_t>> (128, 0u);
    update.stepMasks = std::make_shared<const std::vector<uint32_t>> (128, 1u);
    update.baseStepMasks = previous.stepMasks;
    EXPECT_TRUE (CanApplySunStepRegions (previous, update));
    update.stepWords = 1;
    EXPECT_FALSE (CanApplySunStepRegions (previous, update));
}

TEST (SunStudyTextureUpdates, UnchangedSideMapsRetainBuffersButChangedOriginsOrTilesDoNot)
{
    using namespace geomsrv::archviz;
    SunStudyElementMap map;
    map.guid = "element";
    map.topologyHash = 123;
    map.faces.resize (2);
    auto next = map;
    EXPECT_TRUE (SameSunStudyElementMap (map, next));
    next.faces[1].tile[0] = 10;
    EXPECT_FALSE (SameSunStudyElementMap (map, next));
    next = map;
    next.faces[0].originAndInvSpacing[2] = 5.0f;
    EXPECT_FALSE (SameSunStudyElementMap (map, next));
    next = map;
    ++next.topologyHash;
    EXPECT_FALSE (SameSunStudyElementMap (map, next));
}

TEST (SunStudyTextureUpdates, ExactDeltasReconstructHoursAndStepBitsIncludingRetiredTexels)
{
    constexpr uint32_t width = 16, height = 8, layers = 2;
    std::vector<uint32_t> before (width * height * layers, 0u), after = before;
    after[3 * width + 4] = 0x80000001u;
    after[4 * width + 4] = 0x40000000u;
    after[width * height + 9] = 7u;
    auto regions = AtlasChangedRegions (width, height, layers, before, after);
    ASSERT_EQ (regions.size (), 2u);
    EXPECT_TRUE (AtlasRegionsValid (regions, width, height, layers));
    auto applied = before;
    for (const auto& r : regions)
        for (uint32_t y = r.y; y < r.y + r.height; ++y)
            for (uint32_t x = r.x; x < r.x + r.width; ++x) {
                const size_t i = r.layer * width * height + y * width + x;
                applied[i] = after[i];
            }
    EXPECT_EQ (applied, after);
    std::vector<float> oldHours (width * height, -1.0f), newHours = oldHours;
    oldHours[12] = 8.0f; // deleted receiver must revert to sentinel, not remain lit
    newHours[25] = 0.0f; // valid permanent shade is not an empty texel
    const auto floats = AtlasChangedRegions (width, height, 1, oldHours, newHours);
    applied.assign (width * height, 0u);
    size_t covered = 0;
    for (const auto& r : floats)
        covered += r.width * r.height;
    EXPECT_EQ (covered, 2u);
    EXPECT_TRUE (AtlasChangedRegions (width, height, 1, newHours, newHours).empty ());
}

TEST (SunStudyTextureUpdates, FragmentationIsBoundedAndInvalidRegionsCannotReachGpu)
{
    std::vector<uint32_t> before (64, 0u), after = before;
    for (size_t i = 0; i < after.size (); i += 2)
        after[i] = 1;
    const auto regions = AtlasChangedRegions (8, 8, 1, before, after, 2);
    ASSERT_EQ (regions.size (), 1u);
    EXPECT_EQ (regions[0].width, 8u);
    EXPECT_EQ (regions[0].height, 8u);
    EXPECT_FALSE (AtlasRegionsValid ({ { 7, 0, 2, 1, 0 } }, 8, 8, 1));
    EXPECT_FALSE (AtlasRegionsValid ({ { 0, 0, 1, 1, 1 } }, 8, 8, 1));
    EXPECT_FALSE (AtlasRegionsValid ({ { 0, 0, 0, 1, 0 } }, 8, 8, 1));
    EXPECT_TRUE (AtlasChangedRegions (0, 8, 1, before, after).empty ());
}

TEST (SunStudyTextureUpdates, RandomizedRegionsCoverEveryChangedCellWithoutTouchingAnotherLayer)
{
    std::mt19937 random (504);
    for (size_t trial = 0; trial < 40; ++trial) {
        std::vector<uint32_t> before (32 * 24 * 3), after = before;
        for (auto& value : after)
            if (random () % 5 == 0)
                value = random ();
        const auto regions = AtlasChangedRegions (32, 24, 3, before, after, trial % 2 ? 512 : 4);
        ASSERT_TRUE (AtlasRegionsValid (regions, 32, 24, 3));
        auto reconstructed = before;
        for (const auto& r : regions)
            for (uint32_t y = r.y; y < r.y + r.height; ++y)
                for (uint32_t x = r.x; x < r.x + r.width; ++x) {
                    const size_t i = r.layer * 32 * 24 + y * 32 + x;
                    reconstructed[i] = after[i];
                }
        EXPECT_EQ (reconstructed, after);
    }
}
