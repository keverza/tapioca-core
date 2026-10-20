#include "Geometry/GeometryRustMeshPrep.hpp"
extern "C" int mesh_prep_c_smoke (void);

int main ()
{
    if (mesh_prep_c_smoke () != 0)
        return 1;
    using namespace tapioca::geometry;
    const std::array<WeldCorner, 2> corners { { { 1, { 1, 2, 3 }, { 0, 0, 1 } }, { 1, { 9, 9, 9 }, { 0, 0, 1 } } } };
    const std::array<uint64_t, 3> offsets { 0, 1, 2 };
    WeldedCorners welded;
    std::string error;
    if (!WeldCorners (corners, offsets, welded, error) || welded.cornerVertices != std::vector<uint32_t> { 0, 1 })
        return 2;
    MeshAnalysis result;
    if (!AnalyzeMesh ({ welded.positions, welded.normals, {}, {}, {} }, result, error) || !result.normalsUsable ||
        result.smoothNormals != std::vector<float> (6, 0))
        return 3;
    return 0;
}
