#include "SliceEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace geomsrv {

namespace {

constexpr double kPlaneEps = 1e-9;   // |z - plane| below this counts as "on the plane"

// Quantised point key, so segment endpoints that agree to within `weld` merge
// into one graph node.
struct Key {
    int64_t x, y;
    bool operator== (const Key& o) const { return x == o.x && y == o.y; }
};
struct KeyHash {
    size_t operator() (const Key& k) const
    {
        const uint64_t a = static_cast<uint64_t> (k.x) * 0x9E3779B97F4A7C15ull;
        const uint64_t b = static_cast<uint64_t> (k.y) * 0xC2B2AE3D27D4EB4Full;
        return static_cast<size_t> (a ^ (b + 0x165667B19E3779F9ull + (a << 6) + (a >> 2)));
    }
};
Key MakeKey (double x, double y, double weld)
{
    const double inv = 1.0 / weld;
    return Key { static_cast<int64_t> (std::llround (x * inv)),
                 static_cast<int64_t> (std::llround (y * inv)) };
}

// One undirected segment between two welded node ids.
struct Seg { uint32_t a, b; };

// Chain the segment soup of one element into polylines. Open chains are emitted
// first (walked from their loose ends), then whatever remains forms closed loops.
std::vector<Polyline> ChainSegments (const std::vector<Seg>& segs,
                                     const std::vector<double>& nodesXY,
                                     double z)
{
    std::vector<Polyline> out;
    if (segs.empty ())
        return out;

    const size_t nNodes = nodesXY.size () / 2;
    std::vector<std::vector<uint32_t>> adj (nNodes);   // node -> incident segment ids
    for (uint32_t i = 0; i < segs.size (); ++i) {
        adj[segs[i].a].push_back (i);
        adj[segs[i].b].push_back (i);
    }
    std::vector<bool> used (segs.size (), false);

    auto other = [&] (uint32_t seg, uint32_t node) {
        return segs[seg].a == node ? segs[seg].b : segs[seg].a;
    };

    // Walk a chain from `start`, consuming unused segments.
    auto walk = [&] (uint32_t start) {
        Polyline pl;
        uint32_t cur = start;
        pl.pts.push_back (nodesXY[cur * 2]);
        pl.pts.push_back (nodesXY[cur * 2 + 1]);
        pl.pts.push_back (z);
        for (;;) {
            uint32_t next = UINT32_MAX, viaSeg = UINT32_MAX;
            for (uint32_t s : adj[cur]) {
                if (!used[s]) { viaSeg = s; next = other (s, cur); break; }
            }
            if (viaSeg == UINT32_MAX)
                break;
            used[viaSeg] = true;
            cur = next;
            pl.pts.push_back (nodesXY[cur * 2]);
            pl.pts.push_back (nodesXY[cur * 2 + 1]);
            pl.pts.push_back (z);
            if (cur == start) {           // returned home -> closed loop
                pl.closed = true;
                break;
            }
        }
        return pl;
    };

    // 1) Open chains: start at odd-degree (loose) endpoints.
    for (uint32_t n = 0; n < nNodes; ++n) {
        if (adj[n].size () % 2 == 0)
            continue;
        for (;;) {
            bool anyFree = false;
            for (uint32_t s : adj[n]) if (!used[s]) { anyFree = true; break; }
            if (!anyFree) break;
            Polyline pl = walk (n);
            if (pl.PointCount () >= 2)
                out.push_back (std::move (pl));
        }
    }
    // 2) Whatever is left is made of closed loops.
    for (uint32_t s = 0; s < segs.size (); ++s) {
        if (used[s])
            continue;
        Polyline pl = walk (segs[s].a);
        if (pl.PointCount () >= 2)
            out.push_back (std::move (pl));
    }
    return out;
}

} // namespace

std::vector<Polyline> SliceMesh (const double* vertices, size_t vertexCount,
                                 const uint32_t* triangles, size_t triangleCount,
                                 double z, double weld)
{
    if (weld <= 0.0)
        weld = 1e-6;
    if (vertices == nullptr || triangles == nullptr || vertexCount == 0 || triangleCount == 0)
        return {};

    std::vector<double> nodesXY;
    std::vector<Seg>    segs;
    std::unordered_map<Key, uint32_t, KeyHash> nodeOf;

    auto nodeId = [&] (double x, double y) {
        const Key k = MakeKey (x, y, weld);
        auto it = nodeOf.find (k);
        if (it != nodeOf.end ())
            return it->second;
        const uint32_t id = static_cast<uint32_t> (nodesXY.size () / 2);
        nodesXY.push_back (x);
        nodesXY.push_back (y);
        nodeOf.emplace (k, id);
        return id;
    };

    for (size_t t = 0; t < triangleCount * 3; t += 3) {
        // ⚠️ BOUNDS-CHECKED, unlike the Snapshot path this was lifted from. That
        // path's indices came from MeshSerializer and were trusted; these can now
        // also come from the ArchViz extractor mid-rebuild, and an out-of-range
        // index there would read another element's vertices — a plausible-looking
        // slice of the wrong building rather than a crash.
        if (triangles[t] >= vertexCount || triangles[t + 1] >= vertexCount ||
            triangles[t + 2] >= vertexCount)
            continue;
        const double* p[3] = {
            &vertices[triangles[t + 0] * 3],
            &vertices[triangles[t + 1] * 3],
            &vertices[triangles[t + 2] * 3],
        };
        // Signed distance to the plane, with near-zero snapped to exactly 0.
        double d[3];
        for (int i = 0; i < 3; ++i) {
            d[i] = p[i][2] - z;
            if (std::fabs (d[i]) < kPlaneEps) d[i] = 0.0;
        }
        // Tie-break: treat d == 0 as ABOVE. A fully coplanar triangle is then
        // "all above" and is skipped, while the element's side faces still
        // produce the boundary — so slicing exactly at a face is well-defined.
        const bool above[3] = { d[0] >= 0.0, d[1] >= 0.0, d[2] >= 0.0 };
        if (above[0] == above[1] && above[1] == above[2])
            continue;   // no sign change -> no crossing (includes coplanar)

        // Collect the (exactly two) crossing points on the edges that flip.
        double xy[2][2];
        int found = 0;
        for (int e = 0; e < 3 && found < 2; ++e) {
            const int i = e, j = (e + 1) % 3;
            if (above[i] == above[j])
                continue;
            const double denom = d[i] - d[j];
            if (denom == 0.0)
                continue;                       // guarded by the snap above
            const double s = d[i] / denom;      // in (0,1]
            xy[found][0] = p[i][0] + s * (p[j][0] - p[i][0]);
            xy[found][1] = p[i][1] + s * (p[j][1] - p[i][1]);
            ++found;
        }
        if (found != 2)
            continue;

        const uint32_t a = nodeId (xy[0][0], xy[0][1]);
        const uint32_t b = nodeId (xy[1][0], xy[1][1]);
        if (a != b)                              // drop zero-length segments
            segs.push_back (Seg { a, b });
    }

    if (segs.empty ())
        return {};
    return ChainSegments (segs, nodesXY, z);
}

bool IsTangentToPlane (const double* vertices, size_t vertexCount, double z)
{
    if (vertices == nullptr)
        return false;
    for (size_t v = 0; v < vertexCount; ++v) {
        if (std::fabs (vertices[v * 3 + 2] - z) < kPlaneEps)
            return true;
    }
    return false;
}

SliceResult SliceZ (const Snapshot& snap, double z,
                    const std::vector<int32_t>& types,
                    const std::vector<std::string>& guids,
                    double weld,
                    bool nudge)
{
    if (weld <= 0.0)
        weld = 1e-6;

    SliceResult out;
    out.zUsed = z;

    auto included = [&] (const Mesh& m) {
        if (!types.empty () &&
            std::find (types.begin (), types.end (), m.elemType) == types.end ())
            return false;
        if (!guids.empty () &&
            std::find (guids.begin (), guids.end (), m.guid) == guids.end ())
            return false;
        return true;
    };

    // Tangency check: does the plane land exactly on any vertex of an element we
    // are about to cut? If so, that element is tangent (no cross-section) — lift
    // the plane a micron so the cut is unambiguous. See header for why.
    if (nudge) {
        for (const auto& m : snap.meshes) {
            if (!included (m))
                continue;
            if (m.bounds.Valid () && (m.bounds.mn[2] > z || m.bounds.mx[2] < z))
                continue;
            if (IsTangentToPlane (m.vertices.data (), m.VertexCount (), z)) {
                out.zUsed = z + 1e-6;
                out.nudged = true;
                break;
            }
        }
    }
    z = out.zUsed;

    for (const auto& m : snap.meshes) {
        if (!included (m))
            continue;
        // Cheap reject: element's Z range must straddle the plane.
        if (m.bounds.Valid () && (m.bounds.mn[2] > z || m.bounds.mx[2] < z))
            continue;

        ElementSlice es;
        es.guid = m.guid;
        es.elemType = m.elemType;
        es.loops = SliceMesh (m.vertices.data (), m.VertexCount (),
                              m.triangles.data (), m.TriangleCount (), z, weld);
        if (!es.loops.empty ())
            out.elements.push_back (std::move (es));
    }
    return out;
}

} // namespace geomsrv
