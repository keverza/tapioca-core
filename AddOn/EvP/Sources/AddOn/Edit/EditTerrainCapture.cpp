#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include "Geometry/Mesh.hpp"
#include <algorithm>
#include <cmath>

namespace tapioca::geometry {
namespace {
using namespace terrain_detail;

const char* RoleName (SourceRole role)
{
    switch (role) {
        case SourceRole::PropertyLine:
            return "property";
        case SourceRole::Guide:
            return "guide";
        case SourceRole::PathCenterline:
            return "path";
        case SourceRole::Cutter:
            return "cutter";
        case SourceRole::MaterialBoundary:
            return "material";
        case SourceRole::GradingLimit:
            return "grading";
        case SourceRole::ProtectedZone:
            return "protected";
        case SourceRole::Terrain:
            return "terrain";
    }
    return "source";
}

} // namespace

bool CaptureLandscapeTerrain (const geomsrv::edit::Document& doc, const geomsrv::Mesh& mesh,
                              const TerrainCaptureOptions& options, TerrainCapture& capture, std::string& error)
{
    if (!doc.landscape) {
        error = "ProvenanceStale: terrain capture has no landscape session";
        return false;
    }
    const auto& session = *doc.landscape;
    std::string guid;
    if (!CanonicalLandscapeGuid (mesh.guid, guid) || guid != session.sourceTerrainGuid) {
        error = "ProvenanceStale: the captured mesh is not the session's terrain";
        return false;
    }
    const auto terrain = std::find_if (session.sources.begin (), session.sources.end (), [] (const auto& source) {
        return source.role == SourceRole::Terrain && !source.deleted;
    });
    if (terrain == session.sources.end ()) {
        error = "ProvenanceUnattributable: the session has no terrain source (SetLandscapeTerrain)";
        return false;
    }
    const auto owner = std::find_if (session.objects.begin (), session.objects.end (),
                                     [&] (const auto& object) { return object.id == options.owner; });
    const auto ownerOperation =
        owner == session.objects.end ()
            ? session.operations.end ()
            : std::find_if (session.operations.begin (), session.operations.end (),
                            [&] (const auto& op) { return op.id == owner->footprintOperation; });
    if (options.owner == 0 || ownerOperation == session.operations.end ()) {
        error = "ProvenanceUnattributable: the terrain owner must be an object with a footprint operation";
        return false;
    }
    if (options.table == 0 || options.nativeTable == 0 || options.table == options.nativeTable) {
        error = "ProvenanceConflict: capture and native table tokens must be nonzero and distinct";
        return false;
    }
    const size_t vertices = mesh.vertices.size () / 3, faces = mesh.triangles.size () / 3;
    if (vertices == 0 || faces == 0 || mesh.vertices.size () % 3 != 0 || mesh.triangles.size () % 3 != 0 ||
        std::any_of (mesh.vertices.begin (), mesh.vertices.end (), [] (double v) { return !std::isfinite (v); }) ||
        std::any_of (mesh.triangles.begin (), mesh.triangles.end (), [&] (uint32_t i) { return i >= vertices; })) {
        error = "TerrainShape: the captured terrain needs finite coordinates and valid triangles";
        return false;
    }

    TerrainCapture candidate;
    candidate.points = mesh.vertices;
    candidate.triangles = mesh.triangles;
    candidate.revision = doc.revision;
    candidate.sourceTerrainGuid = guid;
    auto& request = candidate.provenance;
    request.expectedSession = session.sessionUuid;
    request.expectedRevision = doc.revision;
    request.counts = { vertices, 0, faces, 0 };
    request.domains = kMeshProvenanceDomains;
    request.requireObject = true;
    auto& table = request.table;
    table.context = { 1, session.sessionUuid, doc.revision, options.table };
    for (const auto& source : session.sources) {
        if (source.deleted)
            continue;
        IdentitySource row { source.id, { table.bindingRefs.size (), 0 } };
        if (!source.archicadGuid.empty ()) {
            NativeBinding binding;
            binding.logicalKind = LogicalKind::Source;
            binding.logicalId = source.id;
            std::copy (source.archicadGuid.begin (), source.archicadGuid.end (), binding.nativeGuid.begin ());
            const std::string role = RoleName (source.role);
            binding.role = { table.roleBytes.size (), role.size () };
            table.roleBytes.insert (table.roleBytes.end (), role.begin (), role.end ());
            binding.revision = doc.revision; // The link as this capture observed it.
            table.bindingRefs.push_back (table.bindings.size ());
            table.bindings.push_back (binding);
            row.bindings.count = 1;
        }
        table.sources.push_back (row);
    }
    for (const auto& operation : session.operations) {
        IdentityOperation row { operation.id, { table.operationIds.size (), 0 } };
        for (Id input : operation.inputs)
            if (std::any_of (session.operations.begin (), session.operations.end (),
                             [=] (const auto& op) { return op.id == input; })) {
                table.operationIds.push_back (input);
                ++row.parents.count;
            }
        table.operations.push_back (row);
    }
    table.objects = { { owner->id, owner->footprintOperation } };
    table.supportTables = { { { 1, session.sessionUuid, doc.revision, options.nativeTable }, request.counts, true } };
    const auto sources = Append (table.sourceIds, { terrain->id });
    const auto operations = Append (table.operationIds, { owner->footprintOperation });
    for (auto [domain, count] : { std::pair { IdentityDomain::Vertex, vertices }, { IdentityDomain::Face, faces } })
        for (uint64_t i = 0; i < count; ++i) {
            EntityMapping row;
            row.domain = domain;
            row.kind = MappingKind::Exact;
            row.outputIndex = i;
            row.sources = sources;
            row.operations = operations;
            row.object = owner->id;
            row.predecessors = { table.predecessors.size (), 1 };
            table.predecessors.push_back ({ options.nativeTable, domain, i });
            table.mappings.push_back (row);
        }
    if (!Validate (request, error))
        return false;
    capture = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
