#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryEngine.hpp"
#include "GeometryRustFfi.h"

#include <array>

namespace tapioca::geometry {
bool MakeArc (const ArcRequest& request, std::vector<double>& result, std::string& error)
{
    if (request.segments < 2 || request.segments > 4096) {
        error = "ArcSegments: expected 2..4096";
        return false;
    }
    const TapiocaArcRequest input { { request.centre.x, request.centre.y, request.centre.z },
                                    { request.normal.x, request.normal.y, request.normal.z },
                                    request.radius,
                                    request.startRadians,
                                    request.sweepRadians,
                                    static_cast<int32_t> (request.segments) };
    std::vector<double> candidate ((static_cast<size_t> (request.segments) + 1) * 3);
    std::array<char, 256> buffer {};
    const auto status = tapioca_make_arc (&input, candidate.data (), candidate.size (), buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        size_t length = 0;
        while (length < buffer.size () && buffer[length] != '\0')
            ++length;
        error = length != 0 ? std::string (buffer.data (), length)
                            : "Tapioca Rust arc call failed with status " + std::to_string (status);
        return false;
    }
    result = std::move (candidate);
    error.clear ();
    return true;
}
} // namespace tapioca::geometry
