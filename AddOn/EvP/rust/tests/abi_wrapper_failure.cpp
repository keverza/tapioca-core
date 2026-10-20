// Stub-linked parity oracle for wrapper failures; never linked into the add-on.
#include "GeometryRustFfi.h"
#include "GeometryRustTerrain.hpp"
#include "Mesh.hpp"
#include "Transforms.hpp"

#include <cstring>
#include <iostream>

namespace {
TapiocaStatus s_status = TAPIOCA_OK;
uint32_t s_version = TAPIOCA_ABI_VERSION;
bool s_message = true;
uint32_t s_diagnostic = TAPIOCA_DIAGNOSTIC_NONE;
} // namespace

extern "C" TapiocaStatus tapioca_version (uint32_t* version, char* error, size_t capacity)
{
    *version = s_version; // Even a misbehaving failed callee cannot commit the wrapper output.
    if (capacity != 0) {
        const char* message = s_status != TAPIOCA_OK && s_message ? "fixture failure" : "";
        const size_t length = std::strlen (message) < capacity - 1 ? std::strlen (message) : capacity - 1;
        std::memcpy (error, message, length);
        error[length] = '\0';
    }
    return s_status;
}

extern "C" TapiocaStatus tapioca_validate_mesh (const TapiocaMeshView*, char*, size_t)
{
    return TAPIOCA_ERR_INTERNAL;
}

extern "C" TapiocaStatus tapioca_validate_snapshot (const TapiocaSnapshotView*, char*, size_t)
{
    return TAPIOCA_ERR_INTERNAL;
}

extern "C" TapiocaStatus tapioca_validate_mesh_value (const TapiocaMeshValueView*, char*, size_t)
{
    return s_status;
}

extern "C" TapiocaStatus tapioca_validate_snapshot_value (const TapiocaSnapshotValueRequest*, char*, size_t)
{
    return s_status;
}

extern "C" TapiocaStatus tapioca_transform_vectors (const TapiocaAffineRequest*, const TapiocaAffineOutput* output,
                                                    char*, size_t)
{
    if (output->capacity != 0)
        output->data[0] = 123; // Even a failed/misbehaving callee cannot commit wrapper outputs.
    *output->flipsOrientation = s_diagnostic;
    return s_status;
}

extern "C" TapiocaStatus tapioca_compose_transforms (const TapiocaAffineTransform*, size_t,
                                                     TapiocaAffineTransform* output, char*, size_t)
{
    output->m[0][3] = 123; // A failed callee cannot commit the facade's provisional matrix.
    return s_status;
}

extern "C" TapiocaStatus tapioca_validate_provenance (const TapiocaProvenanceRequest*, uint32_t* diagnostic, char*,
                                                      size_t)
{
    *diagnostic = s_diagnostic; // Intentionally write even on transport failure.
    return s_status;
}

extern "C" TapiocaStatus tapioca_validate_attributed_mesh (const TapiocaMeshView*,
                                                           const TapiocaProvenanceRequest* request,
                                                           uint32_t* diagnostic, char* error, size_t capacity)
{
    return tapioca_validate_provenance (request, diagnostic, error, capacity);
}

int main ()
{
    uint32_t output = 77;
    std::string error;
    s_version = TAPIOCA_ABI_VERSION + 1;
    if (tapioca::geometry::ReadRustAbiVersion (output, error) || output != 77 ||
        error.find ("does not match") == std::string::npos)
        return 1;
    for (TapiocaStatus status : { TAPIOCA_ERR_ARG, TAPIOCA_ERR_STATE, TAPIOCA_ERR_INTERNAL }) {
        s_status = status;
        if (tapioca::geometry::ReadRustAbiVersion (output, error) || output != 77 || error != "fixture failure")
            return 2;
    }
    s_message = false;
    if (tapioca::geometry::ReadRustAbiVersion (output, error) || output != 77 ||
        error.find ("status 3") == std::string::npos)
        return 3;
    s_status = TAPIOCA_OK;
    s_version = TAPIOCA_ABI_VERSION;
    if (!tapioca::geometry::ReadRustAbiVersion (output, error) || output != TAPIOCA_ABI_VERSION || !error.empty ())
        return 4;

    using namespace tapioca::geometry;
    const ProvenanceRequest request;
    const geomsrv::Mesh mesh;
    // Exercise both wrappers against inconsistent and failed callee envelopes.
    for (bool attributed : { false, true }) {
        auto validate = [&] (ProvenanceDiagnostic& diagnostic) {
            return attributed ? ValidateAttributedMesh (mesh, request, diagnostic, error)
                              : ValidateProvenance (request, diagnostic, error);
        };
        for (TapiocaStatus status : std::array<TapiocaStatus, 5> { TAPIOCA_OK, TAPIOCA_ERR_ARG, TAPIOCA_ERR_STATE,
                                                                   TAPIOCA_ERR_INTERNAL, 99 }) {
            for (uint32_t code : { 0U, 1U, 2U, 3U, 4U, 99U }) {
                s_status = status;
                s_diagnostic = code;
                auto diagnostic = ProvenanceDiagnostic::PatchKeyUnstable;
                error = "prior error";
                const bool success = validate (diagnostic);
                const bool validSuccess = status == TAPIOCA_OK && code == 0;
                const bool validRefusal = status == TAPIOCA_ERR_STATE && code >= 1 && code <= 4;
                if (success != validSuccess || (success && !error.empty ()) || (!success && error.empty ()))
                    return 5;
                const auto expected = validSuccess || validRefusal ? static_cast<ProvenanceDiagnostic> (code)
                                                                   : ProvenanceDiagnostic::PatchKeyUnstable;
                if (diagnostic != expected)
                    return 6;
            }
        }
    }
    const geomsrv::Snapshot snapshot;
    for (TapiocaStatus status :
         std::array<TapiocaStatus, 5> { TAPIOCA_OK, TAPIOCA_ERR_ARG, TAPIOCA_ERR_STATE, TAPIOCA_ERR_INTERNAL, 99 }) {
        s_status = status;
        const bool success = ValidateMeshValue (mesh, error);
        if (success != (status == TAPIOCA_OK) || (success && !error.empty ()) || (!success && error.empty ()))
            return 7;
        if (ValidateSnapshotValue (snapshot, {}, error) != success || (success && !error.empty ()) ||
            (!success && error.empty ()))
            return 8;
    }
    std::cout << "Tapioca wrapper mismatch/status/output-preservation checks passed\n";
    const geomsrv::engine::Transform transform;
    const std::array<double, 3> coordinates { 1, 2, 3 };
    for (TapiocaStatus status :
         std::array<TapiocaStatus, 5> { TAPIOCA_OK, TAPIOCA_ERR_ARG, TAPIOCA_ERR_STATE, TAPIOCA_ERR_INTERNAL, 99 }) {
        for (uint32_t report : { 0U, 1U, 99U }) {
            s_status = status;
            s_diagnostic = report;
            std::vector<double> result { 77 };
            bool flipped = true;
            const bool success =
                TransformVectors ({ transform, VectorTransformMode::Point, coordinates }, result, flipped, error);
            const bool expectedSuccess = status == TAPIOCA_OK && report <= 1;
            if (success != expectedSuccess || (success && !error.empty ()) || (!success && error.empty ()))
                return 9;
            if (success) {
                if (result.size () != 3 || result.front () != 123 || flipped != (report == 1))
                    return 10;
            }
            else if (result != std::vector<double> { 77 } || !flipped)
                return 11;
        }
    }
    for (TapiocaStatus status :
         std::array<TapiocaStatus, 4> { TAPIOCA_ERR_ARG, TAPIOCA_ERR_STATE, TAPIOCA_ERR_INTERNAL, 99 }) {
        s_status = status;
        geomsrv::engine::Transform composed;
        composed.m[0][3] = 77;
        if (ComposeTransforms ({}, composed, error) || composed.m[0][3] != 77 || error.empty ())
            return 12;
    }
    return 0;
}
