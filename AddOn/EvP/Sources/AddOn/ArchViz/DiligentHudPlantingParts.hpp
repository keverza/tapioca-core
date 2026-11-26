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

// The Scheme sub-tab (#278): a mix of catalogue plants placed on a kept layer.
void DrawSchemeTab (HudState& state);
// Adds a catalogue plant to the scheme (the Plants tab's "Add to scheme").
void AddToScheme (planting::PlantingHud& hud, const planting::Plant& plant);
// The region the Scheme and Fit share: the layer it reads (or -1), read and
// kept current with its room and no-go distances; the region row with its
// space controls; and the site limited to the chosen zone.
int RefreshSchemeRegion (planting::PlantingHud& hud);
void DrawSchemeRegion (planting::PlantingHud& hud, int layer);
planting::SchemeSite SchemeRegionSite (const planting::PlantingHud& hud);
// The Fit sub-tab (DiligentHudPlantingFit.cpp): the largest size class that fits.
void DrawFitTab (HudState& state);
// Publishes the scheme's viewer geometry for this frame (add-on only; the
// playground has no scene and stubs it). Call every frame the Planting tab is open.
void PublishSchemeScene (HudState& state);

// Zones from the selection (#278): the Planting areas row, and the area table's zone
// rows for the shown layer (read once per study, edges and zone list).
void DrawZonesRow (HudState& state);
const planting::ZonesState& RefreshZoneRows (HudState& state, const std::string& studyId, float layerHeight);
// Each zone's class areas on kept layer `layer` (parallel to the zones), read
// once per study, edges and zone list; null when the layer cannot be read.
const std::vector<planting::ZoneClassAreas>* ZoneRowsOfLayer (HudState& state, int layer);
// A plant's fit in the Plants tab's chosen zone (CatalogueState::zone), or on
// its whole layer when none is chosen.
planting::PlantFit FitForPlantsTab (HudState& state, const planting::Plant& plant);

} // namespace plantinghud
} // namespace geomsrv::archviz

#endif
