#include "SunStudy/SunStudyStore.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

namespace evp::sunstudy {

namespace {

SunStudyResultSummary SummarizeRecord (const StudyRecord& record)
{
    const auto& accumulator = record.session.Accumulator ();
    const double quantum = record.series.HoursPerStep ();
    return SummarizeSunHours (
        accumulator.SampleCount (), record.series.DaylightHours (),
        [&accumulator, quantum] (size_t sample) { return accumulator.LitStepCount (sample) * quantum; });
}

// The patch atlas image as the hours stand. Called with the store's lock HELD,
// by both readers that serve it, so the whole-study image has one definition.
std::vector<float> ScatterPatchImage (const StudyRecord& record)
{
    // ⚠️ THE SENTINEL IS THE INITIAL VALUE, NOT ZERO, AND IT IS NOT DECORATION.
    // Every texel no sample lands on -- gutters, the space between shelves, the
    // fragmentation the atlas deliberately never compacts away -- has to read as
    // "no sample here", which the shader discards. Zero would read as "nought
    // hours of sun", which is a legitimate measurement, and the unused half of
    // the atlas would paint as permanent shadow.
    std::vector<float> image (record.patchAtlas.TexelCount (), -1.0f);

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
    return image;
}

} // namespace

bool PatchFaceArrays (const PatchSampleGrid& grid, const SunStudyPatchAtlas& atlas, std::vector<AtlasTile>& tiles,
                      std::vector<FaceLayout>& layouts)
{
    tiles.clear ();
    layouts.clear ();
    if (!grid.valid || grid.patchOfTriangle.empty ())
        return false;

    // One tile and one layout per SPAN first, then fanned out per triangle: a
    // surface of forty triangles resolves its rectangle once, and every one of
    // the forty gets a bit-identical copy.
    std::vector<AtlasTile> spanTiles (grid.spans.size ());
    std::vector<FaceLayout> spanLayouts (grid.spans.size ());
    for (size_t index = 0; index < grid.spans.size (); ++index) {
        const PatchSampleSpan& span = grid.spans[index];
        const PatchAtlasAllocation* allocation = atlas.Find (span.key);
        if (allocation == nullptr || !allocation->Placed () || span.count == 0 ||
            span.first >= grid.cellColumns.size () || span.first >= grid.cellRows.size ())
            return false;

        AtlasTile& tile = spanTiles[index];
        FaceLayout& layout = spanLayouts[index];
        for (int axis = 0; axis < 3; ++axis) {
            layout.origin[axis] = span.origin[axis];
            layout.uAxis[axis] = span.uAxis[axis];
            layout.vAxis[axis] = span.vAxis[axis];
        }
        layout.uStart = 0;
        layout.vStart = 0;
        layout.gridded = true;

        if (span.count == 1) {
            // See the header: the surface's only measurement, everywhere on it.
            tile.x = allocation->x + std::min (grid.cellColumns[span.first], allocation->width - 1);
            tile.y = allocation->y + std::min (grid.cellRows[span.first], allocation->height - 1);
            tile.width = tile.height = 1;
            layout.columns = layout.rows = 1;
        }
        else {
            tile.x = allocation->x;
            tile.y = allocation->y;
            tile.width = allocation->width;
            tile.height = allocation->height;
            layout.columns = allocation->width;
            layout.rows = allocation->height;
        }
    }

    tiles.resize (grid.patchOfTriangle.size ());
    layouts.resize (grid.patchOfTriangle.size ());
    for (size_t face = 0; face < grid.patchOfTriangle.size (); ++face) {
        const uint32_t span = grid.patchOfTriangle[face];
        if (span == PatchSampleGrid::kNoPatch)
            continue; // a default AtlasTile is unplaced: ordinary shading
        if (span >= grid.spans.size ()) {
            tiles.clear ();
            layouts.clear ();
            return false;
        }
        tiles[face] = spanTiles[span];
        layouts[face] = spanLayouts[span];
    }
    return true;
}

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
    if (record->id.empty ()) {
        do {
            record->id = "sun-" + std::to_string (nextId_++);
        } while (studies_.find (record->id) != studies_.end ());
    }

    const std::string id = record->id;
    if (studies_.find (id) != studies_.end ())
        return std::string (); // never replace the session of an in-flight slice
    record->storeRevision = nextRevision_++;
    advancing_[id] = false;
    progress_[id] = record->session.Progress ();
    studies_[id] = std::move (record);
    return id;
}

bool SunStudyStore::Advance (const std::string& id, size_t maxSteps, size_t maxParallel, double tmin, double tmax,
                             size_t& advanced, std::string& error, const std::atomic<bool>* cancelled,
                             uint64_t expectedRevision)
{
    advanced = 0;

    std::shared_ptr<StudyRecord> record;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        const auto found = studies_.find (id);
        if (found == studies_.end ()) {
            error = "no sun study with id '" + id + "'";
            return false;
        }
        if (expectedRevision != 0 && found->second->storeRevision != expectedRevision) {
            error = "sun study record changed";
            return false;
        }
        if (progress_.at (id).converged)
            return true; // completed accumulator is an immutable reuse source
        if (found->second->reusedSamples != 0 && (tmin != 0.001 || tmax != 0.0)) {
            error = "incremental sun study requires its original ray bounds";
            return false;
        }
        // Per-study ownership remains independent of the calculation lane:
        // same-record re-entry refuses, different records queue cancellably.
        if (advancing_[id]) {
            error = "sun study '" + id + "' is already being advanced";
            return false;
        }
        advancing_[id] = true;
        record = found->second;
        if (tmin != 0.001 || tmax != 0.0)
            record->defaultRayBounds = false;
    }

    const auto queued = std::chrono::steady_clock::now ();
    auto start = queued;
    bool succeeded = true;
    std::unique_lock<std::timed_mutex> sessionLock (record->sessionMutex, std::defer_lock);
    try {
        std::unique_lock<std::timed_mutex> lane (executionMutex_, std::defer_lock);
        const auto isCancelled = [record, cancelled] () {
            return record->cancelRequested.load () || (cancelled != nullptr && cancelled->load ());
        };
        while (!isCancelled () && !lane.try_lock_for (std::chrono::milliseconds (10))) {
        }
        while (lane.owns_lock () && !isCancelled () && !sessionLock.try_lock_for (std::chrono::milliseconds (10))) {
        }
        start = std::chrono::steady_clock::now ();
        if (lane.owns_lock () && sessionLock.owns_lock () && !isCancelled () && record->traversal != nullptr)
            advanced = record->session.Advance (*record->traversal, maxSteps, tmin, tmax, maxParallel, isCancelled);
        if (sessionLock.owns_lock () && record->session.Progress ().converged) {
            record->resultSummary = SummarizeRecord (*record);
            record->summaryGeneration = record->session.Progress ().generation;
            record->summaryResolvedSteps = record->session.Progress ().resolvedSteps;
        }
    }
    catch (const std::exception& exception) {
        error = "sun study '" + id + "' advance failed: " + exception.what ();
        succeeded = false;
    }
    catch (...) {
        error = "sun study '" + id + "' advance failed";
        succeeded = false;
    }
    const double elapsed =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();

    {
        std::lock_guard<std::mutex> lock (mutex_);
        // Retained ownership protects the calculation; identity protects a new
        // study inserted under the same id after cancellation from a late finish.
        const auto found = studies_.find (id);
        if (found != studies_.end () && found->second == record) {
            found->second->analysisMilliseconds += elapsed;
            found->second->admissionMilliseconds += std::chrono::duration<double, std::milli> (start - queued).count ();
            progress_[id] = record->session.Progress ();
            advancing_[id] = false;
        }
    }
    return succeeded;
}

bool SunStudyStore::SessionReadable (const std::string& id, std::string& error) const
{
    const auto advancing = advancing_.find (id);
    if (advancing != advancing_.end () && advancing->second) {
        error = "computing";
        return false;
    }
    return true;
}

bool SunStudyStore::Progress (const std::string& id, StudyProgress& progress, std::string& error,
                              uint64_t expectedRevision) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }
    if (expectedRevision != 0 && found->second->storeRevision != expectedRevision) {
        error = "sun study record changed";
        return false;
    }
    progress = progress_.at (id);
    return true;
}

uint64_t SunStudyStore::Revision (const std::string& id) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    return found == studies_.end () ? 0 : found->second->storeRevision;
}

std::shared_ptr<const StudyRecord> SunStudyStore::CompletedRecord (const std::string& id) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end () || advancing_.at (id) || !progress_.at (id).converged)
        return nullptr;
    return found->second;
}

std::shared_ptr<const StudyRecord> SunStudyStore::LatestCompletedRecord () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    std::shared_ptr<const StudyRecord> latest;
    for (const auto& [id, record] : studies_)
        if (!advancing_.at (id) && progress_.at (id).converged &&
            (latest == nullptr || record->storeRevision > latest->storeRevision))
            latest = record;
    return latest;
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
    if (!SessionReadable (id, error))
        return false;
    hours = found->second->session.SunHours ();
    positions = found->second->positions;
    return true;
}

bool SunStudyStore::Results (const std::string& id, std::vector<double>& hours, std::vector<double>& positions,
                             std::vector<double>& normals, std::vector<uint8_t>* stepBits, std::string& error,
                             bool includePositions, StudyProgress* progress) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }

    if (!SessionReadable (id, error))
        return false;
    const StudyRecord& record = *found->second;
    if (progress != nullptr)
        *progress = progress_.at (id);
    hours = record.session.SunHours ();
    if (includePositions) {
        positions = record.positions;
        normals = record.normals;
    }
    else {
        positions.clear ();
        normals.clear ();
    }

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

bool SunStudyStore::Summary (const std::string& id, SunStudyResultSummary& summary, std::string& error,
                             StudyProgress* returnedProgress) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end () || !SessionReadable (id, error)) {
        if (found == studies_.end ())
            error = "no sun study with id '" + id + "'";
        return false;
    }
    const auto& record = *found->second;
    const auto progress = progress_.at (id);
    if (returnedProgress != nullptr)
        *returnedProgress = progress;
    if (record.summaryGeneration != progress.generation || record.summaryResolvedSteps != progress.resolvedSteps) {
        record.resultSummary = SummarizeRecord (record);
        record.summaryGeneration = progress.generation;
        record.summaryResolvedSteps = progress.resolvedSteps;
    }
    summary = record.resultSummary;
    return true;
}

bool SunStudyStore::ReadDisplayRecord (const std::string& id, uint64_t revision,
                                       const std::function<void (const StudyRecord&)>& read, std::string& error,
                                       const std::atomic<bool>* cancelled) const
{
    std::shared_ptr<StudyRecord> record;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        const auto found = studies_.find (id);
        if (found == studies_.end () || found->second->storeRevision != revision) {
            error = "sun study display source changed";
            return false;
        }
        record = found->second;
    }
    const auto stop = [&] { return record->cancelRequested.load () || (cancelled != nullptr && cancelled->load ()); };
    std::unique_lock<std::timed_mutex> session (record->sessionMutex, std::defer_lock);
    while (!stop () && !session.try_lock_for (std::chrono::milliseconds (10))) {
    }
    if (!session.owns_lock () || stop ()) {
        error = "sun study display cancelled";
        return false;
    }
    read (*record); // no store lock across atlas scatter, bit packing or map assembly
    if (stop ()) {
        error = "sun study display cancelled";
        return false;
    }
    return true;
}

bool SunStudyStore::DisplayInfo (const std::string& id, uint32_t& width, uint32_t& height, size_t& faces,
                                 double& daylightHours, uint64_t& revision, uint64_t& snapshotId,
                                 std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }
    const auto& record = *found->second;
    width = record.IsPatchDomain () ? record.patchAtlas.Width () : record.atlas.width;
    height = record.IsPatchDomain () ? record.patchAtlas.Height () : record.atlas.height;
    faces = record.IsPatchDomain () ? record.patchGrid.patchOfTriangle.size () : record.atlas.tiles.size ();
    daylightHours = record.series.DaylightHours ();
    revision = record.storeRevision;
    snapshotId = record.snapshotId;
    if (width == 0 || height == 0 || (!record.IsPatchDomain () && !record.atlas.valid)) {
        error = "sun study has no model-surface atlas";
        return false;
    }
    return true;
}

bool SunStudyStore::PublishDisplayRecord (const std::string& id, uint64_t revision,
                                          const std::function<void ()>& enqueue) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end () || found->second->storeRevision != revision ||
        found->second->cancelRequested.load () || !enqueue)
        return false;
    enqueue ();
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

    if (!SessionReadable (id, error))
        return false;
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

    if (!SessionReadable (id, error))
        return false;
    const StudyRecord& record = *found->second;
    if (!record.IsPatchDomain () || !record.patchGrid.valid || record.patchAtlas.Width () == 0) {
        error = "study '" + id + "' is not a patch-domain study with a packed atlas";
        return false;
    }

    width = record.patchAtlas.Width ();
    height = record.patchAtlas.Height ();
    image = ScatterPatchImage (record);
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

    if (!SessionReadable (id, error))
        return false;
    const StudyRecord& record = *found->second;
    if (record.IsPatchDomain ()) {
        if (!record.patchGrid.valid || record.patchAtlas.Width () == 0) {
            error = "study '" + id + "' is a patch-domain study with no packed patch atlas";
            return false;
        }
        if (!PatchFaceArrays (record.patchGrid, record.patchAtlas, tiles, layouts)) {
            // ⚠️ A REFUSAL, NOT A PARTIAL ANSWER. A span with no rectangle means
            // the atlas and the grid are not one study; drawing the rest would
            // leave one surface wearing whatever texels it happened to address.
            error = "study '" + id + "' has a patch the patch atlas did not place - the two are not one study";
            return false;
        }
        width = record.patchAtlas.Width ();
        height = record.patchAtlas.Height ();
        image = ScatterPatchImage (record);
    }
    else {
        if (!record.atlas.valid) {
            error = "study '" + id + "' has no atlas - it was not sampled on model surfaces";
            return false;
        }
        tiles = record.atlas.tiles;
        layouts = record.sampleGrid.layouts;
        width = record.atlas.width;
        height = record.atlas.height;
        image = ScatterToAtlas (record.atlas, record.session.SunHours ());
    }

    spacing = record.gridSpacing;
    daylightHours = record.series.DaylightHours ();
    const StudyProgress progress = progress_.at (id);
    converged = progress.converged;
    generation = progress.generation;
    return true;
}

bool SunStudyStore::ReadAt (const std::string& id, uint64_t snapshotId, size_t face, size_t meshIndex,
                            const double point[3], SunStudyReading& reading, uint8_t& role, double& daylightHours,
                            std::string& error) const
{
    reading = SunStudyReading ();
    role = 0xff;
    daylightHours = 0.0;
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }
    if (!SessionReadable (id, error))
        return false;
    const StudyRecord& record = *found->second;
    if (record.snapshotId != snapshotId) {
        error = "stale";
        return false;
    }
    if (meshIndex < record.elementRoles.size ())
        role = record.elementRoles[meshIndex];
    daylightHours = record.series.DaylightHours ();
    const std::vector<double>& hours = record.session.SunHours ();
    reading = record.IsPatchDomain () ? ReadPatchStudyAt (record.patchGrid, record.gridSpacing, hours, face, point)
                                      : ReadTriangleStudyAt (record.sampleGrid, record.gridSpacing, hours, face, point);
    return true;
}

bool SunStudyStore::StepMasks (const std::string& id, StepMaskAtlas& atlas, std::vector<uint16_t>& stepMinutes,
                               uint32_t& noonStep, std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }
    if (!SessionReadable (id, error))
        return false;
    const StudyRecord& record = *found->second;
    const OcclusionAccumulator& accumulator = record.session.Accumulator ();

    // Where each sample's value sits in the atlas this study DISPLAYS -- the
    // same table the hours image was scattered through, so the two images
    // agree texel for texel.
    std::vector<int64_t> texelOf (accumulator.SampleCount (), -1);
    uint32_t width = 0;
    uint32_t height = 0;
    if (record.IsPatchDomain ()) {
        if (!record.patchGrid.valid || record.patchAtlas.Width () == 0) {
            error = "study '" + id + "' has no packed patch atlas";
            return false;
        }
        width = record.patchAtlas.Width ();
        height = record.patchAtlas.Height ();
        for (size_t sample = 0; sample < texelOf.size (); ++sample)
            texelOf[sample] = record.patchAtlas.TexelOf (record.patchGrid, sample);
    }
    else {
        if (!record.atlas.valid) {
            error = "study '" + id + "' has no atlas - it was not sampled on model surfaces";
            return false;
        }
        width = record.atlas.width;
        height = record.atlas.height;
        for (size_t sample = 0; sample < texelOf.size () && sample < record.atlas.texels.size (); ++sample)
            texelOf[sample] = record.atlas.texels[sample];
    }

    atlas = PackStepMasks (
        accumulator.SampleCount (), accumulator.StepCount (),
        [&accumulator] (size_t sample, size_t step) { return accumulator.Lit (sample, step); }, texelOf, width, height);
    stepMinutes = StepMinutes (record.series);
    noonStep = SolarNoonStep (record.series);
    return true;
}

bool SunStudyStore::Footprint (const std::string& id, size_t& samples, double& analysedArea, std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end ()) {
        error = "no sun study with id '" + id + "'";
        return false;
    }
    const StudyRecord& record = *found->second;
    samples = record.positions.size () / 3;
    analysedArea = 0.0;
    if (record.IsPatchDomain ())
        analysedArea = record.patchGrid.TotalArea ();
    else
        for (const double area : record.sampleGrid.areas)
            analysedArea += area;
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
    copyOfMetadata.snapshotId = source.snapshotId;
    copyOfMetadata.reusedSamples = source.reusedSamples;
    copyOfMetadata.defaultRayBounds = source.defaultRayBounds;
    copyOfMetadata.timestepMinutes = source.timestepMinutes;
    copyOfMetadata.year = source.year;
    copyOfMetadata.month = source.month;
    copyOfMetadata.day = source.day;
    copyOfMetadata.hourFrom = source.hourFrom;
    copyOfMetadata.hourTo = source.hourTo;
    copyOfMetadata.minAltitudeDegrees = source.minAltitudeDegrees;
    copyOfMetadata.gridSpacing = source.gridSpacing;
    // ⚠️ THE DOMAIN TRAVELS WITH THE METADATA. The follower adopts its rerun
    // configuration from this copy; without it every rerun was a triangle study.
    copyOfMetadata.domain = source.domain;
    // The roles likewise: a rerun that forgot them would analyse the context.
    copyOfMetadata.analysisElements = source.analysisElements;
    copyOfMetadata.contextElements = source.contextElements;
    copyOfMetadata.ignoredElements = source.ignoredElements;
    copyOfMetadata.selectionBinding = source.selectionBinding;
    copyOfMetadata.preset = source.preset;
    copyOfMetadata.glassThreshold = source.glassThreshold;
    copyOfMetadata.analysisRestricted = source.analysisRestricted;
    copyOfMetadata.elementRoles = source.elementRoles;
    copyOfMetadata.groundPad = source.groundPad;
    copyOfMetadata.sourceStepCount = source.sourceStepCount;
    copyOfMetadata.placeInputHash = source.placeInputHash;
    copyOfMetadata.analysisMilliseconds = source.analysisMilliseconds;
    copyOfMetadata.admissionMilliseconds = source.admissionMilliseconds;
    copyOfMetadata.backend = source.backend;
    return true;
}

bool SunStudyStore::Erase (const std::string& id, uint64_t expectedRevision)
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (expectedRevision != 0 && (found == studies_.end () || found->second->storeRevision != expectedRevision))
        return false;
    if (found != studies_.end ())
        found->second->cancelRequested.store (true);
    advancing_.erase (id);
    progress_.erase (id);
    return studies_.erase (id) > 0;
}

void SunStudyStore::Clear ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    for (const auto& entry : studies_)
        entry.second->cancelRequested.store (true);
    studies_.clear ();
    advancing_.clear ();
    progress_.clear ();
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
