#ifndef EVP_ARCHVIZ_VISIBILITYSTUDYDISPLAY_HPP
#define EVP_ARCHVIZ_VISIBILITYSTUDYDISPLAY_HPP

#include "ArchViz/SunStudyOverlay.hpp"
#include "SunStudy/VisibilityStudy.hpp"

#include <memory>
#include <string>

namespace geomsrv::archviz {

// Reuses the SunStudy face-map/atlas display channel with visibility's scalar
// palette. The result's snapshot is retained, so topology identity is checked by
// the same renderer path as direct sun.
std::unique_ptr<SunStudyAtlasUpload> BuildVisibilityStudyUpload (const evp::sunstudy::VisibilityStudyResult& result,
                                                                 std::string& error,
                                                                 const std::function<bool ()>& isCancelled = {});

} // namespace geomsrv::archviz

#endif
