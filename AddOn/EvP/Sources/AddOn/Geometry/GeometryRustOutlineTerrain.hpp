#ifndef TAPIOCA_GEOMETRY_RUST_OUTLINE_TERRAIN_HPP
#define TAPIOCA_GEOMETRY_RUST_OUTLINE_TERRAIN_HPP

#include "Geometry/GeometryRustHybridTerrain.hpp"
#include <vector>

namespace tapioca::geometry {

// ---- outline grading ----------------------------------------------------------
// The terrain under and around a paved area, graded in Rust (terrain::outline):
// the paved surface's own heights inside the area; outside it, faces from the
// area's outline at the cut or fill grade, rounded at top and toe, meeting the
// ground, and the base beyond `limit`. The region is rebuilt with the caller's
// plan breaklines (z ignored) and isolated points; outside vertices keep their
// base height bits. No terrain math in C++. A preview: no support rows.
struct OutlineTerrainRequest {
    TerrainMeshView mesh;                       // the base terrain
    std::span<const double> region;             // closed straight xyz ring, z ignored
    TerrainMeshView surface;                    // the paved area's TIN, xyz
    std::span<const std::vector<double>> lines; // open plan polylines strictly inside the region
    std::span<const double> spots;              // plan points strictly inside the region
    TerrainSlope cut, fill;
    double limit = 10, rounding = 0, tolerance = 1e-3; // metres
    TerrainEditEffect effect = TerrainEditEffect::Both;
};
struct OutlineTerrainResult {
    std::vector<double> points, base; // proposed xyz; base height per vertex
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> ring; // the region ring as output vertices
    HybridVolumes site, context;
    uint32_t unreached = 0; // vertices left at the base although their face had not met it
};
// Atomic: the result changes only on success.
bool BuildOutlineTerrain (const OutlineTerrainRequest& request, OutlineTerrainResult& result, std::string& error);

} // namespace tapioca::geometry
#endif
