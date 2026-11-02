#ifndef TAPIOCA_EDIT_LANDSCAPEFIELDPLAN_HPP
#define TAPIOCA_EDIT_LANDSCAPEFIELDPLAN_HPP

// #112 P2: one immutable, versioned field plan compiled from the validated
// document, evaluated operation values, prepared region surfaces, path/station
// preparation, masks and locked controls. Pure evaluation only: no document
// writes, no triangulation, no baseline edits and zero triangles in the plan.
// The compiled plan is the single input the Rust mesh authority (P3) will
// consume; production lanes stay unchanged and the compiler runs in tests and
// inspection until P3 wires it into the worker.
#include "Edit/EditLandscape.hpp"
#include "Edit/EditPath.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace geomsrv::edit {
struct Document;
}

namespace tapioca::geometry {

inline constexpr uint32_t kFieldPlanVersion = 2;

// Owned equivalent of the path lane's design request (no caller-owned spans):
// which document path is designed and the band parameters the existing C++
// station preparation resolves. A path contributes constraints and height
// targets to the plan; it is not a second mesh build.
struct PathDesignRequest {
    Id source = kNoId;         // PathCenterline source naming the document path.
    OperationId operation = 0; // Enabled DAG operation consuming the source.
    ObjectId object = 0;       // One terrain owner; mixed ownership refuses downstream.
    double maxStep = 2.0;      // Station spacing, metres, plus every break.
    double taper = 0.5;        // Cross-section range-change length, metres.
    double margin = 1.0;       // Derived rebuild ring margin, metres.
    double arcTolerance = 0.05;
    double cutLimit = 10, fillLimit = 10; // Face plan reach, metres.
    TerrainEditEffect effect = TerrainEditEffect::Both;
    double tolerance = 1e-3; // At-grade height band, metres.
    bool outline = false;    // Grade from the paved outline of every path.
    bool operator== (const PathDesignRequest&) const = default;
};

// Immutable capture-side identity the compiler reads. `doc` is caller-owned
// and must outlive the call — an immutable document snapshot, never live state
// the user is still editing; the plan copies what it keeps and never writes
// back. Evaluated domains, masks and controls are derived inside the compiler
// from this snapshot through the existing evaluation authorities (pure stage
// 1/2 work, no triangulation).
struct LandscapeSnapshot {
    const geomsrv::edit::Document* doc = nullptr;
    uint64_t revision = 0;        // Document::revision at capture.
    uint64_t captureRevision = 0; // TerrainCapture::revision; 0: no capture yet.
    std::array<uint8_t, 16> session {};
    std::string sourceTerrainGuid;
    // The current single path design request; absent compiles a region-only
    // plan. Multi-path UI and new spot authoring stay out of scope.
    std::optional<PathDesignRequest> path;
};

// One evaluated domain part: rings[0] is the outer, rings[1..] its holes, in
// the Clipper2-backed planar evaluation's own nesting (PolyTree hierarchy),
// never reconstructed from flat path order. Coordinates are world metres; z is
// the authored surface/seed height, never a terrain result.
struct FieldPlanPart {
    std::vector<std::vector<Vec3>> rings;
    bool operator== (const FieldPlanPart&) const = default;
};

enum class FieldPlanStage : uint8_t { Snapshot, Domain, Surface, Stack, Path, Constraint };
enum class FieldPlanDiagnosticCode : uint8_t {
    SnapshotMismatch,     // Missing document/session, or the snapshot is not a landscape capture.
    DagInvalid,           // The operation DAG does not validate; no entries can be ordered.
    EvaluationRefused,    // The stage-1 domain evaluation refused; no domains can be compiled.
    OwnerRootMissing,     // An active owner cites no live DAG root; its entry is skipped.
    SurfacePrepareFailed, // The authored surface field cannot be prepared for evaluation.
    PathUnresolved,       // The design request names no live path centerline/path.
    ControlOrphaned,      // An enabled locked control has no live source or vertex.
    ControlUnsupported    // A fixed line control requires an open straight polyline.
};

// Diagnostics carry the stage plus the object/operation/source and the
// ring/part where one applies (UINT32_MAX: not applicable).
struct FieldPlanDiagnostic {
    FieldPlanStage stage = FieldPlanStage::Snapshot;
    FieldPlanDiagnosticCode code = FieldPlanDiagnosticCode::SnapshotMismatch;
    Id object = kNoId, operation = kNoId, source = kNoId;
    uint32_t ring = UINT32_MAX, part = UINT32_MAX;
    std::string message;
    bool operator== (const FieldPlanDiagnostic&) const = default;
};

// One active owner's stack entry. Every active owner (a configured terrain
// effect on a live owned root) contributes exactly one entry, ordered by
// root-operation priority, then topological DAG order, then object id, so a
// derived result follows its operands and later entries override earlier ones.
struct FieldPlanEntry {
    uint32_t order = 0; // Stack position; paths follow every region entry.
    int priority = 0;   // The root operation's explicit priority; hash-visible.
    Id object = kNoId;
    OperationId rootOperation = 0;
    ObjectKind kind = ObjectKind::Playground;
    std::string name;
    MaterialSpec material;
    double thickness = 0.0;
    std::vector<Id> sources; // Evaluated domain ancestry.
    std::vector<FieldPlanPart> parts;
    std::optional<RegionSurface> surface; // Authored surface field, verbatim.
    // Non-constant arbitrary plane target transported for the unified Rust build:
    // flat xyz points and triangle triples, empty for Existing/Constant Area targets.
    std::vector<double> surfacePoints;
    std::vector<uint32_t> surfaceTriangles;
    RegionTerrain terrain;                // Effect/transition/falloff/reach/cut/fill, verbatim.
    bool operator== (const FieldPlanEntry&) const = default;
};

// A compiled path band. Stations come from the existing C++ preparation
// (ValidatePath/PreparePath/SampleStations/Evaluate): each station is a height
// target and a band constraint, never a mesh.
struct FieldPlanPath {
    uint32_t order = 0; // After every region entry by default.
    Id source = kNoId;
    OperationId operation = 0;
    ObjectId object = 0;
    Id path = kNoId;
    PathDesignRequest design;
    std::vector<geomsrv::edit::StationFrame> stations;
    // Prepared crossfall percent per station; zero means level. The authored
    // typed slope and fall hand remain available on each StationFrame::section.
    std::vector<double> crossfallPercent;
    bool operator== (const FieldPlanPath&) const = default;
};

// A grading limit or protected zone mask, with full evaluated extents; clipping
// to a footprint is the mesh stage's job. Protection pins baseline heights; a
// grading limit bounds influence.
struct FieldPlanMask {
    OperationId operation = 0;
    OperationKind kind = OperationKind::GradingLimit;
    std::vector<Id> sources;
    std::vector<FieldPlanPart> parts;
    bool operator== (const FieldPlanMask&) const = default;
};

// One locked point/line control resolved from the document's authored refs.
struct FieldPlanControlRef {
    Id source = kNoId, segment = kNoId;
    std::array<Id, 2> vertices {};
    bool operator== (const FieldPlanControlRef&) const = default;
};
struct FieldPlanControl {
    bool point = true;
    Id source = kNoId, vertex = kNoId;         // Authored reference.
    Vec3 pointPosition;                        // Point control: the locked vertex.
    std::vector<Vec3> line;                    // Line control: the polyline's vertices.
    std::vector<FieldPlanControlRef> segments; // Authored per-segment support.
    bool operator== (const FieldPlanControl&) const = default;
};

struct FieldPlan {
    uint32_t version = kFieldPlanVersion;
    uint64_t revision = 0;        // Document revision the plan was compiled from.
    uint64_t captureRevision = 0; // Capture identity; 0 when none.
    std::array<uint8_t, 16> session {};
    std::string sourceTerrainGuid;
    // Evaluated property/terrain boundary consumed by the unified mesh build.
    // Empty means the document has no authored property line yet.
    std::vector<Vec3> propertyLine;
    uint64_t propertyLineRevision = 0;
    Id propertyLineSource = kNoId;
    std::vector<FieldPlanEntry> entries;
    std::vector<FieldPlanPath> paths;
    std::vector<FieldPlanMask> masks;
    std::vector<FieldPlanControl> controls;
    std::vector<FieldPlanDiagnostic> diagnostics;
    uint64_t hash = 0; // Deterministic content hash; 0 until compiled.
    bool ok = false;   // False: no plan exists; `error` names the refusing stage.
    std::string error;
    bool operator== (const FieldPlan&) const = default;
};

// h' rules (reconciled rule 4): None keeps the composed height, Grade sets the
// surface target t(x,y), Add is raise-only fill max(h,t) and Subtract is
// lower-only cut min(h,t). Absolute targets, never signed offsets.
double ComposeFieldHeight (RegionTerrainEffect effect, double h, double t);

// Pure compiler: no document mutation, no triangulation, no Rust calls. Hard
// refusal (ok=false) only when no plan can exist — a missing snapshot, an
// invalid DAG or a refused stage-1 evaluation; everything else is a located
// diagnostic on an inspectable plan. Reordering the document's object vector
// never changes the plan; changing an explicit root-operation priority always
// changes its hash.
FieldPlan CompileLandscapeFieldPlan (const LandscapeSnapshot& snapshot);

} // namespace tapioca::geometry
#endif
