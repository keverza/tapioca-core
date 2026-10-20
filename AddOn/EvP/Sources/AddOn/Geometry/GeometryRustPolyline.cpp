#include "Geometry/GeometryRustTerrain.hpp"
#include "GeometryRustFfi.h"

#include <array>
#include <limits>

namespace tapioca::geometry {

bool SamplePolyline (std::span<const double> points, std::span<const double> parameters, PolylineSamples& result,
                     std::string& error)
{
    if (parameters.size () > std::numeric_limits<size_t>::max () / 3) {
        error = "PolylineOutputOverflow";
        return false;
    }
    const TapiocaPolylineRequest input { { points.data (), points.size () },
                                         { parameters.data (), parameters.size () } };
    PolylineSamples candidate;
    candidate.cumulative.resize (points.size () / 3);
    candidate.points.resize (parameters.size () * 3);
    candidate.tangents.resize (candidate.points.size ());
    const TapiocaPolylineOutput output { candidate.cumulative.data (), candidate.cumulative.size (),
                                         candidate.points.data (),     candidate.tangents.data (),
                                         candidate.points.size (),     &candidate.totalLength };
    std::array<char, 256> buffer {};
    const auto status = tapioca_sample_polyline (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        size_t length = 0;
        while (length < buffer.size () && buffer[length] != '\0')
            ++length;
        error = length != 0 ? std::string (buffer.data (), length)
                            : "Tapioca Rust polyline call failed with status " + std::to_string (status);
        return false;
    }
    result = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
