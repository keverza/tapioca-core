#include "Geometry/GeometryRustTerrain.hpp"

#include "GeometryRustFfi.h"
#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using geomsrv::Mesh;
using namespace tapioca::geometry;

constexpr std::array<uint8_t, 16> kSessionUuid { 1 };
constexpr std::array<uint8_t, 16> kOtherSessionUuid { 2 };

std::array<uint8_t, 36> GuidBytes (std::string_view text)
{
    std::array<uint8_t, 36> result {};
    const size_t count = std::min (text.size (), result.size ());
    for (size_t index = 0; index < count; ++index)
        result[index] = static_cast<uint8_t> (text[index]);
    return result;
}

std::array<uint8_t, 16> MetadataBytes ()
{
    return { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
}

Mesh BuildMesh ()
{
    Mesh mesh = evptest::MakeSingleTriangle ("identity-mesh");
    mesh.normals = { 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F };
    mesh.triMaterial = { 7 };
    mesh.triWireEdges = { 0x05 };
    mesh.triSourceBody = { 1 };
    mesh.triSourcePgon = { 2 };
    mesh.triSurfaceAttribute = { 42 };
    return mesh;
}

NativeBinding MakeObjectBinding (std::string_view guid, const std::array<uint8_t, 16>& metadata, bool present)
{
    NativeBinding binding;
    binding.logicalKind = LogicalKind::Object;
    binding.logicalId = 13;
    binding.nativeGuid = GuidBytes (guid);
    binding.metadataUuid = metadata;
    binding.metadataPresent = present;
    binding.role = IdentityRange { 0, 6 };
    binding.revision = 0;
    return binding;
}

EntityMapping MakeGeneratedRow (IdentityDomain domain, uint64_t outputIndex)
{
    EntityMapping mapping;
    mapping.domain = domain;
    mapping.kind = MappingKind::Generated;
    mapping.outputIndex = outputIndex;
    mapping.sources = IdentityRange { 0, 1 };
    mapping.operations = IdentityRange { 0, 1 };
    mapping.object = 13;
    mapping.predecessors = IdentityRange { 0, 0 };
    mapping.patchKey = 0;
    return mapping;
}

ProvenanceRequest MakeValidRequest ()
{
    ProvenanceRequest request;
    request.expectedSession = kSessionUuid;
    request.expectedRevision = 0;
    request.counts = EntityCounts { 3, 0, 1, 0 };
    request.domains = kMeshProvenanceDomains;
    request.requireObject = true;

    ProvenanceTable& table = request.table;
    table.context = IdentityContext { 1, kSessionUuid, 0, 100 };
    table.sources.push_back (IdentitySource { 11, IdentityRange { 0, 0 } });
    table.operations.push_back (IdentityOperation { 12, IdentityRange { 0, 0 } });
    table.objects.push_back (IdentityObject { 13, 12 });
    table.bindings.push_back (MakeObjectBinding ("12345678-ABCD-EF01-2345-6789ABCDEF01", MetadataBytes (), true));
    table.bindings.push_back (MakeObjectBinding ("87654321-FEDC-BA98-7654-3210FEDCBA98", {}, false));
    table.sourceIds = { 11 };
    table.operationIds = { 12 };
    table.roleBytes = { 'o', 'u', 't', 'p', 'u', 't' };
    table.mappings = { MakeGeneratedRow (IdentityDomain::Vertex, 0), MakeGeneratedRow (IdentityDomain::Vertex, 1),
                       MakeGeneratedRow (IdentityDomain::Vertex, 2), MakeGeneratedRow (IdentityDomain::Face, 0) };
    return request;
}

void ExpectMeshUnchanged (const Mesh& before, const Mesh& after)
{
    EXPECT_EQ (before.guid, after.guid);
    EXPECT_EQ (before.elemType, after.elemType);
    EXPECT_EQ (before.vertices, after.vertices);
    EXPECT_EQ (before.normals, after.normals);
    EXPECT_EQ (before.triangles, after.triangles);
    EXPECT_EQ (before.triMaterial, after.triMaterial);
    EXPECT_EQ (before.triWireEdges, after.triWireEdges);
    EXPECT_EQ (before.triSourceBody, after.triSourceBody);
    EXPECT_EQ (before.triSourcePgon, after.triSourcePgon);
    EXPECT_EQ (before.triSurfaceAttribute, after.triSurfaceAttribute);
    for (size_t axis = 0; axis < 3; ++axis) {
        EXPECT_EQ (before.bounds.mn[axis], after.bounds.mn[axis]);
        EXPECT_EQ (before.bounds.mx[axis], after.bounds.mx[axis]);
    }
}

void ExpectRequestUnchanged (const ProvenanceRequest& before, const ProvenanceRequest& after)
{
    EXPECT_EQ (before.expectedSession, after.expectedSession);
    EXPECT_EQ (before.expectedRevision, after.expectedRevision);
    EXPECT_EQ (before.counts, after.counts);
    EXPECT_EQ (before.domains, after.domains);
    EXPECT_EQ (before.requireObject, after.requireObject);
    EXPECT_EQ (before.table, after.table);
}

void ExpectFailure (const ProvenanceRequest& request, ProvenanceDiagnostic expected, std::string_view prefix)
{
    const ProvenanceRequest before = request;
    ProvenanceDiagnostic diagnostic = ProvenanceDiagnostic::None;
    std::string error;
    EXPECT_FALSE (ValidateProvenance (request, diagnostic, error));
    EXPECT_EQ (expected, diagnostic);
    EXPECT_FALSE (error.empty ());
    EXPECT_EQ (0U, error.rfind (prefix, 0)) << error;
    ExpectRequestUnchanged (before, request);
}

// POD copies for the raw ABI tests; the C++ table remains the source.
struct RawProvenance {
    TapiocaIdentityContext context {};
    std::vector<TapiocaSourceRecord> sources;
    std::vector<TapiocaOperationRecord> operations;
    std::vector<TapiocaObjectRecord> objects;
    std::vector<TapiocaBindingRecord> bindings;
    std::vector<uint64_t> bindingRefs;
    std::vector<uint64_t> sourceIds;
    std::vector<uint64_t> operationIds;
    std::vector<TapiocaPredecessorRecord> predecessors;
    std::vector<TapiocaMappingRecord> mappings;
    std::vector<uint8_t> roleBytes;
    std::vector<TapiocaSupportTable> supportTables;
    TapiocaProvenanceRequest request {};
};

TapiocaIdentityRange AbiRange (const IdentityRange& range)
{
    return TapiocaIdentityRange { range.offset, range.count };
}

void BuildRaw (const ProvenanceRequest& source, RawProvenance& raw)
{
    raw.context.schemaVersion = source.table.context.schemaVersion;
    std::copy (source.table.context.sessionUuid.begin (), source.table.context.sessionUuid.end (),
               raw.context.sessionUuid);
    raw.context.revision = source.table.context.revision;
    raw.context.tableId = source.table.context.tableId;

    for (const IdentitySource& item : source.table.sources)
        raw.sources.push_back (TapiocaSourceRecord { item.id, AbiRange (item.bindings) });
    for (const IdentityOperation& item : source.table.operations)
        raw.operations.push_back (TapiocaOperationRecord { item.id, AbiRange (item.parents) });
    for (const IdentityObject& item : source.table.objects)
        raw.objects.push_back (TapiocaObjectRecord { item.id, item.operation });
    for (const NativeBinding& item : source.table.bindings) {
        TapiocaBindingRecord record {};
        record.logicalKind = static_cast<uint32_t> (item.logicalKind);
        record.logicalId = item.logicalId;
        std::copy (item.nativeGuid.begin (), item.nativeGuid.end (), record.nativeGuid);
        std::copy (item.metadataUuid.begin (), item.metadataUuid.end (), record.metadataUuid);
        record.metadataPresent = item.metadataPresent ? 1U : 0U;
        record.role = AbiRange (item.role);
        record.revision = item.revision;
        raw.bindings.push_back (record);
    }
    raw.bindingRefs = source.table.bindingRefs;
    raw.sourceIds = source.table.sourceIds;
    raw.operationIds = source.table.operationIds;
    for (const EntityPredecessor& item : source.table.predecessors)
        raw.predecessors.push_back (
            TapiocaPredecessorRecord { item.tableId, static_cast<uint32_t> (item.domain), item.entityIndex });
    for (const EntityMapping& item : source.table.mappings) {
        TapiocaMappingRecord record {};
        record.domain = static_cast<uint32_t> (item.domain);
        record.kind = static_cast<uint32_t> (item.kind);
        record.outputIndex = item.outputIndex;
        record.sources = AbiRange (item.sources);
        record.operations = AbiRange (item.operations);
        record.object = item.object;
        record.predecessors = AbiRange (item.predecessors);
        record.patchKey = item.patchKey;
        raw.mappings.push_back (record);
    }
    raw.roleBytes = source.table.roleBytes;
    for (const IdentitySupportTable& item : source.table.supportTables) {
        TapiocaSupportTable record {};
        record.context.schemaVersion = item.context.schemaVersion;
        std::copy (item.context.sessionUuid.begin (), item.context.sessionUuid.end (), record.context.sessionUuid);
        record.context.revision = item.context.revision;
        record.context.tableId = item.context.tableId;
        record.counts =
            TapiocaEntityCounts { item.counts.vertices, item.counts.edges, item.counts.faces, item.counts.patches };
        record.historical = item.historical ? 1U : 0U;
        raw.supportTables.push_back (record);
    }

    TapiocaProvenanceView view {};
    view.context = raw.context;
    view.sources = TapiocaSourceArray { raw.sources.data (), raw.sources.size () };
    view.operations = TapiocaOperationArray { raw.operations.data (), raw.operations.size () };
    view.objects = TapiocaObjectArray { raw.objects.data (), raw.objects.size () };
    view.bindings = TapiocaBindingArray { raw.bindings.data (), raw.bindings.size () };
    view.bindingRefs = TapiocaIdArray { raw.bindingRefs.data (), raw.bindingRefs.size () };
    view.sourceIds = TapiocaIdArray { raw.sourceIds.data (), raw.sourceIds.size () };
    view.operationIds = TapiocaIdArray { raw.operationIds.data (), raw.operationIds.size () };
    view.predecessors = TapiocaPredecessorArray { raw.predecessors.data (), raw.predecessors.size () };
    view.mappings = TapiocaMappingArray { raw.mappings.data (), raw.mappings.size () };
    view.roleBytes = TapiocaByteView { raw.roleBytes.data (), raw.roleBytes.size () };
    view.supportTables = TapiocaSupportArray { raw.supportTables.data (), raw.supportTables.size () };

    raw.request = TapiocaProvenanceRequest {};
    std::copy (source.expectedSession.begin (), source.expectedSession.end (), raw.request.expectedSession);
    raw.request.expectedRevision = source.expectedRevision;
    raw.request.counts =
        TapiocaEntityCounts { source.counts.vertices, source.counts.edges, source.counts.faces, source.counts.patches };
    raw.request.domains = source.domains;
    raw.request.requireObject = source.requireObject ? 1U : 0U;
    raw.request.table = view;
}

// Builds the Explicit/Inherited predecessor support fixture used by the
// historical-table and exact-kind tests.
ProvenanceRequest MakeHistoricalExactRequest ()
{
    ProvenanceRequest request = MakeValidRequest ();
    request.expectedRevision = 3;
    request.table.context.revision = 3;

    request.table.supportTables.push_back (
        IdentitySupportTable { IdentityContext { 1, kSessionUuid, 2, 200 }, EntityCounts { 3, 0, 1, 0 }, true });
    request.table.supportTables.push_back (
        IdentitySupportTable { IdentityContext { 1, kSessionUuid, 3, 201 }, EntityCounts { 3, 0, 1, 0 }, false });
    request.table.predecessors.push_back (EntityPredecessor { 200, IdentityDomain::Vertex, 0 });
    request.table.predecessors.push_back (EntityPredecessor { 201, IdentityDomain::Vertex, 1 });
    request.table.mappings[0].kind = MappingKind::Exact;
    request.table.mappings[0].predecessors = IdentityRange { 0, 1 };
    request.table.mappings[1].kind = MappingKind::Exact;
    request.table.mappings[1].predecessors = IdentityRange { 1, 1 };
    return request;
}

} // namespace

TEST (GeometryRustIdentity, ValidAttributionRoundTripsUnchanged)
{
    Mesh mesh = BuildMesh ();
    ProvenanceRequest request = MakeValidRequest ();
    const Mesh meshBefore = mesh;
    const ProvenanceRequest requestBefore = request;

    ProvenanceDiagnostic diagnostic = ProvenanceDiagnostic::ProvenanceConflict;
    std::string error = "stale";
    ASSERT_TRUE (ValidateProvenance (request, diagnostic, error)) << error;
    EXPECT_EQ (ProvenanceDiagnostic::None, diagnostic);
    EXPECT_TRUE (error.empty ());

    ASSERT_TRUE (ValidateAttributedMesh (mesh, request, diagnostic, error)) << error;
    EXPECT_EQ (ProvenanceDiagnostic::None, diagnostic);
    EXPECT_TRUE (error.empty ());

    // Borrowed C++ -> Rust -> C++ validation round trip; no Rust output is claimed.
    ExpectMeshUnchanged (meshBefore, mesh);
    ExpectRequestUnchanged (requestBefore, request);
}

TEST (GeometryRustIdentity, MissingMappingRowIsUnattributable)
{
    ProvenanceRequest request = MakeValidRequest ();
    request.table.mappings.erase (request.table.mappings.begin () + 2);
    ExpectFailure (request, ProvenanceDiagnostic::ProvenanceUnattributable, "ProvenanceUnattributable");
}

TEST (GeometryRustIdentity, DuplicateOutputIndexIsConflict)
{
    ProvenanceRequest request = MakeValidRequest ();
    request.table.mappings[1].outputIndex = request.table.mappings[0].outputIndex;
    ExpectFailure (request, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");
}

TEST (GeometryRustIdentity, MissingSourceOperationAndObjectAreConflict)
{
    ProvenanceRequest missingSource = MakeValidRequest ();
    missingSource.table.sourceIds[0] = 99;
    ExpectFailure (missingSource, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest missingOperation = MakeValidRequest ();
    missingOperation.table.operationIds[0] = 99;
    ExpectFailure (missingOperation, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest missingObject = MakeValidRequest ();
    missingObject.table.objects.clear ();
    ExpectFailure (missingObject, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");
}

TEST (GeometryRustIdentity, CrossSessionAndStaleRevisionAreRejected)
{
    ProvenanceRequest crossSession = MakeValidRequest ();
    crossSession.expectedSession = kOtherSessionUuid;
    ExpectFailure (crossSession, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest stale = MakeValidRequest ();
    stale.expectedRevision = 1;
    ExpectFailure (stale, ProvenanceDiagnostic::ProvenanceStale, "ProvenanceStale");
}

TEST (GeometryRustIdentity, OperationCycleAndRangeOverflowAreConflict)
{
    ProvenanceRequest cycle = MakeValidRequest ();
    cycle.table.operations = { IdentityOperation { 12, IdentityRange { 1, 1 } },
                               IdentityOperation { 14, IdentityRange { 0, 1 } } };
    cycle.table.operationIds = { 12, 14 };
    ExpectFailure (cycle, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest overflow = MakeValidRequest ();
    overflow.table.mappings[0].sources = IdentityRange { 0, 2 };
    ExpectFailure (overflow, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");
}

TEST (GeometryRustIdentity, MalformedGuidAndMetadataAreRejected)
{
    ProvenanceRequest lowercase = MakeValidRequest ();
    lowercase.table.bindings[0].nativeGuid[0] = static_cast<uint8_t> ('a');
    ExpectFailure (lowercase, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest nil = MakeValidRequest ();
    nil.table.bindings[0].nativeGuid.fill (0);
    ExpectFailure (nil, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest missingMetadata = MakeValidRequest ();
    missingMetadata.table.bindings[0].metadataPresent = false;
    ExpectFailure (missingMetadata, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest absentButPresent = MakeValidRequest ();
    absentButPresent.table.bindings[1].metadataUuid = MetadataBytes ();
    ExpectFailure (absentButPresent, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest emptyRole = MakeValidRequest ();
    emptyRole.table.bindings[0].role = IdentityRange { 0, 0 };
    ExpectFailure (emptyRole, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");
}

TEST (GeometryRustIdentity, UnknownKindAndDomainAreConflict)
{
    ProvenanceRequest unknownKind = MakeValidRequest ();
    unknownKind.table.mappings[0].kind = static_cast<MappingKind> (99);
    ExpectFailure (unknownKind, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest unknownDomain = MakeValidRequest ();
    unknownDomain.table.mappings[0].domain = static_cast<IdentityDomain> (99);
    ExpectFailure (unknownDomain, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest unknownBindingKind = MakeValidRequest ();
    unknownBindingKind.table.bindings[0].logicalKind = static_cast<LogicalKind> (99);
    ExpectFailure (unknownBindingKind, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");
}

TEST (GeometryRustIdentity, ChangedExpectedCountsAreRejected)
{
    ProvenanceRequest tooManyVertices = MakeValidRequest ();
    tooManyVertices.counts.vertices = 4;
    ExpectFailure (tooManyVertices, ProvenanceDiagnostic::ProvenanceUnattributable, "ProvenanceUnattributable");

    ProvenanceRequest undeclaredEdges = MakeValidRequest ();
    undeclaredEdges.counts.edges = 1;
    ExpectFailure (undeclaredEdges, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    Mesh mesh = BuildMesh ();
    const Mesh meshBefore = mesh;
    ProvenanceRequest attributed = MakeValidRequest ();
    attributed.counts.vertices = 4;
    const ProvenanceRequest requestBefore = attributed;
    ProvenanceDiagnostic diagnostic = ProvenanceDiagnostic::ProvenanceConflict;
    std::string error;
    EXPECT_FALSE (ValidateAttributedMesh (mesh, attributed, diagnostic, error));
    EXPECT_EQ (ProvenanceDiagnostic::ProvenanceUnattributable, diagnostic);
    EXPECT_EQ (0U, error.rfind ("ProvenanceUnattributable", 0)) << error;
    ExpectMeshUnchanged (meshBefore, mesh);
    ExpectRequestUnchanged (requestBefore, attributed);
}

TEST (GeometryRustIdentity, AttributedMeshRequiresVertexAndFaceDomainsOnly)
{
    Mesh mesh = BuildMesh ();
    std::string error;
    for (uint32_t domains : { 0U, 0x10U, 0b1111U }) {
        auto request = MakeValidRequest ();
        request.domains = domains;
        auto diagnostic = ProvenanceDiagnostic::None;
        EXPECT_FALSE (ValidateAttributedMesh (mesh, request, diagnostic, error));
        EXPECT_EQ (ProvenanceDiagnostic::ProvenanceConflict, diagnostic);
    }
    for (bool edge : { false, true }) {
        auto request = MakeValidRequest ();
        if (edge)
            request.counts.edges = 1;
        else
            request.counts.patches = 1;
        auto diagnostic = ProvenanceDiagnostic::None;
        EXPECT_FALSE (ValidateAttributedMesh (mesh, request, diagnostic, error));
        EXPECT_EQ (ProvenanceDiagnostic::ProvenanceConflict, diagnostic);
    }
}

TEST (GeometryRustIdentity, ObjectOmissionRequiresExplicitSourceOnlyOptOut)
{
    EXPECT_TRUE (ProvenanceRequest {}.requireObject);
    const auto mesh = BuildMesh ();
    auto request = MakeValidRequest ();
    for (auto& mapping : request.table.mappings)
        mapping.object = 0;
    auto diagnostic = ProvenanceDiagnostic::None;
    std::string error;
    EXPECT_FALSE (ValidateAttributedMesh (mesh, request, diagnostic, error));
    EXPECT_EQ (ProvenanceDiagnostic::ProvenanceUnattributable, diagnostic);
    request.requireObject = false; // Explicit source-only geometry is supported, not object output.
    EXPECT_TRUE (ValidateAttributedMesh (mesh, request, diagnostic, error)) << error;
}

TEST (GeometryRustIdentity, HistoricalSupportAndExactPredecessorAreAccepted)
{
    ProvenanceRequest request = MakeHistoricalExactRequest ();
    const ProvenanceRequest requestBefore = request;

    ProvenanceDiagnostic diagnostic = ProvenanceDiagnostic::ProvenanceConflict;
    std::string error;
    ASSERT_TRUE (ValidateProvenance (request, diagnostic, error)) << error;
    EXPECT_EQ (ProvenanceDiagnostic::None, diagnostic);
    ExpectRequestUnchanged (requestBefore, request);

    // A non-exact row still needs at least one predecessor; generated rows do not.
    ProvenanceRequest inherited = MakeValidRequest ();
    inherited.table.supportTables.push_back (
        IdentitySupportTable { IdentityContext { 1, kSessionUuid, 0, 300 }, EntityCounts { 3, 0, 1, 0 }, false });
    inherited.table.predecessors.push_back (EntityPredecessor { 300, IdentityDomain::Vertex, 0 });
    inherited.table.mappings[0].kind = MappingKind::Inherited;
    inherited.table.mappings[0].predecessors = IdentityRange { 0, 1 };
    ASSERT_TRUE (ValidateProvenance (inherited, diagnostic, error)) << error;
    EXPECT_EQ (ProvenanceDiagnostic::None, diagnostic);
}

TEST (GeometryRustIdentity, BadPredecessorBoundsAndHistoricalFutureAreRejected)
{
    ProvenanceRequest outOfBounds = MakeHistoricalExactRequest ();
    outOfBounds.table.predecessors[0].entityIndex = 3;
    ExpectFailure (outOfBounds, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest unknownTable = MakeHistoricalExactRequest ();
    unknownTable.table.predecessors[0].tableId = 999;
    ExpectFailure (unknownTable, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest futureHistorical = MakeHistoricalExactRequest ();
    futureHistorical.table.supportTables[0].context.revision = 4;
    ExpectFailure (futureHistorical, ProvenanceDiagnostic::ProvenanceStale, "ProvenanceStale");

    ProvenanceRequest futureCurrent = MakeHistoricalExactRequest ();
    futureCurrent.table.supportTables[1].context.revision = 2;
    ExpectFailure (futureCurrent, ProvenanceDiagnostic::ProvenanceStale, "ProvenanceStale");
}

TEST (GeometryRustIdentity, CatalogOrderAndSourceBindingRefsAreAccepted)
{
    ProvenanceRequest reordered = MakeValidRequest ();
    std::reverse (reordered.table.bindings.begin (), reordered.table.bindings.end ());
    ProvenanceDiagnostic diagnostic = ProvenanceDiagnostic::ProvenanceConflict;
    std::string error;
    ASSERT_TRUE (ValidateProvenance (reordered, diagnostic, error)) << error;
    EXPECT_EQ (ProvenanceDiagnostic::None, diagnostic);

    ProvenanceRequest sourceBinding = MakeValidRequest ();
    NativeBinding binding;
    binding.logicalKind = LogicalKind::Source;
    binding.logicalId = 11;
    binding.nativeGuid = GuidBytes ("AAAABBBB-CCCC-DDDD-EEEE-FFFF00001111");
    binding.metadataUuid = {};
    binding.metadataPresent = false;
    binding.role = IdentityRange { 0, 6 };
    binding.revision = 0;
    sourceBinding.table.bindings.push_back (binding);
    sourceBinding.table.bindingRefs = { 2 };
    sourceBinding.table.sources[0].bindings = IdentityRange { 0, 1 };

    ASSERT_TRUE (ValidateProvenance (sourceBinding, diagnostic, error)) << error;
    EXPECT_EQ (ProvenanceDiagnostic::None, diagnostic);

    // A source reference must resolve to a binding of the same logical kind/id.
    ProvenanceRequest mismatched = sourceBinding;
    mismatched.table.bindings[2].logicalKind = LogicalKind::Object;
    ExpectFailure (mismatched, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest dangling = sourceBinding;
    dangling.table.bindingRefs[0] = 9;
    ExpectFailure (dangling, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");
}

TEST (GeometryRustIdentity, PatchRowsRequireOwnerTokenAndUniqueKey)
{
    ProvenanceRequest valid = MakeValidRequest ();
    valid.domains = 0b1111U;
    valid.counts.patches = 1;
    EntityMapping patch = MakeGeneratedRow (IdentityDomain::Patch, 0);
    patch.patchKey = 7;
    valid.table.mappings.push_back (patch);

    ProvenanceDiagnostic diagnostic = ProvenanceDiagnostic::ProvenanceConflict;
    std::string error;
    ASSERT_TRUE (ValidateProvenance (valid, diagnostic, error)) << error;
    EXPECT_EQ (ProvenanceDiagnostic::None, diagnostic);

    ProvenanceRequest noKey = valid;
    noKey.table.mappings.back ().patchKey = 0;
    ExpectFailure (noKey, ProvenanceDiagnostic::ProvenanceConflict, "ProvenanceConflict");

    ProvenanceRequest noOwner = valid;
    noOwner.table.mappings.back ().object = 0;
    ExpectFailure (noOwner, ProvenanceDiagnostic::ProvenanceUnattributable, "ProvenanceUnattributable");

    ProvenanceRequest duplicateToken = valid;
    duplicateToken.counts.patches = 2;
    EntityMapping second = patch;
    second.outputIndex = 1;
    duplicateToken.table.mappings.push_back (second);
    ExpectFailure (duplicateToken, ProvenanceDiagnostic::PatchKeyUnstable, "PatchKeyUnstable");
}

TEST (GeometryRustIdentity, FailedAttributedMeshLeavesInputsUnchanged)
{
    Mesh mesh = BuildMesh ();
    ProvenanceRequest request = MakeValidRequest ();
    request.table.mappings.erase (request.table.mappings.begin ());
    const Mesh meshBefore = mesh;
    const ProvenanceRequest requestBefore = request;

    ProvenanceDiagnostic diagnostic = ProvenanceDiagnostic::ProvenanceConflict;
    std::string error;
    EXPECT_FALSE (ValidateAttributedMesh (mesh, request, diagnostic, error));
    EXPECT_EQ (ProvenanceDiagnostic::ProvenanceUnattributable, diagnostic);
    EXPECT_FALSE (error.empty ());
    ExpectMeshUnchanged (meshBefore, mesh);
    ExpectRequestUnchanged (requestBefore, request);
}

TEST (GeometryRustIdentity, DirectAbiRequiresDiagnosticAndRejectsUnknownBoolean)
{
    ProvenanceRequest source = MakeValidRequest ();
    RawProvenance raw;
    BuildRaw (source, raw);
    std::array<char, 256> error {};

    EXPECT_EQ (TAPIOCA_ERR_ARG, tapioca_validate_provenance (&raw.request, nullptr, error.data (), error.size ()));
    EXPECT_NE ('\0', error[0]);

    error.fill ('\0');
    uint32_t diagnostic = std::numeric_limits<uint32_t>::max ();
    raw.request.requireObject = 2;
    EXPECT_EQ (TAPIOCA_ERR_ARG, tapioca_validate_provenance (&raw.request, &diagnostic, error.data (), error.size ()));
    EXPECT_NE ('\0', error[0]);
    EXPECT_EQ (std::numeric_limits<uint32_t>::max (), diagnostic);
}

TEST (GeometryRustIdentity, DirectAbiReportsStatusAndNumericDiagnostic)
{
    ProvenanceRequest source = MakeValidRequest ();
    RawProvenance valid;
    BuildRaw (source, valid);
    std::array<char, 256> error {};
    uint32_t diagnostic = std::numeric_limits<uint32_t>::max ();
    ASSERT_EQ (TAPIOCA_OK, tapioca_validate_provenance (&valid.request, &diagnostic, error.data (), error.size ()))
        << error.data ();
    EXPECT_EQ (static_cast<uint32_t> (TAPIOCA_DIAGNOSTIC_NONE), diagnostic);

    ProvenanceRequest stale = MakeValidRequest ();
    stale.expectedRevision = 1;
    RawProvenance staleRaw;
    BuildRaw (stale, staleRaw);
    diagnostic = std::numeric_limits<uint32_t>::max ();
    EXPECT_EQ (TAPIOCA_ERR_STATE,
               tapioca_validate_provenance (&staleRaw.request, &diagnostic, error.data (), error.size ()));
    EXPECT_EQ (static_cast<uint32_t> (TAPIOCA_PROVENANCE_STALE), diagnostic);
    EXPECT_EQ (0U, std::string (error.data ()).rfind ("ProvenanceStale", 0)) << error.data ();

    ProvenanceRequest missing = MakeValidRequest ();
    missing.table.mappings.erase (missing.table.mappings.begin ());
    RawProvenance missingRaw;
    BuildRaw (missing, missingRaw);
    diagnostic = std::numeric_limits<uint32_t>::max ();
    EXPECT_EQ (TAPIOCA_ERR_STATE,
               tapioca_validate_provenance (&missingRaw.request, &diagnostic, error.data (), error.size ()));
    EXPECT_EQ (static_cast<uint32_t> (TAPIOCA_PROVENANCE_UNATTRIBUTABLE), diagnostic);
}
