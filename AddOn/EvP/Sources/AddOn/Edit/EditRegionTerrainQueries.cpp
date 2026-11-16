#include "Edit/EditRegionTerrain.hpp"
#include <algorithm>

namespace tapioca::geometry {

std::vector<Id> TerrainRegionObjects (const geomsrv::edit::Document& doc)
{
    // Valid owned-root discovery is independent of the selected calculation
    // lane. Boolean-derived configured domains participate through their root.
    // #189: a construction input of another region is not an owner.
    std::vector<Id> ids;
    std::vector<Id> construction;
    if (doc.landscape)
        for (const auto& input : LandscapeConstructionInputs (*doc.landscape))
            construction.push_back (input.object);
    if (doc.landscape)
        for (const auto& object : doc.landscape->objects)
            if (object.terrain.effect != RegionTerrainEffect::None &&
                std::find (construction.begin (), construction.end (), object.id) == construction.end () &&
                std::any_of (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                             [&] (const auto& op) { return op.id == object.footprintOperation; }))
                ids.push_back (object.id);
    return ids;
}

TerrainRoute LandscapeTerrainRoute (const geomsrv::edit::Document& doc, bool hasPath, bool outline)
{
    // Unified outlines are a build stage; a request with neither a region target
    // nor a path retains the legacy lane and its established refusal text.
    (void) outline;
    return TerrainRegionObjects (doc).empty () && !hasPath ? TerrainRoute::Legacy : TerrainRoute::Unified;
}

} // namespace tapioca::geometry
