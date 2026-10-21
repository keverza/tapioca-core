#ifndef TAPIOCA_EDIT_PLANAR_HPP
#define TAPIOCA_EDIT_PLANAR_HPP

// H1 correctness checkpoint accepted for the documented numerical envelope.
// This is an offline topology service, not a native output/write acceptance.
// Planar arrangement of the editor's source curves. The input is immutable:
// BuildPlanarMap reads sources and returns a planar graph with vertices, edges,
// half-edges, bounded faces, holes and per-edge source provenance. It never
// allocates authored identity; PlanarFace::id is a revision-local locator
// derived from the canonical face descriptor, not a SourceId/OperationId.
//
// Arc convention: arcAngles has one signed central angle per input vertex and
// describes the edge leaving that vertex, or zero for a straight edge. This is
// the Archicad polygon convention: a positive angle bulges to the RIGHT of the
// vertex->next direction. Major arcs up to (but not including) a full turn are
// supported analytically; |angle| >= 2*pi is refused rather than guessed.
// Nonzero shallow sweeps are never classified as lines by an angular epsilon.
// Unsupported tessellation precision is refused, not silently straightened.
// Intersection predicates currently require the flattened XY span (including
// numerical weld bounds) on each axis <= sqrt(DBL_MAX)/4; larger inputs refuse
// as Degenerate before unscaled products can overflow.
//
// Provenance: PlanarEdgePiece::piece is a source-local segment locator: the
// input point ordinal at the actual leaving vertex of that segment. It is
// stable for one input revision, but it is a locator, not an authored ID and
// not an enumeration of arrangement output edges. Face descriptors retain the
// sorted unique (source, piece) pairs of their complete boundary.

#include "Edit/EditTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tapioca::geometry {

using geomsrv::edit::Id;
using geomsrv::edit::kNoId;
using geomsrv::edit::Vec3;

struct PlanarCurve {
    Id source = kNoId;
    std::vector<Vec3> points;      // XY used; Z ignored for topology.
    std::vector<double> arcAngles; // Optional, per input vertex, signed radians.
    // Optional Bezier handle OFFSETS in metres, one per input point. Empty
    // arrays mean all zero. A nonzero incoming or outgoing handle makes that
    // segment a cubic; an arc angle and cubic handles on one segment conflict.
    std::vector<Vec3> inHandles;
    std::vector<Vec3> outHandles;
    bool closed = false;
};

struct PlanarOptions {
    // Descriptor lattice spacing in metres. Quantized face cycles use this
    // spacing with quantizeOrigin; it never decides topology or connectivity.
    double snapTolerance = 0.001;
    // Authored gaps are healed only through this option. Separately, topology
    // welds numerical coincidence at max(1e-9 m, 64 ULP of coordinate scale):
    // that physical floor can also join sub-nanometre authored gaps, so it is
    // not a claim that gapTolerance == 0 preserves every smaller gap. A whole
    // source segment that collapses under that weld is refused as Degenerate.
    double gapTolerance = 0.0;
    double chordTolerance = 0.001;   // Metres, maximum sagitta for arc chords.
    double minArea = 1.0e-6;         // Square metres.
    std::size_t maxSegments = 20000; // Input/arrangement segment bound.
    // Work bounds enforced while candidates/events are generated. They are
    // refusals, never truncation, and keep adversarial coincident inputs from
    // materialising quadratic storage before maxSegments is checked.
    std::size_t maxCandidatePairs = 1000000;
    std::size_t maxSplitEvents = 2000000;
    // Safety cap for adaptive cubic subdivision. Hitting it before the chord
    // tolerance is a named WorkLimit refusal, never a silent chord fallback.
    std::size_t maxSubdivisionDepth = 24;
    double quantizeOriginX = 0.0; // Explicit request-local descriptor origin.
    double quantizeOriginY = 0.0;
};

// One flattened source segment's contribution to an arrangement edge. The
// interval is in the source segment's own direction, independent of the
// canonical a->b direction; `reversed` says the two disagree.
struct PlanarEdgePiece {
    Id source = kNoId;
    uint32_t piece = 0;
    double sourceStart = 0.0;
    double sourceEnd = 1.0;
    bool reversed = false;
    bool operator== (const PlanarEdgePiece&) const = default;
};

struct PlanarEdge {
    uint32_t a = 0, b = 0;
    Id source = kNoId; // Front-most/first source for display.
    double sourceStart = 0.0, sourceEnd = 1.0;
    bool reversed = false;
    std::vector<Id> sources; // Sorted unique coincident inputs.
    std::vector<PlanarEdgePiece> pieces;
    bool operator== (const PlanarEdge&) const = default;
};

struct PlanarHalfEdge {
    uint32_t edge = 0;
    uint32_t twin = 0;
    uint32_t next = 0;
    uint32_t origin = 0;
    bool operator== (const PlanarHalfEdge&) const = default;
};

// A walked half-edge cycle. Positive area is a bounded face; negative area is a
// boundary of the unbounded face (a hole candidate). Spur edges are removed
// from the cycle but remain in PlanarMap::edges.
struct PlanarCycle {
    std::vector<uint32_t> halfEdges;
    bool hole = false;
    double area = 0.0;
    bool operator== (const PlanarCycle&) const = default;
};

// A source-local boundary contributor. `piece` is a source segment ordinal,
// not an authored ID and not a generated arrangement-edge index.
struct FaceSourcePiece {
    Id source = kNoId;
    uint32_t piece = 0;
    bool operator== (const FaceSourcePiece&) const = default;
};

struct FaceCycle {
    std::vector<int64_t> x, y; // Quantized XY; first point not repeated.
    bool hole = false;         // false = outer CCW; true = hole CW.
    bool operator== (const FaceCycle&) const = default;
};

struct FaceDescriptor {
    std::vector<FaceSourcePiece> sourcePieces; // Sorted unique.
    FaceCycle outer;                           // Canonical least rotation.
    std::vector<FaceCycle> holes;              // Sorted, canonical.
    bool operator== (const FaceDescriptor&) const = default;
};

struct PlanarFace {
    FaceDescriptor descriptor;
    uint64_t id = 0;                 // Revision-local locator: 1-based sorted descriptor rank.
    std::vector<uint32_t> halfEdges; // Outer cycle, ordered.
    std::vector<uint32_t> holes;     // Indices into PlanarMap::cycles.
    std::vector<Id> sources;         // Sorted union of boundary provenance.
    double area = 0.0;               // Net area, outer minus holes.
    Vec3 seed;                       // Interior point.
};

enum class PlanarDiagnosticCode {
    OpenGap,
    NearCoincident,
    Tangent,
    SelfIntersection,
    Sliver,
    Degenerate,
    TooManySegments,
    WorkLimit,
    Unbounded,
    FaceKeyAmbiguous
};

struct PlanarDiagnostic {
    PlanarDiagnosticCode code = PlanarDiagnosticCode::Degenerate;
    Id sourceA = kNoId, sourceB = kNoId;
    Vec3 at;
    double amount = 0.0;
    std::string message;
    bool operator== (const PlanarDiagnostic&) const = default;
};

struct PlanarMap {
    std::vector<Vec3> vertices;
    std::vector<PlanarEdge> edges;
    std::vector<PlanarHalfEdge> halfEdges;
    std::vector<PlanarCycle> cycles;
    std::vector<PlanarFace> faces;
    std::vector<PlanarDiagnostic> diagnostics;
};

const char* PlanarDiagnosticCodeName (PlanarDiagnosticCode code);

// Lexicographic ordering of canonical cycles and full descriptors. These are
// the ordering used for face locators; they compare content, never a hash or a
// prior numeric id. Equality is exact content equality (FaceDescriptor's
// default operator==).
bool FaceCycleLess (const FaceCycle& a, const FaceCycle& b);
bool FaceDescriptorLess (const FaceDescriptor& a, const FaceDescriptor& b);

// Builds the arrangement. On false, `error` is non-empty and `out` is
// diagnostic context only; callers must not publish it as a successful map.
// FaceKeyAmbiguous deliberately retains the colliding faces so callers can
// inspect the collision, but the bool remains the only success contract.
bool BuildPlanarMap (const std::vector<PlanarCurve>& curves, const PlanarOptions& options, PlanarMap& out,
                     std::string& error);

namespace detail {

// Implementation seams shared by the three translation units. They are not
// authored-identity APIs and are not intended for product consumers.
bool BuildTopology (const std::vector<PlanarCurve>& curves, const PlanarOptions& options, PlanarMap& out,
                    std::string& error);
bool BuildFaces (const PlanarOptions& options, PlanarMap& map, std::string& error);
bool AssignFaceLocators (PlanarMap& map, std::string& error);

} // namespace detail
} // namespace tapioca::geometry

#endif
