#include "SunStudy/SunStudyWinding.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace evp::sunstudy {
namespace {

// A welded position, as the integer lattice the tolerance defines. Two corners
// closer than the tolerance land on the same key and so count as one point.
struct Lattice {
    int64_t x = 0, y = 0, z = 0;

    bool operator< (const Lattice& other) const
    {
        if (x != other.x)
            return x < other.x;
        if (y != other.y)
            return y < other.y;
        return z < other.z;
    }
};

Lattice Quantise (const double* p, double tolerance)
{
    const double inverse = 1.0 / tolerance;
    Lattice key;
    key.x = static_cast<int64_t> (std::llround (p[0] * inverse));
    key.y = static_cast<int64_t> (std::llround (p[1] * inverse));
    key.z = static_cast<int64_t> (std::llround (p[2] * inverse));
    return key;
}

// An undirected edge between two welded corners. Ordered so the two directions
// a shared edge is traversed from its two faces collapse to one key.
struct EdgeKey {
    Lattice a, b;

    bool operator< (const EdgeKey& other) const
    {
        if (a < other.a)
            return true;
        if (other.a < a)
            return false;
        return b < other.b;
    }
};

EdgeKey MakeEdge (const Lattice& p, const Lattice& q)
{
    EdgeKey edge;
    if (q < p) {
        edge.a = q;
        edge.b = p;
    }
    else {
        edge.a = p;
        edge.b = q;
    }
    return edge;
}

} // namespace

double SignedVolume (const double* vertices, const uint32_t* triangles, const uint32_t* faceList, size_t faceCount)
{
    if (faceCount == 0)
        return 0.0;

    // See the header: centre first, or the sign is noise at survey coordinates.
    double centre[3] = { 0.0, 0.0, 0.0 };
    size_t corners = 0;
    for (size_t f = 0; f < faceCount; ++f) {
        const uint32_t face = faceList[f];
        for (int c = 0; c < 3; ++c) {
            const double* p = vertices + static_cast<size_t> (triangles[face * 3 + c]) * 3;
            centre[0] += p[0];
            centre[1] += p[1];
            centre[2] += p[2];
            ++corners;
        }
    }
    if (corners == 0)
        return 0.0;
    centre[0] /= static_cast<double> (corners);
    centre[1] /= static_cast<double> (corners);
    centre[2] /= static_cast<double> (corners);

    double total = 0.0;
    for (size_t f = 0; f < faceCount; ++f) {
        const uint32_t face = faceList[f];
        const double* p0 = vertices + static_cast<size_t> (triangles[face * 3 + 0]) * 3;
        const double* p1 = vertices + static_cast<size_t> (triangles[face * 3 + 1]) * 3;
        const double* p2 = vertices + static_cast<size_t> (triangles[face * 3 + 2]) * 3;

        const double a[3] = { p0[0] - centre[0], p0[1] - centre[1], p0[2] - centre[2] };
        const double b[3] = { p1[0] - centre[0], p1[1] - centre[1], p1[2] - centre[2] };
        const double c[3] = { p2[0] - centre[0], p2[1] - centre[1], p2[2] - centre[2] };

        const double cross[3] = { b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0] };
        total += a[0] * cross[0] + a[1] * cross[1] + a[2] * cross[2];
    }
    return total / 6.0;
}

std::vector<uint32_t> OrientOutward (const double* vertices, size_t vertexCount, const uint32_t* triangles,
                                     size_t faceCount, const uint32_t* groups, WindingReport& report,
                                     double weldTolerance)
{
    report = WindingReport ();

    std::vector<uint32_t> oriented;
    if (vertices == nullptr || triangles == nullptr || faceCount == 0)
        return oriented;
    if (!(weldTolerance > 0.0))
        weldTolerance = 1e-4;

    oriented.assign (triangles, triangles + faceCount * 3);

    // Group the faces. Ids may be sparse, so this is a map rather than a table.
    std::map<uint32_t, std::vector<uint32_t>> byGroup;
    for (size_t f = 0; f < faceCount; ++f) {
        const uint32_t i0 = triangles[f * 3 + 0];
        const uint32_t i1 = triangles[f * 3 + 1];
        const uint32_t i2 = triangles[f * 3 + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            continue; // an unusable face cannot help prove a group closed
        byGroup[(groups != nullptr) ? groups[f] : 0u].push_back (static_cast<uint32_t> (f));
    }
    report.groups = byGroup.size ();

    for (const auto& entry : byGroup) {
        const std::vector<uint32_t>& faces = entry.second;

        // Closed means every undirected edge is shared by exactly two faces.
        std::map<EdgeKey, int> edges;
        for (uint32_t face : faces) {
            Lattice corner[3];
            for (int c = 0; c < 3; ++c)
                corner[c] = Quantise (vertices + static_cast<size_t> (triangles[face * 3 + c]) * 3, weldTolerance);
            ++edges[MakeEdge (corner[0], corner[1])];
            ++edges[MakeEdge (corner[1], corner[2])];
            ++edges[MakeEdge (corner[2], corner[0])];
        }

        bool closed = true;
        for (const auto& edge : edges) {
            if (edge.second != 2) {
                closed = false;
                break;
            }
        }
        if (!closed)
            continue; // open surface: "outward" is undefined, so leave it alone
        ++report.closed;

        if (SignedVolume (vertices, triangles, faces.data (), faces.size ()) >= 0.0)
            continue;

        ++report.flipped;
        for (uint32_t face : faces) {
            std::swap (oriented[face * 3 + 1], oriented[face * 3 + 2]);
            ++report.flippedTriangles;
        }
    }

    return oriented;
}

} // namespace evp::sunstudy
