#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"

#include <array>
#include <cstring>

namespace {
TapiocaStatus s_status = TAPIOCA_ERR_ARG;
bool s_message = true;
} // namespace

extern "C" TapiocaStatus tapioca_sample_polyline (const TapiocaPolylineRequest*, const TapiocaPolylineOutput* output,
                                                  char* error, size_t capacity)
{
    // A deliberately bad transport may touch provisional storage before refusing.
    // The owned facade must still leave the caller's result completely unchanged.
    *output->totalLength = -999;
    if (capacity > 0) {
        const char* message = s_message ? "stub polyline refusal" : "";
        const auto length = std::strlen (message);
        if (length + 1 <= capacity)
            std::memcpy (error, message, length + 1);
    }
    return s_status;
}

int main ()
{
    using namespace tapioca::geometry;
    const std::vector<double> points { 0, 0, 0, 0, 0, 8 }, parameters { 0.5 };
    for (auto status : std::array<TapiocaStatus, 4> { TAPIOCA_ERR_ARG, TAPIOCA_ERR_STATE, TAPIOCA_ERR_INTERNAL, 99 }) {
        s_status = status;
        for (bool message : { false, true }) {
            s_message = message;
            PolylineSamples result { { 77 }, 88, { 99 }, { 111 } };
            std::string error;
            if (SamplePolyline (points, parameters, result, error) || error.empty () ||
                result.cumulative != std::vector<double> { 77 } || result.totalLength != 88 ||
                result.points != std::vector<double> { 99 } || result.tangents != std::vector<double> { 111 })
                return 1;
        }
    }
    return 0;
}
