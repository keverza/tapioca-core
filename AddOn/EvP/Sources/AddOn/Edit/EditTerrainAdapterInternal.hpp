#ifndef TAPIOCA_EDIT_TERRAIN_ADAPTER_INTERNAL_HPP
#define TAPIOCA_EDIT_TERRAIN_ADAPTER_INTERNAL_HPP

#include "Edit/EditTerrainAdapter.hpp"
#include <set>
#include <span>
#include <vector>

namespace tapioca::geometry::terrain_detail {

// Shared editor framing/identity gates for the terrain and sparse-solve adapters.
// Structural geometry only; neither validates a particular surface topology.
bool ValidateInput (const geomsrv::edit::Document& doc, const EditTerrainInput& input, std::string& error);
bool ValidateOperations (const geomsrv::edit::Document& doc, const ProvenanceRequest& provenance,
                         std::span<const OperationId> operations, std::string& error);
// Until soft/solve requests carry prepared fixed support, fail closed instead of
// silently dropping the document's enabled height constraints. Not a positional
// mask and not a claim that arbitrary controls have been inserted into this mesh.
bool ValidateUnconstrainedTerrainTier (const geomsrv::edit::Document& doc, std::string& error);

// Provenance remapping shared by the split and design adapters
// (EditTerrainTriangulation.cpp). Rows are copied, never guessed: an exact row
// keeps its parent's attribution with one predecessor in `table`; a fragment
// unions its parents' sources/operations with the producing guide/operation.
bool Validate (const ProvenanceRequest& provenance, std::string& error);
IdentityRange Append (std::vector<uint64_t>& pool, const std::set<uint64_t>& values);
EntityMapping Supported (const EntityMapping& input, IdentityDomain domain, uint64_t index, uint64_t table,
                         ProvenanceTable& output);
EntityMapping Fragment (const ProvenanceRequest& input, std::span<const EntityMapping* const> parents,
                        IdentityDomain domain, uint64_t index, OperationId operation, SourceId guide,
                        std::span<const EntityPredecessor> predecessors, ProvenanceRequest& output);
// Both tokens are nonzero, distinct and new to the input's retained support set.
bool NewTables (const ProvenanceRequest& input, uint64_t first, uint64_t second);
// The one existing owner every input row names, or null.
const IdentityObject* UniformOwner (const ProvenanceRequest& input, ObjectId object);
// The operation's existing inputs match its catalog parents in order. The
// operation must already have passed ValidateOperations.
bool CurrentParents (const geomsrv::edit::Document& doc, const ProvenanceRequest& input, OperationId operation);
// A document landscape source present in the catalog, with its native binding
// when it has a native link. `what` names it in the diagnostic.
bool CatalogSource (const geomsrv::edit::Document& doc, const ProvenanceRequest& input, SourceId id, const char* what,
                    std::string& error);

struct InputRows {
    std::vector<const EntityMapping*> vertices, edges, faces;
    explicit InputRows (const ProvenanceRequest& input)
        : vertices (input.counts.vertices), edges (input.counts.edges), faces (input.counts.faces)
    {
        for (const auto& row : input.table.mappings) {
            if (row.domain == IdentityDomain::Vertex)
                vertices.at (row.outputIndex) = &row;
            if (row.domain == IdentityDomain::Face)
                faces.at (row.outputIndex) = &row;
            if (row.domain == IdentityDomain::Edge)
                edges.at (row.outputIndex) = &row;
        }
    }
};

} // namespace tapioca::geometry::terrain_detail
#endif
