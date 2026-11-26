#ifndef EVP_ARCHVIZ_DILIGENTHUDPLANTINGPARTS_HPP
#define EVP_ARCHVIZ_DILIGENTHUDPLANTINGPARTS_HPP

// The Planting tab's pieces shared by its two translation units (#278 Stage
// 1C): DiligentHudPlanting.cpp (form, run queue, shared tint) and
// DiligentHudPlantingResults.cpp (results, areas, report, Compare),
// DiligentHudPlantingCatalogue.cpp (Plants) and DiligentHudPlantingClimate.cpp.

#include "ArchViz/PlantingHudModel.hpp"

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <string>
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

// Packed ImGui colour of 0xRRGGBBAA; the unavailable hatch swatch, `size`
// square, at the cursor; a class's hour range ("3 to 6 h").
ImU32 AbgrOf (uint32_t rgba);
void DrawHatchSwatch (float size);
std::string ClassRangeOf (const planting::PlantingHud& hud, int cls);

// Trace `heights` one after another, each its own planting study.
void RunHeights (HudState& state, std::vector<float> heights);

// Show layer `index` again: a kept planting study goes back on screen without
// tracing; one no longer kept is traced again.
void ShowLayer (HudState& state, size_t index);
// Erase every kept layer and forget them here.
void ClearLayers (HudState& state);

// Read the area table again when `studyId`, its `version` or the class edges
// changed since it was last read (the runner copies the study's samples).
void RefreshAreas (HudState& state, const std::string& studyId, uint64_t version);

void DrawResults (HudState& state, const DiligentSceneStats& scene);
// The Plants sub-tab and its catalogue (DiligentHudPlantingCatalogue.cpp).
void DrawPlantsTab (HudState& state);
// Seed and read the catalogue folder once; Reload reads it again.
void EnsureCatalogue (HudState& state);
void ReloadCatalogue (planting::CatalogueState& cat);
// Read each kept layer's whole-layer class areas at the current edges, once.
void RefreshLayerAreas (HudState& state);
// Open a folder in Explorer on a short-lived thread of its own.
void OpenFolderInExplorer (const std::filesystem::path& folder);
// The Light sub-tab's Climate section (DiligentHudPlantingClimate.cpp).
void DrawClimateSection (HudState& state, const DiligentSceneStats& scene);
void DrawCompareTab (HudState& state);

} // namespace plantinghud
} // namespace geomsrv::archviz

#endif
