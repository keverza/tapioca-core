#include "SurfaceAttributeMap.hpp"

namespace geomsrv::exposed {
namespace {
bool Fail (std::string& error, const char* reason)
{
    error = reason;
    return false;
}
} // namespace

bool ResolveSurfaceSelection (const Snapshot& snapshot, const std::vector<FaceKey>& faces,
                              const std::optional<std::set<int32_t>>& surfaceAttributes, SurfaceSelection& selection,
                              std::string& error)
{
    selection = {};
    error.clear ();
    if (snapshot.surfaceAttributes.empty ())
        return Fail (error, "Captured project surface catalog is absent; prepare a fresh snapshot.");
    const Limits limits;
    if (faces.size () > limits.maxFaces)
        return Fail (error, "Surface mapping face budget exceeded.");
    if (surfaceAttributes)
        for (int32_t index : *surfaceAttributes)
            if (index <= 0 || !snapshot.surfaceAttributes.contains (index))
                return Fail (error, "Selected surface attribute is absent from this capture's project catalog.");
    std::map<size_t, std::set<std::pair<int32_t, int32_t>>> requested;
    for (const auto& face : faces) {
        if (face.body.meshIndex >= snapshot.meshes.size () || face.body.sourceBody <= 0 || face.sourcePgon <= 0)
            return Fail (error, "Invalid capture-local source face for surface mapping.");
        requested[face.body.meshIndex].emplace (face.body.sourceBody, face.sourcePgon);
    }
    SurfaceSelection prepared;
    if (surfaceAttributes)
        prepared.materialPoolFilter.emplace ();
    size_t triangles = 0;
    for (const auto& [meshIndex, keys] : requested) {
        const Mesh& mesh = snapshot.meshes[meshIndex];
        const size_t count = mesh.TriangleCount ();
        if (count > limits.maxInputTriangles || triangles > limits.maxInputTriangles - count)
            return Fail (error, "Surface mapping input triangle budget exceeded.");
        triangles += count;
        if (mesh.triangles.size () % 3 || mesh.triSourceBody.size () != count || mesh.triSourcePgon.size () != count ||
            mesh.triMaterial.size () != count || mesh.triSurfaceAttribute.size () != count)
            return Fail (error,
                         "Captured surface/source sidecars are missing or misaligned; prepare a fresh snapshot.");
        std::map<std::pair<int32_t, int32_t>, std::pair<int32_t, int32_t>> assignments;
        for (size_t i = 0; i < count; ++i) {
            const auto key = std::make_pair (mesh.triSourceBody[i], mesh.triSourcePgon[i]);
            if (!keys.contains (key))
                continue;
            const int32_t pool = mesh.triMaterial[i], surface = mesh.triSurfaceAttribute[i];
            if (pool <= 0 || !snapshot.materialIdentity.contains (pool) || surface < 0 ||
                (surface > 0 && !snapshot.surfaceAttributes.contains (surface)))
                return Fail (error, "Source surface/pool identity is not valid in this captured catalog.");
            if (surface == 0 && surfaceAttributes)
                return Fail (error,
                             "A candidate face has no original project surface; cannot safely apply a surface filter.");
            const auto assignment = std::make_pair (pool, surface);
            const auto [faceIt, faceAdded] = assignments.emplace (key, assignment);
            if (!faceAdded && faceIt->second != assignment)
                return Fail (error, "Source face has conflicting surface assignments.");
            const auto [it, added] = prepared.poolToSurface.emplace (pool, surface);
            if (!added && it->second != surface)
                return Fail (error, "Candidate model pool has ambiguous original surface assignments.");
            if (surfaceAttributes && surfaceAttributes->contains (surface))
                prepared.materialPoolFilter->insert (pool);
        }
        if (assignments.size () != keys.size ())
            return Fail (error, "Requested source face is absent from this capture.");
    }
    selection = std::move (prepared);
    return true;
}
} // namespace geomsrv::exposed
