#ifndef EVP_SUNSTUDY_SUNSTUDYRUST_HPP
#define EVP_SUNSTUDY_SUNSTUDYRUST_HPP

// SunStudy/SunStudyRust — the C++ face of the Rust sun-study core.
//
// Thin, SDK-free wrappers over SunStudyRustFfi.h: they own the error buffer,
// translate C++ value types to POD views, and turn a refused call into
// `false` plus a message. Nothing in production calls them yet — the C++ core
// stays authoritative and is the parity oracle (test_sunstudy_rust_parity.cpp)
// until the TAPIOCA_SUNSTUDY_CORE flip.

#include <cstdint>
#include <string>

namespace evp::sunstudy::rust {

// The linked archive's sun-study ABI version; refuses a header/archive skew.
bool ReadAbiVersion (uint32_t& outVersion, std::string& error);

} // namespace evp::sunstudy::rust

#endif
