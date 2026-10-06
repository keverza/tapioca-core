#include "ArchViz/MassingCollapseZone.hpp"
#include "ArchViz/TerrainProjection.hpp"

namespace geomsrv::archviz::massingcollapse {
bool Project (const Result& zone, const Mesh& terrain, overlaylayers::Layer& layer, std::string& error)
{
    terrainprojection::Style style;
    style.name = kProjectedLayer;
    return terrainprojection::Project (zone.chains, zone.hatchOriginSum, terrain, style, layer, error);
}
} // namespace geomsrv::archviz::massingcollapse
