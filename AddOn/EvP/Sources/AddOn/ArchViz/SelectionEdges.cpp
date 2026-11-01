#include "ArchViz/SelectionEdges.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace geomsrv {
namespace archviz {
namespace {

// A position's identity: its three floats' exact bits. The upload converts each source double to
// float once, so two corners at one source point carry identical bits; a tolerance would only
// add a way to weld two genuinely different corners.
struct PositionKey {
    uint32_t bits[3];
    bool operator== (const PositionKey& other) const
    {
        return std::memcmp (bits, other.bits, sizeof (bits)) == 0;
    }
};

struct PositionKeyHash {
    size_t operator() (const PositionKey& key) const
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t b : key.bits)
            h = (h ^ b) * 1099511628211ull;
        return size_t (h);
    }
};

// The two faces either side of an edge; `count` keeps going past two so a non-manifold edge is
// known as one.
struct EdgeFaces {
    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t count = 0;
};

uint64_t EdgeKey (uint32_t a, uint32_t b)
{
    return a < b ? (uint64_t (a) << 32) | b : (uint64_t (b) << 32) | a;
}

} // namespace

std::vector<uint8_t> BuildSelectionEdgeMasks (const std::vector<float>& positions, const std::vector<uint32_t>& indices,
                                              const std::vector<uint32_t>& wireMasks, float creaseDegrees)
{
    const size_t triCount = indices.size () / 3;
    std::vector<uint8_t> masks (triCount, 0);
    const size_t vertexCount = positions.size () / 3;
    if (triCount == 0 || vertexCount == 0)
        return masks;

    // 1. One id per distinct POSITION (see the header's ⚠️).
    std::vector<uint32_t> positionId (vertexCount);
    {
        std::unordered_map<PositionKey, uint32_t, PositionKeyHash> ids;
        ids.reserve (vertexCount);
        for (size_t v = 0; v < vertexCount; ++v) {
            PositionKey key;
            std::memcpy (key.bits, &positions[v * 3], sizeof (key.bits));
            positionId[v] = ids.emplace (key, uint32_t (ids.size ())).first->second;
        }
    }

    // 2. Each triangle's unit normal; a zero vector for a sliver, which never makes a crease.
    std::vector<float> normals (triCount * 3, 0.0f);
    auto corner = [&] (size_t tri, int k) -> const float* {
        const uint32_t v = indices[tri * 3 + k];
        return v < vertexCount ? &positions[size_t (v) * 3] : nullptr;
    };
    for (size_t t = 0; t < triCount; ++t) {
        const float* a = corner (t, 0);
        const float* b = corner (t, 1);
        const float* c = corner (t, 2);
        if (a == nullptr || b == nullptr || c == nullptr)
            continue;
        const float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const float w[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        const float n[3] = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
        const float length = std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length > 0.0f && std::isfinite (length)) {
            for (int k = 0; k < 3; ++k)
                normals[t * 3 + k] = n[k] / length;
        }
    }

    // 3. Every edge's faces, by position.
    static constexpr int kEdgeCorners[3][2] = { { 1, 2 }, { 2, 0 }, { 0, 1 } };
    auto edgeOf = [&] (size_t tri, int edge) {
        const uint32_t a = indices[tri * 3 + kEdgeCorners[edge][0]];
        const uint32_t b = indices[tri * 3 + kEdgeCorners[edge][1]];
        return a < vertexCount && b < vertexCount ? EdgeKey (positionId[a], positionId[b]) : ~uint64_t (0);
    };
    std::unordered_map<uint64_t, EdgeFaces> faces;
    faces.reserve (triCount * 2);
    for (size_t t = 0; t < triCount; ++t) {
        for (int edge = 0; edge < 3; ++edge) {
            EdgeFaces& f = faces[edgeOf (t, edge)];
            if (f.count == 0)
                f.first = uint32_t (t);
            else if (f.count == 1)
                f.second = uint32_t (t);
            ++f.count;
        }
    }

    // 4. Keep an edge the wireframe draws that is a border or a crease.
    const float creaseCos = std::cos (creaseDegrees * 3.14159265358979f / 180.0f);
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t drawable = wireMasks.empty () ? 0x7u : (t < wireMasks.size () ? wireMasks[t] & 0x7u : 0x7u);
        for (int edge = 0; edge < 3; ++edge) {
            if ((drawable & (1u << edge)) == 0)
                continue;
            const uint64_t key = edgeOf (t, edge);
            if (key == ~uint64_t (0))
                continue;
            const EdgeFaces& f = faces[key];
            bool keep = f.count != 2;
            if (!keep) {
                const size_t other = f.first == t ? f.second : f.first;
                const float* n0 = &normals[t * 3];
                const float* n1 = &normals[other * 3];
                const float n0Length = n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2];
                const float n1Length = n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2];
                if (n0Length > 0.5f && n1Length > 0.5f)
                    keep = n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < creaseCos;
            }
            if (keep)
                masks[t] |= uint8_t (1u << edge);
        }
    }
    return masks;
}

} // namespace archviz
} // namespace geomsrv
