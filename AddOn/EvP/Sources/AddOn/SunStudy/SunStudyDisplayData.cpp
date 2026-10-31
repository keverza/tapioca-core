#include "SunStudy/SunStudyDisplayData.hpp"
#include "SunStudy/SunStudyStore.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustStudy.hpp"
#endif

namespace evp::sunstudy {

#if defined(TAPIOCA_GEOMETRY_RUST)
namespace {

// The packet from the study's core: its image, per-face arrays and step masks
// are the store readers' own, so the display and the store agree.
template <typename Cancelled>
std::shared_ptr<const StudyDisplayData> PrepareRustDisplayData (const StudyRecord& record,
                                                                std::shared_ptr<StudyDisplayData> data,
                                                                std::string& error, const Cancelled& cancelled)
{
    const rust::RustStudyCore& core = *record.rustCore;
    rust::StudyReadStatus status = rust::StudyReadStatus::Ok;
    std::vector<float> image;
    const bool read = record.IsPatchDomain () ? core.DisplayData (data->patchTiles, data->patchLayouts, data->width,
                                                                  data->height, image, status, error)
                                              : core.AtlasImage (data->width, data->height, image, status, error);
    if (!read)
        return nullptr;
    if (status != rust::StudyReadStatus::Ok || image.empty ()) {
        error = record.IsPatchDomain () ? "sun study has no complete packed patch atlas"
                                        : "sun study has no model-surface atlas";
        return nullptr;
    }
    TapiocaStudyInfo info {};
    if (cancelled () || !core.Info (info, error))
        return nullptr;
    data->area = info.analysedArea;
    StepMaskAtlas steps;
    if (!core.StepMasks (steps, data->stepMinutes, data->noonStep, status, error) || cancelled () ||
        status != rust::StudyReadStatus::Ok || steps.width == 0)
        return nullptr;
    data->image = std::make_shared<const std::vector<float>> (std::move (image));
    data->stepWords = steps.words;
    data->stepCount = steps.steps;
    data->stepMasks = std::make_shared<const std::vector<uint32_t>> (std::move (steps.masks));
    record.displayCache = data;
    return data;
}

} // namespace
#endif

std::shared_ptr<const StudyDisplayData> PrepareStudyDisplayData (const StudyRecord& record, std::string& error,
                                                                 const std::function<bool ()>& isCancelled)
{
    const auto cancelled = [&] { return record.cancelRequested.load () || (isCancelled && isCancelled ()); };
    if (cancelled ()) {
        error = "sun study display cancelled";
        return nullptr;
    }
    const auto progress = record.CurrentProgress ();
    if (record.displayCache != nullptr && record.displayCache->generation == progress.generation &&
        record.displayCache->resolvedSteps == progress.resolvedSteps)
        return record.displayCache;
    auto data = std::make_shared<StudyDisplayData> ();
    data->generation = progress.generation;
    data->resolvedSteps = progress.resolvedSteps;
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (record.rustCore != nullptr)
        return PrepareRustDisplayData (record, std::move (data), error, cancelled);
#endif
    error = NoStudyCoreError ();
    return nullptr;
}

} // namespace evp::sunstudy
