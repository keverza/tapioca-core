#include "SunStudy/VisibilityStudy.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustViews.hpp"
#endif

#include <utility>

namespace evp::sunstudy {

VisibilityStudyResult RunSelectedVisibilityStudy (std::shared_ptr<const geomsrv::Snapshot> snapshot,
                                                  const std::vector<std::string>& fromElements,
                                                  const std::vector<std::string>& toElements,
                                                  const VisibilityStudyOptions& options,
                                                  const std::function<bool ()>& isCancelled)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    return rust::RunVisibilityStudy (std::move (snapshot), fromElements, toElements, options, isCancelled);
#else
    (void) options, (void) isCancelled;
    VisibilityStudyResult refused;
    refused.snapshot = std::move (snapshot);
    refused.fromElements = fromElements;
    refused.toElements = toElements;
    refused.error = "this build has no sun-study core";
    return refused;
#endif
}

} // namespace evp::sunstudy
