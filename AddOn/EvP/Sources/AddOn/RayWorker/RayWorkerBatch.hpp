#ifndef EVP_RAYWORKER_RAYWORKERBATCH_HPP
#define EVP_RAYWORKER_RAYWORKERBATCH_HPP

// RayWorker/RayWorkerBatch -- what every hardware backend does on the CPU
// around its traversal, so DXR and OptiX cannot drift apart on it:
//
// - the shared local origin, subtracted in DOUBLE before anything is f32;
// - back-facing samples culled in double (incidence <= 0, the accumulator's
//   own test), so the GPU never decides a self-shadowed sample;
// - empty scenes, zero directions and empty [tmin, tmax] intervals are clear
//   without a ray, as QueryEngine::Occluded returns them;
// - the result packed into the accumulator's bitset (sample-major, bit
//   `step % 64`), each traced step marked resolved.
//
// The backend only answers "is ray r blocked" for the rays this builds.

#include "RayWorker/RayWorkerPacket.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace evp::rayworker {

// Shared origin of a scene: the centre of its vertex bounds. False when a
// vertex is not finite.
bool SceneOrigin (const packet::Scene& scene, double origin[3]);
// The scene's vertices relative to `origin`, as f32 xyz.
std::vector<float> LocalVertices (const packet::Scene& scene, const double origin[3]);

struct RayBatch {
    std::vector<float> positions;   // every sample, local f32 xyz
    std::vector<uint32_t> rays;     // (sample, slot) pairs to trace
    std::vector<float> directions;  // unit f32 xyz per slot
    std::vector<uint32_t> slotStep; // the request step of each slot
    float tmin = 0.0f;
    float tmax = 0.0f; // FLT_MAX when unbounded

    size_t RayCount () const
    {
        return rays.size () / 2;
    }
};

// Build the rays for `request` against a scene of `sceneTriangles` triangles
// placed at `origin`. `result` is sized and already holds the bits of every
// sample that is clear without a ray.
RayBatch BuildRayBatch (const packet::Request& request, size_t sceneTriangles, const double origin[3],
                        packet::Result& result);

// Commit the backend's answer: `occluded[r]` is 1 blocked / 0 clear for ray r.
// False (with `error`) when a word is neither, or the count does not match;
// then nothing in `result` may be published.
bool ApplyOcclusion (const RayBatch& batch, const std::vector<uint32_t>& occluded, const packet::Request& request,
                     packet::Result& result, std::string& error);

} // namespace evp::rayworker

#endif
