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
    // Optional fixed controls: provenance additionally covers line vertices
    // (line order), then independent points; line segments follow ring edges.
    std::span<const std::vector<double>> controlLines;
    std::span<const double> controlPoints;
};
enum class HybridLineKind : uint32_t { Edge = 1, Shoulder = 2, Daylight = 3, End = 4, Crown = 5, Locked = 6 };
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
enum class HybridConflictCode : uint32_t {
    DaylightNotReached = 1,    // Magnitude: z_cone - z_base where the face search stopped.
    DaylightBeyondLimit = 2,   // Magnitude: the reach needed, metres from the edge.
    VerticalFace = 3,          // A wall is a separate surface.
    DaylightTopology = 4,      // Magnitude: root count here (0 at grade).
    EdgeContradictsEffect = 5, // Soft. Magnitude: z_edge - z_base.
    LockedConflict = 6,        // Hard. Magnitude: other/proposed minus fixed height.
};
struct HybridControlRef {
    enum class Kind : uint32_t { Segment = 1, Point = 2 };
    Kind kind = Kind::Point;
    uint32_t index = 0, segment = 0; // Input line/point and segment (0 for points).
    bool operator== (const HybridControlRef&) const = default;
};
struct HybridConflict {
    HybridConflictCode code = HybridConflictCode::DaylightNotReached;
    uint32_t station = 0;
    TerrainHand side = TerrainHand::Left;
    std::array<double, 3> position {}; // Band edge, or where a face search stopped.
    double magnitude = 0;
    bool hard = true; // The build refuses on it.
    // Fixed conflict only: station=UINT32_MAX, no side semantics.
    std::optional<HybridControlRef> control, other;
    uint32_t vertex = UINT32_MAX; // In the refused candidate's topology.
    bool operator== (const HybridConflict&) const = default;
};
// The stable code text and its suggested fix.
const char* HybridConflictName (HybridConflictCode code);
const char* HybridConflictSuggestion (HybridConflictCode code);

struct HybridTerrainResult {
    std::vector<double> points;                  // Proposed xyz; plan equals the rebuilt base.
    std::vector<double> base;                    // Base height per output vertex.
    std::vector<std::optional<uint32_t>> owners; // 0 where the band changed the height.
    std::vector<uint32_t> triangles;
    std::vector<TerrainMapping> mappings;         // Kinds 9 and 12..17 (GeometryRustFfi.h).
    std::vector<HybridLine> lines;                // Indexed by the line of rows 13..16.
    std::vector<std::array<HybridSide, 2>> sides; // [left, right] per station.
    HybridVolumes site, context;                  // Inside / outside the region.
    std::vector<HybridConflict> conflicts;        // Soft only: where the band is held back.
    ProvenanceRequest inputProvenance;            // Retained unchanged input catalog.
    size_t inputPointCount = 0;                   // Ring vertex i has input provenance index inputPointCount+i.
    size_t inputRegionCount = 0, inputControlPointStart = 0;
    std::vector<size_t> inputControlLineStarts; // Combined provenance vertex index of each line's first vertex.
    struct FixedVertex {
        uint32_t vertex = 0;
        std::vector<HybridControlRef> supports; // Every contributor; exact height is in `points`.
        bool operator== (const FixedVertex&) const = default;
    };
    std::vector<FixedVertex> locked;
    bool operator== (const HybridTerrainResult&) const = default;
};
// Query then fill (no retained handle); the whole result publishes only on
// success. Refusals carry the kernel's code: DaylightNotReached, HybridVertical,
// HybridDaylightTopology, Region*, Design*, or a Provenance* attribution refusal.
bool BuildHybridTerrain (const HybridTerrainRequest& request, HybridTerrainResult& result, std::string& error);
// Every located conflict, hard and soft, without building; succeeds for a band
// the build refuses. Same attribution refusals; conflicts unchanged on refusal.
bool AssessHybridTerrain (const HybridTerrainRequest& request, std::vector<HybridConflict>& conflicts,
                          std::string& error);

} // namespace tapioca::geometry
#endif
