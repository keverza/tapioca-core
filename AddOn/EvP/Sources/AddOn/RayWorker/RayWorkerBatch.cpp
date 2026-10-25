#include "RayWorker/RayWorkerBatch.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace evp::rayworker {

bool SceneOrigin (const packet::Scene& scene, double origin[3])
{
    origin[0] = origin[1] = origin[2] = 0.0;
    const size_t vertexCount = scene.vertices.size () / 3;
    if (vertexCount == 0)
        return true;
    double lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        lo[a] = std::numeric_limits<double>::max ();
        hi[a] = -std::numeric_limits<double>::max ();
    }
    for (size_t v = 0; v < vertexCount; ++v)
        for (int a = 0; a < 3; ++a) {
            const double x = scene.vertices[v * 3 + a];
            if (!std::isfinite (x))
                return false;
            lo[a] = std::min (lo[a], x);
            hi[a] = std::max (hi[a], x);
        }
    for (int a = 0; a < 3; ++a)
        origin[a] = 0.5 * (lo[a] + hi[a]);
    return true;
}

std::vector<float> LocalVertices (const packet::Scene& scene, const double origin[3])
{
    std::vector<float> local (scene.vertices.size ());
    for (size_t i = 0; i < local.size (); ++i)
        local[i] = static_cast<float> (scene.vertices[i] - origin[i % 3]);
    return local;
}

RayBatch BuildRayBatch (const packet::Request& request, size_t sceneTriangles, const double origin[3],
                        packet::Result& result)
{
    const size_t samples = request.positions.size () / 3;
    const size_t totalSteps = static_cast<size_t> (request.totalSteps);
    const size_t wordsPerSample = (totalSteps + 63) / 64;
    result.sampleCount = samples;
    result.stepCount = request.totalSteps;
    result.bits.assign (samples * wordsPerSample, 0u);
    result.resolved.assign (totalSteps, 0u);

    // QueryEngine::Occluded's interval: tmin clamps at 0, tmax <= 0 is
    // unbounded, and an empty interval holds nothing (clear, not shadowed).
    const double minT = request.tmin > 0.0 ? request.tmin : 0.0;
    const double maxT = request.tmax > 0.0 ? request.tmax : std::numeric_limits<double>::max ();
    RayBatch batch;
    batch.tmin = static_cast<float> (minT);
    batch.tmax = request.tmax > 0.0 ? static_cast<float> (maxT) : std::numeric_limits<float>::max ();
    const bool emptyInterval = maxT <= minT;

    for (size_t i = 0; i < request.steps.size (); ++i) {
        const size_t step = request.steps[i];
        const double* d = &request.directions[i * 3];
        const double length = std::sqrt (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const bool alwaysClear = sceneTriangles == 0 || length <= 0.0 || emptyInterval;
        const uint32_t slot = static_cast<uint32_t> (batch.slotStep.size ());
        for (size_t s = 0; s < samples; ++s) {
            if (!request.normals.empty ()) {
                const double* n = &request.normals[s * 3];
                if (n[0] * d[0] + n[1] * d[1] + n[2] * d[2] <= 0.0)
                    continue; // facing away: self-shadowed, bit stays clear
            }
            if (alwaysClear)
                result.bits[s * wordsPerSample + step / 64] |= 1ull << (step % 64);
            else {
                batch.rays.push_back (static_cast<uint32_t> (s));
                batch.rays.push_back (slot);
            }
        }
        if (!alwaysClear) {
            for (int a = 0; a < 3; ++a)
                batch.directions.push_back (static_cast<float> (d[a] / length));
            batch.slotStep.push_back (static_cast<uint32_t> (step));
        }
    }
    if (!batch.rays.empty ()) {
        batch.positions.resize (samples * 3);
        for (size_t s = 0; s < samples; ++s)
            for (int a = 0; a < 3; ++a)
                batch.positions[s * 3 + a] = static_cast<float> (request.positions[s * 3 + a] - origin[a]);
    }
    return batch;
}

bool ApplyOcclusion (const RayBatch& batch, const std::vector<uint32_t>& occluded, const packet::Request& request,
                     packet::Result& result, std::string& error)
{
    if (occluded.size () != batch.RayCount ()) {
        error = "the GPU returned " + std::to_string (occluded.size ()) + " words for " +
                std::to_string (batch.RayCount ()) + " rays";
        return false;
    }
    const size_t wordsPerSample = (static_cast<size_t> (request.totalSteps) + 63) / 64;
    for (size_t r = 0; r < occluded.size (); ++r) {
        if (occluded[r] > 1) {
            error = "the GPU wrote an occlusion word that is neither 0 nor 1";
            return false;
        }
        if (occluded[r] != 0)
            continue;
        const size_t s = batch.rays[r * 2];
        const size_t step = batch.slotStep[batch.rays[r * 2 + 1]];
        result.bits[s * wordsPerSample + step / 64] |= 1ull << (step % 64);
    }
    for (const uint32_t step : request.steps)
        result.resolved[step] = 1;
    return true;
}

} // namespace evp::rayworker
