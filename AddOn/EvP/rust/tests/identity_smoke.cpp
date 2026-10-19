#include "Geometry/GeometryRustTerrain.hpp"

#include "GeometryRustFfi.h"
#include "Mesh.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

static_assert (sizeof (TapiocaIdentityContext) == 40, "identity context ABI layout changed");
static_assert (sizeof (TapiocaSourceRecord) == 24, "source record ABI layout changed");
static_assert (sizeof (TapiocaOperationRecord) == 24, "operation record ABI layout changed");
static_assert (sizeof (TapiocaObjectRecord) == 16, "object record ABI layout changed");
static_assert (sizeof (TapiocaBindingRecord) == 96, "binding record ABI layout changed");
static_assert (sizeof (TapiocaMappingRecord) == 80, "mapping record ABI layout changed");
static_assert (sizeof (TapiocaPredecessorRecord) == 24, "predecessor record ABI layout changed");
static_assert (sizeof (TapiocaSupportTable) == 80, "support table ABI layout changed");
static_assert (sizeof (TapiocaProvenanceView) == 216, "provenance view ABI layout changed");
static_assert (sizeof (TapiocaProvenanceRequest) == 280, "provenance request ABI layout changed");

namespace {

using geomsrv::Mesh;
using namespace tapioca::geometry;

constexpr std::array<uint8_t, 16> kSessionUuid { 1 };

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
    Mesh mesh;
    mesh.guid = "identity-smoke";
    mesh.elemType = 1;
    mesh.vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    mesh.normals = { 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F };
    mesh.triangles = { 0, 1, 2 };
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

bool SameMesh (const Mesh& left, const Mesh& right)
{
    return left.guid == right.guid && left.elemType == right.elemType && left.vertices == right.vertices &&
           left.normals == right.normals && left.triangles == right.triangles &&
           left.triMaterial == right.triMaterial && left.triWireEdges == right.triWireEdges &&
           left.triSourceBody == right.triSourceBody && left.triSourcePgon == right.triSourcePgon &&
           left.triSurfaceAttribute == right.triSurfaceAttribute;
}

int Fail (const char* message)
{
    std::fprintf (stderr, "identity_smoke: %s\n", message);
    return 1;
}

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

} // namespace

int main ()
{
    const ProvenanceRequest valid = MakeValidRequest ();
    const Mesh mesh = BuildMesh ();
    const ProvenanceRequest requestBefore = valid;
    const Mesh meshBefore = mesh;

    ProvenanceDiagnostic diagnostic = ProvenanceDiagnostic::ProvenanceConflict;
    std::string error = "stale";
    if (!ValidateProvenance (valid, diagnostic, error))
        return Fail ("valid table was rejected");
    if (diagnostic != ProvenanceDiagnostic::None || !error.empty ())
        return Fail ("valid table did not publish a clean diagnostic");
    if (!ValidateAttributedMesh (mesh, valid, diagnostic, error))
        return Fail ("valid attributed mesh was rejected");
    if (!(valid == requestBefore) || !SameMesh (mesh, meshBefore))
        return Fail ("validated inputs were modified");

    ProvenanceRequest unattributable = MakeValidRequest ();
    unattributable.table.mappings.erase (unattributable.table.mappings.begin () + 2);
    const ProvenanceRequest unattributableBefore = unattributable;
    diagnostic = ProvenanceDiagnostic::None;
    error.clear ();
    if (ValidateProvenance (unattributable, diagnostic, error))
        return Fail ("unattributable table was accepted");
    if (diagnostic != ProvenanceDiagnostic::ProvenanceUnattributable || error.empty ())
        return Fail ("unattributable result was not published consistently");
    if (!(unattributable == unattributableBefore))
        return Fail ("failed validation modified the request");

    diagnostic = ProvenanceDiagnostic::None;
    error.clear ();
    if (ValidateAttributedMesh (mesh, unattributable, diagnostic, error))
        return Fail ("unattributable attributed mesh was accepted");
    if (diagnostic != ProvenanceDiagnostic::ProvenanceUnattributable || error.empty ())
        return Fail ("attributed mesh did not publish the completed refusal");
    if (!SameMesh (mesh, meshBefore))
        return Fail ("failed attributed validation modified the mesh");

    ProvenanceRequest stale = MakeValidRequest ();
    stale.expectedRevision = 1;
    const ProvenanceRequest staleBefore = stale;
    diagnostic = ProvenanceDiagnostic::None;
    error.clear ();
    if (ValidateProvenance (stale, diagnostic, error))
        return Fail ("stale table was accepted");
    if (diagnostic != ProvenanceDiagnostic::ProvenanceStale || error.empty ())
        return Fail ("stale result was not published consistently");
    if (!(stale == staleBefore))
        return Fail ("stale validation modified the request");

    RawProvenance raw;
    BuildRaw (valid, raw);
    std::array<char, 256> buffer {};
    if (tapioca_validate_provenance (&raw.request, nullptr, buffer.data (), buffer.size ()) != TAPIOCA_ERR_ARG)
        return Fail ("missing diagnostic pointer was not rejected");
    if (buffer[0] == '\0')
        return Fail ("missing diagnostic pointer returned no message");

    uint32_t numeric = std::numeric_limits<uint32_t>::max ();
    if (tapioca_validate_provenance (&raw.request, &numeric, buffer.data (), buffer.size ()) != TAPIOCA_OK)
        return Fail ("direct ABI valid request was rejected");
    if (numeric != TAPIOCA_DIAGNOSTIC_NONE)
        return Fail ("direct ABI valid request returned a diagnostic");

    RawProvenance rawStale;
    BuildRaw (stale, rawStale);
    numeric = std::numeric_limits<uint32_t>::max ();
    if (tapioca_validate_provenance (&rawStale.request, &numeric, buffer.data (), buffer.size ()) != TAPIOCA_ERR_STATE)
        return Fail ("direct ABI stale request did not return ERR_STATE");
    if (numeric != TAPIOCA_PROVENANCE_STALE)
        return Fail ("direct ABI stale request returned the wrong numeric code");

    return 0;
}
