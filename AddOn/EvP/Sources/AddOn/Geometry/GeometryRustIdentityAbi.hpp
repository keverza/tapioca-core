#ifndef TAPIOCA_GEOMETRY_RUST_IDENTITY_ABI_HPP
#define TAPIOCA_GEOMETRY_RUST_IDENTITY_ABI_HPP

#include "GeometryIdentity.hpp"
#include "GeometryRustFfi.h"
#include <vector>

namespace tapioca::geometry::detail {
// Internal call-scoped explicit POD converter, shared by attributed services.
// Never copy/move this storage after preparing views into its owned arrays.
struct AbiProvenanceStorage {
    TapiocaIdentityContext context {};
    std::vector<TapiocaSourceRecord> sources;
    std::vector<TapiocaOperationRecord> operations;
    std::vector<TapiocaObjectRecord> objects;
    std::vector<TapiocaBindingRecord> bindings;
    std::vector<uint64_t> bindingRefs, sourceIds, operationIds;
    std::vector<TapiocaPredecessorRecord> predecessors;
    std::vector<TapiocaMappingRecord> mappings;
    std::vector<uint8_t> roleBytes;
    std::vector<TapiocaSupportTable> supportTables;
    TapiocaProvenanceView view {};
};
TapiocaProvenanceRequest ConvertProvenanceRequest (const ProvenanceRequest& request, AbiProvenanceStorage& storage);
} // namespace tapioca::geometry::detail

#endif
