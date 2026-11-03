#include "Edit/EditMeshClean.hpp"

#include "Geometry/TriangleDegeneracy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>

namespace tapioca::geometry {
namespace {

struct CellKey {
    int64_t x = 0, y = 0, z = 0;
    bool operator< (const CellKey& o) const
    {
        if (x != o.x)
            return x < o.x;
        if (y != o.y)
            return y < o.y;
        return z < o.z;
    }
};

CellKey CellFor (const double* p, double tolerance)
{
    // floor, not truncation: negative coordinates must land in the cell that
    // contains the point or the neighbour search misses half the space.
    return { static_cast<int64_t> (std::floor (p[0] / tolerance)), static_cast<int64_t> (std::floor (p[1] / tolerance)),
             static_cast<int64_t> (std::floor (p[2] / tolerance)) };
}

// Sorted-vertex-triangle coincidence key. Orientation is NOT part of the key:
// the map value counts even/odd members so a both-orientation class (an
// internal wall) is detected exactly (the same convention MeshStats pins).
struct ClassKey {
    std::array<uint32_t, 3> verts {};
    bool operator< (const ClassKey& o) const
    {
        return verts < o.verts;
    }
};

ClassKey KeyFor (uint32_t a, uint32_t b, uint32_t c)
{
    ClassKey key;
    key.verts = { a, b, c };
    std::sort (key.verts.begin (), key.verts.end ());
    return key;
}

bool IsEvenOrientation (const ClassKey& key, uint32_t a, uint32_t b, uint32_t c)
{
    return (key.verts[0] == a && key.verts[1] == b) || (key.verts[0] == b && key.verts[1] == c) ||
           (key.verts[0] == c && key.verts[1] == a);
}

} // namespace

bool CleanMesh (const PublishedMesh& input, const MeshCleanOptions& options, PublishedMesh& output,
                MeshCleanReport& report, std::string& error)
{
    if (!input.valid) {
        error = "input mesh is not valid";
        return false;
    }
    const size_t vertexCount = input.VertexCount ();
    const size_t triangleCount = input.TriangleCount ();
    if (input.positions.size () % 3 != 0 || input.triangles.size () % 3 != 0) {
        error = "geometry cardinality mismatch";
        return false;
    }
    if (!input.normals.empty () && input.normals.size () != input.positions.size ()) {
        error = "normal cardinality does not match vertex cardinality";
        return false;
    }
    if (!input.triMaterial.empty () && input.triMaterial.size () != triangleCount) {
        error = "per-triangle material cardinality does not match triangle count";
        return false;
    }
    for (size_t i = 0; i < input.positions.size (); ++i)
        if (!std::isfinite (input.positions[i])) {
            error = "non-finite position";
            return false;
        }
    for (uint32_t index : input.triangles)
        if (index >= vertexCount) {
            error = "triangle index out of range";
            return false;
        }

    MeshCleanReport counts;
    const bool weld = options.weldTolerance > 0.0;
    const double tolerance = options.weldTolerance;

    // Step 1: weld. weldMap[i] is the representative vertex index for i; every
    // representative maps to itself. Representatives are pairwise farther than
    // the tolerance (any later one close enough would have welded into an
    // earlier one), which is what makes the whole pipeline idempotent.
    std::vector<uint32_t> weldMap (vertexCount);
    std::map<CellKey, std::vector<uint32_t>> grid;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const double* p = input.positions.data () + 3 * i;
        uint32_t representative = i;
        CellKey cell;
        if (weld) {
            cell = CellFor (p, tolerance);
            for (int dx = -1; dx <= 1 && representative == i; ++dx)
                for (int dy = -1; dy <= 1 && representative == i; ++dy)
                    for (int dz = -1; dz <= 1 && representative == i; ++dz) {
                        CellKey probe { cell.x + dx, cell.y + dy, cell.z + dz };
                        auto bucket = grid.find (probe);
                        if (bucket == grid.end ())
                            continue;
                        // Smallest-index representative within exact distance:
                        // buckets hold ascending indices by construction.
                        for (uint32_t candidate : bucket->second) {
                            const double* q = input.positions.data () + 3 * candidate;
                            const double ddx = p[0] - q[0], ddy = p[1] - q[1], ddz = p[2] - q[2];
                            if (ddx * ddx + ddy * ddy + ddz * ddz <= tolerance * tolerance) {
                                representative = candidate;
                                break;
                            }
                        }
                    }
        }
        weldMap[i] = representative;
        if (representative == i) {
            if (weld)
                grid[cell].push_back (i);
        }
        else {
            ++counts.weldedVertices;
            if (!input.normals.empty ()) {
                bool conflict = false;
                for (uint32_t c = 0; c < 3; ++c)
                    if (std::abs (double (input.normals[3 * i + c]) - double (input.normals[3 * representative + c])) >
                        kNormalConflictTolerance)
                        conflict = true;
                if (conflict)
                    ++counts.normalConflicts;
            }
        }
    }

    // Steps 2-4: degenerate, interior, duplicate — one classification pass in
    // triangle order, then per-class removal decisions.
    std::vector<uint8_t> keep (triangleCount, 1);
    std::map<ClassKey, std::array<uint32_t, 2>> classes; // key -> {even members, odd members}
    for (size_t t = 0; t < triangleCount; ++t) {
        const uint32_t a = weldMap[input.triangles[3 * t]];
        const uint32_t b = weldMap[input.triangles[3 * t + 1]];
        const uint32_t c = weldMap[input.triangles[3 * t + 2]];
        if (a == b || b == c || c == a) {
            keep[t] = 0;
            ++counts.removedDegenerate;
            continue;
        }
        if (geomsrv::IsRoundoffZeroTriangle (input.positions.data () + 3 * a, input.positions.data () + 3 * b,
                                             input.positions.data () + 3 * c)) {
            keep[t] = 0;
            ++counts.removedDegenerate;
            continue;
        }
        const ClassKey key = KeyFor (a, b, c);
        ++classes[key][IsEvenOrientation (key, a, b, c) ? 0 : 1];
    }
    // Class decisions: a both-orientation class is an internal wall (remove
    // every member); a single-orientation class keeps its first member.
    std::map<ClassKey, uint32_t> keptMember; // class -> first surviving triangle index
    for (size_t t = 0; t < triangleCount; ++t) {
        if (!keep[t])
            continue;
        const uint32_t a = weldMap[input.triangles[3 * t]];
        const uint32_t b = weldMap[input.triangles[3 * t + 1]];
        const uint32_t c = weldMap[input.triangles[3 * t + 2]];
        const ClassKey key = KeyFor (a, b, c);
        const auto& members = classes[key];
        if (members[0] > 0 && members[1] > 0) {
            keep[t] = 0;
            ++counts.removedInterior;
            continue;
        }
        auto [it, inserted] = keptMember.emplace (key, static_cast<uint32_t> (t));
        if (!inserted) {
            keep[t] = 0;
            ++counts.removedDuplicate;
        }
    }

    // Step 5: compact. Surviving vertices are the referenced representatives,
    // emitted in input (representative) index order — deterministic.
    std::vector<uint8_t> referenced (vertexCount, 0);
    size_t keptTriangles = 0;
    for (size_t t = 0; t < triangleCount; ++t)
        if (keep[t]) {
            ++keptTriangles;
            for (uint32_t e = 0; e < 3; ++e)
                referenced[weldMap[input.triangles[3 * t + e]]] = 1;
        }
    std::vector<uint32_t> compact (vertexCount, 0);
    std::vector<uint32_t> keptVertex; // new index -> representative (input) index
    keptVertex.reserve (vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i)
        if (weldMap[i] == i) {
            if (referenced[i]) {
                compact[i] = static_cast<uint32_t> (keptVertex.size ());
                keptVertex.push_back (i);
            }
            else {
                ++counts.removedUnreferenced;
            }
        }

    PublishedMesh cleaned;
    cleaned.object = input.object;
    cleaned.family = input.family;
    cleaned.revision = input.revision;
    cleaned.sourceAnchorRevision = input.sourceAnchorRevision;
    cleaned.definitionHash = HashPublishedMesh (input);
    const bool hasNormals = !input.normals.empty ();
    cleaned.positions.reserve (keptVertex.size () * 3);
    if (hasNormals)
        cleaned.normals.reserve (keptVertex.size () * 3);
    for (uint32_t rep : keptVertex) {
        for (uint32_t c = 0; c < 3; ++c)
            cleaned.positions.push_back (input.positions[3 * rep + c]);
        if (hasNormals)
            for (uint32_t c = 0; c < 3; ++c)
                cleaned.normals.push_back (input.normals[3 * rep + c]);
        cleaned.bounds.Expand (input.positions[3 * rep], input.positions[3 * rep + 1], input.positions[3 * rep + 2]);
    }
    cleaned.triangles.reserve (keptTriangles * 3);
    cleaned.triMaterial.reserve (keptTriangles);
    cleaned.triangleSource.reserve (keptTriangles);
    for (size_t t = 0; t < triangleCount; ++t)
        if (keep[t]) {
            for (uint32_t e = 0; e < 3; ++e)
                cleaned.triangles.push_back (compact[weldMap[input.triangles[3 * t + e]]]);
            cleaned.triMaterial.push_back (input.triMaterial.empty () ? 0 : input.triMaterial[t]);
            cleaned.triangleSource.push_back (static_cast<uint32_t> (t));
        }
    for (uint32_t rep : keptVertex)
        cleaned.vertexSource.push_back (rep);
    cleaned.valid = true;

    output = std::move (cleaned);
    report = counts;
    return true;
}

} // namespace tapioca::geometry
