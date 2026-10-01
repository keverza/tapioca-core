#ifndef EVP_SUNSTUDY_SUNSTUDYREUSE_HPP
#define EVP_SUNSTUDY_SUNSTUDYREUSE_HPP

#include "SunStudy/SunStudyStore.hpp"

namespace evp::sunstudy {

// Exact sample identity on unchanged elements, with conservative old AND new
// shadow bounds. No quantised/hash-only match can silently transfer sunlight.
// The caller supplies a completed immutable source and an unpublished target.
size_t ReuseUnaffectedSamples (const StudyRecord& source, StudyRecord& target, const std::atomic<bool>& cancelled);
void SetSampleMeshes (StudyRecord& record);

} // namespace evp::sunstudy
#endif
