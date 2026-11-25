#ifndef EVP_ARCHVIZ_DILIGENTHUDPLANTINGPARTS_HPP
#define EVP_ARCHVIZ_DILIGENTHUDPLANTINGPARTS_HPP

// The Planting tab's pieces shared by its two translation units (#278 Stage
// 1C): DiligentHudPlanting.cpp (form, run queue, shared tint) and
// DiligentHudPlantingResults.cpp (results, areas, report, Plants, Compare).

#include "ArchViz/PlantingHudModel.hpp"

#include <imgui.h>

#include <cstdint>
#include <vector>

namespace geomsrv::archviz {

struct HudState;
struct DiligentSceneStats;

namespace plantinghud {

// A control for a later stage: drawn, disabled, and saying what it will do.
// Never true.
bool LaterButton (const char* label, const char* tip, ImVec2 size = ImVec2 (0, 0));

// The class colours the stepped tint draws, lowest first: the swatches, or
// the chosen preset sampled at each band's centre.
std::vector<uint32_t> ShownClassColours (const planting::PlantingHud& hud);

// Trace `heights` one after another, each its own planting study.
void RunHeights (HudState& state, std::vector<float> heights);

void DrawResults (HudState& state, const DiligentSceneStats& scene);
void DrawPlantsTab (HudState& state);
void DrawCompareTab (HudState& state);

} // namespace plantinghud
} // namespace geomsrv::archviz

#endif
