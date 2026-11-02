#ifndef TAPIOCA_GEOMETRY_RUST_UNIFIED_TERRAIN_HPP
#define TAPIOCA_GEOMETRY_RUST_UNIFIED_TERRAIN_HPP

#include "Geometry/GeometryRustHybridTerrain.hpp"

namespace tapioca::geometry {

// ---- #112 P3 unified landscape build ---------------------------------------
// One immutable baseline mesh, one ordered field plan (region entries, sampled
// path bands, grading/protection masks and fixed controls) and one constrained
// result topology, built in Rust through the additive tapioca_terrain_unified_build
// ABI. No terrain math in C++; this facade converts to the fixed-layout POD
// plan ingress (offset pools only), runs query then fill (no retained handle)
// and publishes the complete owned result only on success. Existing/Constant
// surface targets only, matching the kernel; non-constant target meshes and
// production dispatch are later slices. Worker-classified.
enum class UnifiedSurfaceKind : uint32_t { Existing = 1, Constant = 2 };
struct UnifiedSurface {
    UnifiedSurfaceKind kind = UnifiedSurfaceKind::Existing;
    double level = 0; // Constant target height inside the domain, metres.
    bool operator== (const UnifiedSurface&) const = default;
};
enum class UnifiedTerrainEffect : uint32_t { Grade = 1, Add = 2, Subtract = 3 };
enum class UnifiedTransitionKind : uint32_t { Falloff = 1, Daylight = 2 };
struct UnifiedTransition {
    UnifiedTransitionKind kind = UnifiedTransitionKind::Falloff;
    double falloff = 0;               // Falloff blur outside the domain, metres.
    TerrainSlope cut, fill;           // Daylight face grades.
    double reach = 0;                 // Maximum daylight reach, metres.
    bool operator== (const UnifiedTransition&) const = default;
};
// One evaluated domain part: one implicitly closed outer ring (xyz) and holes.
struct UnifiedDomainPart {
    std::span<const double> outer;
    std::span<const std::span<const double>> holes;
    bool operator== (const UnifiedDomainPart&) const = default;
};
struct UnifiedEntry {
    uint32_t order = 0; // Contiguous stack index; later entries override earlier ones.
    uint64_t object = 0, rootOperation = 0;
    std::span<const uint64_t> sources;       // Evaluated domain ancestry.
    std::span<const UnifiedDomainPart> parts;
    UnifiedSurface surface;
    UnifiedTerrainEffect effect = UnifiedTerrainEffect::Grade;
    UnifiedTransition transition;
    bool operator== (const UnifiedEntry&) const = default;
};
struct UnifiedPath {
    uint32_t order = 0; // Stack position after every region entry.
    uint64_t source = 0, operation = 0, object = 0;
    std::span<const TerrainStation> stations; // Travel order, at least two.
    TerrainEditEffect effect = TerrainEditEffect::Both;
    double tolerance = 1e-3; // At-grade height band, metres.
    bool operator== (const UnifiedPath&) const = default;
};
enum class UnifiedMaskKind : uint32_t { GradingLimit = 1, ProtectedZone = 2 };
struct UnifiedMask {
    uint64_t operation = 0;
    std::span<const uint64_t> sources;
    UnifiedMaskKind kind = UnifiedMaskKind::GradingLimit;
    std::span<const UnifiedDomainPart> parts;
    bool operator== (const UnifiedMask&) const = default;
};
struct UnifiedControl {
    uint64_t source = 0, vertex = 0; // Authored vertex identity for a point.
    bool point = false;              // True: fixed point; false: fixed open line.
    std::array<double, 3> pointPosition {};
    std::span<const double> line; // Open xyz polyline when point is false.
    bool operator== (const UnifiedControl&) const = default;
};
struct UnifiedPlanRequest {
    TerrainMeshView mesh;
    std::span<const double> property; // Closed ring; empty selects the whole baseline.
    std::span<const UnifiedEntry> entries;
    std::span<const UnifiedPath> paths;
    std::span<const UnifiedMask> masks;
    std::span<const UnifiedControl> controls;
    double controlTolerance = 1e-3;   // Absolute grading/fixed compatibility band.
    uint64_t revision = 0, planHash = 0;
    // Mesh then property, entry rings (part order, outer then holes), mask
    // rings, control line vertices, then point controls; domains=7.
    const ProvenanceRequest& provenance;
};
enum class UnifiedLineDomain : uint32_t { Entry = 1, Path = 2, Mask = 3, Control = 4 };
// Authored origin of one constrained breakline, in breakline order.
struct UnifiedLine {
    UnifiedLineDomain domain = UnifiedLineDomain::Entry;
    uint32_t index = 0;   // Entry/path/mask/control index.
    uint32_t part = 0;    // Entry/mask part index.
    uint32_t ring = 0;    // Entry/mask ring: zero outer, one-based hole.
    uint32_t segment = 0; // Entry/mask ring segment; control: authored segment count.
    std::optional<HybridLineKind> kind; // Path design line only.
    std::optional<TerrainHand> side;    // Path design line only.
    uint32_t root = 0;                  // Path design line only.
    bool operator== (const UnifiedLine&) const = default;
};
struct UnifiedLocked {
    uint32_t control = 0, vertex = 0;
    double height = 0; // Metres; the exact height is also in result.points.
    bool operator== (const UnifiedLocked&) const = default;
};
struct UnifiedTerrainResult {
    std::vector<double> points;                  // Proposed xyz.
    std::vector<double> base;                    // Baseline height per output vertex.
    std::vector<std::optional<uint32_t>> owners; // Last stack position that moved the vertex.
    std::vector<uint32_t> triangles;
    std::vector<TerrainMapping> mappings; // Kinds 9, 12..17, 20/21 (GeometryRustFfi.h).
    std::vector<UnifiedLine> lines;       // Indexed by the mapping line rows.
    std::vector<UnifiedLocked> locked;
    HybridVolumes site, context; // Exact volumes inside/outside the property.
    uint64_t revision = 0, planHash = 0;
    ProvenanceRequest inputProvenance; // Retained unchanged input catalog.
    size_t inputPointCount = 0;        // Property vertex i has input provenance index inputPointCount+i.
    size_t inputPropertyCount = 0;
    bool operator== (const UnifiedTerrainResult&) const = default;
};
// Query then fill; the whole result publishes only on success and stays
// unchanged on any refusal. Refusals carry the kernel's code: Build*, Region*,
// Design*, LockedConflict, HardStep, or a Provenance* attribution refusal.
bool BuildUnifiedTerrain (const UnifiedPlanRequest& request, UnifiedTerrainResult& result, std::string& error);

} // namespace tapioca::geometry
#endif
