#ifndef EVP_SUNSTUDY_SUNSTUDYCORESELECTION_HPP
#define EVP_SUNSTUDY_SUNSTUDYCORESELECTION_HPP

// SunStudy/SunStudyCoreSelection -- which core runs a NEW study.
//
// The Rust study core is the default (P6). `TAPIOCA_SUNSTUDY_CORE=cpp`, or a
// build without the Rust archive, keeps the C++ core; any other value,
// including unset, selects Rust. Read once per process: a study keeps the
// core it started with, so a running session never mixes the two. The C++
// core stays compiled as the oracle and the escape hatch until P7.

#include <cstdint>
#include <optional>

namespace evp::sunstudy {

enum class SunStudyCoreKind : uint8_t { Cpp = 0, Rust = 1 };

SunStudyCoreKind SelectedSunStudyCore ();

// True when this build links the Rust archive (TAPIOCA_GEOMETRY_RUST).
bool RustSunStudyCoreAvailable ();

// Tests only: replace the environment's answer (nullopt restores it).
void OverrideSunStudyCore (std::optional<SunStudyCoreKind> kind);

} // namespace evp::sunstudy

#endif
