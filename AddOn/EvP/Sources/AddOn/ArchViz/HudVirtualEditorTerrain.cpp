#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "Geometry/Mesh.hpp"
#include "Edit/EditPathContours.hpp"
#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditRegionTerrain.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include <cmath>
#include <imgui.h>
#include <algorithm>
#include <set>

// OVERLAY-INVARIANTS.md §§8,12,14: session-owned derived geometry only;
// both hosts consume the same layer, with no camera or renderer changes.
namespace geomsrv::archviz::edithud {
// Lifecycle transitions only, never status polling or per-frame rendering.
void LogTerrain (const Session& s, const std::string& event)
{
    ArchVizLog ("TERRAIN " + event + " revision=" + std::to_string (s.edits.Doc ().revision) +
                " path=" + std::to_string (s.terrainPath));
}

bool RequestTerrainCapture (Session& s, bool fromSelection)
{
#if !defined(TAPIOCA_GEOMETRY_RUST)
    (void) fromSelection;
    s.message = "Terrain design requires the Rust geometry build.";
    LogTerrain (s, "capture refused: " + s.message);
    return false;
#else
    if (s.close || s.terrainCapture || s.edits.InGesture () || GizmoModeLocked (s) || !s.drawing.empty () ||
        s.landscapeCommand || GetTerrainDesignStatus (s).pending) {
        s.message = "Finish the current input or terrain build before capturing topography.";
        LogTerrain (s, "capture refused: " + s.message);
        return false;
    }
    // The terrain comes first: neither a path nor a property line is needed to set it.
    const auto path = s.terrainPaths ? ResolveTerrainDesignPath (s) : 0;
    const auto& doc = s.edits.Doc ();
    const auto bound = TerrainGuid (s);
    if (!doc.landscape) {
        s.message = "Start a landscape before setting its terrain.";
        LogTerrain (s, "capture refused: " + s.message);
        return false;
    }
    if (!fromSelection && bound.empty ()) {
        s.message = "Set the terrain from a selected Archicad Mesh before recapturing it.";
        LogTerrain (s, "capture refused: " + s.message);
        return false;
    }
    s.terrainCapture = std::make_shared<TerrainCaptureRequest> (
        TerrainCaptureRequest { path, doc.revision, fromSelection ? "" : bound });
    s.terrainCaptureQueued = false;
    s.message = "Topography capture requested.";
    LogTerrain (s, fromSelection ? "capture requested source=selection" : "capture requested source=bound");
    return true;
#endif
}

bool TerrainCaptureIsCurrent (const Session& s, const std::shared_ptr<const TerrainCaptureRequest>& request)
{
    return request && s.terrainCapture == request && !s.close && s.edits.Doc ().revision == request->revision &&
           s.edits.Doc ().landscape && (!request->path || edit::FindPath (s.edits.Doc (), request->path)) &&
           !s.edits.InGesture () && !GizmoModeLocked (s) && s.drawing.empty () && !s.landscapeCommand;
}

bool SetTerrainControlSelection (Session& s, bool enabled)
{
    if (!s.edits.Doc ().landscape || s.edits.InGesture () || GizmoModeLocked (s) || !s.drawing.empty ()) {
        s.message = "Finish the current input before changing terrain controls.";
        return false;
    }
    const auto refs = s.selection.Items ();
    if (refs.empty ()) {
        s.message = "Select guide vertices or whole open polylines for fixed terrain heights.";
        return false;
    }
    const bool changed = s.edits.Apply ([refs, enabled] (edit::Document& doc, std::string& error) {
        bool changed = false;
        std::set<std::pair<edit::Id, edit::Id>> controls;
        for (const auto& ref : refs) {
            if ((ref.part != edit::Part::Vertex && ref.part != edit::Part::Entity) || !edit::Exists (doc, ref)) {
                error = "TerrainControl: select guide vertices or whole open polylines.";
                return false;
            }
            edit::Id source = 0;
            for (const auto& candidate : doc.landscape->sources)
                if (!candidate.deleted && candidate.entityId == ref.entity) {
                    if (source) {
                        error = "TerrainControl: the selected geometry has multiple source identities.";
                        return false;
                    }
                    source = candidate.id;
                }
            if (!source) {
                error = "TerrainControl: the selection has no live landscape guide source.";
                return false;
            }
            controls.emplace (source, ref.part == edit::Part::Vertex ? ref.item : 0);
        }
        for (const auto& [source, vertex] : controls) {
            if (tapioca::geometry::SetLandscapeTerrainControl (doc, source, vertex, enabled, error))
                changed = true;
            else if (!error.empty ())
                return false;
        }
        return changed;
    });
    s.message = changed ? "Fixed terrain heights updated; guide geometry remains editable." : s.edits.Message ();
    if (changed)
        s.publishedRevision = UINT64_MAX;
    return changed;
}

namespace {
void TerrainControlsPanel (Session& s)
{
    if (!s.edits.Doc ().landscape || !ImGui::CollapsingHeader ("Fixed terrain heights"))
        return;
    ImGui::BeginDisabled (s.edits.InGesture () || GizmoModeLocked (s) || !s.drawing.empty ());
    ImGui::TextWrapped ("Select guide vertices or an open straight polyline. Its authored Z is fixed in the terrain; "
                        "this is not an object movement lock. Controls must be inside the rebuild region.");
    if (ImGui::Button ("Fix selected heights"))
        SetTerrainControlSelection (s, true);
    ImGui::SameLine ();
    if (ImGui::Button ("Release selected heights"))
        SetTerrainControlSelection (s, false);
    const auto controls = s.edits.Doc ().landscape->terrainControls;
    for (size_t i = 0; i < controls.size (); ++i) {
        const auto control = controls[i];
        ImGui::PushID (static_cast<int> (i));
        bool enabled = control.enabled;
        if (ImGui::Checkbox ("##enabled", &enabled)) {
            const bool changed = s.edits.Apply ([control, enabled] (edit::Document& doc, std::string& error) {
                return tapioca::geometry::SetLandscapeTerrainControl (doc, control.source, control.vertex, enabled,
                                                                      error);
            });
            s.message = s.edits.Message ();
            if (changed)
                s.publishedRevision = UINT64_MAX;
        }
        ImGui::SameLine ();
        ImGui::Text ("Source %llu / %s %llu", static_cast<unsigned long long> (control.source),
                     control.vertex ? "vertex" : "line", static_cast<unsigned long long> (control.vertex));
        ImGui::PopID ();
    }
    ImGui::EndDisabled ();
}
} // namespace
} // namespace geomsrv::archviz::edithud

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditPublishedTerrain.hpp"
#include "ArchViz/HudVirtualEditorTerrainState.hpp"
#include <algorithm>
#include <chrono>
#include <exception>

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;

TerrainDebugSource GetTerrainDebugSource (const Session& s)
{
    TerrainDebugSource out;
    const auto* baseline = s.terrainDesign ? &s.terrainDesign->baseline : s.terrainBaseline.get ();
    if (baseline) {
        out.mesh = baseline->mesh;
        out.nativeSolid = baseline->nativeSolid;
        out.fresh = !baseline->current || baseline->current ();
    }
    if (s.terrainDesign) {
        out.active = true;
        out.options = s.terrainDesign->options;
        out.fresh = out.fresh && !s.terrainDesign->invalid;
        const auto* published = s.terrainDesign->publisher.Current (s.edits.Doc ());
        out.outcomeMatchesOptions = s.terrainDesign->requestedRevision == s.edits.Doc ().revision &&
                                    !s.terrainDesign->worker.valid () && published &&
                                    published->generation == s.terrainDesign->publisher.Latest ();
    }
    return out;
}

namespace {
// A cross-section slider's gesture is the one the worker builds through: it is the live edit.
bool Idle (const Session& s)
{
    return (!s.edits.InGesture () || s.terrainSectionDrag) && !GizmoModeLocked (s) && s.drawing.empty ();
}

// #112 P5: the one authoring step of a design start. It may set the terrain
// source and add the stroke operation/object, so it always runs on a candidate
// document and lands as one Session history apply (revision/object counts may
// change here, by design). The worker start (StartTerrainDesignWorker) is its
// read-only counterpart and never authors.
bool Author (edit::Document& doc, const Mesh& mesh, edit::Id path, bool band, lg::TerrainDesignOptions& options,
             std::string& error)
{
    // No property line: the design area is the path's reach on the terrain (the worker clips it).
    if (!doc.landscape || (path && !edit::FindPath (doc, path))) {
        error = "Choose a landscape path before starting terrain design.";
        return false;
    }
    edit::Id terrain = 0;
    if (!lg::SetLandscapeTerrain (doc, mesh.guid, terrain, error))
        return false;
    if (!path) {
        const auto regions = lg::TerrainRegionObjects (doc);
        if (regions.empty ()) {
            error = "Enable Grade/Raise-only/Lower-only on a region or choose a design path.";
            return false;
        }
        const auto& objects = doc.landscape->objects;
        const auto object =
            std::find_if (objects.begin (), objects.end (), [&] (const auto& o) { return o.id == regions[0]; });
        options.source = 0;
        options.object = object->id;
        options.operation = object->footprintOperation;
        lg::TerrainCapture capture;
        return lg::CaptureLandscapeTerrain (doc, mesh, { options.object, 2, 1 }, capture, error);
    }
    for (const auto& source : doc.landscape->sources)
        if (!source.deleted && source.role == lg::SourceRole::PathCenterline && source.pathId == path)
            options.source = source.id;
    if (!options.source) {
        error = "Terrain design requires a live path centreline source.";
        return false;
    }
    for (const auto& object : doc.landscape->objects)
        if (object.kind == lg::ObjectKind::Path && object.pathId == path) {
            options.object = object.id;
            options.operation = object.footprintOperation;
            break;
        }
    if (!options.object) {
        lg::LandscapeOperation stroke;
        stroke.kind = lg::OperationKind::Stroke;
        stroke.inputs = { options.source };
        if (!lg::AddLandscapeOperation (doc, stroke, options.operation, error))
            return false;
        lg::HardscapeObject object;
        object.kind = lg::ObjectKind::Path;
        object.pathId = path;
        object.name = "Path terrain design";
        object.footprintOperation = options.operation;
        if (!lg::AddHardscapeObject (doc, object, options.object, error))
            return false;
    }
    // Tokens are local to this detached request's retained support set, never
    // allocated from Document::nextId. Every revision starts a fresh capture.
    // Graded from every path's paved outline (EditTerrainOutline), unless the band is asked for.
    options.outline = !band;
    options.designTable = 3;
    options.meshTable = 4;
    lg::TerrainCapture capture;
    return lg::CaptureLandscapeTerrain (doc, mesh, { options.object, 2, 1 }, capture, error);
}

// #112 P5: the worker-start counterpart of Author, called from PollTerrainDesign
// once the document revision moves. It consumes the current immutable document
// snapshot and never authors: no history apply, no added operations or objects,
// so a worker start leaves the document revision and object counts unchanged.
void StartTerrainDesignWorker (Session& s, TerrainDesignState& state)
{
    const auto& doc = s.edits.Doc ();
    state.requestedRevision = doc.revision; // Refusals retry only after an edit.
    state.error.clear ();
    auto capture = std::make_shared<lg::TerrainCapture> ();
    lg::TerrainDesignJob job;
    if (!lg::CaptureLandscapeTerrain (doc, *state.baseline.mesh, { state.options.object, 2, 1 }, *capture,
                                      state.error) ||
        !state.publisher.Prepare (doc, capture, state.options, {}, job, state.error)) {
        LogTerrain (s, "worker prepare refused: " + state.error);
        return;
    }
    job.surfaceTable = state.baseline.nativeSolid ? 5 : 0;
    try {
        LogTerrain (s, "worker launch generation=" + std::to_string (job.generation) +
                           " nativeSurfaceRequired=" + std::to_string (job.surfaceTable != 0));
        state.worker = lg::LaunchLandscapeTerrainDesign (std::move (job));
    }
    catch (const std::exception& exception) {
        state.error = std::string ("TerrainDesignWorker: ") + exception.what ();
        LogTerrain (s, "worker launch refused: " + state.error);
    }
}
} // namespace

bool StartTerrainDesign (Session& s, std::shared_ptr<const Mesh> mesh, edit::Id path)
{
    return StartTerrainDesign (s, TerrainDesignBaseline { std::move (mesh), false, {} }, path);
}

bool StartTerrainDesign (Session& s, TerrainDesignBaseline baseline, edit::Id path)
{
    if (!baseline.mesh || !Idle (s) || (s.terrainDesign && s.terrainDesign->worker.valid ())) {
        s.message = "Finish the current input and terrain build before starting a design.";
        LogTerrain (s, "start refused: " + s.message);
        return false;
    }
    auto state = std::make_shared<TerrainDesignState> ();
    if (baseline.current && !baseline.current ()) {
        s.message = "TerrainCaptureStale: capture the terrain again.";
        LogTerrain (s, "start refused: " + s.message);
        return false;
    }
    state->baseline = std::move (baseline);
    // Validate on a candidate before opening history. Apply returns false on a
    // no-op, so an already-authored path is bound without manufacturing an edit.
    auto candidate = s.edits.Doc ();
    if (!Author (candidate, *state->baseline.mesh, path, s.terrainBand, state->options, s.message)) {
        LogTerrain (s, "start refused requestedPath=" + std::to_string (path) + ": " + s.message);
        return false;
    }
    if (candidate.landscape != s.edits.Doc ().landscape &&
        !s.edits.Apply ([candidate] (edit::Document& doc, std::string&) {
            doc = candidate;
            return true;
        })) {
        s.message = s.edits.Message ();
        LogTerrain (s, "start refused: " + s.message);
        return false;
    }
    s.terrainDesign = std::move (state);
    s.terrainPath = path;
    if (path)
        s.terrainPaths = true;
    s.publishedRevision = UINT64_MAX;
    s.message.clear ();
    const auto& started = *s.terrainDesign;
    LogTerrain (s, "start accepted guid=" + started.baseline.mesh->guid +
                       " nativeSolid=" + std::to_string (started.baseline.nativeSolid) +
                       " inputVertices=" + std::to_string (started.baseline.mesh->vertices.size () / 3) +
                       " inputTriangles=" + std::to_string (started.baseline.mesh->triangles.size () / 3));
    return true;
}

bool SetTerrainBaseline (Session& s, TerrainDesignBaseline baseline, edit::Id path)
{
    if (!baseline.mesh) {
        s.message = "TerrainCaptureSource: the terrain has no mesh.";
        return false;
    }
    if (path)
        return StartTerrainDesign (s, std::move (baseline), path);
    // A new terrain replaces the one a design was built on: that design is over.
    s.terrainDesign.reset ();
    s.terrainBaseline = std::make_shared<TerrainDesignBaseline> (std::move (baseline));
    s.publishedRevision = UINT64_MAX;
    s.message = "Terrain set. Enable a region's Grade/Raise-only/Lower-only or draw/select a design path.";
    LogTerrain (s, "baseline held guid=" + s.terrainBaseline->mesh->guid);
    return true;
}

std::string TerrainGuid (const Session& s)
{
    if (s.terrainBaseline && s.terrainBaseline->mesh)
        return s.terrainBaseline->mesh->guid;
    return s.edits.Doc ().landscape ? s.edits.Doc ().landscape->sourceTerrainGuid : std::string {};
}

void PollTerrainDesign (Session& s)
{
    // Paths only: nothing starts, and a build already running is collected when turned back on.
    if (!s.terrainLive)
        return;
    // A terrain set first starts its design when a path appears, once input is idle.
    if (s.terrainBaseline && !s.terrainDesign && Idle (s) && !s.terrainCapture) {
        const auto path = s.terrainPaths ? ResolveTerrainDesignPath (s) : 0;
        if (path || !lg::TerrainRegionObjects (s.edits.Doc ()).empty ()) {
            auto baseline = *s.terrainBaseline;
            if (StartTerrainDesign (s, std::move (baseline), path))
                s.terrainBaseline.reset ();
        }
    }
    if (!s.terrainDesign)
        return;
    auto& state = *s.terrainDesign;
    const auto& doc = s.edits.Doc ();
    if (!state.invalid && state.baseline.current && !state.baseline.current ()) {
        state.invalid = true;
        state.error = "TerrainCaptureStale: native terrain changed; capture it again.";
        LogTerrain (s, "baseline invalidated: " + state.error);
        s.publishedRevision = UINT64_MAX;
    }
    PollTerrainPreview (s, state);
    if (state.worker.valid ()) {
        if (state.worker.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
            return;
        auto outcome = state.worker.get ();
        const auto& capture = outcome.capture;
        LogTerrain (
            s, "worker completed generation=" + std::to_string (outcome.generation) +
                   " builtRevision=" + std::to_string (outcome.revision) + " built=" + std::to_string (outcome.built) +
                   " surfacePrepared=" + std::to_string (capture && capture->native) +
                   " captureVertices=" + std::to_string (capture ? capture->points.size () / 3 : 0) +
                   " captureTriangles=" + std::to_string (capture ? capture->triangles.size () / 3 : 0) +
                   " vertices=" + std::to_string (outcome.result.build.points.size () / 3) +
                   " triangles=" + std::to_string (outcome.result.build.triangles.size () / 3) + " conflicts=" +
                   std::to_string (outcome.built ? outcome.result.conflicts.size () : outcome.conflicts.size ()) +
                   " unmet=" + std::to_string (outcome.unmet.size ()) + " error=" + outcome.error);
        std::string error;
        if (!state.invalid && state.publisher.Publish (doc, std::move (outcome), error)) {
            s.publishedRevision = UINT64_MAX;
            LogTerrain (s, "publication accepted");
        }
        else {
            LogTerrain (s, "publication discarded: " + (state.invalid ? state.error : error));
        }
    }
    if (state.invalid || !Idle (s) || state.requestedRevision == doc.revision)
        return;
    if (!state.options.source) {
        const auto path = s.terrainPaths ? ResolveTerrainDesignPath (s) : 0;
        const bool owner =
            doc.landscape && std::any_of (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                          [&] (const auto& object) { return object.id == state.options.object; });
        if (path || (!owner && !lg::TerrainRegionObjects (doc).empty ())) {
            const auto baseline = state.baseline;
            StartTerrainDesign (s, baseline, path);
            return;
        }
    }
    if (!state.options.source && lg::TerrainRegionObjects (doc).empty ()) {
        s.terrainBaseline = std::make_shared<TerrainDesignBaseline> (state.baseline);
        s.terrainDesign.reset ();
        s.publishedRevision = UINT64_MAX;
        return;
    }
    // #175: the editor previews from the field; the mesh builds when asked, or on every idle
    // revision when the session asks for that.
    if (!s.terrainAutoBuild && !state.buildRequested)
        return;
    state.buildRequested = false;
    // #112 P5: the worker start is Author's read-only counterpart (see above):
    // it consumes this immutable snapshot and never changes revision or counts.
    StartTerrainDesignWorker (s, state);
}

const lg::TerrainDesignOutcome* CurrentTerrainDesign (const Session& s)
{
    if (!s.terrainDesign || (s.edits.InGesture () && !s.terrainSectionDrag) || s.terrainDesign->invalid ||
        (s.terrainDesign->baseline.current && !s.terrainDesign->baseline.current ()))
        return nullptr;
    // #112 P4b: the status/diagnostic answer is the newest completed outcome
    // that still describes the document, whatever it built. A refusal is
    // reported here; the drawable/quantity snapshot keeps drawing through
    // LastPublishedTerrainDesign until a valid build replaces it.
    return s.terrainDesign->publisher.LatestOutcome (s.edits.Doc ());
}

const lg::TerrainDesignOutcome* LastPublishedTerrainDesign (const Session& s, bool& stale)
{
    // The drawable/quantity snapshot is the last GOOD publication (#112): a
    // newer refusal, an in-flight rebuild or an edit never replace it — they
    // only mark it stale while their diagnostic stands in CurrentTerrainDesign.
    stale = false;
    if (!s.terrainDesign || s.terrainDesign->invalid ||
        (s.terrainDesign->baseline.current && !s.terrainDesign->baseline.current ()))
        return nullptr;
    // A rebuild that could not even start (an undone design, a capture refused) will never
    // replace it: the last build would stand for the document indefinitely.
    if (!s.terrainDesign->error.empty ())
        return nullptr;
    const auto* last = s.terrainDesign->publisher.LastPublished (s.edits.Doc ());
    if (!last || !last->built)
        return nullptr;
    const auto* current = CurrentTerrainDesign (s);
    if (current && current->built && current->generation == last->generation)
        return last; // Fresh: the last good build is this document's outcome.
    stale = true;
    return last;
}

std::optional<lg::PathDesignRequest> TerrainRecipeRequest (const Session& s)
{
    if (!s.terrainDesign || s.terrainDesign->options.source == edit::kNoId)
        return std::nullopt;
    return lg::PathRequestFor (s.terrainDesign->options, false); // the landscape lane names no region
}

// #175: whether the layer shows the field preview: one landed, and the build has not caught up
// with it (none yet, a refusal standing, or the last good build older than the preview).

TerrainDesignStatus GetTerrainDesignStatus (const Session& s)
{
    TerrainDesignStatus status;
    if (!s.terrainDesign)
        return status;
    const auto& state = *s.terrainDesign;
    status.active = true;
    status.pending = state.worker.valid ();
    status.previewPending = state.previewWorker.valid ();
    status.previewMs = state.preview.milliseconds;
    status.error = state.error;
    if (state.baseline.current && !state.baseline.current ())
        status.error = "TerrainCaptureStale: native terrain changed; capture it again.";
    if (const auto* current = CurrentTerrainDesign (s)) {
        status.current = true;
        status.built = current->built;
        status.revision = current->revision;
        status.error = current->error;
        status.conflicts = current->built ? current->result.conflicts.size () : current->conflicts.size ();
        if (current->built) {
            const auto* published = current->published.get ();
            const auto& points = published ? published->points : current->result.build.points;
            const auto& triangles = published ? published->triangles : current->result.build.triangles;
            const auto& site = published ? published->displaySite : current->result.build.site;
            status.vertices = points.size () / 3;
            status.triangles = triangles.size () / 3;
            status.cut = site.cut;
            status.fill = site.fill;
        }
        else
            // A refusal keeps the last valid drawable/quantity snapshot in
            // place (#112): report it as stale while the diagnostic stands.
            LastPublishedTerrainDesign (s, status.stale);
    }
    else
        LastPublishedTerrainDesign (s, status.stale);
    status.previewed = PreviewStandsIn (s);
    status.buildRequested = state.buildRequested;
    if (state.preview.built) {
        status.previewCut = state.preview.preview.cut;
        status.previewFill = state.preview.preview.fill;
        status.previewUnmet = state.preview.unmet;
        if (status.previewed) {
            status.cut = status.previewCut;
            status.fill = status.previewFill;
        }
    }
    return status;
}

bool RequestTerrainBuild (Session& s)
{
    if (!s.terrainDesign || s.terrainDesign->invalid) {
        s.message = "Set the terrain and a design path or region before building its mesh.";
        return false;
    }
    auto& state = *s.terrainDesign;
    if (state.worker.valid () || state.buildRequested)
        return true; // Already on its way.
    if (const auto* current = CurrentTerrainDesign (s); current && current->built) {
        s.message = "The mesh is built for the terrain as it is.";
        return false;
    }
    state.buildRequested = true;
    state.requestedRevision = UINT64_MAX; // The next idle poll starts it, also on this revision.
    LogTerrain (s, "build requested");
    return true;
}

void DrawTerrainDesign (const Session& s, overlaylayers::Layer& layer)
{
    if (!s.terrainLive)
        return;
    // Through a path move and until its rebuild publishes, the last built surface stays as the
    // reference for the move (#71); a newer refusal never replaces it either (#112 P4b).
    // Drawn as it was (the user, 2026-10-30: a faded surface is distracting); the panel says
    // it is stale. The drawable and the quantities are the published snapshot's.
    bool stale = false;
    const auto* last = LastPublishedTerrainDesign (s, stale);
    const auto* current = CurrentTerrainDesign (s);
    // #175: while the build has not caught up with the document (an edit in progress, a build
    // running, none yet), the field preview of the newest revision stands in for it: its grid
    // surface and contours, the same colours.
    const auto* preview = s.terrainDesign && s.terrainDesign->preview.built ? &s.terrainDesign->preview : nullptr;
    if (PreviewStandsIn (s)) {
        overlaylayers::Mesh mesh;
        mesh.points = preview->preview.points;
        mesh.indices = preview->preview.triangles;
        mesh.rgba = 0x6E9A72FFu;
        mesh.styled = true;
        mesh.style.shading = overlaylayers::Shading::Lit;
        mesh.style.opacity = 0.9f;
        mesh.style.behind = overlaylayers::Behind::Fade;
        mesh.style.occludedOpacity = 0.75f;
        layer.meshes.push_back (std::move (mesh));
        if (s.contours) {
            const double interval = preview->preview.interval;
            for (const auto& c : preview->preview.lines) {
                overlaylayers::Polyline line;
                line.points = c.points;
                line.closed = c.closed;
                const bool index = interval > 0.0 && std::fmod (std::abs (std::round (c.level / interval)), 5.0) == 0.0;
                line.rgba = index ? 0x2F4A35E6u : 0x37523D99u;
                line.widthPixels = index ? 1.75f : 1.0f;
                line.behind = overlaylayers::Behind::Show;
                layer.polylines.push_back (std::move (line));
            }
        }
    }
    else if (last && last->built) {
        // Prefer the neutral published snapshot when the lane produced one; the
        // legacy result fields mirror it during migration.
        const auto* published = last->published.get ();
        const auto& points = published ? published->points : last->result.build.points;
        const auto& triangles = published ? published->triangles : last->result.build.triangles;
        const auto& mappings = published ? published->mappings : last->result.build.mappings;
        overlaylayers::Mesh mesh;
        mesh.points = points;
        mesh.indices = triangles;
        // ⚠️ THE PROPOSAL LIES ON THE TERRAIN IT WAS CAPTURED FROM (the user, 2026-10-24: barely
        // visible, and only from below). Unstyled, it took the layer's Fade, so wherever the host
        // surface is coincident or above it -- everywhere outside the cut -- it drew at the faded
        // pass's 0.3 of an alpha that was itself one half. Lit, mostly opaque and held at most of
        // its strength through the surface it replaces, it reads as the ground it proposes.
        mesh.rgba = 0x6E9A72FFu;
        mesh.styled = true;
        mesh.style.shading = overlaylayers::Shading::Lit;
        mesh.style.opacity = 0.9f;
        mesh.style.behind = overlaylayers::Behind::Fade;
        mesh.style.occludedOpacity = 0.75f;
        if (!stale)
            LogTerrain (s, "layer proposed vertices=" + std::to_string (mesh.points.size () / 3) +
                               " triangles=" + std::to_string (mesh.indices.size () / 3) +
                               " guest=" + std::to_string (overlaylayers::DrawnByGuest (mesh, layer)));
        else if (s.terrainDesign->staleLogged != last->generation) {
            s.terrainDesign->staleLogged = last->generation;
            LogTerrain (s, "layer stale generation=" + std::to_string (last->generation) +
                               " builtRevision=" + std::to_string (last->revision));
        }
        layer.meshes.push_back (std::move (mesh));
        overlaylayers::Polyline ring;
        // The kernel's ordered ring includes every split crossing and its
        // actual terrain height; the input region is plan-only (z ignored).
        for (const auto& row : mappings)
            if (row.kind == lg::TerrainMappingKind::RingVertex) {
                const size_t at = size_t (row.values[0]) * 3;
                ring.points.insert (ring.points.end (), points.begin () + at, points.begin () + at + 3);
            }
        ring.closed = true;
        ring.rgba = 0x37523DFFu;
        layer.polylines.push_back (std::move (ring));
        // Its contours, as the paths' are drawn: the interval asked, else about ten over the heights
        // the surface spans; every fifth level an index line. On the surface's own triangles, so a
        // line lies on the face the user sees, and drawn whole through the host surface it lies on.
        if (s.contours) {
            auto& state = *s.terrainDesign;
            double lo = INFINITY, hi = -INFINITY;
            for (size_t i = 2; i < points.size (); i += 3)
                lo = std::min (lo, points[i]), hi = std::max (hi, points[i]);
            const double interval = s.contourStep > 0.0 ? s.contourStep
                                    : hi > lo           ? std::max (edit::NiceStep (hi - lo, 10), 0.1)
                                                        : 0.0;
            // Publication replaces an optional in place: its address and triangle
            // count can both stay unchanged while every height/contour moves.
            if (state.contoursGeneration != last->generation || state.contoursInterval != interval) {
                edit::PavedArea surface;
                for (size_t i = 0; i + 2 < points.size (); i += 3)
                    surface.vertices.push_back ({ points[i], points[i + 1], points[i + 2] });
                surface.triangles = triangles;
                state.contours =
                    interval > 0.0 ? edit::SurfaceContours (surface, interval) : std::vector<edit::Contour> {};
                state.contoursGeneration = last->generation;
                state.contoursInterval = interval;
            }
            for (const auto& c : state.contours) {
                overlaylayers::Polyline line;
                for (const auto& p : c.points)
                    line.points.insert (line.points.end (), { p.x, p.y, p.z });
                const bool index = std::fmod (std::abs (std::round (c.z / interval)), 5.0) == 0.0;
                line.rgba = index ? 0x2F4A35E6u : 0x37523D99u;
                line.widthPixels = index ? 1.75f : 1.0f;
                line.behind = overlaylayers::Behind::Show;
                layer.polylines.push_back (std::move (line));
            }
        }
    }
    // Located conflicts and diagnostic places belong to the newest outcome that
    // describes the document: a refusal's places are current geometry and draw
    // even while the last good surface holds above them. An in-flight rebuild
    // (no current outcome yet) draws no new places: the stale ones would belong
    // to paths that have since moved.
    if (!current)
        return;
    const auto& conflicts = current->built ? current->result.conflicts : current->conflicts;
    if (!last || !last->built)
        LogTerrain (s,
                    "layer no-proposal conflicts=" + std::to_string (conflicts.size ()) + " error=" + current->error);
    for (const auto& conflict : conflicts) {
        overlaylayers::PointSet place;
        place.points.assign (conflict.position.begin (), conflict.position.end ());
        place.rgba = conflict.hard ? 0xD93A49FFu : 0xF59E0BFFu;
        layer.points.push_back (std::move (place));
    }
}

void SetTerrainBand (Session& s, bool band)
{
    s.terrainBand = band;
    if (!s.terrainDesign)
        return;
    // The next idle poll rebuilds the design the other way.
    s.terrainDesign->options.outline = !band;
    s.terrainDesign->requestedRevision = UINT64_MAX;
    s.publishedRevision = UINT64_MAX;
}

void SetTerrainLive (Session& s, bool live)
{
    s.terrainLive = live;
    s.publishedRevision = UINT64_MAX;
}

bool SetTerrainPaths (Session& s, bool enabled)
{
    if (!s.terrainDesign || s.terrainDesign->worker.valid () || !Idle (s))
        return false;
    if (!enabled && lg::TerrainRegionObjects (s.edits.Doc ()).empty ()) {
        s.message = "Enable a region terrain target before switching to region-only grading.";
        return false;
    }
    const auto baseline = s.terrainDesign->baseline;
    const auto path = enabled ? ResolveTerrainDesignPath (s) : 0;
    if (!StartTerrainDesign (s, baseline, path))
        return false;
    s.terrainPaths = enabled;
    return true;
}

void TerrainDesignPanel (Session& s)
{
    bool live = s.terrainLive;
    if (ImGui::Checkbox ("Rebuild terrain (3D)", &live))
        SetTerrainLive (s, live);
    if (ImGui::IsItemHovered ())
        geomsrv::archviz::hudshell::HoverTip ("Off: only the paths are drawn and edited, and no terrain is rebuilt. "
                                              "On: the terrain is rebuilt from the paths as they are.");
    if (!s.terrainLive) {
        ImGui::TextWrapped ("Paths only: the terrain is not rebuilt or shown. Turn it on to rebuild it.");
        return;
    }
    TerrainSetupPanel (s);
    TerrainControlsPanel (s);
    if (!s.terrainDesign)
        return;
    const auto& state = *s.terrainDesign;
    ImGui::Separator ();
    bool includePaths = s.terrainPaths;
    ImGui::BeginDisabled (state.worker.valid ());
    if (ImGui::Checkbox ("Include paths in terrain grading", &includePaths)) {
        SetTerrainPaths (s, includePaths);
        ImGui::EndDisabled ();
        return; // A successful switch replaced the derived state.
    }
    ImGui::EndDisabled ();
    bool band = s.terrainBand;
    ImGui::BeginDisabled (state.worker.valid () || !state.options.source);
    if (ImGui::Checkbox ("Grade along the chosen path only", &band))
        SetTerrainBand (s, band);
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        geomsrv::archviz::hudshell::HoverTip (
            "Off: the terrain follows every path's paved outline, junctions and kerbs included. "
            "On: only the chosen path's band, as sampled stations.");
    TerrainBuildSection (s);
    const auto status = GetTerrainDesignStatus (s);
    const auto* current = CurrentTerrainDesign (s);
    // The worker always tries to build; what it could not satisfy is listed, not thrown at the
    // user as a refusal code (the raw message stays in the log and the status command).
    if (current && !current->unmet.empty ()) {
        ImGui::TextUnformatted (current->built ? "Built, but not everything asked for is met:"
                                               : "Not built. Not satisfied:");
        for (const auto& line : current->unmet)
            ImGui::BulletText ("%s", line.c_str ());
    }
    else if (!status.error.empty ())
        ImGui::TextWrapped ("%s", lg::TerrainConditionText (status.error).c_str ());
    if (status.stale && !status.previewed)
        ImGui::TextWrapped ("Updating: the terrain shown is the last build and does not follow the paths yet. "
                            "It is replaced when the rebuild finishes.");
    if (!current)
        return;
    if (current->built) {
        // #112 P4b: quantities come from the published snapshot when the lane
        // produced one; displayed totals keep the 0.25 m measurement convention.
        const auto* published = current->published.get ();
        const auto& site = published ? published->displaySite : current->result.build.site;
        const auto& points = published ? published->points : current->result.build.points;
        const auto& triangles = published ? published->triangles : current->result.build.triangles;
        ImGui::Text ("Cut %.3f m3   Fill %.3f m3", site.cut, site.fill);
        ImGui::Text ("Topography preview: %zu vertices, %zu triangles", points.size () / 3, triangles.size () / 3);
        if (current->regions) {
            const auto& area = current->regions->evaluation.terrain;
            for (const auto& report : area.reports)
                ImGui::Text ("Region operation %llu: %u moved, %u reach clips, %u extension clips",
                             static_cast<unsigned long long> (report.operation), report.report.moved,
                             report.report.limitClipped, report.report.extensionClipped);
            if (area.volumes)
                for (const auto& volume : area.volumes->operations)
                    ImGui::Text ("  Area stage: cut %.3f m3, fill %.3f m3", volume.cut, volume.fill);
        }
    }
    const auto& conflicts = current->built ? current->result.conflicts : current->conflicts;
    if (s.edits.Doc ().landscape &&
        std::any_of (conflicts.begin (), conflicts.end (),
                     [] (const auto& conflict) { return conflict.hard && !conflict.control; }) &&
        std::any_of (s.edits.Doc ().landscape->terrainControls.begin (),
                     s.edits.Doc ().landscape->terrainControls.end (),
                     [] (const auto& control) { return control.enabled; }))
        ImGui::TextWrapped ("Fixed heights could not be assessed: resolve the band conflicts first.");
    for (const auto& conflict : conflicts)
        if (conflict.control)
            ImGui::TextWrapped ("%s at fixed source %llu (%.3f m): %s", lg::HybridConflictName (conflict.code),
                                static_cast<unsigned long long> (conflict.control->source), conflict.magnitude,
                                lg::HybridConflictSuggestion (conflict.code));
        else
            ImGui::TextWrapped ("%s at %.2f m, %s: %s", lg::HybridConflictName (conflict.code), conflict.station,
                                conflict.side == lg::TerrainHand::Left ? "left" : "right",
                                lg::HybridConflictSuggestion (conflict.code));
}
} // namespace geomsrv::archviz::edithud

#else
namespace geomsrv::archviz::edithud {
TerrainDebugSource GetTerrainDebugSource (const Session& s)
{
    TerrainDebugSource out;
    if (s.terrainBaseline) {
        out.mesh = s.terrainBaseline->mesh;
        out.nativeSolid = s.terrainBaseline->nativeSolid;
        out.fresh = !s.terrainBaseline->current || s.terrainBaseline->current ();
    }
    return out;
}
bool StartTerrainDesign (Session& s, std::shared_ptr<const Mesh>, edit::Id)
{
    s.message = "Terrain design requires the Rust geometry build.";
    LogTerrain (s, "start refused: " + s.message);
    return false;
}
bool StartTerrainDesign (Session& s, TerrainDesignBaseline, edit::Id path)
{
    return StartTerrainDesign (s, std::shared_ptr<const Mesh> {}, path);
}
TerrainDesignStatus GetTerrainDesignStatus (const Session&)
{
    return {};
}
std::optional<lg::PathDesignRequest> TerrainRecipeRequest (const Session&)
{
    return std::nullopt;
}
void PollTerrainDesign (Session&)
{
}
bool SetTerrainBaseline (Session& s, TerrainDesignBaseline, edit::Id)
{
    s.message = "Terrain design requires the Rust geometry build.";
    return false;
}
std::string TerrainGuid (const Session& s)
{
    return s.edits.Doc ().landscape ? s.edits.Doc ().landscape->sourceTerrainGuid : std::string {};
}
const tapioca::geometry::TerrainDesignOutcome* CurrentTerrainDesign (const Session&)
{
    return nullptr;
}
const tapioca::geometry::TerrainDesignOutcome* LastPublishedTerrainDesign (const Session&, bool& stale)
{
    stale = false;
    return nullptr;
}
void DrawTerrainDesign (const Session&, overlaylayers::Layer&)
{
}
void SetTerrainBand (Session& s, bool band)
{
    s.terrainBand = band;
}
void SetTerrainLive (Session& s, bool live)
{
    s.terrainLive = live;
}
bool SetTerrainPaths (Session& s, bool enabled)
{
    (void) enabled;
    s.message = "Terrain design requires the Rust geometry build.";
    return false;
}
void TerrainDesignPanel (Session& s)
{
    TerrainSetupPanel (s);
    TerrainControlsPanel (s);
}
} // namespace geomsrv::archviz::edithud
#endif
