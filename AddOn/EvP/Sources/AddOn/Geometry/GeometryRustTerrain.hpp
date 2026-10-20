#ifndef TAPIOCA_GEOMETRY_RUST_TERRAIN_HPP
#define TAPIOCA_GEOMETRY_RUST_TERRAIN_HPP

#include <cstdint>
#include <string>
#include <span>
#include <vector>
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
// This does not certify geometric contributor truth or D14 algorithms, or
// implement terrain math. Complete detached value/catalog validation is a
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

} // namespace tapioca::geometry

#endif
