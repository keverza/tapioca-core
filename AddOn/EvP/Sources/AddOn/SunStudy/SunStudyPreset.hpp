#ifndef EVP_SUNSTUDY_SUNSTUDYPRESET_HPP
#define EVP_SUNSTUDY_SUNSTUDYPRESET_HPP

#include "SunStudy/SunStudyRoles.hpp"
#include <map>

namespace evp::sunstudy {

// Advisory only, never an automatic mode switch or a timing promise. Unknown
// and medium-sized models have no warning; the palette owns text and colour.
std::string SunStudyPresetAdvice (size_t triangleCount, const std::string& selectedPreset);

struct SunStudyReceivers {
    ElementRoles roles;
    // Empty per-mesh masks mean every face; populated masks keep source-face
    // addresses intact, including unmeasured opaque faces beside glass panes.
    std::vector<std::vector<uint8_t>> faces;
    size_t analysisFaces = 0;
    size_t contextFaces = 0;
    size_t unknownMaterialFaces = 0;
};

// Transparency is the MODEL pool's 0..1 value, not an Archicad attribute index
// or renderer alpha. This selects receiver candidates only: all non-ignored
// faces still cast BINARY shadows, including the glass itself.
SunStudyReceivers BuildSunStudyReceivers (const geomsrv::Snapshot& snapshot, const ElementRoles& roles, bool glassOnly,
                                          const std::map<int32_t, double>& transparency, double threshold);

} // namespace evp::sunstudy
#endif
