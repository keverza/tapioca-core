#include "SunStudy/SunStudyRustStudy.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"
#include "Geometry/Mesh.hpp"

#include <algorithm>

namespace evp::sunstudy::rust {

using namespace detail;

static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyCreateRequest) == 440);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyMeshesView) == 16);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyReuseRequest) == 96);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyReuseOutcome) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyAdvanceRequest) == 56);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyInfo) == 88);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyArraysOutput) == 48);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyImageOutput) == 48);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyImageSummary) == 32);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyStepMaskOutput) == 32);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyStepMaskSummary) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyReadAtRequest) == 32);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStudyReading) == 32);

namespace {

// The day as the core adopts it, with its step PODs kept alive.
struct SeriesPods {
    std::vector<TapiocaSunStep> steps;
    TapiocaSeriesView view {};

    explicit SeriesPods (const SunSeries& series)
    {
        for (const SunStep& step : series.Steps ())
            steps.push_back (ToPod (step));
        view = {
            { steps.data (), steps.size () }, series.TimestepMinutes (), series.SourceStepCount (), series.Version ()
        };
    }
};

void ForwardObservation (void* context, size_t step, double wallMs, const TapiocaOcclusionState* state)
{
    try {
        const auto* observer = static_cast<const RustStudyCore::StepObserver*> (context);
        if (observer != nullptr && *observer && state != nullptr)
            (*observer) (step, *state, wallMs);
    }
    catch (...) {
        // Diagnostics must not fail a completed step, and nothing unwinds into Rust.
    }
}

uint8_t PollAtomic (void* context)
{
    const auto* cancelled = static_cast<const std::atomic<bool>*> (context);
    return cancelled != nullptr && cancelled->load () ? 1 : 0;
}

TapiocaSunStudyStudy* Create (const TapiocaStudyCreateRequest& request, std::string& error)
{
    TapiocaSunStudyStudy* handle = nullptr;
    uint8_t outcome = 0;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_create (&request, &handle, &outcome, m, n); },
                 error))
        return nullptr;
    if (outcome == 1)
        error = "the study's sample arrays are inconsistent";
    else if (outcome == 2)
        error = "the patch atlas could not be packed within the maximum texture dimension";
    return outcome == 0 ? handle : nullptr;
}

} // namespace

RustStudyCore::RustStudyCore (TapiocaSunStudyStudy* handle) : handle_ (handle)
{
}

RustStudyCore::~RustStudyCore ()
{
    std::string ignored;
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_destroy (handle_, m, n); }, ignored);
}

std::unique_ptr<RustStudyCore> RustStudyCore::Ground (const SunSeries& series, double spacing,
                                                      const std::vector<double>& positions,
                                                      const std::vector<double>& normals, std::string& error)
{
    const SeriesPods day (series);
    TapiocaStudyCreateRequest request {};
    request.domain = 0;
    request.series = day.view;
    request.spacing = spacing;
    request.positions = { positions.data (), positions.size () };
    request.normals = { normals.data (), normals.size () };
    TapiocaSunStudyStudy* handle = Create (request, error);
    return handle != nullptr ? std::unique_ptr<RustStudyCore> (new RustStudyCore (handle)) : nullptr;
}

std::unique_ptr<RustStudyCore> RustStudyCore::Triangle (const SunSeries& series, double spacing,
                                                        const TriangleInputs& inputs, const RustStudyCore* previous,
                                                        std::string& error)
{
    if (inputs.grid == nullptr || inputs.snapshot == nullptr) {
        error = "a triangle study needs its grid and snapshot";
        return nullptr;
    }
    const SampleGrid& t = *inputs.grid;
    const SeriesPods day (series);
    std::vector<TapiocaFaceLayout> layouts;
    layouts.reserve (t.layouts.size ());
    for (const FaceLayout& layout : t.layouts)
        layouts.push_back (LayoutPod (layout));
    std::vector<TapiocaAtlasMeshView> meshes;
    for (const geomsrv::Mesh& mesh : inputs.snapshot->meshes)
        meshes.push_back ({ { mesh.guid.data (), mesh.guid.size () }, mesh.TriangleCount () });
    TapiocaStudyCreateRequest request {};
    request.domain = 1;
    request.series = day.view;
    request.spacing = spacing;
    request.sampleGrid = { { t.positions.data (), t.positions.size () },
                           { t.normals.data (), t.normals.size () },
                           { t.areas.data (), t.areas.size () },
                           { t.faces.data (), t.faces.size () },
                           { t.groups.data (), t.groups.size () },
                           { layouts.data (), layouts.size () },
                           { t.cellColumns.data (), t.cellColumns.size () },
                           { t.cellRows.data (), t.cellRows.size () },
                           { t.degenerateFaces, t.excludedFaces, t.undersizedFaces },
                           static_cast<uint8_t> (t.valid ? 1 : 0) };
    request.meshes = { meshes.data (), meshes.size () };
    request.atlas[0] = inputs.options.gutter;
    request.atlas[1] = inputs.options.maxDimension;
    request.previous = previous != nullptr ? previous->handle_ : nullptr;
    TapiocaSunStudyStudy* handle = Create (request, error);
    return handle != nullptr ? std::unique_ptr<RustStudyCore> (new RustStudyCore (handle)) : nullptr;
}

std::unique_ptr<RustStudyCore> RustStudyCore::Patch (const SunSeries& series, double spacing,
                                                     const PatchSampleGrid& grid, const RustStudyCore* previous,
                                                     uint32_t maxDimension, std::string& error)
{
    const SeriesPods day (series);
    std::vector<TapiocaPatchSpanEntry> spans;
    spans.reserve (grid.spans.size ());
    for (const PatchSampleSpan& span : grid.spans)
        spans.push_back ({ SpanPod (span), { span.key.element.data (), span.key.element.size () } });
    const PatchSampleGrid& p = grid;
    TapiocaStudyCreateRequest request {};
    request.domain = 2;
    request.series = day.view;
    request.spacing = spacing;
    request.patchGrid = { { p.positions.data (), p.positions.size () },
                          { p.normals.data (), p.normals.size () },
                          { p.areas.data (), p.areas.size () },
                          { p.cellColumns.data (), p.cellColumns.size () },
                          { p.cellRows.data (), p.cellRows.size () },
                          { p.spanOf.data (), p.spanOf.size () },
                          { spans.data (), spans.size () },
                          { p.patchOfTriangle.data (), p.patchOfTriangle.size () },
                          { p.degenerateFaces, p.centroidPatches, p.excludedPatches },
                          static_cast<uint8_t> (p.sampleLimitExceeded ? 1 : 0),
                          static_cast<uint8_t> (p.valid ? 1 : 0) };
    request.atlas[0] = 1;
    request.atlas[1] = maxDimension;
    request.previous = previous != nullptr ? previous->handle_ : nullptr;
    TapiocaSunStudyStudy* handle = Create (request, error);
    return handle != nullptr ? std::unique_ptr<RustStudyCore> (new RustStudyCore (handle)) : nullptr;
}

bool RustStudyCore::Sync (const StudyInputs& inputs, std::string& error) const
{
    const uint64_t versions[3] = { inputs.geometryVersion, inputs.sunVersion, inputs.gridVersion };
    return Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_sync (handle_, &versions, m, n); }, error);
}

bool RustStudyCore::SetSampleMeshes (const geomsrv::Snapshot& snapshot, std::string& error)
{
    const std::vector<TapiocaReuseMeshView> meshes = MeshViews (snapshot);
    const TapiocaStudyMeshesView view { { meshes.data (), meshes.size () } };
    return Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_set_sample_meshes (handle_, &view, m, n); },
                   error);
}

bool RustStudyCore::ReuseFrom (const RustStudyCore& source, const ReuseInputs& inputs, size_t& reusedSamples,
                               size_t& reusedSampleSteps, std::string& error)
{
    reusedSamples = reusedSampleSteps = 0;
    if (inputs.sourceSnapshot == nullptr || inputs.targetSnapshot == nullptr || inputs.sourceRoles == nullptr ||
        inputs.targetRoles == nullptr) {
        error = "reuse needs both snapshots and both role lists";
        return false;
    }
    const std::vector<TapiocaReuseMeshView> before = MeshViews (*inputs.sourceSnapshot);
    const std::vector<TapiocaReuseMeshView> after = MeshViews (*inputs.targetSnapshot);
    TapiocaStudyReuseRequest request {};
    request.source = source.handle_;
    request.sourceMeshes = { before.data (), before.size () };
    request.sourceRoles = { inputs.sourceRoles->data (), inputs.sourceRoles->size () };
    request.targetMeshes = { after.data (), after.size () };
    request.targetRoles = { inputs.targetRoles->data (), inputs.targetRoles->size () };
    request.sourceDefaultRayBounds = inputs.sourceDefaultRayBounds ? 1 : 0;
    request.cancel = { const_cast<std::atomic<bool>*> (inputs.cancelled), &PollAtomic };
    TapiocaStudyReuseOutcome outcome {};
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_reuse (handle_, &request, &outcome, m, n); },
                 error))
        return false;
    if (outcome.seeded != 0) {
        reusedSamples = outcome.reusedSamples;
        reusedSampleSteps = outcome.reusedSampleSteps;
    }
    return true;
}

bool RustStudyCore::Advance (const ITraversal& traversal, size_t maxSteps, double tmin, double tmax, size_t maxParallel,
                             const std::function<bool ()>& isCancelled, const StepObserver& observer, size_t& advanced,
                             std::string& error) const
{
    advanced = 0;
    TraversalShim shim { &traversal, &isCancelled };
    const TapiocaTraversalVtable vtable = VtableOf (shim);
    TapiocaStudyAdvanceRequest request {};
    request.traversal = &vtable;
    request.tmin = tmin;
    request.tmax = tmax;
    request.maxParallel = maxParallel;
    request.maxSteps = maxSteps;
    if (observer)
        request.observer = { const_cast<StepObserver*> (&observer), &ForwardObservation };
    return Invoke (
        [&] (char* m, size_t n) { return tapioca_sunstudy_study_advance (handle_, &request, &advanced, m, n); }, error);
}

bool RustStudyCore::Progress (StudyProgress& progress, std::string& error) const
{
    TapiocaSessionProgress raw {};
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_progress (handle_, &raw, m, n); }, error))
        return false;
    progress.generation = raw.generation;
    progress.resolvedSteps = raw.resolvedSteps;
    progress.totalSteps = raw.totalSteps;
    progress.sampleCount = raw.sampleCount;
    progress.converged = raw.converged != 0;
    progress.empty = raw.empty != 0;
    return true;
}

bool RustStudyCore::Info (TapiocaStudyInfo& info, std::string& error) const
{
    return Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_info (handle_, &info, m, n); }, error);
}

bool RustStudyCore::SunHours (std::vector<double>& hours, std::string& error) const
{
    TapiocaStudyInfo info {};
    if (!Info (info, error))
        return false;
    hours.assign (info.sampleCount, 0.0);
    TapiocaStudyArraysOutput output {};
    output.hours = hours.data ();
    output.hoursCapacity = hours.size ();
    return hours.empty () ||
           Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_arrays (handle_, &output, m, n); }, error);
}

bool RustStudyCore::StepBits (std::vector<uint8_t>& bits, std::string& error) const
{
    TapiocaStudyInfo info {};
    if (!Info (info, error))
        return false;
    bits.assign (info.sampleCount * info.stepCount, 0);
    TapiocaStudyArraysOutput output {};
    output.stepBits = bits.data ();
    output.stepBitsCapacity = bits.size ();
    return bits.empty () ||
           Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_arrays (handle_, &output, m, n); }, error);
}

bool RustStudyCore::SampleMeshes (std::vector<uint32_t>& meshes, std::string& error) const
{
    TapiocaStudyInfo info {};
    if (!Info (info, error))
        return false;
    meshes.assign (info.sampleMeshCount, 0);
    TapiocaStudyArraysOutput output {};
    output.sampleMeshes = meshes.data ();
    output.sampleMeshesCapacity = meshes.size ();
    return meshes.empty () ||
           Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_arrays (handle_, &output, m, n); }, error);
}

bool RustStudyCore::Summary (SunStudyResultSummary& summary, std::string& error) const
{
    TapiocaResultSummary raw {};
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_summary (handle_, &raw, m, n); }, error))
        return false;
    summary.count = raw.count;
    summary.fullyLit = raw.fullyLit;
    summary.fullyShaded = raw.fullyShaded;
    summary.minHours = raw.minHours;
    summary.meanHours = raw.meanHours;
    summary.maxHours = raw.maxHours;
    summary.daylightHours = raw.daylightHours;
    return true;
}

bool RustStudyCore::Image (uint8_t kind, std::vector<AtlasTile>* tiles, std::vector<FaceLayout>* layouts,
                           uint32_t& width, uint32_t& height, std::vector<float>& image, StudyReadStatus& status,
                           std::string& error) const
{
    TapiocaStudyImageOutput output {};
    output.kind = kind;
    TapiocaStudyImageSummary summary {};
    const auto read = [&] (char* m, size_t n) {
        return tapioca_sunstudy_study_image (handle_, &output, &summary, m, n);
    };
    if (!Invoke (read, error)) // shape
        return false;
    status = static_cast<StudyReadStatus> (summary.status);
    if (status != StudyReadStatus::Ok)
        return true;
    image.assign (summary.texels, 0.0f);
    std::vector<TapiocaAtlasTile> tilePods (summary.faces);
    std::vector<TapiocaFaceLayout> layoutPods (summary.faces);
    output.image = image.data ();
    output.imageCapacity = image.size ();
    output.tiles = tilePods.data ();
    output.layouts = layoutPods.data ();
    output.faceCapacity = summary.faces;
    if ((summary.texels != 0 || summary.faces != 0) && !Invoke (read, error)) // fill
        return false;
    width = summary.width;
    height = summary.height;
    if (tiles != nullptr && layouts != nullptr) {
        tiles->clear ();
        layouts->clear ();
        for (const TapiocaAtlasTile& t : tilePods)
            tiles->push_back ({ t.x, t.y, t.width, t.height });
        for (const TapiocaFaceLayout& l : layoutPods)
            layouts->push_back (LayoutOf (l));
    }
    return true;
}

bool RustStudyCore::AtlasImage (uint32_t& width, uint32_t& height, std::vector<float>& image, StudyReadStatus& status,
                                std::string& error) const
{
    return Image (0, nullptr, nullptr, width, height, image, status, error);
}

bool RustStudyCore::PatchAtlasImage (uint32_t& width, uint32_t& height, std::vector<float>& image,
                                     StudyReadStatus& status, std::string& error) const
{
    return Image (1, nullptr, nullptr, width, height, image, status, error);
}

bool RustStudyCore::DisplayData (std::vector<AtlasTile>& tiles, std::vector<FaceLayout>& layouts, uint32_t& width,
                                 uint32_t& height, std::vector<float>& image, StudyReadStatus& status,
                                 std::string& error) const
{
    return Image (2, &tiles, &layouts, width, height, image, status, error);
}

bool RustStudyCore::StepMasks (StepMaskAtlas& atlas, std::vector<uint16_t>& stepMinutes, uint32_t& noonStep,
                               StudyReadStatus& status, std::string& error) const
{
    TapiocaStudyStepMaskOutput output {};
    TapiocaStudyStepMaskSummary summary {};
    const auto read = [&] (char* m, size_t n) {
        return tapioca_sunstudy_study_step_masks (handle_, &output, &summary, m, n);
    };
    if (!Invoke (read, error)) // shape
        return false;
    status = static_cast<StudyReadStatus> (summary.status);
    if (status != StudyReadStatus::Ok)
        return true;
    atlas.masks.assign (summary.maskCount, 0u);
    stepMinutes.assign (summary.minuteCount, 0u);
    output = { atlas.masks.data (), atlas.masks.size (), stepMinutes.data (), stepMinutes.size () };
    if ((summary.maskCount != 0 || summary.minuteCount != 0) && !Invoke (read, error)) // fill
        return false;
    atlas.width = summary.shape[0];
    atlas.height = summary.shape[1];
    atlas.words = summary.shape[2];
    atlas.steps = summary.shape[3];
    noonStep = summary.shape[4];
    return true;
}

bool RustStudyCore::ReadAt (size_t face, const double point[3], SunStudyReading& reading, std::string& error) const
{
    reading = SunStudyReading ();
    if (point == nullptr)
        return true; // as the C++ reader: no point, no measurement
    TapiocaStudyReadAtRequest request {};
    request.face = face;
    std::copy (point, point + 3, request.point);
    TapiocaStudyReading out {};
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_study_read_at (handle_, &request, &out, m, n); },
                 error))
        return false;
    reading.measured = out.measured != 0;
    reading.hours = out.hours;
    reading.sample = out.sample;
    reading.column = out.cell[0];
    reading.row = out.cell[1];
    reading.nearest = out.nearest != 0;
    return true;
}

} // namespace evp::sunstudy::rust
