#ifndef TAPIOCA_EDIT_REGION_TERRAIN_HPP
#define TAPIOCA_EDIT_REGION_TERRAIN_HPP
#include "Edit/EditTerrainDesignJob.hpp"
#include "Edit/EditLandscapeTerrain.hpp"
#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Geometry/GeometryRustUnifiedTerrain.hpp"

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
// Which lane a landscape design request runs on (#112 P4c). The legacy lane grades every path
// from its paved outline and runs only when there is no region terrain target and the request
// is an outline (or names no path); everything else is one unified build over a compiled plan.
// RunLandscapeTerrainDesign dispatches on this, and the recipe view (EditLandscapeRecipe.hpp)
// reports it, so the two can never disagree.
enum class TerrainRoute : uint8_t { Legacy, Unified };
TerrainRoute LandscapeTerrainRoute (const geomsrv::edit::Document& doc, bool hasPath, bool outline);
// The unified plan's path request for a design's options, exactly as the unified lane builds it.
// `namedRegion`: the caller passed an explicit rebuild region (the landscape lane never does).
PathDesignRequest PathRequestFor (const TerrainDesignOptions& options, bool namedRegion);
TerrainDesignOutcome RunLandscapeTerrainDesign (const TerrainDesignJob& job);
std::future<TerrainDesignOutcome> LaunchLandscapeTerrainDesign (TerrainDesignJob job);

// #175: what the design looks like, from its field: the job's compiled plan previewed on a grid
// (PreviewUnifiedLandscapeTerrain) -- surface, contours, cut/fill -- in milliseconds, for every
// edit, while the build (above) meshes it in seconds. The legacy lane has no preview.
struct TerrainPreviewOutcome {
    uint64_t revision = 0;
    bool built = false;
    UnifiedPreviewResult preview; // When built.
    std::string error;            // When not.
    double milliseconds = 0.0;    // Wall time on the worker.
};
TerrainPreviewOutcome RunLandscapeTerrainPreview (const TerrainDesignJob& job, const UnifiedPreviewAsk& ask);
std::future<TerrainPreviewOutcome> LaunchLandscapeTerrainPreview (TerrainDesignJob job, UnifiedPreviewAsk ask);
} // namespace tapioca::geometry
#endif
