#include "MeshExtraction.hpp"
#include "TriangleDegeneracy.hpp"

namespace geomsrv {

bool NormalizeExtractionCorners (std::span<const double> bodyPositions, std::vector<ExtractionCorner>& corners,
                                 size_t& degenerateNormals)
{
    if (corners.size () < 3 || bodyPositions.size () % 3 != 0)
        return false;
    for (const auto& corner : corners)
        if (corner.vertex < 0 || static_cast<size_t> (corner.vertex) >= bodyPositions.size () / 3)
            return false;

    const auto point = [&] (const ExtractionCorner& corner) { return &bodyPositions[size_t (corner.vertex) * 3]; };
    double fnx = 0.0, fny = 0.0, fnz = 0.0;
    TriangleNormal (point (corners[0]), point (corners[1]), point (corners[2]), fnx, fny, fnz);
    const bool faceOk = NormalizeVector (fnx, fny, fnz);
    for (auto& corner : corners) {
        if (!NormalizeVector (corner.nx, corner.ny, corner.nz)) {
            if (!faceOk)
                return false;
            corner.nx = fnx;
            corner.ny = fny;
            corner.nz = fnz;
            ++degenerateNormals;
        }
    }
    return true;
}

void AppendExtractedConvex (Mesh& mesh, VertexWelder& welder, std::span<const double> bodyPositions,
                            std::vector<ExtractionCorner>& corners, const ExtractionFace& face,
                            ExtractionCounts& counts)
{
    if (!NormalizeExtractionCorners (bodyPositions, corners, counts.degenerateNormals))
        return;
    const auto point = [&] (const ExtractionCorner& corner) { return &bodyPositions[size_t (corner.vertex) * 3]; };
    const auto emit = [&] (const ExtractionCorner& corner) {
        const auto* position = point (corner);
        return welder.Add (corner.vertex, position[0], position[1], position[2], corner.nx, corner.ny, corner.nz);
    };
    const uint32_t i0 = emit (corners[0]);
    for (size_t k = 1; k + 1 < corners.size (); ++k) {
        if (IsRoundoffZeroTriangle (point (corners[0]), point (corners[k]), point (corners[k + 1]))) {
            ++counts.emptyFanTriangles;
            continue;
        }
        const uint32_t sourceVertices[3] = { uint32_t (corners[0].vertex), uint32_t (corners[k].vertex),
                                             uint32_t (corners[k + 1].vertex) };
        mesh.triangles.push_back (i0);
        mesh.triangles.push_back (emit (corners[k]));
        mesh.triangles.push_back (emit (corners[k + 1]));
        mesh.triMaterial.push_back (face.material);
        mesh.triSurfaceAttribute.push_back (face.surfaceAttribute);
        mesh.triWireEdges.push_back (BuildTriangleWireEdgeMask (sourceVertices, face.visibleEdges));
        mesh.triSourceBody.push_back (face.body);
        mesh.triSourcePgon.push_back (face.polygon);
    }
}

} // namespace geomsrv
