#include "ArchViz/StorySliceUnion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace geomsrv {
namespace archviz {

namespace {

// ⚠️ THESE THREE NUMBERS ARE A CONTRACT WITH sliceunion.py. They are its _TOL,
// _EPS and _ON unchanged. The viewer and the Python overlay are meant to draw
// the same outline for the same storey, and these decide what "the same" is.
constexpr double kTol = 1e-4; // quantise / coincidence grid, metres (0.1 mm)
constexpr double kEps = 1e-3; // side-probe offset for the inside test, metres (1 mm)
constexpr double kOn = 1e-7;  // "point lies on the line" threshold

double Q (double v)
{
    return std::floor (v / kTol + 0.5) * kTol;
}

// ⚠️ QUANTISED POINTS ARE COMPARED AS INTEGERS, NOT AS DOUBLES. The Python
// relies on `round(v/_TOL)*_TOL` being bit-identical for two vertices landing in
// the same grid cell. Keying on the integer cell instead removes the question
// entirely, and a dedup that silently fails is a doubled line — the one artefact
// this whole file exists to remove.
struct Pt {
    double x = 0.0, y = 0.0;
    int64_t gx = 0, gy = 0; // the grid cell, which is the identity

    bool operator== (const Pt& o) const
    {
        return gx == o.gx && gy == o.gy;
    }
    // Lexicographic on the CELL, so the dedup key below orders a segment's two
    // endpoints the same way regardless of which direction it was walked in.
    bool operator< (const Pt& o) const
    {
        return gx != o.gx ? gx < o.gx : gy < o.gy;
    }
};

Pt MakePt (double x, double y)
{
    Pt p;
    p.x = Q (x);
    p.y = Q (y);
    p.gx = static_cast<int64_t> (std::llround (x / kTol));
    p.gy = static_cast<int64_t> (std::llround (y / kTol));
    return p;
}

struct Seg {
    Pt a, b;
};

struct Aabb2 {
    double lo[2] = { 0.0, 0.0 };
    double hi[2] = { 0.0, 0.0 };

    bool Overlaps (const Aabb2& o, double pad) const
    {
        return !(hi[0] + pad < o.lo[0] || o.hi[0] + pad < lo[0] || hi[1] + pad < o.lo[1] || o.hi[1] + pad < lo[1]);
    }
    bool Contains (double x, double y, double pad) const
    {
        return x >= lo[0] - pad && x <= hi[0] + pad && y >= lo[1] - pad && y <= hi[1] + pad;
    }
};

Aabb2 SegBounds (const Seg& s)
{
    Aabb2 b;
    b.lo[0] = std::min (s.a.x, s.b.x);
    b.hi[0] = std::max (s.a.x, s.b.x);
    b.lo[1] = std::min (s.a.y, s.b.y);
    b.hi[1] = std::max (s.a.y, s.b.y);
    return b;
}

// One input ring: the quantised (x, y) vertices with any repeated closing vertex
// stripped, plus its AABB so the point-in-polygon sweep can reject it outright.
struct Ring {
    std::vector<double> xy; // x, y interleaved
    Aabb2 bounds;

    size_t Count () const
    {
        return xy.size () / 2;
    }
};

// Even-odd point-in-polygon for one ring.
bool PointInRing (double x, double y, const Ring& r)
{
    const size_t n = r.Count ();
    if (n < 3)
        return false;
    bool inside = false;
    size_t j = n - 1;
    for (size_t i = 0; i < n; ++i) {
        const double xi = r.xy[i * 2], yi = r.xy[i * 2 + 1];
        const double xj = r.xy[j * 2], yj = r.xy[j * 2 + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            inside = !inside;
        j = i;
    }
    return inside;
}

bool InsideUnion (double x, double y, const std::vector<Ring>& rings)
{
    for (const Ring& r : rings) {
        // ⚠️ THE AABB REJECT IS WHAT MAKES THIS AFFORDABLE. Classification is
        // O(sub-edges * rings) and a real storey has both in the thousands; the
        // reject turns almost all of it into two comparisons. It pads by kEps
        // because the probe point is deliberately kEps off the boundary, so a
        // point just outside a ring's own bounding box must still be tested.
        if (!r.bounds.Contains (x, y, kEps))
            continue;
        if (PointInRing (x, y, r))
            return true;
    }
    return false;
}

// Is this loop a region? Its ends coincide — SliceMesh repeats the first point
// on a closed ring, which is both what a drawing wants and what this tests for.
bool IsClosed (const Polyline& loop)
{
    const size_t n = loop.PointCount ();
    if (n < 4)
        return false;
    const double* first = loop.pts.data ();
    const double* last = loop.pts.data () + (n - 1) * 3;
    return std::fabs (first[0] - last[0]) < kTol && std::fabs (first[1] - last[1]) < kTol;
}

// Intersection point(s) of segment ab with segment cd, as points ON ab to split
// at. Proper crossings, T-junction touches, and — for collinear overlap — cd's
// endpoints. Endpoints of ab itself are added by the caller.
void SegPoints (const Pt& a, const Pt& b, const Pt& c, const Pt& d, double out[4], int& count)
{
    count = 0;
    const double abx = b.x - a.x, aby = b.y - a.y;
    const double cdx = d.x - c.x, cdy = d.y - c.y;
    const double denom = abx * cdy - aby * cdx;
    const double acx = c.x - a.x, acy = c.y - a.y;
    if (std::fabs (denom) > kOn) { // non-parallel: one crossing
        const double t = (acx * cdy - acy * cdx) / denom;
        const double u = (acx * aby - acy * abx) / denom;
        if (t >= -kOn && t <= 1.0 + kOn && u >= -kOn && u <= 1.0 + kOn) {
            out[0] = a.x + t * abx;
            out[1] = a.y + t * aby;
            count = 1;
        }
        return;
    }
    // Parallel; keep only if collinear (c lies on line ab), then add cd's ends.
    if (std::fabs (acx * aby - acy * abx) > kOn * std::max (1.0, std::fabs (abx) + std::fabs (aby)))
        return;
    out[0] = c.x;
    out[1] = c.y;
    out[2] = d.x;
    out[3] = d.y;
    count = 2;
}

using CellKey = std::pair<std::pair<int64_t, int64_t>, std::pair<int64_t, int64_t>>;

struct CellKeyHash {
    size_t operator() (const CellKey& k) const
    {
        uint64_t h = 1469598103934665603ull;
        const int64_t v[4] = { k.first.first, k.first.second, k.second.first, k.second.second };
        for (int i = 0; i < 4; ++i) {
            h ^= static_cast<uint64_t> (v[i]);
            h *= 1099511628211ull;
        }
        return static_cast<size_t> (h);
    }
};

} // namespace

std::vector<UnionSegment> UnionLoops (const std::vector<Polyline>& loops)
{
    std::vector<Ring> rings;
    std::vector<std::vector<double>> openPolys; // x, y interleaved

    for (const Polyline& loop : loops) {
        const size_t n = loop.PointCount ();
        if (n < 2)
            continue;
        if (IsClosed (loop)) {
            Ring r;
            r.xy.reserve ((n - 1) * 2);
            for (size_t i = 0; i + 1 < n; ++i) { // strip the repeated closing vertex
                r.xy.push_back (Q (loop.pts[i * 3]));
                r.xy.push_back (Q (loop.pts[i * 3 + 1]));
            }
            if (r.Count () < 3)
                continue;
            r.bounds.lo[0] = r.bounds.hi[0] = r.xy[0];
            r.bounds.lo[1] = r.bounds.hi[1] = r.xy[1];
            for (size_t i = 1; i < r.Count (); ++i) {
                r.bounds.lo[0] = std::min (r.bounds.lo[0], r.xy[i * 2]);
                r.bounds.hi[0] = std::max (r.bounds.hi[0], r.xy[i * 2]);
                r.bounds.lo[1] = std::min (r.bounds.lo[1], r.xy[i * 2 + 1]);
                r.bounds.hi[1] = std::max (r.bounds.hi[1], r.xy[i * 2 + 1]);
            }
            rings.push_back (std::move (r));
        }
        else {
            std::vector<double> poly;
            poly.reserve (n * 2);
            for (size_t i = 0; i < n; ++i) {
                poly.push_back (loop.pts[i * 3]);
                poly.push_back (loop.pts[i * 3 + 1]);
            }
            openPolys.push_back (std::move (poly));
        }
    }

    // 1-2. edge soup -> split every edge at all intersections with every other.
    std::vector<Seg> edges;
    std::vector<Aabb2> edgeBounds;
    for (const Ring& r : rings) {
        const size_t n = r.Count ();
        for (size_t i = 0; i < n; ++i) {
            const size_t k = (i + 1) % n;
            Seg s;
            s.a = MakePt (r.xy[i * 2], r.xy[i * 2 + 1]);
            s.b = MakePt (r.xy[k * 2], r.xy[k * 2 + 1]);
            edges.push_back (s);
            edgeBounds.push_back (SegBounds (s));
        }
    }

    // 3. dedup coincident sub-edges, keyed on the ordered endpoint cell pair.
    std::unordered_map<CellKey, Seg, CellKeyHash> uniq;

    std::vector<double> params;
    for (size_t i = 0; i < edges.size (); ++i) {
        const Seg& seg = edges[i];
        const double abx = seg.b.x - seg.a.x, aby = seg.b.y - seg.a.y;
        const double ab2 = abx * abx + aby * aby;
        if (ab2 < kTol * kTol)
            continue;

        params.clear ();
        params.push_back (0.0);
        params.push_back (1.0);
        for (size_t j = 0; j < edges.size (); ++j) {
            if (j == i)
                continue;
            // ⚠️ THE AABB REJECT AGAIN, and here it is the difference between a
            // storey that slices in milliseconds and one that stalls the
            // extraction thread: the split is O(E^2) and E is every edge of
            // every element's cross-section. Padded by kTol so a T-junction
            // exactly on the shared bound is not rejected before it is found.
            if (!edgeBounds[i].Overlaps (edgeBounds[j], kTol))
                continue;
            double hits[4];
            int count = 0;
            SegPoints (seg.a, seg.b, edges[j].a, edges[j].b, hits, count);
            for (int h = 0; h < count; ++h) {
                const double t = ((hits[h * 2] - seg.a.x) * abx + (hits[h * 2 + 1] - seg.a.y) * aby) / ab2;
                if (t < 0.0 || t > 1.0)
                    continue;
                params.push_back (std::floor (t * ab2 / kTol + 0.5) * kTol / ab2);
            }
        }
        std::sort (params.begin (), params.end ());
        params.erase (std::unique (params.begin (), params.end ()), params.end ());

        const double minStep = kTol / std::sqrt (ab2);
        for (size_t k = 0; k + 1 < params.size (); ++k) {
            const double t0 = params[k], t1 = params[k + 1];
            if (t1 - t0 < minStep)
                continue;
            const Pt p0 = MakePt (seg.a.x + t0 * abx, seg.a.y + t0 * aby);
            const Pt p1 = MakePt (seg.a.x + t1 * abx, seg.a.y + t1 * aby);
            if (p0 == p1)
                continue;
            const Pt& lo = (p0 < p1) ? p0 : p1;
            const Pt& hi = (p0 < p1) ? p1 : p0;
            uniq.emplace (CellKey { { lo.gx, lo.gy }, { hi.gx, hi.gy } }, Seg { p0, p1 });
        }
    }

    // 4. keep sub-edges that straddle the union boundary — inside on exactly one
    //    side. Interior edges (inside on both) and stray exterior ones drop.
    std::vector<UnionSegment> out;
    out.reserve (uniq.size ());
    for (const auto& entry : uniq) {
        const Pt& p0 = entry.second.a;
        const Pt& p1 = entry.second.b;
        const double mx = (p0.x + p1.x) * 0.5, my = (p0.y + p1.y) * 0.5;
        const double dx = p1.x - p0.x, dy = p1.y - p0.y;
        const double length = std::sqrt (dx * dx + dy * dy);
        if (length <= 0.0)
            continue;
        const double nx = -dy / length, ny = dx / length; // unit normal
        const bool pos = InsideUnion (mx + kEps * nx, my + kEps * ny, rings);
        const bool neg = InsideUnion (mx - kEps * nx, my - kEps * ny, rings);
        if (pos != neg)
            out.push_back (UnionSegment { p0.x, p0.y, p1.x, p1.y });
    }

    // Open cross-sections pass through, expanded into consecutive PAIRS (see the
    // header's warning on why an N-point polyline must not be emitted whole).
    for (const std::vector<double>& poly : openPolys) {
        const size_t n = poly.size () / 2;
        for (size_t i = 0; i + 1 < n; ++i)
            out.push_back (UnionSegment { poly[i * 2], poly[i * 2 + 1], poly[(i + 1) * 2], poly[(i + 1) * 2 + 1] });
    }

    // ⚠️ SORTED, because the classification loop above walks an unordered_map and
    // its order varies between runs and between builds. Nothing downstream needs
    // a particular order, but a vertex buffer that reshuffles on every rebuild
    // makes two captures of one unchanged model differ byte-for-byte, which
    // destroys the cheapest way to tell a real regression from noise.
    std::sort (out.begin (), out.end (), [] (const UnionSegment& a, const UnionSegment& b) {
        if (a.x0 != b.x0)
            return a.x0 < b.x0;
        if (a.y0 != b.y0)
            return a.y0 < b.y0;
        if (a.x1 != b.x1)
            return a.x1 < b.x1;
        return a.y1 < b.y1;
    });
    return out;
}

} // namespace archviz
} // namespace geomsrv
