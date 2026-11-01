#ifndef EVP_ARCHVIZ_VIEWERSTATUSSOURCES_HPP
#define EVP_ARCHVIZ_VIEWERSTATUSSOURCES_HPP

// ArchViz/ViewerStatusSources -- where the status service's inputs come from
// (issue #96). ViewerStatus decides what the bar says; this file only reads the
// subsystems that already know: the viewport's stats, the extraction worker, the
// model watch, the HUD's sun study runner and the sun study follower.
//
// ⚠️ ITS OWN FILE SO ViewerStatus STAYS PURE. Everything here reaches a singleton
// or the HUD, which the offline tests cannot link.

#include "ArchViz/ViewerStatus.hpp"

namespace geomsrv::archviz {

struct DiligentViewportStats;
struct HudState;

namespace viewerstatus {

// Main thread (the palette's idle). Reads only what each subsystem already
// publishes thread-safely; no ACAPI.
Inputs Gather (const DiligentViewportStats& viewport);

// Render thread, once per frame: the HUD's projection, and the hint for the
// HUD's own click-in-the-model steps. Writes only when something changed.
void PublishFromHud (const HudState& hud);

} // namespace viewerstatus
} // namespace geomsrv::archviz

#endif
