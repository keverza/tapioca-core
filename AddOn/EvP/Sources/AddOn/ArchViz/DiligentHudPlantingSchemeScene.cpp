// Publishes the Planting tab's scheme (#278) to the viewer: builds the crown
// cylinders and species lines when the scheme, marker or highlight change, and
// hands this frame's key to the frame loop (DiligentHudAnalysisView.cpp), which
// draws and consumes it. Add-on only: the UI playground has no scene and stubs
// PublishSchemeScene.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudPlantingParts.hpp"
#include "ArchViz/PlantingSchemeScene.hpp"

#include <memory>

namespace geomsrv::archviz::plantinghud {

void PublishSchemeScene (HudState& state)
{
    const planting::SchemeState& s = state.planting.scheme;
    state.plantingSchemeSceneKey = 0;
    if (!s.show || s.result.plants.empty ())
        return;
    // Revision, marker and highlight each change the geometry; never 0.
    const uint64_t key = (s.revision << 8) ^ (uint64_t (s.marker & 0x3) << 6) ^ uint64_t ((s.highlight + 1) & 0x3f) ^
                         (uint64_t (1) << 63);
    if (state.plantingSchemeScene == nullptr || state.plantingSchemeSceneBuilt != key) {
        auto geometry = std::make_shared<planting::PlantingSchemeGeometry> ();
        planting::BuildPlantingSchemeScene (s.result, planting::SchemeMarker (s.marker), s.highlight, *geometry);
        state.plantingSchemeScene = std::move (geometry);
        state.plantingSchemeSceneBuilt = key;
    }
    state.plantingSchemeSceneKey = key;
}

} // namespace geomsrv::archviz::plantinghud
