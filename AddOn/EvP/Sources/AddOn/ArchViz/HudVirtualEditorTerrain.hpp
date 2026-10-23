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
    edit::Id path = edit::kNoId;
    uint64_t revision = 0;
    std::string terrainGuid; // Empty: read exactly one native selected Mesh; otherwise recapture this source.
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
TerrainDesignStatus GetTerrainDesignStatus (const Session& session);
// Main-thread, nonblocking poll. One worker at a time; changed revisions coalesce
// until input ends. Results never enter document state or its undo history.
void PollTerrainDesign (Session& session);
const tapioca::geometry::TerrainDesignOutcome* CurrentTerrainDesign (const Session& session);
void DrawTerrainDesign (const Session& session, overlaylayers::Layer& layer);
void TerrainDesignPanel (Session& session);
// Selected guide vertices become point controls; selected whole open polylines
// become line controls. One existing Session history entry for the whole set.
bool SetTerrainControlSelection (Session& session, bool enabled);
// Pure UI request/ownership gate, no native read or document/history mutation.
bool RequestTerrainCapture (Session& session, bool fromSelection);
bool TerrainCaptureIsCurrent (const Session& session, const std::shared_ptr<const TerrainCaptureRequest>& request);
} // namespace geomsrv::archviz::edithud
#endif
