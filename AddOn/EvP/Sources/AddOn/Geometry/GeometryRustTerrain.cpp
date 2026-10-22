#include "Geometry/GeometryRustTerrain.hpp"

#include "GeometryRustFfi.h"
#include "GeometryRustIdentityAbi.hpp"
#include <type_traits>
#include "Mesh.hpp"

#include <array>
#include <vector>
#include <algorithm>
#include <limits>

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

std::string FailureMessage (TapiocaStatus status, const ErrorBuffer& buffer)
{
    const size_t length = ErrorLength (buffer);
    if (length != 0)
        return std::string (buffer.data (), length);
    // A failed status with no diagnostic is still a failure; never turn it into
    // success or drop the status. This also covers overlap/argument cases where
    // the boundary intentionally writes no message.
    return "Tapioca Rust ABI call failed with status " + std::to_string (status);
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

} // namespace

bool ReadRustAbiVersion (uint32_t& outVersion, std::string& error)
{
    ErrorBuffer buffer {};
    uint32_t version = 0;
    const TapiocaStatus status = tapioca_version (&version, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    if (version != TAPIOCA_ABI_VERSION) {
        error = "Tapioca Rust ABI version " + std::to_string (version) + " does not match expected " +
                std::to_string (TAPIOCA_ABI_VERSION);
        return false;
    }
    outVersion = version;
    error.clear ();
    return true;
}

bool ValidateMesh (const geomsrv::Mesh& mesh, std::string& error)
{
    const TapiocaMeshView view = MakeMeshView (mesh);
    ErrorBuffer buffer {};
    const TapiocaStatus status = tapioca_validate_mesh (&view, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    error.clear ();
    return true;
}

bool ValidateSnapshot (const geomsrv::Snapshot& snapshot, std::string& error)
{
    // Own only the descriptors, never the geometry arrays they point at. Catalogs
    // in the geomsrv::Snapshot are deliberately not forwarded or touched.
    std::vector<TapiocaMeshView> descriptors;
    descriptors.reserve (snapshot.meshes.size ());
    for (const geomsrv::Mesh& mesh : snapshot.meshes)
        descriptors.push_back (MakeMeshView (mesh));

    TapiocaSnapshotView view {};
    view.id = snapshot.id;
    view.captureStamp = snapshot.captureStamp;
    view.scope = TapiocaTextView { snapshot.scope.data (), snapshot.scope.size () };
    view.completeModel = snapshot.completeModel ? 1 : 0;
    view.meshes = TapiocaMeshArrayView { descriptors.data (), descriptors.size () };

    ErrorBuffer buffer {};
    const TapiocaStatus status = tapioca_validate_snapshot (&view, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    error.clear ();
    return true;
}

namespace {
template <class Request, class Function>
bool TerrainCall (const Request& input, Function function, const ProvenanceRequest& provenance, size_t inputPointCount,
                  TerrainResult& result, std::string& error)
{
    ErrorBuffer buffer {};
    TapiocaTerrainCounts counts {};
    TapiocaTerrainOutput output {};
    output.counts = &counts;
    output.queryOnly = 1;
    auto status = function (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    TerrainResult candidate;
    std::vector<TapiocaTerrainMapping> mappings;
    if (counts.points % 3 != 0 || counts.triangles % 3 != 0 || counts.points > candidate.points.max_size () ||
        counts.triangles > candidate.triangles.max_size () || counts.mappings > mappings.max_size ()) {
        error = "TerrainOutputShape: invalid query counts";
        return false;
    }
    candidate.points.resize (counts.points);
    candidate.triangles.resize (counts.triangles);
    mappings.resize (counts.mappings);
    const auto expected = counts;
    output.points = candidate.points.data ();
    output.pointCapacity = candidate.points.size ();
    output.triangles = candidate.triangles.data ();
    output.triangleCapacity = candidate.triangles.size ();
    output.mappings = mappings.data ();
    output.mappingCapacity = mappings.size ();
    output.queryOnly = 0;
    status = function (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    if (counts.points != expected.points || counts.triangles != expected.triangles ||
        counts.mappings != expected.mappings) {
        error = "TerrainOutputShape: query/fill counts disagree";
        return false;
    }
    for (const auto& mapping : mappings) {
        const bool surface = std::is_same_v<Request, TapiocaTerrainSurfaceRequest>;
        if (surface ? (mapping.kind != 18 && mapping.kind != 19) : (mapping.kind < 1 || mapping.kind > 11)) {
            error = "TerrainOutputShape: unknown support kind";
            return false;
        }
        TerrainMapping row { static_cast<TerrainMappingKind> (mapping.kind), mapping.index, {} };
        std::copy (std::begin (mapping.values), std::end (mapping.values), row.values.begin ());
        candidate.mappings.push_back (row);
    }
    candidate.inputProvenance = provenance;
    candidate.inputPointCount = inputPointCount;
    result = std::move (candidate);
    error.clear ();
    return true;
}
} // namespace

bool TriangulateTerrain (const TerrainRequest& request, TerrainResult& result, std::string& error)
{
    detail::AbiProvenanceStorage storage;
    TapiocaTerrainRequest input {};
    input.provenance = detail::ConvertProvenanceRequest (request.provenance, storage);
    input.points = { request.points.data (), request.points.size () };
    input.zTolerance = request.zTolerance;
    std::vector<TapiocaTerrainConstraint> constraints;
    std::vector<uint32_t> vertices;
    for (const auto& constraint : request.constraints) {
        constexpr auto limit = std::numeric_limits<uint32_t>::max ();
        if (vertices.size () > limit || constraint.vertices.size () > limit - vertices.size ()) {
            error = "TerrainConstraintRange: index pool overflow";
            return false;
        }
        constraints.push_back ({ static_cast<uint32_t> (constraint.role), static_cast<uint32_t> (vertices.size ()),
                                 static_cast<uint32_t> (constraint.vertices.size ()) });
        vertices.insert (vertices.end (), constraint.vertices.begin (), constraint.vertices.end ());
    }
    input.constraints = { constraints.data (), constraints.size () };
    input.constraintVertices = { vertices.data (), vertices.size () };
    if (request.refinement) {
        const auto& refinement = *request.refinement;
        input.refinement = { refinement.minAngleDegrees, refinement.maxAreaSquareMetres.value_or (0),
                             refinement.maxAddedVertices, 1, refinement.maxAreaSquareMetres ? 1U : 0U };
    }
    return TerrainCall (input, tapioca_terrain_triangulate, request.provenance, request.points.size () / 3, result,
                        error);
}

bool SplitTerrainMesh (const TerrainSplitRequest& request, TerrainResult& result, std::string& error)
{
    detail::AbiProvenanceStorage storage;
    TapiocaTerrainSplitRequest input {};
    input.provenance = detail::ConvertProvenanceRequest (request.provenance, storage);
    input.points = { request.points.data (), request.points.size () };
    input.triangles = { request.triangles.data (), request.triangles.size () };
    input.line = { request.line.data (), request.line.size () };
    input.arcAngles = { request.arcAngles.data (), request.arcAngles.size () };
    return TerrainCall (input, tapioca_terrain_split_mesh, request.provenance, request.points.size () / 3, result,
                        error);
}

bool ExtractTerrainSurface (const TerrainMeshView& mesh, const ProvenanceRequest& provenance, TerrainResult& result,
                            std::string& error)
{
    detail::AbiProvenanceStorage storage;
    TapiocaTerrainSurfaceRequest input {};
    input.points = { mesh.points.data (), mesh.points.size () };
    input.triangles = { mesh.triangles.data (), mesh.triangles.size () };
    input.provenance = detail::ConvertProvenanceRequest (provenance, storage);
    return TerrainCall (input, tapioca_terrain_surface, provenance, mesh.points.size () / 3, result, error);
}

} // namespace tapioca::geometry
