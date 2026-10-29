#ifndef TAPIOCA_EDIT_REGION_TERRAIN_HPP
#define TAPIOCA_EDIT_REGION_TERRAIN_HPP
#include "Edit/EditTerrainDesignJob.hpp"
#include "Edit/EditLandscapeTerrain.hpp"

namespace tapioca::geometry {
struct RegionTerrainResult {
    std::shared_ptr<const TerrainCapture> input;
    std::vector<EditTerrainSplitResult> preparation; // Retain every split's exact support catalogs.
    LandscapeTerrainResult evaluation;
};
std::vector<Id> TerrainRegionObjects (const geomsrv::edit::Document& doc);
// Immutable area worker stage: derives footprints/masks, splits their boundaries
// with Rust, then applies Rust area/surface edits. No path authoring or SDK calls.
bool EvaluateRegionTerrain (const geomsrv::edit::Document& doc, std::shared_ptr<const TerrainCapture> input,
                            RegionTerrainResult& result, std::string& error);
TerrainDesignOutcome RunLandscapeTerrainDesign (const TerrainDesignJob& job);
std::future<TerrainDesignOutcome> LaunchLandscapeTerrainDesign (TerrainDesignJob job);
} // namespace tapioca::geometry
#endif
