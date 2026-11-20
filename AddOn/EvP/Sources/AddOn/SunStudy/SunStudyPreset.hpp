#ifndef EVP_SUNSTUDY_SUNSTUDYPRESET_HPP
#define EVP_SUNSTUDY_SUNSTUDYPRESET_HPP

#include "SunStudy/SunStudyRoles.hpp"
#include <map>

namespace evp::sunstudy {

struct SunStudyReceivers {
    ElementRoles roles;
    // Empty per-mesh masks mean every face; populated masks keep source-face
    // addresses intact, including unmeasured opaque faces beside glass panes.
    std::vector<std::vector<uint8_t>> faces;
    size_t analysisFaces = 0;
    size_t contextFaces = 0;
    size_t unknownMaterialFaces = 0;
};

} // namespace evp::sunstudy
#endif
