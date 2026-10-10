#include "ArchViz/ViewpointProjection.hpp"
#include "ArchViz/MatrixMath.hpp"
#include <cmath>

namespace geomsrv::archviz {
namespace {
using ClipPoint = std::array<float, 4>;
float Distance (const ClipPoint& p, int plane)
{
    switch (plane) {
        case 0:
            return p[0] + p[3];
        case 1:
            return p[3] - p[0];
        case 2:
            return p[1] + p[3];
        case 3:
            return p[3] - p[1];
        case 4:
            return p[2];
        default:
            return p[3] - p[2];
    }
}
} // namespace

ViewpointScreenPolygon ProjectViewpointTriangle (const double a[3], const double b[3], const double c[3],
                                                 const float matrix[16], uint32_t width, uint32_t height)
{
    ViewpointScreenPolygon result;
    if (matrix == nullptr || width == 0 || height == 0)
        return result;
    std::array<ClipPoint, 12> polygon {}, next {};
    const double* source[] = { a, b, c };
    for (size_t vertex = 0; vertex < 3; ++vertex) {
        const float world[4] = { float (source[vertex][0]), float (source[vertex][1]), float (source[vertex][2]),
                                 1.0f };
        TransformPoint (polygon[vertex].data (), world, matrix);
        for (const float component : polygon[vertex])
            if (!std::isfinite (component))
                return result;
    }
    size_t count = 3;
    for (int plane = 0; plane < 6 && count > 0; ++plane) {
        size_t output = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto& start = polygon[i];
            const auto& end = polygon[(i + 1) % count];
            const float da = Distance (start, plane), db = Distance (end, plane);
            if (da >= 0.0f)
                next[output++] = start;
            if ((da >= 0.0f) != (db >= 0.0f)) {
                const float t = da / (da - db);
                ClipPoint cut;
                for (int axis = 0; axis < 4; ++axis)
                    cut[axis] = start[axis] + (end[axis] - start[axis]) * t;
                next[output++] = cut;
            }
        }
        count = output;
        polygon = next;
    }
    if (count < 3)
        return result;
    for (size_t i = 0; i < count; ++i) {
        if (polygon[i][3] <= 1.0e-6f)
            return {};
        result.points[i] = { (polygon[i][0] / polygon[i][3] * 0.5f + 0.5f) * float (width),
                             (0.5f - polygon[i][1] / polygon[i][3] * 0.5f) * float (height) };
    }
    result.count = count;
    return result;
}

} // namespace geomsrv::archviz
