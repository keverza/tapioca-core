#ifndef TAPIOCA_GEOMETRY_RUST_TERRAIN_HPP
#define TAPIOCA_GEOMETRY_RUST_TERRAIN_HPP

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <span>
#include <vector>
#include <limits>
#include <optional>
#include "GeometryIdentity.hpp"

namespace geomsrv {
struct Mesh;
struct Snapshot;
} // namespace geomsrv
namespace geomsrv::engine {
struct Transform;
struct Vector3;
} // namespace geomsrv::engine

namespace tapioca::geometry {

// Service facade over the Rust geometry ABI. Legacy geometry-only and structural
// authored-attribution gates are separate; neither changes or copies geometry.
// Validation alone does not certify geometric contributor truth or D14 algorithms.
// Terrain services below return support mappings and retain the input catalog;
// no terrain math is implemented in C++. Complete value/catalog validation is a
// separate opt-in gate; all snapshots and catalogs stay C++ owned.
//
// Bulk or unbounded validation should run on a worker. Keep synchronous calls
// to inputs whose measured budget is small enough for the calling thread.

// Read the Rust aggregate ABI version and require Tapioca's supported version.
// On failure outVersion is unchanged (including a version mismatch). error is
// cleared on success and receives the ABI diagnostic or mismatch on failure.
bool ReadRustAbiVersion (uint32_t& outVersion, std::string& error);

// Validate one legacy mesh. Empty geometry is valid. Optional sidecars may be
// absent or full; source body/polygon, when present, must both be full and
// positive. Degenerate triangles are accepted because this is not a topology,
// manifold, winding or area gate. The mesh is never modified.
bool ValidateMesh (const geomsrv::Mesh& mesh, std::string& error);

// Validate snapshot mesh geometry and nonempty GUID uniqueness. Blank GUIDs may
// repeat. Builds only call-scoped mesh descriptors over the existing arrays;
// geometry is never copied. Material/surface catalogs are left untouched and
// are not validated in this increment.
bool ValidateSnapshot (const geomsrv::Snapshot& snapshot, std::string& error);

struct SnapshotValueOptions {
    uint64_t expectedCaptureStamp = 0; // Zero accepts detached/synthetic framing.
    bool requireCatalogs = false;      // Unknown/missing catalogs otherwise stay unknown.
};
// Complete detached values, including bounds and catalogs; separate from D14.
// A nonzero expected capture stamp requires matching retained stamp (not an ID). This
// does not replace the host's publication-time freshness check. Worker by default.
bool ValidateMeshValue (const geomsrv::Mesh& mesh, std::string& error);
bool ValidateSnapshotValue (const geomsrv::Snapshot& snapshot, const SnapshotValueOptions& options, std::string& error);

// Structural attribution validation, separate from the legacy geometry gate.
// Inputs are immutable; diagnostic changes only when validation completes.
// ERR_ARG/INTERNAL transport failures leave diagnostic unchanged. No ID repair,
// native writes, geometric attribution inference or retained Rust borrow.
bool ValidateProvenance (const ProvenanceRequest& request, ProvenanceDiagnostic& diagnostic, std::string& error);
// This primitive requires vertex+face domains and exact counts derived from the
// mesh; catalogs/Modeler sidecars remain additive. Bulk calls belong on workers.
bool ValidateAttributedMesh (const geomsrv::Mesh& mesh, const ProvenanceRequest& request,
                             ProvenanceDiagnostic& diagnostic, std::string& error);

enum class VectorTransformMode : uint32_t { Point = 1, Direction = 2, Normal = 3 };
struct TransformVectorsRequest {
    const geomsrv::engine::Transform& transform;
    VectorTransformMode mode;
    std::span<const double> values; // Interleaved xyz, world metres for points.
};
// Initial S2b affine primitive, not a mesh/identity conversion or production
// authority switch. Caller handles winding using flipsOrientation. On failure
// result and flipsOrientation are unchanged; no borrowed input is retained.
bool TransformVectors (const TransformVectorsRequest& request, std::vector<double>& result, bool& flipsOrientation,
                       std::string& error);

// S2b parity primitive. Input order is application order, folded from identity;
// empty input returns identity. Result is unchanged on refusal; inputs can alias
// result at this owned C++ facade (POD conversion completes before publication).
// No authored IDs, winding or sidecars are changed. Bulk chains use a worker.
bool ComposeTransforms (std::span<const geomsrv::engine::Transform> transforms, geomsrv::engine::Transform& result,
                        std::string& error);

enum class TransformBuilderKind : uint32_t { Translation = 1, Rotation = 2, Scaling = 3, Mirroring = 4 };
struct TransformBuilderRequest {
    TransformBuilderKind kind;
    const geomsrv::engine::Vector3& origin; // Pivot in metres; ignored for translation.
    const geomsrv::engine::Vector3& vector; // Displacement / axis / factors / normal.
    double radians = 0;                     // Rotation only; ignored otherwise.
};
// Bounded S2b parity builder, worker-classified until measured. Explicit POD copy;
// no retained references or geometry/identity changes. Output unchanged on refusal.
bool BuildTransform (const TransformBuilderRequest& request, geomsrv::engine::Transform& result, std::string& error);

struct PlaneValue {
    std::array<double, 3> origin {};          // World metres; exact retained value.
    std::array<double, 3> normal { 0, 0, 1 }; // Dimensionless, not necessarily unit.
    std::array<double, 3> u {};               // Optional authored tangent; zero derives seed axes.
};
struct PlaneProjection {
    PlaneValue plane;
    std::array<double, 3> u {}, v {}; // Derived dimensionless axes.
    std::vector<double> points;       // Interleaved xyz projected world metres.
};
// Neutral S2b value/frame/batch projection, not an Edit/WorkingPlane dependency.
// Raw plane fields round-trip unchanged; empty points derive a frame only.
// Result publishes atomically. Inputs may borrow its prior value/point storage.
// Bulk calls are worker-classified; no provenance or production consumer flip.
bool ProjectToPlane (const PlaneValue& plane, std::span<const double> points, PlaneProjection& result,
                     std::string& error);

struct PolylineSamples {
    std::vector<double> cumulative; // One world-metre length per input vertex.
    double totalLength = 0;
    std::vector<double> points;   // Interleaved xyz, world metres.
    std::vector<double> tangents; // Interleaved xyz directions, dimensionless.
};
// S2b parity primitive, no closure/welding, identity allocation or consumer flip.
// Points are flat xyz; parameters are finite clamped length fractions. Empty
// parameters measure only. Result publishes atomically; inputs may borrow its
// prior arrays because publication occurs after the ABI call ends. Worker by default.
bool SamplePolyline (std::span<const double> points, std::span<const double> parameters, PolylineSamples& result,
                     std::string& error);

// Equal arc-length segments, not result-point count: N+1 with ends, N-1 without.
// 1..100000 segments. One without ends returns empty after finite/shape checks,
// without evaluating unused lengths. Result publishes atomically and may own
// input points. No implicit closure/identity generation; worker until measured.
bool DividePolyline (std::span<const double> points, int32_t count, bool includeEnds, std::vector<double>& result,
                     std::string& error);

struct ArcRequest {
    const geomsrv::engine::Vector3& centre;
    const geomsrv::engine::Vector3& normal;
    double radius, startRadians, sweepRadians;
    int segments;
};
// S2b parity primitive: (segments+1) xyz points in sweep order, no endpoint snap.
// Result is unchanged on refusal; centre/normal are copied to POD, never
// reinterpreted or retained. Worker by default, no consumer flip.
bool MakeArc (const ArcRequest& request, std::vector<double>& result, std::string& error);

enum class TerrainConstraintRole : uint32_t { Boundary = 1, Hole = 2, Breakline = 3 };
struct TerrainConstraint {
    TerrainConstraintRole role;
    std::span<const uint32_t> vertices; // Implicitly closed for rings.
};
struct TerrainRefinement {
    double minAngleDegrees = 0;
    std::optional<double> maxAreaSquareMetres;
    uint32_t maxAddedVertices = 0;
};
struct TerrainRequest {
    std::span<const double> points; // xyz f64 world metres.
    std::span<const TerrainConstraint> constraints;
    double zTolerance = 0;
    std::optional<TerrainRefinement> refinement;
    const ProvenanceRequest& provenance; // N vertices + flattened constraint-segment edges, domains=3.
};
struct TerrainSplitRequest {
    std::span<const double> points;
    std::span<const uint32_t> triangles;
    std::span<const double> line, arcAngles;
    const ProvenanceRequest& provenance; // Mesh then line vertices, line edges, baseline faces; domains=7.
};
enum class TerrainMappingKind : uint32_t {
    InputVertex = 1,
    FaceBoundary = 2,
    Crossing = 3,
    ConstraintEdge = 4,
    SteinerSegment = 5,
    SteinerEdge = 6,
    SteinerFace = 7,
    SplitFace = 8,
    SplitVertex = 9,
    LineVertex = 10,
    LineEdge = 11,
    // Hybrid band rows (GeometryRustHybridTerrain.hpp); never from TIN or split.
    RegionFace = 12,
    RegionLineVertex = 13,
    RegionCrossing = 14,
    DesignLineVertex = 15,
    DesignLineEdge = 16,
    RingVertex = 17,
    SurfaceVertex = 18, // Output vertex -> each contributing native input vertex.
    SurfaceFace = 19    // Output face -> native input face.
};
// Lossless support records; indices/values have the documented ABI v1 row
// meanings (GeometryRustFfi.h), not semantic IDs. Source IDs, operation DAG,
// object owners and native GUIDs resolve through the retained input table.
struct TerrainMapping {
    TerrainMappingKind kind;
    uint32_t index = 0;
    std::array<uint32_t, 6> values {};
    bool operator== (const TerrainMapping&) const = default;
};
struct TerrainResult {
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    std::vector<TerrainMapping> mappings;
    ProvenanceRequest inputProvenance; // Complete unchanged catalog, retained support and native bindings.
    size_t inputPointCount = 0;        // Split line vertex i has input provenance index inputPointCount+i.
    bool operator== (const TerrainResult&) const = default;
};
// Pure worker-classified services, no Edit/SDK state or semantic-ID allocation.
// Query then fill recomputes in Rust (no retained handle); publishes the whole
// owned result only on success. Inputs may borrow a prior result. No latency or
// allocation-free claim; no C++ terrain algorithm or production consumer flip.
bool TriangulateTerrain (const TerrainRequest& request, TerrainResult& result, std::string& error);
bool SplitTerrainMesh (const TerrainSplitRequest& request, TerrainResult& result, std::string& error);

// ---- S3b.3 grading solve -------------------------------------------------
// Weighted sparse least squares in Rust: Tapioca's objective and constraints,
// faer only factorizes. Any bound, difference or start selects the constrained
// solve, which returns only a certified KKT point. Refusals (SolveInconsistent,
// SolveUnderdetermined, SolveNotConverged, malformed input) leave result
// unchanged. Worker-classified: one factorization (~15-20 ms at 10k unknowns)
// unconstrained, several plus an operator split when many limits bind.
struct GradingTerm {
    uint32_t unknown = 0;
    double coefficient = 0;
};
struct GradingRow {
    std::span<const GradingTerm> terms; // At least one; repeats of an unknown are summed.
    double target = 0;
    double weight = 1;
};
struct GradingFixed {
    uint32_t unknown = 0;
    double value = 0;
};
struct GradingBound {
    uint32_t unknown = 0;
    double lower = -std::numeric_limits<double>::infinity ();
    double upper = std::numeric_limits<double>::infinity ();
};
struct GradingDifference { // min <= x[a] - x[b] <= max
    uint32_t a = 0, b = 0;
    double min = -std::numeric_limits<double>::infinity ();
    double max = std::numeric_limits<double>::infinity ();
};
struct GradingLimits {
    uint32_t maxIterations = 200; // Factorized solves across all phases.
    double feasibility = 1e-9;    // Absolute, in the unknowns' units.
    double residual = 1e-10;      // Relative normal-equation residual.
};
struct GradingRequest {
    uint32_t unknowns = 0;
    std::span<const GradingRow> rows;
    std::span<const GradingFixed> fixed;
    std::span<const GradingBound> bounds;
    std::span<const GradingDifference> differences;
    std::span<const double> start; // Empty or one per unknown.
    GradingLimits limits;
};
struct GradingResult {
    std::vector<double> values;
    double objective = 0;
    uint32_t iterations = 0, factorizations = 0, active = 0;
};
bool SolveGrading (const GradingRequest& request, GradingResult& result, std::string& error);

// ---- S3b.3 terrain field, edits and volumes --------------------------------
// Upward faces only (unit normal z > 0.001). Edits move vertex heights only:
// indices, plan positions and triangles keep their identity, and owners name
// the edit index that moved each vertex (the caller maps it to its operation).
// Edit provenance (mesh vertices + faces, domains=5) is mandatory and retained.
// Every call publishes atomically; worker-classified.
struct TerrainMeshView {
    std::span<const double> points;      // xyz world metres
    std::span<const uint32_t> triangles; // vertex triples
};
// Native height-field solid -> upward compact plan surface. Exact xy/z merges
// only, all contributing input vertices/faces retained as rows 18/19. No repair
// or arbitrary-sheet overlap certification. Mandatory input domains=5; worker.
bool ExtractTerrainSurface (const TerrainMeshView& mesh, const ProvenanceRequest& provenance, TerrainResult& result,
                            std::string& error);
enum class TerrainGradientMethod : uint32_t { Central = 1, OneSided = 2, Face = 3 };
struct TerrainSample {
    bool onSurface = false; // Every other field is zero when false.
    double height = 0;
    uint32_t face = 0;
    std::array<double, 3> normal {};
    std::array<double, 2> gradient {}; // dz/dx, dz/dy
    std::array<TerrainGradientMethod, 2> method { TerrainGradientMethod::Central, TerrainGradientMethod::Central };
    double slope = 0;             // Radians above horizontal.
    std::optional<double> aspect; // Downslope bearing, radians clockwise from +y.
};
struct TerrainFieldSummary {
    std::array<double, 3> min {}, max {};
    double area = 0;
    uint32_t faces = 0, excluded = 0;
};
bool SampleTerrain (const TerrainMeshView& mesh, std::span<const double> queriesXY, double step,
                    std::vector<TerrainSample>& samples, TerrainFieldSummary& summary, std::string& error);

struct TerrainSlope { // One slope type everywhere; converted to a gradient in Rust.
    enum class Unit : uint32_t { Angle = 1, Percent = 2, RiseRun = 3, Height = 4 };
    Unit unit = Unit::Percent;
    double a = 0, b = 0; // Angle: a radians. Percent: a. RiseRun: a:b. Height: a m over b m.
};
struct TerrainDaylight {
    TerrainSlope slope;
    std::optional<double> extension; // Metres; beyond it the march stops and is reported.
};
enum class TerrainEditTarget : uint32_t { Point = 1, Curve = 2, Area = 3, Path = 4, Surface = 5 };
enum class TerrainEditMode : uint32_t { Absolute = 1, Relative = 2 };
enum class TerrainEditEffect : uint32_t { Cut = 1, Fill = 2, Both = 3 };
enum class TerrainEditTransition : uint32_t { Falloff = 1, Daylight = 2 };
enum class TerrainHand : uint32_t { Left = 1, Right = 2 };
struct TerrainCrossfall {
    TerrainSlope slope;
    TerrainHand fallsTo = TerrainHand::Left;
};
struct TerrainEdit {
    TerrainEditTarget target = TerrainEditTarget::Point;
    std::array<double, 3> position {}; // Point x, y and height (absolute) or displacement (relative).
    std::span<const double> geometry;  // Curve/path/area ring xyz.
    double width = 0;                  // Point radius, curve half width or path full width.
    std::optional<double> level;       // Area level; nullopt flattens to the inside mean.
    std::optional<TerrainCrossfall> crossfall;
    uint32_t surface = 0; // Index into TerrainEditRequest::surfaces.
    TerrainEditMode mode = TerrainEditMode::Absolute;
    TerrainEditEffect effect = TerrainEditEffect::Both;
    TerrainEditTransition transition = TerrainEditTransition::Falloff;
    double blur = 1;
    TerrainDaylight cut, fill;
    double maxDistance = 0;
    double strength = 1;
};
struct TerrainEditRequest {
    TerrainMeshView mesh;
    std::span<const TerrainEdit> edits; // Operation order; the index is the attribution.
    std::span<const TerrainMeshView> surfaces;
    std::span<const double> clip; // Property line xyz; empty for none.
    std::span<const std::span<const double>> protectedRings;
    std::optional<double> smoothing; // Gaussian radius, metres.
    const ProvenanceRequest& provenance;
};
struct TerrainEditReport {
    uint32_t moved = 0, extensionClipped = 0, limitClipped = 0;
    bool operator== (const TerrainEditReport&) const = default;
};
struct TerrainFaceOwner {
    uint32_t face = 0, edit = 0;
    bool operator== (const TerrainFaceOwner&) const = default;
};
enum class TerrainClipKind : uint32_t { Extension = 1, Limit = 2 };
// Where an edit left a vertex in place: the rows behind the report counters.
struct TerrainClip {
    uint32_t edit = 0, vertex = 0; // Vertex xyz is the position.
    TerrainClipKind kind = TerrainClipKind::Limit;
    double distance = 0; // From the edit's core, metres.
    double withheld = 0; // Signed height change not applied, metres.
    bool operator== (const TerrainClip&) const = default;
};
struct TerrainEditResult {
    std::vector<double> heights;
    std::vector<std::optional<uint32_t>> owners;
    std::vector<TerrainFaceOwner> faces; // Ascending (face, edit).
    std::vector<TerrainEditReport> reports;
    std::vector<TerrainClip> clips;    // By vertex then edit; sum to the counters.
    ProvenanceRequest inputProvenance; // Retained unchanged input catalog.
};
bool EditTerrain (const TerrainEditRequest& request, TerrainEditResult& result, std::string& error);

struct TerrainVolumes {
    double cut = 0, fill = 0, net = 0, area = 0;
    uint32_t samples = 0, excluded = 0;
};
struct TerrainEditVolumes {
    uint32_t edit = 0;
    double cut = 0, fill = 0, net = 0, area = 0;
};
struct TerrainVolumeRequest {
    enum class Proposed : uint32_t { Mesh = 1, Plane = 2, Edited = 3 };
    TerrainMeshView existing;
    Proposed proposed = Proposed::Plane;
    TerrainMeshView proposedMesh;                    // Mesh
    double plane = 0;                                // Plane
    std::span<const double> heights;                 // Edited: one per existing vertex
    std::span<const std::optional<uint32_t>> owners; // Edited: one per existing vertex
    double cell = 1;                                 // Metres: coarse 2 m on move, fine 0.25 m on end.
    uint32_t subdivisions = 1;
    std::optional<std::array<double, 4>> domain; // xMin yMin xMax yMax; existing bounds otherwise.
    std::span<const double> property;            // xyz ring; outside it is context.
};
struct TerrainVolumeReport {
    TerrainVolumes site, context;
    std::array<uint32_t, 2> cells {};
    std::vector<TerrainEditVolumes> edits; // Per owning edit; net sums to the total.
};
bool MeasureTerrainVolumes (const TerrainVolumeRequest& request, TerrainVolumeReport& report, std::string& error);

} // namespace tapioca::geometry

#endif
