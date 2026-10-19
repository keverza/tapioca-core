#ifndef TAPIOCA_GEOMETRY_IDENTITY_HPP
#define TAPIOCA_GEOMETRY_IDENTITY_HPP

#include <array>
#include <cstdint>
#include <vector>

namespace tapioca::geometry {

// Authored IDs share the C++ session allocator; zero is absent. These values
// never stand for array indices or native Modeler body/polygon sidecars.
using SourceId = uint64_t;
using OperationId = uint64_t;
using ObjectId = uint64_t;

struct IdentityRange {
    uint64_t offset = 0, count = 0;
    bool operator== (const IdentityRange&) const = default;
};
struct IdentityContext {
    uint32_t schemaVersion = 1;
    std::array<uint8_t, 16> sessionUuid {};
    uint64_t revision = 0, tableId = 0;
    bool operator== (const IdentityContext&) const = default;
};
struct EntityCounts {
    uint64_t vertices = 0, edges = 0, faces = 0, patches = 0;
    bool operator== (const EntityCounts&) const = default;
};
enum class IdentityDomain : uint32_t { Vertex = 1, Edge = 2, Face = 3, Patch = 4 };
enum class MappingKind : uint32_t { Exact = 1, Inherited = 2, Split = 3, Merged = 4, Generated = 5 };
enum class LogicalKind : uint32_t { Source = 1, Operation = 2, Object = 3 };
enum class ProvenanceDiagnostic : uint32_t {
    None = 0,
    ProvenanceUnattributable = 1,
    ProvenanceConflict = 2,
    ProvenanceStale = 3,
    PatchKeyUnstable = 4
};
constexpr uint32_t kMeshProvenanceDomains = 5; // Vertex + face, bit (domain - 1).

struct IdentitySource {
    SourceId id = 0;
    IdentityRange bindings; // Into bindingRefs, whose values index bindings.
    bool operator== (const IdentitySource&) const = default;
};
struct IdentityOperation {
    OperationId id = 0;
    IdentityRange parents; // Into operationIds; ordered unique DAG parents.
    bool operator== (const IdentityOperation&) const = default;
};
struct IdentityObject {
    ObjectId id = 0;
    OperationId operation = 0;
    bool operator== (const IdentityObject&) const = default;
};
struct NativeBinding {
    LogicalKind logicalKind = LogicalKind::Source;
    uint64_t logicalId = 0;
    std::array<uint8_t, 36> nativeGuid {};   // Canonical uppercase dashed non-nil GUID.
    std::array<uint8_t, 16> metadataUuid {}; // Independent metadata UUID, not an editor ID.
    bool metadataPresent = false;
    IdentityRange role; // Nonempty UTF-8 range into roleBytes, no NUL.
    uint64_t revision = 0;
    bool operator== (const NativeBinding&) const = default;
};
struct EntityMapping {
    IdentityDomain domain = IdentityDomain::Vertex;
    MappingKind kind = MappingKind::Generated;
    uint64_t outputIndex = 0;
    IdentityRange sources, operations;
    ObjectId object = 0;
    IdentityRange predecessors;
    uint64_t patchKey = 0;
    bool operator== (const EntityMapping&) const = default;
};
struct EntityPredecessor {
    uint64_t tableId = 0;
    IdentityDomain domain = IdentityDomain::Vertex;
    uint64_t entityIndex = 0;
    bool operator== (const EntityPredecessor&) const = default;
};
struct IdentitySupportTable {
    IdentityContext context;
    EntityCounts counts;
    bool historical = false;
    bool operator== (const IdentitySupportTable&) const = default;
};
struct ProvenanceTable {
    IdentityContext context;
    std::vector<IdentitySource> sources;
    std::vector<IdentityOperation> operations;
    std::vector<IdentityObject> objects;
    std::vector<NativeBinding> bindings;
    std::vector<uint64_t> bindingRefs;
    std::vector<SourceId> sourceIds;
    std::vector<OperationId> operationIds;
    std::vector<EntityPredecessor> predecessors;
    std::vector<EntityMapping> mappings;
    std::vector<uint8_t> roleBytes;
    std::vector<IdentitySupportTable> supportTables;
    bool operator== (const ProvenanceTable&) const = default;
};
struct ProvenanceRequest {
    // Authoritative current session/revision, NOT inferred from the input table.
    std::array<uint8_t, 16> expectedSession {};
    uint64_t expectedRevision = 0;
    EntityCounts counts;
    uint32_t domains = kMeshProvenanceDomains;
    bool requireObject = false;
    ProvenanceTable table;
    bool operator== (const ProvenanceRequest&) const = default;
};

// Structural validation does not allocate identity or prove geometric origin,
// persisted patch matching, or historical monotonic allocation. C++ retains the
// DAG/native bindings and resolves transient support mappings before release.

} // namespace tapioca::geometry
#endif
