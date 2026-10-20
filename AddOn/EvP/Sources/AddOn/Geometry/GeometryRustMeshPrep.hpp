#ifndef TAPIOCA_GEOMETRY_RUST_MESH_PREP_HPP
#define TAPIOCA_GEOMETRY_RUST_MESH_PREP_HPP

#include "Aabb.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tapioca::geometry {
struct WeldCorner {
    int32_t sourceVertex = 0;
    std::array<double, 3> position {}, normal {};
};
struct WeldedCorners {
    std::vector<double> positions;
    std::vector<float> normals;
    std::vector<uint32_t> cornerVertices; // Every corner; all contributors retained.
};
struct EdgeRange {
    uint64_t offset = 0, count = 0;
};
struct MeshAnalysisRequest {
    std::span<const double> positions;
    std::span<const float> normals;
    std::span<const uint32_t> triangles;
    std::span<const uint64_t> visibleEdges;
    std::span<const EdgeRange> edgeRanges;
};
struct MeshAnalysis {
    geomsrv::Aabb bounds; // Tight SUPPLIED vertices, NOT retained extractor body bounds.
    bool normalsUsable = false;
    std::vector<float> smoothNormals;
    std::vector<uint8_t> roundoffZero, wireMasks;
};

// Opt-in G1 parity services. Worker-classified, no production consumer switch.
// Weld GIVEN normals, reset body-local key map at nondecreasing [0,...,N]
// offsets. First values retained, round(normal*1e4) halfway away from zero;
// no position proximity/averaging or semantic-ID allocation. Complete corner
// correspondence is geometry, NOT authored D14 lineage. Source/sidecars/IDs stay
// caller-owned. Whole owned result publishes only on success, input may alias
// old output. No SDK, retained borrows or exceptions cross the Rust boundary.
bool WeldCorners (std::span<const WeldCorner> corners, std::span<const uint64_t> bodyOffsets, WeldedCorners& result,
                  std::string& error);

// Analysis preserves topology/order. Visibility uses SAME index domain as the
// supplied triangles, not original source-body indices after welding. Empty
// ranges use a sorted global key pool; else one sorted range per triangle.
// Smooth fallback is area-weighted f32 legacy behavior, not crease-aware repair.
// Degenerate triangles remain, flagged by the legacy roundoff test. Caller keeps
// capture bounds spanning un-emitted vertices; these tight bounds do not replace
// them. Finite/representable geometry and derived arithmetic required atomically.
bool AnalyzeMesh (const MeshAnalysisRequest& request, MeshAnalysis& result, std::string& error);
} // namespace tapioca::geometry
#endif
