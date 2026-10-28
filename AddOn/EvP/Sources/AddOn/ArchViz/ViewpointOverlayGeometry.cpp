#include "ArchViz/ViewpointOverlayGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz {

namespace {

constexpr size_t kGuideSegments = 180;
constexpr double kRunTickMetres = 0.25; // half-height of the mark at each end of a Context run
constexpr double kPi = 3.14159265358979323846;
// ⚠️ THE SPHERE'S HULL IS INSET TOWARD THE POINT. A ray that stops on Context
// ends exactly ON that surface, so the hull between neighbouring stopped rays
// lies in the wall's own plane and z-fights it. 5 cm, and at most a tenth of
// the ray, keeps it just in front of the surface it describes.
constexpr double kSphereInsetMetres = 0.05;

void Segment (const std::array<double, 3>& a, const std::array<double, 3>& b, uint32_t rgba, GhPreviewBucket& bucket)
{
    const float from[3] = { float (a[0]), float (a[1]), float (a[2]) };
    const float to[3] = { float (b[0]), float (b[1]), float (b[2]) };
    AppendPreviewSegment (from, to, rgba, bucket);
}

GhPreviewMeshVertex Vertex (const std::array<double, 3>& at, const double normal[3], uint32_t rgba)
{
    GhPreviewMeshVertex vertex;
    vertex.x = float (at[0]);
    vertex.y = float (at[1]);
    vertex.z = float (at[2]);
    vertex.nx = float (normal[0]);
    vertex.ny = float (normal[1]);
    vertex.nz = float (normal[2]);
    vertex.rgba = rgba;
    return vertex;
}

GhPreviewMeshVertex Up (const std::array<double, 3>& at, uint32_t rgba)
{
    const double up[3] = { 0.0, 0.0, 1.0 };
    return Vertex (at, up, rgba);
}

// A horizontal arc about `centre` from `from` to `to` radians.
void Arc (const std::array<double, 3>& centre, double radius, double from, double to, uint32_t rgba,
          GhPreviewBucket& bucket)
{
    const size_t segments =
        (std::max) (size_t (2), size_t (std::ceil (double (kGuideSegments) * (to - from) / (2.0 * kPi))));
    for (size_t i = 0; i < segments; ++i) {
        std::array<double, 3> ends[2];
        for (size_t end = 0; end < 2; ++end) {
            const double angle = from + (to - from) * double (i + end) / double (segments);
            ends[end] = { centre[0] + radius * std::cos (angle), centre[1] + radius * std::sin (angle), centre[2] };
        }
        Segment (ends[0], ends[1], rgba, bucket);
    }
}

// The isovist's guide: the full circle, the field of view's arc and its two
// edges, or the sphere's horizon and the rings where its band is cut off.
void Guide (const evp::sunstudy::ViewpointOptions& options, uint32_t rgba, GhPreviewBucket& bucket)
{
    using evp::sunstudy::ViewpointMode;
    const auto& point = options.point;
    const double radius = options.radius;
    if (options.mode == ViewpointMode::FieldOfView) {
        const double from = (options.bearingDegrees - 0.5 * options.fieldOfViewDegrees) * kPi / 180.0;
        const double to = (options.bearingDegrees + 0.5 * options.fieldOfViewDegrees) * kPi / 180.0;
        Arc (point, radius, from, to, rgba, bucket);
        for (const double edge : { from, to })
            Segment (point, { point[0] + radius * std::cos (edge), point[1] + radius * std::sin (edge), point[2] },
                     rgba, bucket);
        return;
    }
    Arc (point, radius, 0.0, 2.0 * kPi, rgba, bucket);
    if (options.mode != ViewpointMode::Sphere)
        return;
    for (const double degrees : { options.minElevationDegrees, options.maxElevationDegrees }) {
        if (std::abs (degrees) >= 89.5)
            continue; // the pole: nothing cut off
        const double e = degrees * kPi / 180.0;
        Arc ({ point[0], point[1], point[2] + radius * std::sin (e) }, radius * std::cos (e), 0.0, 2.0 * kPi, rgba,
             bucket);
    }
}

// The spherical isovist: its ray ends as a closed-in-azimuth grid, each vertex
// coloured by whether its ray reached the radius or stopped on Context.
void Sphere (const evp::sunstudy::ViewpointStudyResult& result, const ViewpointOverlayStyle& style,
             GhPreviewBucket& bucket)
{
    const size_t rows = result.options.sphereRows, columns = result.options.sphereColumns;
    if (rows < 2 || columns < 3 || result.perimeter.size () != rows * columns)
        return;
    const bool rays = result.rayMesh.size () == result.perimeter.size ();
    // The fill's alpha on the blocked colour, so blocked and open read alike in weight.
    const uint32_t blocked = (style.blockedRgba & 0xFFFFFF00u) | (style.fillRgba & 0xFFu);
    const auto& point = result.options.point;
    for (size_t i = 0; i < result.perimeter.size (); ++i) {
        const auto& end = result.perimeter[i];
        double normal[3] = { end[0] - point[0], end[1] - point[1], end[2] - point[2] };
        const double length = std::sqrt (normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        for (double& n : normal)
            n = length > 1.0e-12 ? n / length : 0.0;
        const double reach = length - (std::min) (kSphereInsetMetres, 0.1 * length);
        const std::array<double, 3> at = { point[0] + normal[0] * reach, point[1] + normal[1] * reach,
                                           point[2] + normal[2] * reach };
        bucket.meshVertices.push_back (Vertex (at, normal, rays && result.rayMesh[i] >= 0 ? blocked : style.fillRgba));
    }
    for (size_t row = 0; row + 1 < rows; ++row)
        for (size_t column = 0; column < columns; ++column) {
            const uint32_t a = uint32_t (row * columns + column);
            const uint32_t b = uint32_t (row * columns + (column + 1) % columns);
            const uint32_t c = uint32_t ((row + 1) * columns + column);
            const uint32_t d = uint32_t ((row + 1) * columns + (column + 1) % columns);
            for (const uint32_t index : { a, b, d, a, d, c })
                bucket.meshIndices.push_back (index);
        }
}

} // namespace

GhPreviewDrawables BuildViewpointOverlay (const evp::sunstudy::ViewpointOptions& options,
                                          const evp::sunstudy::ViewpointStudyResult* result,
                                          const ViewpointOverlayStyle& style)
{
    using evp::sunstudy::ViewpointMode;
    GhPreviewDrawables drawables;
    GhPreviewBucket& bucket = drawables.depthTested;
    Guide (options, style.guideRgba, bucket);
    if (result == nullptr || !result->valid)
        return drawables;
    if (result->options.mode == ViewpointMode::Sphere) {
        Sphere (*result, style, bucket);
        return drawables;
    }
    if (result->perimeter.size () < (result->Closed () ? 3u : 2u))
        return drawables;

    // A fan about the point keeps a concave isovist concave; a field of view's
    // fan stops at its last ray instead of closing back to the first.
    const auto& perimeter = result->perimeter;
    const size_t count = perimeter.size ();
    const size_t edges = result->Closed () ? count : count - 1;
    bucket.meshVertices.push_back (Up (result->options.point, style.fillRgba));
    for (const auto& vertex : perimeter)
        bucket.meshVertices.push_back (Up (vertex, style.fillRgba));
    for (size_t i = 0; i < edges; ++i) {
        bucket.meshIndices.push_back (0);
        bucket.meshIndices.push_back (uint32_t (1 + i));
        bucket.meshIndices.push_back (uint32_t (1 + (i + 1) % count));
    }

    // An edge between two stopped rays lies on Context: draw it as a collision.
    const auto& rayMesh = result->rayMesh;
    const bool rays = rayMesh.size () == count;
    for (size_t i = 0; i < edges; ++i) {
        const size_t next = (i + 1) % count;
        const bool blocked = rays && rayMesh[i] >= 0 && rayMesh[next] >= 0;
        Segment (perimeter[i], perimeter[next], blocked ? style.blockedRgba : style.openEdgeRgba, bucket);
    }
    if (!result->Closed ()) {
        // The field of view's two edges, from the point to its first and last rays.
        Segment (result->options.point, perimeter.front (), style.openEdgeRgba, bucket);
        Segment (result->options.point, perimeter.back (), style.openEdgeRgba, bucket);
        return drawables;
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
