#include "MeshFixtures.hpp"
#include "SunStudy/SunStudySurfaceSampling.hpp"
#include "SunStudy/SunStudyReuse.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include <set>

using namespace evp::sunstudy;
using namespace evptest;

namespace {

std::shared_ptr<geomsrv::Snapshot> Scene ()
{
    auto scene = std::make_shared<geomsrv::Snapshot> (
        MakeSnapshot ({ MakeBox ("near", 0, 0, 0, 4, 3, 1), MakeBox ("wall", 6, 0, 0, 1, 8, 4),
                        MakeBox ("far", 70, 0, 0, 4, 3, 1) }));
    // Include a proved inward solid; reused winding must match the fresh proof.
    for (size_t face = 0; face < scene->meshes[1].triangles.size (); face += 3)
        std::swap (scene->meshes[1].triangles[face + 1], scene->meshes[1].triangles[face + 2]);
    return scene;
}

SurfaceSamplingResult WholeScene (const geomsrv::Snapshot& scene, const std::vector<uint8_t>& mask,
                                  const SurfaceSamplingOptions& settings,
                                  const std::vector<std::vector<uint8_t>>& faceMasks = {})
{
    SurfaceSamplingResult result;
    std::vector<double> vertices;
    std::vector<uint32_t> triangles, groups;
    std::vector<std::string> elements;
    std::vector<uint8_t> faces;
    for (size_t mesh = 0; mesh < scene.meshes.size (); ++mesh) {
        const auto& item = scene.meshes[mesh];
        const uint32_t base = static_cast<uint32_t> (vertices.size () / 3);
        vertices.insert (vertices.end (), item.vertices.begin (), item.vertices.end ());
        for (const auto vertex : item.triangles)
            triangles.push_back (base + vertex);
        groups.resize (triangles.size () / 3, static_cast<uint32_t> (mesh));
        elements.push_back (item.guid);
        if (!faceMasks.empty ()) {
            if (faceMasks[mesh].empty ())
                faces.insert (faces.end (), item.TriangleCount (), 1);
            else
                faces.insert (faces.end (), faceMasks[mesh].begin (), faceMasks[mesh].end ());
        }
    }
    const auto oriented = OrientOutward (vertices.data (), vertices.size () / 3, triangles.data (),
                                         triangles.size () / 3, groups.data (), result.winding);
    if (settings.domain == SamplingDomain::SurfacePatch) {
        PatchSamplerOptions options;
        options.spacing = settings.spacing;
        options.normalOffset = settings.normalOffset;
        options.maxSamples = settings.maxSamples;
        options.sampleGroup = &mask;
        options.sampleFace = faceMasks.empty () ? nullptr : &faces;
        result.patches = BuildPatchSampleGrid (vertices.data (), vertices.size () / 3, oriented.data (),
                                               oriented.size () / 3, groups.data (), elements, options);
        result.valid = result.patches.valid;
    }
    else {
        SamplerOptions options;
        options.spacing = settings.spacing;
        options.normalOffset = settings.normalOffset;
        options.maxSamples = settings.maxSamples;
        options.jitter = settings.jitter;
        options.wantLayouts = true;
        options.sampleGroup = &mask;
        options.sampleFace = faceMasks.empty () ? nullptr : &faces;
        result.triangles = BuildSampleGrid (vertices.data (), vertices.size () / 3, oriented.data (),
                                            oriented.size () / 3, groups.data (), options);
        result.valid = result.triangles.valid;
    }
    return result;
}

void ExpectSame (const SurfaceSamplingResult& actual, const SurfaceSamplingResult& expected)
{
    ASSERT_EQ (actual.valid, expected.valid);
    if (!expected.valid)
        return;
    EXPECT_EQ (actual.winding.groups, expected.winding.groups);
    EXPECT_EQ (actual.winding.closed, expected.winding.closed);
    EXPECT_EQ (actual.winding.flipped, expected.winding.flipped);
    EXPECT_EQ (actual.winding.flippedTriangles, expected.winding.flippedTriangles);
    const auto& a = actual.triangles;
    const auto& b = expected.triangles;
    EXPECT_EQ (a.valid, b.valid);
    EXPECT_EQ (a.positions, b.positions);
    EXPECT_EQ (a.normals, b.normals);
    EXPECT_EQ (a.areas, b.areas);
    EXPECT_EQ (a.faces, b.faces);
    EXPECT_EQ (a.groups, b.groups);
    EXPECT_EQ (a.cellColumns, b.cellColumns);
    EXPECT_EQ (a.cellRows, b.cellRows);
    EXPECT_EQ (a.degenerateFaces, b.degenerateFaces);
    EXPECT_EQ (a.undersizedFaces, b.undersizedFaces);
    EXPECT_EQ (a.excludedFaces, b.excludedFaces);
    ASSERT_EQ (a.layouts.size (), b.layouts.size ());
    for (size_t face = 0; face < a.layouts.size (); ++face) {
        const auto& x = a.layouts[face];
        const auto& y = b.layouts[face];
        EXPECT_TRUE (std::equal (x.origin, x.origin + 3, y.origin));
        EXPECT_TRUE (std::equal (x.uAxis, x.uAxis + 3, y.uAxis));
        EXPECT_TRUE (std::equal (x.vAxis, x.vAxis + 3, y.vAxis));
        EXPECT_EQ (x.uStart, y.uStart);
        EXPECT_EQ (x.vStart, y.vStart);
        EXPECT_EQ (x.columns, y.columns);
        EXPECT_EQ (x.rows, y.rows);
        EXPECT_EQ (x.gridded, y.gridded);
    }
    const auto& p = actual.patches;
    const auto& q = expected.patches;
    EXPECT_EQ (p.valid, q.valid);
    EXPECT_EQ (p.positions, q.positions);
    EXPECT_EQ (p.normals, q.normals);
    EXPECT_EQ (p.areas, q.areas);
    EXPECT_EQ (p.cellColumns, q.cellColumns);
    EXPECT_EQ (p.cellRows, q.cellRows);
    EXPECT_EQ (p.spanOf, q.spanOf);
    EXPECT_EQ (p.patchOfTriangle, q.patchOfTriangle);
    EXPECT_EQ (p.degenerateFaces, q.degenerateFaces);
    EXPECT_EQ (p.centroidPatches, q.centroidPatches);
    EXPECT_EQ (p.excludedPatches, q.excludedPatches);
    ASSERT_EQ (p.spans.size (), q.spans.size ());
    for (size_t patch = 0; patch < p.spans.size (); ++patch) {
        const auto& x = p.spans[patch];
        const auto& y = q.spans[patch];
        EXPECT_EQ (x.key, y.key);
        EXPECT_EQ (x.first, y.first);
        EXPECT_EQ (x.count, y.count);
        EXPECT_EQ (x.columns, y.columns);
        EXPECT_EQ (x.rows, y.rows);
        EXPECT_EQ (x.area, y.area);
        EXPECT_TRUE (std::equal (x.origin, x.origin + 3, y.origin));
        EXPECT_TRUE (std::equal (x.uAxis, x.uAxis + 3, y.uAxis));
        EXPECT_TRUE (std::equal (x.vAxis, x.vAxis + 3, y.vAxis));
        EXPECT_TRUE (std::equal (x.normal, x.normal + 3, y.normal));
        EXPECT_TRUE (std::equal (x.boundsMin, x.boundsMin + 3, y.boundsMin));
        EXPECT_TRUE (std::equal (x.boundsMax, x.boundsMax + 3, y.boundsMax));
    }
}

std::unique_ptr<StudyRecord> Record (std::shared_ptr<const geomsrv::Snapshot> snapshot, SurfaceSamplingResult result)
{
    auto record = std::make_unique<StudyRecord> ();
    record->snapshot = std::move (snapshot);
    record->snapshotId = record->snapshot->id;
    record->domain = result.layout.options.domain;
    record->gridSpacing = result.layout.options.spacing;
    record->samplingLayout = std::move (result.layout);
    record->sampleGrid = std::move (result.triangles);
    record->patchGrid = std::move (result.patches);
    record->positions = record->IsPatchDomain () ? record->patchGrid.positions : record->sampleGrid.positions;
    record->normals = record->IsPatchDomain () ? record->patchGrid.normals : record->sampleGrid.normals;
    return record;
}

class SurfaceSampling : public testing::TestWithParam<SamplingDomain> {
  protected:
    SurfaceSamplingOptions Options () const
    {
        SurfaceSamplingOptions options;
        options.domain = GetParam ();
        options.spacing = 0.75;
        return options;
    }
};

} // namespace

TEST_P (SurfaceSampling, ColdAndUnchangedMaterialOnlyMatchWholeSceneExactly)
{
    auto scene = Scene ();
    const auto options = Options ();
    auto cold = BuildSurfaceSampling (*scene, { 1, 1, 1 }, options);
    ExpectSame (cold, WholeScene (*scene, { 1, 1, 1 }, options));
    EXPECT_EQ (cold.rebuiltMeshes, 3u);
    EXPECT_EQ (cold.reusedMeshes, 0u);
    ASSERT_EQ (cold.winding.flipped, 1u);
    const auto source = Record (scene, std::move (cold));
    auto next = std::make_shared<geomsrv::Snapshot> (*scene);
    next->meshes[0].triMaterial.assign (12, 42);
    next->meshes[0].normals.assign (24, 0.5); // sampling uses geometric normals
    next->meshes[0].bounds = {};              // bounds do not define the grid
    const auto warm = BuildSurfaceSampling (*next, { 1, 1, 1 }, options, source.get ());
    ExpectSame (warm, WholeScene (*next, { 1, 1, 1 }, options));
    EXPECT_EQ (warm.reusedMeshes, 3u);
    EXPECT_EQ (warm.rebuiltMeshes, 0u);
    EXPECT_EQ (warm.generatedSamples, 0u);
    EXPECT_EQ (warm.reusedSamples, source->positions.size () / 3);
}

TEST_P (SurfaceSampling, MoveAddDeleteReorderAndExactSubQuantumEditRebaseAllMappings)
{
    const auto scene = Scene ();
    const auto options = Options ();
    const auto source = Record (scene, BuildSurfaceSampling (*scene, { 1, 1, 1 }, options));
    for (int edit = 0; edit < 6; ++edit) {
        SCOPED_TRACE (edit);
        auto next = *scene;
        if (edit == 0 || edit == 4)
            next.meshes[1].vertices[0] += edit == 0 ? 3.0 : 0.00001;
        if (edit == 1)
            next.meshes.insert (next.meshes.begin (), MakeBox ("added", -10, 0, 0));
        if (edit == 2)
            next.meshes.erase (next.meshes.begin () + 1);
        if (edit == 3)
            std::reverse (next.meshes.begin (), next.meshes.end ());
        if (edit == 5)
            std::swap (next.meshes[1].triangles[0], next.meshes[1].triangles[1]);
        const std::vector<uint8_t> mask (next.meshes.size (), 1);
        const auto result = BuildSurfaceSampling (next, mask, options, source.get ());
        ExpectSame (result, WholeScene (next, mask, options));
        EXPECT_EQ (result.rebuiltMeshes, edit == 2 || edit == 3 ? 0u : 1u);
        EXPECT_EQ (result.reusedMeshes + result.rebuiltMeshes, next.meshes.size ());
        EXPECT_GT (result.reusedSamples, 0u);
        EXPECT_EQ (result.reusedSamples + result.generatedSamples, result.triangles.Count () + result.patches.Count ());
        if (GetParam () == SamplingDomain::SurfacePatch) {
            SunStudyPatchAtlas atlas;
            atlas.Fit (result.patches);
            EXPECT_EQ (atlas.AllocationCount (), result.patches.spans.size ());
            std::set<int64_t> texels;
            for (size_t sample = 0; sample < result.patches.Count (); ++sample) {
                const auto texel = atlas.TexelOf (result.patches, sample);
                EXPECT_GE (texel, 0);
                EXPECT_TRUE (texels.insert (texel).second);
            }
        }
        else {
            const auto atlas = BuildSunStudyAtlas (result.triangles);
            const auto fresh = BuildSunStudyAtlas (WholeScene (next, mask, options).triangles);
            EXPECT_EQ (atlas.texels, fresh.texels);
        }
    }
}

TEST_P (SurfaceSampling, CanonicalGuidRewritesPatchSpellingWithoutResampling)
{
    auto scene = Scene ();
    scene->meshes[1].guid = "{ABC-def}";
    const auto options = Options ();
    const auto source = Record (scene, BuildSurfaceSampling (*scene, { 1, 1, 1 }, options));
    auto next = *scene;
    next.meshes[1].guid = "abc-DEF";
    const auto result = BuildSurfaceSampling (next, { 1, 1, 1 }, options, source.get ());
    ExpectSame (result, WholeScene (next, { 1, 1, 1 }, options));
    EXPECT_EQ (result.reusedMeshes, 3u);
}

TEST_P (SurfaceSampling, RoleExclusionRestorationAndAllExcludedPreserveValidity)
{
    const auto scene = Scene ();
    const auto options = Options ();
    const auto source = Record (scene, BuildSurfaceSampling (*scene, { 0, 1, 1 }, options));
    for (const auto mask : { std::vector<uint8_t> { 0, 1, 1 }, { 1, 1, 1 }, { 0, 0, 1 }, { 0, 0, 0 } }) {
        const auto result = BuildSurfaceSampling (*scene, mask, options, source.get ());
        ExpectSame (result, WholeScene (*scene, mask, options));
        if (result.valid)
            EXPECT_EQ (result.rebuiltMeshes, static_cast<size_t> (mask[0] != 0) + static_cast<size_t> (mask[1] == 0) +
                                                 static_cast<size_t> (mask[2] == 0));
    }
}

TEST_P (SurfaceSampling, EmptyDegenerateAndCentroidMeshesDoNotInvalidateOtherReceivers)
{
    auto scene = Scene ();
    scene->meshes.insert (scene->meshes.begin (), MakeEmpty ());
    scene->meshes.push_back (MakeZeroAreaTriangle ());
    scene->meshes.push_back (MakeSliverTriangle ());
    const auto options = Options ();
    const std::vector<uint8_t> mask (scene->meshes.size (), 1);
    const auto cold = BuildSurfaceSampling (*scene, mask, options);
    ExpectSame (cold, WholeScene (*scene, mask, options));
    const auto source = Record (scene, cold);
    const auto warm = BuildSurfaceSampling (*scene, mask, options, source.get ());
    ExpectSame (warm, WholeScene (*scene, mask, options));
    EXPECT_EQ (warm.reusedMeshes, scene->meshes.size ());
    EXPECT_EQ (warm.generatedSamples, 0u);
}

TEST_P (SurfaceSampling, SettingsChangesRebuildAndGlobalCapRefusesRatherThanTruncates)
{
    const auto scene = Scene ();
    const auto options = Options ();
    const auto source = Record (scene, BuildSurfaceSampling (*scene, { 1, 1, 1 }, options));
    for (int setting = 0; setting < 4; ++setting) {
        auto changed = options;
        if (setting == 0)
            changed.spacing = 1.2;
        if (setting == 1)
            changed.normalOffset = 0.1;
        if (setting == 2)
            changed.jitter = 0.8;
        if (setting == 3)
            changed.domain = GetParam () == SamplingDomain::SurfacePatch ? SamplingDomain::TriangleLegacy
                                                                         : SamplingDomain::SurfacePatch;
        const auto result = BuildSurfaceSampling (*scene, { 1, 1, 1 }, changed, source.get ());
        ExpectSame (result, WholeScene (*scene, { 1, 1, 1 }, changed));
        EXPECT_EQ (result.reusedMeshes, 0u);
    }
    const size_t count = source->positions.size () / 3;
    for (const size_t cap : { size_t (0), count - 1, count, count + 1 }) {
        auto changed = options;
        changed.maxSamples = cap;
        for (const StudyRecord* previous :
             { static_cast<const StudyRecord*> (nullptr), static_cast<const StudyRecord*> (source.get ()) }) {
            const auto result = BuildSurfaceSampling (*scene, { 1, 1, 1 }, changed, previous);
            EXPECT_EQ (result.valid, cap >= count);
            if (cap >= count)
                ExpectSame (result, WholeScene (*scene, { 1, 1, 1 }, changed));
            else {
                EXPECT_EQ (result.triangles.Count (), 0u);
                EXPECT_EQ (result.patches.Count (), 0u);
                EXPECT_FALSE (result.layout.valid);
            }
        }
    }
}

TEST_P (SurfaceSampling, JitterKeepsGlobalFaceIdentityAndOnlyRebuildsChangedSeeds)
{
    const auto scene = Scene ();
    auto options = Options ();
    options.jitter = 0.7;
    const auto source = Record (scene, BuildSurfaceSampling (*scene, { 1, 1, 1 }, options));
    auto next = *scene;
    std::reverse (next.meshes.begin (), next.meshes.end ());
    const auto result = BuildSurfaceSampling (next, { 1, 1, 1 }, options, source.get ());
    ExpectSame (result, WholeScene (next, { 1, 1, 1 }, options));
    EXPECT_EQ (result.reusedMeshes, GetParam () == SamplingDomain::SurfacePatch ? 3u : 1u);
}

TEST_P (SurfaceSampling, AmbiguousGuidsAndMalformedMetadataRebuildWithoutGuessing)
{
    const auto scene = Scene ();
    const auto options = Options ();
    const auto source = Record (scene, BuildSurfaceSampling (*scene, { 1, 1, 1 }, options));
    for (int bad = 0; bad < 7; ++bad) {
        SCOPED_TRACE (bad);
        StudyRecord damaged;
        damaged.snapshot = source->snapshot;
        damaged.domain = source->domain;
        damaged.samplingLayout = source->samplingLayout;
        damaged.sampleGrid = source->sampleGrid;
        damaged.patchGrid = source->patchGrid;
        if (bad == 0)
            damaged.samplingLayout.valid = false;
        if (bad == 1)
            damaged.samplingLayout.meshes.pop_back ();
        if (bad == 2)
            damaged.samplingLayout.meshes[1].firstFace = 0;
        if (bad == 3)
            damaged.samplingLayout.meshes[1].sampleCount = std::numeric_limits<size_t>::max ();
        if (bad == 4) {
            if (damaged.IsPatchDomain ())
                damaged.patchGrid.spanOf.back () = PatchSampleGrid::kNoPatch;
            else
                damaged.sampleGrid.groups.back () = 0;
        }
        if (bad == 5) {
            if (damaged.IsPatchDomain ())
                damaged.patchGrid.positions.pop_back ();
            else
                damaged.sampleGrid.normals.pop_back ();
        }
        if (bad == 6) {
            auto ambiguous = std::make_shared<geomsrv::Snapshot> (*scene);
            ambiguous->meshes[2].guid = "{NEAR}";
            damaged.snapshot = ambiguous;
        }
        const auto result = BuildSurfaceSampling (*scene, { 1, 1, 1 }, options, &damaged);
        ExpectSame (result, WholeScene (*scene, { 1, 1, 1 }, options));
        EXPECT_GT (result.rebuiltMeshes, 0u);
    }
    auto ambiguous = *scene;
    ambiguous.meshes[2].guid = "near";
    const auto result = BuildSurfaceSampling (ambiguous, { 1, 1, 1 }, options, source.get ());
    ExpectSame (result, WholeScene (ambiguous, { 1, 1, 1 }, options));
    EXPECT_FALSE (result.layout.valid);
    EXPECT_EQ (result.reusedMeshes, 0u);
}

TEST_P (SurfaceSampling, CancellationAndMalformedInputsPublishNoPartialGrid)
{
    const auto scene = Scene ();
    const auto options = Options ();
    const auto source = Record (scene, BuildSurfaceSampling (*scene, { 1, 1, 1 }, options));
    for (size_t cancelAt = 1; cancelAt <= 7; ++cancelAt) {
        size_t checks = 0;
        const auto result =
            BuildSurfaceSampling (*scene, { 1, 1, 1 }, options, source.get (), [&] { return ++checks >= cancelAt; });
        EXPECT_FALSE (result.valid);
        EXPECT_FALSE (result.layout.valid);
        EXPECT_EQ (result.triangles.Count (), 0u);
        EXPECT_EQ (result.patches.Count (), 0u);
    }
    for (int bad = 0; bad < 4; ++bad) {
        auto damaged = *scene;
        if (bad == 0)
            damaged.meshes[0].vertices.pop_back ();
        if (bad == 1)
            damaged.meshes[0].triangles.pop_back ();
        if (bad == 2)
            damaged.meshes[0].triangles[0] = UINT32_MAX;
        if (bad == 3)
            damaged.meshes[0].vertices[0] = std::numeric_limits<double>::quiet_NaN ();
        EXPECT_FALSE (BuildSurfaceSampling (damaged, { 1, 1, 1 }, options, source.get ()).valid);
    }
    EXPECT_FALSE (BuildSurfaceSampling (*scene, {}, options).valid);
}

TEST_P (SurfaceSampling, FaceReceiverChangesRebuildOnlyTheirMeshAndNeverMergeIntoContext)
{
    auto scene = Scene ();
    std::vector<std::vector<uint8_t>> faces (3);
    faces[0].assign (12, 0);
    faces[0][2] = faces[0][3] = 1; // top glass; sides and bottom remain context
    const auto options = Options ();
    auto cold = BuildSurfaceSampling (*scene, { 1, 1, 1 }, options, nullptr, {}, faces);
    ExpectSame (cold, WholeScene (*scene, { 1, 1, 1 }, options, faces));
    const auto source = Record (scene, std::move (cold));
    const auto warm = BuildSurfaceSampling (*scene, { 1, 1, 1 }, options, source.get (), {}, faces);
    ExpectSame (warm, WholeScene (*scene, { 1, 1, 1 }, options, faces));
    EXPECT_EQ (warm.reusedMeshes, 3u);
    faces[0][0] = 1;
    const auto changed = BuildSurfaceSampling (*scene, { 1, 1, 1 }, options, source.get (), {}, faces);
    ExpectSame (changed, WholeScene (*scene, { 1, 1, 1 }, options, faces));
    EXPECT_EQ (changed.reusedMeshes, 2u);
    EXPECT_EQ (changed.rebuiltMeshes, 1u);
    faces[0].pop_back ();
    EXPECT_FALSE (BuildSurfaceSampling (*scene, { 1, 1, 1 }, options, source.get (), {}, faces).valid);
}

INSTANTIATE_TEST_SUITE_P (Domains, SurfaceSampling,
                          testing::Values (SamplingDomain::TriangleLegacy, SamplingDomain::SurfacePatch));
