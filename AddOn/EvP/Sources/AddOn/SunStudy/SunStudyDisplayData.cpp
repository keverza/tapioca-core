#include "SunStudy/SunStudyDisplayData.hpp"
#include "SunStudy/SunStudyStore.hpp"

namespace evp::sunstudy {

std::shared_ptr<const StudyDisplayData> PrepareStudyDisplayData (const StudyRecord& record, std::string& error,
                                                                 const std::function<bool ()>& isCancelled)
{
    const auto cancelled = [&] { return record.cancelRequested.load () || (isCancelled && isCancelled ()); };
    if (cancelled ()) {
        error = "sun study display cancelled";
        return nullptr;
    }
    const auto progress = record.session.Progress ();
    if (record.displayCache != nullptr && record.displayCache->generation == progress.generation &&
        record.displayCache->resolvedSteps == progress.resolvedSteps)
        return record.displayCache;
    auto data = std::make_shared<StudyDisplayData> ();
    data->generation = progress.generation;
    data->resolvedSteps = progress.resolvedSteps;
    const auto& accumulator = record.session.Accumulator ();
    std::vector<int64_t> texels (accumulator.SampleCount (), -1);
    std::vector<float> image;
    if (record.IsPatchDomain ()) {
        if (!record.patchGrid.valid || record.patchAtlas.Width () == 0 ||
            !PatchFaceArrays (record.patchGrid, record.patchAtlas, data->patchTiles, data->patchLayouts)) {
            error = "sun study has no complete packed patch atlas";
            return nullptr;
        }
        data->width = record.patchAtlas.Width ();
        data->height = record.patchAtlas.Height ();
        for (size_t sample = 0; sample < texels.size (); ++sample) {
            if (sample % 4096 == 0 && cancelled ())
                return nullptr;
            texels[sample] = record.patchAtlas.TexelOf (record.patchGrid, sample);
        }
        data->area = record.patchGrid.TotalArea ();
    }
    else {
        if (!record.atlas.valid || record.atlas.TexelCount () == 0) {
            error = "sun study has no model-surface atlas";
            return nullptr;
        }
        data->width = record.atlas.width;
        data->height = record.atlas.height;
        for (size_t sample = 0; sample < texels.size () && sample < record.atlas.texels.size (); ++sample)
            texels[sample] = record.atlas.texels[sample];
        for (const double area : record.sampleGrid.areas)
            data->area += area;
    }
    if (cancelled ())
        return nullptr;
    image.assign (static_cast<size_t> (data->width) * data->height, -1.0f);
    for (size_t sample = 0; sample < texels.size (); ++sample) {
        if (sample % 4096 == 0 && cancelled ())
            return nullptr;
        if (texels[sample] >= 0 && static_cast<size_t> (texels[sample]) < image.size ())
            image[static_cast<size_t> (texels[sample])] =
                static_cast<float> (accumulator.LitStepCount (sample) * record.series.HoursPerStep ());
    }
    auto steps = PackStepMasks (
        accumulator.SampleCount (), accumulator.StepCount (),
        [&] (size_t sample, size_t step) { return accumulator.Lit (sample, step); }, texels, data->width, data->height,
        cancelled);
    if (cancelled () || steps.width == 0)
        return nullptr;
    data->image = std::make_shared<const std::vector<float>> (std::move (image));
    data->stepWords = steps.words;
    data->stepCount = steps.steps;
    data->stepMasks = std::make_shared<const std::vector<uint32_t>> (std::move (steps.masks));
    data->stepMinutes = StepMinutes (record.series);
    data->noonStep = SolarNoonStep (record.series);
    record.displayCache = data;
    return data;
}

} // namespace evp::sunstudy
