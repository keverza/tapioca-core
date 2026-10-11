#include "SurfaceSourceBoundary.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace geomsrv {
namespace surfacemap {
namespace boundary {

namespace {

constexpr double kEps = 1.0e-12;
// Quantized keys must stay well inside int64 and the range the orientation
// arithmetic treats exactly (MSVC's long double is double).
constexpr double kKeyLimit = 1.0e12;
// Two boundary points are collinear when the middle lies within this many grid
// cells of the chord (the grid is already `vertexTolerance`).
constexpr double kCollinearCells = 1.0;

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

Vec3 Sub (const Vec3& a, const Vec3& b)
{
    return Vec3 { a.x - b.x, a.y - b.y, a.z - b.z };
}
Vec3 Cross (const Vec3& a, const Vec3& b)
{
    return Vec3 { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
double Dot (const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
double Length (const Vec3& a)
{
    return std::sqrt (Dot (a, a));
}
Vec3 FromArray (const double v[3])
{
    return Vec3 { v[0], v[1], v[2] };
}

using Key = std::pair<int64_t, int64_t>;

using EdgeKey = std::pair<Key, Key>;
EdgeKey MakeEdge (const Key& a, const Key& b)
{
    return a < b ? std::make_pair (a, b) : std::make_pair (b, a);
}

long double SignedArea2D (const Loop& loop)
{
    long double twiceArea = 0.0;
    for (size_t i = 0; i < loop.size (); ++i) {
        const Key& p = loop[i];
        const Key& q = loop[(i + 1) % loop.size ()];
        twiceArea += static_cast<long double> (p.first) * q.second - static_cast<long double> (q.first) * p.second;
    }
    return 0.5L * twiceArea;
}

long double Orient (const Key& a, const Key& b, const Key& c)
{
    return static_cast<long double> (b.first - a.first) * (c.second - a.second) -
           static_cast<long double> (b.second - a.second) * (c.first - a.first);
}

bool OnSegment (const Key& a, const Key& b, const Key& r)
{
    return std::min (a.first, b.first) <= r.first && r.first <= std::max (a.first, b.first) &&
           std::min (a.second, b.second) <= r.second && r.second <= std::max (a.second, b.second);
}

// True when the closed segments ab and cd meet at all (crossing or touching).
bool SegmentsIntersect (const Key& a, const Key& b, const Key& c, const Key& d)
{
    const long double o1 = Orient (a, b, c);
    const long double o2 = Orient (a, b, d);
    const long double o3 = Orient (c, d, a);
    const long double o4 = Orient (c, d, b);
    if (((o1 > 0.0L && o2 < 0.0L) || (o1 < 0.0L && o2 > 0.0L)) &&
        ((o3 > 0.0L && o4 < 0.0L) || (o3 < 0.0L && o4 > 0.0L)))
        return true;
    if (o1 == 0.0L && OnSegment (a, b, c))
        return true;
    if (o2 == 0.0L && OnSegment (a, b, d))
        return true;
    if (o3 == 0.0L && OnSegment (c, d, a))
        return true;
    if (o4 == 0.0L && OnSegment (c, d, b))
        return true;
    return false;
}

// A closed loop with no self-crossing between non-adjacent edges.
bool LoopsAreSimple (const Loop& loop)
{
    const size_t n = loop.size ();
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            if (j == i + 1 || (i == 0 && j + 1 == n))
                continue; // adjacent edges share an endpoint by construction
            if (SegmentsIntersect (loop[i], loop[(i + 1) % n], loop[j], loop[(j + 1) % n]))
                return false;
        }
    }
    return true;
}

void RotateToMin (Loop& loop)
{
    size_t minIndex = 0;
    for (size_t i = 1; i < loop.size (); ++i)
        if (loop[i] < loop[minIndex])
            minIndex = i;
    std::rotate (loop.begin (), loop.begin () + static_cast<ptrdiff_t> (minIndex), loop.end ());
}

// Drop collinear SUBDIVISION vertices, reject backtracking spikes. A collinear
// vertex is only removed when it lies between its neighbours; a collinear vertex
// outside that span is a spike and the loop is refused.
bool CanonicalizeLoop (Loop& loop)
{
    bool changed = true;
    while (loop.size () > 3 && changed) {
        changed = false;
        for (size_t i = 0; i < loop.size ();) {
            const Key& prev = loop[(i + loop.size () - 1) % loop.size ()];
            const Key& cur = loop[i];
            const Key& next = loop[(i + 1) % loop.size ()];
            const long double span = std::hypot (static_cast<double> (next.first - prev.first),
                                                 static_cast<double> (next.second - prev.second));
            if (span > 0.0) {
                const long double cross =
                    static_cast<long double> (cur.first - prev.first) * (next.second - prev.second) -
                    static_cast<long double> (cur.second - prev.second) * (next.first - prev.first);
                if (std::fabs (cross) <= kCollinearCells * span) {
                    const bool between = std::min (prev.first, next.first) <= cur.first &&
                                         cur.first <= std::max (prev.first, next.first) &&
                                         std::min (prev.second, next.second) <= cur.second &&
                                         cur.second <= std::max (prev.second, next.second);
                    if (!between)
                        return false; // a backtracking spike, not a subdivision
                    loop.erase (loop.begin () + static_cast<ptrdiff_t> (i));
                    changed = true;
                    if (loop.size () <= 3)
                        break;
                    continue;
                }
            }
            ++i;
        }
    }
    if (loop.size () < 3)
        return false;
    RotateToMin (loop);
    return true;
}

bool PointInLoop (double px, double py, const Loop& loop)
{
    bool inside = false;
    for (size_t i = 0; i < loop.size (); ++i) {
        const Key& a = loop[i];
        const Key& b = loop[(i + 1) % loop.size ()];
        if ((a.second > py) != (b.second > py)) {
            const double x = static_cast<double> (a.first) + (py - static_cast<double> (a.second)) *
                                                                 static_cast<double> (b.first - a.first) /
                                                                 static_cast<double> (b.second - a.second);
            if (px < x)
                inside = !inside;
        }
    }
    return inside;
}

} // namespace

bool MakeFrame (const double normal[3], const double origin[3], Frame& frame, std::string& reason)
{
    const Vec3 n = FromArray (normal);
    const double magnitude = Length (n);
    if (!(magnitude > kEps) || !std::isfinite (magnitude)) {
        reason = "face normal is degenerate";
        return false;
    }
    const Vec3 nn { n.x / magnitude, n.y / magnitude, n.z / magnitude };
    const Vec3 helper = (std::fabs (nn.x) < 0.9) ? Vec3 { 1.0, 0.0, 0.0 } : Vec3 { 0.0, 1.0, 0.0 };
    const Vec3 uRaw = Cross (nn, helper);
    const double uLen = Length (uRaw);
    if (!(uLen > kEps)) {
        reason = "could not build a plane frame";
        return false;
    }
    const Vec3 u { uRaw.x / uLen, uRaw.y / uLen, uRaw.z / uLen };
    const Vec3 v = Cross (nn, u);
    frame.origin[0] = origin[0];
    frame.origin[1] = origin[1];
    frame.origin[2] = origin[2];
    frame.normal[0] = nn.x;
    frame.normal[1] = nn.y;
    frame.normal[2] = nn.z;
    frame.u[0] = u.x;
    frame.u[1] = u.y;
    frame.u[2] = u.z;
    frame.v[0] = v.x;
    frame.v[1] = v.y;
    frame.v[2] = v.z;
    return true;
}

bool BuildCanonicalLoops (const std::vector<std::vector<double>>& pieces, const Frame& frame, double vertexTolerance,
                          std::vector<Loop>& loops, std::string& reason)
{
    loops.clear ();
    reason.clear ();
    const Vec3 origin = FromArray (frame.origin);
    const Vec3 axisU = FromArray (frame.u);
    const Vec3 axisV = FromArray (frame.v);

    std::map<EdgeKey, int> edgeCount;
    std::map<std::pair<Key, Key>, int> directedCount;
    std::vector<std::pair<Key, Key>> directed;

    for (const std::vector<double>& piece : pieces) {
        if (piece.size () % 3 != 0 || piece.size () < 9) {
            reason = "a boundary piece needs at least three xyz points";
            return false;
        }
        const size_t count = piece.size () / 3;
        std::vector<Key> keys (count);
        for (size_t i = 0; i < count; ++i) {
            const Vec3 p { piece[i * 3], piece[i * 3 + 1], piece[i * 3 + 2] };
            if (!std::isfinite (p.x) || !std::isfinite (p.y) || !std::isfinite (p.z)) {
                reason = "boundary piece has a non-finite coordinate";
                return false;
            }
            const Vec3 rel = Sub (p, origin);
            const double rx = Dot (rel, axisU) / vertexTolerance;
            const double ry = Dot (rel, axisV) / vertexTolerance;
            if (!std::isfinite (rx) || !std::isfinite (ry) || std::fabs (rx) > kKeyLimit ||
                std::fabs (ry) > kKeyLimit) {
                reason = "coordinate is outside the quantizable range";
                return false;
            }
            keys[i] = Key { static_cast<int64_t> (std::llround (rx)), static_cast<int64_t> (std::llround (ry)) };
        }
        for (size_t i = 0; i < count; ++i) {
            const Key a = keys[i];
            const Key b = keys[(i + 1) % count];
            if (a == b)
                continue;
            edgeCount[MakeEdge (a, b)]++;
            directedCount[std::make_pair (a, b)]++;
            directed.push_back (std::make_pair (a, b));
        }
    }

    if (edgeCount.empty ()) {
        reason = "the piece set has no edges";
        return false;
    }
    for (const auto& entry : edgeCount) {
        if (entry.second > 2) {
            reason = "non-manifold piece set (an edge is shared by more than two pieces)";
            return false;
        }
        if (entry.second == 2) {
            // A shared interior edge must be traversed once each way. Two pieces
            // wound the same way share it in the same direction; cancelling it
            // would delete a real contour and inflate the matched region.
            const auto forward = directedCount.find (entry.first);
            const auto reverse = directedCount.find (std::make_pair (entry.first.second, entry.first.first));
            const int f = forward == directedCount.end () ? 0 : forward->second;
            const int r = reverse == directedCount.end () ? 0 : reverse->second;
            if (f != 1 || r != 1) {
                reason = "duplicated or overlapping pieces (an interior edge is shared in the same direction)";
                return false;
            }
        }
    }

    std::map<Key, Key> next;
    for (const auto& edge : directed) {
        if (edgeCount[MakeEdge (edge.first, edge.second)] != 1)
            continue;
        if (!next.emplace (edge.first, edge.second).second) {
            reason = "non-manifold boundary (a vertex has two outgoing edges)";
            return false;
        }
    }
    if (next.empty ()) {
        reason = "the piece set has no boundary";
        return false;
    }

    std::set<Key> visited;
    for (const auto& start : next) {
        if (visited.count (start.first) != 0)
            continue;
        Loop loop;
        Key cur = start.first;
        while (true) {
            loop.push_back (cur);
            visited.insert (cur);
            const auto it = next.find (cur);
            if (it == next.end ()) {
                reason = "boundary chain is open";
                return false;
            }
            cur = it->second;
            if (cur == start.first)
                break;
            if (visited.count (cur) != 0) {
                reason = "boundary chain crosses another";
                return false;
            }
        }
        if (loop.size () < 3 || !CanonicalizeLoop (loop)) {
            reason = "boundary loop is degenerate or has a backtracking spike";
            return false;
        }
        if (!LoopsAreSimple (loop)) {
            reason = "boundary loop self-intersects";
            return false;
        }
        loops.push_back (std::move (loop));
    }
    if (visited.size () != next.size ()) {
        reason = "boundary topology could not be proven";
        return false;
    }

    std::vector<long double> areas (loops.size ());
    for (size_t i = 0; i < loops.size (); ++i) {
        areas[i] = SignedArea2D (loops[i]);
        if (!(std::fabs (areas[i]) > 0.0L)) {
            reason = "zero-area boundary loop";
            return false;
        }
    }

    size_t dominant = 0;
    for (size_t i = 1; i < loops.size (); ++i)
        if (std::fabs (areas[i]) > std::fabs (areas[dominant]))
            dominant = i;
    if (areas[dominant] < 0.0L) { // orient the whole set so the outer is positive
        for (size_t i = 0; i < loops.size (); ++i) {
            std::reverse (loops[i].begin (), loops[i].end ());
            RotateToMin (loops[i]);
            areas[i] = -areas[i];
        }
    }
    for (size_t i = 0; i < loops.size (); ++i) {
        if (i == dominant)
            continue;
        if (!(areas[i] < 0.0L)) {
            reason = "boundary is not one outer contour with oppositely wound holes";
            return false;
        }
    }
    // Separate simple loops can still cross or touch each other. With those
    // cases excluded, one boundary vertex proves containment even for concave
    // contours (an average of vertices can lie outside a valid concave hole).
    for (size_t i = 0; i < loops.size (); ++i) {
        for (size_t j = i + 1; j < loops.size (); ++j) {
            for (size_t a = 0; a < loops[i].size (); ++a) {
                for (size_t b = 0; b < loops[j].size (); ++b) {
                    if (SegmentsIntersect (loops[i][a], loops[i][(a + 1) % loops[i].size ()], loops[j][b],
                                           loops[j][(b + 1) % loops[j].size ()])) {
                        reason = "boundary loops cross or touch";
                        return false;
                    }
                }
            }
        }
    }
    for (size_t i = 0; i < loops.size (); ++i) {
        if (i == dominant)
            continue;
        const std::pair<double, double> c { static_cast<double> (loops[i][0].first),
                                            static_cast<double> (loops[i][0].second) };
        if (!PointInLoop (c.first, c.second, loops[dominant])) {
            reason = "a hole is not enclosed by the outer contour";
            return false;
        }
        for (size_t j = 0; j < loops.size (); ++j) {
            if (j == dominant || j == i)
                continue;
            if (PointInLoop (c.first, c.second, loops[j])) {
                reason = "holes are nested";
                return false;
            }
        }
    }
    long double holes = 0.0L;
    for (size_t i = 0; i < loops.size (); ++i)
        if (i != dominant)
            holes += std::fabs (areas[i]);
    if (!(std::fabs (areas[dominant]) > holes)) {
        reason = "hole area is not smaller than the outer contour";
        return false;
    }

    std::sort (loops.begin (), loops.end ());
    return true;
}

} // namespace boundary
} // namespace surfacemap
} // namespace geomsrv
