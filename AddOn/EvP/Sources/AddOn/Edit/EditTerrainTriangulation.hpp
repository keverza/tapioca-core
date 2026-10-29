#ifndef TAPIOCA_EDIT_TERRAIN_TRIANGULATION_HPP
#define TAPIOCA_EDIT_TERRAIN_TRIANGULATION_HPP

#include "Edit/EditTerrainAdapter.hpp"
#include "Edit/EditTypes.hpp"

namespace tapioca::geometry {

struct EditTerrainSplitOptions {
    OperationId operation = 0;
    ObjectId object = 0; // One existing terrain owner; mixed ownership refuses.
    // Caller-owned transient table tokens, NOT authored IDs. Both must be new
    // within this request's retained support set and distinct from each other.
    uint64_t splitTable = 0, meshTable = 0;
};
struct EditTerrainPropertyEdge {
    uint32_t a = 0, b = 0, segment = 0; // Segment is a locator in propertySegments.
    bool operator== (const EditTerrainPropertyEdge&) const = default;
};
struct EditTerrainSplitResult {
    uint64_t revision = 0;
    std::string sourceTerrainGuid;
    TerrainResult split;                  // Complete Rust supports, including all constraint edges.
    ProvenanceRequest baselineProvenance; // Retained input table behind split supports.
    ProvenanceRequest meshProvenance;     // Vertex/face projection for field/edit services (domains=5).
    std::vector<uint32_t> baselineTriangles, parents;
    std::vector<uint8_t> inside;
    std::vector<geomsrv::edit::Id> propertyVertices, propertySegments; // Authored document IDs, not array ordinals.
    std::vector<EditTerrainPropertyEdge> propertyEdges;
    size_t derivedRingVertices = 0; // Transient derived ring; never authored vertex/segment IDs.
    bool operator== (const EditTerrainSplitResult&) const = default;
};
struct EditTerrainPreservation {
    size_t outsideFaces = 0, fixedVertices = 0, propertyEdges = 0;
    bool operator== (const EditTerrainPreservation&) const = default;
};

// Worker-only split of the current document's straight property line, reached
// exclusively through Geometry::SplitTerrainMesh. No C++ intersection/TIN math.
// Remaps the kernel's explicit supports into a validated field-ready provenance
// table, retaining the full split input/edge supports and native catalogs.
// This increment requires one nonzero existing terrain owner, not an invented
// owner or downgraded requireObject flag. Inputs/result may alias; atomic refusal.
bool SplitEditTerrain (const geomsrv::edit::Document& doc, const EditTerrainInput& input,
                       const EditTerrainSplitOptions& options, EditTerrainSplitResult& result, std::string& error);
// Derived footprint/transition/mask preparation. Caller derives the ring from
// the current DAG; every contributor is retained, without inventing authored IDs.
bool SplitEditTerrainRing (const geomsrv::edit::Document& doc, const EditTerrainInput& input,
                           const EditTerrainSplitOptions& options, std::span<const geomsrv::edit::Vec3> ring,
                           std::span<const SourceId> sources, EditTerrainSplitResult& result, std::string& error);

// Checks the proposed surface against the immutable split baseline. Plan/index
// topology is unchanged everywhere; outside-face and property-edge vertices are
// bit-identical in xyz. Together with Rust's split contract this preserves the
// outside piecewise-linear surface, not just a few sample heights. No interpolation
// is implemented here. The report is unchanged on refusal.
bool VerifyEditTerrainOutside (const EditTerrainSplitResult& baseline, const EditTerrainResult& proposed,
                               EditTerrainPreservation& report, std::string& error);

} // namespace tapioca::geometry
#endif
