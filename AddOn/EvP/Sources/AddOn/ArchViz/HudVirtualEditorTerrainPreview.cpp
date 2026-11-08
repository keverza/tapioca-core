#if defined(TAPIOCA_GEOMETRY_RUST)
#include "ArchViz/HudVirtualEditorTerrainState.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Geometry/Mesh.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>

// #175: the terrain design's field-preview lane: one preview per revision on its own worker,
// drawn while the build lane has not caught up. OVERLAY-INVARIANTS.md §§7,8: every decline is
// recorded in the preview's error; nothing outlives the session.
namespace geomsrv::archviz::edithud {
namespace {
// #175: the preview's grid at rest, and the grid nodes it is held to while a gesture moves it.
constexpr double kRestStep = 0.25;
constexpr double kDragNodes = 60000.0;

// #175: one field preview of this revision, on its own worker: the build's inputs (a fresh
// capture, the surface a native solid prepares), compiled and sampled on a grid, no mesh.
void StartTerrainPreview (Session& s, TerrainDesignState& state)
{
    const auto& doc = s.edits.Doc ();
    state.previewRequested = doc.revision;
    state.previewStep = kRestStep;
    if (!state.options.source && lg::TerrainRegionObjects (doc).empty ()) {
        state.preview = {};
        state.preview.revision = doc.revision;
        state.preview.error = "TerrainPreviewNothing: no design path or region target yet";
        return;
    }
    auto capture = std::make_shared<lg::TerrainCapture> ();
    std::string error;
    if (!lg::CaptureLandscapeTerrain (doc, *state.baseline.mesh, { state.options.object, 2, 1 }, *capture, error)) {
        state.preview = {};
        state.preview.revision = doc.revision;
        state.preview.error = error;
        return;
    }
    lg::TerrainDesignJob job;
    job.doc = std::make_shared<const edit::Document> (doc);
    job.capture = std::move (capture);
    job.options = state.options;
    job.surfaceTable = state.baseline.nativeSolid ? 5 : 0;
    auto ask = state.previewAsk;
    ask.interval = s.contourStep > 0.0 ? s.contourStep : 0.0;
    // Coarser while a gesture moves the design -- about kDragNodes grid nodes over the terrain,
    // so a large site keeps up with the pointer -- and at full detail once it rests.
    ask.step = kRestStep;
    if (s.edits.InGesture ()) {
        double lo[2] = { INFINITY, INFINITY }, hi[2] = { -INFINITY, -INFINITY };
        const auto& v = state.baseline.mesh->vertices;
        for (size_t i = 0; i + 2 < v.size (); i += 3)
            for (int a = 0; a < 2; ++a)
                lo[a] = std::min (lo[a], v[i + a]), hi[a] = std::max (hi[a], v[i + a]);
        const double area = (hi[0] - lo[0]) * (hi[1] - lo[1]);
        if (area > 0.0 && std::isfinite (area))
            ask.step = std::max (kRestStep, std::sqrt (area / kDragNodes));
    }
    state.previewStep = ask.step;
    try {
        state.previewWorker = lg::LaunchLandscapeTerrainPreview (std::move (job), ask);
    }
    catch (const std::exception& exception) {
        state.preview = {};
        state.preview.revision = doc.revision;
        state.preview.error = std::string ("TerrainPreviewWorker: ") + exception.what ();
    }
}

} // namespace

// Collect a landed preview, then ask for the current revision's if it is not the one asked.
// Unlike the build it runs through gestures: it is what the user sees move.
void PollTerrainPreview (Session& s, TerrainDesignState& state)
{
    if (state.previewWorker.valid ()) {
        if (state.previewWorker.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
            return;
        auto outcome = state.previewWorker.get ();
        if (outcome.built) {
            // The next call tries these sizes first: one Rust call while the preview keeps its size.
            auto& ask = state.previewAsk;
            ask.points = outcome.preview.points.size () + outcome.preview.points.size () / 4;
            ask.triangles = outcome.preview.triangles.size () + outcome.preview.triangles.size () / 4;
            ask.lines = outcome.preview.lines.size () + outcome.preview.lines.size () / 4 + 16;
            size_t linePoints = 0;
            for (const auto& line : outcome.preview.lines)
                linePoints += line.points.size ();
            ask.linePoints = linePoints + linePoints / 4 + 48;
        }
        if (outcome.built || state.preview.revision != outcome.revision)
            LogTerrain (s, "preview revision=" + std::to_string (outcome.revision) + " built=" +
                               std::to_string (outcome.built) + " ms=" + std::to_string (int (outcome.milliseconds)) +
                               " triangles=" + std::to_string (outcome.preview.triangles.size () / 3) +
                               " lines=" + std::to_string (outcome.preview.lines.size ()) + " error=" + outcome.error);
        state.preview = std::move (outcome);
        s.publishedRevision = UINT64_MAX;
    }
    // A new revision, or the first rest after a gesture's coarse previews.
    const bool coarse = state.previewStep > kRestStep && !s.edits.InGesture ();
    if (!state.invalid && (state.previewRequested != s.edits.Doc ().revision || coarse))
        StartTerrainPreview (s, state);
}

bool PreviewStandsIn (const Session& s)
{
    if (!s.terrainDesign)
        return false;
    const auto& state = *s.terrainDesign;
    // Whatever hides the last build hides the preview too: a stale capture, a design that
    // could not start.
    if (!state.preview.built || state.invalid || !state.error.empty () ||
        (state.baseline.current && !state.baseline.current ()))
        return false;
    // A build of this revision or a later one answers for it, a refusal included.
    if (const auto* current = CurrentTerrainDesign (s); current && current->revision >= state.preview.revision)
        return false;
    bool stale = false;
    const auto* last = LastPublishedTerrainDesign (s, stale);
    return !last || !last->built || state.preview.revision > last->revision;
}
} // namespace geomsrv::archviz::edithud
#endif
