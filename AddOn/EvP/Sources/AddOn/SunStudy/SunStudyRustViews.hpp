#ifndef EVP_SUNSTUDY_SUNSTUDYRUSTVIEWS_HPP
#define EVP_SUNSTUDY_SUNSTUDYRUSTVIEWS_HPP

// SunStudy/SunStudyRustViews -- RunViewpointStudy and RunVisibilityStudy with
// the policy in Rust (P4). The snapshot's query index stays C++ and is
// reached through a ray-query vtable whose shim catches every exception; it
// is acquired where the C++ studies acquire it, so refusals keep their order
// and their messages.

#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/ViewpointStudy.hpp"
#include "SunStudy/VisibilityStudy.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace evp::sunstudy::rust {

ViewpointStudyResult RunViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                        const std::vector<std::string>& context, const ViewpointOptions& options,
                                        const std::function<bool ()>& isCancelled = {});

// analysisMilliseconds is measured around the whole call, as the C++ study does.
VisibilityStudyResult RunVisibilityStudy (std::shared_ptr<const geomsrv::Snapshot> snapshot,
                                          const std::vector<std::string>& fromElements,
                                          const std::vector<std::string>& toElements,
                                          const VisibilityStudyOptions& options = VisibilityStudyOptions (),
                                          const std::function<bool ()>& isCancelled = {});

} // namespace evp::sunstudy::rust

#endif
