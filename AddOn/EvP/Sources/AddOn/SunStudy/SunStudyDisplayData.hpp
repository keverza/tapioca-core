#ifndef EVP_SUNSTUDY_DISPLAYDATA_HPP
#define EVP_SUNSTUDY_DISPLAYDATA_HPP

#include "SunStudy/SunStudyAtlas.hpp"

#include <functional>
#include <memory>
#include <string>

namespace evp::sunstudy {
struct StudyRecord;

// Immutable generation cache; no sample positions/normals/areas are copied.
// Triangle projections borrow the record's arrays while the worker holds it;
// patches project once per generation. Images reach the renderer by shared value.
struct StudyDisplayData {
    uint64_t generation = 0;
    size_t resolvedSteps = 0;
    uint32_t width = 0, height = 0, stepWords = 0, stepCount = 0, noonStep = 0;
    std::vector<AtlasTile> patchTiles;
    std::vector<FaceLayout> patchLayouts;
    std::shared_ptr<const std::vector<float>> image;
    std::shared_ptr<const std::vector<uint32_t>> stepMasks;
    std::vector<uint16_t> stepMinutes;
    double area = 0.0;
};

// Caller holds the record's session mutex (or an unpublished record). Palette,
// debug and depth-only changes do not invalidate this cache.
std::shared_ptr<const StudyDisplayData> PrepareStudyDisplayData (const StudyRecord& record, std::string& error,
                                                                 const std::function<bool ()>& isCancelled = {});

} // namespace evp::sunstudy
#endif
