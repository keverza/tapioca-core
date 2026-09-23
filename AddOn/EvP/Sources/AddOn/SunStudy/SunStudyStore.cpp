#include "SunStudy/SunStudyStore.hpp"

#include <algorithm>
#include <chrono>

namespace evp::sunstudy {

namespace {

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
    const StudyProgress progress = record.session.Progress ();
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
    const auto advancing = advancing_.find (id);
    if (advancing != advancing_.end () && advancing->second) {
        error = "computing";
        return false;
    }
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
    // ⚠️ THE DOMAIN TRAVELS WITH THE METADATA. The follower adopts its rerun
    // configuration from this copy; without it every rerun was a triangle study.
    copyOfMetadata.domain = source.domain;
    // The roles likewise: a rerun that forgot them would analyse the context.
    copyOfMetadata.analysisElements = source.analysisElements;
    copyOfMetadata.contextElements = source.contextElements;
    copyOfMetadata.ignoredElements = source.ignoredElements;
    copyOfMetadata.elementRoles = source.elementRoles;
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
