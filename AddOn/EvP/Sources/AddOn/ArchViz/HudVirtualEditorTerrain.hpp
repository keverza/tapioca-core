#ifndef TAPIOCA_HUD_VIRTUAL_EDITOR_TERRAIN_HPP
#define TAPIOCA_HUD_VIRTUAL_EDITOR_TERRAIN_HPP

#include "Edit/EditDocument.hpp"
#include <memory>
#include <functional>

namespace geomsrv {
struct Mesh;
}
namespace tapioca::geometry {
struct TerrainDesignOutcome;
}
namespace geomsrv::archviz::overlaylayers {
struct Layer;
}
namespace geomsrv::archviz::edithud {
struct Session;
struct TerrainDesignState;
struct TerrainCaptureRequest {
    edit::Id path = edit::kNoId; // kNoId: set the terrain alone; a design starts once a path exists.
    uint64_t revision = 0;
    std::string terrainGuid; // Empty: read exactly one native selected Mesh; otherwise recapture this source.
};
struct TerrainSectionDraft {
    edit::Id path = edit::kNoId, section = edit::kNoId;
    uint64_t revision = 0;
    edit::Section value; // Only the existing section's dimensional fields are editable here.
};
struct TerrainDesignBaseline {
    std::shared_ptr<const Mesh> mesh;
    bool nativeSolid = false; // Compact the upward surface on the worker.
    // Main-thread-only freshness read, never handed to a worker. False requires
    // an explicit recapture; the existing cache and any completion are hidden.
    std::function<bool ()> current;
};
struct TerrainDesignStatus {
    bool active = false, pending = false, current = false, built = false;
    uint64_t revision = 0;
    size_t vertices = 0, triangles = 0, conflicts = 0;
    double cut = 0, fill = 0;
    std::string error;
};

// Detached plan-view terrain, captured by the host on the main thread. Starts
// one path design in the existing document/history; selection stays the same.
// The immutable baseline is retained across edits and surface switches. Closed
// native solids require an attributed surface adapter before this entrypoint.
bool StartTerrainDesign (Session& session, std::shared_ptr<const Mesh> mesh, edit::Id path);
bool StartTerrainDesign (Session& session, TerrainDesignBaseline baseline, edit::Id path);
// The terrain first (the user, 2026-10-24: "allow set terrain first"): with a design path the
// design starts at once; without one the baseline is held and PollTerrainDesign starts the
// design as soon as one can be resolved. No document edit until then.
bool SetTerrainBaseline (Session& session, TerrainDesignBaseline baseline, edit::Id path);
// The GUID of the terrain set, whether a design owns it yet or not; empty when none is.
std::string TerrainGuid (const Session& session);
TerrainDesignStatus GetTerrainDesignStatus (const Session& session);
// Main-thread, nonblocking poll. One worker at a time; changed revisions coalesce
// until input ends. Results never enter document state or its undo history.
void PollTerrainDesign (Session& session);
const tapioca::geometry::TerrainDesignOutcome* CurrentTerrainDesign (const Session& session);
void DrawTerrainDesign (const Session& session, overlaylayers::Layer& layer);
void TerrainDesignPanel (Session& session);
// Grade along the chosen path's band only (true), or from every path's paved outline (false, the
// default); a running design rebuilds the other way on the next idle poll.
void SetTerrainBand (Session& session, bool band);
bool SetTerrainPaths (Session& session, bool enabled);
// Setup uses only live authored centreline paths, never native/model GUIDs as
// virtual path IDs. Initial choice follows one selected path or the sole eligible
// path; an unavailable explicit choice is not silently retargeted.
std::vector<edit::Id> TerrainDesignPaths (const Session& session);
edit::Id ResolveTerrainDesignPath (Session& session);
bool ChooseTerrainDesignPath (Session& session, edit::Id path);
bool EditTerrainDesignPath (Session& session);
bool DrawTerrainDesignPath (Session& session);
bool DrawTerrainPropertyLine (Session& session);
void TerrainSetupPanel (Session& session);
// One explicit Apply through the existing Session history. Drafts are revision-
// bound; stale/missing ranges refuse rather than retarget or overwrite newer edits.
bool BeginTerrainSectionEdit (Session& session, edit::Id section);
bool ApplyTerrainSectionEdit (Session& session);
// A cross-section slider held, moved and released: the draft applies live within one editor
// gesture, the terrain preview rebuilding as it goes, and the release records one undo step.
bool BeginTerrainSectionDrag (Session& session);
bool MoveTerrainSectionDrag (Session& session);
void EndTerrainSectionDrag (Session& session);
// Selected guide vertices become point controls; selected whole open polylines
// become line controls. One existing Session history entry for the whole set.
bool SetTerrainControlSelection (Session& session, bool enabled);
// Pure UI request/ownership gate, no native read or document/history mutation.
bool RequestTerrainCapture (Session& session, bool fromSelection);
bool TerrainCaptureIsCurrent (const Session& session, const std::shared_ptr<const TerrainCaptureRequest>& request);
} // namespace geomsrv::archviz::edithud
#endif
