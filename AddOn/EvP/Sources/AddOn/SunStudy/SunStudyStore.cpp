#include "SunStudy/SunStudyStore.hpp"

#include <chrono>

namespace evp::sunstudy {

SampleSet StudyRecord::Samples () const
{
    SampleSet set;
    set.positions = positions.empty () ? nullptr : positions.data ();
    set.normals = normals.empty () ? nullptr : normals.data ();
    set.count = positions.size () / 3;
    return set;
}

SunStudyStore& SunStudyStore::Get ()
{
    static SunStudyStore instance;
    return instance;
}

std::string SunStudyStore::Insert (std::unique_ptr<StudyRecord> record)
{
    if (record == nullptr)
        return std::string ();

    std::lock_guard<std::mutex> lock (mutex_);
    if (record->id.empty ())
        record->id = "sun-" + std::to_string (nextId_++);

    const std::string id = record->id;
    advancing_[id] = false;
    studies_[id] = std::move (record);
    return id;
}

bool SunStudyStore::Advance (const std::string& id, size_t maxSteps, size_t maxParallel, double tmin, double tmax,
                             size_t& advanced, std::string& error)
{
    advanced = 0;

    StudyRecord* record = nullptr;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        const auto found = studies_.find (id);
        if (found == studies_.end ()) {
            error = "no sun study with id '" + id + "'";
            return false;
        }
        // ⚠️ A PER-STUDY FLAG, NOT A GLOBAL ONE. Two callers advancing the SAME
        // study would interleave slices into one accumulator; two callers
        // advancing DIFFERENT studies is fine and must stay fine.
        if (advancing_[id]) {
            error = "sun study '" + id + "' is already being advanced";
            return false;
        }
        advancing_[id] = true;
        record = found->second.get ();
    }

    const auto start = std::chrono::steady_clock::now ();
    if (record->traversal != nullptr)
        advanced = record->session.Advance (*record->traversal, maxSteps, tmin, tmax, maxParallel);
    const double elapsed =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();

    {
        std::lock_guard<std::mutex> lock (mutex_);
        // ⚠️ RE-LOOK RATHER THAN REUSE THE POINTER. Erase could have run while
        // the lock was released, in which case `record` is gone and touching it
        // is a use-after-free -- the exact hazard that not holding the lock buys
        // performance at the cost of.
        const auto found = studies_.find (id);
        if (found != studies_.end ())
            found->second->analysisMilliseconds += elapsed;
        advancing_[id] = false;
    }
    return true;
}

bool SunStudyStore::Progress (const std::string& id, StudyProgress& progress, std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }
    progress = found->second->session.Progress ();
    return true;
}

bool SunStudyStore::SunHours (const std::string& id, std::vector<double>& hours, std::vector<double>& positions,
                              std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }
    hours = found->second->session.SunHours ();
    positions = found->second->positions;
    return true;
}

bool SunStudyStore::Results (const std::string& id, std::vector<double>& hours, std::vector<double>& positions,
                             std::vector<double>& normals, std::vector<uint8_t>* stepBits, std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }

    const StudyRecord& record = *found->second;
    hours = record.session.SunHours ();
    positions = record.positions;
    normals = record.normals;

    if (stepBits != nullptr) {
        const OcclusionAccumulator& accumulator = record.session.Accumulator ();
        const size_t samples = accumulator.SampleCount ();
        const size_t steps = accumulator.StepCount ();
        stepBits->assign (samples * steps, 0);
        // Sample-major, so one sample's whole day is contiguous -- which is how
        // every consumer reads it.
        for (size_t sample = 0; sample < samples; ++sample) {
            for (size_t step = 0; step < steps; ++step)
                (*stepBits)[sample * steps + step] = accumulator.Lit (sample, step) ? 1u : 0u;
        }
    }
    return true;
}

bool SunStudyStore::AtlasImage (const std::string& id, uint32_t& width, uint32_t& height, std::vector<float>& image,
                                std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }

    const StudyRecord& record = *found->second;
    if (!record.atlas.valid) {
        // ⚠️ THE PATCH CASE IS NAMED SEPARATELY BECAUSE THE GENERAL MESSAGE IS
        // A LIE FOR IT. A patch study WAS sampled on model surfaces; what it has
        // is a different atlas. Telling its caller otherwise would send the next
        // reader looking at the sampler instead of at the display path.
        error = record.IsPatchDomain ()
                    ? "study '" + id + "' is a patch-domain study - ask for its patch atlas, not the triangle one"
                    : "study '" + id + "' has no atlas - it was not sampled on model surfaces";
        return false;
    }

    width = record.atlas.width;
    height = record.atlas.height;
    // ⚠️ SCATTERED FROM THE HOURS AS THEY STAND, so a study still converging
    // yields an atlas of the hours SO FAR -- which are too low. The caller is
    // told `converged` alongside and must not paint a final picture from a
    // partial one.
    image = ScatterToAtlas (record.atlas, record.session.SunHours ());
    return true;
}

bool SunStudyStore::PatchAtlasImage (const std::string& id, uint32_t& width, uint32_t& height,
                                     std::vector<float>& image, std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }

    const StudyRecord& record = *found->second;
    if (!record.IsPatchDomain () || !record.patchGrid.valid || record.patchAtlas.Width () == 0) {
        error = "study '" + id + "' is not a patch-domain study with a packed atlas";
        return false;
    }

    width = record.patchAtlas.Width ();
    height = record.patchAtlas.Height ();

    // ⚠️ THE SENTINEL IS THE INITIAL VALUE, NOT ZERO, AND IT IS NOT DECORATION.
    // Every texel no sample lands on -- gutters, the space between shelves, the
    // fragmentation the atlas deliberately never compacts away -- has to read as
    // "no sample here", which the shader discards. Zero would read as "nought
    // hours of sun", which is a legitimate measurement, and the unused half of
    // the atlas would paint as permanent shadow.
    image.assign (record.patchAtlas.TexelCount (), -1.0f);

    const std::vector<double>& hours = record.session.SunHours ();

    // Scattered PATCH BY PATCH through the atlas's own routine rather than
    // sample by sample through a flat loop. It is the same routine the
    // incremental path will use for a single surface, so the whole-study image
    // and a one-surface update cannot disagree about where a patch's texels are.
    std::vector<double> forSpan;
    for (size_t spanIndex = 0; spanIndex < record.patchGrid.spans.size (); ++spanIndex) {
        const PatchSampleSpan& span = record.patchGrid.spans[spanIndex];
        if (span.first + span.count > hours.size ())
            continue; // the study has not produced these yet
        forSpan.assign (hours.begin () + span.first, hours.begin () + span.first + span.count);
        record.patchAtlas.ScatterPatch (record.patchGrid, spanIndex, forSpan, image);
    }
    return true;
}

bool SunStudyStore::DisplayData (const std::string& id, std::vector<AtlasTile>& tiles, std::vector<FaceLayout>& layouts,
                                 uint32_t& width, uint32_t& height, double& spacing, std::vector<float>& image,
                                 double& daylightHours, bool& converged, uint64_t& generation, std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }

    const StudyRecord& record = *found->second;
    if (!record.atlas.valid) {
        // See the note in AtlasImage: the patch case needs its own words.
        error = record.IsPatchDomain ()
                    ? "study '" + id +
                          "' is a patch-domain study - the renderer needs patch tiles, which this call does not carry"
                    : "study '" + id + "' has no atlas - it was not sampled on model surfaces";
        return false;
    }

    tiles = record.atlas.tiles;
    layouts = record.sampleGrid.layouts;
    width = record.atlas.width;
    height = record.atlas.height;
    spacing = record.gridSpacing;
    image = ScatterToAtlas (record.atlas, record.session.SunHours ());
    daylightHours = record.series.DaylightHours ();
    const StudyProgress progress = record.session.Progress ();
    converged = progress.converged;
    generation = progress.generation;
    return true;
}

bool SunStudyStore::Describe (const std::string& id, StudyRecord& copyOfMetadata, std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }

    const StudyRecord& source = *found->second;
    copyOfMetadata.id = source.id;
    copyOfMetadata.timestepMinutes = source.timestepMinutes;
    copyOfMetadata.year = source.year;
    copyOfMetadata.month = source.month;
    copyOfMetadata.day = source.day;
    copyOfMetadata.hourFrom = source.hourFrom;
    copyOfMetadata.hourTo = source.hourTo;
    copyOfMetadata.minAltitudeDegrees = source.minAltitudeDegrees;
    copyOfMetadata.gridSpacing = source.gridSpacing;
    copyOfMetadata.groundPad = source.groundPad;
    copyOfMetadata.sourceStepCount = source.sourceStepCount;
    copyOfMetadata.analysisMilliseconds = source.analysisMilliseconds;
    return true;
}

bool SunStudyStore::Erase (const std::string& id)
{
    std::lock_guard<std::mutex> lock (mutex_);
    advancing_.erase (id);
    return studies_.erase (id) > 0;
}

void SunStudyStore::Clear ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    studies_.clear ();
    advancing_.clear ();
}

std::vector<std::string> SunStudyStore::Ids () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    std::vector<std::string> ids;
    ids.reserve (studies_.size ());
    for (const auto& entry : studies_)
        ids.push_back (entry.first);
    return ids;
}

size_t SunStudyStore::Count () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return studies_.size ();
}

} // namespace evp::sunstudy
