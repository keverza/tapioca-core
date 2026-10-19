// Stub-linked parity oracle for wrapper failures; never linked into the add-on.
#include "GeometryRustFfi.h"
#include "GeometryRustTerrain.hpp"

#include <cstring>
#include <iostream>

namespace {
TapiocaStatus s_status = TAPIOCA_OK;
uint32_t s_version = TAPIOCA_ABI_VERSION;
bool s_message = true;
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
    std::cout << "Tapioca wrapper mismatch/status/output-preservation checks passed\n";
    return 0;
}
