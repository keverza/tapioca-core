// Publishes the Planting tab's scheme (#278) to the viewer: builds the crown
// cylinders and species lines when the scheme, marker or highlight change, and
// hands this frame's key to the frame loop (DiligentHudAnalysisView.cpp), which
// draws and consumes it. Add-on only: the UI playground has no scene and stubs
// PublishSchemeScene.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudPlantingParts.hpp"
#include "ArchViz/PlantingSchemeScene.hpp"

#include <memory>
#include <utility>

namespace geomsrv::archviz::plantinghud {

void PublishSchemeScene (HudState& state)
{
    const planting::SchemeState& s = state.planting.scheme;
    planting::FitState& fit = state.planting.fit;
    state.plantingSchemeSceneKey = 0;
    // The size map is drawn only while the Fit sub-tab is open; it says so each frame.
    const bool map = std::exchange (fit.tabOpen, false) && fit.show && !fit.result.largest.empty ();
    const bool plants = s.show && !s.result.plants.empty ();
    if (!plants && !map)
        return;
    // Revision, marker, highlight and the map each change the geometry; never 0.
    const uint64_t key = (uint64_t (plants ? s.revision : 0) << 8) ^ (uint64_t (s.marker & 0x3) << 6) ^
                         uint64_t ((s.highlight + 1) & 0x3f) ^ (map ? (fit.revision + 1) * 0x9E3779B97F4A7C15ull : 0) ^
                         (uint64_t (1) << 63);
    if (state.plantingSchemeScene == nullptr || state.plantingSchemeSceneBuilt != key) {
        auto geometry = std::make_shared<planting::PlantingSchemeGeometry> ();
        if (plants) // resets the geometry first, so before the map
            planting::BuildPlantingSchemeScene (s.result, planting::SchemeMarker (s.marker), s.highlight, *geometry);
        if (map)
            planting::AppendSizeFitMap (fit.site, fit.result, *geometry);
        state.plantingSchemeScene = std::move (geometry);
        state.plantingSchemeSceneBuilt = key;
    }
    state.plantingSchemeSceneKey = key;
}

} // namespace geomsrv::archviz::plantinghud
