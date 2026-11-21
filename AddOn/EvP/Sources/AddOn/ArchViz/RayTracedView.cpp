// ArchViz/RayTracedView -- see the header.

#include "ArchViz/RayTracedView.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace geomsrv {
namespace archviz {

RayTracedGeometry BuildRayTracedGeometry (const std::vector<RayTracedSource>& sources)
{
    RayTracedGeometry out;
    std::unordered_map<int32_t, uint32_t> slotOf;
    auto slot = [&] (int32_t material) {
        const auto found = slotOf.find (material);
        if (found != slotOf.end ())
            return found->second;
        const uint32_t s = uint32_t (out.materials.size ());
        out.materials.push_back (material);
        slotOf.emplace (material, s);
        return s;
    };

    std::vector<uint32_t> perTriangle;
    for (const RayTracedSource& source : sources) {
        if (source.mesh == nullptr)
            continue;
        const SectionMesh& mesh = *source.mesh;
        const size_t vertexCount = mesh.positions.size () / 3;
        const size_t triangleCount = mesh.indices.size () / 3;
        if (vertexCount == 0 || triangleCount == 0)
            continue;
        // Default first, then each range over its triangles.
        perTriangle.assign (triangleCount, slot (-1));
        for (const MaterialRange& r : source.ranges) {
            const size_t first = r.firstIndex / 3;
            const size_t count = r.indexCount / 3;
            for (size_t t = first; t < std::min (first + count, triangleCount); ++t)
                perTriangle[t] = slot (r.material);
        }
        const uint32_t base = uint32_t (out.positions.size () / 3);
        out.positions.insert (out.positions.end (), mesh.positions.begin (), mesh.positions.begin () + vertexCount * 3);
        if (mesh.uvs.size () >= vertexCount * 2)
            out.uvs.insert (out.uvs.end (), mesh.uvs.begin (), mesh.uvs.begin () + vertexCount * 2);
        else
            out.uvs.resize (out.uvs.size () + vertexCount * 2, 0.0f);
        if (mesh.normals.size () >= vertexCount * 3)
            out.normals.insert (out.normals.end (), mesh.normals.begin (), mesh.normals.begin () + vertexCount * 3);
        else
            out.normals.resize (out.normals.size () + vertexCount * 3, 0.0f);
        for (size_t t = 0; t < triangleCount; ++t) {
            const uint32_t a = mesh.indices[t * 3], b = mesh.indices[t * 3 + 1], c = mesh.indices[t * 3 + 2];
            if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
                continue;
            out.indices.insert (out.indices.end (), { base + a, base + b, base + c });
            out.triangleMaterial.push_back (perTriangle[t]);
        }
    }
    return out;
}

RayTracedTextureSlots AssignTextureSlots (const std::vector<uint64_t>& keys, uint32_t capacity)
{
    RayTracedTextureSlots out;
    out.slotOf.assign (keys.size (), -1);
    std::unordered_map<uint64_t, int32_t> slotOfKey;
    for (size_t m = 0; m < keys.size (); ++m) {
        if (keys[m] == 0)
            continue;
        const auto found = slotOfKey.find (keys[m]);
        if (found != slotOfKey.end ()) {
            out.slotOf[m] = found->second;
            continue;
        }
        if (out.keys.size () >= capacity) {
            ++out.overflow;
            continue;
        }
        const int32_t slot = int32_t (out.keys.size ());
        out.keys.push_back (keys[m]);
        slotOfKey.emplace (keys[m], slot);
        out.slotOf[m] = slot;
    }
    return out;
}

bool InvertMatrix4 (const float in[16], float out[16])
{
    double m[16], inv[16];
    for (int k = 0; k < 16; ++k)
        m[k] = double (in[k]);
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite (det) || std::fabs (det) < 1e-300)
        return false;
    for (int k = 0; k < 16; ++k)
        out[k] = float (inv[k] / det);
    return true;
}

void RayTracedBudget::Measured (double milliseconds, uint32_t samples)
{
    if (samples == 0 || !(milliseconds > 0.0) || !std::isfinite (milliseconds))
        return;
    const double perSample = milliseconds / double (samples);
    // A moving average: one slow frame (the OS, a shader cache miss) does not halve the count.
    msPerSample_ = msPerSample_ <= 0.0 ? perSample : msPerSample_ * 0.7 + perSample * 0.3;
}

uint32_t RayTracedBudget::SamplesThisFrame (float targetMs)
{
    uint32_t count = 1;
    if (targetMs > 0.0f && msPerSample_ > 0.0)
        count = uint32_t (std::clamp (std::floor (double (targetMs) / msPerSample_), 1.0, double (kMaxPerFrame)));
    count = std::min (count, last_ * 2);
    last_ = count;
    return count;
}

RayTracedStep RayTracedAccumulator::Advance (uint64_t key, uint32_t maxSamples, uint32_t perFrame)
{
    RayTracedStep step;
    if (!haveKey_ || key != key_) {
        key_ = key;
        haveKey_ = true;
        still_ = false;
        samples_ = 0;
        return step; // moving: the raster image
    }
    if (!still_) {
        still_ = true;
        step.firstSample = 0;
        step.trace = std::min (std::max (kFirstBurst, perFrame), std::max (maxSamples, 1u));
        samples_ = step.trace;
        step.show = true;
        return step;
    }
    step.show = true;
    if (samples_ >= maxSamples)
        return step; // converged: re-show
    step.firstSample = samples_;
    step.trace = std::min (std::max (perFrame, 1u), maxSamples - samples_);
    samples_ += step.trace;
    return step;
}

void RayTracedAccumulator::Reset ()
{
    *this = RayTracedAccumulator {};
}

uint64_t HashBytes (uint64_t h, const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*> (data);
    for (size_t i = 0; i < size; ++i)
        h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

const char* RayTracedStateName (RayTracedState state)
{
    switch (state) {
        case RayTracedState::Off:
            return "off";
        case RayTracedState::Unavailable:
            return "unavailable";
        case RayTracedState::Moving:
            return "moving -- raster shown";
        case RayTracedState::Accumulating:
            return "accumulating";
        case RayTracedState::Converged:
            return "converged";
    }
    return "?";
}

} // namespace archviz
} // namespace geomsrv
