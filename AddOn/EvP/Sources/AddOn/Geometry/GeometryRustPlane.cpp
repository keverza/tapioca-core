#include "Geometry/GeometryRustTerrain.hpp"
#include "GeometryRustFfi.h"

namespace tapioca::geometry {
bool ProjectToPlane (const PlaneValue& plane, std::span<const double> points, PlaneProjection& result,
                     std::string& error)
{
    TapiocaPlaneRequest input {};
    for (size_t axis = 0; axis < 3; ++axis) {
        input.plane.origin[axis] = plane.origin[axis];
        input.plane.normal[axis] = plane.normal[axis];
        input.plane.u[axis] = plane.u[axis];
    }
    input.points = { points.data (), points.size () };
    PlaneProjection candidate;
    candidate.points.resize (points.size ());
    TapiocaPlaneFrame frame {};
    const TapiocaPlaneOutput output { &frame, candidate.points.data (), candidate.points.size () };
    std::array<char, 256> buffer {};
    const auto status = tapioca_project_to_plane (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        size_t length = 0;
        while (length < buffer.size () && buffer[length] != '\0')
            ++length;
        error = length != 0 ? std::string (buffer.data (), length)
                            : "Tapioca Rust plane call failed with status " + std::to_string (status);
        return false;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        candidate.plane.origin[axis] = frame.plane.origin[axis];
        candidate.plane.normal[axis] = frame.plane.normal[axis];
        candidate.plane.u[axis] = frame.plane.u[axis];
        candidate.u[axis] = frame.u[axis];
        candidate.v[axis] = frame.v[axis];
    }
    result = std::move (candidate);
    error.clear ();
    return true;
}
} // namespace tapioca::geometry
