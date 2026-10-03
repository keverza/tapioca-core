#include "NativeCommands/SunStudyPreparation.hpp"
#include "SunStudy/SunStudyReuse.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/SunStudyGpuTraversal.hpp"

#include <algorithm>

namespace geomsrv {

void LogSunStudySurfaceSampling (uint64_t snapshot, const evp::sunstudy::SurfaceSamplingResult& result)
{
    archviz::ArchVizLog ("pipeline: stage=sun-grid-reuse snapshot=" + std::to_string (snapshot) +
                         " reusedMeshes=" + std::to_string (result.reusedMeshes) +
                         " rebuiltMeshes=" + std::to_string (result.rebuiltMeshes) +
                         " reusedGridSamples=" + std::to_string (result.reusedSamples) + " generatedSamples=" +
                         std::to_string (result.generatedSamples) + " valid=" + std::to_string (result.valid));
}

bool SunStudySnapshotBounds (const Snapshot& snapshot, double min[3], double max[3])
{
    bool haveBounds = false;
    for (const Mesh& mesh : snapshot.meshes) {
        for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3) {
            for (int axis = 0; axis < 3; ++axis) {
                const double value = mesh.vertices[v + axis];
                min[axis] = haveBounds ? std::min (min[axis], value) : value;
                max[axis] = haveBounds ? std::max (max[axis], value) : value;
            }
            haveBounds = true;
        }
    }
    return haveBounds;
}

std::shared_ptr<const evp::sunstudy::SunStudyOccluders>
PrepareSunStudyOccluders (std::shared_ptr<const Snapshot> snapshot, const evp::sunstudy::ElementRoles& roles,
                          const evp::sunstudy::StudyRecord* previous, const std::function<bool ()>& isCancelled)
{
    // A combined BVH built first would defeat reuse of a large static site.
    if (roles.context > 0)
        return evp::sunstudy::BuildSunStudyOccluders (
            *snapshot, roles, previous != nullptr ? previous->occluders.get () : nullptr, isCancelled);
    auto parts = std::make_shared<evp::sunstudy::SunStudyOccluders> ();
    if (const auto subset = evp::sunstudy::OccluderSnapshot (*snapshot, roles))
        parts->analysis = std::make_shared<const QueryEngine> (subset);
    else
        parts->analysis = QueryIndexCache::Get ().For (snapshot);
    return (parts->analysis == nullptr || (isCancelled && isCancelled ())) ? nullptr : parts;
}

SunStudyPreparationTrace::SunStudyPreparationTrace (uint64_t snapshot)
    : snapshot_ (snapshot), last_ (std::chrono::steady_clock::now ())
{
}

void SunStudyPreparationTrace::Mark (const char* stage, size_t payloadBytes)
{
    const auto now = std::chrono::steady_clock::now ();
    const double elapsed = std::chrono::duration<double, std::milli> (now - last_).count ();
    archviz::ArchVizLog ("pipeline: stage=sun-" + std::string (stage) + " snapshot=" + std::to_string (snapshot_) +
                         " payloadBytes=" + std::to_string (payloadBytes) + " wallMs=" + std::to_string (elapsed));
    last_ = std::chrono::steady_clock::now (); // exclude this diagnostic write
}

void FinishSunStudyPreparation (evp::sunstudy::StudyRecord& record, std::shared_ptr<const Snapshot> snapshot,
                                const evp::sunstudy::StudyRecord* reuseSource, const std::atomic<bool>* cancelled,
                                std::shared_ptr<const QueryEngine> occluders)
{
    // Both producers choose explicitly. CPU remains the public default; FP64
    // GPU packets retain exact fallback and lazy device creation.
    if (record.backend == "gpu") {
        const auto* previousGpu =
            reuseSource != nullptr ? dynamic_cast<const archviz::SunStudyGpuTraversal*> (reuseSource->traversal.get ())
                                   : nullptr;
        record.traversal = std::make_shared<archviz::SunStudyGpuTraversal> (
            std::move (occluders), record.occluders != nullptr ? record.occluders->context : nullptr, previousGpu);
    }
    if (record.occluders != nullptr && record.occluders->context != nullptr)
        archviz::ArchVizLog ("pipeline: stage=sun-context-index snapshot=" + std::to_string (record.snapshotId) +
                             " meshes=" + std::to_string (record.occluders->context->MeshCount ()) +
                             " triangles=" + std::to_string (record.occluders->context->TriangleCount ()) +
                             " reused=" + std::to_string (record.occluders->contextReused));
    record.snapshot = std::move (snapshot);
    evp::sunstudy::SetSampleMeshes (record);
    const std::atomic<bool> notCancelled { false };
    // Manual benchmarks always trace a fresh day, even if grids/atlases reuse.
    if (reuseSource != nullptr && cancelled != nullptr)
        evp::sunstudy::ReuseUnaffectedSamples (*reuseSource, record, cancelled != nullptr ? *cancelled : notCancelled);
    const uint64_t snapshotId = record.snapshotId;
    archviz::ArchVizLog ("pipeline: stage=sun-reuse snapshot=" + std::to_string (snapshotId) +
                         " samples=" + std::to_string (record.positions.size () / 3) +
                         " reused=" + std::to_string (record.reusedSamples) +
                         " dirty=" + std::to_string (record.positions.size () / 3 - record.reusedSamples) +
                         " gridM=" + std::to_string (record.gridSpacing));
    record.session.SetStepObserver ([snapshotId] (size_t step, const evp::sunstudy::OcclusionAccumulator& accumulator,
                                                  double wallMs) {
        const size_t rays = accumulator.LastRayCount ();
        archviz::ArchVizLog (
            "pipeline: stage=sun-step snapshot=" + std::to_string (snapshotId) + " step=" + std::to_string (step + 1) +
            "/" + std::to_string (accumulator.StepCount ()) +
            " samples=" + std::to_string (accumulator.SampleCount ()) +
            " dirty=" + std::to_string (accumulator.ActiveSampleCount ()) + " rays=" + std::to_string (rays) +
            " rayPacketBytes=" + std::to_string (rays * (3 * sizeof (double) + sizeof (uint32_t) + 1)) +
            " compactMs=" + std::to_string (accumulator.LastCompactMilliseconds ()) +
            " traceMs=" + std::to_string (accumulator.LastTraceMilliseconds ()) + " wallMs=" + std::to_string (wallMs));
    });
}

} // namespace geomsrv
