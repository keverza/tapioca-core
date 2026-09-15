// Tests for the patch-domain sampler and the patch atlas — the two pieces that
// have to exist before a live update can be selective.
//
// ⚠️ THE CENTRAL CLAIM IS "NOTHING MOVED", AND IT IS INVISIBLE. A correct
// incremental atlas update and a full repack that happens to produce the same
// picture are indistinguishable on screen; the only difference is whether every
// OTHER surface's texture coordinates survived. So the assertions here are about
// counts and addresses, not about values.

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "SunStudy/SunStudyPatchAtlas.hpp"
#include "SunStudy/SunStudyPatchSampler.hpp"
#include "SunStudy/SunStudySampler.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

struct Scene {
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> groups;
    std::vector<std::string> elementOf;
};

void AddQuad (Scene& scene, const std::string& element, double x0, double y0, double x1, double y1, double z)
{
    const uint32_t base = static_cast<uint32_t> (scene.vertices.size () / 3);
    uint32_t group = 0;
    bool found = false;
    for (size_t i = 0; i < scene.elementOf.size (); ++i) {
        if (scene.elementOf[i] == element) {
            group = static_cast<uint32_t> (i);
            found = true;
        }
    }
    if (!found) {
        group = static_cast<uint32_t> (scene.elementOf.size ());
        scene.elementOf.push_back (element);
    }
    const double v[12] = { x0, y0, z, x1, y0, z, x1, y1, z, x0, y1, z };
    scene.vertices.insert (scene.vertices.end (), v, v + 12);
    const uint32_t t[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
    scene.triangles.insert (scene.triangles.end (), t, t + 6);
    scene.groups.push_back (group);
    scene.groups.push_back (group);
}

// The same rectangle as `strips` columns SHARING their vertices -- which is what
// Geometry/VertexWeld produces for one body's coplanar faces.
void AddWeldedStrips (Scene& scene, const std::string& element, double w, double h, int strips)
{
    const uint32_t base = static_cast<uint32_t> (scene.vertices.size () / 3);
    const uint32_t group = static_cast<uint32_t> (scene.elementOf.size ());
    scene.elementOf.push_back (element);
    for (int i = 0; i <= strips; ++i) {
        const double x = w * i / strips;
        scene.vertices.insert (scene.vertices.end (), { x, 0.0, 0.0 });
        scene.vertices.insert (scene.vertices.end (), { x, h, 0.0 });
    }
    for (int i = 0; i < strips; ++i) {
        const uint32_t a = base + uint32_t (i * 2);
        const uint32_t b = base + uint32_t (i * 2 + 1);
        const uint32_t c = base + uint32_t (i * 2 + 2);
        const uint32_t d = base + uint32_t (i * 2 + 3);
        scene.triangles.insert (scene.triangles.end (), { a, c, d });
        scene.triangles.insert (scene.triangles.end (), { a, d, b });
        scene.groups.push_back (group);
        scene.groups.push_back (group);
    }
}

PatchSampleGrid SampleOf (const Scene& scene, double spacing)
{
    PatchSamplerOptions options;
    options.spacing = spacing;
    options.normalOffset = 0.0;
    return BuildPatchSampleGrid (scene.vertices.data (), scene.vertices.size () / 3, scene.triangles.data (),
                                 scene.triangles.size () / 3, scene.groups.data (), scene.elementOf, options);
}

} // namespace

// ---------------------------------------------------------------------------
// the sampler
// ---------------------------------------------------------------------------

TEST (PatchSampler, EachPatchOwnsAContiguousSampleRange)
{
    // ⚠️ CONTIGUITY IS WHAT MAKES INCREMENTAL ANALYSIS A SUBSET RATHER THAN A
    // REWRITE. The occlusion accumulator takes a (pointer, count) SampleSet, so a
    // patch's own span is already a valid sample set -- recomputing one surface
    // is the same engine over a shorter array, with no copying and no second
    // accumulator.
    Scene scene;
    AddQuad (scene, "a", 0, 0, 4, 3, 0);
    AddQuad (scene, "b", 10, 0, 14, 3, 0);
    const PatchSampleGrid grid = SampleOf (scene, 1.0);

    ASSERT_TRUE (grid.valid);
    ASSERT_EQ (grid.spans.size (), 2u);
    size_t cursor = 0;
    for (const PatchSampleSpan& span : grid.spans) {
        EXPECT_EQ (span.first, cursor) << "a gap or an overlap between spans";
        cursor += span.count;
    }
    EXPECT_EQ (cursor, grid.Count ());
    EXPECT_EQ (grid.Count (), 24u); // two 4x3 lattices, all cells covered
}

TEST (PatchSampler, TheSamplesDoNotDependOnTheTriangulation)
{
    // The whole reason the patch domain exists, stated as an equality: the same
    // surface cut differently must measure the same places, with the same areas.
    Scene coarse;
    AddQuad (coarse, "wall", 0, 0, 6, 2, 0);

    Scene fine;
    AddWeldedStrips (fine, "wall", 6.0, 2.0, 6);

    const PatchSampleGrid a = SampleOf (coarse, 1.0);
    const PatchSampleGrid b = SampleOf (fine, 1.0);
    ASSERT_TRUE (a.valid);
    ASSERT_TRUE (b.valid);
    ASSERT_EQ (a.spans.size (), 1u);
    ASSERT_EQ (b.spans.size (), 1u) << "the strips did not merge into one surface";
    ASSERT_EQ (a.Count (), b.Count ());
    for (size_t i = 0; i < a.Count (); ++i) {
        EXPECT_NEAR (a.areas[i], b.areas[i], 1e-9);
        for (int axis = 0; axis < 3; ++axis)
            EXPECT_NEAR (a.positions[i * 3 + axis], b.positions[i * 3 + axis], 1e-9);
    }
    EXPECT_NEAR (a.TotalArea (), 12.0, 1e-9);
}

TEST (PatchSampler, UNWELDED_DUPLICATE_VERTICES_DO_NOT_MERGE_AND_THAT_IS_A_REAL_DEPENDENCY)
{
    // ⚠️ PATCH MERGING RIDES ON THE EXTRACTOR HAVING WELDED COINCIDENT CORNERS,
    // and this test exists so that dependency is written down rather than
    // discovered. Adjacency is built from SHARED SOURCE VERTEX INDICES -- two
    // triangles that merely touch geometrically, with their own copies of the
    // corner, are not neighbours and never will be.
    //
    // In the live path that is satisfied: `Geometry/VertexWeld` merges corners
    // agreeing on source vertex AND normal, and one body's coplanar faces agree
    // on both. If an extraction ever stopped welding, patch mode would silently
    // degrade back to one patch per triangle -- the picture would keep working,
    // the diagonal seams would return, and the incremental cache would re-key
    // itself on every edit. The symptom to look for is a patch count equal to
    // the triangle count.
    Scene unwelded;
    for (int i = 0; i < 6; ++i)
        AddQuad (unwelded, "wall", i, 0, i + 1, 2, 0);
    const PatchSampleGrid grid = SampleOf (unwelded, 1.0);
    ASSERT_TRUE (grid.valid);
    EXPECT_EQ (grid.spans.size (), 6u) << "unwelded quads cannot be edge-connected";

    // The AREA is still right -- coverage never depended on merging.
    EXPECT_NEAR (grid.TotalArea (), 12.0, 1e-9);
}

TEST (PatchSampler, TheMeasuredAreaMatchesTheTriangleSamplerToTheSurfaceItself)
{
    // ⚠️ THE ONE NUMBER THE TWO DOMAINS MUST AGREE ON. The sample POSITIONS
    // deliberately differ -- that is the migration -- so they cannot be compared
    // point for point. The physical surface area is the same building either
    // way, and both samplers claim to cover it.
    Scene scene;
    AddQuad (scene, "floor", 0, 0, 7, 5, 0);
    const PatchSampleGrid patch = SampleOf (scene, 0.6);

    SamplerOptions legacy;
    legacy.spacing = 0.6;
    legacy.normalOffset = 0.0;
    legacy.wantLayouts = true;
    const SampleGrid triangles =
        BuildSampleGrid (scene.vertices.data (), scene.vertices.size () / 3, scene.triangles.data (),
                         scene.triangles.size () / 3, scene.groups.data (), legacy);
    ASSERT_TRUE (triangles.valid);

    double legacyArea = 0.0;
    for (const double area : triangles.areas)
        legacyArea += area;

    EXPECT_NEAR (patch.TotalArea (), 35.0, 1e-9) << "the patch domain must cover the real surface exactly";
    EXPECT_NEAR (legacyArea, 35.0, 1e-6);
    // And the patch domain measures FEWER, better-placed points for it: the
    // triangle domain gives the two halves of the rectangle their own lattices.
    EXPECT_NE (patch.Count (), triangles.Count ());
}

TEST (PatchSampler, TheSpanBoundsCoverTheWHOLESurfaceNotJustItsSamples)
{
    // ⚠️ AN UNDER-SIZED BOX IS AN UNDER-DIRTY. Cell centres stop half a cell
    // inside the surface; a dirty classifier testing that box against a shadow
    // envelope would miss a shadow falling only on the outer strip, and leave a
    // stale result in a finished-looking picture.
    Scene scene;
    AddQuad (scene, "floor", 0, 0, 4, 3, 0);
    const PatchSampleGrid grid = SampleOf (scene, 1.0);
    ASSERT_EQ (grid.spans.size (), 1u);
    EXPECT_NEAR (grid.spans[0].boundsMin[0], 0.0, 1e-9);
    EXPECT_NEAR (grid.spans[0].boundsMin[1], 0.0, 1e-9);
    EXPECT_NEAR (grid.spans[0].boundsMax[0], 4.0, 1e-9);
    EXPECT_NEAR (grid.spans[0].boundsMax[1], 3.0, 1e-9);
}

// ---------------------------------------------------------------------------
// the atlas
// ---------------------------------------------------------------------------

TEST (PatchAtlas, EverySampleLandsOnItsOwnPatchsTile)
{
    Scene scene;
    AddQuad (scene, "a", 0, 0, 4, 3, 0);
    AddQuad (scene, "b", 10, 0, 13, 5, 0);
    const PatchSampleGrid grid = SampleOf (scene, 1.0);

    SunStudyPatchAtlas atlas;
    const PatchAtlasUpdate update = atlas.Fit (grid);
    EXPECT_EQ (update.added, 2u);
    EXPECT_GT (atlas.Width (), 0u);

    std::set<int64_t> texels;
    for (size_t i = 0; i < grid.Count (); ++i) {
        const int64_t texel = atlas.TexelOf (grid, i);
        ASSERT_GE (texel, 0) << "sample " << i << " has no texel";
        EXPECT_TRUE (texels.insert (texel).second) << "two samples share a texel";

        // ...and it is inside its OWN patch's rectangle.
        const PatchAtlasAllocation* allocation = atlas.Find (grid.spans[grid.spanOf[i]].key);
        ASSERT_NE (allocation, nullptr);
        const int64_t x = texel % atlas.Width ();
        const int64_t y = texel / atlas.Width ();
        EXPECT_GE (x, allocation->x);
        EXPECT_LT (x, allocation->x + allocation->width);
        EXPECT_GE (y, allocation->y);
        EXPECT_LT (y, allocation->y + allocation->height);
    }
}

TEST (PatchAtlas, RefittingAnUnchangedGridMovesNothing)
{
    // ⚠️ THE PROPERTY THE WHOLE CLASS EXISTS FOR, AND IT IS INVISIBLE ON SCREEN.
    // A repack hiding behind an "update" produces an identical picture and
    // invalidates every texture coordinate already handed out.
    Scene scene;
    AddQuad (scene, "a", 0, 0, 4, 3, 0);
    AddQuad (scene, "b", 10, 0, 13, 5, 0);
    const PatchSampleGrid grid = SampleOf (scene, 1.0);

    SunStudyPatchAtlas atlas;
    atlas.Fit (grid);
    std::map<PatchKey, PatchAtlasAllocation> before = atlas.Allocations ();

    const PatchAtlasUpdate again = atlas.Fit (grid);
    EXPECT_EQ (again.retained, 2u);
    EXPECT_EQ (again.added, 0u);
    EXPECT_EQ (again.moved, 0u);
    EXPECT_EQ (again.removed, 0u);
    EXPECT_FALSE (again.resized);

    for (const auto& entry : before) {
        const PatchAtlasAllocation* now = atlas.Find (entry.first);
        ASSERT_NE (now, nullptr);
        EXPECT_EQ (now->x, entry.second.x);
        EXPECT_EQ (now->y, entry.second.y);
        EXPECT_EQ (now->generation, entry.second.generation) << "an unchanged tile was reissued";
    }
}

TEST (PatchAtlas, AChangedElementLeavesEveryOtherTileWhereItWas)
{
    // The live case: one wall moves, everything else must keep its address.
    Scene before;
    AddQuad (before, "floor", 0, 0, 12, 8, 0);
    AddQuad (before, "wallA", 3, 0, 4, 8, 2);
    AddQuad (before, "wallB", 30, 0, 31, 8, 2);
    const PatchSampleGrid gridBefore = SampleOf (before, 1.0);

    SunStudyPatchAtlas atlas;
    atlas.Fit (gridBefore);
    const PatchAtlasAllocation floorBefore = *atlas.Find (gridBefore.spans[0].key);
    // Find wallB's key by element name.
    PatchKey wallBKey;
    for (const PatchSampleSpan& span : gridBefore.spans) {
        if (span.key.element == "wallB")
            wallBKey = span.key;
    }
    const PatchAtlasAllocation wallBBefore = *atlas.Find (wallBKey);

    // wallA moves; floor and wallB are byte-identical.
    Scene after;
    AddQuad (after, "floor", 0, 0, 12, 8, 0);
    AddQuad (after, "wallA", 7, 0, 8, 8, 2);
    AddQuad (after, "wallB", 30, 0, 31, 8, 2);
    const PatchSampleGrid gridAfter = SampleOf (after, 1.0);

    const PatchAtlasUpdate update = atlas.Fit (gridAfter);
    EXPECT_GE (update.retained, 2u) << "the floor and the far wall must have kept their tiles";
    EXPECT_EQ (update.removed, 1u) << "the moved wall's old key must be retired";
    EXPECT_EQ (update.added, 1u) << "and its new key allocated";
    EXPECT_FALSE (update.resized);

    // ⚠️ THE ADDRESSES, NOT JUST THE COUNTS. A retained count that came with
    // moved rectangles would be worse than useless.
    const PatchAtlasAllocation* floorNow = atlas.Find (gridBefore.spans[0].key);
    ASSERT_NE (floorNow, nullptr);
    EXPECT_EQ (floorNow->x, floorBefore.x);
    EXPECT_EQ (floorNow->y, floorBefore.y);
    EXPECT_EQ (floorNow->generation, floorBefore.generation);

    const PatchAtlasAllocation* wallBNow = atlas.Find (wallBKey);
    ASSERT_NE (wallBNow, nullptr);
    EXPECT_EQ (wallBNow->x, wallBBefore.x);
    EXPECT_EQ (wallBNow->y, wallBBefore.y);
}

TEST (PatchAtlas, OnePatchScattersIntoItsOwnRectangleAndNowhereElse)
{
    // ⚠️ THE MECHANISM THAT MAKES A PARTIAL GPU UPLOAD POSSIBLE. If scattering one
    // patch touched a texel outside its rectangle, uploading that rectangle
    // alone would lose the write -- and the picture would be right in the places
    // that were re-uploaded and stale everywhere else.
    Scene scene;
    AddQuad (scene, "a", 0, 0, 4, 3, 0);
    AddQuad (scene, "b", 10, 0, 13, 5, 0);
    const PatchSampleGrid grid = SampleOf (scene, 1.0);

    SunStudyPatchAtlas atlas;
    atlas.Fit (grid);
    std::vector<float> image (atlas.TexelCount (), -1.0f);

    const std::vector<double> hours (grid.spans[0].count, 4.0);
    ASSERT_TRUE (atlas.ScatterPatch (grid, 0, hours, image));

    const PatchAtlasAllocation* mine = atlas.Find (grid.spans[0].key);
    const PatchAtlasAllocation* other = atlas.Find (grid.spans[1].key);
    ASSERT_NE (mine, nullptr);
    ASSERT_NE (other, nullptr);

    for (size_t texel = 0; texel < image.size (); ++texel) {
        if (image[texel] < 0.0f)
            continue;
        const uint32_t x = static_cast<uint32_t> (texel % atlas.Width ());
        const uint32_t y = static_cast<uint32_t> (texel / atlas.Width ());
        EXPECT_GE (x, mine->x);
        EXPECT_LT (x, mine->x + mine->width);
        EXPECT_GE (y, mine->y);
        EXPECT_LT (y, mine->y + mine->height);
    }

    // The other patch's rectangle is untouched -- still every texel a sentinel.
    std::vector<float> otherRectangle;
    ASSERT_TRUE (atlas.ReadRectangle (*other, image, otherRectangle));
    for (const float value : otherRectangle)
        EXPECT_LT (value, 0.0f) << "scattering one patch wrote into another's tile";
}

TEST (PatchAtlas, TheRectangleReadBackIsWhatAPartialUploadWouldSend)
{
    Scene scene;
    AddQuad (scene, "a", 0, 0, 4, 3, 0);
    const PatchSampleGrid grid = SampleOf (scene, 1.0);

    SunStudyPatchAtlas atlas;
    atlas.Fit (grid);
    std::vector<float> image (atlas.TexelCount (), -1.0f);
    std::vector<double> hours (grid.spans[0].count);
    for (size_t i = 0; i < hours.size (); ++i)
        hours[i] = double (i);
    ASSERT_TRUE (atlas.ScatterPatch (grid, 0, hours, image));

    const PatchAtlasAllocation* allocation = atlas.Find (grid.spans[0].key);
    ASSERT_NE (allocation, nullptr);
    std::vector<float> rectangle;
    ASSERT_TRUE (atlas.ReadRectangle (*allocation, image, rectangle));
    EXPECT_EQ (rectangle.size (), size_t (allocation->width) * allocation->height);

    // Every hour written is somewhere in the rectangle, and the rectangle is all
    // this patch needs to upload.
    for (const double hour : hours) {
        bool present = false;
        for (const float value : rectangle)
            present = present || std::fabs (value - float (hour)) < 1e-6f;
        EXPECT_TRUE (present) << "hour " << hour << " is outside the patch's own rectangle";
    }
}
