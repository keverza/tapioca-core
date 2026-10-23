#ifndef TAPIOCA_EDIT_SPARSE_SOLVE_HPP
#define TAPIOCA_EDIT_SPARSE_SOLVE_HPP

#include "Edit/EditTerrainAdapter.hpp"

namespace tapioca::geometry {

struct EditSparseSolveRequest {
    const EditTerrainInput& input;
    OperationId operation = 0;
    // Unknown i is the height (world metres) of this baseline vertex. Unique,
    // in-range locators; never authored IDs or a second unknown allocator.
    std::span<const uint32_t> vertices;
    const GradingRequest& system;
};
struct EditSparseSolveResult {
    uint64_t revision = 0;
    std::string sourceTerrainGuid;
    OperationId operation = 0;
    std::vector<uint32_t> vertices; // Preserves the caller's unknown order.
    GradingResult solution;
    ProvenanceRequest inputProvenance; // Baseline ancestry, not fabricated solved attribution.
};

// Worker-only, pure adapter: structural geometry/framing and enabled operation
// validation, explicit unknown->vertex mapping, early contradictory-fixed-value
// refusal, then Geometry::SolveGrading. Rust owns assembly, bounds and KKT
// certification. No C++ solver/residual math, mesh mutation or history entry.
// Result is unchanged on any refusal; inputs may borrow its previous arrays.
// Enabled authored fixed controls refuse with TerrainControlTierUnsupported
// until the request can carry their prepared support; they are never ignored.
// A caller applying heights must also enforce its property/locked constraints
// and recheck the retained session/revision before publication.
bool SolveEditTerrain (const geomsrv::edit::Document& doc, const EditSparseSolveRequest& request,
                       EditSparseSolveResult& result, std::string& error);

} // namespace tapioca::geometry
#endif
