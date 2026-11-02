#ifndef TAPIOCA_EDIT_TERRAINSTATION_HPP
#define TAPIOCA_EDIT_TERRAINSTATION_HPP

#include "Edit/EditPath.hpp"
#include "Geometry/GeometryRustHybridTerrain.hpp"

namespace tapioca::geometry {

// The single C++ station transport used by both the legacy path lane and the
// unified field-plan adapter. Percent is the already prepared signed crossfall;
// all positions/reaches are world metres and authored slope forms are retained.
inline TerrainStation CompileTerrainStation (const geomsrv::edit::StationFrame& frame, double crossfallPercent,
                                             double cutLimit, double fillLimit)
{
    const auto& section = frame.section;
    TerrainStation value;
    value.position = { frame.centre.x, frame.centre.y, frame.centre.z };
    value.halfLeft = section.widthLeft;
    value.halfRight = section.widthRight;
    if (crossfallPercent != 0.0 && section.fall == geomsrv::edit::Fall::Crown)
        value.crown = TerrainSlope { TerrainSlope::Unit::Percent, crossfallPercent, 0 };
    else if (crossfallPercent != 0.0) {
        value.crossfall = TerrainCrossfall {
            { TerrainSlope::Unit::Percent, crossfallPercent, 0 },
            section.fall == geomsrv::edit::Fall::Left ? TerrainHand::Left : TerrainHand::Right,
        };
    }
    value.shoulderLeft = value.shoulderRight = section.shoulder;
    value.cut = section.cutSlope;
    value.fill = section.fillSlope;
    value.cutLimit = cutLimit;
    value.fillLimit = fillLimit;
    value.rounding = section.rounding;
    return value;
}

} // namespace tapioca::geometry
#endif
