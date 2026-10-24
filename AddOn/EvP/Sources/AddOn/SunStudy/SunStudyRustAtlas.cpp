#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

#include <algorithm>
#include <array>

namespace evp::sunstudy::rust {

using namespace detail;

namespace {

// Layout and per-sample views of a grid; valid while `grid` lives.
struct SamplesView {
    std::vector<TapiocaFaceLayout> layouts;
    TapiocaAtlasSamplesView view {};
};

SamplesView ViewOf (const SampleGrid& grid)
{
    SamplesView out;
    out.layouts.reserve (grid.layouts.size ());
    for (const FaceLayout& layout : grid.layouts) {
        TapiocaFaceLayout pod {};
        std::copy (layout.origin, layout.origin + 3, pod.origin);
        std::copy (layout.uAxis, layout.uAxis + 3, pod.uAxis);
        std::copy (layout.vAxis, layout.vAxis + 3, pod.vAxis);
        pod.uStart = layout.uStart;
        pod.vStart = layout.vStart;
        pod.columns = layout.columns;
        pod.rows = layout.rows;
        pod.gridded = layout.gridded ? 1 : 0;
        out.layouts.push_back (pod);
    }
    out.view.layouts = out.layouts.data ();
    out.view.layoutCount = out.layouts.size ();
    out.view.faces = { grid.faces.data (), grid.faces.size () };
    out.view.cellColumns = { grid.cellColumns.data (), grid.cellColumns.size () };
    out.view.cellRows = { grid.cellRows.data (), grid.cellRows.size () };
    return out;
}

// Destination for an atlas; sized for the grid.
struct AtlasBuffers {
    std::vector<TapiocaAtlasTile> tiles;
    std::vector<uint32_t> texels;
    TapiocaAtlasSummary summary {};
    TapiocaAtlasOutput output {};
};

void Prepare (AtlasBuffers& buffers, const SampleGrid& grid)
{
    buffers.tiles.resize (grid.layouts.size ());
    buffers.texels.resize (grid.faces.size ());
    buffers.output = { buffers.tiles.data (), buffers.tiles.size (), buffers.texels.data (), buffers.texels.size (),
                       &buffers.summary };
}

SunStudyAtlas AtlasOf (const AtlasBuffers& buffers)
{
    SunStudyAtlas atlas;
    atlas.width = buffers.summary.width;
    atlas.height = buffers.summary.height;
    for (size_t i = 0; i < buffers.summary.tileCount; ++i) {
        const TapiocaAtlasTile& t = buffers.tiles[i];
        atlas.tiles.push_back ({ t.x, t.y, t.width, t.height });
    }
    atlas.texels.assign (buffers.texels.begin (), buffers.texels.begin () + buffers.summary.texelCount);
    atlas.placedFaces = buffers.summary.placedFaces;
    atlas.valid = buffers.summary.valid != 0;
    return atlas;
}

// A state's allocations as views; valid while `state` lives.
std::vector<TapiocaPatchAllocationView> AllocationViews (const PatchAtlasState& state)
{
    std::vector<TapiocaPatchAllocationView> views;
    views.reserve (state.allocations.size ());
    for (const auto& [key, a] : state.allocations)
        views.push_back ({ { { key.element.data (), key.element.size () }, key.plane, key.occurrence },
                           { a.x, a.y, a.width, a.height, a.generation } });
    return views;
}

TapiocaPatchAtlasStateView StateView (const PatchAtlasState& state,
                                      const std::vector<TapiocaPatchAllocationView>& views)
{
    TapiocaPatchAtlasStateView out {};
    out.cursor = { { state.width, state.height, state.penX, state.penY, state.shelfHeight, state.nextGeneration } };
    out.allocations = views.data ();
    out.allocationCount = views.size ();
    return out;
}

struct StateBuffers {
    TapiocaPatchAtlasCursor cursor {};
    std::vector<TapiocaPatchAllocation> allocations;
    size_t allocationCount = 0;
    std::vector<char> text;
    size_t textLength = 0;
    TapiocaPatchAtlasStateOutput output {};
};

void Prepare (StateBuffers& buffers, size_t allocations, size_t textBytes)
{
    buffers.allocations.resize (allocations);
    buffers.text.resize (textBytes);
    buffers.output = { &buffers.cursor,          buffers.allocations.data (), buffers.allocations.size (),
                       &buffers.allocationCount, buffers.text.data (),        buffers.text.size (),
                       &buffers.textLength };
}

PatchAtlasState StateOf (const StateBuffers& buffers)
{
    PatchAtlasState state;
    const uint32_t* f = buffers.cursor.fields;
    state.width = f[0];
    state.height = f[1];
    state.penX = f[2];
    state.penY = f[3];
    state.shelfHeight = f[4];
    state.nextGeneration = f[5];
    for (size_t i = 0; i < buffers.allocationCount; ++i) {
        const TapiocaPatchAllocation& pod = buffers.allocations[i];
        PatchAtlasAllocation allocation;
        allocation.key.element.assign (buffers.text.data () + pod.elementOffset, pod.elementLength);
        allocation.key.plane = pod.plane;
        allocation.key.occurrence = pod.occurrence;
        allocation.x = pod.rect[0];
        allocation.y = pod.rect[1];
        allocation.width = pod.rect[2];
        allocation.height = pod.rect[3];
        allocation.generation = pod.rect[4];
        state.allocations[allocation.key] = allocation;
    }
    return state;
}

} // namespace

bool BuildSunStudyAtlas (const SampleGrid& grid, const AtlasOptions& options, SunStudyAtlas& out, std::string& error)
{
    const SamplesView samples = ViewOf (grid);
    const TapiocaTriangleAtlasRequest request { samples.view, options.gutter, options.maxDimension };
    AtlasBuffers buffers;
    Prepare (buffers, grid);
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_triangle_atlas (&request, &buffers.output, m, n); },
                 error))
        return false;
    out = AtlasOf (buffers);
    return true;
}

bool FitPatchAtlas (PatchAtlasState& state, const PatchSampleGrid& grid, uint32_t gutter, uint32_t maxDimension,
                    PatchAtlasUpdate& out, std::string& error)
{
    const auto views = AllocationViews (state);
    std::vector<TapiocaPatchShapeView> shapes;
    size_t textBytes = 0;
    for (const PatchSampleSpan& span : grid.spans) {
        shapes.push_back (
            { { { span.key.element.data (), span.key.element.size () }, span.key.plane, span.key.occurrence },
              span.columns,
              span.rows });
        textBytes += span.key.element.size ();
    }
    const TapiocaPatchAtlasFitRequest request { StateView (state, views), shapes.data (), shapes.size (), gutter,
                                                maxDimension };
    StateBuffers buffers;
    Prepare (buffers, shapes.size (), textBytes);
    TapiocaPatchAtlasUpdate update {};
    const TapiocaPatchAtlasFitOutput output { buffers.output, &update };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_patch_atlas_fit (&request, &output, m, n); }, error))
        return false;
    state = StateOf (buffers);
    out.retained = update.counts[0];
    out.reused = update.counts[1];
    out.moved = update.counts[2];
    out.added = update.counts[3];
    out.removed = update.counts[4];
    out.resized = update.resized != 0;
    return true;
}

bool BuildStableTriangleAtlas (const SampleGrid& grid, const geomsrv::Snapshot& snapshot, PatchAtlasState& allocations,
                               const PatchAtlasState* previous, const AtlasOptions& options, SunStudyAtlas& out,
                               std::string& error)
{
    const SamplesView samples = ViewOf (grid);
    std::vector<TapiocaAtlasMeshView> meshes;
    size_t triangles = 0;
    size_t textBytes = 0;
    for (const geomsrv::Mesh& mesh : snapshot.meshes) {
        meshes.push_back ({ { mesh.guid.data (), mesh.guid.size () }, mesh.TriangleCount () });
        triangles += mesh.TriangleCount ();
        textBytes += mesh.guid.size ();
    }
    const auto current = AllocationViews (allocations);
    static const PatchAtlasState kEmpty;
    const auto earlier = AllocationViews (previous != nullptr ? *previous : kEmpty);
    TapiocaStableAtlasRequest request {};
    request.samples = samples.view;
    request.meshes = meshes.data ();
    request.meshCount = meshes.size ();
    request.allocations = StateView (allocations, current);
    request.previous = StateView (previous != nullptr ? *previous : kEmpty, earlier);
    request.hasPrevious = previous != nullptr ? 1 : 0;
    request.gutter = options.gutter;
    request.maxDimension = options.maxDimension;
    AtlasBuffers atlas;
    Prepare (atlas, grid);
    StateBuffers state;
    Prepare (state, std::max (triangles, grid.layouts.size ()), textBytes);
    const TapiocaStableAtlasOutput output { atlas.output, state.output };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_stable_triangle_atlas (&request, &output, m, n); },
                 error))
        return false;
    allocations = StateOf (state);
    out = AtlasOf (atlas);
    return true;
}

bool PackStepMasks (size_t sampleCount, size_t stepCount, const std::vector<uint8_t>& stepBits,
                    const std::vector<int64_t>& texelOfSample, uint32_t width, uint32_t height, StepMaskAtlas& out,
                    std::string& error)
{
    const TapiocaStepMaskRequest request { { stepBits.data (), stepBits.size () },
                                           texelOfSample.data (),
                                           texelOfSample.size (),
                                           sampleCount,
                                           stepCount,
                                           width,
                                           height };
    const size_t words = std::max<size_t> (1, (stepCount + 31) / 32);
    StepMaskAtlas atlas;
    atlas.masks.resize (static_cast<size_t> (width) * height * words);
    uint32_t written = 0;
    const TapiocaStepMaskOutput output { atlas.masks.data (), atlas.masks.size (), &written };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_step_masks (&request, &output, m, n); }, error))
        return false;
    atlas.width = width;
    atlas.height = height;
    atlas.words = written;
    atlas.steps = static_cast<uint32_t> (stepCount);
    out = std::move (atlas);
    return true;
}

template <typename T>
bool AtlasChangedRegions (uint32_t width, uint32_t height, uint32_t layers, const std::vector<T>& before,
                          const std::vector<T>& after, size_t maxRegionsPerLayer, std::vector<AtlasRegion>& out,
                          std::string& error)
{
    static_assert (sizeof (T) == sizeof (uint32_t), "32-bit texels only");
    const TapiocaTextureDeltaRequest request { reinterpret_cast<const uint32_t*> (before.data ()),
                                               before.size (),
                                               reinterpret_cast<const uint32_t*> (after.data ()),
                                               after.size (),
                                               { width, height, layers },
                                               maxRegionsPerLayer };
    std::vector<TapiocaAtlasRegion> regions (static_cast<size_t> (layers) * std::max<size_t> (1, maxRegionsPerLayer));
    size_t count = 0;
    const TapiocaTextureDeltaOutput output { regions.data (), regions.size (), &count };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_texture_delta (&request, &output, m, n); }, error))
        return false;
    out.clear ();
    for (size_t i = 0; i < count; ++i)
        out.push_back ({ regions[i].x, regions[i].y, regions[i].width, regions[i].height, regions[i].layer });
    return true;
}

template bool AtlasChangedRegions<float> (uint32_t, uint32_t, uint32_t, const std::vector<float>&,
                                          const std::vector<float>&, size_t, std::vector<AtlasRegion>&, std::string&);
template bool AtlasChangedRegions<uint32_t> (uint32_t, uint32_t, uint32_t, const std::vector<uint32_t>&,
                                             const std::vector<uint32_t>&, size_t, std::vector<AtlasRegion>&,
                                             std::string&);

bool StepSchedule (const SunSeries& series, double fanIntervalMinutes, std::vector<uint16_t>& minutes,
                   std::vector<uint32_t>& fan, uint32_t& solarNoon, std::string& error)
{
    std::vector<TapiocaSunStep> steps;
    for (const SunStep& step : series.Steps ())
        steps.push_back (ToPod (step));
    std::vector<uint16_t> outMinutes (steps.size ());
    std::vector<uint32_t> outFan (steps.size ());
    size_t fanCount = 0;
    uint32_t noon = 0;
    const TapiocaStepScheduleRequest request { { steps.data (), steps.size () }, fanIntervalMinutes };
    const TapiocaStepScheduleOutput output { outMinutes.data (), outFan.data (), steps.size (), &fanCount, &noon };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_step_schedule (&request, &output, m, n); }, error))
        return false;
    outFan.resize (fanCount);
    minutes = std::move (outMinutes);
    fan = std::move (outFan);
    solarNoon = noon;
    return true;
}

} // namespace evp::sunstudy::rust
