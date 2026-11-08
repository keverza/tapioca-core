#ifndef TAPIOCA_ARCHVIZ_HUD_VIRTUAL_EDITOR_TERRAIN_STATE_HPP
#define TAPIOCA_ARCHVIZ_HUD_VIRTUAL_EDITOR_TERRAIN_STATE_HPP
// The terrain design's session state, shared by its build lane (HudVirtualEditorTerrain.cpp)
// and its field-preview lane (HudVirtualEditorTerrainPreview.cpp). Private to those two.
// OVERLAY-INVARIANTS.md §8: session-owned derived state; nothing outlives the session.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditPathContours.hpp"
#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditTerrainOutline.hpp"
#include <future>
#include <string>
#include <vector>

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
    // The built surface's contour lines, traced once per outcome and interval (DrawTerrainDesign):
    // a selection change republishes the layer and must not trace a large mesh again.
    uint64_t contoursGeneration = UINT64_MAX;
    double contoursInterval = 0.0;
    std::vector<edit::Contour> contours;
    uint64_t staleLogged = UINT64_MAX; // The generation last logged as drawn stale: once, not per move.
    // #175: the design previewed from its field (surface, contours, cut/fill) in milliseconds,
    // asked for every revision -- through a gesture too -- and drawn while the build above has
    // not caught up. One in flight; the newest that landed is kept.
    std::future<lg::TerrainPreviewOutcome> previewWorker;
    bool buildRequested = false; // Build mesh pressed; the worker starts at the next idle poll.
    double previewStep = 0.0;    // The grid spacing the newest preview was asked at.
    uint64_t previewRequested = UINT64_MAX;
    lg::TerrainPreviewOutcome preview;
    lg::UnifiedPreviewAsk previewAsk;
    // #176: the outline geometry of the last preview, reused while its inputs are unchanged.
    std::shared_ptr<lg::OutlineGeometryCache> outlineCache = std::make_shared<lg::OutlineGeometryCache> ();
};

// Lifecycle transitions only, never status polling or per-frame rendering.
void LogTerrain (const Session& s, const std::string& event);
// #175: collect a landed field preview and ask for the current revision's (every poll).
void PollTerrainPreview (Session& s, TerrainDesignState& state);
// #175: whether the layer shows the field preview rather than the last build.
bool PreviewStandsIn (const Session& s);
} // namespace geomsrv::archviz::edithud
#endif
