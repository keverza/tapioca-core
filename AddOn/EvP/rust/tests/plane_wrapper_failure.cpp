#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"

#include <cstring>
namespace {
TapiocaStatus s_status = TAPIOCA_ERR_ARG;
bool s_message = true;
} // namespace
extern "C" TapiocaStatus tapioca_project_to_plane (const TapiocaPlaneRequest*, const TapiocaPlaneOutput* output,
                                                   char* error, size_t capacity)
{
    output->frame->plane.origin[0] = -999;
    output->frame->u[0] = -999;
    if (output->scalarCapacity != 0)
        output->points[0] = -999;
    if (capacity > 0) {
        const char* message = s_message ? "stub plane refusal" : "";
        if (std::strlen (message) + 1 <= capacity)
            std::memcpy (error, message, std::strlen (message) + 1);
    }
    return s_status;
}
int main ()
{
    using namespace tapioca::geometry;
    for (auto status : std::array<TapiocaStatus, 4> { TAPIOCA_ERR_ARG, TAPIOCA_ERR_STATE, TAPIOCA_ERR_INTERNAL, 99 }) {
        s_status = status;
        for (bool message : { false, true }) {
            s_message = message;
            PlaneProjection result {
                { { 77, 77, 77 }, { 88, 88, 88 }, { 99, 99, 99 } }, { 111, 111, 111 }, { 222, 222, 222 }, { 333 }
            };
            const auto saved = result;
            std::string error;
            if (ProjectToPlane ({}, std::array<double, 3> { 1, 2, 3 }, result, error) || error.empty () ||
                result.plane.origin != saved.plane.origin || result.plane.normal != saved.plane.normal ||
                result.plane.u != saved.plane.u || result.u != saved.u || result.v != saved.v ||
                result.points != saved.points)
                return 1;
        }
    }
    return 0;
}
