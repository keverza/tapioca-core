#include "Geometry/SurfaceAttributeMap.hpp"
#include "MeshFixtures.hpp"
#include <gtest/gtest.h>

namespace {
using namespace geomsrv;
namespace ex = geomsrv::exposed;

Snapshot Capture ()
{
    Snapshot snapshot;
    snapshot.id = 81;
    snapshot.captureStamp = 4;
    snapshot.surfaceAttributes = { { 101, { "Finish A", "attribute-A" } },
                                   { 202, { "Finish B", "attribute-B" } },
                                   { 303, { "Unused", "attribute-C" } } };
    snapshot.materialIdentity = { { 7, { "pool A", 909 } }, { 8, { "pool B", 808 } }, { 9, { "generated", -1 } } };
    Mesh mesh = evptest::MakeBox ("wall", 0, 0, 0, 5, .2, 3);
    mesh.triSourceBody.assign (12, 1);
    mesh.triSourcePgon.assign (12, 1);
    mesh.triMaterial.assign (12, 7);
    mesh.triSurfaceAttribute.assign (12, 101);
    for (size_t i = 2; i < 12; ++i) {
        mesh.triSourcePgon[i] = static_cast<int32_t> (i / 2 + 1);
        mesh.triMaterial[i] = 8;
        mesh.triSurfaceAttribute[i] = 202;
    }
    snapshot.meshes.push_back (std::move (mesh));
    return snapshot;
}

const std::vector<ex::FaceKey> kFaces { { { 0, 1 }, 1 }, { { 0, 1 }, 2 } };

void Refuses (const Snapshot& snapshot, const std::vector<ex::FaceKey>& faces,
              const std::optional<std::set<int32_t>>& filter, const std::string& reason)
{
    ex::SurfaceSelection selection;
    selection.poolToSurface[999] = 999;
    selection.materialPoolFilter = { 999 };
    std::string error;
    EXPECT_FALSE (ex::ResolveSurfaceSelection (snapshot, faces, filter, selection, error));
    EXPECT_NE (error.find (reason), std::string::npos) << error;
    EXPECT_TRUE (selection.poolToSurface.empty ());
    EXPECT_FALSE (selection.materialPoolFilter.has_value ());
}
} // namespace

TEST (SurfaceAttributeMap, UsesOriginalPolygonSurfaceNotPoolNameOrExternalReference)
{
    const auto snapshot = Capture ();
    ex::SurfaceSelection selection;
    std::string error;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, kFaces, std::set<int32_t> { 101 }, selection, error)) << error;
    EXPECT_EQ (selection.poolToSurface.at (7), 101);
    EXPECT_EQ (selection.poolToSurface.at (8), 202);
    ASSERT_TRUE (selection.materialPoolFilter.has_value ());
    EXPECT_EQ (*selection.materialPoolFilter, std::set<int32_t> { 7 });
    EXPECT_NE (selection.poolToSurface.at (7), snapshot.materialIdentity.at (7).externalReference);
}

TEST (SurfaceAttributeMap, MissingEmptyAndUnusedFiltersHaveDistinctExplicitSemantics)
{
    const auto snapshot = Capture ();
    ex::SurfaceSelection selection;
    std::string error;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, kFaces, std::nullopt, selection, error)) << error;
    EXPECT_FALSE (selection.materialPoolFilter.has_value ());
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, kFaces, std::set<int32_t> {}, selection, error)) << error;
    ASSERT_TRUE (selection.materialPoolFilter.has_value ());
    EXPECT_TRUE (selection.materialPoolFilter->empty ());
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, kFaces, std::set<int32_t> { 303 }, selection, error)) << error;
    EXPECT_TRUE (selection.materialPoolFilter->empty ());
}

TEST (SurfaceAttributeMap, SeveralPoolEntriesForOneSurfaceSelectAllMatchingFragments)
{
    auto snapshot = Capture ();
    snapshot.meshes[0].triSurfaceAttribute.assign (12, 101);
    ex::SurfaceSelection selection;
    std::string error;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, kFaces, std::set<int32_t> { 101 }, selection, error)) << error;
    EXPECT_EQ (*selection.materialPoolFilter, (std::set<int32_t> { 7, 8 }));
    ex::Request request;
    request.snapshotId = snapshot.id;
    request.captureStamp = snapshot.captureStamp;
    request.coverScopeId = "explicit-empty-covers";
    request.faces = kFaces;
    request.materialPoolFilter = selection.materialPoolFilter;
    ex::Result result;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    EXPECT_NEAR (result.areas.selectedExposedArea, 2, 1e-7); // top and bottom, 5 * .2 each
    EXPECT_DOUBLE_EQ (result.areas.selectedExposedArea, result.areas.exposedArea);
    for (const auto& fragment : result.fragments) {
        EXPECT_TRUE (fragment.selected);
        EXPECT_EQ (selection.poolToSurface.at (fragment.materialPoolIndex), 101);
    }
}

TEST (SurfaceAttributeMap, DifferentProjectSurfacesCanBeSelectedTogether)
{
    const auto snapshot = Capture ();
    ex::SurfaceSelection selection;
    std::string error;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, kFaces, std::set<int32_t> { 101, 202 }, selection, error))
        << error;
    EXPECT_EQ (*selection.materialPoolFilter, (std::set<int32_t> { 7, 8 }));
}

TEST (SurfaceAttributeMap, PoolRenumberingDoesNotChangeProjectSelection)
{
    auto snapshot = Capture ();
    std::swap (snapshot.materialIdentity[7], snapshot.materialIdentity[8]);
    for (auto& pool : snapshot.meshes[0].triMaterial)
        pool = pool == 7 ? 8 : 7;
    ex::SurfaceSelection selection;
    std::string error;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, kFaces, std::set<int32_t> { 101 }, selection, error)) << error;
    EXPECT_EQ (*selection.materialPoolFilter, std::set<int32_t> { 8 });
}

TEST (SurfaceAttributeMap, ConflictingOriginalsWithinOnePoolRefuseWithoutPartialMapping)
{
    auto snapshot = Capture ();
    snapshot.meshes[0].triMaterial.assign (12, 7);
    Refuses (snapshot, kFaces, std::nullopt, "ambiguous");
}

TEST (SurfaceAttributeMap, MixedAssignmentWithinOneSourceFaceRefuses)
{
    auto snapshot = Capture ();
    snapshot.meshes[0].triSurfaceAttribute[1] = 202;
    Refuses (snapshot, kFaces, std::nullopt, "conflicting");
}

TEST (SurfaceAttributeMap, GeneratedUnknownSurfaceIsVisibleButNeverGuessedForFiltering)
{
    auto snapshot = Capture ();
    snapshot.meshes[0].triSurfaceAttribute[0] = snapshot.meshes[0].triSurfaceAttribute[1] = 0;
    ex::SurfaceSelection selection;
    std::string error;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, kFaces, std::nullopt, selection, error)) << error;
    EXPECT_EQ (selection.poolToSurface.at (7), 0);
    Refuses (snapshot, kFaces, std::set<int32_t> { 101 }, "no original project surface");
    Refuses (snapshot, kFaces, std::set<int32_t> {}, "no original project surface");
}

TEST (SurfaceAttributeMap, OldSerializedCapturesCannotInventProvenance)
{
    auto snapshot = Capture ();
    snapshot.meshes[0].triSurfaceAttribute.clear ();
    Refuses (snapshot, kFaces, std::nullopt, "sidecars");
    snapshot = Capture ();
    snapshot.surfaceAttributes.clear ();
    Refuses (snapshot, kFaces, std::nullopt, "catalog is absent");
    snapshot = Capture ();
    snapshot.materialIdentity.clear ();
    Refuses (snapshot, kFaces, std::nullopt, "pool identity");
}

TEST (SurfaceAttributeMap, StaleAndNegativeAttributeIndicesAreNotModelPoolFallbacks)
{
    auto snapshot = Capture ();
    Refuses (snapshot, kFaces, std::set<int32_t> { 7 }, "catalog");
    Refuses (snapshot, kFaces, std::set<int32_t> { -1 }, "catalog");
    snapshot.meshes[0].triSurfaceAttribute[0] = 999;
    Refuses (snapshot, kFaces, std::nullopt, "catalog");
}

TEST (SurfaceAttributeMap, MissingAndInvalidFaceKeysRefuse)
{
    const auto snapshot = Capture ();
    Refuses (snapshot, { { { 0, 1 }, 99 } }, std::nullopt, "face is absent");
    Refuses (snapshot, { { { 2, 1 }, 1 } }, std::nullopt, "Invalid");
    Refuses (snapshot, { { { 0, 0 }, 1 } }, std::nullopt, "Invalid");
}

TEST (SurfaceAttributeMap, UnrelatedUnknownAndMalformedFacesDoNotPoisonExplicitCandidates)
{
    auto snapshot = Capture ();
    snapshot.meshes[0].triSurfaceAttribute[4] = -1;
    snapshot.meshes[0].triSurfaceAttribute[5] = 999;
    ex::SurfaceSelection selection;
    std::string error;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, kFaces, std::set<int32_t> { 101 }, selection, error)) << error;
    EXPECT_EQ (selection.poolToSurface.size (), 2u);
}

TEST (SurfaceAttributeMap, DuplicateFaceRequestsDoNotMultiplyMappingOrScanBudgets)
{
    const auto snapshot = Capture ();
    auto faces = kFaces;
    faces.insert (faces.end (), kFaces.begin (), kFaces.end ());
    ex::SurfaceSelection selection;
    std::string error;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, faces, std::nullopt, selection, error)) << error;
    EXPECT_EQ (selection.poolToSurface.size (), 2u);
    faces.resize (513, faces.front ());
    Refuses (snapshot, faces, std::nullopt, "face budget");
}

TEST (SurfaceAttributeMap, OversizedInputRefusesBeforeScanningOrAllocatingResult)
{
    auto snapshot = Capture ();
    snapshot.meshes[0].triangles.resize (100001 * 3);
    Refuses (snapshot, kFaces, std::nullopt, "input triangle budget");
}

TEST (SurfaceAttributeMap, OriginalSurfaceFilteringSelectsOnlyTheCorrespondingEmittedGeometry)
{
    const auto snapshot = Capture ();
    ex::SurfaceSelection selection;
    std::string error;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, kFaces, std::set<int32_t> { 101 }, selection, error)) << error;
    ex::Request request;
    request.snapshotId = snapshot.id;
    request.captureStamp = snapshot.captureStamp;
    request.coverScopeId = "explicit-empty-covers";
    request.faces = kFaces;
    request.materialPoolFilter = selection.materialPoolFilter;
    ex::Result result;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    EXPECT_NEAR (result.areas.exposedArea, 2, 1e-7);
    EXPECT_NEAR (result.areas.selectedExposedArea, 1, 1e-7);
    double selectedArea = 0;
    for (const auto& fragment : result.fragments) {
        const int32_t surface = selection.poolToSurface.at (fragment.materialPoolIndex);
        EXPECT_EQ (fragment.selected, surface == 101);
        if (fragment.selected) {
            selectedArea += fragment.areaM2;
            for (size_t i = 2; i < fragment.points.size (); i += 3)
                EXPECT_DOUBLE_EQ (fragment.points[i], 0); // only the bottom plane
        }
    }
    EXPECT_DOUBLE_EQ (selectedArea, result.areas.selectedExposedArea);
}
