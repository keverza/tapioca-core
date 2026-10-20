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

bool ComposeTransforms (std::span<const geomsrv::engine::Transform> transforms, geomsrv::engine::Transform& result,
                        std::string& error)
{
    // Explicit POD conversion, never reinterpret C++ object storage as a Rust
    // array. Only 12-double matrix descriptors are copied, no mesh/provenance.
    std::vector<TapiocaAffineTransform> input (transforms.size ());
    for (size_t i = 0; i < transforms.size (); ++i)
        for (size_t row = 0; row < 3; ++row)
            for (size_t column = 0; column < 4; ++column)
                input[i].m[row][column] = transforms[i].m[row][column];
    TapiocaAffineTransform candidate {};
    std::array<char, 256> buffer {};
    const auto status =
        tapioca_compose_transforms (input.data (), input.size (), &candidate, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        size_t length = 0;
        while (length < buffer.size () && buffer[length] != '\0')
            ++length;
        error = length != 0 ? std::string (buffer.data (), length)
                            : "Tapioca Rust composition failed with status " + std::to_string (status);
        return false;
    }
    geomsrv::engine::Transform composed;
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 4; ++column)
            composed.m[row][column] = candidate.m[row][column];
    result = composed;
    error.clear ();
    return true;
}

bool BuildTransform (const TransformBuilderRequest& request, geomsrv::engine::Transform& result, std::string& error)
{
    const TapiocaTransformBuilderRequest input { static_cast<uint32_t> (request.kind),
                                                 { request.origin.x, request.origin.y, request.origin.z },
                                                 { request.vector.x, request.vector.y, request.vector.z },
                                                 request.radians };
    TapiocaAffineTransform candidate {};
    std::array<char, 256> buffer {};
    const auto status = tapioca_build_transform (&input, &candidate, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        size_t length = 0;
        while (length < buffer.size () && buffer[length] != '\0')
            ++length;
        error = length != 0 ? std::string (buffer.data (), length)
                            : "Tapioca Rust transform builder failed with status " + std::to_string (status);
        return false;
    }
    geomsrv::engine::Transform built;
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 4; ++column)
            built.m[row][column] = candidate.m[row][column];
    result = built;
    error.clear ();
    return true;
}
} // namespace tapioca::geometry
