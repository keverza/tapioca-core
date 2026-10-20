#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"
#include "Geometry/GeometryEngine.hpp"

#include <array>
#include <cstring>

namespace {
TapiocaStatus s_status = TAPIOCA_ERR_ARG;
bool s_message = true;
} // namespace
extern "C" TapiocaStatus tapioca_make_arc (const TapiocaArcRequest*, double* output, size_t, char* error,
                                           size_t capacity)
{
    *output = -999; // Deliberately corrupt provisional storage before refusing.
    if (capacity > 0) {
        const char* message = s_message ? "stub arc refusal" : "";
        if (std::strlen (message) + 1 <= capacity)
            std::memcpy (error, message, std::strlen (message) + 1);
    }
    return s_status;
}
int main ()
{
    const geomsrv::engine::Vector3 centre { 0, 0, 0 }, normal { 0, 0, 1 };
    for (auto status : std::array<TapiocaStatus, 4> { TAPIOCA_ERR_ARG, TAPIOCA_ERR_STATE, TAPIOCA_ERR_INTERNAL, 99 }) {
        s_status = status;
        for (bool message : { false, true }) {
            s_message = message;
            std::vector<double> result { 77 };
            std::string error;
            if (tapioca::geometry::MakeArc ({ centre, normal, 2, 0, 1, 2 }, result, error) || error.empty () ||
                result != std::vector<double> { 77 })
                return 1;
        }
    }
    return 0;
}
