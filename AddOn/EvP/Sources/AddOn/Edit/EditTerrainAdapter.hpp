#ifndef TAPIOCA_EDIT_TERRAIN_ADAPTER_HPP
#define TAPIOCA_EDIT_TERRAIN_ADAPTER_HPP

#include "Geometry/GeometryRustTerrain.hpp"
#include <string>

namespace geomsrv::edit {
struct Document;
}

namespace tapioca::geometry {

// Detached capture, not document state. The host stamps it from the immutable
// document used to prepare the worker request; revision zero is valid at ingress.
struct EditTerrainInput {
    TerrainMeshView mesh;
    const ProvenanceRequest& provenance;
    uint64_t revision = 0;
    std::string sourceTerrainGuid;
};
struct EditTerrainOperation {
    OperationId operation = 0;
    TerrainEdit parameters; // Flattened, call-scoped parameters from the authored DAG.
};
struct EditTerrainVolumeOptions {
    double cell = 0.25;
    uint32_t subdivisions = 1;
};
struct EditTerrainRequest {
    const EditTerrainInput& input;
    std::span<const EditTerrainOperation> operations;
    std::span<const TerrainMeshView> surfaces;
    std::span<const double> property; // Closed straight xyz ring, implicit closing edge.
    std::span<const std::span<const double>> protectedRings;
    std::optional<double> smoothing;
    std::span<const double> queriesXY; // Sample the proposed surface, not the baseline.
    double sampleStep = 0.25;
    std::optional<EditTerrainVolumeOptions> volumes; // Omit on drag.
};
struct EditTerrainFaceOwner {
    uint32_t face = 0; // Unchanged baseline face locator, never an authored ID.
    OperationId operation = 0;
    bool operator== (const EditTerrainFaceOwner&) const = default;
};
struct EditTerrainOperationReport {
    OperationId operation = 0;
    TerrainEditReport report;
    bool operator== (const EditTerrainOperationReport&) const = default;
};
// Where an operation left a vertex in place (its report counters, located).
struct EditTerrainClip {
    OperationId operation = 0;
    uint32_t vertex = 0; // Baseline vertex locator, never an authored ID.
    TerrainClipKind kind = TerrainClipKind::Limit;
    double distance = 0, withheld = 0;
    bool operator== (const EditTerrainClip&) const = default;
};
struct EditTerrainOperationVolume {
    OperationId operation = 0;
    double cut = 0, fill = 0, net = 0, area = 0;
};
struct EditTerrainVolumes {
    TerrainVolumes site, context; // Whole terrain; property-exterior remains context.
    std::array<uint32_t, 2> cells {};
    std::vector<EditTerrainOperationVolume> operations;
};
struct EditTerrainResult {
    uint64_t revision = 0;
    std::string sourceTerrainGuid;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    // Baseline attribution is retained, not relabelled as proposed attribution.
    // These owner deltas resolve every transient Rust edit index to its DAG ID.
    ProvenanceRequest inputProvenance;
    std::vector<std::optional<OperationId>> vertexOperations;
    std::vector<EditTerrainFaceOwner> faces;
    std::vector<EditTerrainOperationReport> reports;
    std::vector<EditTerrainClip> clips; // By vertex then operation.
    std::vector<TerrainSample> samples;
    TerrainFieldSummary summary;
    std::optional<EditTerrainVolumes> volumes;
};

// Thin worker-only composition over Geometry/: no terrain math, retained borrow,
// persistence or history. Validates current document framing and ordered enabled
// operation IDs, edits heights, samples, optionally measures, then publishes once.
// All inputs/result may alias; refusal (including exceptions) leaves result
// unchanged. The host must recheck revision AND session before worker publication.
// Property splitting and derivation of grading/protection masks are caller work;
// this adapter forwards already-derived rings, not LandscapeOperation semantics.
bool EvaluateEditTerrain (const geomsrv::edit::Document& doc, const EditTerrainRequest& request,
                          EditTerrainResult& result, std::string& error);

} // namespace tapioca::geometry
#endif
