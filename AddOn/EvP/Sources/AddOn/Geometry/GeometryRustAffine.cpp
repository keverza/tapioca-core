#include "Geometry/GeometryRustTerrain.hpp"
#include "GeometryRustFfi.h"
#include "Transforms.hpp"

#include <array>
#include <limits>

namespace tapioca::geometry {

bool TransformVectors (const TransformVectorsRequest& request, std::vector<double>& result, bool& flipsOrientation,
                       std::string& error)
{
    TapiocaAffineRequest input {};
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 4; ++column)
            input.transform.m[row][column] = request.transform.m[row][column];
    input.mode = static_cast<uint32_t> (request.mode);
    input.values = { request.values.data (), request.values.size () };
    // Only output storage is allocated. Input coordinates stay borrowed; the
    // small matrix/descriptor copies have no geometry or identity ownership.
    std::vector<double> candidate (request.values.size ());
    uint32_t orientation = std::numeric_limits<uint32_t>::max ();
    TapiocaAffineOutput output { candidate.data (), candidate.size (), &orientation };
    std::array<char, 256> buffer {};
    const auto status = tapioca_transform_vectors (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK || orientation > 1) {
        size_t length = 0;
        while (length < buffer.size () && buffer[length] != '\0')
            ++length;
        error = length != 0 ? std::string (buffer.data (), length)
                            : "Tapioca Rust affine call failed with status " + std::to_string (status);
        return false;
    }
    result = std::move (candidate);
    flipsOrientation = orientation != 0;
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
