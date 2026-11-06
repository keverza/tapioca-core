#include "SunStudy/SunStudyStore.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustStudy.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>

namespace evp::sunstudy {

namespace {

// The hours a reading is out of: the day's daylight, or the period a finished
// day study presents instead (#152). The core's info carries whichever holds.
double ReadCeiling (const StudyRecord& record)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    TapiocaStudyInfo info {};
    std::string ignored;
    if (record.rustCore != nullptr && record.rustCore->Info (info, ignored))
        return info.daylightHours;
#endif
    return record.series.DaylightHours ();
}

} // namespace

namespace {

SunStudyResultSummary SummarizeRecord (const StudyRecord& record)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (record.rustCore != nullptr) {
        SunStudyResultSummary summary;
        std::string error;
        record.rustCore->Summary (summary, error);
        return summary;
    }
#else
    (void) record;
#endif
    return SunStudyResultSummary ();
}

#if defined(TAPIOCA_GEOMETRY_RUST)
// The store's refusal text for a Rust read status, as the C++ readers word it.
std::string RefusalText (const std::string& id, bool patchDomain, rust::StudyReadStatus status)
{
    switch (status) {
        case rust::StudyReadStatus::NoTriangleAtlas:
            return patchDomain
                       ? "study '" + id + "' is a patch-domain study - ask for its patch atlas, not the triangle one"
                       : "study '" + id + "' has no atlas - it was not sampled on model surfaces";
        case rust::StudyReadStatus::NoPatchAtlas:
            return "study '" + id + "' is not a patch-domain study with a packed atlas";
        case rust::StudyReadStatus::UnplacedPatch:
            return "study '" + id + "' has a patch the patch atlas did not place - the two are not one study";
        case rust::StudyReadStatus::Ok:
            break;
    }
    return std::string ();
}
#endif

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
    progress_[id] = record->CurrentProgress ();
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
        if ((found->second->reusedSamples != 0 || found->second->reusedSampleSteps != 0) &&
            (tmin != 0.001 || tmax != 0.0)) {
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
        const bool ready =
            lane.owns_lock () && sessionLock.owns_lock () && !isCancelled () && record->traversal != nullptr;
        if (ready && !record->positions.empty () && !record->series.Empty ()) {
            // The whole day, every advance: a backend that pays per request
            // answers it once and ignores the repeats (ITraversal::PrepareSteps).
            std::vector<double> directions;
            directions.reserve (record->series.StepCount () * 3);
            for (const SunStep& step : record->series.Steps ())
                directions.insert (directions.end (), step.direction, step.direction + 3);
            record->traversal->PrepareSteps (record->positions.data (), record->positions.size () / 3,
                                             directions.data (), record->series.StepCount (), tmin, tmax, isCancelled);
        }
#if defined(TAPIOCA_GEOMETRY_RUST)
        if (ready && record->rustCore != nullptr) {
            std::string coreError;
            if (!record->rustCore->Advance (*record->traversal, maxSteps, tmin, tmax, maxParallel, isCancelled,
                                            record->rustStepObserver, advanced, coreError))
                throw std::runtime_error (coreError);
        }
        else
#else
        (void) maxSteps;
        (void) maxParallel;
#endif
            if (ready)
            throw std::runtime_error (NoStudyCoreError ());
        const StudyProgress now = record->CurrentProgress ();
        if (sessionLock.owns_lock () && now.converged) {
            record->resultSummary = SummarizeRecord (*record);
            record->summaryGeneration = now.generation;
            record->summaryResolvedSteps = now.resolvedSteps;
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
            progress_[id] = record->CurrentProgress ();
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
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (found->second->rustCore != nullptr) {
        if (!found->second->rustCore->SunHours (hours, error))
            return false;
        positions = found->second->positions;
        return true;
    }
#endif
    (void) hours;
    (void) positions;
    error = NoStudyCoreError ();
    return false;
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
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (record.rustCore != nullptr) {
        if (!record.rustCore->SunHours (hours, error) ||
            (stepBits != nullptr && !record.rustCore->StepBits (*stepBits, error)))
            return false;
        positions = includePositions ? record.positions : std::vector<double> ();
        normals = includePositions ? record.normals : std::vector<double> ();
        return true;
    }
#endif
    (void) record;
    (void) hours;
    (void) positions;
    (void) normals;
    (void) includePositions;
    (void) stepBits;
    error = NoStudyCoreError ();
    return false;
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
    daylightHours = ReadCeiling (record);
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
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (record.rustCore != nullptr) {
        rust::StudyReadStatus status = rust::StudyReadStatus::Ok;
        if (!record.rustCore->AtlasImage (width, height, image, status, error))
            return false;
        error = RefusalText (id, record.IsPatchDomain (), status);
        return status == rust::StudyReadStatus::Ok;
    }
#endif
    (void) record;
    (void) width;
    (void) height;
    (void) image;
    error = NoStudyCoreError ();
    return false;
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
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (record.rustCore != nullptr) {
        rust::StudyReadStatus status = rust::StudyReadStatus::Ok;
        if (!record.rustCore->PatchAtlasImage (width, height, image, status, error))
            return false;
        error = RefusalText (id, record.IsPatchDomain (), status);
        return status == rust::StudyReadStatus::Ok;
    }
#endif
    (void) record;
    (void) width;
    (void) height;
    (void) image;
    error = NoStudyCoreError ();
    return false;
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
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (record.rustCore != nullptr) {
        rust::StudyReadStatus status = rust::StudyReadStatus::Ok;
        if (!record.rustCore->DisplayData (tiles, layouts, width, height, image, status, error))
            return false;
        if (status != rust::StudyReadStatus::Ok) {
            error = record.IsPatchDomain () && status == rust::StudyReadStatus::NoPatchAtlas
                        ? "study '" + id + "' is a patch-domain study with no packed patch atlas"
                    : status == rust::StudyReadStatus::NoTriangleAtlas
                        ? "study '" + id + "' has no atlas - it was not sampled on model surfaces"
                        : RefusalText (id, record.IsPatchDomain (), status);
            return false;
        }
        spacing = record.gridSpacing;
        daylightHours = ReadCeiling (record);
        const StudyProgress progress = progress_.at (id);
        converged = progress.converged;
        generation = progress.generation;
        return true;
    }
#endif
    (void) record;
    (void) tiles;
    (void) layouts;
    (void) width;
    (void) height;
    (void) spacing;
    (void) image;
    (void) daylightHours;
    (void) converged;
    (void) generation;
    error = NoStudyCoreError ();
    return false;
}

bool SunStudyStore::ReadAt (const std::string& id, uint64_t snapshotId, size_t face, size_t meshIndex,
                            const double point[3], SunStudyReading& reading, uint8_t& role, double& daylightHours,
                            std::string& error, std::vector<uint8_t>* litSteps) const
{
    reading = SunStudyReading ();
    if (litSteps != nullptr)
        litSteps->clear ();
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
    daylightHours = ReadCeiling (record);
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (record.rustCore != nullptr) {
        if (!record.rustCore->ReadAt (face, point, reading, error))
            return false;
        return litSteps == nullptr || !reading.measured ||
               record.rustCore->SampleSteps (reading.sample, *litSteps, error);
    }
#endif
    (void) face;
    (void) point;
    error = NoStudyCoreError ();
    return false;
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
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (record.rustCore != nullptr) {
        rust::StudyReadStatus status = rust::StudyReadStatus::Ok;
        if (!record.rustCore->StepMasks (atlas, stepMinutes, noonStep, status, error))
            return false;
        if (status == rust::StudyReadStatus::NoPatchAtlas)
            error = "study '" + id + "' has no packed patch atlas";
        else if (status != rust::StudyReadStatus::Ok)
            error = "study '" + id + "' has no atlas - it was not sampled on model surfaces";
        return status == rust::StudyReadStatus::Ok;
    }
#endif
    (void) record;
    (void) atlas;
    (void) stepMinutes;
    (void) noonStep;
    error = NoStudyCoreError ();
    return false;
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
    copyOfMetadata.reusedSampleSteps = source.reusedSampleSteps;
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
    // And the cylinder: a rerun without it would sample the whole model.
    copyOfMetadata.clipX = source.clipX;
    copyOfMetadata.clipY = source.clipY;
    copyOfMetadata.clipRadius = source.clipRadius;
    copyOfMetadata.elementRoles = source.elementRoles;
    copyOfMetadata.groundPad = source.groundPad;
    copyOfMetadata.sourceStepCount = source.sourceStepCount;
    copyOfMetadata.placeInputHash = source.placeInputHash;
    copyOfMetadata.analysisMilliseconds = source.analysisMilliseconds;
    copyOfMetadata.admissionMilliseconds = source.admissionMilliseconds;
    copyOfMetadata.backend = source.backend;
    return true;
}

bool SunStudyStore::Backend (const std::string& id, SunStudyBackendReport& report, std::string& error) const
{
    std::string requested;
    std::shared_ptr<const ITraversal> traversal;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        const auto found = studies_.find (id);
        if (found == studies_.end ()) {
            error = "no sun study with id '" + id + "'";
            return false;
        }
        requested = found->second->backend;
        traversal = found->second->traversal;
    }
    report = ReportSunStudyBackend (requested, traversal.get ());
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

bool SunStudyStore::Represent (const std::string& id, uint64_t expectedRevision,
                               const std::function<bool (const StudyRecord&, std::string&)>& present,
                               std::string& error)
{
    std::lock_guard<std::mutex> lock (mutex_);
    const auto found = studies_.find (id);
    if (found == studies_.end () || (expectedRevision != 0 && found->second->storeRevision != expectedRevision)) {
        error = "no sun study with id '" + id + "' at that revision";
        return false;
    }
    if (advancing_.at (id) || !progress_.at (id).converged) {
        error = "sun study '" + id + "' is not a converged, idle study";
        return false;
    }
    const StudyRecord& record = *found->second;
    if (!present (record, error))
        return false;
    record.summaryGeneration = UINT64_MAX;
    record.summaryResolvedSteps = SIZE_MAX;
    record.displayCache.reset ();
    return true;
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
