// Tests for ArchViz/SunStudyOverlay — the correspondence between a rendered
// triangle and the atlas tile it reads.
//
// ⚠️ EVERY FAILURE THIS FILE CATCHES RENDERS AS A PLAUSIBLE STUDY. A face that
// reads its neighbour's tile is a smooth, continuous, entirely wrong heat map; a
// gutter texel drawn as zero is a shadow that is not there; a side buffer built
// through the wrong permutation tints the right building with the wrong
// surfaces. None of them looks like an error in a viewport, so none of them can
// be found by looking at one.

#include <cmath>
#include <vector>

#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "gtest/gtest.h"

using namespace geomsrv::archviz;
using evp::sunstudy::BuildSampleGrid;
using evp::sunstudy::BuildSunStudyAtlas;
using evp::sunstudy::SampleGrid;
using evp::sunstudy::SamplerOptions;
using evp::sunstudy::ScatterToAtlas;
using evp::sunstudy::SunStudyAtlas;

namespace {

// THE FIXTURE THE TASK ASKED FOR: two triangles, two atlas tiles, on a plane
// where every expected number can be worked out by hand.
//
// A 4 m square in z = 0, split along the 0-2 diagonal. At 1 m spacing each
// triangle spans a 4x4 cell lattice, so each gets its own 4x4 tile and the two
// tiles are necessarily disjoint.
struct Quad {
    std::vector<double> vertices { 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0 };
    std::vector<uint32_t> triangles { 0, 1, 2, 0, 2, 3 };
};

SampleGrid SampleQuad (const Quad& quad, double spacing)
{
    SamplerOptions options;
    options.spacing = spacing;
    // ⚠️ NO LIFT. The sampler normally pushes each sample off the surface along
    // its normal so a ray does not start on the face it belongs to; here the
    // points are being mapped back INTO the face's own plane, and a 1 cm offset
    // along z would be testing the lift rather than the mapping.
    options.normalOffset = 0.0;
    options.wantLayouts = true;
    return BuildSampleGrid (quad.vertices.data (), quad.vertices.size () / 3, quad.triangles.data (),
                            quad.triangles.size () / 3, nullptr, options);
}

} // namespace

// ---------------------------------------------------------------------------
// the count contract
// ---------------------------------------------------------------------------

TEST (SunStudyOverlay, OneRecordPerRenderedTriangle)
{
    const Quad quad;
    const SampleGrid grid = SampleQuad (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    SunStudyElementMap map;
    ASSERT_TRUE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 1.0, quad.triangles, std::vector<int32_t> { 0, 0 },
                                          0, map));

    // The index buffer is 3 per triangle, and the side buffer is indexed by
    // SV_PrimitiveID — so this is the equality the render thread checks before
    // it binds anything.
    EXPECT_EQ (map.faces.size (), quad.triangles.size () / 3);
}

// ---------------------------------------------------------------------------
// the mapping itself, checked against the table the hours were scattered through
// ---------------------------------------------------------------------------

TEST (SunStudyOverlay, EverySampleMapsBackToItsOwnTexel)
{
    const Quad quad;
    const SampleGrid grid = SampleQuad (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);
    ASSERT_GT (grid.Count (), 0u);

    SunStudyElementMap map;
    ASSERT_TRUE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 1.0, quad.triangles, std::vector<int32_t> { 0, 0 },
                                          0, map));

    // One material, so the permutation is the identity and render triangle i is
    // source face i. The permuted case is its own test below.
    for (size_t i = 0; i < grid.Count (); ++i) {
        const uint32_t face = grid.faces[i];
        const double point[3] = { grid.positions[i * 3 + 0], grid.positions[i * 3 + 1], grid.positions[i * 3 + 2] };
        const int64_t texel = SunStudyTexelAt (map.faces[face], point, atlas.width, atlas.height);
        // ⚠️ AGAINST atlas.texels, NOT AGAINST A SECOND COPY OF THE FORMULA.
        // `texels[i]` is where sample i's hours were actually written, so this
        // asserts the shader would read the value the engine stored — which is
        // the entire claim the display path makes.
        EXPECT_EQ (texel, static_cast<int64_t> (atlas.texels[i])) << "sample " << i << " on face " << face;
    }
}

TEST (SunStudyOverlay, NeighbouringFacesNeverShareATexel)
{
    const Quad quad;
    const SampleGrid grid = SampleQuad (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    SunStudyElementMap map;
    ASSERT_TRUE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 1.0, quad.triangles, std::vector<int32_t> { 0, 0 },
                                          0, map));
    ASSERT_EQ (map.faces.size (), 2u);

    // The two triangles share the whole 0-2 diagonal, and in the RENDER mesh
    // they share their vertices outright — which is why the side buffer is per
    // triangle. Walk a dense lattice over the square and require that whichever
    // face is asked, the texel lands inside THAT face's tile.
    for (int fi = 0; fi < 2; ++fi) {
        const SunFaceMap& face = map.faces[fi];
        const int64_t tileX = static_cast<int64_t> (face.tile[0]);
        const int64_t tileY = static_cast<int64_t> (face.tile[1]);
        const int64_t tileW = static_cast<int64_t> (face.tile[2]);
        const int64_t tileH = static_cast<int64_t> (face.tile[3]);
        ASSERT_GT (tileW, 0);
        for (int ix = 0; ix <= 40; ++ix) {
            for (int iy = 0; iy <= 40; ++iy) {
                const double point[3] = { ix * 0.1, iy * 0.1, 0.0 };
                const int64_t texel = SunStudyTexelAt (face, point, atlas.width, atlas.height);
                ASSERT_GE (texel, 0);
                const int64_t x = texel % atlas.width;
                const int64_t y = texel / atlas.width;
                EXPECT_GE (x, tileX);
                EXPECT_LT (x, tileX + tileW);
                EXPECT_GE (y, tileY);
                EXPECT_LT (y, tileY + tileH);
            }
        }
    }
}

TEST (SunStudyOverlay, PointsOutsideTheFaceClampInsideItsOwnTile)
{
    // The clamp is what stops a corner that rounds a hair past its last cell
    // from reading the GUTTER, which is filled with the sentinel and would draw
    // as a one-texel unshaded seam along every element edge.
    const Quad quad;
    const SampleGrid grid = SampleQuad (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    SunStudyElementMap map;
    ASSERT_TRUE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 1.0, quad.triangles, std::vector<int32_t> { 0, 0 },
                                          0, map));

    const SunFaceMap& face = map.faces[0];
    const double far[3] = { 1000.0, 1000.0, 0.0 };
    const double behind[3] = { -1000.0, -1000.0, 0.0 };
    const int64_t hi = SunStudyTexelAt (face, far, atlas.width, atlas.height);
    const int64_t lo = SunStudyTexelAt (face, behind, atlas.width, atlas.height);
    ASSERT_GE (hi, 0);
    ASSERT_GE (lo, 0);
    EXPECT_EQ (lo % atlas.width, static_cast<int64_t> (face.tile[0]));
    EXPECT_EQ (hi % atlas.width, static_cast<int64_t> (face.tile[0] + face.tile[2] - 1));
}

// ---------------------------------------------------------------------------
// the permutation
// ---------------------------------------------------------------------------

TEST (SunStudyOverlay, FaceRecordsFollowTheMaterialReordering)
{
    // ⚠️ THE FAILURE THIS PINS IS INVISIBLE. BuildMaterialGroups sorts the
    // triangles by material before upload, so SV_PrimitiveID counts them in the
    // SORTED order while the study's faces are in SOURCE order. Building the
    // side buffer in source order swaps the two triangles' tiles here — and on
    // a real element swaps hundreds of them, which renders as a complete study
    // of a building whose surfaces have been shuffled.
    const Quad quad;
    const SampleGrid grid = SampleQuad (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    SunStudyElementMap identity;
    ASSERT_TRUE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 1.0, quad.triangles, std::vector<int32_t> { 0, 0 },
                                          0, identity));

    // Materials {1, 0} sort to {0, 1}, so render triangle 0 is SOURCE face 1.
    SunStudyElementMap swapped;
    ASSERT_TRUE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 1.0, quad.triangles, std::vector<int32_t> { 1, 0 },
                                          0, swapped));

    EXPECT_EQ (swapped.faces[0].tile[0], identity.faces[1].tile[0]);
    EXPECT_EQ (swapped.faces[0].tile[1], identity.faces[1].tile[1]);
    EXPECT_EQ (swapped.faces[1].tile[0], identity.faces[0].tile[0]);
    EXPECT_EQ (swapped.faces[1].tile[1], identity.faces[0].tile[1]);
    // The two tiles really are different, or the assertions above pass vacuously.
    EXPECT_NE (identity.faces[0].tile[0] + identity.faces[0].tile[1] * 4096.0f,
               identity.faces[1].tile[0] + identity.faces[1].tile[1] * 4096.0f);
}

TEST (SunStudyOverlay, IndexHashSeparatesTwoExtractionsOfOneElement)
{
    const Quad quad;
    const uint64_t hash = MeshIndexHash (quad.triangles);

    EXPECT_EQ (hash, MeshIndexHash (quad.triangles));
    // A different triangulation of the same square.
    EXPECT_NE (hash, MeshIndexHash (std::vector<uint32_t> { 0, 1, 3, 1, 2, 3 }));
    // A truncated buffer that shares a prefix.
    EXPECT_NE (hash, MeshIndexHash (std::vector<uint32_t> { 0, 1, 2 }));
}

TEST (SunStudyOverlay, TheSideCarCarriesTheHashOfTheBufferTheViewerWouldDraw)
{
    // ⚠️ THE HASH IS OF THE REORDERED INDICES, so the same triangles under
    // different materials produce DIFFERENT hashes -- which is the point: the
    // permutation is what SV_PrimitiveID counts through, and a side buffer built
    // through one permutation is wrong for the other.
    const Quad quad;
    const SampleGrid grid = SampleQuad (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    SunStudyElementMap identity;
    ASSERT_TRUE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 1.0, quad.triangles, std::vector<int32_t> { 0, 0 },
                                          0, identity));
    SunStudyElementMap swapped;
    ASSERT_TRUE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 1.0, quad.triangles, std::vector<int32_t> { 1, 0 },
                                          0, swapped));

    EXPECT_EQ (identity.topologyHash, MeshIndexHash (quad.triangles));
    EXPECT_NE (swapped.topologyHash, identity.topologyHash);
    // A refused build leaves no hash to match against, so a caller cannot pair a
    // half-built side car with an element by accident.
    SunStudyElementMap refused;
    EXPECT_FALSE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 0.0, quad.triangles,
                                           std::vector<int32_t> { 0, 0 }, 0, refused));
    EXPECT_EQ (refused.topologyHash, 0u);
}

// ---------------------------------------------------------------------------
// refusals
// ---------------------------------------------------------------------------

TEST (SunStudyOverlay, ElementOutsideTheStudyIsRefused)
{
    const Quad quad;
    const SampleGrid grid = SampleQuad (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    SunStudyElementMap map;
    // faceBase past the end of the study's face list: the caller paired this
    // element with a study that never measured it.
    EXPECT_FALSE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 1.0, quad.triangles,
                                           std::vector<int32_t> { 0, 0 }, 5, map));
    EXPECT_TRUE (map.faces.empty ());

    // An invalid atlas is refused whatever the geometry says.
    const SunStudyAtlas empty;
    EXPECT_FALSE (BuildSunStudyElementMap (empty.tiles, grid.layouts, 1.0, quad.triangles,
                                           std::vector<int32_t> { 0, 0 }, 0, map));
    // And so is a spacing that cannot describe a lattice.
    EXPECT_FALSE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 0.0, quad.triangles,
                                           std::vector<int32_t> { 0, 0 }, 0, map));
}

TEST (SunStudyOverlay, AnUnplacedFaceCarriesNoTile)
{
    // A face below the grid emits only its centroid and has no cell lattice, so
    // the atlas never places it. Its record must read as "no sample here" — the
    // fall-through to ordinary shading — rather than as tile (0,0).
    const SunFaceMap unplaced;
    const double point[3] = { 0.0, 0.0, 0.0 };
    EXPECT_EQ (SunStudyTexelAt (unplaced, point, 256, 256), -1);
}

// ---------------------------------------------------------------------------
// the sentinel, which is the whole reason the atlas is not zero-filled
// ---------------------------------------------------------------------------

TEST (SunStudyOverlay, ZeroHoursIsNotTheEmptySentinel)
{
    const Quad quad;
    const SampleGrid grid = SampleQuad (quad, 1.0);
    const SunStudyAtlas atlas = BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);

    // Every sample measured nought hours — a north-facing wall in December is
    // exactly this, and it is a RESULT.
    const std::vector<double> hours (grid.Count (), 0.0);
    const std::vector<float> image = ScatterToAtlas (atlas, hours, -1.0f);
    ASSERT_EQ (image.size (), atlas.TexelCount ());

    SunStudyElementMap map;
    ASSERT_TRUE (BuildSunStudyElementMap (atlas.tiles, grid.layouts, 1.0, quad.triangles, std::vector<int32_t> { 0, 0 },
                                          0, map));

    size_t sentinels = 0;
    for (float texel : image) {
        if (texel < 0.0f)
            ++sentinels;
    }
    // The gutters and the cells that fell outside their triangle: the atlas is
    // deliberately far from full, so these must exist or the test proves nothing.
    EXPECT_GT (sentinels, 0u);

    for (size_t i = 0; i < grid.Count (); ++i) {
        const uint32_t face = grid.faces[i];
        const double point[3] = { grid.positions[i * 3 + 0], grid.positions[i * 3 + 1], grid.positions[i * 3 + 2] };
        const int64_t texel = SunStudyTexelAt (map.faces[face], point, atlas.width, atlas.height);
        ASSERT_GE (texel, 0);
        // The shader's test is `texel < 0`, so a measured zero must survive
        // transport as exactly 0 and never as the sentinel.
        EXPECT_FLOAT_EQ (image[static_cast<size_t> (texel)], 0.0f);
    }
}

// ---------------------------------------------------------------------------
// binding a study to the scene as it stands
//
// ⚠️ THESE ARE THE LIFECYCLE REGRESSIONS, AND THEY MATTER MORE THAN MORE
// SAMPLER TESTS. On 2026-09-15 a study was shown five times with a 3D view open
// and no tint ever appeared: the study named elements the scene had not received
// yet, the binder skipped them silently, and the next full batch threw the whole
// payload away. Every case below is one step of that story.
// ---------------------------------------------------------------------------

namespace {

SunStudyElementMap MakeMap (const char* guid, size_t faces, uint64_t hash)
{
    SunStudyElementMap map;
    map.guid = guid;
    map.faces.resize (faces);
    map.topologyHash = hash;
    return map;
}

} // namespace

TEST (SunStudyOverlay, AnElementTheSceneHasNotReceivedYetIsNotARefusal)
{
    const SunStudyElementMap map = MakeMap ("a", 4, 99);
    SceneElementFacts absent; // present = false
    // ⚠️ THE DISTINCTION THE LIVE FAILURE TURNED ON. A study pushed before the
    // extraction that carries its elements names elements the scene does not
    // hold; that bind has to happen LATER, and calling it a refusal sends a
    // reader hunting for a model mismatch that is not there.
    EXPECT_EQ (ClassifySunFaceBinding (map, absent), SunFaceBinding::NotYetReceived);

    // An element that arrives later, unchanged, binds.
    SceneElementFacts arrived;
    arrived.present = true;
    arrived.triangleCount = 4;
    arrived.topologyHash = 99;
    EXPECT_EQ (ClassifySunFaceBinding (map, arrived), SunFaceBinding::Attach);
}

TEST (SunStudyOverlay, AnAlreadyBoundElementIsNotRebuilt)
{
    const SunStudyElementMap map = MakeMap ("a", 4, 99);
    SceneElementFacts bound;
    bound.present = true;
    bound.alreadyBound = true;
    bound.triangleCount = 4;
    bound.topologyHash = 99;
    // ⚠️ AN EndBatch THAT CHANGED NOTHING MUST COST NOTHING. Without this answer
    // every live-sync edit rebuilds one GPU buffer per element of the study.
    EXPECT_EQ (ClassifySunFaceBinding (map, bound), SunFaceBinding::AlreadyBound);
}

TEST (SunStudyOverlay, AReExtractedElementIsRefusedEvenWhenAlreadyBound)
{
    const SunStudyElementMap map = MakeMap ("a", 4, 99);

    // Same triangle count, different geometry or material order.
    SceneElementFacts rehashed;
    rehashed.present = true;
    rehashed.alreadyBound = true;
    rehashed.triangleCount = 4;
    rehashed.topologyHash = 1234;
    // ⚠️ THE ORDER OF THE CHECKS IS THE TEST. "Already bound" must never win over
    // "this is not the element the study measured", or a re-extraction keeps the
    // previous buffer and every face reads a neighbour's atlas tile -- smooth,
    // continuous and entirely wrong.
    EXPECT_EQ (ClassifySunFaceBinding (map, rehashed), SunFaceBinding::RefusedTopologyHash);

    // A different triangle count is the hard stop: the buffer is indexed by
    // SV_PrimitiveID with no bounds check in hardware.
    SceneElementFacts resized;
    resized.present = true;
    resized.alreadyBound = true;
    resized.triangleCount = 7;
    resized.topologyHash = 99;
    EXPECT_EQ (ClassifySunFaceBinding (map, resized), SunFaceBinding::RefusedTriangleCount);
}

TEST (SunStudyOverlay, AnEmptySideCarBindsNothingAndRefusesNothing)
{
    const SunStudyElementMap empty = MakeMap ("a", 0, 99);
    SceneElementFacts present;
    present.present = true;
    present.triangleCount = 4;
    present.topologyHash = 99;
    EXPECT_EQ (ClassifySunFaceBinding (empty, present), SunFaceBinding::NotYetReceived);
}

TEST (SunStudyOverlay, TheBinderIsDrivenEntirelyByTheHashAndTheCount)
{
    // A study built for one element and offered against another element's facts
    // is refused, whatever the GUIDs say. ⚠️ THE GUID IS HOW THE PAIR IS FOUND,
    // NOT WHAT MAKES IT VALID -- two extractions of one element share a GUID and
    // that is exactly the case that must not bind.
    const SunStudyElementMap a = MakeMap ("same-guid", 4, 0xAAAA);
    SceneElementFacts b;
    b.present = true;
    b.triangleCount = 4;
    b.topologyHash = 0xBBBB;
    EXPECT_EQ (ClassifySunFaceBinding (a, b), SunFaceBinding::RefusedTopologyHash);
}

// ---------------------------------------------------------------------------
// transport: replacement and clear
// ---------------------------------------------------------------------------

TEST (SunStudyOverlay, QueueCarriesAndReplacesAStudy)
{
    SceneCmdQueue& queue = SceneCmdQueue::Get ();
    queue.Clear ();

    auto first = std::make_unique<SunStudyAtlasUpload> ();
    first->studyId = "study-1";
    first->version = 1;
    first->width = 4;
    first->height = 4;
    first->texels = std::make_shared<const std::vector<float>> (16, -1.0f);
    queue.PushSunStudyAtlas (std::move (first));
    EXPECT_EQ (queue.PendingCount (), 1u);
    EXPECT_GT (queue.PendingBytes (), 0u);

    std::vector<SceneCmd> taken = queue.Take (4);
    ASSERT_EQ (taken.size (), 1u);
    EXPECT_EQ (taken[0].type, SceneCmdType::SetSunStudyAtlas);
    ASSERT_NE (taken[0].sunStudy, nullptr);
    EXPECT_EQ (taken[0].sunStudy->studyId, "study-1");
    EXPECT_EQ (queue.PendingBytes (), 0u);

    queue.PushClearSunStudy ();
    taken = queue.Take (4);
    ASSERT_EQ (taken.size (), 1u);
    EXPECT_EQ (taken[0].type, SceneCmdType::ClearSunStudy);
    // ⚠️ A CLEAR CARRIES NO PAYLOAD AT ALL. It must be distinguishable from an
    // upload whose image failed to build, or cancelling a study would leave the
    // last one tinted.
    EXPECT_EQ (taken[0].sunStudy, nullptr);

    queue.Clear ();
}
