#include "NativeCommands/SunStudyPreparation.hpp"
#include "NativeCommands/SunStudyCommandsSupport.hpp"
#include "SunStudy/SunStudyCoreSelection.hpp"
#include "SunStudy/SunStudyReuse.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/SunStudyGpuTraversal.hpp"
#include "RayWorker/RayWorkerTraversal.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

namespace geomsrv {

namespace {

constexpr size_t kUnknownCount = static_cast<size_t> (-1);

struct SunRunTotals {
    std::string scene;
    bool started = false;
    size_t steps = 0;
    size_t rays = 0;
    size_t hits = 0;
    bool hitsKnown = true;
    size_t threads = 0;
    double traceMs = 0.0;
    double traceCpuMs = 0.0;
    bool cpuKnown = true;
    double wallMs = 0.0;
    std::chrono::steady_clock::time_point first;
};

std::string IndexFields (const char* role, const QueryEngine* index)
{
    const std::string name (role);
    if (index == nullptr)
        return " " + name + "Triangles=0";
    return " " + name + "Triangles=" + std::to_string (index->TriangleCount ()) + " " + name +
           "BvhNodes=" + std::to_string (index->BvhNodeCount ()) + " " + name +
           "BvhDepth=" + std::to_string (index->BvhDepth ());
}

// The study's occluder indexes as traced: the analysis one first, then the
// context one a clear ray ALSO walks (SunStudyPartitionTraversal).
std::string SunRunSceneFields (const evp::sunstudy::SunStudyOccluders* occluders)
{
    if (occluders == nullptr)
        return " occluders=none";
    return IndexFields ("analysis", occluders->analysis.get ()) + IndexFields ("context", occluders->context.get ()) +
           " contextReused=" + std::to_string (occluders->contextReused);
}

std::string Count (size_t value)
{
    return value == kUnknownCount ? std::string ("?") : std::to_string (value);
}

void NoteSunRunStep (SunRunTotals& totals, uint64_t snapshotId, const char* core, size_t samples, size_t stepCount,
                     size_t resolved, size_t rays, size_t hits, size_t threads, double traceMs, double traceCpuMs,
                     double wallMs)
{
    const std::string study = " snapshot=" + std::to_string (snapshotId) + " core=" + core;
    if (!totals.started) {
        totals.started = true;
        totals.first = std::chrono::steady_clock::now ();
        archviz::ArchVizLog ("pipeline: stage=sun-run-start" + study + " steps=" + std::to_string (stepCount) +
                             " samples=" + std::to_string (samples) + " firstStep=" + std::to_string (resolved) +
                             totals.scene);
    }
    ++totals.steps;
    totals.rays += rays;
    totals.hitsKnown = totals.hitsKnown && hits != kUnknownCount;
    if (hits != kUnknownCount)
        totals.hits += hits;
    if (threads != kUnknownCount)
        totals.threads = (std::max) (totals.threads, threads);
    totals.traceMs += traceMs;
    totals.cpuKnown = totals.cpuKnown && traceCpuMs >= 0.0;
    totals.traceCpuMs += (std::max) (0.0, traceCpuMs);
    totals.wallMs += wallMs;
    if (resolved < stepCount)
        return;
    const double elapsedMs =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - totals.first).count ();
    const double usPerRay = totals.rays == 0 ? 0.0 : 1000.0 * totals.traceMs / double (totals.rays);
    const size_t hitsTotal = totals.hitsKnown ? totals.hits : kUnknownCount;
    archviz::ArchVizLog ("pipeline: stage=sun-run-end" + study + " steps=" + std::to_string (totals.steps) + "/" +
                         std::to_string (stepCount) + " samples=" + std::to_string (samples) +
                         " rays=" + std::to_string (totals.rays) + " hitRays=" + Count (hitsTotal) +
                         " clearRays=" + Count (totals.hitsKnown ? totals.rays - totals.hits : kUnknownCount) +
                         " threads=" + (totals.threads == 0 ? std::string ("?") : std::to_string (totals.threads)) +
                         " traceMs=" + std::to_string (totals.traceMs) +
                         " traceCpuMs=" + (totals.cpuKnown ? std::to_string (totals.traceCpuMs) : std::string ("?")) +
                         " wallMs=" + std::to_string (totals.wallMs) + " elapsedMs=" + std::to_string (elapsedMs) +
                         " usPerRay=" + std::to_string (usPerRay) + totals.scene);
}

} // namespace

void LogSunStudySurfaceSampling (uint64_t snapshot, const evp::sunstudy::SurfaceSamplingResult& result)
{
    archviz::ArchVizLog ("pipeline: stage=sun-grid-reuse snapshot=" + std::to_string (snapshot) +
                         " reusedMeshes=" + std::to_string (result.reusedMeshes) +
                         " rebuiltMeshes=" + std::to_string (result.rebuiltMeshes) +
                         " reusedGridSamples=" + std::to_string (result.reusedSamples) + " generatedSamples=" +
                         std::to_string (result.generatedSamples) + " valid=" + std::to_string (result.valid));
}

void LogSunStudyCylinderMembership (uint64_t snapshot, size_t analysis, const std::string& entered,
                                    const std::string& left)
{
    archviz::ArchVizLog ("sun cylinder: snapshot=" + std::to_string (snapshot) +
                         " analysis=" + std::to_string (analysis) + " entered=[" + entered + "] left=[" + left + "]");
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

void StartSunStudyCore (evp::sunstudy::StudyRecord& record, const Snapshot& snapshot,
                        const evp::sunstudy::StudyRecord* reuseSource, const evp::sunstudy::StudyInputs& inputs)
{
    // Timed, with the sync below and the reuse in FinishSunStudyPreparation: the
    // sun-reuse-seed stage is all three, and on a 1.6M-sample refresh it is a
    // second of every edit (#97).
    const auto started = std::chrono::steady_clock::now ();
    const auto msSince = [] (std::chrono::steady_clock::time_point since) {
        return std::to_string (
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - since).count ());
    };
    if (evp::sunstudy::SelectedSunStudyCore () == evp::sunstudy::SunStudyCoreKind::Rust) {
        const auto limits =
            sunstudysupport::MachineAnalysisLimits (record.series.StepCount (), record.IsPatchDomain ());
        std::string error;
        const bool attached = evp::sunstudy::AttachRustStudyCore (
            record, snapshot, reuseSource, record.IsPatchDomain () ? limits.maxAtlasDimension : 8192u, error);
        archviz::ArchVizLog ("pipeline: stage=sun-core snapshot=" + std::to_string (snapshot.id) +
                             " core=" + (attached ? "rust" : "cpp") + " attachMs=" + msSince (started) +
                             (attached ? "" : " reason=" + error));
    }
    const auto syncing = std::chrono::steady_clock::now ();
    evp::sunstudy::SyncStudy (record, inputs);
    archviz::ArchVizLog ("pipeline: stage=sun-core-sync snapshot=" + std::to_string (snapshot.id) +
                         " wallMs=" + msSince (syncing));
}

void FinishSunStudyPreparation (evp::sunstudy::StudyRecord& record, std::shared_ptr<const Snapshot> snapshot,
                                const evp::sunstudy::StudyRecord* reuseSource, const std::atomic<bool>* cancelled,
                                std::shared_ptr<const QueryEngine> occluders)
{
    // Both producers choose explicitly. CPU remains the public default; FP64
    // GPU packets retain exact fallback and lazy device creation.
    //
    // ⚠️ TAPIOCA_SUNSTUDY_TRAVERSAL=dxr|optix IS A DEVELOPER OVERRIDE, AND WINS WHEN
    // SET: the follower re-issues `backend` from the stored metadata, so an
    // explicit request cannot be told from a rerun here yet (the G1 selector
    // carries the request). The CPU partition traversal stays underneath as the
    // fallback.
    const std::string worker = evp::rayworker::RequestedWorkerTraversal ();
    if (!worker.empty () && record.occluders != nullptr) {
        const auto scene =
            evp::rayworker::SceneFromIndices (record.occluders->analysis.get (), record.occluders->context.get ());
        record.traversal = std::make_shared<evp::rayworker::RayWorkerTraversal> (
            record.traversal, evp::rayworker::WorkerChannel (worker), scene, worker,
            [] (const std::string& line) { archviz::ArchVizLog (line); });
        archviz::ArchVizLog ("pipeline: stage=sun-traversal snapshot=" + std::to_string (record.snapshotId) +
                             " requested=" + worker + " backendParam=" + record.backend +
                             " triangles=" + std::to_string (scene->triangles.size () / 3));
    }
    else if (record.backend == "gpu") {
        const auto* previousGpu =
            reuseSource != nullptr ? dynamic_cast<const archviz::SunStudyGpuTraversal*> (reuseSource->traversal.get ())
                                   : nullptr;
        record.traversal = std::make_shared<archviz::SunStudyGpuTraversal> (
            std::move (occluders), record.occluders != nullptr ? record.occluders->context : nullptr, previousGpu);
    }
    // Every producer, so a follower replacement shows it kept the adopted
    // study's choice (#54); what ran is reported by the traversal itself.
    archviz::ArchVizLog ("pipeline: stage=sun-backend snapshot=" + std::to_string (record.snapshotId) +
                         " requested=" + record.backend + " automatic=" + (cancelled != nullptr ? "1" : "0"));
    if (record.occluders != nullptr && record.occluders->context != nullptr)
        archviz::ArchVizLog ("pipeline: stage=sun-context-index snapshot=" + std::to_string (record.snapshotId) +
                             " meshes=" + std::to_string (record.occluders->context->MeshCount ()) +
                             " triangles=" + std::to_string (record.occluders->context->TriangleCount ()) +
                             " reused=" + std::to_string (record.occluders->contextReused));
    record.snapshot = std::move (snapshot);
    evp::sunstudy::SetSampleMeshes (record);
    const std::atomic<bool> notCancelled { false };
    // Manual benchmarks always trace a fresh day, even if grids/atlases reuse.
    const auto reusing = std::chrono::steady_clock::now ();
    if (reuseSource != nullptr && cancelled != nullptr)
        evp::sunstudy::ReuseUnaffectedSamples (*reuseSource, record, cancelled != nullptr ? *cancelled : notCancelled);
    const double reuseMs =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - reusing).count ();
    const uint64_t snapshotId = record.snapshotId;
    archviz::ArchVizLog (
        "pipeline: stage=sun-reuse snapshot=" + std::to_string (snapshotId) + " samples=" +
        std::to_string (record.positions.size () / 3) + " reused=" + std::to_string (record.reusedSamples) +
        " dirty=" + std::to_string (record.positions.size () / 3 - record.reusedSamples) +
        " reusedSampleSteps=" + std::to_string (record.reusedSampleSteps) + " dirtySampleSteps=" +
        std::to_string (record.positions.size () / 3 * record.series.StepCount () - record.reusedSampleSteps) +
        " gridM=" + std::to_string (record.gridSpacing) + " wallMs=" + std::to_string (reuseMs));
    // One study's totals, carried from step to step: sun-run-start on its first
    // traced step, sun-run-end on its last (#97 P0). The scene's shape goes on
    // those two lines, not on every sun-step, because it is fixed per study.
    auto totals = std::make_shared<SunRunTotals> ();
    totals->scene = SunRunSceneFields (record.occluders.get ());
    if (record.rustCore != nullptr) {
        record.rustStepObserver = [snapshotId, totals] (size_t step, const TapiocaOcclusionState& state,
                                                        double wallMs) {
            archviz::ArchVizLog (
                "pipeline: stage=sun-step snapshot=" + std::to_string (snapshotId) +
                " core=rust step=" + std::to_string (step + 1) + "/" + std::to_string (state.stepCount) +
                " samples=" + std::to_string (state.sampleCount) + " dirty=" + std::to_string (state.activeSamples) +
                " rays=" + std::to_string (state.lastRayCount) + " compactMs=" + std::to_string (state.compactMs) +
                " traceMs=" + std::to_string (state.traceMs) + " wallMs=" + std::to_string (wallMs));
            NoteSunRunStep (*totals, snapshotId, "rust", state.sampleCount, state.stepCount, state.resolvedSteps,
                            state.lastRayCount, kUnknownCount, kUnknownCount, state.traceMs, -1.0, wallMs);
        };
        return;
    }
    record.session.SetStepObserver (
        [snapshotId, totals] (size_t step, const evp::sunstudy::OcclusionAccumulator& accumulator, double wallMs) {
            const size_t rays = accumulator.LastRayCount ();
            const size_t hits = accumulator.LastHitCount ();
            const size_t threads = evp::sunstudy::ChooseThreadCount (rays, accumulator.LastMaxParallel ());
            archviz::ArchVizLog (
                "pipeline: stage=sun-step snapshot=" + std::to_string (snapshotId) +
                " step=" + std::to_string (step + 1) + "/" + std::to_string (accumulator.StepCount ()) +
                " samples=" + std::to_string (accumulator.SampleCount ()) +
                " dirty=" + std::to_string (accumulator.ActiveSampleCount ()) + " rays=" + std::to_string (rays) +
                " hitRays=" + std::to_string (hits) + " clearRays=" + std::to_string (rays - hits) + " maxParallel=" +
                std::to_string (accumulator.LastMaxParallel ()) + " threads=" + std::to_string (threads) +
                " rayPacketBytes=" + std::to_string (rays * (3 * sizeof (double) + sizeof (uint32_t) + 1)) +
                " compactMs=" + std::to_string (accumulator.LastCompactMilliseconds ()) +
                " traceMs=" + std::to_string (accumulator.LastTraceMilliseconds ()) + " traceCpuMs=" +
                std::to_string (accumulator.LastTraceCpuMilliseconds ()) + " wallMs=" + std::to_string (wallMs));
            NoteSunRunStep (*totals, snapshotId, "cpp", accumulator.SampleCount (), accumulator.StepCount (),
                            accumulator.ResolvedStepCount (), rays, hits, threads, accumulator.LastTraceMilliseconds (),
                            accumulator.LastTraceCpuMilliseconds (), wallMs);
        });
}

} // namespace geomsrv
