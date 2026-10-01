#include "ArchViz/ElementPacket.hpp"
#include "ArchViz/ArchVizLog.hpp"

#include <algorithm>
#include <atomic>

namespace geomsrv::archviz {
namespace {
std::atomic<uint64_t> s_packetSequence { 0 };
}

std::unique_ptr<ElementUpload> MakeElementPacket (const CapturedMeshPacket& packet)
{
    const auto started = std::chrono::steady_clock::now ();
    const Mesh& mesh = packet.mesh;
    if (mesh.triangles.empty () || mesh.vertices.empty ())
        return nullptr;
    auto up = std::make_unique<ElementUpload> ();
    up->guid = mesh.guid;
    up->packetId = ++s_packetSequence;
    up->capturedAt = packet.capturedAt;
    // Keep world bounds: float32 conversion still has the documented precision
    // limitation on survey-placed projects (ElementUpload's contract).
    up->vertices.resize (mesh.vertices.size ());
    for (size_t i = 0; i < mesh.vertices.size (); ++i)
        up->vertices[i] = static_cast<float> (mesh.vertices[i]);
    up->normals = mesh.normals;
    BuildMaterialGroups (mesh.triangles, mesh.triMaterial, up->indices, up->ranges, &mesh.triWireEdges, &up->wireEdges);
    if (up->indices.empty ())
        return nullptr;
    if (mesh.bounds.Valid ()) {
        for (int axis = 0; axis < 3; ++axis) {
            up->boundsMin[axis] = static_cast<float> (mesh.bounds.mn[axis]);
            up->boundsMax[axis] = static_cast<float> (mesh.bounds.mx[axis]);
        }
    }
    else {
        for (int axis = 0; axis < 3; ++axis) {
            up->boundsMin[axis] = 1e30f;
            up->boundsMax[axis] = -1e30f;
        }
        for (size_t i = 0; i + 2 < up->vertices.size (); i += 3) {
            for (int axis = 0; axis < 3; ++axis) {
                up->boundsMin[axis] = std::min (up->boundsMin[axis], up->vertices[i + axis]);
                up->boundsMax[axis] = std::max (up->boundsMax[axis], up->vertices[i + axis]);
            }
        }
    }
    const size_t sourceBytes = mesh.vertices.size () * sizeof (double) + mesh.normals.size () * sizeof (float) +
                               mesh.triangles.size () * sizeof (uint32_t) +
                               mesh.triMaterial.size () * sizeof (int32_t) + mesh.triWireEdges.size ();
    const double convertMs =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    ArchVizLog (
        "pipeline: stage=geometry-convert packet=" + std::to_string (up->packetId) + " guid=" + mesh.guid +
        " vertices=" + std::to_string (up->VertexCount ()) + " triangles=" + std::to_string (up->indices.size () / 3) +
        " sourceBytes=" + std::to_string (sourceBytes) + " payloadBytes=" + std::to_string (up->PayloadBytes ()) +
        " retainedBytes=" + std::to_string (up->Bytes ()) +
        " captureMs=" + std::to_string (packet.captureMilliseconds) + " convertMs=" + std::to_string (convertMs));
    return up;
}

} // namespace geomsrv::archviz
