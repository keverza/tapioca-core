#include "Geometry/GeometryRustMeshPrep.hpp"
#include "Geometry/GeometryRustFfi.h"
#include <cstring>

namespace {
TapiocaStatus s_status = TAPIOCA_ERR_ARG;
bool s_message = false;
void Message (char* error, size_t capacity)
{
    if (capacity != 0) {
        const char* text = s_message ? "stub mesh prep refusal" : "";
        if (std::strlen (text) + 1 <= capacity)
            std::memcpy (error, text, std::strlen (text) + 1);
    }
}
} // namespace
extern "C" TapiocaStatus tapioca_weld_corners (const TapiocaWeldRequest*, const TapiocaWeldOutput* output, char* error,
                                               size_t capacity)
{
    *output->vertexCount = 999;
    if (output->scalarCapacity != 0) {
        output->positions[0] = -999;
        output->normals[0] = -999;
    }
    if (output->cornerCapacity != 0)
        output->cornerVertices[0] = 999;
    Message (error, capacity);
    return s_status;
}
extern "C" TapiocaStatus tapioca_analyze_mesh (const TapiocaMeshAnalysisRequest*,
                                               const TapiocaMeshAnalysisOutput* output, char* error, size_t capacity)
{
    output->summary->normalsUsable = 0;
    output->summary->bounds.mn[0] = -999;
    if (output->scalarCapacity != 0)
        output->smoothNormals[0] = -999;
    if (output->triangleCapacity != 0) {
        output->roundoffZero[0] = 99;
        output->wireMasks[0] = 99;
    }
    Message (error, capacity);
    return s_status;
}

int main ()
{
    using namespace tapioca::geometry;
    const std::array<WeldCorner, 1> corners { { { 1, { 1, 2, 3 }, { 0, 0, 1 } } } };
    const std::array<uint64_t, 2> offsets { 0, 1 };
    const std::array<double, 9> positions { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    const std::array<uint32_t, 3> triangles { 0, 1, 2 };
    for (auto status : std::array<TapiocaStatus, 4> { TAPIOCA_ERR_ARG, TAPIOCA_ERR_STATE, TAPIOCA_ERR_INTERNAL, 99 }) {
        s_status = status;
        for (bool message : { false, true }) {
            s_message = message;
            WeldedCorners welded { { 77 }, { 88 }, { 99 } };
            std::string error;
            if (WeldCorners (corners, offsets, welded, error) || error.empty () ||
                welded.positions != std::vector<double> { 77 } || welded.normals != std::vector<float> { 88 } ||
                welded.cornerVertices != std::vector<uint32_t> { 99 })
                return 1;
            MeshAnalysis result;
            result.bounds.Expand (1, 2, 3);
            result.normalsUsable = true;
            result.smoothNormals = { 77 };
            result.roundoffZero = { 88 };
            result.wireMasks = { 99 };
            if (AnalyzeMesh ({ positions, {}, triangles, {}, {} }, result, error) || error.empty () ||
                result.bounds.mn[0] != 1 || !result.normalsUsable ||
                result.smoothNormals != std::vector<float> { 77 } ||
                result.roundoffZero != std::vector<uint8_t> { 88 } || result.wireMasks != std::vector<uint8_t> { 99 })
                return 2;
        }
    }
    // Even an erroneous successful stub cannot publish an out-of-range count/remap.
    s_status = TAPIOCA_OK;
    WeldedCorners welded { { 77 }, { 88 }, { 99 } };
    std::string error;
    if (WeldCorners (corners, offsets, welded, error) || error.empty () ||
        welded.positions != std::vector<double> { 77 })
        return 3;
    return 0;
}
