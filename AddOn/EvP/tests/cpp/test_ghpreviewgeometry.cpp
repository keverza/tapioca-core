// ArchViz/GhPreviewGeometry.cpp — a preview snapshot turned into drawables.
//
// Everything here fails as a PICTURE rather than as an error, which is why it is
// pure and tested instead of looked at in Archicad:
//
//   * a primitive routed to the wrong surface draws plan linework in the 3D
//     window, and the author hunts their definition rather than a surface byte;
//   * an invisible primitive that still reaches a buffer makes "preview off"
//     cost a full upload and a full pass, which is the opposite of the promise;
//   * an unordered buffer draws the same geometry in a different order each
//     frame, and with any transparency that is a flicker nobody would trace back
//     to a hash seed;
//   * a mesh with no normals shaded with a zero normal is a silhouette, which
//     reads as the definition having produced a blob;
//   * an x-ray primitive in the depth-tested bucket is simply invisible inside
//     the wall it exists to be compared against.
//
// None of the five produces a log line, and all five look like the definition's
// fault.

#include "ArchViz/GhPreviewGeometry.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

using namespace geomsrv::archviz;
using namespace evp::preview;
using evp::grasshopper::protocol::PreviewFlagDepthTest;
using evp::grasshopper::protocol::PreviewFlagHighlighted;
using evp::grasshopper::protocol::PreviewFlagSelected;
using evp::grasshopper::protocol::PreviewFlagVisible;
using evp::grasshopper::protocol::PreviewFlagXRay;
using evp::grasshopper::protocol::PreviewKind;

namespace {

std::shared_ptr<GhPreviewPrimitive> Mesh (uint64_t id, PreviewSurface surface, uint8_t flags, bool withNormals)
{
    auto primitive = std::make_shared<GhPreviewPrimitive> ();
    primitive->id = id;
    primitive->kind = PreviewKind::TriangleMesh;
    primitive->surface = surface;
    primitive->flags = flags;
    // One triangle in the XY plane, so its face normal is +Z and a test can say
    // what the shading normal must be.
    primitive->positions = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    primitive->indices = { 0, 1, 2 };
    if (withNormals)
        primitive->normals = { 0, 0, 1, 0, 0, 1, 0, 0, 1 };
    return primitive;
}

std::shared_ptr<GhPreviewPrimitive> Line (uint64_t id, PreviewSurface surface, uint8_t flags, bool closed)
{
    auto primitive = std::make_shared<GhPreviewPrimitive> ();
    primitive->id = id;
    primitive->kind = PreviewKind::Polyline3D;
    primitive->surface = surface;
    primitive->flags = flags;
    primitive->closed = closed;
    primitive->positions = { 0, 0, 0, 1, 0, 0, 1, 1, 0 };
    return primitive;
}

std::shared_ptr<GhPreviewPrimitive> Text (uint64_t id, PreviewKind kind, const std::string& text)
{
    auto primitive = std::make_shared<GhPreviewPrimitive> ();
    primitive->id = id;
    primitive->kind = kind;
    primitive->surface = PreviewSurface::Model3D;
    primitive->flags = PreviewFlagVisible;
    primitive->positions = { 3, 4, 5 };
    primitive->text = text;
    return primitive;
}

std::shared_ptr<GhPreviewPrimitive> OfKind (uint64_t id, PreviewKind kind)
{
    auto primitive = std::make_shared<GhPreviewPrimitive> ();
    primitive->id = id;
    primitive->kind = kind;
    primitive->surface = PreviewSurface::Model3D;
    primitive->flags = PreviewFlagVisible;
    primitive->positions = { 0, 0, 0 };
    return primitive;
}

GhPreviewSnapshot Snapshot (std::vector<std::shared_ptr<GhPreviewPrimitive>> primitives)
{
    GhPreviewSnapshot snapshot;
    for (auto& primitive : primitives)
        snapshot.primitives.push_back (primitive);
    return snapshot;
}

const GhPreviewStyle kStyle;
const GhPreviewLimits kNoLimit;

} // namespace

// ---------------------------------------------------------------------------
// Which surface
// ---------------------------------------------------------------------------

// ⚠️ THE 3D WINDOW AND THE FLOOR PLAN ARE NOT ONE SURFACE. A `FloorPlan`
// primitive reaching the 3D layer draws plan linework in the model, and the
// author would hunt their geometry rather than a surface byte.
TEST (GhPreviewGeometry, EachLayerReadsOnlyItsOwnSurfaceAndBothAnswersToEither)
{
    const GhPreviewSnapshot snapshot = Snapshot ({
        Mesh (1, PreviewSurface::Model3D, PreviewFlagVisible, true),
        Mesh (2, PreviewSurface::FloorPlan, PreviewFlagVisible, true),
        Mesh (3, PreviewSurface::Both, PreviewFlagVisible, true),
    });

    const GhPreviewDrawables model =
        BuildGhPreviewDrawables (snapshot, PreviewSurface::Model3D, kStyle, kNoLimit);
    const GhPreviewDrawables plan =
        BuildGhPreviewDrawables (snapshot, PreviewSurface::FloorPlan, kStyle, kNoLimit);

    // One triangle each from the surface's own primitive plus the Both one.
    EXPECT_EQ (model.depthTested.meshIndices.size (), 6u);
    EXPECT_EQ (plan.depthTested.meshIndices.size (), 6u);
}

// ---------------------------------------------------------------------------
// Visibility, order and colour
// ---------------------------------------------------------------------------

// Toggling a component's preview off is meant to be free. An alpha-zero triangle
// still costs an upload and a fragment, so an invisible primitive must not reach
// a buffer at all.
TEST (GhPreviewGeometry, AnInvisiblePrimitiveProducesNoGeometryAtAll)
{
    const GhPreviewDrawables drawables = BuildGhPreviewDrawables (
        Snapshot ({ Mesh (1, PreviewSurface::Model3D, 0, true), Line (2, PreviewSurface::Model3D, 0, false) }),
        PreviewSurface::Model3D, kStyle, kNoLimit);

    EXPECT_TRUE (drawables.depthTested.Empty ());
    EXPECT_TRUE (drawables.xray.Empty ());
}

// The snapshot's primitives come out of an unordered map. Two builds of the same
// snapshot must produce the same buffer, or a transparent style flickers.
TEST (GhPreviewGeometry, TheBufferIsOrderedByIdRatherThanByMapOrder)
{
    auto first = Line (30, PreviewSurface::Model3D, PreviewFlagVisible, false);
    auto second = Line (10, PreviewSurface::Model3D, PreviewFlagVisible, false);
    auto third = Line (20, PreviewSurface::Model3D, PreviewFlagVisible, false);
    // Distinct colours are the only way to observe the order in the buffer.
    first->flags |= PreviewFlagSelected;
    third->flags |= PreviewFlagHighlighted;

    const GhPreviewDrawables a =
        BuildGhPreviewDrawables (Snapshot ({ first, second, third }), PreviewSurface::Model3D, kStyle, kNoLimit);
    const GhPreviewDrawables b =
        BuildGhPreviewDrawables (Snapshot ({ third, first, second }), PreviewSurface::Model3D, kStyle, kNoLimit);

    ASSERT_EQ (a.depthTested.lineVertices.size (), b.depthTested.lineVertices.size ());
    ASSERT_FALSE (a.depthTested.lineVertices.empty ());
    for (size_t index = 0; index < a.depthTested.lineVertices.size (); ++index)
        EXPECT_EQ (a.depthTested.lineVertices[index].rgba, b.depthTested.lineVertices[index].rgba);

    // id 10 is unselected, so the first vertices carry the ordinary colour.
    EXPECT_EQ (a.depthTested.lineVertices.front ().rgba, kStyle.rgba);
}

// Selection is the thing the user is doing right now, so it wins.
TEST (GhPreviewGeometry, SelectionOutranksHighlightWhichOutranksOrdinary)
{
    EXPECT_EQ (GhPreviewColour (PreviewFlagVisible, kStyle), kStyle.rgba);
    EXPECT_EQ (GhPreviewColour (PreviewFlagVisible | PreviewFlagHighlighted, kStyle), kStyle.highlightedRgba);
    EXPECT_EQ (GhPreviewColour (PreviewFlagVisible | PreviewFlagSelected, kStyle), kStyle.selectedRgba);
    EXPECT_EQ (GhPreviewColour (PreviewFlagVisible | PreviewFlagSelected | PreviewFlagHighlighted, kStyle),
               kStyle.selectedRgba);
}

// ---------------------------------------------------------------------------
// X-ray is a pipeline, not a colour
// ---------------------------------------------------------------------------

// An x-ray primitive exists to be seen INSIDE the wall it is compared against.
// Left in the depth-tested bucket it is simply invisible, which looks exactly
// like the definition having produced nothing.
TEST (GhPreviewGeometry, XRayGeometryGoesToItsOwnBucket)
{
    const GhPreviewDrawables drawables = BuildGhPreviewDrawables (
        Snapshot ({ Mesh (1, PreviewSurface::Model3D, PreviewFlagVisible | PreviewFlagDepthTest, true),
                    Mesh (2, PreviewSurface::Model3D, PreviewFlagVisible | PreviewFlagXRay, true) }),
        PreviewSurface::Model3D, kStyle, kNoLimit);

    EXPECT_EQ (drawables.depthTested.meshIndices.size (), 3u);
    EXPECT_EQ (drawables.xray.meshIndices.size (), 3u);
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

TEST (GhPreviewGeometry, SentNormalsAreUsedUnchanged)
{
    const GhPreviewDrawables drawables = BuildGhPreviewDrawables (
        Snapshot ({ Mesh (1, PreviewSurface::Model3D, PreviewFlagVisible, true) }), PreviewSurface::Model3D,
        kStyle, kNoLimit);

    ASSERT_EQ (drawables.depthTested.meshVertices.size (), 3u);
    EXPECT_FLOAT_EQ (drawables.depthTested.meshVertices[0].nz, 1.0f);
}

// A mesh drawn with a zero normal is one flat shade all over -- a silhouette,
// which reads as the definition having produced a blob rather than as a missing
// attribute.
TEST (GhPreviewGeometry, AMeshWithNoNormalsGetsFlatFaceNormalsRatherThanZero)
{
    const GhPreviewDrawables drawables = BuildGhPreviewDrawables (
        Snapshot ({ Mesh (1, PreviewSurface::Model3D, PreviewFlagVisible, false) }), PreviewSurface::Model3D,
        kStyle, kNoLimit);

    ASSERT_EQ (drawables.depthTested.meshVertices.size (), 3u);
    for (const GhPreviewMeshVertex& vertex : drawables.depthTested.meshVertices) {
        const float length = std::sqrt (vertex.nx * vertex.nx + vertex.ny * vertex.ny + vertex.nz * vertex.nz);
        EXPECT_NEAR (length, 1.0f, 1e-5f);
        // The triangle lies in XY, so its face normal is +/-Z.
        EXPECT_NEAR (std::fabs (vertex.nz), 1.0f, 1e-5f);
    }
}

// Two primitives must not share vertices: the colour is per vertex, and the
// second primitive's indices have to be rebased onto its own copy or it draws
// the first one's triangles.
TEST (GhPreviewGeometry, IndicesAreRebasedOntoEachPrimitivesOwnVertices)
{
    const GhPreviewDrawables drawables = BuildGhPreviewDrawables (
        Snapshot ({ Mesh (1, PreviewSurface::Model3D, PreviewFlagVisible, true),
                    Mesh (2, PreviewSurface::Model3D, PreviewFlagVisible | PreviewFlagSelected, true) }),
        PreviewSurface::Model3D, kStyle, kNoLimit);

    ASSERT_EQ (drawables.depthTested.meshVertices.size (), 6u);
    ASSERT_EQ (drawables.depthTested.meshIndices.size (), 6u);
    // The second primitive's triangle indexes 3,4,5 rather than 0,1,2.
    EXPECT_EQ (drawables.depthTested.meshIndices[3], 3u);
    EXPECT_EQ (drawables.depthTested.meshVertices[3].rgba, kStyle.selectedRgba);
    EXPECT_EQ (drawables.depthTested.meshVertices[0].rgba, kStyle.rgba);
}

// ---------------------------------------------------------------------------
// Curves
// ---------------------------------------------------------------------------

TEST (GhPreviewGeometry, EachSegmentBecomesSixVerticesAndClosedAddsOneMore)
{
    const GhPreviewDrawables open = BuildGhPreviewDrawables (
        Snapshot ({ Line (1, PreviewSurface::Model3D, PreviewFlagVisible, false) }), PreviewSurface::Model3D,
        kStyle, kNoLimit);
    const GhPreviewDrawables closed = BuildGhPreviewDrawables (
        Snapshot ({ Line (1, PreviewSurface::Model3D, PreviewFlagVisible, true) }), PreviewSurface::Model3D,
        kStyle, kNoLimit);

    // Three points: two segments open, three closed.
    EXPECT_EQ (open.depthTested.lineVertices.size (), 12u);
    EXPECT_EQ (closed.depthTested.lineVertices.size (), 18u);
}

// ⚠️ THE ANTIALIASING. This target has no MSAA, so `side` has to carry the signed
// position across the line for the pixel shader to fade the outermost pixel. A
// quad whose corners are all one side is a zero-width line; all the same sign is
// the mistake that produces one.
TEST (GhPreviewGeometry, EverySegmentQuadSpansBothSidesOfItsCentreline)
{
    const GhPreviewDrawables drawables = BuildGhPreviewDrawables (
        Snapshot ({ Line (1, PreviewSurface::Model3D, PreviewFlagVisible, false) }), PreviewSurface::Model3D,
        kStyle, kNoLimit);

    ASSERT_EQ (drawables.depthTested.lineVertices.size (), 12u);
    for (size_t triangle = 0; triangle < 4; ++triangle) {
        float minSide = 1.0f;
        float maxSide = -1.0f;
        for (size_t corner = 0; corner < 3; ++corner) {
            const float side = drawables.depthTested.lineVertices[triangle * 3 + corner].side;
            EXPECT_TRUE (side == -1.0f || side == +1.0f);
            minSide = (std::min) (minSide, side);
            maxSide = (std::max) (maxSide, side);
        }
        EXPECT_FLOAT_EQ (minSide, -1.0f);
        EXPECT_FLOAT_EQ (maxSide, +1.0f);
    }
}

// The corner at the far end of a segment names the NEAR end as its other point,
// so the shader's direction is computed from where that corner actually is. The
// alternative goes wrong exactly where the perspective divide differs most.
TEST (GhPreviewGeometry, EachCornerCarriesTheOppositeEndOfItsOwnSegment)
{
    const GhPreviewDrawables drawables = BuildGhPreviewDrawables (
        Snapshot ({ Line (1, PreviewSurface::Model3D, PreviewFlagVisible, false) }), PreviewSurface::Model3D,
        kStyle, kNoLimit);

    ASSERT_FALSE (drawables.depthTested.lineVertices.empty ());
    for (const GhPreviewLineVertex& vertex : drawables.depthTested.lineVertices) {
        const bool atStart = vertex.cap < 0.0f;
        EXPECT_TRUE (atStart ? vertex.cap == -1.0f : vertex.cap == +1.0f);
        // The two ends of a segment are never the same point.
        EXPECT_FALSE (vertex.x == vertex.ox && vertex.y == vertex.oy && vertex.z == vertex.oz);
    }
}

// A zero-length segment has no direction to be perpendicular to, and guessing
// one draws a line that is not in the definition.
TEST (GhPreviewGeometry, ARepeatedPointContributesNoQuad)
{
    auto line = Line (1, PreviewSurface::Model3D, PreviewFlagVisible, false);
    line->positions = { 0, 0, 0, 0, 0, 0, 1, 0, 0 };

    const GhPreviewDrawables drawables =
        BuildGhPreviewDrawables (Snapshot ({ line }), PreviewSurface::Model3D, kStyle, kNoLimit);

    EXPECT_EQ (drawables.depthTested.lineVertices.size (), 6u);
}

// ---------------------------------------------------------------------------
// Text and the kinds that are not built yet
// ---------------------------------------------------------------------------

// ⚠️ COLLECTED, NOT DRAWN AND NOT DROPPED. This renderer has no text capability
// yet; a label the layer cannot draw has to be reportable, because "my text
// shows nothing" and "Tapioca cannot draw text yet" are the same symptom and
// different problems.
TEST (GhPreviewGeometry, TextBecomesALabelWithItsAnchorAndFacingRatherThanGeometry)
{
    const GhPreviewDrawables drawables = BuildGhPreviewDrawables (
        Snapshot ({ Text (1, PreviewKind::BillboardText, "42.0 m2"), Text (2, PreviewKind::WorldText, "north") }),
        PreviewSurface::Model3D, kStyle, kNoLimit);

    ASSERT_EQ (drawables.LabelCount (), 2u);
    EXPECT_TRUE (drawables.depthTested.Empty ());
    EXPECT_EQ (drawables.labels[0].text, "42.0 m2");
    EXPECT_TRUE (drawables.labels[0].billboard);
    EXPECT_FLOAT_EQ (drawables.labels[0].x, 3.0f);
    EXPECT_EQ (drawables.labels[1].text, "north");
    EXPECT_FALSE (drawables.labels[1].billboard);
}

// The cheap host-built set and the long tail. Counted so the viewport can say
// they were not drawn rather than showing nothing and explaining nothing.
TEST (GhPreviewGeometry, KindsThisBuildDoesNotDrawYetAreCountedRatherThanDropped)
{
    const GhPreviewDrawables drawables = BuildGhPreviewDrawables (
        Snapshot ({ OfKind (1, PreviewKind::PointMarker), OfKind (2, PreviewKind::PlaneGizmo),
                    OfKind (3, PreviewKind::Arrow3D), OfKind (4, PreviewKind::Bounds),
                    OfKind (5, PreviewKind::PointCloud), OfKind (6, PreviewKind::BillboardSprite) }),
        PreviewSurface::Model3D, kStyle, kNoLimit);

    EXPECT_EQ (drawables.deferredKinds, 6u);
    EXPECT_TRUE (drawables.depthTested.Empty ());
    EXPECT_EQ (drawables.LabelCount (), 0u);
}

// ---------------------------------------------------------------------------
// Ceilings
// ---------------------------------------------------------------------------

// ⚠️ REFUSED WHILE BUILDING, NOT TRIMMED AFTERWARDS. Building the lot and then
// cutting it down has already paid the cost the ceiling exists to refuse.
TEST (GhPreviewGeometry, ACeilingStopsTheBuildAndSaysSo)
{
    GhPreviewLimits limits;
    limits.maxMeshVertices = 4; // one triangle fits; the second does not

    const GhPreviewDrawables drawables = BuildGhPreviewDrawables (
        Snapshot ({ Mesh (1, PreviewSurface::Model3D, PreviewFlagVisible, true),
                    Mesh (2, PreviewSurface::Model3D, PreviewFlagVisible, true) }),
        PreviewSurface::Model3D, kStyle, limits);

    EXPECT_TRUE (drawables.truncated);
    EXPECT_EQ (drawables.depthTested.meshVertices.size (), 3u);
}

TEST (GhPreviewGeometry, AnEmptySnapshotIsOrdinaryAndSilent)
{
    const GhPreviewDrawables drawables =
        BuildGhPreviewDrawables (GhPreviewSnapshot (), PreviewSurface::Model3D, kStyle, kNoLimit);

    EXPECT_TRUE (drawables.depthTested.Empty ());
    EXPECT_TRUE (drawables.xray.Empty ());
    EXPECT_EQ (drawables.deferredKinds, 0u);
    EXPECT_FALSE (drawables.truncated);
}
