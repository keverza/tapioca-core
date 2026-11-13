// The Rust terrain kernel's entry points, refusing. The Playground links the editor kernel without
// the Rust bridge (no terrain worker runs here), but EditLandscapeTerrainJob.cpp and
// EditRegionTerrain.cpp, which the editor's recipe and region code share, name them.
#include "Edit/EditLandscapeTerrain.hpp"
#include "Edit/EditLandscapeUnifiedBuild.hpp"
#include "Edit/EditPublishedTerrain.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditTerrainDesign.hpp"
#include "Edit/EditTerrainDesignJob.hpp"
#include "Edit/EditTerrainTriangulation.hpp"

namespace tapioca::geometry {
namespace {
constexpr char kRefusal[] = "Terrain requires the Rust geometry build; the UI Playground has none.";
bool Refuse (std::string& error)
{
    error = kRefusal;
    return false;
}
} // namespace
bool EvaluateLandscapeTerrain (const geomsrv::edit::Document&, const LandscapeTerrainRequest&, LandscapeTerrainResult&,
                               std::string& error)
{
    return Refuse (error);
}
bool BuildUnifiedLandscapeTerrain (const geomsrv::edit::Document&, const TerrainCapture&, const FieldPlan&,
                                   const UnifiedLandscapeBuildOptions&, UnifiedTerrainResult&,
                                   UnifiedLandscapeBuildScope&, std::string& error)
{
    return Refuse (error);
}
bool PreviewUnifiedLandscapeTerrain (const geomsrv::edit::Document&, const TerrainCapture&, const FieldPlan&,
                                     const UnifiedLandscapeBuildOptions&, const UnifiedPreviewAsk&,
                                     UnifiedPreviewResult&, std::string& error)
{
    return Refuse (error);
}
bool PublishUnifiedTerrain (const UnifiedTerrainResult&, const FieldPlan&, const TerrainCapture&,
                            const UnifiedLandscapeBuildScope&, PublishedTerrain&, std::string& error)
{
    return Refuse (error);
}
bool PrepareTerrainSurface (std::shared_ptr<const TerrainCapture>, uint64_t, TerrainCapture&, std::string& error)
{
    return Refuse (error);
}
std::vector<double> TerrainDesignArea (const geomsrv::edit::Document&, const TerrainMeshView&)
{
    return {};
}
bool AssessTerrainDesign (const geomsrv::edit::Document&, const EditTerrainInput&, const TerrainDesignOptions&,
                          std::vector<TerrainDesignConflict>&, std::string& error)
{
    return Refuse (error);
}
std::string TerrainConditionText (const std::string& error)
{
    return error;
}
TerrainDesignOutcome RunTerrainDesign (const TerrainDesignJob&)
{
    TerrainDesignOutcome outcome;
    outcome.error = kRefusal;
    return outcome;
}
bool SplitEditTerrainRing (const geomsrv::edit::Document&, const EditTerrainInput&, const EditTerrainSplitOptions&,
                           std::span<const geomsrv::edit::Vec3>, std::span<const SourceId>, EditTerrainSplitResult&,
                           std::string& error)
{
    return Refuse (error);
}
} // namespace tapioca::geometry
