#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyRustFfi.h"

#include <array>

namespace evp::sunstudy::rust {

namespace {

constexpr size_t kErrorCapacity = 256;
using ErrorBuffer = std::array<char, kErrorCapacity>;

std::string FailureMessage (TapiocaStatus status, const ErrorBuffer& buffer)
{
    size_t length = 0;
    while (length < buffer.size () && buffer[length] != '\0')
        ++length;
    if (length != 0)
        return std::string (buffer.data (), length);
    // A failed status with no diagnostic is still a failure (overlap refusals
    // deliberately write nothing); never turn it into success.
    return "Tapioca Rust sun-study call failed with status " + std::to_string (status);
}

} // namespace

bool ReadAbiVersion (uint32_t& outVersion, std::string& error)
{
    ErrorBuffer buffer {};
    uint32_t version = 0;
    const TapiocaStatus status = tapioca_sunstudy_version (&version, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    if (version != TAPIOCA_SUNSTUDY_ABI_VERSION) {
        error = "Tapioca Rust sun-study ABI version " + std::to_string (version) + " does not match expected " +
                std::to_string (TAPIOCA_SUNSTUDY_ABI_VERSION);
        return false;
    }
    outVersion = version;
    error.clear ();
    return true;
}

} // namespace evp::sunstudy::rust
