#include "ArchViz/ViewpointOverlayGeometry.hpp"

#include <cmath>

namespace geomsrv::archviz {

namespace {

constexpr size_t kGuideSegments = 180;
constexpr double kRunTickMetres = 0.25; // half-height of the mark at each end of a Context run

void Segment (const std::array<double, 3>& a, const std::array<double, 3>& b, uint32_t rgba, GhPreviewBucket& bucket)
{
    const float from[3] = { float (a[0]), float (a[1]), float (a[2]) };
    const float to[3] = { float (b[0]), float (b[1]), float (b[2]) };
    AppendPreviewSegment (from, to, rgba, bucket);
}

GhPreviewMeshVertex Up (const std::array<double, 3>& at, uint32_t rgba)
{
    GhPreviewMeshVertex vertex;
    vertex.x = float (at[0]);
    vertex.y = float (at[1]);
    vertex.z = float (at[2]);
    vertex.nx = 0.0f;
    vertex.ny = 0.0f;
    vertex.nz = 1.0f;
    vertex.rgba = rgba;
    return vertex;
}

} // namespace

GhPreviewDrawables BuildViewpointOverlay (const std::array<double, 3>& point, double radius,
                                          const evp::sunstudy::ViewpointStudyResult* result,
                                          const ViewpointOverlayStyle& style)
{
    GhPreviewDrawables drawables;
    GhPreviewBucket& bucket = drawables.depthTested;
    // The full circle is a radius guide, distinct from the clipped visibility boundary.
    for (size_t i = 0; i < kGuideSegments; ++i) {
        std::array<double, 3> ends[2];
        for (size_t end = 0; end < 2; ++end) {
            const double angle = 6.28318530717958647692 * double (i + end) / double (kGuideSegments);
            ends[end] = { point[0] + radius * std::cos (angle), point[1] + radius * std::sin (angle), point[2] };
        }
        Segment (ends[0], ends[1], style.guideRgba, bucket);
    }
    if (result == nullptr || !result->valid || result->perimeter.size () < 3)
        return drawables;

    // A fan about the point keeps a concave isovist concave.
    const auto& perimeter = result->perimeter;
    const size_t count = perimeter.size ();
    bucket.meshVertices.push_back (Up (result->options.point, style.fillRgba));
    for (const auto& vertex : perimeter)
        bucket.meshVertices.push_back (Up (vertex, style.fillRgba));
    for (size_t i = 0; i < count; ++i) {
        bucket.meshIndices.push_back (0);
        bucket.meshIndices.push_back (uint32_t (1 + i));
        bucket.meshIndices.push_back (uint32_t (1 + (i + 1) % count));
    }

    // An edge between two stopped rays lies on Context: draw it as a collision.
    const auto& rayMesh = result->rayMesh;
    const bool rays = rayMesh.size () == count;
    for (size_t i = 0; i < count; ++i) {
        const size_t next = (i + 1) % count;
        const bool blocked = rays && rayMesh[i] >= 0 && rayMesh[next] >= 0;
        Segment (perimeter[i], perimeter[next], blocked ? style.blockedRgba : style.openEdgeRgba, bucket);
    }
    // Mark where each Context run starts and ends: the edges of what blocks the view.
    if (rays)
        for (const auto& arc : evp::sunstudy::ViewpointArcs (rayMesh)) {
            if (arc.mesh < 0 || arc.count == count)
                continue;
            for (const size_t ray : { arc.first, (arc.first + arc.count - 1) % count }) {
                auto low = perimeter[ray], high = perimeter[ray];
                low[2] -= kRunTickMetres;
                high[2] += kRunTickMetres;
                Segment (low, high, style.blockedRgba, bucket);
            }
        }
    return drawables;
}

} // namespace geomsrv::archviz
