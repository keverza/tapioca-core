#ifndef TAPIOCA_EDIT_TERRAIN_ADAPTER_INTERNAL_HPP
#define TAPIOCA_EDIT_TERRAIN_ADAPTER_INTERNAL_HPP

#include "Edit/EditTerrainAdapter.hpp"

namespace tapioca::geometry::terrain_detail {

// Shared editor framing/identity gates for the terrain and sparse-solve adapters.
// Structural geometry only; neither validates a particular surface topology.
bool ValidateInput (const geomsrv::edit::Document& doc, const EditTerrainInput& input, std::string& error);
bool ValidateOperations (const geomsrv::edit::Document& doc, const ProvenanceRequest& provenance,
                         std::span<const OperationId> operations, std::string& error);

} // namespace tapioca::geometry::terrain_detail
#endif
