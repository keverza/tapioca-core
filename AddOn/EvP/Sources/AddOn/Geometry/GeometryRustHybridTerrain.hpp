#ifndef TAPIOCA_GEOMETRY_RUST_HYBRID_TERRAIN_HPP
#define TAPIOCA_GEOMETRY_RUST_HYBRID_TERRAIN_HPP

#include "Geometry/GeometryRustTerrain.hpp"

namespace tapioca::geometry {

// ---- S4.2 hybrid band -------------------------------------------------------
// One path band built on a base mesh inside a closed straight region ring, as a
// constrained TIN in Rust: band edges, shoulder ends, daylight lines and end
// sections are breaklines and every base edge is kept, so the base is affine on
// every output triangle and the triangle-sum cut/fill is exact. Outside triangles
// are verbatim; outside and daylight-line vertices keep their base height bits.
// No terrain math in C++. Worker-classified; P1 rebuilds the whole region.
struct TerrainStation {
    std::array<double, 3> position {}; // Centreline x, y and design height, world metres.
    double halfLeft = 0, halfRight = 0;
    std::optional<TerrainCrossfall> crossfall; // One-way fall about the centreline.
    std::optional<TerrainSlope> crown;         // Both halves fall away; exclusive with crossfall.
    double shoulderLeft = 0, shoulderRight = 0;
    TerrainSlope cut, fill;             // Face grades below / above the ground.
    double cutLimit = 0, fillLimit = 0; // Plan reach of each face, metres.
};
struct HybridTerrainRequest {
    TerrainMeshView mesh;
    std::span<const double> region;           // Closed straight xyz ring (z ignored), implicit closing edge.
    std::span<const TerrainStation> stations; // Travel order, at least two.
    TerrainEditEffect effect = TerrainEditEffect::Both;
    double tolerance = 1e-3;             // At-grade height band, metres.
    const ProvenanceRequest& provenance; // Mesh then ring vertices, ring edges, mesh faces; domains=7.
};
enum class HybridLineKind : uint32_t { Edge = 1, Shoulder = 2, Daylight = 3, End = 4, Crown = 5 };
struct HybridLine {
    HybridLineKind kind = HybridLineKind::Edge;
    std::optional<TerrainHand> side; // Absent for an end section or the crowned centreline.
    uint32_t root = 0;               // Daylight root outward; an end's 0 first / 1 last.
    bool operator== (const HybridLine&) const = default;
};
struct HybridSide {
    uint32_t roots = 0;         // 0: at grade.
    double first = 0, last = 0; // Metres outward from the band edge.
    bool operator== (const HybridSide&) const = default;
};
struct HybridVolumes {
    double cut = 0, fill = 0, net = 0, area = 0;
    uint32_t triangles = 0;
    bool operator== (const HybridVolumes&) const = default;
};
struct HybridTerrainResult {
    std::vector<double> points;                  // Proposed xyz; plan equals the rebuilt base.
    std::vector<double> base;                    // Base height per output vertex.
    std::vector<std::optional<uint32_t>> owners; // 0 where the band changed the height.
    std::vector<uint32_t> triangles;
    std::vector<TerrainMapping> mappings;         // Kinds 9 and 12..17 (GeometryRustFfi.h).
    std::vector<HybridLine> lines;                // Indexed by the line of rows 13..16.
    std::vector<std::array<HybridSide, 2>> sides; // [left, right] per station.
    HybridVolumes site, context;                  // Inside / outside the region.
    ProvenanceRequest inputProvenance;            // Retained unchanged input catalog.
    size_t inputPointCount = 0;                   // Ring vertex i has input provenance index inputPointCount+i.
    bool operator== (const HybridTerrainResult&) const = default;
};
// Query then fill (no retained handle); the whole result publishes only on
// success. Refusals carry the kernel's code: DaylightNotReached, HybridVertical,
// HybridDaylightTopology, Region*, Design*, or a Provenance* attribution refusal.
bool BuildHybridTerrain (const HybridTerrainRequest& request, HybridTerrainResult& result, std::string& error);

} // namespace tapioca::geometry
#endif
