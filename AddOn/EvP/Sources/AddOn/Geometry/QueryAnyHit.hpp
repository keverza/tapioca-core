#ifndef EVP_GEOMETRY_QUERYANYHIT_HPP
#define EVP_GEOMETRY_QUERYANYHIT_HPP

// Private to QueryEngine.cpp. Reuse nanort's exact box/triangle arithmetic and
// existing BVH, but stop at the first accepted blocker, not the nearest one.
#include <nanort.h>

namespace geomsrv {
inline bool QueryAnyHit (const nanort::BVHAccel<double>& accel, const nanort::Ray<double>& ray, const double* vertices,
                         const uint32_t* faces)
{
    nanort::TriangleIntersector<double, nanort::TriangleIntersection<double>> intersector (vertices, faces,
                                                                                           sizeof (double) * 3);
    intersector.PrepareTraversal (ray, nanort::BVHTraceOptions {});
    const auto& nodes = accel.GetNodes ();
    const auto& indices = accel.GetIndices ();
    nanort::real3<double> origin (ray.org), direction (ray.dir);
    const auto inverse = nanort::vsafe_inverse (direction);
    int signs[3] = { ray.dir[0] < 0.0 ? 1 : 0, ray.dir[1] < 0.0 ? 1 : 0, ray.dir[2] < 0.0 ? 1 : 0 };
    uint32_t stack[512] = { 0 };
    size_t pending = 1;
    while (pending != 0) {
        const auto& node = nodes[stack[--pending]];
        double nearT, farT;
        if (!nanort::IntersectRayAABB (&nearT, &farT, ray.min_t, ray.max_t, node.bmin, node.bmax, origin, inverse,
                                       signs))
            continue;
        if (node.flag == 0) {
            if (pending + 2 > 512) {
                nanort::TriangleIntersection<double> hit;
                return accel.Traverse (ray, intersector, &hit); // no truncation on unusual trees
            }
            const int nearChild = signs[node.axis];
            stack[pending++] = node.data[1 - nearChild];
            stack[pending++] = node.data[nearChild];
        }
        else {
            for (uint32_t primitive = 0; primitive < node.data[0]; ++primitive) {
                double distance = ray.max_t;
                if (intersector.Intersect (&distance, indices[node.data[1] + primitive]) && distance < ray.max_t)
                    return true; // matches Traverse's strict upper bound
            }
        }
    }
    return false;
}
} // namespace geomsrv
#endif
