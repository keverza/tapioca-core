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

bool DividePolyline (std::span<const double> points, int32_t count, bool includeEnds, std::vector<double>& result,
                     std::string& error)
{
    if (count < 1 || count > 100000) {
        error = "PolylineDivideCount: expected between 1 and 100000 segments";
        return false;
    }
    const size_t samples = includeEnds ? size_t (count) + 1 : size_t (count) - 1;
    const TapiocaDividePolylineRequest input { { points.data (), points.size () }, count, includeEnds ? 1U : 0U };
    std::vector<double> candidate (samples * 3);
    std::array<char, 256> buffer {};
    const auto status =
        tapioca_divide_polyline (&input, candidate.data (), candidate.size (), buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        size_t length = 0;
        while (length < buffer.size () && buffer[length] != '\0')
            ++length;
        error = length != 0 ? std::string (buffer.data (), length)
                            : "Tapioca Rust polyline division failed with status " + std::to_string (status);
        return false;
    }
    result = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
