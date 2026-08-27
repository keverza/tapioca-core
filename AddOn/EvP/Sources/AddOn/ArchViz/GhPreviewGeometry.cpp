#include "ArchViz/GhPreviewGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

using evp::preview::GhPreviewPrimitive;
using evp::preview::GhPreviewSnapshot;
using evp::grasshopper::protocol::PreviewFlagHighlighted;
using evp::grasshopper::protocol::PreviewFlagSelected;
using evp::grasshopper::protocol::PreviewFlagXRay;
using evp::grasshopper::protocol::PreviewKind;
using evp::preview::PreviewSurface;

namespace {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 At (const std::vector<float>& positions, size_t point)
{
    Vec3 v;
    v.x = positions[point * 3 + 0];
    v.y = positions[point * 3 + 1];
    v.z = positions[point * 3 + 2];
    return v;
}

// The face normal of one triangle, unnormalised length guarded.
//
// ⚠️ COMPUTED HERE WHEN THE PRIMITIVE DID NOT SEND ONE, AND FLAT. Preview meshes
// arrive from a tessellator that may or may not have produced normals, and a
// mesh drawn with no normal at all is a silhouette: every face the same shade,
// which reads as the definition having produced a blob. A flat face normal is
// the honest default -- preview is an instrument, and smoothing a mesh nobody
// asked to be smooth invents surface that is not in the result.
Vec3 FaceNormal (const Vec3& a, const Vec3& b, const Vec3& c)
{
    const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    Vec3 n;
    n.x = uy * vz - uz * vy;
    n.y = uz * vx - ux * vz;
    n.z = ux * vy - uy * vx;
    const float length = std::sqrt (n.x * n.x + n.y * n.y + n.z * n.z);
    if (length <= 1e-12f) {
        // A degenerate triangle. Up rather than zero: a zero normal makes the
        // shading term undefined, and one sliver triangle should not put a black
        // speck in the middle of an otherwise correct preview.
        return Vec3 { 0.0f, 0.0f, 1.0f };
    }
    n.x /= length;
    n.y /= length;
    n.z /= length;
    return n;
}

void AppendMesh (const GhPreviewPrimitive& primitive, uint32_t rgba, GhPreviewBucket& bucket)
{
    const bool haveNormals = primitive.normals.size () == primitive.positions.size ();
    const uint32_t base = (uint32_t) bucket.meshVertices.size ();

    // ⚠️ THE VERTICES ARE COPIED PER PRIMITIVE, NOT SHARED, because the colour
    // is per vertex and two primitives may share nothing but a position. The
    // indices are rebased onto that copy.
    for (size_t point = 0; point * 3 + 2 < primitive.positions.size (); ++point) {
        GhPreviewMeshVertex vertex;
        const Vec3 p = At (primitive.positions, point);
        vertex.x = p.x;
        vertex.y = p.y;
        vertex.z = p.z;
        if (haveNormals) {
            vertex.nx = primitive.normals[point * 3 + 0];
            vertex.ny = primitive.normals[point * 3 + 1];
            vertex.nz = primitive.normals[point * 3 + 2];
        }
        else {
            vertex.nx = 0.0f;
            vertex.ny = 0.0f;
            vertex.nz = 0.0f;
        }
        vertex.rgba = rgba;
        bucket.meshVertices.push_back (vertex);
    }

    const size_t vertexCount = bucket.meshVertices.size () - base;
    for (size_t triangle = 0; triangle * 3 + 2 < primitive.indices.size (); ++triangle) {
        const uint32_t i0 = primitive.indices[triangle * 3 + 0];
        const uint32_t i1 = primitive.indices[triangle * 3 + 1];
        const uint32_t i2 = primitive.indices[triangle * 3 + 2];
        // The protocol already refused an index past the vertex count
        // (ValidatePreviewIndices), so this is the belt to that brace: this file
        // is pure and offline-tested, and a test may hand it anything.
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            continue;

        if (!haveNormals) {
            // Flat shading needs a normal PER TRIANGLE, and vertices are shared
            // between triangles, so the normal is accumulated: a vertex ends up
            // with the average of the faces meeting at it. Not as crisp as
            // splitting every triangle into three unshared vertices, and a third
            // of the memory -- which on a two-million-triangle preview is the
            // difference that matters.
            const Vec3 n = FaceNormal (At (primitive.positions, i0), At (primitive.positions, i1),
                                       At (primitive.positions, i2));
            for (const uint32_t index : { i0, i1, i2 }) {
                GhPreviewMeshVertex& vertex = bucket.meshVertices[base + index];
                vertex.nx += n.x;
                vertex.ny += n.y;
                vertex.nz += n.z;
            }
        }

        bucket.meshIndices.push_back (base + i0);
        bucket.meshIndices.push_back (base + i1);
        bucket.meshIndices.push_back (base + i2);
    }

    if (!haveNormals) {
        for (size_t index = base; index < bucket.meshVertices.size (); ++index) {
            GhPreviewMeshVertex& vertex = bucket.meshVertices[index];
            const float length =
                std::sqrt (vertex.nx * vertex.nx + vertex.ny * vertex.ny + vertex.nz * vertex.nz);
            if (length <= 1e-12f) {
                vertex.nz = 1.0f;
                continue;
            }
            vertex.nx /= length;
            vertex.ny /= length;
            vertex.nz /= length;
        }
    }
}

// One segment -> two triangles, each corner carrying the other endpoint so the
// vertex shader can find the direction ON SCREEN rather than in world space.
void AppendSegment (const Vec3& a, const Vec3& b, uint32_t rgba, GhPreviewBucket& bucket)
{
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    // A zero-length segment has no direction to be perpendicular to, and guessing
    // one draws a line that is not in the definition.
    if (dx * dx + dy * dy + dz * dz <= 1e-20f)
        return;

    const auto corner = [&] (const Vec3& here, const Vec3& other, float side, float cap) {
        GhPreviewLineVertex vertex;
        vertex.x = here.x;
        vertex.y = here.y;
        vertex.z = here.z;
        vertex.ox = other.x;
        vertex.oy = other.y;
        vertex.oz = other.z;
        vertex.side = side;
        vertex.cap = cap;
        vertex.rgba = rgba;
        return vertex;
    };

    const GhPreviewLineVertex a0 = corner (a, b, -1.0f, -1.0f);
    const GhPreviewLineVertex a1 = corner (a, b, +1.0f, -1.0f);
    const GhPreviewLineVertex b0 = corner (b, a, -1.0f, +1.0f);
    const GhPreviewLineVertex b1 = corner (b, a, +1.0f, +1.0f);

    // ⚠️ b0 AND b1 CARRY `a` AS THEIR OTHER ENDPOINT, so the shader's direction
    // at that end points backwards. `cap` already says which end this is, and the
    // shader negates accordingly; the alternative -- both corners naming the same
    // ordered pair -- means one of them projects a direction from a point it is
    // not at, which goes wrong exactly where the perspective divide differs most,
    // near the camera.
    bucket.lineVertices.push_back (a0);
    bucket.lineVertices.push_back (a1);
    bucket.lineVertices.push_back (b1);

    bucket.lineVertices.push_back (a0);
    bucket.lineVertices.push_back (b1);
    bucket.lineVertices.push_back (b0);
}

void AppendPolyline (const GhPreviewPrimitive& primitive, uint32_t rgba, GhPreviewBucket& bucket)
{
    const size_t points = primitive.positions.size () / 3;
    if (points < 2)
        return;

    for (size_t index = 0; index + 1 < points; ++index)
        AppendSegment (At (primitive.positions, index), At (primitive.positions, index + 1), rgba, bucket);

    // The closing segment is a segment like any other. It is NOT inferred from
    // the first and last points being equal: a closed curve whose ends coincide
    // and an open one that happens to return to its start are different results,
    // and only the definition knows which it produced.
    if (primitive.closed)
        AppendSegment (At (primitive.positions, points - 1), At (primitive.positions, 0), rgba, bucket);
}

} // namespace

uint32_t GhPreviewColour (uint8_t flags, const GhPreviewStyle& style)
{
    if ((flags & PreviewFlagSelected) != 0)
        return style.selectedRgba;
    if ((flags & PreviewFlagHighlighted) != 0)
        return style.highlightedRgba;
    return style.rgba;
}

GhPreviewDrawables BuildGhPreviewDrawables (const GhPreviewSnapshot& snapshot, PreviewSurface surface,
                                            const GhPreviewStyle& style, const GhPreviewLimits& limits)
{
    GhPreviewDrawables drawables;

    // ⚠️ SORTED BY id. The snapshot's primitives come out of an unordered map, so
    // their order differs between runs and can differ between frames; with any
    // transparency in the style that is a flicker nobody would trace back to a
    // hash seed.
    std::vector<const GhPreviewPrimitive*> ordered;
    ordered.reserve (snapshot.primitives.size ());
    for (const auto& primitive : snapshot.primitives) {
        if (primitive == nullptr)
            continue;
        if (!primitive->DrawnOn (surface))
            continue;
        // Invisible costs nothing to draw, rather than costing a transparent
        // fragment: toggling a component's preview off is meant to be free.
        if (!primitive->Visible ())
            continue;
        ordered.push_back (primitive.get ());
    }
    std::sort (ordered.begin (), ordered.end (),
               [] (const GhPreviewPrimitive* left, const GhPreviewPrimitive* right) { return left->id < right->id; });

    for (const GhPreviewPrimitive* primitive : ordered) {
        const uint32_t rgba = GhPreviewColour (primitive->flags, style);
        GhPreviewBucket& bucket =
            (primitive->flags & PreviewFlagXRay) != 0 ? drawables.xray : drawables.depthTested;

        switch (primitive->kind) {
            case PreviewKind::TriangleMesh:
                if (drawables.depthTested.meshVertices.size () + drawables.xray.meshVertices.size () +
                        primitive->positions.size () / 3 >
                    limits.maxMeshVertices) {
                    drawables.truncated = true;
                    continue;
                }
                AppendMesh (*primitive, rgba, bucket);
                break;

            case PreviewKind::Polyline3D: {
                const size_t points = primitive->positions.size () / 3;
                const size_t segments = points < 2 ? 0 : (primitive->closed ? points : points - 1);
                if (drawables.depthTested.lineVertices.size () + drawables.xray.lineVertices.size () + segments * 6 >
                    limits.maxLineVertices) {
                    drawables.truncated = true;
                    continue;
                }
                AppendPolyline (*primitive, rgba, bucket);
                break;
            }

            case PreviewKind::BillboardText:
            case PreviewKind::WorldText: {
                // Collected, not drawn. See the header: this renderer has no text
                // capability yet, and a private one invented inside the preview
                // layer is the one that gets thrown away when a real one lands.
                if (primitive->positions.size () < 3)
                    break;
                GhPreviewLabel label;
                const Vec3 anchor = At (primitive->positions, 0);
                label.x = anchor.x;
                label.y = anchor.y;
                label.z = anchor.z;
                label.rgba = rgba;
                label.billboard = primitive->kind == PreviewKind::BillboardText;
                label.text = primitive->text;
                drawables.labels.push_back (std::move (label));
                break;
            }

            default:
                // PointMarker, PlaneGizmo, Arrow3D, Bounds, PointCloud and
                // BillboardSprite: the cheap host-built set, and the long tail.
                // Counted so the viewport can say they were not drawn.
                ++drawables.deferredKinds;
                break;
        }
    }

    return drawables;
}

} // namespace archviz
} // namespace geomsrv
