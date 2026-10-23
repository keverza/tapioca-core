#ifndef TAPIOCA_EDIT_TERRAIN_CONTROLS_HPP
#define TAPIOCA_EDIT_TERRAIN_CONTROLS_HPP

#include "Edit/EditTerrainDesign.hpp"

namespace tapioca::geometry::terrain_detail {
// Revision-local controls compiled solely from the document's authored refs.
// Full vertex/segment IDs remain beside the geometry; no second authoring model.
struct TerrainControls {
    std::vector<std::vector<double>> lines;
    std::vector<double> points;
    std::vector<std::vector<TerrainDesignControlRef>> lineSupport;
    std::vector<TerrainDesignControlRef> pointSupport;
};
bool CompileTerrainControls (const geomsrv::edit::Document& doc, const ProvenanceRequest& captured,
                             TerrainControls& result, std::string& error);
bool AppendTerrainControls (const TerrainControls& controls, const TerrainDesignOptions& options,
                            const IdentityObject& owner, ProvenanceRequest& combined, std::string& error);
TerrainDesignControlRef ResolveTerrainControl (const TerrainControls& controls, const HybridControlRef& ref);
} // namespace tapioca::geometry::terrain_detail
#endif
