#ifndef EVP_SUNSTUDY_SUNSTUDYSELECTIONBINDING_HPP
#define EVP_SUNSTUDY_SUNSTUDYSELECTIONBINDING_HPP

#include "SunStudy/SunStudyRoles.hpp"
#include <cstdint>
#include <string>

namespace evp::sunstudy {

enum class SelectionBindingRefresh { Inactive, Unchanged, Changed, Detached };

// Optional live role source. The palette's declaration generation prevents a
// later command (even with identically named sets) from changing this study.
// The refresh decision runs in the Rust core (kernels::RefreshSelectionBinding).
struct SunStudySelectionBinding {
    std::string contextSet, ignoredSet;
    uint64_t generation = 0;
    uint64_t revision = 0;
    std::string analysisSet;
};

} // namespace evp::sunstudy
#endif
