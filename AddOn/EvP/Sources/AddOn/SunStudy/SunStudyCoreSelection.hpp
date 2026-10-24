#ifndef EVP_SUNSTUDY_SUNSTUDYCORESELECTION_HPP
#define EVP_SUNSTUDY_SUNSTUDYCORESELECTION_HPP

// SunStudy/SunStudyCoreSelection -- which core runs a NEW study (P5 flip).
//
// `TAPIOCA_SUNSTUDY_CORE=rust` attaches the Rust study core to every study
// started afterwards; anything else, or a build without the Rust archive,
// keeps the C++ core. Read once per process: a study keeps the core it
// started with, so a running session never mixes the two. The C++ core stays
// compiled as the oracle until the default flips (P6).

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
