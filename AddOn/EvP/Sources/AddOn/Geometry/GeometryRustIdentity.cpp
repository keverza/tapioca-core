#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustPolygon.hpp"

#include "GeometryRustFfi.h"
#include "Mesh.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace tapioca::geometry {
namespace {

constexpr size_t kErrorCapacity = 256;
using ErrorBuffer = std::array<char, kErrorCapacity>;

size_t ErrorLength (const ErrorBuffer& buffer)
{
    for (size_t index = 0; index < buffer.size (); ++index) {
        if (buffer[index] == '\0')
            return index;
    }
    return buffer.size ();
}

std::string FailureMessage (TapiocaStatus status, uint32_t diagnostic, const ErrorBuffer& buffer)
{
    const size_t length = ErrorLength (buffer);
    if (length != 0)
        return std::string (buffer.data (), length);
    // A failed status with no diagnostic is still a failure; never turn it into
    // success or drop the status. This also covers overlap/argument cases where
    // the boundary intentionally writes no message.
    return "Tapioca Rust ABI call failed with status " + std::to_string (status) + " and diagnostic " +
           std::to_string (diagnostic);
}

TapiocaIdentityRange MakeIdentityRange (const IdentityRange& range)
{
    TapiocaIdentityRange result {};
    result.offset = range.offset;
    result.count = range.count;
    return result;
}

TapiocaIdentityContext MakeIdentityContext (const IdentityContext& context)
{
    TapiocaIdentityContext result {};
    result.schemaVersion = context.schemaVersion;
    // Explicit byte copy: the ABI array is POD storage, not a C++ UUID type.
    std::copy (context.sessionUuid.begin (), context.sessionUuid.end (), result.sessionUuid);
    result.revision = context.revision;
    result.tableId = context.tableId;
    return result;
}

TapiocaEntityCounts MakeEntityCounts (const EntityCounts& counts)
{
    TapiocaEntityCounts result {};
    result.vertices = counts.vertices;
    result.edges = counts.edges;
    result.faces = counts.faces;
    result.patches = counts.patches;
    return result;
}

TapiocaMeshView MakeMeshView (const geomsrv::Mesh& mesh)
{
    TapiocaMeshView view {};
    view.guid = TapiocaTextView { mesh.guid.data (), mesh.guid.size () };
    view.elemType = mesh.elemType;
    view.vertices = TapiocaDoubleView { mesh.vertices.data (), mesh.vertices.size () };
    view.normals = TapiocaFloatView { mesh.normals.data (), mesh.normals.size () };
    view.triangles = TapiocaIndexView { mesh.triangles.data (), mesh.triangles.size () };
    view.triMaterial = TapiocaIntView { mesh.triMaterial.data (), mesh.triMaterial.size () };
    view.triWireEdges = TapiocaByteView { mesh.triWireEdges.data (), mesh.triWireEdges.size () };
    view.triSourceBody = TapiocaIntView { mesh.triSourceBody.data (), mesh.triSourceBody.size () };
    view.triSourcePgon = TapiocaIntView { mesh.triSourcePgon.data (), mesh.triSourcePgon.size () };
    view.triSurfaceAttribute = TapiocaIntView { mesh.triSurfaceAttribute.data (), mesh.triSurfaceAttribute.size () };
    return view;
}

// Owns only POD copies of the C++ catalogs. Mesh coordinates and sidecars stay
// borrowed by the caller and are never copied here.
struct AbiProvenanceStorage {
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
    TapiocaProvenanceView view {};
};

void ConvertProvenance (const ProvenanceTable& table, AbiProvenanceStorage& storage)
{
    storage.context = MakeIdentityContext (table.context);

    storage.sources.reserve (table.sources.size ());
    for (const IdentitySource& source : table.sources) {
        TapiocaSourceRecord record {};
        record.id = source.id;
        record.bindings = MakeIdentityRange (source.bindings);
        storage.sources.push_back (record);
    }

    storage.operations.reserve (table.operations.size ());
    for (const IdentityOperation& operation : table.operations) {
        TapiocaOperationRecord record {};
        record.id = operation.id;
        record.parents = MakeIdentityRange (operation.parents);
        storage.operations.push_back (record);
    }

    storage.objects.reserve (table.objects.size ());
    for (const IdentityObject& object : table.objects) {
        TapiocaObjectRecord record {};
        record.id = object.id;
        record.operation = object.operation;
        storage.objects.push_back (record);
    }

    storage.bindings.reserve (table.bindings.size ());
    for (const NativeBinding& binding : table.bindings) {
        TapiocaBindingRecord record {};
        record.logicalKind = static_cast<uint32_t> (binding.logicalKind);
        record.logicalId = binding.logicalId;
        std::copy (binding.nativeGuid.begin (), binding.nativeGuid.end (), record.nativeGuid);
        std::copy (binding.metadataUuid.begin (), binding.metadataUuid.end (), record.metadataUuid);
        record.metadataPresent = binding.metadataPresent ? 1U : 0U;
        record.role = MakeIdentityRange (binding.role);
        record.revision = binding.revision;
        storage.bindings.push_back (record);
    }

    storage.bindingRefs.assign (table.bindingRefs.begin (), table.bindingRefs.end ());
    storage.sourceIds.assign (table.sourceIds.begin (), table.sourceIds.end ());
    storage.operationIds.assign (table.operationIds.begin (), table.operationIds.end ());

    storage.predecessors.reserve (table.predecessors.size ());
    for (const EntityPredecessor& predecessor : table.predecessors) {
        TapiocaPredecessorRecord record {};
        record.tableId = predecessor.tableId;
        record.domain = static_cast<uint32_t> (predecessor.domain);
        record.entityIndex = predecessor.entityIndex;
        storage.predecessors.push_back (record);
    }

    storage.mappings.reserve (table.mappings.size ());
    for (const EntityMapping& mapping : table.mappings) {
        TapiocaMappingRecord record {};
        record.domain = static_cast<uint32_t> (mapping.domain);
        record.kind = static_cast<uint32_t> (mapping.kind);
        record.outputIndex = mapping.outputIndex;
        record.sources = MakeIdentityRange (mapping.sources);
        record.operations = MakeIdentityRange (mapping.operations);
        record.object = mapping.object;
        record.predecessors = MakeIdentityRange (mapping.predecessors);
        record.patchKey = mapping.patchKey;
        storage.mappings.push_back (record);
    }

    storage.roleBytes.assign (table.roleBytes.begin (), table.roleBytes.end ());
    storage.supportTables.reserve (table.supportTables.size ());
    for (const IdentitySupportTable& support : table.supportTables) {
        TapiocaSupportTable record {};
        record.context = MakeIdentityContext (support.context);
        record.counts = MakeEntityCounts (support.counts);
        record.historical = support.historical ? 1U : 0U;
        storage.supportTables.push_back (record);
    }

    TapiocaProvenanceView view {};
    view.context = storage.context;
    view.sources = TapiocaSourceArray { storage.sources.data (), storage.sources.size () };
    view.operations = TapiocaOperationArray { storage.operations.data (), storage.operations.size () };
    view.objects = TapiocaObjectArray { storage.objects.data (), storage.objects.size () };
    view.bindings = TapiocaBindingArray { storage.bindings.data (), storage.bindings.size () };
    view.bindingRefs = TapiocaIdArray { storage.bindingRefs.data (), storage.bindingRefs.size () };
    view.sourceIds = TapiocaIdArray { storage.sourceIds.data (), storage.sourceIds.size () };
    view.operationIds = TapiocaIdArray { storage.operationIds.data (), storage.operationIds.size () };
    view.predecessors = TapiocaPredecessorArray { storage.predecessors.data (), storage.predecessors.size () };
    view.mappings = TapiocaMappingArray { storage.mappings.data (), storage.mappings.size () };
    view.roleBytes = TapiocaByteView { storage.roleBytes.data (), storage.roleBytes.size () };
    view.supportTables = TapiocaSupportArray { storage.supportTables.data (), storage.supportTables.size () };
    storage.view = view;
}

TapiocaProvenanceRequest MakeAbiRequest (const ProvenanceRequest& request, AbiProvenanceStorage& storage)
{
    ConvertProvenance (request.table, storage);

    TapiocaProvenanceRequest abiRequest {};
    std::copy (request.expectedSession.begin (), request.expectedSession.end (), abiRequest.expectedSession);
    abiRequest.expectedRevision = request.expectedRevision;
    abiRequest.counts = MakeEntityCounts (request.counts);
    abiRequest.domains = request.domains;
    abiRequest.requireObject = request.requireObject ? 1U : 0U;
    abiRequest.table = storage.view;
    return abiRequest;
}

bool PublishOutcome (TapiocaStatus status, uint32_t rawDiagnostic, const ErrorBuffer& buffer,
                     ProvenanceDiagnostic& diagnostic, std::string& error)
{
    if (status == TAPIOCA_OK && rawDiagnostic == static_cast<uint32_t> (ProvenanceDiagnostic::None)) {
        diagnostic = ProvenanceDiagnostic::None;
        error.clear ();
        return true;
    }

    const uint32_t firstDiagnostic = static_cast<uint32_t> (ProvenanceDiagnostic::ProvenanceUnattributable);
    const uint32_t lastDiagnostic = static_cast<uint32_t> (ProvenanceDiagnostic::PatchKeyUnstable);
    if (status == TAPIOCA_ERR_STATE && rawDiagnostic >= firstDiagnostic && rawDiagnostic <= lastDiagnostic) {
        // Publish only a completed structural refusal. Transport/argument
        // failures and inconsistent status/diagnostic pairs leave the caller's
        // diagnostic untouched.
        diagnostic = static_cast<ProvenanceDiagnostic> (rawDiagnostic);
        error = FailureMessage (status, rawDiagnostic, buffer);
        return false;
    }

    error = FailureMessage (status, rawDiagnostic, buffer);
    return false;
}

TapiocaStatus CallProvenance (const ProvenanceRequest& request, uint32_t& rawDiagnostic, ErrorBuffer& buffer)
{
    AbiProvenanceStorage storage;
    const TapiocaProvenanceRequest abiRequest = MakeAbiRequest (request, storage);
    return tapioca_validate_provenance (&abiRequest, &rawDiagnostic, buffer.data (), buffer.size ());
}

TapiocaStatus CallAttributedMesh (const geomsrv::Mesh& mesh, const ProvenanceRequest& request, uint32_t& rawDiagnostic,
                                  ErrorBuffer& buffer)
{
    AbiProvenanceStorage storage;
    const TapiocaProvenanceRequest abiRequest = MakeAbiRequest (request, storage);
    const TapiocaMeshView meshView = MakeMeshView (mesh);
    return tapioca_validate_attributed_mesh (&meshView, &abiRequest, &rawDiagnostic, buffer.data (), buffer.size ());
}

} // namespace

bool ValidateProvenance (const ProvenanceRequest& request, ProvenanceDiagnostic& diagnostic, std::string& error)
{
    ErrorBuffer buffer {};
    uint32_t rawDiagnostic = std::numeric_limits<uint32_t>::max ();
    const TapiocaStatus status = CallProvenance (request, rawDiagnostic, buffer);
    return PublishOutcome (status, rawDiagnostic, buffer, diagnostic, error);
}

bool ValidateAttributedMesh (const geomsrv::Mesh& mesh, const ProvenanceRequest& request,
                             ProvenanceDiagnostic& diagnostic, std::string& error)
{
    // The ABI owns the vertex+face-only rule and derives actual counts from the
    // borrowed mesh arrays; Edge/Patch mapping uses the generic gate instead.
    ErrorBuffer buffer {};
    uint32_t rawDiagnostic = std::numeric_limits<uint32_t>::max ();
    const TapiocaStatus status = CallAttributedMesh (mesh, request, rawDiagnostic, buffer);
    return PublishOutcome (status, rawDiagnostic, buffer, diagnostic, error);
}

bool CopyAttributedPolygon (const AttributedPolygon& source, AttributedPolygon& result, std::string& error)
{
    // Reuse the existing explicit catalog converter. Never reinterpret C++
    // Vector3/table storage; the Rust validator checks actual contour counts.
    AbiProvenanceStorage storage;
    const auto provenance = MakeAbiRequest (source.provenance, storage);
    std::vector<double> points;
    if (source.polygon.points.size () > points.max_size () / 3) {
        error = "PolygonShape: coordinate scalar capacity overflow";
        return false;
    }
    points.reserve (source.polygon.points.size () * 3);
    for (const auto& point : source.polygon.points)
        points.insert (points.end (), { point.x, point.y, point.z });
    const TapiocaAttributedPolygonRequest input { { points.data (), points.size () }, provenance };
    std::vector<double> output (points.size ());
    ErrorBuffer buffer {};
    const auto status =
        tapioca_copy_attributed_polygon (&input, output.data (), output.size (), buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, 0, buffer);
        return false;
    }
    AttributedPolygon candidate;
    candidate.provenance = source.provenance; // Entire retained table, no new identity or mapping.
    candidate.polygon.points.reserve (source.polygon.points.size ());
    for (size_t index = 0; index < output.size (); index += 3)
        candidate.polygon.points.push_back ({ output[index], output[index + 1], output[index + 2] });
    result = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
