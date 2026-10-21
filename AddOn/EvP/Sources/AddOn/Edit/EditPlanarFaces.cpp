#include "Edit/EditPlanar.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace tapioca::geometry {

namespace {

constexpr long double kMachineEpsilon = static_cast<long double> (std::numeric_limits<double>::epsilon ());
constexpr std::size_t kNoEdge = std::numeric_limits<std::size_t>::max ();
constexpr uint32_t kNoIndex = std::numeric_limits<uint32_t>::max ();

bool Quantize (double value, double origin, double spacing, int64_t& grid)
{
    const long double quotient =
        (static_cast<long double> (value) - static_cast<long double> (origin)) / static_cast<long double> (spacing);
    if (!std::isfinite (quotient))
        return false;
    const long double rounded = std::round (quotient); // Half away from zero.
    // 2^63 is exact in any long double; INT64_MAX as long double may round up.
    constexpr long double kInt64Limit = 9223372036854775808.0L;
    if (rounded < -kInt64Limit || rounded >= kInt64Limit)
        return false;
    grid = static_cast<int64_t> (rounded);
    return true;
}

std::vector<Vec3> CycleVertices (const PlanarCycle& cycle, const std::vector<PlanarHalfEdge>& halfEdges,
                                 const std::vector<Vec3>& vertices)
{
    std::vector<Vec3> points;
    points.reserve (cycle.halfEdges.size ());
    for (uint32_t halfEdge : cycle.halfEdges)
        points.push_back (vertices[halfEdges[halfEdge].origin]);
    return points;
}

// Shoelace sums are computed relative to the first vertex. At georeferenced
// coordinates the absolute products (for example 1e8 * 1e8) lose the small
// area to cancellation; the translated products are the same area but keep
// the side lengths in the arithmetic.
bool TwiceArea (const std::vector<Vec3>& polygon, long double& twice)
{
    twice = 0.0L;
    if (polygon.size () < 3)
        return true;
    const Vec3 origin = polygon.front ();
    long double sum = 0.0L;
    for (std::size_t i = 0; i < polygon.size (); ++i) {
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[(i + 1) % polygon.size ()];
        const long double ax = static_cast<long double> (a.x) - static_cast<long double> (origin.x);
        const long double ay = static_cast<long double> (a.y) - static_cast<long double> (origin.y);
        const long double bx = static_cast<long double> (b.x) - static_cast<long double> (origin.x);
        const long double by = static_cast<long double> (b.y) - static_cast<long double> (origin.y);
        sum += ax * by - bx * ay;
        if (!std::isfinite (sum))
            return false;
    }
    twice = sum;
    return std::isfinite (twice);
}

bool SignedArea (const std::vector<Vec3>& polygon, double& area)
{
    long double twice = 0.0L;
    if (!TwiceArea (polygon, twice))
        return false;
    const long double half = twice * 0.5L;
    if (!std::isfinite (half) || std::abs (half) > static_cast<long double> (std::numeric_limits<double>::max ()))
        return false;
    area = static_cast<double> (half);
    return std::isfinite (area);
}

bool PolygonCentroid (const std::vector<Vec3>& polygon, Vec3& centroid)
{
    long double twice = 0.0L;
    if (!TwiceArea (polygon, twice) || twice == 0.0L)
        return false;
    const Vec3 origin = polygon.front ();
    long double cx = 0.0L;
    long double cy = 0.0L;
    for (std::size_t i = 0; i < polygon.size (); ++i) {
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[(i + 1) % polygon.size ()];
        const long double ax = static_cast<long double> (a.x) - static_cast<long double> (origin.x);
        const long double ay = static_cast<long double> (a.y) - static_cast<long double> (origin.y);
        const long double bx = static_cast<long double> (b.x) - static_cast<long double> (origin.x);
        const long double by = static_cast<long double> (b.y) - static_cast<long double> (origin.y);
        const long double cross = ax * by - bx * ay;
        cx += (ax + bx) * cross;
        cy += (ay + by) * cross;
        if (!std::isfinite (cx) || !std::isfinite (cy))
            return false;
    }
    const long double localX = cx / (3.0L * twice);
    const long double localY = cy / (3.0L * twice);
    const long double x = static_cast<long double> (origin.x) + localX;
    const long double y = static_cast<long double> (origin.y) + localY;
    const long double limit = static_cast<long double> (std::numeric_limits<double>::max ());
    if (!std::isfinite (x) || !std::isfinite (y) || std::abs (x) > limit || std::abs (y) > limit)
        return false;
    centroid = { static_cast<double> (x), static_cast<double> (y), 0.0 };
    return true;
}

// The on-boundary band is derived from the two cross-product terms and machine
// roundoff rather than a fixed spatial epsilon. It is only roundoff: a point
// half the measured gap from a boundary stays strictly inside or outside it.
int PointInPolygon (Vec3 point, const std::vector<Vec3>& polygon)
{
    if (polygon.size () < 3)
        return -1;
    const long double px = static_cast<long double> (point.x);
    const long double py = static_cast<long double> (point.y);
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size () - 1; i < polygon.size (); j = i++) {
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[j];
        const long double ax = static_cast<long double> (a.x);
        const long double ay = static_cast<long double> (a.y);
        const long double bx = static_cast<long double> (b.x);
        const long double by = static_cast<long double> (b.y);
        const long double dx = bx - ax;
        const long double dy = by - ay;
        const long double length = std::hypot (dx, dy);
        if (!(length > 0.0L))
            continue;
        const long double localX = px - ax;
        const long double localY = py - ay;
        const long double first = dx * localY;
        const long double second = dy * localX;
        const long double cross = first - second;
        const long double scale = std::max (
            { 1.0L, std::abs (ax), std::abs (ay), std::abs (bx), std::abs (by), std::abs (px), std::abs (py) });
        // A coordinate subtraction is rounded relative to its own result, so
        // the cross-product roundoff follows the two products even when the
        // absolute coordinates are georeferenced. A coordinate-magnitude bound
        // would call a tiny but representable face's interior "on boundary".
        const long double crossTolerance = 16.0L * kMachineEpsilon * (std::abs (first) + std::abs (second));
        const long double boxTolerance = 16.0L * kMachineEpsilon * scale;
        if (std::abs (cross) <= crossTolerance && px >= std::min (ax, bx) - boxTolerance &&
            px <= std::max (ax, bx) + boxTolerance && py >= std::min (ay, by) - boxTolerance &&
            py <= std::max (ay, by) + boxTolerance)
            return 0;
        if ((ay > py) != (by > py)) {
            const long double x = ax + (py - ay) * dx / dy;
            if (x > px)
                inside = !inside;
        }
    }
    return inside ? 1 : -1;
}

void RemoveAdjacentTwins (std::vector<uint32_t>& cycle, const std::vector<PlanarHalfEdge>& halfEdges)
{
    bool changed = true;
    while (changed && cycle.size () >= 2) {
        changed = false;
        for (std::size_t i = 0; i < cycle.size (); ++i) {
            const std::size_t j = (i + 1) % cycle.size ();
            if (cycle[j] != halfEdges[cycle[i]].twin)
                continue;
            if (j == 0) {
                cycle.erase (cycle.begin ());
                cycle.pop_back ();
            }
            else {
                cycle.erase (cycle.begin () + static_cast<std::ptrdiff_t> (j));
                cycle.erase (cycle.begin () + static_cast<std::ptrdiff_t> (i));
            }
            changed = true;
            break;
        }
    }
}

std::size_t LeastRotation (const std::vector<std::pair<int64_t, int64_t>>& points)
{
    const std::size_t count = points.size ();
    if (count < 2)
        return 0;
    std::size_t i = 0, j = 1, k = 0;
    while (i < count && j < count && k < count) {
        const auto a = points[(i + k) % count];
        const auto b = points[(j + k) % count];
        if (a == b) {
            ++k;
            continue;
        }
        if (a < b) {
            j = j + k + 1;
            if (j <= i)
                j = i + 1;
        }
        else {
            i = i + k + 1;
            if (i <= j)
                i = j + 1;
        }
        k = 0;
    }
    return std::min (i, j) % count;
}

bool QuantizeCycle (const std::vector<Vec3>& vertices, bool hole, const PlanarOptions& options, FaceCycle& out)
{
    const double spacing = options.snapTolerance;
    std::vector<std::pair<int64_t, int64_t>> quantized;
    quantized.reserve (vertices.size ());
    for (const Vec3& point : vertices) {
        int64_t x = 0, y = 0;
        if (!Quantize (point.x, options.quantizeOriginX, spacing, x) ||
            !Quantize (point.y, options.quantizeOriginY, spacing, y))
            return false;
        const std::pair<int64_t, int64_t> q { x, y };
        if (quantized.empty () || quantized.back () != q)
            quantized.push_back (q);
    }
    if (quantized.size () > 1 && quantized.front () == quantized.back ())
        quantized.pop_back ();
    if (quantized.empty ()) {
        out = FaceCycle {};
        out.hole = hole;
        return true;
    }
    const std::size_t start = LeastRotation (quantized);
    out = FaceCycle {};
    out.hole = hole;
    out.x.reserve (quantized.size ());
    out.y.reserve (quantized.size ());
    for (std::size_t i = 0; i < quantized.size (); ++i) {
        const auto& point = quantized[(start + i) % quantized.size ()];
        out.x.push_back (point.first);
        out.y.push_back (point.second);
    }
    return true;
}

bool CycleLess (const FaceCycle& a, const FaceCycle& b)
{
    const std::size_t count = std::min (a.x.size (), b.x.size ());
    for (std::size_t i = 0; i < count; ++i) {
        if (a.x[i] != b.x[i])
            return a.x[i] < b.x[i];
        if (a.y[i] != b.y[i])
            return a.y[i] < b.y[i];
    }
    return a.x.size () < b.x.size ();
}

struct CandidateFace {
    uint32_t cycle = 0;
};

void AddDiagnostic (PlanarMap& map, PlanarDiagnosticCode code, Id sourceA, Id sourceB, Vec3 at, double amount,
                    std::string message)
{
    map.diagnostics.push_back ({ code, sourceA, sourceB, at, amount, std::move (message) });
}

// An undirected bridge has both half-edges on the same face walk. Such an edge
// (a non-separating connector or a dangling guide) is part of the graph but not
// of a face boundary. The iterative DFS keeps the traversal bounded by the
// graph size; recursion would make a deep input a stack risk.
std::vector<char> FindBridges (const PlanarMap& map)
{
    std::vector<char> bridge (map.edges.size (), 0);
    if (map.vertices.empty ())
        return bridge;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> adjacency (map.vertices.size ());
    for (std::size_t edge = 0; edge < map.edges.size (); ++edge) {
        const PlanarEdge& item = map.edges[edge];
        adjacency[item.a].push_back ({ item.b, static_cast<uint32_t> (edge) });
        adjacency[item.b].push_back ({ item.a, static_cast<uint32_t> (edge) });
    }

    std::vector<uint32_t> discovered (map.vertices.size (), kNoIndex);
    std::vector<uint32_t> low (map.vertices.size (), 0);
    std::vector<uint32_t> parentEdge (map.vertices.size (), kNoIndex);
    struct Frame {
        uint32_t vertex = 0;
        std::size_t next = 0;
    };
    std::vector<Frame> stack;
    uint32_t clock = 0;
    for (uint32_t root = 0; root < map.vertices.size (); ++root) {
        if (discovered[root] != kNoIndex)
            continue;
        discovered[root] = low[root] = clock++;
        stack.push_back ({ root, 0 });
        while (!stack.empty ()) {
            const uint32_t vertex = stack.back ().vertex;
            if (stack.back ().next < adjacency[vertex].size ()) {
                const auto [to, edge] = adjacency[vertex][stack.back ().next++];
                if (edge == parentEdge[vertex])
                    continue;
                if (discovered[to] == kNoIndex) {
                    parentEdge[to] = edge;
                    discovered[to] = low[to] = clock++;
                    stack.push_back ({ to, 0 });
                }
                else {
                    low[vertex] = std::min (low[vertex], discovered[to]);
                }
            }
            else {
                stack.pop_back ();
                if (!stack.empty ()) {
                    const uint32_t parent = stack.back ().vertex;
                    low[parent] = std::min (low[parent], low[vertex]);
                    if (low[vertex] > discovered[parent])
                        bridge[parentEdge[vertex]] = 1;
                }
            }
        }
    }
    return bridge;
}

bool SplitAtBridges (const std::vector<uint32_t>& walk, const std::vector<char>& bridges,
                     const std::vector<PlanarHalfEdge>& halfEdges, std::vector<std::vector<uint32_t>>& chains)
{
    std::set<uint32_t> remaining;
    for (uint32_t h : walk)
        if (!bridges[halfEdges[h].edge])
            remaining.insert (h);
    // Bypass each bridge at its attachment vertex, through twin.next. Splitting
    // at bridge occurrences instead makes arbitrary open fragments when two
    // spurs touch one boundary, and their implicit closing chords invent faces.
    // A bridge to a nested component still leaves that component its own cycle.
    while (!remaining.empty ()) {
        const uint32_t start = *remaining.begin ();
        uint32_t current = start;
        std::vector<uint32_t> cycle;
        do {
            if (!remaining.erase (current))
                return false;
            cycle.push_back (current);
            uint32_t next = halfEdges[current].next;
            size_t skipped = 0;
            while (bridges[halfEdges[next].edge]) {
                if (++skipped > walk.size ())
                    return false;
                next = halfEdges[halfEdges[next].twin].next;
            }
            if (halfEdges[halfEdges[current].twin].origin != halfEdges[next].origin)
                return false;
            current = next;
        } while (current != start);
        chains.push_back (std::move (cycle));
    }
    return true;
}

bool RayHitDistance (long double px, long double py, long double dx, long double dy, const std::vector<Vec3>& polygon,
                     std::size_t skipEdge, long double& distance)
{
    if (polygon.size () < 2)
        return false;
    const long double parameterTolerance = 128.0L * kMachineEpsilon;
    bool found = false;
    long double nearest = std::numeric_limits<long double>::infinity ();
    for (std::size_t i = 0; i < polygon.size (); ++i) {
        if (i == skipEdge)
            continue;
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[(i + 1) % polygon.size ()];
        const long double sx = static_cast<long double> (b.x) - static_cast<long double> (a.x);
        const long double sy = static_cast<long double> (b.y) - static_cast<long double> (a.y);
        const long double denominator = dx * sy - dy * sx;
        if (denominator == 0.0L)
            continue;
        const long double qx = static_cast<long double> (a.x) - px;
        const long double qy = static_cast<long double> (a.y) - py;
        const long double t = (qx * sy - qy * sx) / denominator;
        const long double u = (qx * dy - qy * dx) / denominator;
        if (!(t > 0.0L) || !std::isfinite (t))
            continue;
        if (u < -parameterTolerance || u > 1.0L + parameterTolerance)
            continue;
        if (t < nearest) {
            nearest = t;
            found = true;
        }
    }
    if (found)
        distance = nearest;
    return found;
}

bool FaceSeed (const std::vector<Vec3>& outer, const std::vector<std::vector<Vec3>>& holes, Vec3& seed)
{
    const auto valid = [&] (Vec3 point) {
        if (!std::isfinite (point.x) || !std::isfinite (point.y))
            return false;
        if (PointInPolygon (point, outer) != 1)
            return false;
        for (const auto& hole : holes)
            if (PointInPolygon (point, hole) != -1)
                return false;
        return true;
    };

    Vec3 centroid;
    if (PolygonCentroid (outer, centroid) && valid (centroid)) {
        seed = centroid;
        return true;
    }

    if (!outer.empty ()) {
        long double sx = 0.0L, sy = 0.0L;
        for (const Vec3& point : outer) {
            sx += static_cast<long double> (point.x);
            sy += static_cast<long double> (point.y);
        }
        if (std::isfinite (sx) && std::isfinite (sy)) {
            const Vec3 average { static_cast<double> (sx / static_cast<long double> (outer.size ())),
                                 static_cast<double> (sy / static_cast<long double> (outer.size ())), 0.0 };
            if (valid (average)) {
                seed = average;
                return true;
            }
        }
    }

    // Look inward from each outer edge midpoint. The nearest other boundary in
    // that direction bounds the local face width; half of it is strictly inside
    // the face even for a gap much narrower than the old fixed probe step. If a
    // representable seed cannot be found the caller refuses instead of using a
    // boundary vertex.
    for (std::size_t i = 0; i < outer.size (); ++i) {
        const Vec3& a = outer[i];
        const Vec3& b = outer[(i + 1) % outer.size ()];
        const long double ax = static_cast<long double> (a.x);
        const long double ay = static_cast<long double> (a.y);
        const long double bx = static_cast<long double> (b.x);
        const long double by = static_cast<long double> (b.y);
        const long double dx = bx - ax;
        const long double dy = by - ay;
        const long double length = std::hypot (dx, dy);
        if (!(length > 0.0L))
            continue;
        const long double mx = (ax + bx) * 0.5L;
        const long double my = (ay + by) * 0.5L;
        const long double nx = -dy / length;
        const long double ny = dx / length;
        long double nearest = std::numeric_limits<long double>::infinity ();
        long double hit = 0.0L;
        if (RayHitDistance (mx, my, nx, ny, outer, i, hit))
            nearest = hit;
        for (const auto& hole : holes) {
            if (RayHitDistance (mx, my, nx, ny, hole, kNoEdge, hit))
                nearest = std::min (nearest, hit);
        }
        if (!std::isfinite (nearest) || !(nearest > 0.0L))
            continue;
        for (double fraction : { 0.5, 0.25, 0.75 }) {
            const long double step = nearest * static_cast<long double> (fraction);
            const Vec3 candidate { static_cast<double> (mx + nx * step), static_cast<double> (my + ny * step), 0.0 };
            if (valid (candidate)) {
                seed = candidate;
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool FaceCycleLess (const FaceCycle& a, const FaceCycle& b)
{
    return CycleLess (a, b);
}

bool FaceDescriptorLess (const FaceDescriptor& a, const FaceDescriptor& b)
{
    const std::size_t pieces = std::min (a.sourcePieces.size (), b.sourcePieces.size ());
    for (std::size_t i = 0; i < pieces; ++i) {
        if (a.sourcePieces[i].source != b.sourcePieces[i].source)
            return a.sourcePieces[i].source < b.sourcePieces[i].source;
        if (a.sourcePieces[i].piece != b.sourcePieces[i].piece)
            return a.sourcePieces[i].piece < b.sourcePieces[i].piece;
    }
    if (a.sourcePieces.size () != b.sourcePieces.size ())
        return a.sourcePieces.size () < b.sourcePieces.size ();
    if (CycleLess (a.outer, b.outer))
        return true;
    if (CycleLess (b.outer, a.outer))
        return false;
    if (a.holes.size () != b.holes.size ())
        return a.holes.size () < b.holes.size ();
    for (std::size_t i = 0; i < a.holes.size (); ++i) {
        if (CycleLess (a.holes[i], b.holes[i]))
            return true;
        if (CycleLess (b.holes[i], a.holes[i]))
            return false;
    }
    return false;
}

namespace detail {

bool AssignFaceLocators (PlanarMap& map, std::string& error)
{
    std::sort (map.faces.begin (), map.faces.end (), [] (const PlanarFace& a, const PlanarFace& b) {
        return FaceDescriptorLess (a.descriptor, b.descriptor);
    });
    for (std::size_t i = 1; i < map.faces.size (); ++i) {
        if (!(map.faces[i - 1].descriptor == map.faces[i].descriptor))
            continue;
        const Id sourceA = map.faces[i - 1].sources.empty () ? kNoId : map.faces[i - 1].sources.front ();
        const Id sourceB = map.faces[i].sources.empty () ? kNoId : map.faces[i].sources.front ();
        const Vec3 at { (map.faces[i - 1].seed.x + map.faces[i].seed.x) * 0.5,
                        (map.faces[i - 1].seed.y + map.faces[i].seed.y) * 0.5, 0.0 };
        AddDiagnostic (map, PlanarDiagnosticCode::FaceKeyAmbiguous, sourceA, sourceB, at, 0.0,
                       "distinct faces share one canonical face descriptor");
        error = "two distinct faces share one canonical descriptor (FaceKeyAmbiguous)";
        return false;
    }
    for (std::size_t i = 0; i < map.faces.size (); ++i)
        map.faces[i].id = static_cast<uint64_t> (i) + 1;
    return true;
}

bool BuildFaces (const PlanarOptions& options, PlanarMap& map, std::string& error)
{
    map.cycles.clear ();
    map.faces.clear ();
    if (map.halfEdges.empty ()) {
        if (!map.edges.empty ())
            AddDiagnostic (map, PlanarDiagnosticCode::Unbounded, kNoId, kNoId, {}, 0.0, "there is no bounded face");
        return true;
    }

    // Walk the raw DCEL before changing any next pointer. Bridges are then
    // removed by edge, not by cancelling adjacent twins: the two half-edges of
    // a non-separating connector occur at opposite ends of the same walk.
    std::vector<std::vector<uint32_t>> walks;
    std::vector<bool> visited (map.halfEdges.size (), false);
    for (std::size_t start = 0; start < map.halfEdges.size (); ++start) {
        if (visited[start])
            continue;
        std::vector<uint32_t> walked;
        uint32_t current = static_cast<uint32_t> (start);
        do {
            if (current >= map.halfEdges.size () || visited[current]) {
                error = "the planar half-edge walk did not close";
                AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
                return false;
            }
            visited[current] = true;
            walked.push_back (current);
            current = map.halfEdges[current].next;
            if (walked.size () > map.halfEdges.size ()) {
                error = "the planar half-edge walk did not close";
                AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
                return false;
            }
        } while (current != start);
        walks.push_back (std::move (walked));
    }

    const std::vector<char> bridges = FindBridges (map);
    for (const auto& walk : walks) {
        std::vector<std::vector<uint32_t>> chains;
        if (!SplitAtBridges (walk, bridges, map.halfEdges, chains)) {
            error = "bridge removal did not preserve closed planar boundaries";
            AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
            return false;
        }
        for (auto& chain : chains) {
            RemoveAdjacentTwins (chain, map.halfEdges);
            if (chain.empty ())
                continue;
            PlanarCycle cycle;
            cycle.halfEdges = std::move (chain);
            const auto polygon = CycleVertices (cycle, map.halfEdges, map.vertices);
            double area = 0.0;
            if (!SignedArea (polygon, area)) {
                error = "a face boundary area is not representable as a finite double";
                AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId,
                               polygon.empty () ? Vec3 {} : polygon.front (), 0.0, error);
                return false;
            }
            cycle.area = area;
            cycle.hole = area < 0.0;
            map.cycles.push_back (std::move (cycle));
        }
    }

    // Keep the public next permutation consistent with the reduced cycles.
    // Bridges are retained in edges/halfEdges as two-half-edge cycles, the same
    // representation an adjacent spur pair had before.
    std::vector<char> onCycle (map.halfEdges.size (), 0);
    for (const PlanarCycle& cycle : map.cycles) {
        for (std::size_t i = 0; i < cycle.halfEdges.size (); ++i) {
            const uint32_t halfEdge = cycle.halfEdges[i];
            map.halfEdges[halfEdge].next = cycle.halfEdges[(i + 1) % cycle.halfEdges.size ()];
            onCycle[halfEdge] = 1;
        }
    }
    for (std::size_t halfEdge = 0; halfEdge < map.halfEdges.size (); ++halfEdge) {
        if (onCycle[halfEdge])
            continue;
        const uint32_t twin = map.halfEdges[halfEdge].twin;
        if (twin >= map.halfEdges.size () || onCycle[twin]) {
            error = "the reduced planar boundary lost a half-edge twin";
            AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
            return false;
        }
        map.halfEdges[halfEdge].next = twin;
    }
    std::vector<uint32_t> incoming (map.halfEdges.size (), 0);
    for (const PlanarHalfEdge& halfEdge : map.halfEdges) {
        if (halfEdge.next >= map.halfEdges.size ()) {
            error = "the reduced planar half-edge walk is inconsistent";
            AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
            return false;
        }
        ++incoming[halfEdge.next];
    }
    for (uint32_t count : incoming) {
        if (count != 1u) {
            error = "the reduced planar half-edge walk is inconsistent";
            AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
            return false;
        }
    }

    std::vector<CandidateFace> positive;
    for (std::size_t i = 0; i < map.cycles.size (); ++i) {
        if (!(map.cycles[i].area > 0.0))
            continue;
        if (map.cycles[i].area < options.minArea) {
            const auto polygon = CycleVertices (map.cycles[i], map.halfEdges, map.vertices);
            const Vec3 at = polygon.empty () ? Vec3 {} : polygon.front ();
            AddDiagnostic (map, PlanarDiagnosticCode::Sliver, kNoId, kNoId, at, map.cycles[i].area,
                           "bounded face is below minArea");
            continue;
        }
        positive.push_back ({ static_cast<uint32_t> (i) });
    }

    std::vector<std::vector<Vec3>> positivePolygons;
    positivePolygons.reserve (positive.size ());
    for (const auto& candidate : positive)
        positivePolygons.push_back (CycleVertices (map.cycles[candidate.cycle], map.halfEdges, map.vertices));

    std::vector<std::vector<uint32_t>> attachedHoles (positive.size ());
    for (std::size_t negative = 0; negative < map.cycles.size (); ++negative) {
        const PlanarCycle& hole = map.cycles[negative];
        if (!(hole.area < 0.0))
            continue;
        if (-hole.area < options.minArea) {
            const auto polygon = CycleVertices (hole, map.halfEdges, map.vertices);
            const Vec3 at = polygon.empty () ? Vec3 {} : polygon.front ();
            AddDiagnostic (map, PlanarDiagnosticCode::Sliver, kNoId, kNoId, at, hole.area,
                           "hole boundary is below minArea");
            continue;
        }
        // A hole is enclosed when one of its own boundary vertices is strictly
        // inside a positive boundary. The hole's vertices are on the island
        // face's boundary, so strict containment excludes that island without a
        // displaced probe or a fixed minimum step.
        int best = -1;
        double bestArea = std::numeric_limits<double>::infinity ();
        for (Vec3 probe : CycleVertices (hole, map.halfEdges, map.vertices)) {
            for (std::size_t candidate = 0; candidate < positive.size (); ++candidate) {
                if (PointInPolygon (probe, positivePolygons[candidate]) != 1)
                    continue;
                const double area = map.cycles[positive[candidate].cycle].area;
                if (area < bestArea) {
                    bestArea = area;
                    best = static_cast<int> (candidate);
                }
            }
            if (best >= 0)
                break;
        }
        if (best >= 0)
            attachedHoles[static_cast<std::size_t> (best)].push_back (static_cast<uint32_t> (negative));
    }

    for (std::size_t candidate = 0; candidate < positive.size (); ++candidate) {
        const PlanarCycle& outerCycle = map.cycles[positive[candidate].cycle];
        PlanarFace face;
        face.halfEdges = outerCycle.halfEdges;
        face.holes = attachedHoles[candidate];
        face.area = outerCycle.area;
        for (uint32_t hole : face.holes)
            face.area += map.cycles[hole].area;
        if (!std::isfinite (face.area)) {
            error = "a face net area is not representable as a finite double";
            AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId,
                           positivePolygons[candidate].empty () ? Vec3 {} : positivePolygons[candidate].front (), 0.0,
                           error);
            return false;
        }
        if (face.area < options.minArea) {
            const auto polygon = CycleVertices (outerCycle, map.halfEdges, map.vertices);
            const Vec3 at = polygon.empty () ? Vec3 {} : polygon.front ();
            AddDiagnostic (map, PlanarDiagnosticCode::Sliver, kNoId, kNoId, at, face.area,
                           "face net area is below minArea");
            continue;
        }

        std::set<Id> sources;
        std::set<std::pair<Id, uint32_t>> pieces;
        const auto collect = [&] (const PlanarCycle& cycle) {
            for (uint32_t halfEdge : cycle.halfEdges) {
                const PlanarEdge& edge = map.edges[map.halfEdges[halfEdge].edge];
                sources.insert (edge.sources.begin (), edge.sources.end ());
                for (const auto& piece : edge.pieces)
                    pieces.insert ({ piece.source, piece.piece });
            }
        };
        collect (outerCycle);
        for (uint32_t hole : face.holes)
            collect (map.cycles[hole]);
        face.sources.assign (sources.begin (), sources.end ());
        for (const auto& piece : pieces)
            face.descriptor.sourcePieces.push_back ({ piece.first, piece.second });

        if (!QuantizeCycle (positivePolygons[candidate], false, options, face.descriptor.outer)) {
            error = "quantized face descriptor exceeds the signed 64-bit grid";
            AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
            return false;
        }

        std::vector<std::pair<FaceCycle, uint32_t>> quantizedHoles;
        for (uint32_t hole : face.holes) {
            FaceCycle cycle;
            if (!QuantizeCycle (CycleVertices (map.cycles[hole], map.halfEdges, map.vertices), true, options, cycle)) {
                error = "quantized face descriptor exceeds the signed 64-bit grid";
                AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
                return false;
            }
            quantizedHoles.push_back ({ std::move (cycle), hole });
        }
        std::sort (quantizedHoles.begin (), quantizedHoles.end (), [] (const auto& a, const auto& b) {
            if (CycleLess (a.first, b.first))
                return true;
            if (CycleLess (b.first, a.first))
                return false;
            return a.second < b.second;
        });
        face.holes.clear ();
        for (auto& hole : quantizedHoles) {
            face.descriptor.holes.push_back (std::move (hole.first));
            face.holes.push_back (hole.second);
        }

        std::vector<std::vector<Vec3>> holePolygons;
        for (uint32_t hole : face.holes)
            holePolygons.push_back (CycleVertices (map.cycles[hole], map.halfEdges, map.vertices));
        if (!FaceSeed (positivePolygons[candidate], holePolygons, face.seed)) {
            error = "a bounded face has no representable strict interior seed";
            AddDiagnostic (map, PlanarDiagnosticCode::Degenerate, kNoId, kNoId,
                           positivePolygons[candidate].empty () ? Vec3 {} : positivePolygons[candidate].front (), 0.0,
                           error);
            return false;
        }
        map.faces.push_back (std::move (face));
    }

    if (!AssignFaceLocators (map, error))
        return false;
    if (map.faces.empty () && !map.edges.empty ())
        AddDiagnostic (map, PlanarDiagnosticCode::Unbounded, kNoId, kNoId, {}, 0.0, "there is no bounded face");
    return true;
}

} // namespace detail
} // namespace tapioca::geometry
