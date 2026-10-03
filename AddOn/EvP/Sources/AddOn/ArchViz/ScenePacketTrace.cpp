#include "ArchViz/ScenePacketTrace.hpp"
#include "ArchViz/ArchVizLog.hpp"

namespace geomsrv::archviz {
namespace {
double Milliseconds (std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
{
    return std::chrono::duration<double, std::milli> (to - from).count ();
}
} // namespace

size_t SunStudyPayloadBytes (const SunStudyAtlasUpload& study)
{
    size_t bytes = study.studyId.size () + study.stepMinutes.size () * sizeof (uint16_t);
    if (study.texels != nullptr)
        bytes += study.texels->size () * sizeof (float);
    if (study.stepMasks != nullptr)
        bytes += study.stepMasks->size () * sizeof (uint32_t);
    for (const auto& element : study.Elements ())
        bytes += element.guid.size () + element.faces.size () * sizeof (SunFaceMap);
    return bytes;
}

ScenePacketTrace::ScenePacketTrace (const SceneCmd& command)
    : started_ (std::chrono::steady_clock::now ()), prepared_ (started_)
{
    if (command.type == SceneCmdType::UpsertElement && command.upload != nullptr) {
        const auto& packet = *command.upload;
        fields_ = "stage=geometry-apply packet=" + std::to_string (packet.packetId) + " guid=" + packet.guid +
                  " payloadBytes=" + std::to_string (packet.PayloadBytes ());
        if (packet.capturedAt != std::chrono::steady_clock::time_point {})
            captureAgeMs_ = Milliseconds (packet.capturedAt, started_);
    }
    else if (command.type == SceneCmdType::SetSunStudyAtlas && command.sunStudy != nullptr) {
        fields_ = "stage=sun-apply study=" + command.sunStudy->studyId +
                  " payloadBytes=" + std::to_string (SunStudyPayloadBytes (*command.sunStudy));
    }
    if (!fields_.empty () && command.queuedAt != std::chrono::steady_clock::time_point {})
        queueWaitMs_ = Milliseconds (command.queuedAt, started_);
}

ScenePacketTrace::~ScenePacketTrace ()
{
    if (fields_.empty ())
        return;
    const auto now = std::chrono::steady_clock::now ();
    try {
        ArchVizLog ("pipeline: " + fields_ + " applied=" + std::to_string (applied_) +
                    " gpuBufferBytes=" + std::to_string (gpuBytes_) + " queueWaitMs=" + std::to_string (queueWaitMs_) +
                    " captureAgeMs=" + std::to_string (captureAgeMs_) +
                    " cpuPrepareMs=" + std::to_string (Milliseconds (started_, prepared_)) +
                    " apiApplyMs=" + std::to_string (Milliseconds (prepared_, now)) +
                    " wallMs=" + std::to_string (Milliseconds (started_, now)));
    }
    catch (...) {
    }
}

void ScenePacketTrace::CpuPrepared ()
{
    prepared_ = std::chrono::steady_clock::now ();
}
void ScenePacketTrace::Applied (size_t gpuBytes)
{
    applied_ = true;
    gpuBytes_ = gpuBytes;
}

void LogSnapshotCapture (const Snapshot& snapshot, double wallMs)
{
    size_t bytes = 0, triangles = 0, vertices = 0;
    for (const auto& mesh : snapshot.meshes) {
        vertices += mesh.VertexCount ();
        triangles += mesh.TriangleCount ();
        bytes += mesh.vertices.size () * sizeof (double) + mesh.normals.size () * sizeof (float) +
                 mesh.triangles.size () * sizeof (uint32_t) + mesh.triMaterial.size () * sizeof (int32_t) +
                 mesh.triWireEdges.size ();
    }
    ArchVizLog ("pipeline: stage=snapshot-capture snapshot=" + std::to_string (snapshot.id) +
                " elements=" + std::to_string (snapshot.meshes.size ()) + " vertices=" + std::to_string (vertices) +
                " triangles=" + std::to_string (triangles) + " payloadBytes=" + std::to_string (bytes) +
                " wallMs=" + std::to_string (wallMs));
}

void LogSunStudyDisplay (const SunStudyAtlasUpload& study, uint64_t snapshot, double wallMs)
{
    ArchVizLog ("pipeline: stage=sun-display study=" + study.studyId + " snapshot=" + std::to_string (snapshot) +
                " payloadBytes=" + std::to_string (SunStudyPayloadBytes (study)) +
                " atlasBytes=" + std::to_string (study.texels != nullptr ? study.texels->size () * sizeof (float) : 0) +
                " elements=" + std::to_string (study.Elements ().size ()) + " wallMs=" + std::to_string (wallMs));
}

} // namespace geomsrv::archviz
