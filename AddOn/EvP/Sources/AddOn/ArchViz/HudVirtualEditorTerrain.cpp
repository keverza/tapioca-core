#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "Geometry/Mesh.hpp"
#include <imgui.h>
#include <algorithm>
#include <set>

// OVERLAY-INVARIANTS.md §§8,12,14: session-owned derived geometry only;
// both hosts consume the same layer, with no camera or renderer changes.
namespace geomsrv::archviz::edithud {
namespace {
// Lifecycle transitions only, never status polling or per-frame rendering.
void LogTerrain (const Session& s, const std::string& event)
{
    ArchVizLog ("TERRAIN " + event + " revision=" + std::to_string (s.edits.Doc ().revision) +
                " path=" + std::to_string (s.terrainPath));
}
} // namespace

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
    const auto path = ResolveTerrainDesignPath (s);
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape || !doc.landscape->propertyLineSourceId || !path) {
        s.message = "Choose a landscape path and a property line before capturing topography.";
        LogTerrain (s, "capture refused: " + s.message);
        return false;
    }
    if (!fromSelection && doc.landscape->sourceTerrainGuid.empty ()) {
        s.message = "Capture a selected Archicad Mesh before recapturing terrain.";
        LogTerrain (s, "capture refused: " + s.message);
        return false;
    }
    s.terrainCapture = std::make_shared<TerrainCaptureRequest> (
        TerrainCaptureRequest { path, doc.revision, fromSelection ? "" : doc.landscape->sourceTerrainGuid });
    s.terrainCaptureQueued = false;
    s.message = "Topography capture requested.";
    LogTerrain (s, fromSelection ? "capture requested source=selection" : "capture requested source=bound");
    return true;
#endif
}

bool TerrainCaptureIsCurrent (const Session& s, const std::shared_ptr<const TerrainCaptureRequest>& request)
{
    return request && s.terrainCapture == request && !s.close && s.edits.Doc ().revision == request->revision &&
           s.edits.Doc ().landscape && edit::FindPath (s.edits.Doc (), request->path) && !s.edits.InGesture () &&
           !GizmoModeLocked (s) && s.drawing.empty () && !s.landscapeCommand;
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
#include <algorithm>
#include <chrono>
#include <exception>

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;

struct TerrainDesignState {
    TerrainDesignBaseline baseline;
    bool invalid = false;
    lg::TerrainDesignOptions options;
    lg::TerrainDesignPublisher publisher;
    std::future<lg::TerrainDesignOutcome> worker;
    uint64_t requestedRevision = UINT64_MAX;
    std::string error;
};

namespace {
bool Idle (const Session& s)
{
    return !s.edits.InGesture () && !GizmoModeLocked (s) && s.drawing.empty ();
}

bool Author (edit::Document& doc, const Mesh& mesh, edit::Id path, lg::TerrainDesignOptions& options,
             std::string& error)
{
    if (!doc.landscape || !doc.landscape->propertyLineSourceId || !edit::FindPath (doc, path)) {
        error = "Choose a landscape path and a property line before starting terrain design.";
        return false;
    }
    edit::Id terrain = 0;
    if (!lg::SetLandscapeTerrain (doc, mesh.guid, terrain, error))
        return false;
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
    options.designTable = 3;
    options.meshTable = 4;
    lg::TerrainCapture capture;
    return lg::CaptureLandscapeTerrain (doc, mesh, { options.object, 2, 1 }, capture, error);
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
    if (!Author (candidate, *state->baseline.mesh, path, state->options, s.message)) {
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
    s.publishedRevision = UINT64_MAX;
    s.message.clear ();
    const auto& started = *s.terrainDesign;
    LogTerrain (s, "start accepted guid=" + started.baseline.mesh->guid +
                       " nativeSolid=" + std::to_string (started.baseline.nativeSolid) +
                       " inputVertices=" + std::to_string (started.baseline.mesh->vertices.size () / 3) +
                       " inputTriangles=" + std::to_string (started.baseline.mesh->triangles.size () / 3));
    return true;
}

void PollTerrainDesign (Session& s)
{
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
                   " error=" + outcome.error);
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
        state.worker = lg::LaunchTerrainDesign (std::move (job));
    }
    catch (const std::exception& exception) {
        state.error = std::string ("TerrainDesignWorker: ") + exception.what ();
        LogTerrain (s, "worker launch refused: " + state.error);
    }
}

const lg::TerrainDesignOutcome* CurrentTerrainDesign (const Session& s)
{
    if (!s.terrainDesign || s.edits.InGesture () || s.terrainDesign->invalid ||
        (s.terrainDesign->baseline.current && !s.terrainDesign->baseline.current ()))
        return nullptr;
    return s.terrainDesign->publisher.Current (s.edits.Doc ());
}

TerrainDesignStatus GetTerrainDesignStatus (const Session& s)
{
    TerrainDesignStatus status;
    if (!s.terrainDesign)
        return status;
    const auto& state = *s.terrainDesign;
    status.active = true;
    status.pending = state.worker.valid ();
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
            status.vertices = current->result.build.points.size () / 3;
            status.triangles = current->result.build.triangles.size () / 3;
            status.cut = current->result.build.site.cut;
            status.fill = current->result.build.site.fill;
        }
    }
    return status;
}

void DrawTerrainDesign (const Session& s, overlaylayers::Layer& layer)
{
    const auto* current = CurrentTerrainDesign (s);
    if (!current)
        return;
    if (current->built) {
        overlaylayers::Mesh mesh;
        mesh.points = current->result.build.points;
        mesh.indices = current->result.build.triangles;
        mesh.rgba = 0x5A7F6280u;
        LogTerrain (s, "layer proposed vertices=" + std::to_string (mesh.points.size () / 3) +
                           " triangles=" + std::to_string (mesh.indices.size () / 3) +
                           " guest=" + std::to_string (overlaylayers::DrawnByGuest (mesh, layer)));
        layer.meshes.push_back (std::move (mesh));
        overlaylayers::Polyline ring;
        // The kernel's ordered ring includes every split crossing and its
        // actual terrain height; the input region is plan-only (z ignored).
        const auto& build = current->result.build;
        for (const auto& row : build.mappings)
            if (row.kind == lg::TerrainMappingKind::RingVertex) {
                const size_t at = size_t (row.values[0]) * 3;
                ring.points.insert (ring.points.end (), build.points.begin () + at, build.points.begin () + at + 3);
            }
        ring.closed = true;
        ring.rgba = 0x37523DFFu;
        layer.polylines.push_back (std::move (ring));
    }
    const auto& conflicts = current->built ? current->result.conflicts : current->conflicts;
    if (!current->built)
        LogTerrain (s,
                    "layer no-proposal conflicts=" + std::to_string (conflicts.size ()) + " error=" + current->error);
    for (const auto& conflict : conflicts) {
        overlaylayers::PointSet place;
        place.points.assign (conflict.position.begin (), conflict.position.end ());
        place.rgba = conflict.hard ? 0xD93A49FFu : 0xF59E0BFFu;
        layer.points.push_back (std::move (place));
    }
}

void TerrainDesignPanel (Session& s)
{
    TerrainSetupPanel (s);
    TerrainControlsPanel (s);
    if (!s.terrainDesign)
        return;
    const auto& state = *s.terrainDesign;
    ImGui::Separator ();
    if (state.worker.valid ())
        ImGui::TextUnformatted ("Building terrain...");
    const auto status = GetTerrainDesignStatus (s);
    if (!status.error.empty ())
        ImGui::TextWrapped ("%s", status.error.c_str ());
    const auto* current = CurrentTerrainDesign (s);
    if (!current)
        return;
    if (current->built) {
        ImGui::Text ("Cut %.3f m3   Fill %.3f m3", current->result.build.site.cut, current->result.build.site.fill);
        ImGui::Text ("Path-conforming preview: %zu vertices, %zu triangles", current->result.build.points.size () / 3,
                     current->result.build.triangles.size () / 3);
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
void PollTerrainDesign (Session&)
{
}
const tapioca::geometry::TerrainDesignOutcome* CurrentTerrainDesign (const Session&)
{
    return nullptr;
}
void DrawTerrainDesign (const Session&, overlaylayers::Layer&)
{
}
void TerrainDesignPanel (Session& s)
{
    TerrainSetupPanel (s);
    TerrainControlsPanel (s);
}
} // namespace geomsrv::archviz::edithud
#endif
