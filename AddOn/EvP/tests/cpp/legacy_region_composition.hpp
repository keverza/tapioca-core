#ifndef TAPIOCA_TEST_LEGACY_REGION_COMPOSITION_HPP
#define TAPIOCA_TEST_LEGACY_REGION_COMPOSITION_HPP

// Test-only reconstruction of the production region-first/path-second
// composition removed in #112 P4b: the attributed region evaluation becomes
// the proposed capture the unchanged legacy path lane consumes. It is the
// parity oracle for the unified region/mixed lanes, never a production path —
// production routes every scene through the unified authority since P4c.

#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include "Edit/EditTerrainCapture.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace legacycomposition {
namespace lg = tapioca::geometry;

// The former ProposedCapture: the region evaluation's terrain, re-emitted
// through a fresh support table so the path lane can consume it with the
// region operations attributed as inherited moves.
inline std::shared_ptr<lg::TerrainCapture> ComposedCapture (const lg::RegionTerrainResult& regions, std::string& error)
{
    const auto& edited = regions.evaluation.terrain;
    auto out = std::make_shared<lg::TerrainCapture> ();
    out->revision = edited.revision;
    out->sourceTerrainGuid = edited.sourceTerrainGuid;
    out->points = edited.points;
    out->triangles = edited.triangles;
    out->provenance = edited.inputProvenance;
    auto& provenance = out->provenance;
    const auto inputTable = provenance.table.context;
    uint64_t token = 10000;
    while (token == inputTable.tableId ||
           std::any_of (provenance.table.supportTables.begin (), provenance.table.supportTables.end (),
                        [=] (const auto& table) { return table.context.tableId == token; }))
        ++token;
    provenance.table.context.tableId = token;
    provenance.table.supportTables.push_back ({ inputTable, provenance.counts, false });
    const auto rows = provenance.table.mappings;
    std::map<uint32_t, std::set<uint64_t>> faceOperations;
    for (const auto& face : edited.faces)
        faceOperations[face.face].insert (face.operation);
    provenance.table.mappings.clear ();
    for (const auto& input : rows) {
        auto row = lg::terrain_detail::Supported (input, input.domain, input.outputIndex, inputTable.tableId,
                                                  provenance.table);
        std::set<uint64_t> operations;
        for (uint64_t i = 0; i < input.operations.count; ++i)
            operations.insert (edited.inputProvenance.table.operationIds.at (input.operations.offset + i));
        bool moved = false;
        if (input.domain == lg::IdentityDomain::Vertex && edited.vertexOperations.at (input.outputIndex)) {
            operations.insert (*edited.vertexOperations.at (input.outputIndex));
            moved = true;
        }
        if (input.domain == lg::IdentityDomain::Face) {
            const auto found = faceOperations.find (static_cast<uint32_t> (input.outputIndex));
            if (found != faceOperations.end ()) {
                operations.insert (found->second.begin (), found->second.end ());
                moved = true;
            }
        }
        if (moved) {
            row.kind = lg::MappingKind::Inherited;
            row.operations = lg::terrain_detail::Append (provenance.table.operationIds, operations);
        }
        provenance.table.mappings.push_back (row);
    }
    if (!lg::terrain_detail::Validate (provenance, error))
        return {};
    return out;
}
} // namespace legacycomposition

#endif
